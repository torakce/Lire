/* bench2.c — décompose le coût d'affichage d'une page lourde :
 *   analyse (display list), parcours seul, rendu des tuiles d'une vue à plusieurs zooms,
 *   et passage à l'échelle sur plusieurs threads (verrous MuPDF réels). */
#include <mupdf/fitz.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TILE 512
static int VW = 2560, VH = 1440;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

#include <sched.h>
#include <immintrin.h>
static pthread_mutex_t locks[FZ_LOCK_MAX];
static struct { volatile int v; char pad[60]; } spins[FZ_LOCK_MAX];
static int use_spin;
static void lk(void *u, int l)
{
	(void)u;
	if (!use_spin) { pthread_mutex_lock(&locks[l]); return; }
	for (;;) {
		int n = 0;
		if (!__atomic_exchange_n(&spins[l].v, 1, __ATOMIC_ACQUIRE)) return;
		while (__atomic_load_n(&spins[l].v, __ATOMIC_RELAXED)) {
			if (++n < 100) _mm_pause(); else { sched_yield(); n = 0; }
		}
	}
}
static void ulk(void *u, int l) { (void)u; if (!use_spin) pthread_mutex_unlock(&locks[l]); else __atomic_store_n(&spins[l].v, 0, __ATOMIC_RELEASE); }

typedef struct { fz_irect r; } T;
static T *tiles;
static int ntiles, next_tile;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static fz_display_list *LIST;
static fz_matrix CTM;
static int null_dev;

static void render(fz_context *c, fz_irect r)
{
	fz_device *dev;
	fz_pixmap *pix = NULL;
	if (null_dev) {
		dev = fz_new_device_of_size(c, sizeof(fz_device));
	} else {
		pix = fz_new_pixmap_with_bbox(c, fz_device_bgr(c), r, NULL, 1);
		fz_clear_pixmap_with_value(c, pix, 255);
		dev = fz_new_draw_device(c, fz_identity, pix);
	}
	fz_run_display_list(c, LIST, dev, CTM, fz_rect_from_irect(r), NULL);
	fz_close_device(c, dev);
	fz_drop_device(c, dev);
	fz_drop_pixmap(c, pix);
}

static void *worker(void *arg)
{
	fz_context *c = arg;
	for (;;) {
		int k;
		pthread_mutex_lock(&qmu);
		k = next_tile++;
		pthread_mutex_unlock(&qmu);
		if (k >= ntiles) break;
		render(c, tiles[k].r);
	}
	return NULL;
}

/* tuiles couvrant une vue VWxVH centrée sur (cx, cy) (fraction de la page) au zoom s */
static void make_view(fz_rect b, float s, float cx, float cy)
{
	fz_irect pb = fz_round_rect(fz_transform_rect(b, fz_scale(s, s)));
	int vx0 = (int)(pb.x0 + (pb.x1 - pb.x0) * cx - VW / 2), vy0 = (int)(pb.y0 + (pb.y1 - pb.y0) * cy - VH / 2);
	int vx1 = vx0 + VW, vy1 = vy0 + VH, x, y;
	if (vx0 < pb.x0) vx0 = pb.x0;
	if (vy0 < pb.y0) vy0 = pb.y0;
	if (vx1 > pb.x1) vx1 = pb.x1;
	if (vy1 > pb.y1) vy1 = pb.y1;
	ntiles = 0;
	free(tiles);
	tiles = malloc(sizeof *tiles * 4096);
	for (y = (vy0 - pb.y0) / TILE; pb.y0 + y * TILE < vy1; y++)
		for (x = (vx0 - pb.x0) / TILE; pb.x0 + x * TILE < vx1; x++) {
			fz_irect r = { pb.x0 + x * TILE, pb.y0 + y * TILE, 0, 0 };
			r.x1 = r.x0 + TILE < pb.x1 ? r.x0 + TILE : pb.x1;
			r.y1 = r.y0 + TILE < pb.y1 ? r.y0 + TILE : pb.y1;
			tiles[ntiles++].r = r;
		}
	CTM = fz_scale(s, s);
}

static double run_view(fz_context **ctxs, int nth)
{
	pthread_t th[64];
	double t0 = now();
	int i;
	next_tile = 0;
	for (i = 0; i < nth; i++) pthread_create(&th[i], NULL, worker, ctxs[i]);
	for (i = 0; i < nth; i++) pthread_join(th[i], NULL);
	return (now() - t0) * 1000;
}

int main(int argc, char **argv)
{
	fz_locks_context lc = { NULL, lk, ulk };
	fz_context *ctx, *ctxs[64];
	fz_document *doc;
	fz_page *page;
	fz_rect b;
	fz_device *dev;
	double t0;
	int i, nth = argc > 2 ? atoi(argv[2]) : 2, pg = argc > 3 ? atoi(argv[3]) : 0;
	float sfit, mult[] = { 1, 2, 4, 8, 16, 32 };
	for (i = 0; i < FZ_LOCK_MAX; i++) pthread_mutex_init(&locks[i], NULL);
	use_spin = getenv("SPIN") != NULL;
	ctx = fz_new_context(NULL, &lc, (size_t)512 << 20);
	fz_register_document_handlers(ctx);
	fz_disable_icc(ctx);
	fz_set_graphics_aa_level(ctx, 6);
	fz_set_text_aa_level(ctx, 8);
	for (i = 0; i < nth; i++) ctxs[i] = fz_clone_context(ctx);

	t0 = now();
	doc = fz_open_document(ctx, argv[1]);
	printf("ouverture : %.1f ms, %d pages\n", (now() - t0) * 1000, fz_count_pages(ctx, doc));
	t0 = now();
	page = fz_load_page(ctx, doc, pg);
	b = fz_bound_page(ctx, page);
	printf("chargement page : %.1f ms  (%.0f x %.0f pt)\n", (now() - t0) * 1000, b.x1 - b.x0, b.y1 - b.y0);
	t0 = now();
	LIST = fz_new_display_list(ctx, b);
	dev = fz_new_list_device(ctx, LIST);
	fz_run_page(ctx, page, dev, fz_identity, NULL);
	fz_close_device(ctx, dev);
	fz_drop_device(ctx, dev);
	printf("analyse -> display list : %.1f ms\n", (now() - t0) * 1000);

	sfit = fminf((VW - 20) / (b.x1 - b.x0), (VH - 20) / (b.y1 - b.y0));
	printf("vue %dx%d, zoom page entière s=%.3f\n", VW, VH, sfit);
	for (i = 0; i < (int)(sizeof mult / sizeof *mult); i++) {
		double tn, t1, tN;
		make_view(b, sfit * mult[i], 0.5f, 0.5f);
		null_dev = 1;
		tn = run_view(ctxs, 1);
		null_dev = 0;
		t1 = run_view(ctxs, 1);
		tN = nth > 1 ? run_view(ctxs, nth) : t1;
		if (getenv("REPEAT")) { double a = run_view(ctxs, 1), b2 = run_view(ctxs, nth); printf("   (repeat: 1 thread %.1f ms, %d threads %.1f ms)\n", a, nth, b2); }
		printf("x%-3g : %3d tuiles  parcours seul %7.1f ms (%.1f/tuile)  rendu 1 thread %7.1f ms  %d threads %7.1f ms (x%.2f)\n",
				mult[i], ntiles, tn, tn / ntiles, t1, nth, tN, t1 / tN);
	}
	return 0;
}
