/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#include "theme.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------------ */
/* The values                                                                 */
/* ------------------------------------------------------------------------ */

static const char *const color_key[TC_COUNT] = {
	"window_bg", "window_fg", "panel_bg", "accent",
	"term_bg", "term_fg", "cursor",
	"touch", "create", "dest", "remove",
};
static const char *const color_label[TC_COUNT] = {
	"Window background", "Text", "File list background", "Accent",
	"Terminal background", "Terminal text", "Cursor",
	"Mentioned file", "Created file", "Destination", "Removed file",
};
/* Only the preview highlights have a value of their own; the rest follow the system */
static const char *const color_default[TC_COUNT] = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	"#e5a50a", "#26a269", "#3584e4", "#e01b24",
};

const char *fhi_theme_color_key(int idx) { return color_key[idx]; }
const char *fhi_theme_color_label(int idx) { return color_label[idx]; }

void
fhi_theme_init(FhiTheme *t)
{
	memset(t, 0, sizeof *t);
	for (int i = 0; i < TC_COUNT; i++)
		if (color_default[i]) {
			gdk_rgba_parse(&t->col[i], color_default[i]);
			t->set[i] = TRUE;
		}
	t->scheme = 0;
	t->radius = -1;
	t->opacity = 100;
}

void
fhi_theme_clear(FhiTheme *t)
{
	g_free(t->name);
	t->name = NULL;
}

void
fhi_theme_copy(FhiTheme *dst, const FhiTheme *src)
{
	char *name = g_strdup(src->name);
	fhi_theme_clear(dst);
	*dst = *src;
	dst->name = name;
}

/* A sensible 16 colors (the usual xterm-ish set) for "custom palette" before anything is chosen */
void
fhi_theme_default_palette(GdkRGBA out[FHI_PALETTE_SIZE])
{
	static const char *const p[FHI_PALETTE_SIZE] = {
		"#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
		"#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#ffffff",
	};
	for (int i = 0; i < FHI_PALETTE_SIZE; i++) gdk_rgba_parse(&out[i], p[i]);
}

double
fhi_color_luminance(const GdkRGBA *c)
{
	return 0.2126 * c->red + 0.7152 * c->green + 0.0722 * c->blue;
}

/* ------------------------------------------------------------------------ */
/* Reading and writing                                                        */
/* ------------------------------------------------------------------------ */

static void
to_hex(const GdkRGBA *c, char out[8])
{
	g_snprintf(out, 8, "#%02x%02x%02x",
	           (int)(CLAMP(c->red, 0, 1) * 255 + 0.5),
	           (int)(CLAMP(c->green, 0, 1) * 255 + 0.5),
	           (int)(CLAMP(c->blue, 0, 1) * 255 + 0.5));
}

/* Parse "#rrggbb", "rgb(...)", names. Alpha is dropped: opacity is its own setting. */
static gboolean
parse_color(const char *text, GdkRGBA *out)
{
	GdkRGBA c;
	gboolean ok = gdk_rgba_parse(&c, text);
	if (ok) { c.alpha = 1.0; *out = c; }
	return ok;
}

/*
 * The value of `key`, trimmed, with a trailing comment removed ("light   # or dark").
 * A '#' only starts a comment after white space, so "#fff0f5" stays a color.
 * NULL if the key is missing or empty. Free with g_free().
 */
static char *
get_value(GKeyFile *kf, const char *group, const char *key)
{
	char *v = g_key_file_get_string(kf, group, key, NULL);
	if (v == NULL) return NULL;
	for (char *c = v + 1; *c; c++)
		if (*c == '#' && g_ascii_isspace(c[-1])) { *c = '\0'; break; }
	g_strstrip(v);
	if (*v == '\0') g_clear_pointer(&v, g_free);
	return v;
}

static gboolean
get_int(GKeyFile *kf, const char *group, const char *key, int *out)
{
	char *v = get_value(kf, group, key);
	if (v == NULL) return FALSE;
	char *end;
	long n = strtol(v, &end, 10);
	gboolean ok = *end == '\0';
	if (ok) *out = (int)CLAMP(n, -1000000, 1000000);
	g_free(v);
	return ok;
}

