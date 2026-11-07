/* harness.c — banc de test sans fenêtre : pilote le moteur, mesure les temps, enregistre des captures */
#include "../src/core.h"
#include "../src/gfx.h"
#include <mupdf/fitz.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int soft_enabled;
extern long soft_tex_bytes;
unsigned char *soft_framebuffer(int *w, int *h);

static pthread_mutex_t wm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wc = PTHREAD_COND_INITIALIZER;
static int woken;
static const char *outdir = ".";
static int W = 1600, H = 1000;

void platform_wake(void)
{
	pthread_mutex_lock(&wm);
	woken = 1;
	pthread_cond_signal(&wc);
	pthread_mutex_unlock(&wm);
}
void platform_set_title(const char *s) { printf("  [titre] %s\n", s); }
void platform_open_dialog(void) {}
void platform_toggle_fullscreen(void) {}
void platform_quit(void) {}
void platform_wheel_mode_changed(int z) { (void)z; }
double platform_time(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void wait_wake(double timeout)
{
	struct timespec ts;
	pthread_mutex_lock(&wm);
	if (!woken) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)(timeout * 1e9);
		ts.tv_sec += ts.tv_nsec / 1000000000;
		ts.tv_nsec %= 1000000000;
		pthread_cond_timedwait(&wc, &wm, &ts);
	}
	woken = 0;
	pthread_mutex_unlock(&wm);
}

static void save(const char *name)
{
	int w, h;
	unsigned char *fb;
	char path[1024];
	fz_context *c = fz_new_context(NULL, NULL, 0);
	soft_enabled = 1;
	core_draw();
	soft_enabled = 0;
	fb = soft_framebuffer(&w, &h);
	snprintf(path, sizeof path, "%s/%s", outdir, name);
	fz_try(c) {
		fz_pixmap *pix = fz_new_pixmap(c, fz_device_rgb(c), w, h, NULL, 0);
		unsigned char *d = fz_pixmap_samples(c, pix);
		int k;
		for (k = 0; k < w * h; k++) {
			d[k * 3 + 0] = fb[k * 4 + 2];
			d[k * 3 + 1] = fb[k * 4 + 1];
			d[k * 3 + 2] = fb[k * 4 + 0];
		}
		fz_save_pixmap_as_png(c, pix, path);
		fz_drop_pixmap(c, pix);
	}
	fz_catch(c) fprintf(stderr, "png: %s\n", fz_caught_message(c));
	fz_drop_context(c);
}

/* Fait tourner la boucle jusqu'à ce que la vue soit entièrement nette. */
static double pump(const char *label, double timeout)
{
	double t0 = platform_time(), t, tv = -1;
	int frames = 0;
	char st[256];
	for (;;) {
		core_poll();
		core_draw();
		frames++;
		if (tv < 0 && core_view_complete()) tv = (platform_time() - t0) * 1000;
		if (!core_busy()) break;
		wait_wake(0.02);
		if (platform_time() - t0 > timeout) { printf("  !! délai dépassé\n"); break; }
	}
	t = (platform_time() - t0) * 1000;
	core_stats(st, sizeof st);
	printf("%-40s net: %8.1f ms  repos: %8.1f ms  %s\n", label, tv, t, st);
	return tv;
}

/* mesure le coût d'une image pendant une interaction (sans attendre les rendus) */
static double frame_cost(void)
{
	double t0 = platform_time();
	core_poll();
	core_draw();
	return (platform_time() - t0) * 1000;
}

