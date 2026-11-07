/* bench.c — mesure les coûts élémentaires MuPDF sur une page : analyse, rendu complet, rendu d'une tuile */
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static double tile(fz_context *ctx, fz_display_list *list, float s, int x, int y, int w, int h)
{
	fz_irect r = { x, y, x + w, y + h };
	double t0 = now();
	fz_pixmap *pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), r, NULL, 1);
	fz_clear_pixmap_with_value(ctx, pix, 255);
	fz_device *dev = fz_new_draw_device(ctx, fz_identity, pix);
	fz_run_display_list(ctx, list, dev, fz_scale(s, s), fz_rect_from_irect(r), NULL);
	fz_close_device(ctx, dev);
	fz_drop_device(ctx, dev);
	fz_drop_pixmap(ctx, pix);
	return (now() - t0) * 1000;
}

int main(int argc, char **argv)
{
	fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	fz_register_document_handlers(ctx);
	if (argc > 2) fz_disable_icc(ctx);
	fz_document *doc = fz_open_document(ctx, argv[1]);
	double t0 = now();
	fz_page *page = fz_load_page(ctx, doc, 0);
	fz_rect b = fz_bound_page(ctx, page);
	fz_display_list *list = fz_new_display_list(ctx, b);
	fz_device *dev = fz_new_list_device(ctx, list);
	fz_run_page(ctx, page, dev, fz_identity, NULL);
	fz_close_device(ctx, dev);
	fz_drop_device(ctx, dev);
	printf("analyse -> display list : %.1f ms\n", (now() - t0) * 1000);

	t0 = now();
	{
		fz_device *bb = fz_new_bbox_device(ctx, &b);
		fz_run_display_list(ctx, list, bb, fz_identity, fz_infinite_rect, NULL);
		fz_close_device(ctx, bb);
		fz_drop_device(ctx, bb);
	}
	printf("parcours seul (bbox device) : %.1f ms\n", (now() - t0) * 1000);

	float sfit = 1600.0f / (b.x1 - b.x0);
	for (int aa = 8; aa >= 0; aa -= 2) { fz_set_aa_level(ctx, aa); printf("aa=%d page complète : %.1f ms   tuile s=1 : %.1f ms\n", aa, tile(ctx, list, sfit, 0, 0, (int)((b.x1 - b.x0) * sfit), (int)((b.y1 - b.y0) * sfit)), tile(ctx, list, 1, (int)(b.x1 / 2), (int)(b.y1 / 2), 512, 512)); }
	fz_set_aa_level(ctx, 8);
	printf("page complète %.0fx%.0f px (s=%.3f) : %.1f ms\n", (b.x1 - b.x0) * sfit, (b.y1 - b.y0) * sfit, sfit,
			tile(ctx, list, sfit, 0, 0, (int)((b.x1 - b.x0) * sfit), (int)((b.y1 - b.y0) * sfit)));
	printf("tuile 512 au centre, s=1   : %.1f ms\n", tile(ctx, list, 1, (int)(b.x1 / 2), (int)(b.y1 / 2), 512, 512));
	printf("tuile 512 au centre, s=1   : %.1f ms (2e fois)\n", tile(ctx, list, 1, (int)(b.x1 / 2), (int)(b.y1 / 2), 512, 512));
	printf("tuile 512 au centre, s=8   : %.1f ms\n", tile(ctx, list, 8, (int)(b.x1 * 4), (int)(b.y1 * 4), 512, 512));
	printf("tuile 512 hors page, s=1   : %.1f ms\n", tile(ctx, list, 1, -5000, -5000, 512, 512));
	return 0;
}