static int
parse_scheme(const char *v)
{
	if (g_ascii_strcasecmp(v, "light") == 0 || strcmp(v, "1") == 0) return 1;
	if (g_ascii_strcasecmp(v, "dark") == 0 || strcmp(v, "2") == 0) return 2;
	return 0;   /* "system", "0", anything else */
}

void
fhi_theme_load_group(FhiTheme *t, GKeyFile *kf, const char *group)
{
	char *v;

	if ((v = get_value(kf, group, "name"))) {
		g_free(t->name);
		t->name = v;
	}
	if ((v = get_value(kf, group, "scheme"))) {
		t->scheme = parse_scheme(v);
		g_free(v);
	}
	for (int i = 0; i < TC_COUNT; i++) {
		if ((v = get_value(kf, group, color_key[i]))) {
			if (parse_color(v, &t->col[i])) t->set[i] = TRUE;
			g_free(v);
		}
	}
	/* palette: exactly 16 colors, separated by ; or , or spaces */
	if ((v = get_value(kf, group, "palette"))) {
		for (char *c = v; *c; c++) if (*c == ';' || *c == ',') *c = ' ';
		char **w = g_strsplit_set(v, " \t", -1);
		GdkRGBA pal[FHI_PALETTE_SIZE];
		int n = 0;
		gboolean ok = TRUE;
		for (char **x = w; *x; x++) {
			if (!**x) continue;
			if (n >= FHI_PALETTE_SIZE || !parse_color(*x, &pal[n])) { ok = FALSE; break; }
			n++;
		}
		if (ok && n == FHI_PALETTE_SIZE) {
			memcpy(t->palette, pal, sizeof pal);
			t->has_palette = TRUE;
		}
		g_strfreev(w);
		g_free(v);
	}
	int n;
	if (get_int(kf, group, "radius", &n)) t->radius = CLAMP(n, 0, 24);
	if (get_int(kf, group, "opacity", &n)) t->opacity = CLAMP(n, 20, 100);
}

void
fhi_theme_save_group(const FhiTheme *t, GKeyFile *kf, const char *group)
{
	static const char *const schemes[] = { "system", "light", "dark" };
	char hex[8];
	if (t->name) g_key_file_set_string(kf, group, "name", t->name);
	g_key_file_set_string(kf, group, "scheme", schemes[CLAMP(t->scheme, 0, 2)]);
	for (int i = 0; i < TC_COUNT; i++)
		if (t->set[i]) {
			to_hex(&t->col[i], hex);
			g_key_file_set_string(kf, group, color_key[i], hex);
		}
	if (t->has_palette) {
		GString *p = g_string_new(NULL);
		for (int i = 0; i < FHI_PALETTE_SIZE; i++) {
			to_hex(&t->palette[i], hex);
			g_string_append_printf(p, "%s%s", i ? ";" : "", hex);
		}
		g_key_file_set_string(kf, group, "palette", p->str);
		g_string_free(p, TRUE);
	}
	if (t->radius >= 0) g_key_file_set_integer(kf, group, "radius", t->radius);
	if (t->opacity < 100) g_key_file_set_integer(kf, group, "opacity", t->opacity);
}

/* ------------------------------------------------------------------------ */
/* CSS                                                                        */
/* ------------------------------------------------------------------------ */

static void
mix(const GdkRGBA *a, const GdkRGBA *b, double t, GdkRGBA *out)
{
	out->red = a->red * (1 - t) + b->red * t;
	out->green = a->green * (1 - t) + b->green * t;
	out->blue = a->blue * (1 - t) + b->blue * t;
	out->alpha = 1;
}

/* Black or white text, whichever reads on `bg` */
static void
contrast_for(const GdkRGBA *bg, GdkRGBA *out)
{
	gdk_rgba_parse(out, fhi_color_luminance(bg) > 0.58 ? "#1e1e1e" : "#ffffff");
}