int main(int argc, char **argv)
{
	const char *file;
	int threads = 0, i, n;
	double t0, worst = 0;
	char name[64];
	if (argc < 2) { fprintf(stderr, "usage: harness fichier.pdf [dossier_sortie] [threads] [scenario]\n"); return 1; }
	file = argv[1];
	if (argc > 2) outdir = argv[2];
	if (argc > 3) threads = atoi(argv[3]);
	soft_enabled = 0;
	if (core_init(threads)) return 1;
	gfx_init();
	core_set_dpi(96);
	core_resize(W, H);

	printf("== %s\n", file);
	if (argc > 4 && !strcmp(argv[4], "stress")) {
		/* actions aléatoires enchaînées sans attendre la fin des rendus */
		unsigned seed = 12345;
		int nf = argc - 5, maxtabs = 0;
		core_open(file);
		for (i = 0; i < 4000; i++) {
			int a, x, y;
			seed = seed * 1103515245 + 12345; a = (seed >> 16) % (nf > 0 ? 22 : 16);
			seed = seed * 1103515245 + 12345; x = (seed >> 16) % W;
			seed = seed * 1103515245 + 12345; y = (seed >> 16) % H;
			switch (a) {
			case 0: case 1: case 2: core_wheel(1, 0, x, y, LMOD_CTRL); break;
			case 3: case 4: core_wheel(-1, 0, x, y, LMOD_CTRL); break;
			case 5: core_wheel(-2, 0, x, y, 0); break;
			case 6: core_mouse_down(BTN_LEFT, x, y, 0); core_mouse_move(x - 200, y - 90, 0); core_mouse_up(BTN_LEFT, x - 200, y - 90, 0); break;
			case 7: core_key(K_PGDN, 0); break;
			case 8: core_key(K_PGUP, 0); break;
			case 9: core_char('f'); break;
			case 10: core_char('l'); break;
			case 11: if (i % 7 == 0) core_char('r'); break;
			case 12: core_mouse_down(BTN_RIGHT, x, y, 0); core_mouse_up(BTN_RIGHT, x + 120, y + 80, 0); break;
			case 13: core_key(K_END, 0); break;
			case 14: if (i % 97 == 0) core_reload(); break;
			case 15: core_resize(W - (x % 400), H - (y % 300)); break;
			/* onglets */
			case 16: core_open(argv[5 + (x % nf)]); break;
			case 17: if (core_tab_count() > 1 || (i % 5 == 0)) core_key('W', LMOD_CTRL); break;
			case 18: core_key(K_TAB, LMOD_CTRL | ((y & 1) ? LMOD_SHIFT : 0)); break;
			case 19: core_mouse_move(x, 15, 0); core_mouse_down(BTN_LEFT, x, 15, 0); core_mouse_up(BTN_LEFT, x, 15, 0); break;
			case 20: core_key('T', LMOD_CTRL | LMOD_SHIFT); break;
			case 21: core_mouse_down(BTN_MIDDLE, x, 15, 0); core_mouse_up(BTN_MIDDLE, x, 15, 0); break;
			}
			core_poll();
			core_draw();
			if (core_tab_count() > maxtabs) maxtabs = core_tab_count();
			if (i % 50 == 0) wait_wake(0.01);
		}
		core_resize(W, H);
		printf("onglets ouverts à la fin : %d (max %d)\n", core_tab_count(), maxtabs);
		pump("stress : retour au repos", 300);
		save("stress.png");
		core_shutdown();
		printf("stress OK, textures restantes : %ld octets\n", soft_tex_bytes);
		return 0;
	}
	if (argc > 4 && !strcmp(argv[4], "tabs")) {
		/* scénario onglets : harness f1 sortie threads tabs f2 f3 ... */
		char st[256];
		int k, bw = 34, cwx = W - 6 - bw / 2, ccwx = W - 6 - bw - bw / 2;
		t0 = platform_time();
		core_open(file);
		pump("onglet 1 : premier affichage", 120);
		for (k = 5; k < argc; k++) {
			core_open(argv[k]);
			snprintf(name, sizeof name, "onglet %d : premier affichage", core_tab_count());
			pump(name, 120);
		}
		save("t01_onglets.png");
		/* zoom dans l'onglet actif, puis aller-retour */
		for (i = 0; i < 6; i++) core_wheel(1, 0, W / 2, H / 2, LMOD_CTRL);
		pump("dernier onglet : zoom x4", 120);
		printf("  zoom avant aller-retour : %.1f %%\n", core_zoom_percent());
		core_key(K_TAB, LMOD_CTRL);
		pump("Ctrl+Tab -> onglet 1 (retour)", 60);
		core_key(K_TAB, LMOD_CTRL | LMOD_SHIFT);
		pump("Ctrl+Maj+Tab -> dernier (retour)", 60);
		printf("  zoom après aller-retour : %.1f %%\n", core_zoom_percent());
		/* boutons de rotation de la barre */
		core_mouse_move(cwx, 17, 0);
		core_mouse_down(BTN_LEFT, cwx, 17, 0); core_mouse_up(BTN_LEFT, cwx, 17, 0);
		pump("bouton rotation horaire", 120);
		core_stats(st, sizeof st); printf("  %s\n", st);
		save("t02_rotation_horaire.png");
		core_mouse_move(ccwx, 17, 0);
		core_mouse_down(BTN_LEFT, ccwx, 17, 0); core_mouse_up(BTN_LEFT, ccwx, 17, 0);
		core_mouse_down(BTN_LEFT, ccwx, 17, 0); core_mouse_up(BTN_LEFT, ccwx, 17, 0);
		pump("2 x bouton rotation antihoraire", 120);
		core_stats(st, sizeof st); printf("  %s\n", st);
		/* infobulle */
		core_mouse_move(cwx, 16, 0);
		core_mouse_move(cwx, 17, 0);
		{ double tw = platform_time(); while (platform_time() - tw < 0.6) { core_poll(); core_draw(); wait_wake(0.05); } }
		save("t03_infobulle.png");
		core_mouse_leave();
		/* fermeture / réouverture */
		core_key('W', LMOD_CTRL);
		pump("Ctrl+W (fermer l'onglet)", 60);
		core_stats(st, sizeof st); printf("  %s\n", st);
		core_key('T', LMOD_CTRL | LMOD_SHIFT);
		pump("Ctrl+Maj+T (rouvrir)", 120);
		core_stats(st, sizeof st); printf("  %s\n", st);
		/* ouvrir un fichier déjà ouvert -> active l'onglet existant */
		core_open(file);
		printf("  réouverture du 1er fichier : onglet actif %d / %d\n", core_active_tab() + 1, core_tab_count());
		/* clic sur un onglet */
		core_mouse_down(BTN_LEFT, 6 + 100, 20, 0); core_mouse_up(BTN_LEFT, 6 + 100, 20, 0);
		printf("  clic sur le 1er onglet : actif %d\n", core_active_tab() + 1);
		save("t04_final.png");
		while (core_tab_count() > 0) core_key('W', LMOD_CTRL);
		pump("tous les onglets fermés", 30);
		save("t05_vide.png");
		core_shutdown();
		printf("textures restantes : %ld octets\n", soft_tex_bytes);
		return 0;
	}
	t0 = platform_time();
	core_open(file);
	printf("%-44s %9.1f ms  (%d pages)\n", "ouverture (fz_open + dimensions des pages)", (platform_time() - t0) * 1000, core_page_count());
	pump("premier affichage complet", 120);
	save("01_initial.png");

	/* zoom molette : 7 crans rapides au même endroit (x5) */
	for (i = 0; i < 7; i++) {
		core_wheel(1, 0, W / 3, H / 3, LMOD_CTRL);
		double c = frame_cost();
		if (c > worst) worst = c;
	}
	printf("%-44s %9.2f ms\n", "  coût max d'une image pendant le zoom", worst);
	save("02_zoom_immediat.png");
	pump("zoom x5 : vue entièrement nette", 120);
	save("03_zoom_net.png");

	/* zoom très profond */
	for (i = 0; i < 9; i++) core_wheel(1, 0, W / 2, H / 2, LMOD_CTRL);
	frame_cost();
	pump("zoom x40 : vue entièrement nette", 120);
	save("04_zoom_profond.png");

	/* déplacement (glisser) */
	worst = 0;
	core_mouse_down(BTN_LEFT, 800, 500, 0);
	for (i = 1; i <= 30; i++) {
		core_mouse_move(800 - i * 25, 500 - i * 10, 0);
		double c = frame_cost();
		if (c > worst) worst = c;
	}
	core_mouse_up(BTN_LEFT, 800 - 750, 500 - 300, 0);
	printf("%-44s %9.2f ms\n", "  coût max d'une image pendant le glisser", worst);
	pump("déplacement : vue entièrement nette", 120);
	save("05_deplacement.png");

	/* retour page entière */
	core_char('f');
	frame_cost();
	pump("retour page entière", 120);

	/* navigation entre pages */
	n = core_page_count();
	if (n > 1) {
		double tot = 0, mx = 0;
		int steps = n < 12 ? n - 1 : 12;
		for (i = 0; i < steps; i++) {
			double t;
			core_key(K_PGDN, 0);
			snprintf(name, sizeof name, "page suivante -> %d", core_current_page() + 1);
			t = pump(name, 120);
			tot += t;
			if (t > mx) mx = t;
		}
		printf("%-44s %9.1f ms (max %.1f)\n", "  moyenne page suivante (net)", tot / steps, mx);
		save("06_page_suivante.png");
		core_key(K_PGDN, 0);
		core_key(K_PGUP, 0);
		pump("page précédente (déjà en cache)", 60);
		core_key(K_END, 0);
		pump("dernière page", 120);
		save("07_derniere.png");
	}
	core_char('r');
	pump("rotation 90°", 120);
	save("08_rotation.png");
	core_key(K_F1, 0);
	save("09_aide.png");
	core_key(K_ESC, 0);
	core_char('5');
	save("10_aller_page.png");
	core_key(K_ESC, 0);

	core_shutdown();
	printf("textures restantes : %ld octets\n", soft_tex_bytes);
	return 0;
}
