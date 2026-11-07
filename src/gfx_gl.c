/* gfx_gl.c — rendu par OpenGL 1.1 (disponible sur tout Windows, accéléré par la carte graphique) */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

typedef struct { GLuint id; int w, h, tw, th; } GTex;

static int npot, edge_clamp;
static GLint maxtex = 1024;
static int cur_blend = -1;
static GLuint cur_tex;
static int tex_enabled = 1;

int gfx_init(void)
{
	const char *ver = (const char *)glGetString(GL_VERSION);
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	int major = ver ? atoi(ver) : 1, minor = 0;
	if (ver) {
		const char *dot = strchr(ver, '.');
		if (dot) minor = atoi(dot + 1);
	}
	npot = major >= 2 || (ext && strstr(ext, "GL_ARB_texture_non_power_of_two"));
	edge_clamp = major >= 2 || (major == 1 && minor >= 2) || (ext && strstr(ext, "GL_EXT_texture_edge_clamp"));
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
	if (maxtex < 64) maxtex = 64;
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_DITHER);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	return 0;
}

int gfx_max_texture(void) { return maxtex; }

static int pot(int v)
{
	int p = 1;
	while (p < v) p <<= 1;
	return p;
}

void *gfx_tex_create(int w, int h, const unsigned char *bgra, int stride)
{
	GTex *t;
	if (w <= 0 || h <= 0 || w > maxtex || h > maxtex) return NULL;
	t = calloc(1, sizeof *t);
	t->w = w;
	t->h = h;
	t->tw = npot ? w : pot(w);
	t->th = npot ? h : pot(h);
	while (glGetError() != GL_NO_ERROR) {}
	glGenTextures(1, &t->id);
	glBindTexture(GL_TEXTURE_2D, t->id);
	cur_tex = t->id;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, edge_clamp ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, edge_clamp ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	if (t->tw == w && t->th == h) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	} else {
		/* vieux pilote sans textures de taille quelconque : on complète en répétant le bord */
		unsigned char *buf = malloc((size_t)t->tw * t->th * 4);
		int y, x;
		for (y = 0; y < t->th; y++) {
			const unsigned char *src = bgra + (size_t)(y < h ? y : h - 1) * stride;
			unsigned char *dst = buf + (size_t)y * t->tw * 4;
			memcpy(dst, src, (size_t)w * 4);
			for (x = w; x < t->tw; x++) memcpy(dst + x * 4, src + (w - 1) * 4, 4);
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->tw, t->th, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, buf);
		free(buf);
	}
	if (glGetError() != GL_NO_ERROR) {
		glDeleteTextures(1, &t->id);
		cur_tex = 0;
		free(t);
		return NULL;
	}
	return t;
}

void gfx_tex_free(void *p)
{
	GTex *t = p;
	if (!t) return;
	if (cur_tex == t->id) cur_tex = 0;
	glDeleteTextures(1, &t->id);
	free(t);
}

void gfx_begin(int w, int h, unsigned rgb)
{
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, w, h, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	cur_blend = -1;
	cur_tex = 0;
}

static void set_blend(int mode)
{
	if (mode == cur_blend) return;
	cur_blend = mode;
	if (mode == 0) glDisable(GL_BLEND);
	else {
		glEnable(GL_BLEND);
		if (mode == 1) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);           /* prémultiplié */
		else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);               /* couleur simple */
	}
}

static void set_tex(int on)
{
	if (on == tex_enabled) return;
	tex_enabled = on;
	if (on) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
}

void gfx_image(void *p, float sx0, float sy0, float sx1, float sy1,
		float x0, float y0, float x1, float y1, unsigned tint)
{
	GTex *t = p;
	float u0, v0, u1, v1;
	if (!t) return;
	u0 = sx0 / t->tw; v0 = sy0 / t->th;
	u1 = sx1 / t->tw; v1 = sy1 / t->th;
	set_tex(1);
	if (!tint) {
		set_blend(0);
		glColor4ub(255, 255, 255, 255);
	} else {
		unsigned a = tint >> 24;
		set_blend(1);
		glColor4ub((GLubyte)(((tint >> 16) & 255) * a / 255), (GLubyte)(((tint >> 8) & 255) * a / 255),
				(GLubyte)((tint & 255) * a / 255), (GLubyte)a);
	}
	if (cur_tex != t->id) {
		glBindTexture(GL_TEXTURE_2D, t->id);
		cur_tex = t->id;
	}
	glBegin(GL_QUADS);
	glTexCoord2f(u0, v0); glVertex2f(x0, y0);
	glTexCoord2f(u1, v0); glVertex2f(x1, y0);
	glTexCoord2f(u1, v1); glVertex2f(x1, y1);
	glTexCoord2f(u0, v1); glVertex2f(x0, y1);
	glEnd();
}

void gfx_rect(float x0, float y0, float x1, float y1, unsigned argb)
{
	unsigned a = argb >> 24;
	set_tex(0);
	set_blend(a < 255 ? 2 : 0);
	glColor4ub((GLubyte)((argb >> 16) & 255), (GLubyte)((argb >> 8) & 255), (GLubyte)(argb & 255), (GLubyte)a);
	glBegin(GL_QUADS);
	glVertex2f(x0, y0);
	glVertex2f(x1, y0);
	glVertex2f(x1, y1);
	glVertex2f(x0, y1);
	glEnd();
}

void gfx_origin(float x, float y)
{
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(x, y, 0);
}

void gfx_end(void)
{
	set_tex(1);
}
