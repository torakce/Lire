/*
 * core.c — moteur de la visionneuse Lire (portable : Windows / Linux)
 *
 * Principe :
 *  - Chaque page est analysée UNE seule fois (thread "chargeur") en une
 *    display list MuPDF. Les rendus suivants rejouent cette liste, ce qui évite
 *    de ré-interpréter le PDF à chaque zoom.
 *  - L'affichage est découpé en tuiles de 512 px rendues en parallèle par
 *    plusieurs threads. Les tuiles sont calculées sur une grille de niveaux
 *    alignée sur les crans de la molette (x1,26) : un cran de molette tombe
 *    pile sur un niveau déjà calculé, et revenir en arrière ne recalcule rien.
 *  - Une fois la zone visible nette, on anticipe en tâche de fond : couronne
 *    autour de l'écran, cran de zoom suivant et précédent, puis le reste de la
 *    page (dans la limite d'un budget mémoire proportionnel à la RAM).
 *  - Pendant un zoom, la carte graphique ré-étire instantanément ce qui est
 *    déjà calculé, puis les tuiles nettes arrivent au fur et à mesure. Les rendus
 *    devenus inutiles sont interrompus immédiatement (fz_cookie.abort).
 *  - Toutes les pages sont analysées en tâche de fond (plusieurs threads, chacun
 *    avec sa propre instance du document), la page affichée en premier.
 *  - Onglets : chaque document (Tab) garde son état complet (zoom, position,
 *    rotation, cache). Les threads ne travaillent que pour l'onglet affiché ;
 *    un onglet mis en arrière-plan conserve juste ce qui était à l'écran, d'où
 *    un retour instantané.
 */
#include "core.h"
#include "gfx.h"
#include "thread.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE 512
#define LPO 768                    /* niveaux de zoom par octave (pas de 0,09 %) */
#define GRID 256                   /* un cran de molette = 2^(1/3) = x1,26 ; grille des niveaux de rendu */
#define PREVIEW_MAX 1024           /* côté max de l'aperçu basse résolution */
#define MAX_WORKERS 32
#define MAX_LOADERS 3
#define HASH_SIZE 8192
#define TILE_BYTES ((size_t)TILE * TILE * 4)
#define MAX_CLOSED 20              /* historique des onglets fermés (Ctrl+Maj+T) */
#define PREVIEW_LEVEL INT_MIN
#define BG_COLOR 0x3c3f43

enum { L_NONE, L_QUEUED, L_BUILDING, L_READY, L_ERROR };
enum { J_IDLE, J_QUEUED, J_RUNNING, J_DONE, J_READY, J_FAILED };
enum { FIT_NONE, FIT_PAGE, FIT_WIDTH };
enum { DRAG_NONE, DRAG_PAN, DRAG_ZOOM, DRAG_VBAR, DRAG_HBAR, DRAG_AUTO };
enum { HIT_NONE = 0, HIT_PLUS, HIT_ROT_CCW, HIT_ROT_CW, HIT_TAB = 100000, HIT_CLOSE = 200000 };

typedef struct Job Job;
typedef struct Tab Tab;

struct Job {
	Tab *tab;
	int page, level, tx, ty;
	int state;              /* protégé par mu */
	int dropped;            /* job retiré du cache pendant son rendu : le thread le libère */
	fz_cookie cookie;
	fz_irect rect;          /* rectangle en pixels dans l'espace du niveau */
	fz_matrix ctm;
	double scale;
	fz_pixmap *pix;         /* résultat en attente d'envoi à la carte graphique */
	void *tex;
	size_t bytes;
	unsigned used;          /* dernière génération de planification où il était utile */
	unsigned vis_gen;       /* dernière génération où il était à l'écran */
	double key;             /* priorité (plus petit = plus urgent) */
	Job *hnext, *dnext;
	Job *pnext, *pprev;     /* chaînage des jobs prêts d'une page */
};

typedef struct {
	fz_rect bounds;         /* limites de la page (pt), sans la rotation utilisateur */
	fz_irect pbox;          /* boîte en pixels au zoom courant */
	double top, left;       /* position dans le contenu (px) */
	int pw, ph;
	fz_display_list *list;
	int lstate;
	unsigned lused;
	fz_cookie lcookie;
	fz_rect lbounds;        /* limites réelles mesurées au chargement de la page */
	int bounds_differ;
	int urgent;             /* page visible (à analyser avant les autres) */
	size_t lbytes;          /* mémoire de la display list */
	Job *ready;
} Page;

struct Tab {
	fz_document *doc;                  /* utilisé par le thread principal et le chargeur 0 */
	fz_document *ldoc[MAX_LOADERS];    /* instances supplémentaires pour les autres chargeurs */
	int ldoc_tried[MAX_LOADERS];
	char *password;
	char *path, *name;
	int need_password;
	int exact_level;                   /* niveau rendu tel quel (page entière, 100 %…) hors grille */
	size_t list_bytes;
	Page *pages;
	int npages;
	double max_page_dim;
	Job *htab[HASH_SIZE];
	int njobs;
	size_t tex_bytes;
	int level, min_level, max_level;
	double scale;
	int rot;
	double sx, sy, cw, ch;
	int maxpw;
	int fit_mode;
	int *lq, *lq_old, nlq, nlq_old, lq_pos;
	unsigned last_gen;
	int needs_relayout;
	int synced_vw, synced_vh;
	float synced_dpi;
	char label[200];
	int label_w, label_px;
};

typedef struct { Job **v; int n, cap; } JobVec;

/* ---------- état global ---------- */

static fz_context *ctx;
static fz_locks_context fzlocks;
static lire_mutex fzmu[FZ_LOCK_MAX];

static lire_mutex mu;
static lire_cond cv_work, cv_load, cv_idle;
static lire_thread th_loader[MAX_LOADERS], th_workers[MAX_WORKERS];
static fz_context *wctx[MAX_WORKERS], *lctx[MAX_LOADERS];
static int nworkers, nloaders;
static int quit, paused;
static int nrunning, loader_busy[MAX_LOADERS], loader_page[MAX_LOADERS], resched;
static Tab *loader_tab[MAX_LOADERS];
static Job *running[MAX_WORKERS];
static size_t tex_budget = (size_t)384 << 20;   /* mémoire graphique des tuiles (onglet actif) */
static size_t list_budget = (size_t)512 << 20;  /* mémoire des pages analysées (onglet actif) */
static JobVec wq, wq_next;
static int wq_pos;
static Job *done_head;
static unsigned gen;

/* onglets */
static Tab **tabs;
static int ntabs, tabs_cap, active = -1;
static Tab *T;                     /* onglet affiché (NULL si aucun) */
static char *closed_paths[MAX_CLOSED];
static int nclosed;
static float tab_scroll;
static int ensure_tab_visible;

/* fenêtre et vue */
static int win_w = 800, win_h = 600, bar_h = 34;
static int vw = 800, vh = 566;
static float dpi = 96, ui = 1;
static int gap = 10, sbw = 12;
static int view_dirty, need_schedule, dirty = 1;
static int wheel_zoom;
static size_t tex_total;

/* interaction */
static int drag, drag_btn, mx0, my0, mx, my;
static double drag_off;
static int input_mode;
static char input_buf[256];
static int show_help;
static char toast[300];
static double toast_until;
static int hover = HIT_NONE;
static double hover_since;

/* ---------- utilitaires ---------- */

static double clampd(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
static int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static int has_doc(void) { return T && T->doc && T->npages > 0 && !T->need_password; }

static void lock_fz(void *u, int l) { (void)u; mutex_lock(&fzmu[l]); }
static void unlock_fz(void *u, int l) { (void)u; mutex_unlock(&fzmu[l]); }

static void jv_push(JobVec *a, Job *j)
{
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 256;
		a->v = realloc(a->v, a->cap * sizeof *a->v);
	}
	a->v[a->n++] = j;
}

static void set_toast(const char *msg)
{
	snprintf(toast, sizeof toast, "%s", msg);
	toast_until = platform_time() + 1.8;
	dirty = 1;
}

static double level_scale(int L) { return exp2((double)L / LPO); }
static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int grid_floor(int L) { return floor_div(L, GRID) * GRID; }
static int grid_ceil(int L) { return -grid_floor(-L); }

/* Niveau auquel on calcule les tuiles pour afficher le niveau L : le niveau de la
   grille juste au-dessus (affiché réduit de 0 à 21 %, donc net), sauf pour un
   niveau « exact » (page entière, 100 %, zoom sur une zone) rendu tel quel. */
static int render_level(Tab *t, int L)
{
	int R;
	if (L == t->exact_level) return L;
	R = grid_ceil(L);
	return R > t->max_level ? L : R;
}
static fz_matrix view_ctm(int r, double s) { return fz_concat(fz_rotate((float)r), fz_scale((float)s, (float)s)); }
static fz_irect page_pbox(Page *p, int r, double s) { return fz_round_rect(fz_transform_rect(p->bounds, view_ctm(r, s))); }
static fz_rect rotated_bounds(Page *p, int r) { return fz_transform_rect(p->bounds, fz_rotate((float)r)); }

static const char *base_name(const char *p)
{
	const char *b = p, *s;
	for (s = p; *s; s++)
		if (*s == '/' || *s == '\\') b = s + 1;
	return b;
}

static int same_path(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int ca = *a == '\\' ? '/' : tolower((unsigned char)*a);
		int cb = *b == '\\' ? '/' : tolower((unsigned char)*b);
		if (ca != cb) return 0;
	}
	return *a == *b;
}

/* ---------- table de hachage des tuiles d'un onglet (thread principal uniquement) ---------- */

static unsigned hkey(int p, int l, int x, int y)
{
	unsigned h = (unsigned)p * 2654435761u ^ (unsigned)l * 40503u ^ (unsigned)x * 73856093u ^ (unsigned)y * 19349663u;
	h ^= h >> 15;
	return h & (HASH_SIZE - 1);
}

static Job *hfind(Tab *t, int p, int l, int x, int y)
{
	Job *j;
	for (j = t->htab[hkey(p, l, x, y)]; j; j = j->hnext)
		if (j->page == p && j->level == l && j->tx == x && j->ty == y)
			return j;
	return NULL;
}

static void hremove(Job *j)
{
	Tab *t = j->tab;
	Job **pp = &t->htab[hkey(j->page, j->level, j->tx, j->ty)];
	while (*pp && *pp != j) pp = &(*pp)->hnext;
	if (*pp) *pp = j->hnext;
	j->hnext = NULL;
	t->njobs--;
}

static Job *hget(Tab *t, int p, int l, int x, int y)
{
	Job *j = hfind(t, p, l, x, y);
	unsigned h;
	if (j) return j;
	j = calloc(1, sizeof *j);
	j->tab = t;
	j->page = p; j->level = l; j->tx = x; j->ty = y;
	j->state = J_IDLE;
	h = hkey(p, l, x, y);
	j->hnext = t->htab[h];
	t->htab[h] = j;
	t->njobs++;
	return j;
}

static void ready_add(Page *p, Job *j)
{
	j->pprev = NULL;
	j->pnext = p->ready;
	if (p->ready) p->ready->pprev = j;
	p->ready = j;
}

static void ready_remove(Page *p, Job *j)
{
	if (j->pprev) j->pprev->pnext = j->pnext; else if (p->ready == j) p->ready = j->pnext;
	if (j->pnext) j->pnext->pprev = j->pprev;
	j->pnext = j->pprev = NULL;
}

/* Libère un job qui n'est ni en file, ni en cours, ni en attente d'envoi. */
static void job_free(Job *j)
{
	Tab *t = j->tab;
	hremove(j);
	if (j->tex) {
		ready_remove(&t->pages[j->page], j);
		gfx_tex_free(j->tex);
		t->tex_bytes -= j->bytes;
		tex_total -= j->bytes;
	}
	if (j->pix) fz_drop_pixmap(ctx, j->pix);
	free(j);
}

/* boîte de la page en pixels au niveau L */
static fz_irect level_pbox(Tab *t, Page *p, int L)
{
	return L == t->level ? p->pbox : page_pbox(p, t->rot, level_scale(L));
}

static Job *get_tile(Tab *t, int i, int L, int tx, int ty)
{
	Job *j = hget(t, i, L, tx, ty);
	if (j->scale == 0) {
		Page *p = &t->pages[i];
		double s = level_scale(L);
		fz_irect pb = level_pbox(t, p, L);
		j->scale = s;
		j->ctm = view_ctm(t->rot, s);
		j->rect.x0 = pb.x0 + tx * TILE;
		j->rect.y0 = pb.y0 + ty * TILE;
		j->rect.x1 = j->rect.x0 + TILE < pb.x1 ? j->rect.x0 + TILE : pb.x1;
		j->rect.y1 = j->rect.y0 + TILE < pb.y1 ? j->rect.y0 + TILE : pb.y1;
		if (j->rect.x1 <= j->rect.x0 || j->rect.y1 <= j->rect.y0) j->state = J_FAILED;
	}
	return j;
}

static Job *get_preview(Tab *t, int i)
{
	Job *j = hget(t, i, PREVIEW_LEVEL, 0, 0);
	if (j->scale == 0) {
		Page *p = &t->pages[i];
		fz_rect r = rotated_bounds(p, t->rot);
		double w = r.x1 - r.x0, h = r.y1 - r.y0;
		int lim = PREVIEW_MAX < gfx_max_texture() ? PREVIEW_MAX : gfx_max_texture();
		double s = fmin(lim / fmax(w, 1), lim / fmax(h, 1));
		j->scale = s;
		j->ctm = view_ctm(t->rot, s);
		j->rect = page_pbox(p, t->rot, s);
		if (j->rect.x1 - j->rect.x0 > lim) j->rect.x1 = j->rect.x0 + lim;
		if (j->rect.y1 - j->rect.y0 > lim) j->rect.y1 = j->rect.y0 + lim;
		if (j->rect.x1 <= j->rect.x0 || j->rect.y1 <= j->rect.y0) j->state = J_FAILED;
	}
	return j;
}

/* ---------- threads de travail ---------- */

static fz_pixmap *render_job(fz_context *c, Job *j, fz_display_list *list)
{
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;
	fz_var(pix);
	fz_var(dev);
	fz_try(c) {
		pix = fz_new_pixmap_with_bbox(c, fz_device_bgr(c), j->rect, NULL, 1);
		fz_clear_pixmap_with_value(c, pix, 0xff);
		dev = fz_new_draw_device(c, fz_identity, pix);
		fz_run_display_list(c, list, dev, j->ctm, fz_rect_from_irect(j->rect), &j->cookie);
		fz_close_device(c, dev);
	}
	fz_always(c)
		fz_drop_device(c, dev);
	fz_catch(c) {
		fz_drop_pixmap(c, pix);
		pix = NULL;
	}
	return pix;
}

