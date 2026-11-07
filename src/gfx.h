/* gfx.h — interface de rendu minimale (OpenGL sous Windows, logiciel pour les tests) */
#ifndef LIRE_GFX_H
#define LIRE_GFX_H

int  gfx_init(void);                 /* après création du contexte ; renvoie 0 si OK */
int  gfx_max_texture(void);          /* taille max d'une texture (px) */

/* Crée une texture à partir de pixels BGRA (prémultipliés). */
void *gfx_tex_create(int w, int h, const unsigned char *bgra, int stride);
void gfx_tex_free(void *tex);

void gfx_begin(int w, int h, unsigned rgb);      /* début d'image, efface le fond */
/* Dessine la zone (sx0,sy0)-(sx1,sy1) de la texture (en texels) dans (x0,y0)-(x1,y1) (en pixels écran).
   tint = 0 : copie opaque ; sinon mélange alpha prémultiplié, teinté par la couleur ARGB tint. */
void gfx_image(void *tex, float sx0, float sy0, float sx1, float sy1,
		float x0, float y0, float x1, float y1, unsigned tint);
void gfx_rect(float x0, float y0, float x1, float y1, unsigned argb);
void gfx_origin(float x, float y);               /* décale les dessins suivants */
void gfx_end(void);

#endif
