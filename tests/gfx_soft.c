/* gfx_soft.c — implémentation logicielle de gfx.h pour le banc de test (sans carte graphique) */
#include "../src/gfx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int w, h; unsigned char *px; } STex;

static unsigned char *fb;
static int fbw, fbh;
static float ox, oy;

void gfx_origin(float x, float y) { ox = x; oy = y; }
int soft_enabled = 1;
long soft_calls, soft_tex_bytes;

int gfx_init(void) { return 0; }
int gfx_max_texture(void) { return 16384; }

void *gfx_tex_create(int w, int h, const unsigned char *bgra, int stride)
{
	STex *t = malloc(sizeof *t);
	int y;
	t->w = w; t->h = h;
	t->px = malloc((size_t)w * h * 4);
	for (y = 0; y < h; y++) memcpy(t->px + (size_t)y * w * 4, bgra + (size_t)y * stride, (size_t)w * 4);
	soft_tex_bytes += (long)w * h * 4;
	return t;
}

void gfx_tex_free(void *p)
{
	STex *t = p;
	if (!t) return;
	soft_tex_bytes -= (long)t->w * t->h * 4;
	free(t->px);
	free(t);
}

void gfx_begin(int w, int h, unsigned rgb)
{
	int i;
	soft_calls++;
	if (!soft_enabled) return;
	if (w != fbw || h != fbh) {
		fb = realloc(fb, (size_t)w * h * 4);
		fbw = w; fbh = h;
	}
	for (i = 0; i < w * h; i++) {
		fb[i * 4 + 0] = rgb & 255;
		fb[i * 4 + 1] = (rgb >> 8) & 255;
		fb[i * 4 + 2] = (rgb >> 16) & 255;
		fb[i * 4 + 3] = 255;
	}
}

static void blend_px(unsigned char *d, int b, int g, int r, int a)
{
	/* source prémultipliée */
	d[0] = (unsigned char)(b + d[0] * (255 - a) / 255);
	d[1] = (unsigned char)(g + d[1] * (255 - a) / 255);
	d[2] = (unsigned char)(r + d[2] * (255 - a) / 255);
}

void gfx_image(void *p, float sx0, float sy0, float sx1, float sy1,
		float x0, float y0, float x1, float y1, unsigned tint)
{
	STex *t = p;
	int X0, Y0, X1, Y1, x, y;
	float kx, ky;
	soft_calls++;
	if (!soft_enabled || !t || x1 <= x0 || y1 <= y0) return;
	x0 += ox; x1 += ox; y0 += oy; y1 += oy;
	X0 = (int)floorf(x0 + 0.5f); X1 = (int)floorf(x1 + 0.5f);
	Y0 = (int)floorf(y0 + 0.5f); Y1 = (int)floorf(y1 + 0.5f);
	kx = (sx1 - sx0) / (x1 - x0);
	ky = (sy1 - sy0) / (y1 - y0);
	if (X0 < 0) X0 = 0;
	if (Y0 < 0) Y0 = 0;
	if (X1 > fbw) X1 = fbw;
	if (Y1 > fbh) Y1 = fbh;
	for (y = Y0; y < Y1; y++) {
		int ty = (int)(sy0 + (y + 0.5f - y0) * ky);
		if (ty < 0) ty = 0;
		if (ty >= t->h) ty = t->h - 1;
		for (x = X0; x < X1; x++) {
			int tx = (int)(sx0 + (x + 0.5f - x0) * kx);
			unsigned char *s, *d = fb + ((size_t)y * fbw + x) * 4;
			if (tx < 0) tx = 0;
			if (tx >= t->w) tx = t->w - 1;
			s = t->px + ((size_t)ty * t->w + tx) * 4;
			if (!tint) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
			else {
				int ta = tint >> 24, tr = (tint >> 16) & 255, tg = (tint >> 8) & 255, tb = tint & 255;
				int a = s[3] * ta / 255;
				blend_px(d, s[0] * tb / 255 * ta / 255, s[1] * tg / 255 * ta / 255, s[2] * tr / 255 * ta / 255, a);
			}
		}
	}
}

void gfx_rect(float x0, float y0, float x1, float y1, unsigned argb)
{
	int X0, Y0, X1, Y1, x, y, a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
	soft_calls++;
	if (!soft_enabled) return;
	x0 += ox; x1 += ox; y0 += oy; y1 += oy;
	X0 = (int)floorf(x0 + 0.5f); X1 = (int)floorf(x1 + 0.5f);
	Y0 = (int)floorf(y0 + 0.5f); Y1 = (int)floorf(y1 + 0.5f);
	if (X0 < 0) X0 = 0;
	if (Y0 < 0) Y0 = 0;
	if (X1 > fbw) X1 = fbw;
	if (Y1 > fbh) Y1 = fbh;
	for (y = Y0; y < Y1; y++)
		for (x = X0; x < X1; x++)
			blend_px(fb + ((size_t)y * fbw + x) * 4, b * a / 255, g * a / 255, r * a / 255, a);
}

void gfx_end(void) {}

unsigned char *soft_framebuffer(int *w, int *h)
{
	*w = fbw;
	*h = fbh;
	return fb;
}