static void worker_main(void *arg)
{
	int id = (int)(intptr_t)arg;
	fz_context *c = wctx[id];
	mutex_lock(&mu);
	while (!quit) {
		Job *j = NULL;
		Page *p;
		fz_display_list *list;
		fz_pixmap *pix;
		int wake = 0;
		if (!paused)
			while (wq_pos < wq.n) {
				Job *k = wq.v[wq_pos++];
				if (k->state == J_QUEUED) { j = k; break; }
			}
		if (!j) { cond_wait(&cv_work, &mu); continue; }
		p = &j->tab->pages[j->page];
		if (p->lstate != L_READY || !p->list) { j->state = J_IDLE; resched = 1; continue; }
		list = fz_keep_display_list(c, p->list);
		j->state = J_RUNNING;
		memset(&j->cookie, 0, sizeof j->cookie);
		running[id] = j;
		nrunning++;
		mutex_unlock(&mu);

		pix = render_job(c, j, list);
		fz_drop_display_list(c, list);

		mutex_lock(&mu);
		running[id] = NULL;
		nrunning--;
		if (j->dropped) {
			fz_drop_pixmap(c, pix);
			free(j);
		} else if (j->cookie.abort) {
			fz_drop_pixmap(c, pix);
			j->state = J_IDLE;
			resched = 1; wake = 1;
		} else if (!pix) {
			j->state = J_FAILED;
			wake = 1;
		} else {
			j->pix = pix;
			j->state = J_DONE;
			j->dnext = done_head;
			done_head = j;
			wake = 1;
		}
		cond_broadcast(&cv_idle);
		if (wake) {
			mutex_unlock(&mu);
			platform_wake();
			mutex_lock(&mu);
		}
	}
	mutex_unlock(&mu);
}

#ifdef LIRE_STOCK_MUPDF
/* MuPDF sans les correctifs de Lire (banc de test sur la version système) : estimation */
static size_t fz_display_list_memory(fz_context *c, fz_display_list *l) { (void)c; return l ? (size_t)8 << 20 : 0; }
#endif

static fz_display_list *build_list(fz_context *c, fz_document *d, int i, fz_cookie *cookie, fz_rect *bounds)
{
	fz_page *page = NULL;
	fz_device *dev = NULL;
	fz_display_list *list = NULL;
	fz_var(page);
	fz_var(dev);
	fz_var(list);
	fz_try(c) {
		page = fz_load_page(c, d, i);
		*bounds = fz_bound_page(c, page);
		list = fz_new_display_list(c, *bounds);
		dev = fz_new_list_device(c, list);
		fz_run_page(c, page, dev, fz_identity, cookie);
		fz_close_device(c, dev);
	}
	fz_always(c) {
		fz_drop_device(c, dev);
		fz_drop_page(c, page);
	}
	fz_catch(c) {
		fz_drop_display_list(c, list);
		list = NULL;
	}
	return list;
}

/* Autre instance du même fichier, pour analyser des pages en parallèle
   (un fz_document ne peut servir qu'à un thread à la fois). */
static fz_document *open_extra(fz_context *c, const char *path, const char *pw, int npages)
{
	fz_document *d = NULL;
	fz_var(d);
	fz_try(c) {
		d = fz_open_document(c, path);
		if (fz_needs_password(c, d) && !(pw && fz_authenticate_password(c, d, pw)))
			fz_throw(c, FZ_ERROR_GENERIC, "mot de passe");
		if (fz_count_pages(c, d) != npages)   /* fichier remplacé entre-temps */
			fz_throw(c, FZ_ERROR_GENERIC, "fichier modifie");
	}
	fz_catch(c) {
		fz_drop_document(c, d);
		d = NULL;
	}
	return d;
}

/* (sous mu) prochaine page en attente d'analyse, sans la retirer de la file */
static int queued_page(Tab *t)
{
	int k;
	for (k = t->lq_pos; k < t->nlq; k++)
		if (t->lq[k] < t->npages && t->pages[t->lq[k]].lstate == L_QUEUED) return 1;
	return 0;
}

static int any_loader_busy(void)
{
	int k;
	for (k = 0; k < nloaders; k++)
		if (loader_busy[k]) return 1;
	return 0;
}

static void loader_main(void *arg)
{
	int k = (int)(intptr_t)arg;
	fz_context *c = lctx[k];
	mutex_lock(&mu);
	while (!quit) {
		Tab *t = T;
		int i = -1;
		Page *p;
		fz_display_list *list;
		fz_document *d;
		fz_rect lb;
		if (paused || !t || !t->doc || t->need_password || !t->pages || !queued_page(t)) {
			cond_wait(&cv_load, &mu);
			continue;
		}
		d = k == 0 ? t->doc : t->ldoc[k];
		if (!d) {
			char *path, *pw;
			int n;
			if (t->ldoc_tried[k]) { cond_wait(&cv_load, &mu); continue; }
			/* première fois que ce chargeur travaille pour cet onglet : il ouvre sa propre instance */
			t->ldoc_tried[k] = 1;
			loader_busy[k] = 1;
			loader_tab[k] = t;
			loader_page[k] = -1;
			path = t->path;
			pw = t->password;
			n = t->npages;
			mutex_unlock(&mu);
			d = open_extra(c, path, pw, n);
			mutex_lock(&mu);
			t->ldoc[k] = d;          /* l'onglet ne peut pas disparaître pendant ce temps (quiesce attend) */
			loader_busy[k] = 0;
			loader_tab[k] = NULL;
			cond_broadcast(&cv_idle);
			continue;
		}
		while (t->lq_pos < t->nlq) {
			int q = t->lq[t->lq_pos++];
			if (q < t->npages && t->pages[q].lstate == L_QUEUED) { i = q; break; }
		}
		if (i < 0) continue;
		p = &t->pages[i];
		p->lstate = L_BUILDING;
		memset(&p->lcookie, 0, sizeof p->lcookie);
		loader_busy[k] = 1;
		loader_page[k] = i;
		loader_tab[k] = t;
		lb = p->bounds;
		mutex_unlock(&mu);
		list = build_list(c, d, i, &p->lcookie, &lb);

		mutex_lock(&mu);
		if (list && !p->lcookie.abort && !fz_is_empty_rect(lb) &&
				(fabsf(lb.x0 - p->bounds.x0) > 0.5f || fabsf(lb.y0 - p->bounds.y0) > 0.5f ||
				 fabsf(lb.x1 - p->bounds.x1) > 0.5f || fabsf(lb.y1 - p->bounds.y1) > 0.5f)) {
			p->lbounds = lb;
			p->bounds_differ = 1;
		}
		loader_busy[k] = 0;
		loader_page[k] = -1;
		loader_tab[k] = NULL;
		if (p->lcookie.abort) {
			fz_drop_display_list(c, list);
			p->lstate = L_NONE;
		} else if (list) {
			p->list = list;
			p->lbytes = fz_display_list_memory(c, list);
			t->list_bytes += p->lbytes;
			p->lstate = L_READY;
		} else {
			p->lstate = L_ERROR;
		}
		resched = 1;
		cond_broadcast(&cv_idle);
		mutex_unlock(&mu);
		platform_wake();
		mutex_lock(&mu);
	}
	mutex_unlock(&mu);
}

/* (sous mu) libère la display list d'une page */
static void drop_list(Tab *t, Page *p)
{
	fz_drop_display_list(ctx, p->list);
	p->list = NULL;
	t->list_bytes -= p->lbytes;
	p->lbytes = 0;
	p->lstate = L_NONE;
}

/* (sous mu) vide la file du chargeur d'un onglet */
static void clear_lq(Tab *t)
{
	int k;
	for (k = 0; k < t->nlq; k++) {
		Page *p = &t->pages[t->lq[k]];
		if (p->lstate == L_QUEUED) p->lstate = L_NONE;
	}
	t->nlq = t->lq_pos = 0;
}

/* (sous mu) vide la file des tuiles */
static void clear_wq(void)
{
	int k;
	for (k = wq_pos; k < wq.n; k++)
		if (wq.v[k]->state == J_QUEUED) wq.v[k]->state = J_IDLE;
	wq.n = wq_pos = 0;
}

/* Arrête tout rendu en cours et attend que les threads soient au repos. */
static void quiesce(void)
{
	int k;
	mutex_lock(&mu);
	paused = 1;
	clear_wq();
	if (T && T->lq) clear_lq(T);
	for (k = 0; k < nworkers; k++)
		if (running[k]) running[k]->cookie.abort = 1;
	for (k = 0; k < nloaders; k++)
		if (loader_tab[k] && loader_page[k] >= 0) loader_tab[k]->pages[loader_page[k]].lcookie.abort = 1;
	while (nrunning > 0 || any_loader_busy())
		cond_wait(&cv_idle, &mu);
	mutex_unlock(&mu);
}

static void resume(void)
{
	mutex_lock(&mu);
	paused = 0;
	cond_broadcast(&cv_work);
	cond_broadcast(&cv_load);
	mutex_unlock(&mu);
}

/* Onglet qui passe en arrière-plan : on arrête son travail sans attendre. */
static void stop_tab_work(Tab *t)
{
	int k;
	mutex_lock(&mu);
	clear_wq();
	if (t->lq) clear_lq(t);
	for (k = 0; k < nworkers; k++)
		if (running[k] && running[k]->tab == t) running[k]->cookie.abort = 1;
	for (k = 0; k < nloaders; k++)
		if (loader_tab[k] == t && loader_page[k] >= 0) t->pages[loader_page[k]].lcookie.abort = 1;
	mutex_unlock(&mu);
}

/* Onglet en arrière-plan : on ne garde que ce qui était à l'écran (retour instantané). */
static void trim_tab(Tab *t)
{
	int h, i;
	mutex_lock(&mu);
	for (h = 0; h < HASH_SIZE; h++) {
		Job *j = t->htab[h];
		while (j) {
			Job *next = j->hnext;
			if (j->vis_gen != t->last_gen && (j->state == J_READY || j->state == J_IDLE || j->state == J_FAILED))
				job_free(j);
			j = next;
		}
	}
	for (i = 0; i < t->npages; i++) {
		Page *p = &t->pages[i];
		if (p->lstate == L_READY && !(p->lused == t->last_gen && p->urgent)) drop_list(t, p);
	}
	mutex_unlock(&mu);
}

/* Vide le cache de tuiles d'un onglet (rotation, fermeture, rechargement). */
static void flush_jobs(Tab *t)
{
	int h, i;
	mutex_lock(&mu);
	clear_wq();
	for (h = 0; h < HASH_SIZE; h++) {
		Job *j = t->htab[h];
		while (j) {
			Job *next = j->hnext;
			if (j->state == J_RUNNING || j->state == J_DONE) {
				j->dropped = 1;
				j->cookie.abort = 1;
			} else {
				if (j->tex) { gfx_tex_free(j->tex); tex_total -= j->bytes; }
				if (j->pix) fz_drop_pixmap(ctx, j->pix);
				free(j);
			}
			j = next;
		}
		t->htab[h] = NULL;
	}
	t->njobs = 0;
	t->tex_bytes = 0;
	for (i = 0; i < t->npages; i++) t->pages[i].ready = NULL;
	mutex_unlock(&mu);
}

/* Libère le document d'un onglet (threads arrêtés au préalable par quiesce). */
static void tab_clear(Tab *t)
{
	int i;
	flush_jobs(t);
	for (i = 0; i < t->npages; i++)
		fz_drop_display_list(ctx, t->pages[i].list);
	free(t->pages);
	t->pages = NULL;
	t->npages = 0;
	t->list_bytes = 0;
	free(t->lq); free(t->lq_old);
	t->lq = t->lq_old = NULL;
	t->nlq = t->nlq_old = t->lq_pos = 0;
	for (i = 1; i < MAX_LOADERS; i++) {
		fz_drop_document(ctx, t->ldoc[i]);
		t->ldoc[i] = NULL;
		t->ldoc_tried[i] = 0;
	}
	fz_drop_document(ctx, t->doc);
	t->doc = NULL;
	if (t->password) {
		memset(t->password, 0, strlen(t->password));
		free(t->password);
		t->password = NULL;
	}
	t->need_password = 0;
}

static void tab_free(Tab *t)
{
	tab_clear(t);
	free(t->path);
	free(t->name);
	free(t);
}

/* ---------- mise en page et vue (onglet actif) ---------- */

static void layout(void)
{
	Tab *t = T;
	fz_matrix m = view_ctm(t->rot, t->scale);
	double y = gap;
	int i;
	t->maxpw = 0;
	for (i = 0; i < t->npages; i++) {
		Page *p = &t->pages[i];
		p->pbox = fz_round_rect(fz_transform_rect(p->bounds, m));
		p->pw = p->pbox.x1 - p->pbox.x0;
		p->ph = p->pbox.y1 - p->pbox.y0;
		if (p->pw < 1) p->pw = 1;
		if (p->ph < 1) p->ph = 1;
		if (p->pw > t->maxpw) t->maxpw = p->pw;
		p->top = y;
		y += p->ph + gap;
	}
	for (i = 0; i < t->npages; i++)
		t->pages[i].left = gap + (t->maxpw - t->pages[i].pw) / 2;
	t->cw = t->maxpw + 2.0 * gap;
	t->ch = y;
}

static void clamp_scroll(void)
{
	Tab *t = T;
	if (t->cw <= vw) t->sx = -floor((vw - t->cw) / 2); else t->sx = clampd(t->sx, 0, t->cw - vw);
	if (t->ch <= vh) t->sy = -floor((vh - t->ch) / 2); else t->sy = clampd(t->sy, 0, t->ch - vh);
}

static void compute_level_limits(void)
{
	Tab *t = T;
	double s100 = dpi / 72.0;
	double smin = fmax(1e-3, 24.0 / fmax(t->max_page_dim, 1));
	double smax = fmin(64 * s100, 4.0e6 / fmax(t->max_page_dim, 1));
	t->min_level = (int)ceil(LPO * log2(smin));
	t->max_level = (int)floor(LPO * log2(smax));
	if (t->max_level < t->min_level) t->max_level = t->min_level;
}

static void set_level(int L)
{
	T->level = clampi(L, T->min_level, T->max_level);
	T->scale = level_scale(T->level);
	layout();
	view_dirty = dirty = 1;
}

static int page_at_y(double y)
{
	int lo = 0, hi = T->npages - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (T->pages[mid].top - gap / 2.0 <= y) lo = mid; else hi = mid - 1;
	}
	return lo;
}

static void visible_range(int *first, int *last)
{
	Page *pg = T->pages;
	int i = page_at_y(T->sy), k;
	if (pg[i].top + pg[i].ph <= T->sy && i < T->npages - 1) i++;
	k = i;
	while (k + 1 < T->npages && pg[k + 1].top < T->sy + vh) k++;
	*first = i;
	*last = k;
}

static int current_page(void)
{
	return has_doc() ? page_at_y(T->sy + vh / 2.0) : 0;
}

/* Zoom au niveau L en gardant fixe le point du document situé sous (x, y). */
static void zoom_at(int L, double x, double y)
{
	Tab *t = T;
	int i;
	Page *p;
	double ux, uy;
	if (!has_doc()) return;
	i = page_at_y(t->sy + y);
	p = &t->pages[i];
	ux = (x + t->sx - p->left + p->pbox.x0) / t->scale;
	uy = (y + t->sy - p->top + p->pbox.y0) / t->scale;
	set_level(L);
	t->sx = p->left - p->pbox.x0 + ux * t->scale - x;
	t->sy = p->top - p->pbox.y0 + uy * t->scale - y;
	clamp_scroll();
}