/* @define-color for each name in the space separated list */
static void
define(GString *s, const char *names, const char *value)
{
	char **n = g_strsplit(names, " ", -1);
	for (char **x = n; *x; x++) g_string_append_printf(s, "@define-color %s %s;\n", *x, value);
	g_strfreev(n);
}

gboolean
fhi_theme_is_dark(const FhiTheme *t, gboolean system_dark)
{
	if (t->scheme == 2) return TRUE;
	if (t->scheme == 1) return FALSE;
	if (t->set[TC_WINDOW_BG]) return fhi_color_luminance(&t->col[TC_WINDOW_BG]) < 0.5;
	return system_dark;
}

gboolean
fhi_theme_is_transparent(const FhiTheme *t, gboolean composited)
{
	return composited && t->opacity < 100;
}

/* "#rrggbb" of a mixed with b */
static void
hexmix(const GdkRGBA *a, const GdkRGBA *b, double t, char out[8])
{
	GdkRGBA m;
	mix(a, b, t, &m);
	to_hex(&m, out);
}

/*
 * Rules for the standard widgets, written out with the real colors.
 *
 * The named colors below are enough on themes that use them (GTK 4.10 and newer), but older
 * GTK 4 themes (4.8, still common) bake their colors in and would ignore them, leaving a
 * pink theme with gray buttons. A style sheet of ours beats the theme's whatever the
 * selectors, so these rules work everywhere. States are spelled out (hover, checked ...)
 * because a rule of ours also wins over the theme's hover rule, and feedback must survive.
 *
 *   W window  F text  P panel  PF panel text  A accent  AF text on accent
 * Anything that is not set is left alone.
 */
static void
widget_rules(GString *s, gboolean has_win, const GdkRGBA *W, gboolean has_fg, const GdkRGBA *F,
             gboolean has_panel, const GdkRGBA *P, const GdkRGBA *PF,
             gboolean has_acc, const GdkRGBA *A, const GdkRGBA *AF)
{
	char w[8], f[8], p[8], pf[8], a[8], af[8], b0[8], b1[8], b2[8], b3[8];
	if (has_win) to_hex(W, w);
	if (has_fg) to_hex(F, f);
	if (has_panel) { to_hex(P, p); to_hex(PF, pf); }
	if (has_acc) { to_hex(A, a); to_hex(AF, af); }

	if (has_win && has_fg) {
		hexmix(W, F, 0.04, b0);   /* fields */
		hexmix(W, F, 0.09, b1);   /* buttons */
		hexmix(W, F, 0.15, b2);   /* hovered */
		hexmix(W, F, 0.24, b3);   /* pressed, troughs */
	}
	if (has_win)
		g_string_append_printf(s, "window { background-color: %s; color: %s; }\n"
		    "headerbar { background: %s; color: %s; box-shadow: none; border-color: alpha(%s, 0.15); }\n"
		    "popover > contents, popover.background > contents { background-color: %s; color: %s;"
		    " border-color: alpha(%s, 0.18); }\n"
		    "tooltip.background { background-color: %s; color: %s; }\n",
		    w, f, w, f, f, w, f, f, w, f);
	else if (has_fg)
		g_string_append_printf(s, "window { color: %s; }\n", f);

	if (has_win && has_fg) {
		const char *field = has_panel ? p : b0;
		g_string_append_printf(s,
		    "button { background: %s; background-image: none; color: %s; border-color: alpha(%s, 0.18);"
		    " box-shadow: none; text-shadow: none; }\n"
		    "button:hover { background: %s; }\n"
		    "button:active, button:checked { background: %s; }\n"
		    "button:disabled { color: alpha(%s, 0.45); }\n"
		    "button.flat, button.flat:disabled { background: transparent; border-color: transparent; }\n"
		    "button.flat:hover { background: %s; }\n"
		    "button.flat:active, button.flat:checked { background: %s; }\n"
		    "entry, spinbutton { background-color: %s; color: %s; border-color: alpha(%s, 0.2); box-shadow: none; }\n"
		    "separator { background-color: alpha(%s, 0.15); }\n"
		    "frame > border { border-color: alpha(%s, 0.18); }\n"
		    "paned > separator { background-color: alpha(%s, 0.18); }\n"
		    "scrollbar slider { background-color: alpha(%s, 0.35); }\n"
		    "scrollbar slider:hover { background-color: alpha(%s, 0.55); }\n"
		    "modelbutton:hover { background-color: %s; }\n"
		    "switch { background-color: %s; background-image: none; border-color: alpha(%s, 0.2); }\n"
		    "switch slider { background-color: %s; background-image: none; border-color: alpha(%s, 0.3); }\n"
		    "scale trough { background-color: %s; background-image: none; }\n"
		    "check, radio { background-color: %s; background-image: none; color: %s; border-color: alpha(%s, 0.4); }\n",
		    b1, f, f, b2, b3, f, b1, b3, field, f, f, f, f, f, f, f, b1, b3, f, w, f, b3, field, f, f);
	}
	if (has_panel)
		g_string_append_printf(s,
		    "listview, .view, textview, iconview { background-color: %s; color: %s; }\n"
		    "listview > row:hover { background-color: alpha(%s, 0.07); }\n",
		    p, pf, pf);
	if (has_acc) {
		g_string_append_printf(s,
		    "listview > row:selected, row:selected, .view:selected, iconview:selected, selection,\n"
		    "listview > row:selected:hover { background-color: %s; color: %s; }\n"
		    "switch:checked { background-color: %s; }\n"
		    "scale trough highlight { background-color: %s; background-image: none; }\n"
		    "scale slider { background-color: %s; background-image: none; border-color: %s; }\n"
		    "check:checked, radio:checked { background-color: %s; color: %s; border-color: %s; }\n"
		    "entry:focus-within, spinbutton:focus-within { border-color: %s; }\n"
		    "*:focus-visible { outline-color: alpha(%s, 0.7); }\n",
		    a, af, a, a, a, a, a, af, a, a, a);
	}
}

