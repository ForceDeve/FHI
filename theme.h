/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#ifndef FHI_THEME_H
#define FHI_THEME_H

#include <gtk/gtk.h>

/*
 * A theme is a handful of OPTIONAL values. Whatever a theme does not set follows the
 * system's GTK theme, so a user whose whole desktop is already pink gets a pink FHI
 * without configuring anything, and a theme only has to say what it wants to change.
 *
 * Themes are small INI files (the examples are in the themes folder of the source tree),
 * looked up in ~/.config/fhi/themes/ first and then in the folder FHI was installed with:
 *
 *   [theme]
 *   name       = Pastel Pink
 *   scheme     = light            # light | dark | system
 *   window_bg  = #fff0f5          # window, title bar, menus
 *   window_fg  = #5b3a4a          # text
 *   panel_bg   = #fff8fb          # the file list (default: a little off window_bg)
 *   accent     = #f28cb1          # selection, switches, focus
 *   term_bg    = #fff5f8          # terminal
 *   term_fg    = #6b4457
 *   cursor     = #e75a8f
 *   palette    = #000000;#cd0000;... (16 colors: the terminal's ANSI colors)
 *   touch / create / dest / remove = #rrggbb     command preview highlights
 *   radius     = 12               # rounded corners in px (0 = square)
 *   opacity    = 90               # percent, 100 = solid (see fhi_theme_css)
 */

enum {
	TC_WINDOW_BG, TC_WINDOW_FG, TC_PANEL_BG, TC_ACCENT,
	TC_TERM_BG, TC_TERM_FG, TC_CURSOR,
	TC_TOUCH, TC_CREATE, TC_DEST, TC_REMOVE,
	TC_COUNT
};

#define FHI_PALETTE_SIZE 16

typedef struct {
	gboolean set[TC_COUNT];            /* FALSE: follow the system */
	GdkRGBA  col[TC_COUNT];            /* the value, alpha always 1 (opacity is separate) */
	gboolean has_palette;
	GdkRGBA  palette[FHI_PALETTE_SIZE];
	int      scheme;                   /* 0 system, 1 light, 2 dark */
	int      radius;                   /* px, -1 = the system's */
	int      opacity;                  /* 20..100; 100 = solid and costs nothing */
	char    *name;                     /* the theme this came from, or NULL */
} FhiTheme;

/* The look FHI had before themes existed: only the four preview colors are set */
void fhi_theme_init(FhiTheme *t);
void fhi_theme_clear(FhiTheme *t);               /* frees name; the struct can be fhi_theme_init()ed again */
void fhi_theme_copy(FhiTheme *dst, const FhiTheme *src);

const char *fhi_theme_color_key(int idx);        /* "window_bg", ... (also the key in settings.ini) */
const char *fhi_theme_color_label(int idx);      /* "Window background", ... */

/* Layer the keys found in `group` of `kf` over `t`; keys that are missing change nothing. */
void fhi_theme_load_group(FhiTheme *t, GKeyFile *kf, const char *group);
void fhi_theme_save_group(const FhiTheme *t, GKeyFile *kf, const char *group);

/* The palette FHI starts from when someone switches "custom palette" on */
void fhi_theme_default_palette(GdkRGBA out[FHI_PALETTE_SIZE]);

/* Brightness 0..1 of a color: tells light from dark */
double fhi_color_luminance(const GdkRGBA *c);

/*
 * Is the effective look dark? Follows t->scheme (0 system / 1 light / 2 dark); with
 * "system" and a window_bg set by the theme, the background decides.
 */
gboolean fhi_theme_is_dark(const FhiTheme *t, gboolean system_dark);

/*
 * GTK CSS for the colors, corner radius and transparency of the theme. It redefines the
 * theme's named colors (@theme_bg_color, @window_bg_color, @accent_bg_color ...), so
 * every standard widget follows along, not only the ones FHI draws itself.
 *
 * Transparency: only when opacity < 100 AND the screen is composited does the CSS get
 * extra rules and the window the class "fhi-transparent". With opacity 100 (the
 * default) the result is exactly as if the feature did not exist: no extra rules, nothing
 * to blend. Returns a new string.
 */
char *fhi_theme_css(const FhiTheme *t, gboolean composited);

/* Does this theme need the compositor, i.e. is the window going to be see-through? */
gboolean fhi_theme_is_transparent(const FhiTheme *t, gboolean composited);

/* ---- finding themes ---- */

typedef struct {
	char *id;      /* file name without ".ini", used to select it and saved in settings.ini */
	char *name;    /* what the file calls itself ("Pastel Pink"), or the id */
	char *path;
	gboolean user; /* from ~/.config/fhi/themes */
} FhiThemeInfo;

char *fhi_theme_user_dir(void);                       /* ~/.config/fhi/themes (new string) */
GPtrArray *fhi_theme_list(const char *system_dir);    /* FhiThemeInfo*, sorted by name; a user file hides an installed one of the same id */
void fhi_theme_info_free(gpointer info);
/* Read theme `info` on top of the defaults. FALSE if the file cannot be read. */
gboolean fhi_theme_load_file(FhiTheme *t, const FhiThemeInfo *info);
/* Write `t` as a theme file called `name` in the user folder. Returns the id, or NULL. */
char *fhi_theme_save_as(const FhiTheme *t, const char *name);

#endif