static void center_on_page(int i)
{
	Tab *t = T;
	Page *p = &t->pages[i];
	if (p->ph + 2 * gap <= vh) t->sy = p->top + p->ph / 2.0 - vh / 2.0;
	else t->sy = p->top - gap;
	if (p->pw + 2 * gap <= vw) t->sx = p->left + p->pw / 2.0 - vw / 2.0;
	clamp_scroll();
	view_dirty = dirty = 1;
}

static void apply_fit(int mode, int i, int keep_position)
{
	Tab *t = T;
	Page *p;
	fz_rect r;
	double w, h, s;
	if (!has_doc()) return;
	t->fit_mode = mode;
	if (mode == FIT_NONE) return;
	i = clampi(i, 0, t->npages - 1);
	p = &t->pages[i];
	r = rotated_bounds(p, t->rot);
	w = fmax(r.x1 - r.x0, 1);
	h = fmax(r.y1 - r.y0, 1);
	if (mode == FIT_PAGE) {
		s = fmin((vw - 2.0 * gap) / w, (vh - 2.0 * gap) / h);
		set_level((int)floor(LPO * log2(fmax(s, 1e-6))));
		t->exact_level = t->level;
		center_on_page(i);
	} else {
		s = (vw - 2.0 * gap - sbw) / w;
		if (keep_position) {
			zoom_at((int)floor(LPO * log2(fmax(s, 1e-6))), vw / 2.0, vh / 2.0);
		} else {
			set_level((int)floor(LPO * log2(fmax(s, 1e-6))));
			t->sy = p->top - gap;
		}
		t->exact_level = t->level;
		t->sx = p->left + p->pw / 2.0 - vw / 2.0;
		clamp_scroll();
	}
	view_dirty = dirty = 1;
}

static void goto_page(int i)
{
	if (!has_doc()) return;
	i = clampi(i, 0, T->npages - 1);
	if (T->fit_mode != FIT_NONE) apply_fit(T->fit_mode, i, 0);
	else center_on_page(i);
}

/* Zoom par crans (molette, +/-) : on se cale sur la grille des niveaux de rendu,
   pour que chaque cran tombe sur des tuiles déjà calculées ou anticipées.
   Un défilement fractionnaire (pavé tactile) zoome librement. */
static void zoom_step(double steps, double x, double y)
{
	int L, n;
	if (!has_doc()) return;
	T->fit_mode = FIT_NONE;
	L = T->level;
	n = (int)lround(steps);
	if (n != 0 && fabs(steps - n) < 1e-3)
		L = n > 0 ? grid_floor(L) + n * GRID : grid_ceil(L) + n * GRID;
	else
		L = (int)lround(L + steps * GRID);
	zoom_at(L, x, y);
}

/* Remet la vue d'un onglet en accord avec la fenêtre (taille / DPI changés pendant qu'il était caché). */
static void sync_view(void)
{
	Tab *t = T;
	int force = 0;
	if (!has_doc()) return;
	if (t->needs_relayout) {
		flush_jobs(t);
		t->needs_relayout = 0;
		force = 1;
	}
	if (force || t->synced_vw != vw || t->synced_vh != vh || t->synced_dpi != dpi) {
		int cur = current_page();
		double cx = t->sx + t->synced_vw / 2.0, cy = t->sy + t->synced_vh / 2.0;
		compute_level_limits();
		layout();
		if (t->fit_mode != FIT_NONE) apply_fit(t->fit_mode, cur, t->fit_mode == FIT_WIDTH);
		else {
			t->sx = cx - vw / 2.0;
			t->sy = cy - vh / 2.0;
			clamp_scroll();
		}
	}
	t->synced_vw = vw;
	t->synced_vh = vh;
	t->synced_dpi = dpi;
	view_dirty = dirty = 1;
}

/* ---------- planification ---------- */

/* Tuiles du niveau L couvrant la partie de la page p visible à l'écran, élargie
   de mx, my pixels d'écran de chaque côté. 0 si rien n'est concerné. */
static int tiles_for(Tab *t, Page *p, int L, double mx, double my, int *tx0, int *ty0, int *tx1, int *ty1)
{
	double X = p->left - floor(t->sx), Y = p->top - floor(t->sy);
	double u0 = fmax(0, -X - mx), u1 = fmin(p->pw, vw - X + mx);
	double v0 = fmax(0, -Y - my), v1 = fmin(p->ph, vh - Y + my);
	double k = L == t->level ? 1 : level_scale(L) / t->scale;
	fz_irect pb = level_pbox(t, p, L);
	int nx = (pb.x1 - pb.x0 - 1) / TILE, ny = (pb.y1 - pb.y0 - 1) / TILE;
	if (u0 >= u1 || v0 >= v1 || pb.x1 <= pb.x0 || pb.y1 <= pb.y0) return 0;
	*tx0 = clampi((int)floor(((u0 + p->pbox.x0) * k - pb.x0) / TILE), 0, nx);
	*tx1 = clampi((int)floor(((u1 + p->pbox.x0) * k - pb.x0 - 1e-3) / TILE), 0, nx);
	*ty0 = clampi((int)floor(((v0 + p->pbox.y0) * k - pb.y0) / TILE), 0, ny);
	*ty1 = clampi((int)floor(((v1 + p->pbox.y0) * k - pb.y0 - 1e-3) / TILE), 0, ny);
	return 1;
}

/* distance (pixels d'écran) entre le centre d'une tuile du niveau L et le centre de la vue */
static double tile_dist(Tab *t, Page *p, int L, fz_irect pb, int tx, int ty)
{
	double k = L == t->level ? 1 : level_scale(L) / t->scale;
	double X = p->left - floor(t->sx), Y = p->top - floor(t->sy);
	double x = X + (pb.x0 + (tx + 0.5) * TILE) / k - p->pbox.x0;
	double y = Y + (pb.y0 + (ty + 0.5) * TILE) / k - p->pbox.y0;
	return hypot(x - vw * 0.5, y - vh * 0.5);
}

static size_t want_bytes;   /* mémoire des tuiles voulues dans la passe en cours */

static void want(Job *j, double key)
{
	if (j->used == gen) {           /* déjà demandé dans cette passe : on garde la priorité la plus forte */
		if (key < j->key) j->key = key;
		if (key < 2e9) j->vis_gen = gen;
		return;
	}
	j->used = gen;
	j->key = key;
	if (key < 2e9) j->vis_gen = gen;
	if (j->state == J_FAILED) return;
	want_bytes += j->bytes ? j->bytes : TILE_BYTES;
	if (j->state == J_IDLE || j->state == J_QUEUED) jv_push(&wq_next, j);
}

typedef struct { Job *j; double d; } Cand;
static Cand *cands;
static int ncand, cand_cap;

static int cmp_cand(const void *a, const void *b)
{
	double x = ((const Cand *)a)->d, y = ((const Cand *)b)->d;
	return x < y ? -1 : x > y;
}

/* Anticipation : tuiles du niveau L autour de la vue (marge mx, my), des plus
   proches aux plus lointaines, tant que le budget le permet. */
static void want_area(Tab *t, int i, int L, double mx, double my, double base, size_t limit)
{
	Page *p = &t->pages[i];
	fz_irect pb;
	int a0, b0, a1, b1, tx, ty, k;
	if (want_bytes >= limit || !tiles_for(t, p, L, mx, my, &a0, &b0, &a1, &b1)) return;
	pb = level_pbox(t, p, L);
	ncand = 0;
	for (ty = b0; ty <= b1; ty++)
		for (tx = a0; tx <= a1; tx++) {
			if (ncand == cand_cap) {
				cand_cap = cand_cap ? cand_cap * 2 : 1024;
				cands = realloc(cands, cand_cap * sizeof *cands);
			}
			cands[ncand].j = NULL;
			cands[ncand].d = tile_dist(t, p, L, pb, tx, ty);
			cands[ncand].j = (Job *)(intptr_t)(ty * (a1 - a0 + 1) + (tx - a0));   /* indice, résolu après le tri */
			ncand++;
		}
	if (ncand > 1) qsort(cands, ncand, sizeof *cands, cmp_cand);
	for (k = 0; k < ncand && want_bytes < limit; k++) {
		int idx = (int)(intptr_t)cands[k].j;
		want(get_tile(t, i, L, a0 + idx % (a1 - a0 + 1), idx / (a1 - a0 + 1)), base + cands[k].d);
	}
}

static int cmp_key(const void *a, const void *b)
{
	double x = (*(Job *const *)a)->key, y = (*(Job *const *)b)->key;
	return x < y ? -1 : x > y;
}

static int evict_R;

static int cmp_used(const void *a, const void *b)
{
	const Job *x = *(Job *const *)a, *y = *(Job *const *)b;
	int dx, dy;
	if (x->used != y->used) return x->used < y->used ? -1 : 1;
	/* à âge égal, on libère d'abord les niveaux éloignés du zoom courant */
	dx = abs(x->level - evict_R);
	dy = abs(y->level - evict_R);
	return dx > dy ? -1 : dx < dy;
}

static int evict_cur;

static int cmp_lused(const void *a, const void *b)
{
	const Page *x = &T->pages[*(const int *)a], *y = &T->pages[*(const int *)b];
	int dx = abs(*(const int *)a - evict_cur), dy = abs(*(const int *)b - evict_cur);
	if (x->lused != y->lused) return x->lused < y->lused ? -1 : 1;
	return dx > dy ? -1 : dx < dy;
}

static void evict(void)
{
	static Job **cand;
	static int ccap;
	Tab *t = T;
	int h, n = 0, k;

	/* pages analysées : au-delà du budget, on libère les plus anciennes et les plus
	   éloignées de la page courante (jamais celles qui sont à l'écran) */
	if (t->list_bytes > list_budget) {
		int *idx = malloc(t->npages * sizeof *idx), m = 0;
		evict_cur = current_page();
		for (k = 0; k < t->npages; k++) {
			Page *p = &t->pages[k];
			if (p->lstate == L_READY && !(p->lused == gen && p->urgent)) idx[m++] = k;
		}
		if (m > 1) qsort(idx, m, sizeof *idx, cmp_lused);
		mutex_lock(&mu);
		for (k = 0; k < m && t->list_bytes > list_budget / 100 * 85; k++)
			drop_list(t, &t->pages[idx[k]]);
		mutex_unlock(&mu);
		free(idx);
	}

	/* tuiles */
	if (t->tex_bytes <= tex_budget && t->njobs < 40000) return;
	evict_R = render_level(t, t->level);
	if (ccap < t->njobs) { ccap = t->njobs * 2; cand = realloc(cand, ccap * sizeof *cand); }
	mutex_lock(&mu);
	for (h = 0; h < HASH_SIZE; h++) {
		Job *j;
		for (j = t->htab[h]; j; j = j->hnext)
			if (j->used != gen && (j->state == J_READY || j->state == J_IDLE || j->state == J_FAILED))
				cand[n++] = j;
	}
	if (n > 1) qsort(cand, n, sizeof *cand, cmp_used);
	for (k = 0; k < n; k++) {
		Job *j = cand[k];
		if (j->state != J_READY) job_free(j);
		else if (t->tex_bytes > tex_budget / 10 * 8) job_free(j);
	}
	mutex_unlock(&mu);
}

/* (sous mu) met une page dans la file d'analyse */
static void queue_page(Tab *t, int i, int urgent)
{
	Page *p = &t->pages[i];
	p->lused = gen;
	p->urgent = urgent;
	if (p->lstate == L_NONE) p->lstate = L_QUEUED;
	if (p->lstate == L_QUEUED) t->lq[t->nlq++] = i;
}