/*
 * Names GTK 4.x themes use for the same few colors: the old ones (@theme_*) and the
 * newer Adwaita ones (@window_bg_color ...). Defining all of them makes this work on
 * any GTK 4 theme without asking which one it is.
 */
#define NAMES_WINDOW_BG "theme_bg_color window_bg_color headerbar_bg_color popover_bg_color dialog_bg_color sidebar_bg_color"
#define NAMES_WINDOW_FG "theme_fg_color window_fg_color headerbar_fg_color popover_fg_color dialog_fg_color sidebar_fg_color"
#define NAMES_PANEL_BG  "theme_base_color view_bg_color card_bg_color"
#define NAMES_PANEL_FG  "theme_text_color view_fg_color card_fg_color"
#define NAMES_ACCENT_BG "accent_bg_color accent_color theme_selected_bg_color"
#define NAMES_ACCENT_FG "accent_fg_color theme_selected_fg_color"

char *
fhi_theme_css(const FhiTheme *t, gboolean composited)
{
	GString *s = g_string_new(NULL);
	char buf[8];

	/* ---- colors ---- */
	GdkRGBA fg = { 0 }, panel = { 0 }, panel_fg = { 0 }, acc_fg = { 0 };
	gboolean have_fg = FALSE, have_panel = FALSE;
	if (t->set[TC_WINDOW_BG]) {
		const GdkRGBA *bg = &t->col[TC_WINDOW_BG];
		to_hex(bg, buf);
		define(s, NAMES_WINDOW_BG, buf);
		if (t->set[TC_WINDOW_FG]) fg = t->col[TC_WINDOW_FG];
		else contrast_for(bg, &fg);
		have_fg = TRUE;
		if (t->set[TC_PANEL_BG]) panel = t->col[TC_PANEL_BG];
		else mix(bg, &fg, 0.05, &panel);   /* a hair away from the window: panels read as panels */
		have_panel = TRUE;
	} else {
		if (t->set[TC_WINDOW_FG]) { fg = t->col[TC_WINDOW_FG]; have_fg = TRUE; }
		if (t->set[TC_PANEL_BG]) { panel = t->col[TC_PANEL_BG]; have_panel = TRUE; }
	}
	if (have_fg) {
		to_hex(&fg, buf);
		define(s, NAMES_WINDOW_FG, buf);
		/* The list shows text on the panel: follow the same text color unless the panel is
		 * set on its own and is so different that this text would not read on it. */
		if (!have_panel || (fhi_color_luminance(&fg) > 0.5) == (fhi_color_luminance(&panel) < 0.5)) {
			define(s, NAMES_PANEL_FG, buf);
			panel_fg = fg;
		} else {
			contrast_for(&panel, &panel_fg);
		}
	}
	if (have_panel) {
		to_hex(&panel, buf);
		define(s, NAMES_PANEL_BG, buf);
		if (!have_fg) {   /* a panel without a text color: keep the list readable */
			contrast_for(&panel, &panel_fg);
			to_hex(&panel_fg, buf);
			define(s, NAMES_PANEL_FG, buf);
		}
	}
	if (t->set[TC_ACCENT]) {
		to_hex(&t->col[TC_ACCENT], buf);
		define(s, NAMES_ACCENT_BG, buf);
		contrast_for(&t->col[TC_ACCENT], &acc_fg);
		to_hex(&acc_fg, buf);
		define(s, NAMES_ACCENT_FG, buf);
	}
	widget_rules(s, t->set[TC_WINDOW_BG], &t->col[TC_WINDOW_BG], have_fg, &fg,
	             have_panel, &panel, &panel_fg, t->set[TC_ACCENT], &t->col[TC_ACCENT], &acc_fg);

	/* ---- corners ---- */
	if (t->radius >= 0)
		g_string_append_printf(s,
		    "button, entry, spinbutton, dropdown > button, .fhi-row, .fhi-key, .fhi-sshcard,\n"
		    ".fhi-unsupported, .fhi-swatch, popover > contents, tooltip { border-radius: %dpx; }\n",
		    t->radius);

	/* ---- transparency: nothing at all unless it is asked for AND can work ----
	 * Translucent layers stack: three 55% backgrounds on top of each other look 91% solid. So
	 * the window itself becomes clear and each region carries the alpha exactly once: the title
	 * bar, the file panel (list + info bar, class fhi-leftpanel) and, in VTE, the terminal. */
	if (fhi_theme_is_transparent(t, composited)) {
		char a[G_ASCII_DTOSTR_BUF_SIZE];
		g_ascii_formatd(a, sizeof a, "%.2f", t->opacity / 100.0);
		g_string_append_printf(s,
		    "window.fhi-transparent { background-color: transparent; }\n"
		    "window.fhi-transparent headerbar { background: alpha(@theme_bg_color, %s); box-shadow: none; }\n"
		    "window.fhi-transparent .fhi-leftpanel { background-color: alpha(@theme_base_color, %s); }\n"
		    "window.fhi-transparent listview, window.fhi-transparent scrolledwindow:not(.fhi-menu),\n"
		    "window.fhi-transparent frame, window.fhi-transparent paned, window.fhi-transparent vte-terminal {"
		    " background: transparent; }\n",
		    a, a);
	}
	return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Finding, loading and saving theme files                                    */
/* ------------------------------------------------------------------------ */

char *
fhi_theme_user_dir(void)
{
	return g_build_filename(g_get_user_config_dir(), "fhi", "themes", NULL);
}

void
fhi_theme_info_free(gpointer p)
{
	FhiThemeInfo *i = p;
	g_free(i->id);
	g_free(i->name);
	g_free(i->path);
	g_free(i);
}

static char *
file_theme_name(const char *path)
{
	GKeyFile *kf = g_key_file_new();
	char *name = NULL;
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
		name = get_value(kf, "theme", "name");
	g_key_file_unref(kf);
	return name;
}

static void
scan_dir(GPtrArray *out, const char *dir, gboolean user)
{
	GDir *d = g_dir_open(dir, 0, NULL);
	if (!d) return;
	const char *f;
	while ((f = g_dir_read_name(d))) {
		if (!g_str_has_suffix(f, ".ini") || strlen(f) <= 4) continue;
		char *id = g_strndup(f, strlen(f) - 4);
		gboolean dup = FALSE;
		for (guint i = 0; i < out->len; i++)
			if (strcmp(((FhiThemeInfo *)out->pdata[i])->id, id) == 0) dup = TRUE;   /* the user's file came first */
		if (dup) { g_free(id); continue; }
		FhiThemeInfo *info = g_new0(FhiThemeInfo, 1);
		info->path = g_build_filename(dir, f, NULL);
		info->name = file_theme_name(info->path);
		if (!info->name) info->name = g_strdup(id);
		info->id = id;
		info->user = user;
		g_ptr_array_add(out, info);
	}
	g_dir_close(d);
}

static gint
compare_info(gconstpointer a, gconstpointer b)
{
	const FhiThemeInfo *x = *(FhiThemeInfo *const *)a, *y = *(FhiThemeInfo *const *)b;
	return g_utf8_collate(x->name, y->name);
}

GPtrArray *
fhi_theme_list(const char *system_dir)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(fhi_theme_info_free);
	char *user = fhi_theme_user_dir();
	scan_dir(out, user, TRUE);
	g_free(user);
	if (system_dir) scan_dir(out, system_dir, FALSE);
	g_ptr_array_sort(out, compare_info);
	return out;
}

