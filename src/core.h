/* core.h — moteur portable de la visionneuse Lire */
#ifndef LIRE_CORE_H
#define LIRE_CORE_H

enum { BTN_LEFT = 1, BTN_MIDDLE = 2, BTN_RIGHT = 3 };
enum { LMOD_CTRL = 1, LMOD_SHIFT = 2, LMOD_ALT = 4 };

/* Touches spéciales (les lettres/chiffres physiques sont passés en ASCII majuscule : 'A'..'Z', '0'..'9') */
enum {
	K_PGUP = 256, K_PGDN, K_HOME, K_END, K_UP, K_DOWN, K_LEFT, K_RIGHT,
	K_SPACE, K_ENTER, K_ESC, K_BACKSPACE, K_F1, K_F5, K_F11, K_PLUS, K_MINUS,
	K_TAB, K_F4, K_F3
};

enum { CUR_ARROW, CUR_HAND, CUR_CROSS, CUR_AUTO, CUR_TEXT };
enum { INPUT_NONE, INPUT_GOTO, INPUT_PASSWORD, INPUT_FIND };

int  core_init(int nthreads);            /* 0 si OK ; nthreads <= 0 : automatique */
void core_shutdown(void);
void core_quick_exit(void);              /* arrête les rendus avant ExitProcess */
void core_open(const char *path);        /* chemin UTF-8 ; ouvre un nouvel onglet (ou active celui déjà ouvert) */
void core_reload(void);                  /* recharge l'onglet actif */
void core_resize(int w, int h);          /* taille de la zone cliente en pixels */
void core_set_dpi(float dpi);
/* mode d'affichage (bits) : deux pages côte à côte, page de garde seule, défilement continu */
enum { VIEW_TWO_UP = 1, VIEW_COVER = 2, VIEW_CONTINUOUS = 4 };
void core_set_view_mode(int flags);     /* mode des prochains fichiers ouverts (réglage mémorisé) */
int  core_view_mode(void);
/* menu contextuel : drapeaux par ligne */
enum { MENU_CHECKED = 1, MENU_DISABLED = 2, MENU_SEPARATOR = 4, MENU_TICKED = 8 };
void core_set_wheel_zoom(int on);
int  core_wheel_zoom(void);

int  core_poll(void);                    /* intègre les résultats des threads ; 1 si redessin nécessaire */
void core_draw(void);
int  core_dirty(void);                   /* 1 si un redessin est demandé (et remet l'indicateur à 0) */
int  core_timeout_ms(void);              /* délai avant le prochain redessin programmé, -1 si aucun */
int  core_busy(void);                    /* 1 si du rendu (y compris anticipé) est encore en cours */
int  core_view_complete(void);           /* 1 si toute la zone visible est nette */

void core_mouse_down(int btn, int x, int y, int mods);
void core_mouse_up(int btn, int x, int y, int mods);
int  core_mouse_double(int btn, int x, int y, int mods);   /* double-clic : 1 si traité (mot sélectionné) */
void core_mouse_move(int x, int y, int mods);
void core_mouse_leave(void);
void core_wheel(float dy, float dx, int x, int y, int mods);   /* dy>0 : vers le haut, dx>0 : vers la droite */
void core_key(int key, int mods);
void core_char(unsigned codepoint);
int  core_input_mode(void);
int  core_cursor(void);

/* accès pour les tests */
int    core_page_count(void);
int    core_current_page(void);
double core_zoom_percent(void);
int    core_tab_count(void);
int    core_active_tab(void);
void   core_stats(char *buf, int len);
void   core_find_info(int *total, int *rank, int *pending, int *page);   /* recherche (tests) */

/* Fonctions à fournir par la plateforme */
void   platform_wake(void);                  /* appelable depuis n'importe quel thread */
void   platform_set_title(const char *utf8);
void   platform_open_dialog(void);
/* menu déroulant sous un bouton de la barre ; rend l'indice choisi ou -1 (libellés UTF-8, « \t » avant le raccourci) */
int    platform_popup_menu(const char *const *labels, const int *flags, int n, int right_x, int top_y);
void   platform_set_clipboard(const char *utf8);
char  *platform_get_clipboard(void);        /* texte UTF-8 (à libérer avec free), NULL si vide */
void   platform_open_with(const char *utf8_path, int right_x, int top_y);   /* menu « Ouvrir avec » sous le bouton */
void   platform_toggle_fullscreen(void);
void   platform_quit(void);
void   platform_wheel_mode_changed(int zoom);
double platform_time(void);                  /* secondes */

#endif