static void schedule(void)
{
	static int *order;
	static int ocap;
	Tab *t = T;
	int first, last, i, k, cur, *tmp, n = 0, nvis, d, R, up, dn, all_ready = 1;
	double cx = vw * 0.5, cy = vh * 0.5;
	size_t limit = tex_budget / 10 * 7;
	if (!has_doc() || vw <= 0 || vh <= 0) return;
	gen++;
	t->last_gen = gen;
	wq_next.n = 0;
	want_bytes = 0;
	visible_range(&first, &last);
	cur = clampi(current_page(), first, last);
	R = render_level(t, t->level);
	up = grid_floor(t->level) + GRID;      /* niveau d'un cran de molette en avant / en arrière */
	dn = grid_ceil(t->level) - GRID;
	if (up > t->max_level) up = INT_MIN;
	if (dn < t->min_level) dn = INT_MIN;

	mutex_lock(&mu);
	tmp = t->lq_old; t->lq_old = t->lq; t->lq = tmp;
	t->nlq_old = t->nlq;
	t->nlq = t->lq_pos = 0;

	/* pages visibles, de la plus centrale vers l'extérieur, puis les voisines */
	if (ocap < t->npages + 8) { ocap = t->npages + 8; order = realloc(order, ocap * sizeof *order); }
	order[n++] = cur;
	for (d = 1; cur - d >= first || cur + d <= last; d++) {
		if (cur + d <= last) order[n++] = cur + d;
		if (cur - d >= first) order[n++] = cur - d;
	}
	nvis = n;
	if (last + 1 < t->npages) order[n++] = last + 1;
	if (first - 1 >= 0) order[n++] = first - 1;

	/* passe 1 : analyse des pages, tuiles visibles et couronne, pages voisines */
	for (k = 0; k < n; k++) {
		Page *p;
		Job *pj;
		int visible, a0, b0, a1, b1, r0, s0, r1, s1, tx, ty, has_fb;
		double X, Y, pdist;
		fz_irect pb;
		i = order[k];
		p = &t->pages[i];
		queue_page(t, i, 1);
		if (p->lstate != L_READY) {
			if (k < nvis && p->lstate != L_ERROR) all_ready = 0;
			continue;
		}
		visible = k < nvis;
		X = p->left - floor(t->sx);
		Y = p->top - floor(t->sy);
		pdist = hypot(X + p->pw / 2.0 - cx, Y + p->ph / 2.0 - cy);
		pb = level_pbox(t, p, R);
		if (!visible) {
			/* page voisine qui tiendra entièrement à l'écran : calculée en entier -> Page suivante instantané */
			if (p->pw <= vw && p->ph <= vh) {
				int nx = (pb.x1 - pb.x0 - 1) / TILE, ny = (pb.y1 - pb.y0 - 1) / TILE;
				for (ty = 0; ty <= ny; ty++)
					for (tx = 0; tx <= nx; tx++)
						want(get_tile(t, i, R, tx, ty), 2.5e9 + pdist + (ty * (nx + 1) + tx));
			}
			continue;
		}
		/* Aperçu basse résolution : seulement si on est zoomé bien au-delà de sa
		   résolution et qu'on n'a rien d'autre à étirer en attendant les tuiles. */
		pj = hfind(t, i, PREVIEW_LEVEL, 0, 0);
		has_fb = (pj && (pj->state == J_READY || pj->state == J_DONE)) || p->ready != NULL;
		if (!pj && !has_fb) pj = get_preview(t, i);
		if (pj && !has_fb && level_scale(R) > pj->scale * 1.5) want(pj, pdist);
		if (!tiles_for(t, p, R, 0, 0, &a0, &b0, &a1, &b1)) continue;
		tiles_for(t, p, R, TILE, TILE, &r0, &s0, &r1, &s1);
		for (ty = s0; ty <= s1; ty++)
			for (tx = r0; tx <= r1; tx++) {
				int inview = tx >= a0 && tx <= a1 && ty >= b0 && ty <= b1;
				want(get_tile(t, i, R, tx, ty), (inview ? 1e9 : 2e9) + tile_dist(t, p, R, pb, tx, ty));
			}
	}

	/* passe 2 : crans de zoom voisins sur la zone visible (en arrière, la zone s'élargit) */
	for (k = 0; k < nvis; k++) {
		i = order[k];
		if (t->pages[i].lstate != L_READY) continue;
		if (dn != INT_MIN && dn != R) want_area(t, i, dn, vw * 0.15, vh * 0.15, 3e9, limit);
		if (up != INT_MIN && up != R) want_area(t, i, up, 0, 0, 3.5e9, limit);
	}

	/* passe 3 : le reste de la page au niveau courant (déplacements), dans le budget */
	for (k = 0; k < nvis && want_bytes < limit; k++) {
		int a = vw / TILE + 2, b = vh / TILE + 2, m = 0;
		size_t room = (limit - want_bytes) / TILE_BYTES;
		i = order[k];
		if (t->pages[i].lstate != L_READY) continue;
		while (m < 64 && (size_t)((a + 2 * (m + 1)) * (b + 2 * (m + 1)) - a * b) <= room) m++;
		if (m > 0) want_area(t, i, R, (double)m * TILE, (double)m * TILE, 5e9, limit);
	}

	/* passe 4 : analyse de toutes les autres pages en tâche de fond, des plus proches
	   aux plus lointaines, tant que la mémoire des pages analysées le permet */
	if (all_ready) {
		size_t est, sum = t->list_bytes;
		int nready = 0;
		for (k = 0; k < t->npages; k++)
			if (t->pages[k].lstate == L_READY) nready++;
		est = nready ? t->list_bytes / nready : (size_t)16 << 20;
		for (d = 1; d < t->npages; d++) {
			int side;
			for (side = 0; side < 2; side++) {
				Page *p;
				i = side ? cur - d : cur + d;
				if (i < 0 || i >= t->npages) continue;
				p = &t->pages[i];
				if (p->lused == gen) continue;
				if (p->lstate == L_READY) { p->lused = gen; p->urgent = 0; continue; }
				if (p->lstate == L_ERROR || sum + est > list_budget / 10 * 7) continue;
				queue_page(t, i, 0);
				sum += est;
			}
		}
	}

	/* pages qui ne sont plus voulues */
	for (k = 0; k < t->nlq_old; k++) {
		Page *p = &t->pages[t->lq_old[k]];
		if (p->lstate == L_QUEUED && p->lused != gen) p->lstate = L_NONE;
	}
	{
		int need = 0, idle = 0;
		for (k = 0; k < t->nlq; k++)
			if (t->pages[t->lq[k]].urgent && t->pages[t->lq[k]].lstate == L_QUEUED) need++;
		for (k = 0; k < nloaders; k++) {
			if (!loader_busy[k]) idle++;
			else if (loader_tab[k] == t && loader_page[k] >= 0 && t->pages[loader_page[k]].lused != gen)
				t->pages[loader_page[k]].lcookie.abort = 1;
		}
		/* une page à l'écran attend alors que tous les chargeurs analysent des pages
		   lointaines : on en interrompt (elles seront reprises plus tard) */
		for (k = 0; k < nloaders && need > idle; k++)
			if (loader_busy[k] && loader_tab[k] == t && loader_page[k] >= 0 && !t->pages[loader_page[k]].urgent) {
				t->pages[loader_page[k]].lcookie.abort = 1;
				need--;
			}
	}

	/* nouvelle file de travail */
	for (k = wq_pos; k < wq.n; k++)
		if (wq.v[k]->state == J_QUEUED && wq.v[k]->used != gen) wq.v[k]->state = J_IDLE;
	if (wq_next.n > 1) qsort(wq_next.v, wq_next.n, sizeof *wq_next.v, cmp_key);
	for (k = 0; k < wq_next.n; k++) wq_next.v[k]->state = J_QUEUED;
	{ JobVec tv = wq; wq = wq_next; wq_next = tv; }
	wq_pos = 0;
	for (k = 0; k < nworkers; k++)
		if (running[k] && running[k]->used != gen) running[k]->cookie.abort = 1;
	cond_broadcast(&cv_work);
	cond_broadcast(&cv_load);
	mutex_unlock(&mu);

	evict();
}

/* ---------- résultats des threads ---------- */

int core_poll(void)
{
	Job *list;
	int changed = 0, relayout = 0, k, ti;
	double t0 = platform_time();
	mutex_lock(&mu);
	if (resched) {
		resched = 0; need_schedule = 1; changed = 1;
		/* PDF abîmé : la taille réelle d'une page diffère de celle annoncée -> on corrige */
		for (ti = 0; ti < ntabs; ti++) {
			Tab *t = tabs[ti];
			for (k = 0; k < t->npages; k++)
				if (t->pages[k].bounds_differ) {
					t->pages[k].bounds = t->pages[k].lbounds;
					t->pages[k].bounds_differ = 0;
					t->needs_relayout = 1;
					if (t == T) relayout = 1;
				}
		}
	}
	list = done_head;
	done_head = NULL;
	mutex_unlock(&mu);
	if (relayout) sync_view();
	/* les tuiles à l'écran d'abord, l'anticipation ensuite */
	if (list && list->dnext) {
		static JobVec dv;
		Job *j;
		dv.n = 0;
		for (j = list; j; j = j->dnext) jv_push(&dv, j);
		qsort(dv.v, dv.n, sizeof *dv.v, cmp_key);
		for (k = 0; k < dv.n; k++) dv.v[k]->dnext = k + 1 < dv.n ? dv.v[k + 1] : NULL;
		list = dv.v[0];
	}
	while (list) {
		Job *j = list;
		list = j->dnext;
		j->dnext = NULL;
		if (j->dropped) {
			fz_drop_pixmap(ctx, j->pix);
			free(j);
			continue;
		}
		j->tex = gfx_tex_create(fz_pixmap_width(ctx, j->pix), fz_pixmap_height(ctx, j->pix),
				fz_pixmap_samples(ctx, j->pix), (int)fz_pixmap_stride(ctx, j->pix));
		j->bytes = (size_t)fz_pixmap_width(ctx, j->pix) * fz_pixmap_height(ctx, j->pix) * 4;
		fz_drop_pixmap(ctx, j->pix);
		j->pix = NULL;
		mutex_lock(&mu);
		j->state = j->tex ? J_READY : J_FAILED;
		mutex_unlock(&mu);
		if (j->tex) {
			j->tab->tex_bytes += j->bytes;
			tex_total += j->bytes;
			ready_add(&j->tab->pages[j->page], j);
		}
		changed = 1;
		/* on limite le temps passé à envoyer des textures pour garder l'interface fluide */
		if (list && platform_time() - t0 > 0.006) {
			Job *tail = list;
			while (tail->dnext) tail = tail->dnext;
			mutex_lock(&mu);
			tail->dnext = done_head;
			done_head = list;
			mutex_unlock(&mu);
			platform_wake();
			break;
		}
	}
	if (changed) dirty = 1;
	return changed;
}

int core_busy(void)
{
	int busy, k;
	mutex_lock(&mu);
	busy = nrunning > 0 || any_loader_busy() || done_head != NULL || resched;
	for (k = wq_pos; !busy && k < wq.n; k++)
		if (wq.v[k]->state == J_QUEUED) busy = 1;
	if (T && T->lq)
		for (k = T->lq_pos; !busy && k < T->nlq; k++)
			if (T->pages[T->lq[k]].lstate == L_QUEUED) busy = 1;
	mutex_unlock(&mu);
	return busy || view_dirty || need_schedule;
}

/* 1 si toutes les tuiles visibles sont nettes (pour les mesures et l'indicateur) */
int core_view_complete(void)
{
	int first, last, i, a0, b0, a1, b1, tx, ty, R;
	if (!has_doc()) return 1;
	if (view_dirty || need_schedule) return 0;
	visible_range(&first, &last);
	R = render_level(T, T->level);
	for (i = first; i <= last; i++) {
		Page *p = &T->pages[i];
		if (p->lstate == L_ERROR) continue;
		if (p->lstate != L_READY) return 0;
		if (!tiles_for(T, p, R, 0, 0, &a0, &b0, &a1, &b1)) continue;
		for (ty = b0; ty <= b1; ty++)
			for (tx = a0; tx <= a1; tx++) {
				Job *j = hfind(T, i, R, tx, ty);
				if (!j || (j->state != J_READY && j->state != J_FAILED)) return 0;
			}
	}
	return 1;
}

/* ---------- texte et icônes de l'interface (rendus par MuPDF) ---------- */

typedef struct { char s[200]; int px, bold; void *tex; int w, h; unsigned used; } TextTex;
#define NTEXT 160
static TextTex texts[NTEXT];
static unsigned text_clock;
static fz_font *font_regular, *font_bold;