gboolean
fhi_theme_load_file(FhiTheme *t, const FhiThemeInfo *info)
{
	GKeyFile *kf = g_key_file_new();
	gboolean ok = g_key_file_load_from_file(kf, info->path, G_KEY_FILE_NONE, NULL) &&
	              g_key_file_has_group(kf, "theme");
	if (ok) {
		FhiTheme fresh;
		fhi_theme_init(&fresh);
		/* a theme file says what it wants; the preview colors it leaves out keep their defaults */
		fhi_theme_load_group(&fresh, kf, "theme");
		if (!fresh.name) fresh.name = g_strdup(info->name);
		fhi_theme_copy(t, &fresh);
		fhi_theme_clear(&fresh);
	}
	g_key_file_unref(kf);
	return ok;
}

char *
fhi_theme_save_as(const FhiTheme *t, const char *name)
{
	GString *slug = g_string_new(NULL);
	for (const char *c = name; *c; c++) {
		if (g_ascii_isalnum(*c)) g_string_append_c(slug, g_ascii_tolower(*c));
		else if (slug->len && slug->str[slug->len - 1] != '-') g_string_append_c(slug, '-');
	}
	while (slug->len && slug->str[slug->len - 1] == '-') g_string_truncate(slug, slug->len - 1);
	if (slug->len == 0) { g_string_free(slug, TRUE); return NULL; }

	FhiTheme copy;
	fhi_theme_init(&copy);
	fhi_theme_copy(&copy, t);
	g_free(copy.name);
	copy.name = g_strstrip(g_strdup(name));

	GKeyFile *kf = g_key_file_new();
	fhi_theme_save_group(&copy, kf, "theme");
	fhi_theme_clear(&copy);

	char *dir = fhi_theme_user_dir();
	g_mkdir_with_parents(dir, 0755);
	char *file = g_strdup_printf("%s.ini", slug->str);
	char *path = g_build_filename(dir, file, NULL);
	gboolean ok = g_key_file_save_to_file(kf, path, NULL);
	g_key_file_unref(kf);
	g_free(file);
	g_free(path);
	g_free(dir);
	return ok ? g_string_free(slug, FALSE) : (g_string_free(slug, TRUE), NULL);
}