static TextTex *text_get(const char *s, int px, int bold)
{
	int i, slot = 0;
	TextTex *t;
	fz_text *text = NULL;
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;
	for (i = 0; i < NTEXT; i++)
		if (texts[i].px == px && texts[i].bold == bold && !strcmp(texts[i].s, s)) {
			texts[i].used = ++text_clock;
			return &texts[i];
		}
	for (i = 1; i < NTEXT; i++)
		if (texts[i].used < texts[slot].used) slot = i;
	t = &texts[slot];
	if (t->tex) gfx_tex_free(t->tex);
	memset(t, 0, sizeof *t);
	snprintf(t->s, sizeof t->s, "%s", s);
	t->px = px;
	t->bold = bold;
	t->used = ++text_clock;
	fz_var(text);
	fz_var(pix);
	fz_var(dev);
	fz_try(ctx) {
		fz_font *font = bold ? font_bold : font_regular;
		float asc = fz_font_ascender(ctx, font), desc = fz_font_descender(ctx, font);
		float base = ceilf(asc * px) + 1;
		float white[3] = { 1, 1, 1 };
		fz_matrix end;
		text = fz_new_text(ctx);
		end = fz_show_string(ctx, text, font, fz_make_matrix((float)px, 0, 0, (float)-px, 1, base), s, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
		t->w = (int)ceilf(end.e) + 2;
		t->h = (int)ceilf((asc - desc) * px) + 2;
		pix = fz_new_pixmap(ctx, fz_device_bgr(ctx), t->w, t->h, NULL, 1);
		fz_clear_pixmap(ctx, pix);
		dev = fz_new_draw_device(ctx, fz_identity, pix);
		fz_fill_text(ctx, dev, text, fz_identity, fz_device_bgr(ctx), white, 1, fz_default_color_params);
		fz_close_device(ctx, dev);
		t->tex = gfx_tex_create(t->w, t->h, fz_pixmap_samples(ctx, pix), (int)fz_pixmap_stride(ctx, pix));
	}
	fz_always(ctx) {
		fz_drop_device(ctx, dev);
		fz_drop_text(ctx, text);
		fz_drop_pixmap(ctx, pix);
	}
	fz_catch(ctx) {
		t->w = t->h = 0;
	}
	return t;
}

static int text_width(const char *s, int px, int bold) { return text_get(s, px, bold)->w; }

/* largeur d'un texte sans créer de texture */
static float text_measure(const char *s, int px, int bold)
{
	fz_text *text = NULL;
	float w = 0;
	fz_var(text);
	fz_try(ctx) {
		fz_matrix end;
		text = fz_new_text(ctx);
		end = fz_show_string(ctx, text, bold ? font_bold : font_regular, fz_make_matrix((float)px, 0, 0, (float)-px, 1, 0),
				s, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
		w = end.e + 2;
	}
	fz_always(ctx)
		fz_drop_text(ctx, text);
	fz_catch(ctx)
		w = 0;
	return w;
}

static int draw_text(const char *s, int px, int bold, float x, float y, unsigned argb)
{
	TextTex *t = text_get(s, px, bold);
	if (t->tex)
		gfx_image(t->tex, 0, 0, (float)t->w, (float)t->h, x, y, x + t->w, y + t->h, argb);
	return t->w;
}

/* icônes de rotation (flèche circulaire), dessinées avec MuPDF */
static void *icon_rot[2];
static int icon_size, icon_w;

static void *make_rot_icon(int size, int clockwise)
{
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;
	fz_path *arc = NULL, *head = NULL;
	fz_stroke_state *ss = NULL;
	void *tex = NULL;
	fz_var(pix); fz_var(dev); fz_var(arc); fz_var(head); fz_var(ss);
	fz_try(ctx) {
		float c = size / 2.0f, r = size * 0.30f, white[3] = { 1, 1, 1 };
		float a1 = 228 * 3.14159265f / 180, px, py, dx, dy, nx, ny, hl = size * 0.32f, hw = size * 0.23f;
		int k, n = 32;
		pix = fz_new_pixmap(ctx, fz_device_bgr(ctx), size, size, NULL, 1);
		fz_clear_pixmap(ctx, pix);
		arc = fz_new_path(ctx);
		for (k = 0; k <= n; k++) {
			/* arc de -48° à 228° parcouru dans le sens horaire (y vers le bas) */
			float a = (-48 + 276.0f * k / n) * 3.14159265f / 180;
			float x = c + r * cosf(a), y = c + r * sinf(a);
			if (!clockwise) x = size - x;
			if (k == 0) fz_moveto(ctx, arc, x, y); else fz_lineto(ctx, arc, x, y);
		}
		/* pointe de flèche au bout de l'arc, dans le sens du parcours */
		px = c + r * cosf(a1); py = c + r * sinf(a1);
		dx = -sinf(a1); dy = cosf(a1);
		if (!clockwise) { px = size - px; dx = -dx; }
		nx = -dy; ny = dx;
		head = fz_new_path(ctx);
		fz_moveto(ctx, head, px + dx * hl * 0.75f, py + dy * hl * 0.75f);
		fz_lineto(ctx, head, px + nx * hw - dx * hl * 0.25f, py + ny * hw - dy * hl * 0.25f);
		fz_lineto(ctx, head, px - nx * hw - dx * hl * 0.25f, py - ny * hw - dy * hl * 0.25f);
		fz_closepath(ctx, head);
		ss = fz_new_stroke_state(ctx);
		ss->linewidth = size * 0.10f;
		dev = fz_new_draw_device(ctx, fz_identity, pix);
		fz_stroke_path(ctx, dev, arc, ss, fz_identity, fz_device_bgr(ctx), white, 1, fz_default_color_params);
		fz_fill_path(ctx, dev, head, 0, fz_identity, fz_device_bgr(ctx), white, 1, fz_default_color_params);
		fz_close_device(ctx, dev);
		tex = gfx_tex_create(size, size, fz_pixmap_samples(ctx, pix), (int)fz_pixmap_stride(ctx, pix));
	}
	fz_always(ctx) {
		fz_drop_device(ctx, dev);
		fz_drop_path(ctx, arc);
		fz_drop_path(ctx, head);
		fz_drop_stroke_state(ctx, ss);
		fz_drop_pixmap(ctx, pix);
	}
	fz_catch(ctx)
		tex = NULL;
	return tex;
}

/* repère du défilement automatique (clic molette maintenu) : rond et 4 flèches */
static void *icon_auto;
static int icon_auto_size;

static void *make_auto_icon(int size)
{
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;
	fz_path *disc = NULL, *arrows = NULL, *dot = NULL;
	fz_stroke_state *ss = NULL;
	void *tex = NULL;
	fz_var(pix); fz_var(dev); fz_var(disc); fz_var(arrows); fz_var(dot); fz_var(ss);
	fz_try(ctx) {
		float c = size / 2.0f, r = size * 0.44f, dark[3] = { 0.13f, 0.14f, 0.16f }, light[3] = { 0.92f, 0.93f, 0.95f };
		float a = size * 0.30f, w = size * 0.11f, h = size * 0.12f;
		int k, n = 40;
		pix = fz_new_pixmap(ctx, fz_device_bgr(ctx), size, size, NULL, 1);
		fz_clear_pixmap(ctx, pix);
		disc = fz_new_path(ctx);
		dot = fz_new_path(ctx);
		for (k = 0; k < n; k++) {
			float t = k * 6.2831853f / n;
			if (k == 0) {
				fz_moveto(ctx, disc, c + r * cosf(t), c + r * sinf(t));
				fz_moveto(ctx, dot, c + size * 0.07f * cosf(t), c + size * 0.07f * sinf(t));
			} else {
				fz_lineto(ctx, disc, c + r * cosf(t), c + r * sinf(t));
				fz_lineto(ctx, dot, c + size * 0.07f * cosf(t), c + size * 0.07f * sinf(t));
			}
		}
		fz_closepath(ctx, disc);
		fz_closepath(ctx, dot);
		arrows = fz_new_path(ctx);
		for (k = 0; k < 4; k++) {   /* pointes vers le haut, la droite, le bas, la gauche */
			float dx = k == 1 ? 1.0f : k == 3 ? -1.0f : 0.0f, dy = k == 0 ? -1.0f : k == 2 ? 1.0f : 0.0f;
			float tx = c + dx * a, ty = c + dy * a, bx = c + dx * (a - h), by = c + dy * (a - h);
			fz_moveto(ctx, arrows, tx, ty);
			fz_lineto(ctx, arrows, bx - dy * w, by + dx * w);
			fz_lineto(ctx, arrows, bx + dy * w, by - dx * w);
			fz_closepath(ctx, arrows);
		}
		ss = fz_new_stroke_state(ctx);
		ss->linewidth = fmaxf(1.0f, size * 0.05f);
		dev = fz_new_draw_device(ctx, fz_identity, pix);
		fz_fill_path(ctx, dev, disc, 0, fz_identity, fz_device_bgr(ctx), dark, 0.85f, fz_default_color_params);
		fz_stroke_path(ctx, dev, disc, ss, fz_identity, fz_device_bgr(ctx), light, 0.9f, fz_default_color_params);
		fz_fill_path(ctx, dev, arrows, 0, fz_identity, fz_device_bgr(ctx), light, 1, fz_default_color_params);
		fz_fill_path(ctx, dev, dot, 0, fz_identity, fz_device_bgr(ctx), light, 1, fz_default_color_params);
		fz_close_device(ctx, dev);
		tex = gfx_tex_create(size, size, fz_pixmap_samples(ctx, pix), (int)fz_pixmap_stride(ctx, pix));
	}
	fz_always(ctx) {
		fz_drop_device(ctx, dev);
		fz_drop_path(ctx, disc);
		fz_drop_path(ctx, dot);
		fz_drop_path(ctx, arrows);
		fz_drop_stroke_state(ctx, ss);
		fz_drop_pixmap(ctx, pix);
	}
	fz_catch(ctx)
		tex = NULL;
	return tex;
}

static void ensure_icons(void)
{
	int s = (int)lround(20 * ui);
	if (s == icon_size && icon_rot[0]) return;
	if (icon_rot[0]) gfx_tex_free(icon_rot[0]);
	if (icon_rot[1]) gfx_tex_free(icon_rot[1]);
	icon_rot[0] = make_rot_icon(s, 0);
	icon_rot[1] = make_rot_icon(s, 1);
	icon_size = icon_w = s;
}

/* ---------- barre d'onglets ---------- */

typedef struct { float x0, tab_w, avail_x1, plus_x0, plus_x1, ccw_x0, cw_x0, btn_w, top; } BarLayout;

static void bar_layout(BarLayout *L)
{
	float pad = 6 * ui, plus_w = bar_h * 0.95f, avail, total;
	L->top = floorf(6 * ui);
	L->btn_w = (float)bar_h;
	L->cw_x0 = win_w - pad - L->btn_w;
	L->ccw_x0 = L->cw_x0 - L->btn_w;
	L->x0 = pad;
	L->avail_x1 = L->ccw_x0 - pad - plus_w;
	avail = fmaxf(L->avail_x1 - L->x0, 1);
	L->tab_w = ntabs ? floorf(fminf(fmaxf(avail / ntabs, 90 * ui), 240 * ui)) : 0;
	total = ntabs * L->tab_w;
	if (ensure_tab_visible && active >= 0) {
		float a0 = active * L->tab_w;
		if (a0 < tab_scroll) tab_scroll = a0;
		if (a0 + L->tab_w > tab_scroll + avail) tab_scroll = a0 + L->tab_w - avail;
		ensure_tab_visible = 0;
	}
	tab_scroll = fmaxf(0, fminf(tab_scroll, fmaxf(0, total - avail)));
	L->plus_x0 = L->x0 + fminf(total, avail) + 2 * ui;
	L->plus_x1 = L->plus_x0 + plus_w;
}

static float tab_x(const BarLayout *L, int i) { return L->x0 + i * L->tab_w - tab_scroll; }

static void close_box(const BarLayout *L, int i, float *x0, float *y0, float *x1, float *y1)
{
	float s = floorf(18 * ui), tx1 = tab_x(L, i) + L->tab_w - 1;
	*x1 = tx1 - floorf(7 * ui);
	*x0 = *x1 - s;
	*y0 = floorf(L->top + (bar_h - L->top - s) / 2);
	*y1 = *y0 + s;
}

static int bar_hit(int x, int y)
{
	BarLayout L;
	int i;
	if (y < 0 || y >= bar_h || x < 0 || x >= win_w) return HIT_NONE;
	bar_layout(&L);
	if (x >= L.cw_x0 && x < L.cw_x0 + L.btn_w) return HIT_ROT_CW;
	if (x >= L.ccw_x0 && x < L.ccw_x0 + L.btn_w) return HIT_ROT_CCW;
	if (x >= L.plus_x0 && x < L.plus_x1) return HIT_PLUS;
	if (x >= L.avail_x1 || y < L.top) return HIT_NONE;
	for (i = 0; i < ntabs; i++) {
		float x0 = tab_x(&L, i), x1 = x0 + L.tab_w, c0, c1, d0, d1;
		if (x < x0 || x >= x1) continue;
		close_box(&L, i, &c0, &d0, &c1, &d1);
		if (x >= c0 && x < c1 && y >= d0 - 4 * ui && y < d1 + 4 * ui) return HIT_CLOSE + i;
		return HIT_TAB + i;
	}
	return HIT_NONE;
}

/* nom raccourci avec « … » pour tenir dans la largeur de l'onglet */
static const char *tab_label(Tab *t, int px, int maxw)
{
	int lo, hi, n;
	const char *s = t->name ? t->name : "";
	char buf[sizeof t->label];
	if (t->label_w == maxw && t->label_px == px) return t->label;
	t->label_w = maxw;
	t->label_px = px;
	if (text_measure(s, px, 0) <= maxw) {
		snprintf(t->label, sizeof t->label, "%s", s);
		return t->label;
	}
	n = (int)strlen(s);
	lo = 0; hi = n;
	while (lo < hi) {   /* plus long préfixe (en caractères UTF-8 entiers) qui tient avec « … » */
		int mid = (lo + hi + 1) / 2, m = mid;
		while (m > 0 && (s[m] & 0xc0) == 0x80) m--;
		snprintf(buf, sizeof buf, "%.*s\xe2\x80\xa6", m, s);
		if (text_measure(buf, px, 0) <= maxw) lo = mid; else hi = mid - 1;
	}
	while (lo > 0 && (s[lo] & 0xc0) == 0x80) lo--;
	snprintf(t->label, sizeof t->label, "%.*s\xe2\x80\xa6", lo, s);
	return t->label;
}

static void draw_tabbar(void)
{
	BarLayout L;
	int i, px = (int)lround(13 * ui);
	float c0, d0, c1, d1;
	bar_layout(&L);
	gfx_rect(0, 0, (float)win_w, (float)bar_h, 0xff222427);
	gfx_rect(0, bar_h - 1.0f, (float)win_w, (float)bar_h, 0xff18191b);
	for (i = 0; i < ntabs; i++) {
		Tab *t = tabs[i];
		float x0 = tab_x(&L, i), x1 = x0 + L.tab_w - 1, ty;
		int act = i == active, hv = hover == HIT_TAB + i || hover == HIT_CLOSE + i;
		const char *lab;
		TextTex *tt;
		if (x1 < 0 || x0 > L.avail_x1) continue;
		gfx_rect(x0, L.top, x1, act ? (float)bar_h : bar_h - 1.0f,
				act ? 0xff000000 | BG_COLOR : hv ? 0xff313337 : 0xff2a2c2f);
		if (act) gfx_rect(x0, L.top, x1, L.top + 2 * ui, 0xff4a90d9);
		lab = tab_label(t, px, (int)(x1 - x0 - 10 * ui - 30 * ui));
		tt = text_get(lab, px, 0);
		ty = floorf(L.top + (bar_h - L.top - tt->h) / 2.0f) + 1;
		draw_text(lab, px, 0, x0 + floorf(10 * ui), ty, act ? 0xffffffff : hv ? 0xffd8dce2 : 0xff9ea4ac);
		close_box(&L, i, &c0, &d0, &c1, &d1);
		if (hover == HIT_CLOSE + i) gfx_rect(c0, d0, c1, d1, 0xff4b4f55);
		if (act || hv) {
			TextTex *xt = text_get("\xc3\x97", (int)lround(16 * ui), 0);
			draw_text("\xc3\x97", (int)lround(16 * ui), 0, floorf((c0 + c1 - xt->w) / 2) + 1,
					floorf((d0 + d1 - xt->h) / 2), act ? 0xffd0d4da : 0xffa0a6ae);
		}
	}
	/* au-delà de la zone des onglets (débordement) : fond de la barre */
	gfx_rect(L.avail_x1, 0, (float)win_w, bar_h - 1.0f, 0xff222427);
	/* bouton + */
	{
		int pp = (int)lround(19 * ui);
		TextTex *pt = text_get("+", pp, 0);
		float b0 = L.top + 2 * ui, b1 = bar_h - 3 * ui;
		if (hover == HIT_PLUS) gfx_rect(L.plus_x0, b0, L.plus_x1, b1, 0xff3a3d42);
		draw_text("+", pp, 0, floorf((L.plus_x0 + L.plus_x1 - pt->w) / 2), floorf((b0 + b1 - pt->h) / 2), 0xffc8ccd2);
	}
	/* boutons de rotation */
	ensure_icons();
	for (i = 0; i < 2; i++) {
		float bx = i == 0 ? L.ccw_x0 : L.cw_x0, b0 = 4 * ui, b1 = bar_h - 4 * ui;
		int h = i == 0 ? HIT_ROT_CCW : HIT_ROT_CW;
		unsigned col = has_doc() ? (hover == h ? 0xffffffff : 0xffc8ccd2) : 0x60c8ccd2;
		if (hover == h && has_doc()) gfx_rect(bx + 2 * ui, b0, bx + L.btn_w - 2 * ui, b1, 0xff3a3d42);
		if (icon_rot[i]) {
			float ix = floorf(bx + (L.btn_w - icon_w) / 2), iy = floorf((bar_h - icon_w) / 2.0f);
			gfx_image(icon_rot[i], 0, 0, (float)icon_w, (float)icon_w, ix, iy, ix + icon_w, iy + icon_w, col);
		}
	}
}

static void draw_tooltip(void)
{
	char buf[1100];
	const char *s = NULL;
	BarLayout L;
	float x, w, h, pad = 7 * ui;
	int px = (int)lround(12 * ui);
	TextTex *tt;
	if (hover == HIT_NONE || drag || platform_time() - hover_since < 0.5) return;
	bar_layout(&L);
	switch (hover) {
	case HIT_PLUS: s = "Ouvrir un fichier  (Ctrl+O)"; x = L.plus_x0; break;
	case HIT_ROT_CCW: s = "Rotation antihoraire  (Maj+R)"; x = L.ccw_x0; break;
	case HIT_ROT_CW: s = "Rotation horaire  (R)"; x = L.cw_x0; break;
	default:
		if (hover >= HIT_CLOSE && hover - HIT_CLOSE < ntabs) {
			s = "Fermer l'onglet  (Ctrl+W)";
			x = tab_x(&L, hover - HIT_CLOSE) + L.tab_w - 30 * ui;
		} else if (hover >= HIT_TAB && hover - HIT_TAB < ntabs) {
			snprintf(buf, sizeof buf, "%s", tabs[hover - HIT_TAB]->path);
			s = buf;
			x = tab_x(&L, hover - HIT_TAB);
		} else return;
	}
	tt = text_get(s, px, 0);
	w = tt->w + 2 * pad;
	h = tt->h + pad;
	if (x + w > win_w - 4) x = win_w - 4 - w;
	if (x < 4) x = 4;
	gfx_rect(x, bar_h + 4 * ui, x + w, bar_h + 4 * ui + h, 0xf0101113);
	draw_text(s, px, 0, x + pad, bar_h + 4 * ui + pad / 2, 0xffe6e8eb);
}

/* ---------- dessin de la vue ---------- */

static int vbar(float *x0, float *y0, float *x1, float *y1, float *t0, float *t1)
{
	float len, th;
	if (!has_doc() || T->ch <= vh) return 0;
	*x0 = (float)(vw - sbw); *x1 = (float)vw;
	*y0 = 0; *y1 = (float)(vh - (T->cw > vw ? sbw : 0));
	len = *y1 - *y0;
	th = fmaxf(len * (float)(vh / T->ch), 28 * ui);
	*t0 = *y0 + (float)(T->sy / (T->ch - vh)) * (len - th);
	*t1 = *t0 + th;
	return 1;
}

static int hbar(float *x0, float *y0, float *x1, float *y1, float *t0, float *t1)
{
	float len, th;
	if (!has_doc() || T->cw <= vw) return 0;
	*y0 = (float)(vh - sbw); *y1 = (float)vh;
	*x0 = 0; *x1 = (float)(vw - (T->ch > vh ? sbw : 0));
	len = *x1 - *x0;
	th = fmaxf(len * (float)(vw / T->cw), 28 * ui);
	*t0 = *x0 + (float)(T->sx / (T->cw - vw)) * (len - th);
	*t1 = *t0 + th;
	return 1;
}

static int fb_R;

static int cmp_fallback(const void *a, const void *b)
{
	const Job *x = *(Job *const *)a, *y = *(Job *const *)b;
	int dx = x->level == PREVIEW_LEVEL ? INT_MAX : abs(x->level - fb_R);
	int dy = y->level == PREVIEW_LEVEL ? INT_MAX : abs(y->level - fb_R);
	return dx > dy ? -1 : dx < dy;
}

/* dessine une tuile (de n'importe quel niveau) à sa place à l'écran */
static void draw_tile(Tab *t, Page *p, Job *f, float X, float Y)
{
	float x0, y0, x1, y1;
	if (f->level == PREVIEW_LEVEL) {
		x0 = X; y0 = Y; x1 = X + p->pw; y1 = Y + p->ph;
	} else if (f->level == t->level) {   /* niveau affiché : au pixel près */
		x0 = X + (f->rect.x0 - p->pbox.x0);
		y0 = Y + (f->rect.y0 - p->pbox.y0);
		x1 = x0 + (f->rect.x1 - f->rect.x0);
		y1 = y0 + (f->rect.y1 - f->rect.y0);
	} else {                             /* autre niveau : la carte graphique met à l'échelle */
		double r = t->scale / f->scale;
		x0 = X + (float)(f->rect.x0 * r - p->pbox.x0);
		y0 = Y + (float)(f->rect.y0 * r - p->pbox.y0);
		x1 = X + (float)(f->rect.x1 * r - p->pbox.x0);
		y1 = Y + (float)(f->rect.y1 * r - p->pbox.y0);
	}
	if (x1 < 0 || y1 < 0 || x0 > vw || y0 > vh) return;
	gfx_image(f->tex, 0, 0, (float)(f->rect.x1 - f->rect.x0), (float)(f->rect.y1 - f->rect.y0), x0, y0, x1, y1, 0);
}

static void draw_pages(void)
{
	static Job **fb;
	static int fbcap;
	Tab *t = T;
	int first, last, i, R = render_level(T, T->level);
	double fx = floor(t->sx), fy = floor(t->sy);
	visible_range(&first, &last);
	for (i = first; i <= last; i++) {
		Page *p = &t->pages[i];
		float X = (float)(p->left - fx), Y = (float)(p->top - fy);
		int a0, b0, a1, b1, tx, ty, complete = 1, drawn = 0;
		gfx_rect(X - 1, Y - 1, X + p->pw + 1, Y + p->ph + 1, 0xff1e2023);
		gfx_rect(X, Y, X + p->pw, Y + p->ph, 0xffffffff);
		if (!tiles_for(t, p, R, 0, 0, &a0, &b0, &a1, &b1)) continue;
		for (ty = b0; ty <= b1 && complete; ty++)
			for (tx = a0; tx <= a1; tx++) {
				Job *j = hfind(t, i, R, tx, ty);
				if (!j || j->state != J_READY) { complete = 0; break; }
			}
		if (!complete) {
			/* repli : aperçu et tuiles d'autres niveaux, étirés par la carte graphique,
			   du plus éloigné au plus proche du niveau voulu (le plus fidèle par-dessus) */
			int n = 0, k;
			Job *j;
			for (j = p->ready; j; j = j->pnext) {
				if (j->level == R) continue;
				if (n == fbcap) { fbcap = fbcap ? fbcap * 2 : 64; fb = realloc(fb, fbcap * sizeof *fb); }
				fb[n++] = j;
			}
			fb_R = R;
			if (n > 1) qsort(fb, n, sizeof *fb, cmp_fallback);
			for (k = 0; k < n; k++) draw_tile(t, p, fb[k], X, Y);
			drawn = n > 0;
		}
		for (ty = b0; ty <= b1; ty++)
			for (tx = a0; tx <= a1; tx++) {
				Job *j = hfind(t, i, R, tx, ty);
				if (j && j->state == J_READY) {
					draw_tile(t, p, j, X, Y);
					drawn = 1;
				}
			}
		if (p->pw > 160 * ui && p->ph > 40 * ui) {
			const char *msg = NULL;
			if (p->lstate == L_ERROR) msg = "Page illisible";
			else if (!drawn) msg = "Chargement\xe2\x80\xa6";
			if (msg) {
				int px = (int)(14 * ui), w = text_width(msg, px, 0);
				float tx0 = X + p->pw / 2.0f - w / 2.0f, ty0 = fmaxf(Y, 0) + fminf(p->ph, (float)vh) / 2.0f - px;
				draw_text(msg, px, 0, tx0, ty0, 0xff8a8f96);
			}
		}
	}
}

static const char *help_lines[][2] = {
	{ "Ouvrir un fichier (nouvel onglet)", "Ctrl+O, bouton +  ou glisser-d\xc3\xa9poser" },
	{ "Onglet suivant / pr\xc3\xa9""c\xc3\xa9""dent", "Ctrl+Tab / Ctrl+Maj+Tab" },
	{ "Fermer l'onglet", "Ctrl+W  ou clic molette sur l'onglet" },
	{ "Rouvrir l'onglet ferm\xc3\xa9", "Ctrl+Maj+T" },
	{ "D\xc3\xa9""filer", "Molette, fl\xc3\xa8""ches, Espace" },
	{ "Zoomer / d\xc3\xa9zoomer", "Ctrl+Molette,  +  /  -" },
	{ "Zoom sur une zone", "Clic droit + glisser" },
	{ "D\xc3\xa9placer la vue", "Clic gauche + glisser" },
	{ "D\xc3\xa9""filement automatique", "Maintenir le clic molette, viser la direction" },
	{ "Page enti\xc3\xa8re / pleine largeur", "F (Ctrl+0)  /  L (Ctrl+2)" },
	{ "Taille r\xc3\xa9""elle (100 %)", "Ctrl+1" },
	{ "Page pr\xc3\xa9""c\xc3\xa9""dente / suivante", "Pg.Pr\xc3\xa9""c / Pg.Suiv" },
	{ "Premi\xc3\xa8re / derni\xc3\xa8re page", "D\xc3\xa9""but / Fin" },
	{ "Aller \xc3\xa0 la page", "Ctrl+G  ou taper le num\xc3\xa9ro" },
	{ "Rotation horaire", "R,  Ctrl+Maj++  ou bouton en haut \xc3\xa0 droite" },
	{ "Rotation antihoraire", "Maj+R,  Ctrl+Maj+-  ou bouton" },
	{ "Molette = zoom ou d\xc3\xa9""filement", "M" },
	{ "Recharger le fichier", "F5" },
	{ "Plein \xc3\xa9""cran", "F11" },
	{ "Quitter", "Ctrl+Q" },
};

static void draw_help(void)
{
	int n = (int)(sizeof help_lines / sizeof help_lines[0]), i;
	int px = (int)(14 * ui), lh = (int)(px * 1.7f), pad = (int)(20 * ui);
	int w0 = 0, w1 = 0, title = (int)(18 * ui);
	float x0, y0, w, h;
	for (i = 0; i < n; i++) {
		int a = text_width(help_lines[i][0], px, 0), b = text_width(help_lines[i][1], px, 1);
		if (a > w0) w0 = a;
		if (b > w1) w1 = b;
	}
	w = (float)(w0 + w1 + 3 * pad);
	h = (float)(n * lh + 2 * pad + title * 2);
	x0 = floorf((vw - w) / 2);
	y0 = fmaxf(4, floorf((vh - h) / 2));
	gfx_rect(0, 0, (float)vw, (float)vh, 0x60000000);
	gfx_rect(x0, y0, x0 + w, y0 + h, 0xf0202226);
	gfx_rect(x0, y0, x0 + w, y0 + 2 * ui, 0xff4a90d9);
	draw_text("Lire \xe2\x80\x94 raccourcis", title, 1, x0 + pad, y0 + pad * 0.8f, 0xffffffff);
	for (i = 0; i < n; i++) {
		float y = y0 + pad + title * 2 + i * lh;
		draw_text(help_lines[i][0], px, 0, x0 + pad, y, 0xffc9ccd1);
		draw_text(help_lines[i][1], px, 1, x0 + 2 * pad + w0, y, 0xffffffff);
	}
}

static void draw_overlays(void)
{
	char buf[300];
	float a, b, c, d, t0, t1;
	int px = (int)(13 * ui);
	double t = platform_time();

	if (!T) {
		int big = (int)(20 * ui), w;
		const char *l1 = "Glissez un ou plusieurs fichiers PDF ici";
		const char *l2 = "ou cliquez / Ctrl+O pour ouvrir   \xc2\xb7   F1 : aide";
		w = text_width(l1, big, 1);
		draw_text(l1, big, 1, floorf((vw - w) / 2.0f), floorf(vh / 2.0f - big * 1.6f), 0xffe6e8eb);
		w = text_width(l2, px + 1, 0);
		draw_text(l2, px + 1, 0, floorf((vw - w) / 2.0f), floorf(vh / 2.0f + px * 0.5f), 0xff9aa0a8);
	}

	if (drag == DRAG_AUTO) {
		int s = (int)lround(30 * ui);
		if (s != icon_auto_size || !icon_auto) {
			if (icon_auto) gfx_tex_free(icon_auto);
			icon_auto = make_auto_icon(s);
			icon_auto_size = s;
		}
		if (icon_auto)
			gfx_image(icon_auto, 0, 0, (float)s, (float)s, mx0 - s / 2.0f, my0 - s / 2.0f, mx0 + s / 2.0f, my0 + s / 2.0f, 0xffffffff);
	}

	if (drag == DRAG_ZOOM) {
		float x0 = (float)(mx0 < mx ? mx0 : mx), x1 = (float)(mx0 < mx ? mx : mx0);
		float y0 = (float)(my0 < my ? my0 : my), y1 = (float)(my0 < my ? my : my0);
		gfx_rect(x0, y0, x1, y1, 0x304a90d9);
		gfx_rect(x0, y0, x1, y0 + 1, 0xff4a90d9);
		gfx_rect(x0, y1 - 1, x1, y1, 0xff4a90d9);
		gfx_rect(x0, y0, x0 + 1, y1, 0xff4a90d9);
		gfx_rect(x1 - 1, y0, x1, y1, 0xff4a90d9);
	}

	if (vbar(&a, &b, &c, &d, &t0, &t1)) {
		gfx_rect(a, b, c, d, 0x40000000);
		gfx_rect(a + 3 * ui, t0 + 2, c - 3 * ui, t1 - 2, drag == DRAG_VBAR ? 0xe0d0d4da : 0xa0a0a6ae);
		if (drag == DRAG_VBAR && T->npages > 1) {
			int w;
			snprintf(buf, sizeof buf, "Page %d / %d", current_page() + 1, T->npages);
			w = text_width(buf, px + 2, 1);
			gfx_rect(a - w - 26 * ui, (t0 + t1) / 2 - px, a - 8 * ui, (t0 + t1) / 2 + px + 4 * ui, 0xe0202226);
			draw_text(buf, px + 2, 1, a - w - 17 * ui, (t0 + t1) / 2 - px * 0.75f, 0xffffffff);
		}
	}
	if (hbar(&a, &b, &c, &d, &t0, &t1)) {
		gfx_rect(a, b, c, d, 0x40000000);
		gfx_rect(t0 + 2, b + 3 * ui, t1 - 2, d - 3 * ui, drag == DRAG_HBAR ? 0xe0d0d4da : 0xa0a0a6ae);
	}

	if (has_doc()) {
		int w, pad = (int)(8 * ui), right = vw - (T->ch > vh ? sbw : 0) - (int)(10 * ui);
		int bottom = vh - (T->cw > vw ? sbw : 0) - (int)(10 * ui);
		TextTex *st;
		if (T->rot)
			snprintf(buf, sizeof buf, "%d / %d    %.0f %%    %d\xc2\xb0%s", current_page() + 1, T->npages, core_zoom_percent(),
					T->rot, core_view_complete() ? "" : "   \xe2\x80\xa6");
		else
			snprintf(buf, sizeof buf, "%d / %d    %.0f %%%s", current_page() + 1, T->npages, core_zoom_percent(),
					core_view_complete() ? "" : "   \xe2\x80\xa6");
		st = text_get(buf, px, 0);
		w = st->w;
		gfx_rect((float)(right - w - 2 * pad), (float)(bottom - st->h - pad), (float)right, (float)bottom, 0xc0202226);
		draw_text(buf, px, 0, (float)(right - w - pad), (float)(bottom - st->h - pad / 2), 0xffe6e8eb);
	}

	if (toast[0] && t < toast_until) {
		int big = (int)(15 * ui), pad = (int)(11 * ui);
		TextTex *tt = text_get(toast, big, 1);
		int w = tt->w, th = tt->h;
		float x0 = floorf((vw - w) / 2.0f) - pad, y0 = floorf(vh * 0.82f);
		gfx_rect(x0, y0, x0 + w + 2 * pad, y0 + th + 2 * pad, 0xe0202226);
		draw_text(toast, big, 1, x0 + pad, y0 + pad, 0xffffffff);
	}

	if (input_mode != INPUT_NONE) {
		int big = (int)(17 * ui), pad = (int)(16 * ui), w;
		float x0, y0, bw = 360 * ui;
		if (input_mode == INPUT_GOTO) {
			snprintf(buf, sizeof buf, "Aller \xc3\xa0 la page (1 \xe2\x80\x93 %d) :  %s_", T ? T->npages : 0, input_buf);
		} else {
			char stars[200];
			int k, n = 0;
			for (k = 0; input_buf[k] && n < 60; k++)
				if ((input_buf[k] & 0xc0) != 0x80) stars[n++] = '*';
			stars[n] = 0;
			snprintf(buf, sizeof buf, "Mot de passe :  %s_", stars);
		}
		w = text_width(buf, big, 1);
		if (w + 2 * pad > bw) bw = (float)(w + 2 * pad);
		x0 = floorf((vw - bw) / 2);
		y0 = floorf(vh * 0.25f);
		gfx_rect(x0, y0, x0 + bw, y0 + text_get(buf, big, 1)->h + 2 * pad, 0xf0202226);
		gfx_rect(x0, y0, x0 + bw, y0 + 2 * ui, 0xff4a90d9);
		draw_text(buf, big, 1, x0 + pad, y0 + pad, 0xffffffff);
	}

	if (show_help) draw_help();
}

/* Défilement automatique (clic molette maintenu) : la vue file dans la direction du
   curseur par rapport au point de départ, d'autant plus vite qu'il en est loin. */
static double auto_last;

static int auto_active(void)
{
	return drag == DRAG_AUTO && has_doc() && hypot(mx - mx0, my - my0) > 8 * ui;
}

static void autoscroll_step(void)
{
	double t = platform_time(), dt = t - auto_last, dx = mx - mx0, dy = my - my0, d = hypot(dx, dy), e, v;
	auto_last = t;
	if (!auto_active()) return;
	if (dt > 0.05) dt = 0.05;            /* après une pause : pas de saut */
	e = (d - 8 * ui) / ui;                /* distance utile, en pixels « 96 dpi » */
	v = (7.0 * e + 0.02 * e * e) * ui;    /* pixels par seconde : 50 px -> ~400 px/s, 200 px -> ~2200 px/s */
	T->sx += v * dx / d * dt;
	T->sy += v * dy / d * dt;
	clamp_scroll();
	view_dirty = 1;
}

void core_draw(void)
{
	autoscroll_step();
	if (view_dirty || need_schedule) {
		view_dirty = need_schedule = 0;
		schedule();
	}
	gfx_begin(win_w, win_h, BG_COLOR);
	gfx_origin(0, (float)bar_h);
	if (has_doc()) draw_pages();
	draw_overlays();
	gfx_origin(0, 0);
	draw_tabbar();
	draw_tooltip();
	gfx_end();
	dirty = 0;
}

int core_dirty(void)
{
	return dirty || view_dirty || need_schedule || auto_active();
}

int core_timeout_ms(void)
{
	double t = platform_time();
	int ms = -1;
	if (toast[0] && t < toast_until) ms = (int)((toast_until - t) * 1000) + 20;
	else if (toast[0]) { toast[0] = 0; dirty = 1; }
	if (hover != HIT_NONE && !drag && t - hover_since < 0.5) {
		int h = (int)((0.5 - (t - hover_since)) * 1000) + 20;
		if (ms < 0 || h < ms) ms = h;
	}
	return ms;
}

/* ---------- onglets : ouverture / fermeture / activation ---------- */

static void update_title(void)
{
	char buf[1024];
	if (T && T->name)
		snprintf(buf, sizeof buf, "%s \xe2\x80\x94 Lire", T->name);
	else
		snprintf(buf, sizeof buf, "Lire");
	platform_set_title(buf);
}

static void activate(int idx)
{
	if (idx < 0 || idx >= ntabs) return;
	if (T && tabs[idx] != T) {
		stop_tab_work(T);
		trim_tab(T);
	}
	active = idx;
	T = tabs[idx];
	input_mode = T->need_password ? INPUT_PASSWORD : INPUT_NONE;
	memset(input_buf, 0, sizeof input_buf);
	drag = DRAG_NONE;
	ensure_tab_visible = 1;
	sync_view();
	update_title();
	view_dirty = need_schedule = dirty = 1;
	mutex_lock(&mu);
	cond_broadcast(&cv_load);
	mutex_unlock(&mu);
}

static void remember_closed(const char *path)
{
	int k;
	if (!path) return;
	if (nclosed == MAX_CLOSED) {
		free(closed_paths[0]);
		for (k = 1; k < MAX_CLOSED; k++) closed_paths[k - 1] = closed_paths[k];
		nclosed--;
	}
	closed_paths[nclosed++] = strdup(path);
}

static void close_tab(int idx)
{
	Tab *t;
	int k, was_active = idx == active;
	if (idx < 0 || idx >= ntabs) return;
	t = tabs[idx];
	quiesce();
	if (t->doc && !t->need_password) remember_closed(t->path);
	if (was_active) T = NULL;
	tab_free(t);
	for (k = idx + 1; k < ntabs; k++) tabs[k - 1] = tabs[k];
	ntabs--;
	if (ntabs == 0) {
		active = -1;
		T = NULL;
		input_mode = INPUT_NONE;
		update_title();
	} else if (was_active) {
		active = -1;
		activate(idx < ntabs ? idx : ntabs - 1);
	} else if (idx < active) {
		active--;
	}
	hover = HIT_NONE;
	resume();
	view_dirty = need_schedule = dirty = 1;
}

/* Lit le nombre et la taille des pages, puis choisit la vue initiale (onglet actif). */
static int finish_open(void)
{
	Tab *t = T;
	pdf_document *pdf = NULL;
	int i, tree = 0;
	fz_var(tree);
	fz_var(i);
	fz_var(pdf);
	fz_try(ctx)
		t->npages = fz_count_pages(ctx, t->doc);
	fz_catch(ctx)
		t->npages = 0;
	if (t->npages <= 0) {
		t->npages = 0;
		return -1;
	}
	t->pages = calloc(t->npages, sizeof *t->pages);
	t->lq = calloc(t->npages + 8, sizeof *t->lq);
	t->lq_old = calloc(t->npages + 8, sizeof *t->lq_old);
	pdf = pdf_specifics(ctx, t->doc);
	if (pdf) {
		fz_try(ctx) { pdf_load_page_tree(ctx, pdf); tree = 1; }
		fz_catch(ctx) { tree = 0; }
	}
	t->max_page_dim = 0;
	for (i = 0; i < t->npages; i++) {
		fz_rect r = fz_empty_rect;
		fz_try(ctx) {
			if (pdf) {
				fz_rect mb;
				fz_matrix pctm;
				pdf_obj *po = pdf_lookup_page_obj(ctx, pdf, i);
				pdf_page_obj_transform(ctx, po, &mb, &pctm);
				r = fz_transform_rect(mb, pctm);
			} else {
				fz_page *pg = fz_load_page(ctx, t->doc, i);
				r = fz_bound_page(ctx, pg);
				fz_drop_page(ctx, pg);
			}
		}
		fz_catch(ctx)
			r = fz_empty_rect;
		if (fz_is_empty_rect(r) || r.x1 - r.x0 < 1 || r.y1 - r.y0 < 1)
			r = fz_make_rect(0, 0, 595, 842);
		t->pages[i].bounds = r;
		if (r.x1 - r.x0 > t->max_page_dim) t->max_page_dim = r.x1 - r.x0;
		if (r.y1 - r.y0 > t->max_page_dim) t->max_page_dim = r.y1 - r.y0;
	}
	if (tree) {
		fz_try(ctx) pdf_drop_page_tree(ctx, pdf);
		fz_catch(ctx) {}
	}
	compute_level_limits();
	{
		fz_rect r = rotated_bounds(&t->pages[0], t->rot);
		double w = r.x1 - r.x0, h = r.y1 - r.y0;
		/* plans (grand format ou paysage) : page entière ; documents A4/A3 portrait : pleine largeur */
		int mode = (w > h || h > 1191 * 1.05 || t->npages == 1) ? FIT_PAGE : FIT_WIDTH;
		t->scale = 1;
		t->level = 0;
		layout();
		apply_fit(mode, 0, 0);
	}
	t->synced_vw = vw;
	t->synced_vh = vh;
	t->synced_dpi = dpi;
	update_title();
	view_dirty = need_schedule = dirty = 1;
	return 0;
}

static void open_failed(int idx, const char *what)
{
	char msg[400];
	snprintf(msg, sizeof msg, "%s \xc2\xab %s \xc2\xbb", what, tabs[idx]->name);
	close_tab(idx);
	set_toast(msg);
}

void core_open(const char *path)
{
	Tab *t;
	int i;
	fz_document *d = NULL;
	for (i = 0; i < ntabs; i++)
		if (tabs[i]->path && same_path(tabs[i]->path, path)) {
			activate(i);
			return;
		}
	fz_var(d);
	fz_try(ctx)
		d = fz_open_document(ctx, path);
	fz_catch(ctx) {
		char msg[400];
		snprintf(msg, sizeof msg, "Impossible d'ouvrir \xc2\xab %s \xc2\xbb", base_name(path));
		set_toast(msg);
		return;
	}
	t = calloc(1, sizeof *t);
	t->doc = d;
	t->path = strdup(path);
	t->name = strdup(base_name(path));
	t->scale = 1;
	t->fit_mode = FIT_PAGE;
	t->exact_level = INT_MIN;
	t->label_w = -1;
	if (ntabs == tabs_cap) {
		tabs_cap = tabs_cap ? tabs_cap * 2 : 8;
		tabs = realloc(tabs, tabs_cap * sizeof *tabs);
	}
	tabs[ntabs++] = t;
	activate(ntabs - 1);
	if (fz_needs_password(ctx, d)) {
		t->need_password = 1;
		input_mode = INPUT_PASSWORD;
		input_buf[0] = 0;
		dirty = 1;
		return;
	}
	if (finish_open())
		open_failed(active, "Aucune page lisible dans");
}

void core_reload(void)
{
	Tab *t = T;
	char *path;
	int old_level, old_rot, old_fit, old_n, cur, idx = active;
	double ox, oy;
	fz_document *d = NULL;
	if (!t || !t->path || t->need_password) return;
	old_level = t->level; old_rot = t->rot; old_fit = t->fit_mode; old_n = t->npages;
	cur = current_page();
	ox = t->sx; oy = t->sy;
	path = strdup(t->path);
	quiesce();
	tab_clear(t);
	fz_var(d);
	fz_try(ctx)
		d = fz_open_document(ctx, path);
	fz_catch(ctx)
		d = NULL;
	free(path);
	resume();
	if (!d) {
		open_failed(idx, "Impossible de recharger");
		return;
	}
	t->doc = d;
	if (fz_needs_password(ctx, d)) {
		t->need_password = 1;
		input_mode = INPUT_PASSWORD;
		return;
	}
	t->rot = old_rot;
	if (finish_open()) {
		open_failed(idx, "Aucune page lisible dans");
		return;
	}
	if (t->npages == old_n) {
		if (old_fit != FIT_NONE) {
			apply_fit(old_fit, cur, 0);
		} else {
			t->fit_mode = FIT_NONE;
			set_level(old_level);
			t->sx = ox; t->sy = oy;
			clamp_scroll();
		}
	}
	set_toast("Fichier recharg\xc3\xa9");
	view_dirty = need_schedule = dirty = 1;
}

static void reopen_closed(void)
{
	char *p;
	if (!nclosed) return;
	p = closed_paths[--nclosed];
	core_open(p);
	free(p);
}

static void next_tab(int delta)
{
	if (ntabs > 1) activate(((active + delta) % ntabs + ntabs) % ntabs);
}

/* ---------- saisie ---------- */

static void input_commit(void)
{
	if (input_mode == INPUT_GOTO) {
		int n = atoi(input_buf);
		input_mode = INPUT_NONE;
		if (has_doc() && n >= 1 && n <= T->npages) goto_page(n - 1);
		else if (input_buf[0]) set_toast("Cette page n'existe pas");
	} else if (input_mode == INPUT_PASSWORD && T) {
		if (T->doc && fz_authenticate_password(ctx, T->doc, input_buf)) {
			T->password = strdup(input_buf);   /* pour les instances des autres chargeurs */
			input_mode = INPUT_NONE;
			memset(input_buf, 0, sizeof input_buf);
			T->need_password = 0;
			if (finish_open()) open_failed(active, "Aucune page lisible dans");
		} else {
			set_toast("Mot de passe incorrect");
			input_buf[0] = 0;
		}
	}
	dirty = 1;
}

static void input_cancel(void)
{
	int was_pw = input_mode == INPUT_PASSWORD;
	input_mode = INPUT_NONE;
	memset(input_buf, 0, sizeof input_buf);
	if (was_pw && T && T->need_password) close_tab(active);
	dirty = 1;
}

static void start_goto(const char *initial)
{
	if (!has_doc()) return;
	input_mode = INPUT_GOTO;
	snprintf(input_buf, sizeof input_buf, "%s", initial);
	dirty = 1;
}

static void rotate(int delta)
{
	int cur;
	char msg[64];
	if (!has_doc()) return;
	cur = current_page();
	T->rot = ((T->rot + delta) % 360 + 360) % 360;
	flush_jobs(T);
	compute_level_limits();
	layout();
	if (T->fit_mode != FIT_NONE) apply_fit(T->fit_mode, cur, 0);
	else { set_level(T->level); center_on_page(cur); }
	snprintf(msg, sizeof msg, "Rotation %s : %d\xc2\xb0", delta > 0 ? "horaire" : "antihoraire", T->rot);
	set_toast(msg);
	view_dirty = dirty = 1;
}

void core_set_wheel_zoom(int on) { wheel_zoom = on; }
int core_wheel_zoom(void) { return wheel_zoom; }

static void toggle_wheel(void)
{
	wheel_zoom = !wheel_zoom;
	set_toast(wheel_zoom ? "Molette : zoom (Ctrl+molette : d\xc3\xa9""filement)"
			: "Molette : d\xc3\xa9""filement (Ctrl+molette : zoom)");
	platform_wheel_mode_changed(wheel_zoom);
}

static void set_hover(int x, int y)
{
	int h = drag ? HIT_NONE : bar_hit(x, y);
	if (h != hover) {
		hover = h;
		hover_since = platform_time();
		dirty = 1;
	}
}

void core_mouse_leave(void)
{
	if (hover != HIT_NONE) { hover = HIT_NONE; dirty = 1; }
}

void core_mouse_down(int btn, int x, int y, int mods)
{
	float a, b, c, d, t0, t1;
	(void)mods;
	dirty = 1;
	if (show_help) { show_help = 0; return; }
	if (y < bar_h) {
		int h = bar_hit(x, y);
		hover_since = platform_time() + 3600;   /* pas d'infobulle juste après un clic */
		if (btn == BTN_MIDDLE && h >= HIT_TAB) close_tab((h >= HIT_CLOSE ? h - HIT_CLOSE : h - HIT_TAB));
		else if (btn == BTN_LEFT) {
			if (h >= HIT_CLOSE) close_tab(h - HIT_CLOSE);
			else if (h >= HIT_TAB) activate(h - HIT_TAB);
			else if (h == HIT_PLUS) platform_open_dialog();
			else if (h == HIT_ROT_CCW) rotate(-90);
			else if (h == HIT_ROT_CW) rotate(90);
		}
		return;
	}
	y -= bar_h;
	mx = x; my = y;
	if (input_mode == INPUT_GOTO) input_cancel();
	if (input_mode == INPUT_PASSWORD) return;
	if (!has_doc()) {
		if (btn == BTN_LEFT) platform_open_dialog();
		return;
	}
	if (btn == BTN_LEFT) {
		if (vbar(&a, &b, &c, &d, &t0, &t1) && x >= a && y >= b && y < d) {
			if (y >= t0 && y < t1) { drag = DRAG_VBAR; drag_btn = btn; drag_off = y - t0; }
			else { T->sy += (y < t0 ? -0.9 : 0.9) * vh; clamp_scroll(); view_dirty = 1; }
			return;
		}
		if (hbar(&a, &b, &c, &d, &t0, &t1) && y >= b && x >= a && x < c) {
			if (x >= t0 && x < t1) { drag = DRAG_HBAR; drag_btn = btn; drag_off = x - t0; }
			else { T->sx += (x < t0 ? -0.9 : 0.9) * vw; clamp_scroll(); view_dirty = 1; }
			return;
		}
	}
	if (btn == BTN_LEFT) { drag = DRAG_PAN; drag_btn = btn; mx0 = x; my0 = y; }
	else if (btn == BTN_MIDDLE) { drag = DRAG_AUTO; drag_btn = btn; mx0 = x; my0 = y; auto_last = platform_time(); }
	else if (btn == BTN_RIGHT) { drag = DRAG_ZOOM; drag_btn = btn; mx0 = x; my0 = y; }
}

void core_mouse_move(int x, int y, int mods)
{
	int dx, dy;
	float a, b, c, d, t0, t1;
	(void)mods;
	set_hover(x, y);
	y -= bar_h;
	dx = x - mx; dy = y - my;
	mx = x; my = y;
	if (!has_doc()) return;
	switch (drag) {
	case DRAG_PAN:
		if (dx || dy) {
			T->sx -= dx; T->sy -= dy;
			clamp_scroll();
			view_dirty = dirty = 1;
		}
		break;
	case DRAG_ZOOM:
	case DRAG_AUTO:
		dirty = 1;
		break;
	case DRAG_VBAR:
		if (vbar(&a, &b, &c, &d, &t0, &t1)) {
			double len = d - b, th = t1 - t0;
			T->sy = (y - drag_off - b) / fmax(len - th, 1) * (T->ch - vh);
			clamp_scroll();
			view_dirty = dirty = 1;
		}
		break;
	case DRAG_HBAR:
		if (hbar(&a, &b, &c, &d, &t0, &t1)) {
			double len = c - a, th = t1 - t0;
			T->sx = (x - drag_off - a) / fmax(len - th, 1) * (T->cw - vw);
			clamp_scroll();
			view_dirty = dirty = 1;
		}
		break;
	}
}

void core_mouse_up(int btn, int x, int y, int mods)
{
	(void)mods;
	y -= bar_h;
	mx = x; my = y;
	if (!drag || btn != drag_btn) return;
	if (drag == DRAG_ZOOM && has_doc()) {
		double x0 = mx0 < x ? mx0 : x, x1 = mx0 < x ? x : mx0;
		double y0 = my0 < y ? my0 : y, y1 = my0 < y ? y : my0;
		if (x1 - x0 >= 8 && y1 - y0 >= 8) {
			double f = fmin(vw / (x1 - x0), vh / (y1 - y0));
			double cxs = (x0 + x1) / 2, cys = (y0 + y1) / 2;
			T->fit_mode = FIT_NONE;
			zoom_at(T->level + (int)floor(LPO * log2(f)), cxs, cys);
			T->exact_level = T->level;
			T->sx += cxs - vw / 2.0;
			T->sy += cys - vh / 2.0;
			clamp_scroll();
		}
	}
	drag = DRAG_NONE;
	view_dirty = dirty = 1;
}

void core_wheel(float dy, float dx, int x, int y, int mods)
{
	int zoom = (mods & LMOD_CTRL) ? !wheel_zoom : wheel_zoom;
	dirty = 1;
	if (show_help) { show_help = 0; return; }
	if (y < bar_h) {   /* molette sur la barre : défile les onglets s'ils débordent */
		tab_scroll -= (dy + dx) * 60 * ui;
		return;
	}
	y -= bar_h;
	if (!has_doc() || drag == DRAG_ZOOM) return;
	if (zoom && dy != 0) {
		zoom_step(dy, x, y);
		return;
	}
	if (mods & LMOD_SHIFT) { dx = -dy; dy = 0; }
	T->sy -= dy * 110 * ui;
	T->sx += dx * 110 * ui;
	clamp_scroll();
	view_dirty = 1;
}

void core_key(int key, int mods)
{
	int ctrl = mods & LMOD_CTRL, shift = mods & LMOD_SHIFT, cur;
	dirty = 1;
	/* raccourcis globaux (onglets, fichiers), actifs même pendant une saisie */
	if (ctrl) {
		switch (key) {
		case K_TAB: next_tab(shift ? -1 : 1); return;
		case K_PGDN: next_tab(1); return;
		case K_PGUP: next_tab(-1); return;
		case 'W': case K_F4: if (T) close_tab(active); return;
		case 'T': if (shift) reopen_closed(); else platform_open_dialog(); return;
		case 'O': platform_open_dialog(); return;
		case 'Q': platform_quit(); return;
		}
	}
	if (input_mode != INPUT_NONE) {
		if (key == K_ENTER) input_commit();
		else if (key == K_ESC) input_cancel();
		else if (key == K_BACKSPACE) {
			int n = (int)strlen(input_buf);
			while (n > 0 && (input_buf[n - 1] & 0xc0) == 0x80) n--;
			if (n > 0) n--;
			input_buf[n] = 0;
		}
		return;
	}
	if (show_help && (key == K_ESC || key == K_F1)) { show_help = 0; return; }
	if (ctrl) {
		switch (key) {
		case 'G': start_goto(""); return;
		case '0': apply_fit(FIT_PAGE, current_page(), 0); return;
		case '1':
			if (has_doc()) {
				T->fit_mode = FIT_NONE;
				zoom_at((int)lround(LPO * log2(dpi / 72.0)), vw / 2.0, vh / 2.0);
				T->exact_level = T->level;
			}
			return;
		case '2': apply_fit(FIT_WIDTH, current_page(), 1); return;
		case K_PLUS: if (shift) rotate(90); else zoom_step(1, vw / 2.0, vh / 2.0); return;
		case K_MINUS: if (shift) rotate(-90); else zoom_step(-1, vw / 2.0, vh / 2.0); return;
		case K_HOME: goto_page(0); return;
		case K_END: if (has_doc()) goto_page(T->npages - 1); return;
		}
	}
	switch (key) {
	case K_F1: show_help = !show_help; return;
	case K_F5: core_reload(); return;
	case K_F11: platform_toggle_fullscreen(); return;
	case K_ESC: show_help = 0; toast[0] = 0; return;
	}
	if (!has_doc()) return;
	cur = current_page();
	switch (key) {
	case K_PGUP:
		if (T->pages[cur].ph + 2 * gap > vh && T->sy > T->pages[cur].top - gap + 2) {
			T->sy = T->pages[cur].top - gap;
			clamp_scroll();
			view_dirty = 1;
		} else goto_page(cur - 1);
		break;
	case K_PGDN: goto_page(cur + 1); break;
	case K_HOME: goto_page(0); break;
	case K_END: goto_page(T->npages - 1); break;
	case K_UP: T->sy -= 60 * ui; clamp_scroll(); view_dirty = 1; break;
	case K_DOWN: T->sy += 60 * ui; clamp_scroll(); view_dirty = 1; break;
	case K_LEFT:
		if (T->cw <= vw) goto_page(cur - 1);
		else { T->sx -= 60 * ui; clamp_scroll(); view_dirty = 1; }
		break;
	case K_RIGHT:
		if (T->cw <= vw) goto_page(cur + 1);
		else { T->sx += 60 * ui; clamp_scroll(); view_dirty = 1; }
		break;
	case K_SPACE:
		T->sy += (shift ? -0.9 : 0.9) * vh;
		clamp_scroll();
		view_dirty = 1;
		break;
	case K_PLUS: zoom_step(1, vw / 2.0, vh / 2.0); break;
	case K_MINUS: zoom_step(-1, vw / 2.0, vh / 2.0); break;
	}
}

void core_char(unsigned c)
{
	dirty = 1;
	if (input_mode == INPUT_GOTO) {
		size_t n = strlen(input_buf);
		if (c >= '0' && c <= '9' && n < 9) { input_buf[n] = (char)c; input_buf[n + 1] = 0; }
		return;
	}
	if (input_mode == INPUT_PASSWORD) {
		size_t n = strlen(input_buf);
		if (c >= 32 && n + 4 < sizeof input_buf) {
			char tmp[8];
			int k = fz_runetochar(tmp, (int)c);
			memcpy(input_buf + n, tmp, k);
			input_buf[n + k] = 0;
		}
		return;
	}
	if (show_help && c != 'h' && c != 'H' && c != '?') { show_help = 0; }
	switch (c) {
	case 'h': case 'H': case '?': show_help = !show_help; return;
	case 'm': case 'M': toggle_wheel(); return;
	}
	if (!has_doc()) return;
	switch (c) {
	case '+': case '=': zoom_step(1, vw / 2.0, vh / 2.0); break;
	case '-': case '_': zoom_step(-1, vw / 2.0, vh / 2.0); break;
	case 'f': case 'F': apply_fit(FIT_PAGE, current_page(), 0); break;
	case 'l': case 'L': case 'w': case 'W': apply_fit(FIT_WIDTH, current_page(), 1); break;
	case 'r': rotate(90); break;
	case 'R': rotate(-90); break;
	default:
		if (c >= '0' && c <= '9') {
			char s[2] = { (char)c, 0 };
			start_goto(s);
		}
	}
}

int core_input_mode(void) { return input_mode; }

int core_cursor(void)
{
	if (drag == DRAG_PAN) return CUR_HAND;
	if (drag == DRAG_AUTO) return CUR_AUTO;
	if (drag == DRAG_ZOOM) return CUR_CROSS;
	return CUR_ARROW;
}

/* ---------- taille, DPI ---------- */

void core_resize(int w, int h)
{
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w == win_w && h == win_h) return;
	win_w = w;
	win_h = h;
	vw = w;
	vh = h - bar_h > 1 ? h - bar_h : 1;
	sync_view();
	view_dirty = dirty = 1;
}

void core_set_dpi(float d)
{
	if (d < 48) d = 48;
	dpi = d;
	ui = d / 96.0f;
	gap = (int)lround(10 * ui);
	sbw = (int)lround(12 * ui);
	bar_h = (int)lround(34 * ui);
	vh = win_h - bar_h > 1 ? win_h - bar_h : 1;
	sync_view();
	view_dirty = dirty = 1;
}

/* ---------- informations ---------- */

int core_page_count(void) { return has_doc() ? T->npages : 0; }
int core_current_page(void) { return current_page(); }
double core_zoom_percent(void) { return T ? T->scale / (dpi / 72.0) * 100.0 : 0; }
int core_tab_count(void) { return ntabs; }
int core_active_tab(void) { return active; }

void core_stats(char *buf, int len)
{
	int k, lists = 0;
	if (!T) { snprintf(buf, len, "aucun onglet"); return; }
	for (k = 0; k < T->npages; k++) if (T->pages[k].lstate == L_READY) lists++;
	snprintf(buf, len, "onglet %d/%d jobs=%d tex=%.1fMo (total %.1fMo, budget %.0f) lists=%d (%.1fMo) zoom=%.1f%% niveau=%d rendu=%d page=%d/%d rot=%d",
			active + 1, ntabs, T->njobs, T->tex_bytes / 1048576.0, tex_total / 1048576.0, tex_budget / 1048576.0, lists,
			T->list_bytes / 1048576.0, core_zoom_percent(), T->level, render_level(T, T->level), current_page() + 1, T->npages, T->rot);
}

/* ---------- initialisation ---------- */

int core_init(int nthreads)
{
	int i;
	for (i = 0; i < FZ_LOCK_MAX; i++) mutex_init(&fzmu[i]);
	mutex_init(&mu);
	cond_init(&cv_work);
	cond_init(&cv_load);
	cond_init(&cv_idle);
	fzlocks.user = NULL;
	fzlocks.lock = lock_fz;
	fzlocks.unlock = unlock_fz;
	ctx = fz_new_context(NULL, &fzlocks, (size_t)512 << 20);
	if (!ctx) return -1;
	fz_register_document_handlers(ctx);
	/* Gestion des couleurs ICC désactivée : rendu nettement plus rapide,
	   différence de teinte imperceptible sur des plans. */
	fz_disable_icc(ctx);
	/* Lissage des traits sur 64 niveaux (au lieu de 256) : visuellement identique
	   sur des plans, ~1,6x plus rapide. Le texte garde le lissage maximal. */
	fz_set_graphics_aa_level(ctx, 6);
	fz_set_text_aa_level(ctx, 8);
	fz_try(ctx) {
		font_regular = fz_new_base14_font(ctx, "Helvetica");
		font_bold = fz_new_base14_font(ctx, "Helvetica-Bold");
	}
	fz_catch(ctx) return -1;

	{
		/* Budgets mémoire proportionnels à la RAM : plus il y en a, plus on garde de
		   tuiles calculées (zoom/déplacement instantanés) et de pages analysées.
		   8 Go -> 0,8 Go de tuiles et 1 Go de pages ; 32 Go -> 1,5 Go et 3 Go. */
		unsigned long long ram = ram_bytes();
		const char *e = getenv("LIRE_TEX_MB");
		tex_budget = (size_t)fmin(fmax(ram / 10.0, 256.0 * (1 << 20)), 1536.0 * (1 << 20));
		list_budget = (size_t)fmin(fmax(ram / 8.0, 256.0 * (1 << 20)), 3072.0 * (1 << 20));
		if (e && atoi(e) > 0) tex_budget = (size_t)atoi(e) << 20;
		if ((e = getenv("LIRE_LIST_MB")) && atoi(e) > 0) list_budget = (size_t)atoi(e) << 20;
	}
	if (nthreads <= 0) nthreads = cpu_count() - 1;
	nworkers = clampi(nthreads, 1, MAX_WORKERS);
	nloaders = clampi(cpu_count() / 4, 1, MAX_LOADERS);
	if (getenv("LIRE_LOADERS")) nloaders = clampi(atoi(getenv("LIRE_LOADERS")), 1, MAX_LOADERS);
	for (i = 0; i < nloaders; i++) {
		lctx[i] = fz_clone_context(ctx);
		loader_page[i] = -1;
	}
	for (i = 0; i < nworkers; i++) {
		wctx[i] = fz_clone_context(ctx);
		fz_set_graphics_aa_level(wctx[i], 6);
		fz_set_text_aa_level(wctx[i], 8);
	}
	for (i = 0; i < nloaders; i++)
		thread_create(&th_loader[i], loader_main, (void *)(intptr_t)i, 1, 16 << 20);
	for (i = 0; i < nworkers; i++)
		thread_create(&th_workers[i], worker_main, (void *)(intptr_t)i, 1, 4 << 20);
	update_title();
	return 0;
}

/* Sortie rapide : interrompt les rendus et attend que les threads soient au repos
   (aucun verrou tenu) ; la libération mémoire est laissée au système. */
void core_quick_exit(void)
{
	quiesce();
}

void core_shutdown(void)
{
	int i;
	quiesce();
	T = NULL;
	for (i = 0; i < ntabs; i++) tab_free(tabs[i]);
	ntabs = 0;
	active = -1;
	free(tabs);
	tabs = NULL;
	mutex_lock(&mu);
	quit = 1;
	cond_broadcast(&cv_work);
	cond_broadcast(&cv_load);
	mutex_unlock(&mu);
	for (i = 0; i < nloaders; i++) thread_join(th_loader[i]);
	for (i = 0; i < nworkers; i++) thread_join(th_workers[i]);
	for (i = 0; i < nworkers; i++) fz_drop_context(wctx[i]);
	for (i = 0; i < nloaders; i++) fz_drop_context(lctx[i]);
	for (i = 0; i < NTEXT; i++)
		if (texts[i].tex) gfx_tex_free(texts[i].tex);
	if (icon_rot[0]) gfx_tex_free(icon_rot[0]);
	if (icon_rot[1]) gfx_tex_free(icon_rot[1]);
	if (icon_auto) gfx_tex_free(icon_auto);
	for (i = 0; i < nclosed; i++) free(closed_paths[i]);
	fz_drop_font(ctx, font_regular);
	fz_drop_font(ctx, font_bold);
	free(wq.v);
	free(wq_next.v);
	fz_drop_context(ctx);
}
