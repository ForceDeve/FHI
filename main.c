/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#include <gtk/gtk.h>
#include <vte/vte.h>
#include <string.h>
#include <unistd.h> /* tcgetpgrp() */
#include <stdio.h>
#include <glib/gstdio.h>
#include "preview.h"
#include "filetype.h"
#include "gitinfo.h"
#include "sshinfo.h"
#include "llmcli.h"
#include "tmuxinfo.h"
#include <signal.h>
#include "theme.h"
#include <strings.h>

/* Sort modes. For size / dates the "normal" direction is largest / newest first. */
enum { SORT_NAME, SORT_SIZE, SORT_MODIFIED, SORT_CREATED, SORT_TYPE, SORT_COUNT };
static const char *const sort_name[SORT_COUNT] = { "Name", "Size", "Modified", "Created", "Type" };
static const char *const sort_dir_text[SORT_COUNT][2] = {
	{ "A to Z", "Z to A" }, { "largest first", "smallest first" },
	{ "newest first", "oldest first" }, { "newest first", "oldest first" },
	{ "A to Z", "Z to A" },
};

/* Everything the hamburger menu can change. Saved in ~/.config/fhi/settings.ini */
typedef struct {
	FhiTheme theme;             /* colors, scheme, corners, opacity (see theme.h) */
	gboolean show_hidden;       /* list files starting with "." */
	gboolean info_type, info_size, info_created, info_accessed;
	gboolean info_wrap;         /* wrap the info bar instead of cutting it off */
	int info_font;              /* font size of the info bar, in points */
	int sort_mode;              /* SORT_* */
	gboolean sort_reverse;
	gboolean folders_first;
	char *term_font;            /* terminal font from settings.ini ("term_font"), NULL = default */
} Settings;

/*
 * Everything the callbacks need to share. GTK callbacks only receive ONE
 * user_data pointer, so we bundle our variables in a struct and pass a pointer
 * to it. activate() creates one of these and hands it to every callback.
 */
typedef struct {
	GtkWidget *window;
	GtkWidget *terminal;
	GtkWidget *list_view;            /* the file list (left panel) */
	GtkDirectoryList *dir_list;      /* the folder the list is showing */
	GtkSingleSelection *selection;   /* which row is highlighted */
	GPid shell_pid;                  /* process id of zsh in the current tab, 0 until it started */
	GtkWidget *notebook;             /* the tabs; every page is a VteTerminal, st->terminal is the shown one */

	/* --- tmux in the terminal: the file list follows the pane that has focus --- */
	pid_t tmux_client;               /* the tmux client these belong to, 0 = tmux is not in front */
	pid_t tmux_pane_pid;             /* first process of the active pane (its shell) */
	char *tmux_tty;                  /* the pane's terminal, NULL until the first answer */
	char *tmux_path;                 /* the pane's folder */
	gboolean tmux_fhi;               /* the pane's shell has FHI's zsh hooks */

	/* --- command preview (highlighting files the typed command affects) --- */
	GListStore *ghosts;              /* fake rows for files that do not exist yet */
	GListStore *peeks;               /* rows of opened ("peeked") folders, shown indented */
	GHashTable *expanded;            /* paths of the folders that are peeked open (a set) */
	GHashTable *peek_info;           /* path -> GFileInfo of a peeked folder's own row, for sorting */
	GHashTable *roles;               /* file name -> FhiRole, for the highlighting */
	GHashTable *bound_rows;          /* the row widgets currently on screen */
	char *last_buffer;               /* the last command line text we received */
	char *ghost_key;                 /* names of the current ghosts, to spot changes */
	guint preview_idle;              /* id of a scheduled update_preview(), 0 if none */
	guint resync_timer;              /* id of a scheduled resync_directory(), 0 if none */

	/* --- hamburger menu --- */
	Settings cfg;
	gboolean system_dark;            /* the system's dark-theme setting at startup */
	GtkCssProvider *css;             /* rebuilt whenever a theme setting changes */
	GtkFilter *hidden_filter;        /* hides dotfiles when cfg.show_hidden is off */
	GtkWidget *info_label;           /* the file info bar under the list */
	GtkWidget *menu_button;          /* toggle button in the title bar */
	GtkWidget *menu_layer;           /* the menu panel + click-away area, over the window */
	GtkWidget *menu_stack;
	GtkWidget *menu_first;           /* first button of the main page, gets focus on open */
	GtkWidget *left_panel;           /* the file list side: dimmed when an unsupported shell runs */
	GtkWidget *unsupported_label;    /* "bash is not supported", over the panel */
	char *unsupported_shell;         /* name of that shell, NULL while all is fine */
	FhiSsh *ssh;                     /* the ssh session in front of the terminal, NULL if none */
	GtkWidget *ssh_card;             /* session info, over the dimmed panel */

	/* --- AI assistant in the terminal (claude, codex, aider...), see llmcli.c --- */
	const FhiLlmTool *llm;           /* the assistant in front of the terminal, NULL if none */
	pid_t llm_pid;
	FhiLlmWatch *llm_watch;          /* looks for files it creates or edits */
	GHashTable *llm_act;             /* path (relative to the list's folder) -> LlmAct, shown as lit rows */
	guint llm_tick;
	GtkWidget *llm_label;            /* "Claude Code" under the list while one runs */
	GtkWidget *last_pane;            /* list or terminal: where focus goes after the menu closes */
	GtkWidget *font_btn;             /* terminal font chooser */
	/* theme page widgets, kept so they can follow the settings (see theme_sync_widgets) */
	GtkWidget *scheme_dd;
	GtkWidget *theme_dd;
	GtkWidget *opt_btn[TC_COUNT];    /* one color button per theme color */
	GtkWidget *opt_clear[TC_COUNT];  /* "follow the system" button next to it (NULL if none) */
	GtkWidget *pal_switch, *pal_box, *pal_btn[FHI_PALETTE_SIZE];
	GtkWidget *radius_scale, *radius_clear, *opacity_scale;
	GtkWidget *radius_label, *opacity_label;   /* "Corners 12 px", "Opacity 85%" */
	GtkWidget *theme_entry;          /* name for "Save as theme" */
	GtkStringList *theme_names;      /* what theme_dd shows */
	GPtrArray *themes;               /* FhiThemeInfo*, same order as theme_names (after "Default") */
	char *theme_id;                  /* the theme chosen in theme_dd, NULL = none */
	gboolean syncing;                /* TRUE while we set widgets ourselves: their signals are ignored */
	guint save_timer;                /* pending save_settings() after a slider moved */
	GtkCssProvider *user_css;        /* ~/.config/fhi/user.css, loaded last */
	GFileMonitor *user_css_mon;      /* reloads it when it is saved */

	/* --- sorting and filtering --- */
	GtkSorter *sorter;
	FhiFilter *filter;               /* from the filter box, NULL = show everything */
	GtkWidget *filter_entry;
	GtkWidget *filter_bar;           /* holds filter_entry; hidden until you type */
	GtkWidget *sort_label;           /* faint "Size ↓" text, only shown when not the default sort */
	GtkWidget *sort_dd;              /* sorting page widgets, so the keyboard can drive them */
	GtkWidget *reverse_switch;
} State;

static void tab_close_later(State *st, GtkWidget *terminal);

static void
child_ready(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
	State *st = user_data;
	if (error) {
		g_printerr("failed to spawn shell: %s\n", error->message);
		g_clear_error(&error);
	}
	if (pid == -1) {
		tab_close_later(st, GTK_WIDGET(terminal));
		return;
	}
	/* remember zsh's PID (see shell_is_foreground()); every tab has its own */
	g_object_set_data(G_OBJECT(terminal), "fhi-shell-pid", GINT_TO_POINTER(pid));
	if (GTK_WIDGET(terminal) == st->terminal) st->shell_pid = pid;
}

/* The shell of a tab ended (`exit`, Ctrl+D): close that tab, or the window if it was the last */
static void
child_exited(VteTerminal *terminal, int status, gpointer user_data)
{
	tab_close_later(user_data, GTK_WIDGET(terminal));
}


/* What we ask the file system about every file (the list and peeked folders) */
#define FHI_ATTRS "standard::name,standard::display-name,standard::icon,standard::type,standard::size,time::modified,time::created"

/* ======================================================================== */
/* Folder peeking                                                             */
/*                                                                            */
/* Right arrow opens the highlighted folder INSIDE the list, indented, without */
/* changing directory; Left arrow closes it. The opened rows live in their own */
/* model (st->peeks) next to the real files. Each peeked row carries extra     */
/* attributes: "fhi::depth" (indent level), "fhi::parent" (path of the folder  */
/* it is in) and "fhi::path" (its own full path).                             */
/* ======================================================================== */

static guint
row_depth(GFileInfo *info)
{
	return g_file_info_has_attribute(info, "fhi::depth")
	       ? g_file_info_get_attribute_uint32(info, "fhi::depth") : 0;
}

/* Full path of a row (new string, free it), NULL for ghosts */
static char *
row_path(GFileInfo *info)
{
	const char *p = g_file_info_get_attribute_string(info, "fhi::path");
	if (p) return g_strdup(p);
	if (g_file_info_get_attribute_boolean(info, "fhi::ghost")) return NULL;
	GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
	return file ? g_file_get_path(file) : NULL;
}

/* Name that tells two siblings apart: the plain name at top level, the full path inside a peeked folder */
static const char *
row_key(GFileInfo *info)
{
	const char *p = g_file_info_get_attribute_string(info, "fhi::path");
	return p ? p : g_file_info_get_name(info);
}

#define PEEK_MAX_DEPTH 24

/* The folders a row sits in, outermost first, ending with the row itself. Returns the length. */
static int
row_chain(State *st, GFileInfo *row, GFileInfo **chain)
{
	GFileInfo *tmp[PEEK_MAX_DEPTH];
	int n = 0;
	GFileInfo *cur = row;
	while (cur != NULL && n < PEEK_MAX_DEPTH) {
		tmp[n++] = cur;
		const char *parent = g_file_info_get_attribute_string(cur, "fhi::parent");
		if (parent == NULL) break;
		cur = g_hash_table_lookup(st->peek_info, parent);
	}
	for (int i = 0; i < n; i++) chain[i] = tmp[n - 1 - i];
	return n;
}

/* The CSS classes used to color a row, indexed by FhiRole (see style in activate) */
static const char *const role_class[] = {
	NULL, "fhi-touch", "fhi-create", "fhi-dest", "fhi-remove"
};

/* A file an AI assistant just created or edited; the row stays lit for a few seconds */
typedef struct {
	gint64 time;        /* g_get_monotonic_time() of the last change */
	gboolean created;
} LlmAct;

#define LLM_LIT_SECONDS 12

/* Path of the folder the list shows (new string, free it), NULL if unknown */
static char *
list_dir_path(State *st)
{
	GFile *dir = gtk_directory_list_get_file(st->dir_list);   /* not ours */
	return dir ? g_file_get_path(dir) : NULL;
}

/* The assistant's activity on the file a row shows, NULL if none. */
static const LlmAct *
row_llm_act(State *st, GtkWidget *box)
{
	if (g_hash_table_size(st->llm_act) == 0) return NULL;
	const char *name = g_object_get_data(G_OBJECT(box), "fhi-name");
	if (name) return g_hash_table_lookup(st->llm_act, name);

	/* a row inside a peeked folder: its key is the path below the list's folder */
	const char *path = g_object_get_data(G_OBJECT(box), "fhi-path");
	if (path == NULL) return NULL;
	const LlmAct *act = NULL;
	char *dir = list_dir_path(st);
	if (dir && g_str_has_prefix(path, dir) && path[strlen(dir)] == '/')
		act = g_hash_table_lookup(st->llm_act, path + strlen(dir) + 1);
	g_free(dir);
	return act;
}

/* Give one row the color that matches what the typed command does to its file. */
static void
apply_row_style(State *st, GtkWidget *box)
{
	for (guint i = 1; i < G_N_ELEMENTS(role_class); i++)
		gtk_widget_remove_css_class(box, role_class[i]);
	gtk_widget_remove_css_class(box, "fhi-llm-new");
	gtk_widget_remove_css_class(box, "fhi-llm-mod");

	const char *name = g_object_get_data(G_OBJECT(box), "fhi-name");
	FhiRole role = name ? GPOINTER_TO_INT(g_hash_table_lookup(st->roles, name)) : FHI_ROLE_NONE;
	if (role != FHI_ROLE_NONE) {   /* what the typed command does wins over the assistant's work */
		gtk_widget_add_css_class(box, role_class[role]);
		return;
	}
	const LlmAct *act = row_llm_act(st, box);
	if (act) gtk_widget_add_css_class(box, act->created ? "fhi-llm-new" : "fhi-llm-mod");
}

static void
refresh_all_rows(State *st)
{
	GHashTableIter it;
	gpointer box;
	g_hash_table_iter_init(&it, st->bound_rows);
	while (g_hash_table_iter_next(&it, &box, NULL)) apply_row_style(st, box);
}

/*
 * Dragging a row out of the window (to a browser, Discord, a file manager...).
 * We offer the file as a GdkFileList, which GTK turns into the standard
 * "text/uri-list" other apps understand. The row widget is reused for different
 * files, so we look up which file it shows at the moment the drag starts.
 */
static GdkContentProvider *
on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data)
{
	GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	GFileInfo *info = g_object_get_data(G_OBJECT(box), "fhi-info");
	if (info == NULL || g_file_info_get_attribute_boolean(info, "fhi::ghost")) return NULL;   /* nothing to drag */
	GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
	if (file == NULL) return NULL;

	GFile *files[1] = { file };
	GdkFileList *list = gdk_file_list_new_from_array(files, 1);
	GdkContentProvider *provider = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, list);
	g_object_unref(list);
	return provider;
}

/* While dragging, the pointer carries a picture of the row */
static void
on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
	GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	GdkPaintable *paintable = gtk_widget_paintable_new(box);
	gtk_drag_source_set_icon(source, paintable, 0, 0);
	g_object_unref(paintable);
}

/* "setup": GTK needs an empty row. Called once per row WIDGET (they get reused). */
static void
setup_item(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *icon = gtk_label_new(NULL);   /* a Nerd Font glyph, see filetype.c */
	GtkWidget *label = gtk_label_new(NULL);
	gtk_widget_add_css_class(icon, "fhi-icon");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(box, "fhi-row");
	gtk_box_append(GTK_BOX(box), icon);
	gtk_box_append(GTK_BOX(box), label);

	GtkDragSource *drag = gtk_drag_source_new();
	gtk_drag_source_set_actions(drag, GDK_ACTION_COPY);   /* never move or delete the original */
	g_signal_connect(drag, "prepare", G_CALLBACK(on_drag_prepare), NULL);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));

	gtk_list_item_set_child(item, box);
}

/* (Re)draw the icon of a bound row: Nerd Font glyph, "open folder" while the folder is peeked open */
static void
update_row_icon(State *st, GtkWidget *box)
{
	GFileInfo *info = g_object_get_data(G_OBJECT(box), "fhi-info");
	GtkWidget *icon = gtk_widget_get_first_child(box);
	if (info == NULL) return;
	gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
	const char *color;
	const char *glyph = fhi_file_icon(g_file_info_get_name(info), is_dir, &color);
	if (is_dir && g_hash_table_size(st->expanded) > 0) {
		char *path = row_path(info);
		if (path && g_hash_table_contains(st->expanded, path)) glyph = "\uf07c";   /* open folder */
		g_free(path);
	}
	char *markup = g_markup_printf_escaped("<span foreground=\"%s\">%s</span>", color, glyph);
	gtk_label_set_markup(GTK_LABEL(icon), markup);
	g_free(markup);
}

/* "bind": a row widget now has to show this particular file. */
static void
bind_item(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	State *st = user_data;
	GFileInfo *info = G_FILE_INFO(gtk_list_item_get_item(item));
	GtkWidget *box = gtk_list_item_get_child(item);
	GtkWidget *label = gtk_widget_get_last_child(box);

	gboolean peeked = g_file_info_has_attribute(info, "fhi::depth");
	g_object_set_data(G_OBJECT(box), "fhi-info", info);   /* borrowed: valid while the row is bound */
	update_row_icon(st, box);
	gtk_widget_set_margin_start(box, row_depth(info) * 18);
	gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));

	/* Remember which file this row shows, and keep track of the row, so that
	 * update_preview() can recolor it later without GTK asking again. */
	g_object_set_data_full(G_OBJECT(box), "fhi-name",
	                       peeked ? NULL : g_strdup(g_file_info_get_name(info)), g_free);   /* peeked rows are never highlighted */
	g_object_set_data_full(G_OBJECT(box), "fhi-path", peeked ? row_path(info) : NULL, g_free);
	g_hash_table_add(st->bound_rows, box);

	if (g_file_info_get_attribute_boolean(info, "fhi::ghost"))
		gtk_widget_add_css_class(box, "fhi-ghost");
	else
		gtk_widget_remove_css_class(box, "fhi-ghost");

	apply_row_style(st, box);
}

/* "unbind": the row widget is about to be reused for another file (or destroyed) */
static void
unbind_item(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	State *st = user_data;
	g_hash_table_remove(st->bound_rows, gtk_list_item_get_child(item));
}

/* The part of the name after the last dot ("" if none; a leading dot is no extension) */
static const char *
extension_of(const char *name)
{
	const char *dot = strrchr(name, '.');
	return (dot && dot != name) ? dot + 1 : "";
}

static int
cmp_u64(guint64 a, guint64 b)
{
	return a < b ? -1 : a > b;
}

/* Order two files that sit in the same folder, by the chosen sort mode */
static int
compare_siblings(State *st, GFileInfo *a, GFileInfo *b)
{
	const Settings *c = &st->cfg;

	if (c->folders_first) {
		gboolean da = g_file_info_get_file_type(a) == G_FILE_TYPE_DIRECTORY;
		gboolean db = g_file_info_get_file_type(b) == G_FILE_TYPE_DIRECTORY;
		if (da != db) return da ? -1 : 1;   /* not flipped by "reverse" */
	}

	int r = 0;
	switch (c->sort_mode) {
	case SORT_SIZE:     /* biggest first */
		r = -cmp_u64(g_file_info_get_size(a), g_file_info_get_size(b));
		break;
	case SORT_MODIFIED: /* newest first */
		r = -cmp_u64(g_file_info_get_attribute_uint64(a, "time::modified"),
		             g_file_info_get_attribute_uint64(b, "time::modified"));
		break;
	case SORT_CREATED:
		r = -cmp_u64(g_file_info_get_attribute_uint64(a, "time::created"),
		             g_file_info_get_attribute_uint64(b, "time::created"));
		break;
	case SORT_TYPE:
		r = strcasecmp(extension_of(g_file_info_get_name(a)), extension_of(g_file_info_get_name(b)));
		break;
	default:
		break;
	}
	if (r == 0)   /* same size / date / type: fall back to the name */
		r = g_utf8_collate(g_file_info_get_display_name(a), g_file_info_get_display_name(b));
	return c->sort_reverse ? -r : r;
}

/*
 * The sorter of the whole list. A peeked row must stay right below its folder, so
 * we compare the two "paths" folder by folder: the first place where they differ
 * is a pair of siblings, which compare_siblings() orders. If one path is just the
 * start of the other, the shorter one (the folder itself) comes first.
 */
static int
compare_info(gconstpointer pa, gconstpointer pb, gpointer user_data)
{
	State *st = user_data;
	GFileInfo *a = G_FILE_INFO((gpointer)pa), *b = G_FILE_INFO((gpointer)pb);
	GFileInfo *ca[PEEK_MAX_DEPTH], *cb[PEEK_MAX_DEPTH];
	int na = row_chain(st, a, ca), nb = row_chain(st, b, cb);

	for (int i = 0; i < na && i < nb; i++) {
		if (ca[i] == cb[i]) continue;
		if (g_strcmp0(row_key(ca[i]), row_key(cb[i])) != 0)
			return compare_siblings(st, ca[i], cb[i]);
	}
	return na < nb ? -1 : na > nb;
}

/* A row for a file that does not exist yet but the typed command would create. */
static GFileInfo *
make_ghost(GFile *dir, const char *name)
{
	GFileInfo *info = g_file_info_new();
	GIcon *icon = g_themed_icon_new("text-x-generic");
	GFile *file = g_file_get_child(dir, name);

	g_file_info_set_name(info, name);
	g_file_info_set_display_name(info, name);
	g_file_info_set_icon(info, icon);
	g_file_info_set_attribute_object(info, "standard::file", G_OBJECT(file));
	g_file_info_set_attribute_boolean(info, "fhi::ghost", TRUE);

	g_object_unref(icon);
	g_object_unref(file);
	return info;
}

/* Mark the files of `files` (takes the reference) with `role` in the preview result */
static void
add_git_hits(GPtrArray *hits, GPtrArray *files, FhiRole role)
{
	for (guint i = 0; i < files->len; i++) {
		const char *name = files->pdata[i];
		FhiHit *hit = NULL;
		for (guint k = 0; k < hits->len && !hit; k++)
			if (strcmp(((FhiHit *)hits->pdata[k])->name, name) == 0) hit = hits->pdata[k];
		if (hit == NULL) {
			hit = g_new0(FhiHit, 1);
			hit->name = g_strdup(name);
			hit->exists = TRUE;
			g_ptr_array_add(hits, hit);
		}
		if (role > hit->role) hit->role = role;
	}
	g_ptr_array_unref(files);
}

/*
 * Re-read the command line `buffer`, work out which files it affects (see
 * preview.c) and update the file list: color the real rows and add "ghost"
 * rows for the files that would be created.
 */
static void
update_preview(State *st, const char *buffer)
{
	/* copy first: `buffer` may be st->last_buffer itself */
	char *copy = g_strdup(buffer);
	g_free(st->last_buffer);
	st->last_buffer = copy;

	GFile *dir = gtk_directory_list_get_file(st->dir_list);   /* not ours */
	if (dir == NULL) return;
	char *cwd = g_file_get_path(dir);
	if (cwd == NULL) return;

	/* names of the files currently listed, as a NULL-terminated array */
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	GListModel *model = G_LIST_MODEL(st->dir_list);
	guint n = g_list_model_get_n_items(model);
	for (guint i = 0; i < n; i++) {
		GFileInfo *info = g_list_model_get_item(model, i);   /* new reference */
		g_ptr_array_add(names, g_strdup(g_file_info_get_name(info)));
		g_object_unref(info);
	}
	g_ptr_array_add(names, NULL);

	FhiGitAction git;
	GPtrArray *hits = fhi_preview_parse_ex(copy, cwd, (const char *const *)names->pdata, &git);

	/* `git commit` / `git push` do not name their files: ask git (see gitinfo.c) */
	if (git.commit && !git.has_paths)
		add_git_hits(hits, fhi_git_files(cwd, git.commit_all ? FHI_GIT_TRACKED : FHI_GIT_STAGED), FHI_ROLE_CREATE);
	if (git.push)
		add_git_hits(hits, fhi_git_files(cwd, FHI_GIT_UNPUSHED), FHI_ROLE_DEST);

	g_hash_table_remove_all(st->roles);
	GString *key = g_string_new(NULL);
	for (guint i = 0; i < hits->len; i++) {
		FhiHit *hit = hits->pdata[i];
		g_hash_table_insert(st->roles, g_strdup(hit->name), GINT_TO_POINTER(hit->role));
		if (!hit->exists && g_utf8_validate(hit->name, -1, NULL))
			g_string_append_printf(key, "%s\n", hit->name);
	}

	/* Only touch the model when the set of ghosts changed, otherwise the list
	 * would be rebuilt (and flicker) on every single keystroke. */
	if (g_strcmp0(key->str, st->ghost_key) != 0) {
		g_list_store_remove_all(st->ghosts);
		for (guint i = 0; i < hits->len; i++) {
			FhiHit *hit = hits->pdata[i];
			if (hit->exists || !g_utf8_validate(hit->name, -1, NULL)) continue;
			GFileInfo *ghost = make_ghost(dir, hit->name);
			g_list_store_append(st->ghosts, ghost);
			g_object_unref(ghost);   /* the store keeps its own reference */
		}
		g_free(st->ghost_key);
		st->ghost_key = g_strdup(key->str);
	}

	refresh_all_rows(st);

	g_string_free(key, TRUE);
	g_ptr_array_unref(hits);
	g_ptr_array_unref(names);
	g_free(cwd);
}

/*
 * Safety net: make sure the file list matches what is really on disk.
 *
 * GtkDirectoryList normally keeps itself up to date by watching the folder, but
 * that can miss changes (many files changed at once, for example). So after
 * every command we compare the names in the folder with the names in the list,
 * and if they differ we reload the list, which is the same thing that
 * happens when you change directory.
 */
static gboolean
resync_directory(gpointer user_data)
{
	State *st = user_data;
	st->resync_timer = 0;

	GFile *dir = gtk_directory_list_get_file(st->dir_list);   /* not ours */
	if (dir == NULL || gtk_directory_list_is_loading(st->dir_list)) return G_SOURCE_REMOVE;
	char *path = g_file_get_path(dir);
	GDir *handle = path ? g_dir_open(path, 0, NULL) : NULL;
	g_free(path);
	if (handle == NULL) return G_SOURCE_REMOVE;

	/* every name that is really in the folder (g_dir_read_name skips "." and "..") */
	GHashTable *on_disk = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	const char *name;
	while ((name = g_dir_read_name(handle)) != NULL) g_hash_table_add(on_disk, g_strdup(name));
	g_dir_close(handle);

	/* the same number of names, and every listed name exists on disk? */
	GListModel *model = G_LIST_MODEL(st->dir_list);
	guint n = g_list_model_get_n_items(model);
	gboolean same = (n == g_hash_table_size(on_disk));
	for (guint i = 0; same && i < n; i++) {
		GFileInfo *info = g_list_model_get_item(model, i);   /* new reference */
		same = g_hash_table_contains(on_disk, g_file_info_get_name(info));
		g_object_unref(info);
	}
	g_hash_table_unref(on_disk);

	if (!same) {
		/* Reload. `dir` belongs to the list, which lets go of it on set_file(NULL),
		 * so take our own reference first. */
		g_object_ref(dir);
		gtk_directory_list_set_file(st->dir_list, NULL);
		gtk_directory_list_set_file(st->dir_list, dir);
		g_object_unref(dir);
	}
	return G_SOURCE_REMOVE;
}

/*
 * zsh (see zsh/.zshrc) puts the command line being typed into the terminal
 * title as "FHI-BUF:<text>". VTE reports each title change with this signal.
 */
static void
on_title_changed(VteTerminal *terminal, gpointer user_data)
{
	State *st = user_data;
	if (GTK_WIDGET(terminal) != st->terminal) return;   /* a tab in the background */
	const char *title = vte_terminal_get_window_title(terminal);
	if (title == NULL) return;

	/* "FHI-PROMPT:n" is sent by zsh each time a command has finished and the
	 * prompt is back. Wait a moment, so GTK can finish its own file watching
	 * first, then check that the list is right. */
	if (g_str_has_prefix(title, "FHI-PROMPT:")) {
		fhi_git_invalidate();   /* the command may have staged, committed or pushed something */
		if (st->resync_timer == 0) st->resync_timer = g_timeout_add(300, resync_directory, st);
		return;
	}

	if (!g_str_has_prefix(title, "FHI-BUF:")) return;

	char *buffer = g_uri_unescape_string(title + strlen("FHI-BUF:"), NULL);   /* undo %0A etc. */
	if (buffer == NULL) return;
	update_preview(st, buffer);
	g_free(buffer);
}

static void peek_refresh(State *st);
static void peek_clear_all(State *st);
static gboolean list_has_focus(State *st);

/* Runs once GTK is idle, i.e. after it has finished handling the file change. */
static gboolean
preview_idle_cb(gpointer user_data)
{
	State *st = user_data;
	st->preview_idle = 0;
	peek_refresh(st);
	update_preview(st, st->last_buffer ? st->last_buffer : "");
	return G_SOURCE_REMOVE;   /* run only once */
}

/*
 * The list of files itself changed (new folder, or a file was created/deleted):
 * run the preview again so ghosts and colors match the new situation.
 *
 * We must NOT call update_preview() right here. This signal is emitted while
 * GTK's list models are in the middle of processing the change, and update_preview()
 * edits another of those models (the ghost rows): doing that now crashes GTK.
 * g_idle_add() asks the main loop to call us back a moment later instead, and
 * several changes in a row only schedule one update.
 */
static void
on_dir_items_changed(GListModel *model, guint position, guint removed, guint added, gpointer user_data)
{
	State *st = user_data;
	if (st->preview_idle == 0) st->preview_idle = g_idle_add(preview_idle_cb, st);
}

static gboolean tmux_mode(State *st);

/* Tab title: the folder's name ("~" for the home folder) */
static void
tab_set_title(State *st, GtkWidget *terminal, const char *path)
{
	if (st->notebook == NULL) return;
	char *base = NULL;
	if (path == NULL) base = g_strdup("shell");
	else if (strcmp(path, g_get_home_dir()) == 0) base = g_strdup("~");
	else if (strcmp(path, "/") == 0) base = g_strdup("/");
	else base = g_path_get_basename(path);
	gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(st->notebook), terminal, base);
	g_free(base);
}

/* Make the file list show folder `file` (does nothing if it already does) */
static void
sidebar_set_dir(State *st, GFile *file)
{
	GtkDirectoryList *list = st->dir_list;
	/* Only reload the list when the directory really is a different one. */
	GFile *current = gtk_directory_list_get_file(list); /* not ours: do not unref */
	if (current == NULL || !g_file_equal(current, file)) {
		/* The focused row is thrown away with the old list, and GTK would then
		 * hand the focus to the terminal. Take it back afterwards. */
		gboolean had_focus = list_has_focus(st);
		peek_clear_all(st);
		gtk_directory_list_set_file(list, file);
		if (had_focus) gtk_widget_grab_focus(st->list_view);
	}
}

static void
on_cwd_changed(VteTerminal *terminal, gpointer user_data)
{
	State *st = user_data;

	/* VTE owns this string, so we must not free it. NULL means "unknown". */
	const char *uri = vte_terminal_get_current_directory_uri(terminal);
	if (uri == NULL) return;

	/* g_file_new_for_uri() decodes "file://" and the %20-style escapes for us */
	GFile *file = g_file_new_for_uri(uri);
	char *path = g_file_get_path(file);
	tab_set_title(st, GTK_WIDGET(terminal), path);
	g_free(path);

	/* The signal fires before every prompt, even if we did not change folder. Only the tab
	 * in front drives the file list; inside tmux the focused pane does (see on_tmux_pane). */
	if (GTK_WIDGET(terminal) == st->terminal && !tmux_mode(st)) sidebar_set_dir(st, file);

	/* We created "file", so we release it. The list took its own reference. */
	g_object_unref(file);
}

/*
 * Is zsh itself waiting for a command right now?
 *
 * A terminal can be running something else in front of the shell, like vim or
 * a long program. In that case we must NOT type "cd ..." or a path into it,
 * because that text would land inside vim.
 *
 * How it works: every terminal has one "foreground process group", the
 * program that currently receives the keyboard. tcgetpgrp() asks the pty
 * (the pseudo-terminal VTE created) who that is. When zsh is sitting at its
 * prompt it is the foreground program, and its process group id equals its
 * PID. While vim runs, the foreground group is vim's, so the numbers differ.
 */
/* Name of the program in front of the terminal (new string, free it), NULL if unknown */
/* Process id of the program in front of the terminal (0 if unknown) */
static pid_t
vte_fg_pid(State *st)   /* what the terminal itself has in front: tmux, not the program inside tmux */
{
	VtePty *pty = vte_terminal_get_pty(VTE_TERMINAL(st->terminal));
	if (pty == NULL || st->shell_pid <= 0) return 0;
	pid_t pgid = tcgetpgrp(vte_pty_get_fd(pty));
	return pgid > 0 ? pgid : 0;
}

/* tmux is in front and we know which pane has focus */
static gboolean
tmux_mode(State *st)
{
	return st->tmux_tty != NULL && st->tmux_client > 0 && st->tmux_client == vte_fg_pid(st);
}

/* The program that receives the keyboard: inside tmux, the one in the focused pane */
static pid_t
foreground_pid(State *st)
{
	pid_t pid = vte_fg_pid(st);
	if (pid > 0 && st->tmux_tty != NULL && pid == st->tmux_client) {
		pid_t inner = fhi_tmux_tty_foreground(st->tmux_tty);
		if (inner > 0) return inner;
	}
	return pid;
}

/* The shell to compare the foreground with: the tab's, or the focused tmux pane's */
static pid_t
effective_shell_pid(State *st)
{
	return tmux_mode(st) ? st->tmux_pane_pid : st->shell_pid;
}

static char *
foreground_comm(State *st)
{
	pid_t pgid = foreground_pid(st);
	if (pgid <= 0) return NULL;
	/* The leader of the foreground process group is the program itself; its name is in /proc */
	char *file = g_strdup_printf("/proc/%d/comm", (int)pgid);
	char *comm = NULL;
	g_file_get_contents(file, &comm, NULL, NULL);
	g_free(file);
	if (comm) g_strstrip(comm);
	return comm;
}

/* Other shells: FHI only understands zsh (its hooks live in zsh/.zshrc) */
static gboolean
is_unsupported_shell(const char *comm)
{
	static const char *const shells[] = { "bash", "sh", "dash", "fish", "ksh", "csh", "tcsh" };
	for (guint i = 0; i < G_N_ELEMENTS(shells); i++)
		if (strcmp(comm, shells[i]) == 0) return TRUE;
	return FALSE;
}

static gboolean
shell_is_foreground(State *st)
{
	pid_t shell = effective_shell_pid(st);
	if (shell <= 0 || foreground_pid(st) != shell) return FALSE;
	/* a tmux pane whose shell was not started by FHI would type our "push-line" key into the void */
	if (tmux_mode(st) && !st->tmux_fhi) return FALSE;
	return st->unsupported_shell == NULL;   /* `exec bash` keeps the pid, so look at the name too */
}

/* Send text to the shell exactly as if the user had typed it. */
static void
type_into_terminal(State *st, const char *text)
{
	vte_terminal_feed_child(VTE_TERMINAL(st->terminal), text, -1);
}

static void
refresh_row_icons(State *st)
{
	GHashTableIter it;
	gpointer box;
	g_hash_table_iter_init(&it, st->bound_rows);
	while (g_hash_table_iter_next(&it, &box, NULL)) update_row_icon(st, box);
}

static gboolean
list_has_focus(State *st)
{
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(st->window));
	return focus != NULL && (focus == st->list_view || gtk_widget_is_ancestor(focus, st->list_view));
}

/* Close every peeked folder (used when the directory changes) */
static void
peek_clear_all(State *st)
{
	g_hash_table_remove_all(st->expanded);
	g_hash_table_remove_all(st->peek_info);
	g_list_store_remove_all(st->peeks);
}

static void
peek_open(State *st, GFileInfo *info)
{
	if (g_file_info_get_attribute_boolean(info, "fhi::ghost")) return;
	GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
	char *path = row_path(info);
	guint depth = row_depth(info) + 1;
	if (file == NULL || path == NULL || depth >= PEEK_MAX_DEPTH || g_hash_table_contains(st->expanded, path) ||
	    g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
		g_free(path);
		return;
	}
	GFileEnumerator *en = g_file_enumerate_children(file, FHI_ATTRS, G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (en == NULL) {   /* no permission, for example */
		g_free(path);
		return;
	}

	GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
	GFileInfo *child;
	while (rows->len < 2000 && (child = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
		GFile *cf = g_file_get_child(file, g_file_info_get_name(child));
		char *cp = g_file_get_path(cf);
		if (cp == NULL) {
			g_object_unref(child);
			g_object_unref(cf);
			continue;
		}
		g_file_info_set_attribute_object(child, "standard::file", G_OBJECT(cf));
		g_file_info_set_attribute_uint32(child, "fhi::depth", depth);
		g_file_info_set_attribute_string(child, "fhi::parent", path);
		g_file_info_set_attribute_string(child, "fhi::path", cp);
		g_free(cp);
		g_object_unref(cf);
		g_ptr_array_add(rows, child);
	}
	g_object_unref(en);

	/* the sorter looks the folder's own row up here, so register it BEFORE adding the children */
	g_hash_table_replace(st->peek_info, g_strdup(path), g_object_ref(info));
	g_hash_table_add(st->expanded, path);   /* the set keeps `path` */
	g_list_store_splice(st->peeks, g_list_model_get_n_items(G_LIST_MODEL(st->peeks)), 0,
	                    rows->pdata, rows->len);
	g_ptr_array_unref(rows);
	refresh_row_icons(st);   /* the folder icon changes to "open" */
}

static void
peek_close(State *st, const char *path_in)
{
	char *path = g_strdup(path_in);
	char *prefix = g_strdup_printf("%s/", path);
	GHashTable *tables[2] = { st->expanded, st->peek_info };
	for (int t = 0; t < 2; t++) {
		GHashTableIter it;
		gpointer key;
		g_hash_table_iter_init(&it, tables[t]);
		while (g_hash_table_iter_next(&it, &key, NULL))
			if (g_str_has_prefix(key, prefix) || strcmp(key, path) == 0) g_hash_table_iter_remove(&it);
	}
	for (guint i = g_list_model_get_n_items(G_LIST_MODEL(st->peeks)); i-- > 0; ) {
		GFileInfo *row = g_list_model_get_item(G_LIST_MODEL(st->peeks), i);
		const char *p = g_file_info_get_attribute_string(row, "fhi::path");
		if (p && g_str_has_prefix(p, prefix)) g_list_store_remove(st->peeks, i);
		g_object_unref(row);
	}
	g_free(prefix);
	g_free(path);
	refresh_row_icons(st);
}

/* Highlight the row at `pos` and make sure it is on screen */
static void
select_row(State *st, guint pos)
{
	gtk_single_selection_set_selected(st->selection, pos);
#if GTK_CHECK_VERSION(4, 10, 0)
	gtk_widget_activate_action(st->list_view, "list.scroll-to-item", "u", pos);
#endif
}

/*
 * Right arrow: open the highlighted folder (or step into it when already open).
 * Left arrow:  close it (or, on a file inside a peeked folder, jump to that folder).
 */
static void
peek_key(State *st, gboolean open)
{
	GFileInfo *info = gtk_single_selection_get_selected_item(st->selection);   /* not ours */
	if (info == NULL) return;
	char *path = row_path(info);
	if (path == NULL) return;   /* ghost */
	gboolean is_open = g_hash_table_contains(st->expanded, path);
	guint pos = gtk_single_selection_get_selected(st->selection);
	guint n = g_list_model_get_n_items(G_LIST_MODEL(st->selection));

	if (open) {
		if (!is_open) peek_open(st, info);
		else if (pos + 1 < n) select_row(st, pos + 1);
	} else if (is_open) {
		peek_close(st, path);
	} else if (row_depth(info) > 0) {
		const char *parent = g_file_info_get_attribute_string(info, "fhi::parent");
		for (guint i = 0; parent && i < pos; i++) {
			GFileInfo *row = g_list_model_get_item(G_LIST_MODEL(st->selection), i);
			char *rp = row_path(row);
			gboolean hit = g_strcmp0(rp, parent) == 0;
			g_free(rp);
			g_object_unref(row);
			if (hit) {
				select_row(st, i);
				break;
			}
		}
	}
	g_free(path);
}

/*
 * After the files of the current folder were (re)loaded: point the sorter at the new
 * rows of the opened folders, and close peeks whose folder has disappeared.
 */
static void
peek_refresh(State *st)
{
	if (g_hash_table_size(st->expanded) == 0) return;
	GFile *dir = gtk_directory_list_get_file(st->dir_list);
	if (dir == NULL || gtk_directory_list_is_loading(st->dir_list)) return;
	char *cwd = g_file_get_path(dir);
	if (cwd == NULL) return;

	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GListModel *model = G_LIST_MODEL(st->dir_list);
	guint n = g_list_model_get_n_items(model);
	for (guint i = 0; i < n; i++) {
		GFileInfo *info = g_list_model_get_item(model, i);
		char *path = g_build_filename(cwd, g_file_info_get_name(info), NULL);
		if (g_hash_table_contains(st->expanded, path)) {
			g_hash_table_replace(st->peek_info, g_strdup(path), g_object_ref(info));
			g_hash_table_add(seen, path);
		} else {
			g_free(path);
		}
		g_object_unref(info);
	}

	/* opened folders directly inside cwd that are gone now */
	GPtrArray *gone = g_ptr_array_new_with_free_func(g_free);
	char *prefix = g_strdup_printf("%s/", cwd);
	GHashTableIter it;
	gpointer key;
	g_hash_table_iter_init(&it, st->expanded);
	while (g_hash_table_iter_next(&it, &key, NULL))
		if (g_str_has_prefix(key, prefix) && strchr((char *)key + strlen(prefix), '/') == NULL &&
		    !g_hash_table_contains(seen, key))
			g_ptr_array_add(gone, g_strdup(key));
	for (guint i = 0; i < gone->len; i++) peek_close(st, gone->pdata[i]);

	gtk_sorter_changed(st->sorter, GTK_SORTER_CHANGE_DIFFERENT);
	g_ptr_array_unref(gone);
	g_hash_table_unref(seen);
	g_free(prefix);
	g_free(cwd);
}

static int
cmp_path_len(gconstpointer a, gconstpointer b)
{
	return (int)strlen(*(char *const *)a) - (int)strlen(*(char *const *)b);
}

/*
 * Read the opened ("peeked") folders again from disk. peek_open() lists a folder once, so
 * files that appear inside one later (an assistant writing src/new.c while src/ is open)
 * would not show up. Outer folders are reopened first, so nesting survives.
 */
static void
peek_rescan(State *st)
{
	if (g_hash_table_size(st->expanded) == 0) return;

	GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
	GHashTable *infos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	GHashTableIter it;
	gpointer key, value;
	g_hash_table_iter_init(&it, st->peek_info);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		g_ptr_array_add(paths, g_strdup(key));
		g_hash_table_insert(infos, g_strdup(key), g_object_ref(value));
	}
	g_ptr_array_sort(paths, cmp_path_len);

	/* keep the highlighted row on the same file */
	GFileInfo *sel = gtk_single_selection_get_selected_item(st->selection);
	char *sel_path = sel ? row_path(sel) : NULL;
	gboolean had_focus = list_has_focus(st);

	peek_clear_all(st);
	for (guint i = 0; i < paths->len; i++) {
		GFileInfo *info = g_hash_table_lookup(infos, paths->pdata[i]);
		if (info) peek_open(st, info);
	}

	if (sel_path) {
		GListModel *m = G_LIST_MODEL(st->selection);
		guint n = g_list_model_get_n_items(m);
		for (guint i = 0; i < n; i++) {
			GFileInfo *row = g_list_model_get_item(m, i);
			char *rp = row_path(row);
			gboolean hit = g_strcmp0(rp, sel_path) == 0;
			g_free(rp);
			g_object_unref(row);
			if (hit) {
				select_row(st, i);
				break;
			}
		}
	}
	if (had_focus) gtk_widget_grab_focus(st->list_view);
	g_free(sel_path);
	g_hash_table_unref(infos);
	g_ptr_array_unref(paths);
}

/*
 * Run `line` (a command ending in \r) in the shell without destroying what the
 * user has typed: zsh/.zshrc binds ESC [ 7000 ~ to "push-line", which puts the
 * half-typed command aside and brings it back at the next prompt. Focus stays
 * where it was (the file list, normally).
 */
static void
run_in_shell(State *st, const char *line)
{
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(st->window));
	type_into_terminal(st, "\033[7000~");
	type_into_terminal(st, line);
	if (focus != NULL) gtk_widget_grab_focus(focus);
}

/* Who is in front of the terminal right now? */
typedef enum { FG_OTHER, FG_SHELL, FG_VIM, FG_NANO, FG_PYTHON, FG_LLM } Foreground;
static Foreground foreground_kind(State *st);

/* Is this a file we would edit in a text editor? (source code, config, notes...) */
static gboolean
is_text_file(GFile *file)
{
	GFileInfo *fi = g_file_query_info(file, "standard::content-type,standard::type", 0, NULL, NULL);
	if (fi == NULL) return FALSE;
	gboolean regular = g_file_info_get_file_type(fi) == G_FILE_TYPE_REGULAR;
	const char *ct = g_file_info_get_content_type(fi);
	gboolean text = regular && ct != NULL && g_content_type_is_a(ct, "text/plain");
	gboolean other_media = ct != NULL && (g_str_has_prefix(ct, "image/") || g_str_has_prefix(ct, "audio/") ||
	                                      g_str_has_prefix(ct, "video/") || g_str_has_prefix(ct, "font/"));
	g_object_unref(fi);
	if (text || !regular || other_media) return text;

	/* Not a known text type (a *.toml, a Makefile.in, a PKGBUILD...): text if the start has no NUL byte */
	gboolean ok = FALSE;
	GFileInputStream *in = g_file_read(file, NULL, NULL);
	if (in) {
		char buf[8192];
		gssize n = g_input_stream_read(G_INPUT_STREAM(in), buf, sizeof buf, NULL, NULL);
		ok = n >= 0 && memchr(buf, '\0', (gsize)n) == NULL;
		g_object_unref(in);
	}
	return ok;
}

/* `path` with the characters that are special on a vim command line escaped, like fnameescape() */
static char *
vim_escape(const char *path)
{
	GString *out = g_string_new(NULL);
	for (const char *c = path; *c; c++) {
		if (strchr(" \t*?[{`$\\%#'\"|!<", *c)) g_string_append_c(out, '\\');
		g_string_append_c(out, *c);
	}
	return g_string_free(out, FALSE);
}

/*
 * The editor (vim / nano) is in front and a text file was chosen in the list: save what
 * is being edited and open the chosen file in the same editor, as if you had typed it.
 * Returns FALSE if we can't do that safely (then the caller opens the file normally).
 *
 *   vim   Ctrl-\ Ctrl-N goes to normal mode from ANY mode (and from a half-typed ":").
 *         `:update | edit FILE`  writes only if there are changes. If writing fails (read-only
 *         file, no name) vim stops at the error and does NOT switch: nothing gets lost.
 *   nano  Ctrl-O Enter writes the buffer; Ctrl-R opens "insert file", Alt-F in that prompt
 *         means "into a new buffer" (switch between buffers with Alt-< and Alt->).
 */
static gboolean
editor_open_file(State *st, Foreground fg, const char *path)
{
	for (const char *c = path; *c; c++)
		if ((unsigned char)*c < 0x20 || *c == 0x7f) return FALSE;   /* would be typed as keys */

	GString *keys = g_string_new(NULL);
	if (fg == FG_VIM) {
		char *esc = vim_escape(path);
		g_string_append_printf(keys, "\034\016:update | edit %s\r", esc);
		g_free(esc);
	} else if (fg == FG_NANO) {
		g_string_append_printf(keys, "\017\r\022\033f%s\r", path);
	} else {
		g_string_free(keys, TRUE);
		return FALSE;
	}
	type_into_terminal(st, keys->str);
	g_string_free(keys, TRUE);
	gtk_widget_grab_focus(st->terminal);   /* you will want to keep typing in the editor */
	return TRUE;
}

/*
 * Enter (or double click) on a row of the file list.
 * GtkListView emits "activate" with the row number ("position").
 *   - directory -> make the terminal cd into it (the list follows by itself,
 *                  through on_cwd_changed, once the next prompt appears)
 *   - text file while vim / nano is open -> save, and continue in that file
 *   - file      -> open it with the default application
 */
static void
on_row_activated(GtkListView *view, guint position, gpointer user_data)
{
	State *st = user_data;

	/* get_item returns a new reference, so we unref it at the end */
	GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(st->selection), position);
	if (info == NULL) return;

	/* GtkDirectoryList stores the GFile of every entry in this attribute.
	 * The object belongs to "info", so we must not unref it. */
	GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));

	/* a "ghost" row is a file that does not exist yet: nothing to open */
	if (g_file_info_get_attribute_boolean(info, "fhi::ghost")) file = NULL;

	if (file != NULL) {
		/* Follows symlinks, so a link to a folder counts as a folder */
		if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY) {
			if (shell_is_foreground(st)) {
				char *path = g_file_get_path(file);
				char *quoted = g_shell_quote(path); /* 'my folder' is safe to type */
				/* "\r" is what the Enter key sends */
				char *text = g_strdup_printf("cd -- %s\r", quoted);
				run_in_shell(st, text);
				g_free(text);
				g_free(quoted);
				g_free(path);
			}
		} else {
			Foreground fg = foreground_kind(st);
			char *path = (fg == FG_VIM || fg == FG_NANO) && is_text_file(file) ? g_file_get_path(file) : NULL;
			if (path && editor_open_file(st, fg, path)) {
				/* done: the editor took over */
			} else {
				char *uri = g_file_get_uri(file);
				GError *error = NULL;
				if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
					g_printerr("could not open %s: %s\n", uri, error->message);
					g_clear_error(&error);
				}
				g_free(uri);
			}
			g_free(path);
		}
	}

	g_object_unref(info);
}

static Foreground
foreground_kind(State *st)
{
	pid_t pgid = foreground_pid(st);
	if (pgid <= 0) return FG_OTHER;
	if (pgid == effective_shell_pid(st)) return st->unsupported_shell ? FG_OTHER : FG_SHELL;

	char *comm = foreground_comm(st);
	if (comm == NULL) return FG_OTHER;

	Foreground kind = FG_OTHER;
	static const char *const vims[] = { "vim", "nvim", "vi", "view", "gvim", "vimx", "nvi", "ex" };
	for (guint i = 0; i < G_N_ELEMENTS(vims); i++)
		if (g_str_has_prefix(comm, vims[i]) && (comm[strlen(vims[i])] == '\0' || comm[strlen(vims[i])] == '.'))
			kind = FG_VIM;   /* also vim.basic, vim.tiny */
	if (strcmp(comm, "nano") == 0 || strcmp(comm, "rnano") == 0) kind = FG_NANO;
	if (g_str_has_prefix(comm, "python") || g_str_has_prefix(comm, "ipython") ||
	    g_str_has_prefix(comm, "bpython") || g_str_has_prefix(comm, "ptpython"))
		kind = FG_PYTHON;
	g_free(comm);
	if (kind == FG_OTHER && fhi_llm_detect(pgid)) kind = FG_LLM;   /* claude, codex, aider... */
	return kind;
}

/* `path` as a Python string literal: 'it\'s a file.txt' */
static char *
python_literal(const char *path)
{
	GString *out = g_string_new("'");
	for (const char *c = path; *c; c++) {
		unsigned char ch = (unsigned char)*c;
		if (ch == '\\' || ch == '\'') g_string_append_c(out, '\\'), g_string_append_c(out, ch);
		else if (ch == '\n') g_string_append(out, "\\n");
		else if (ch == '\r') g_string_append(out, "\\r");
		else if (ch == '\t') g_string_append(out, "\\t");
		else if (ch < 0x20 || ch == 0x7f) g_string_append_printf(out, "\\x%02x", ch);
		else g_string_append_c(out, ch);   /* UTF-8 bytes pass through unchanged */
	}
	g_string_append_c(out, '\'');
	return g_string_free(out, FALSE);
}

/* Ctrl+Shift+C: copy the terminal selection (nothing selected: nothing happens) */
static void
copy_from_terminal(State *st)
{
	VteTerminal *terminal = VTE_TERMINAL(st->terminal);
	if (vte_terminal_get_has_selection(terminal))
		vte_terminal_copy_clipboard_format(terminal, VTE_FORMAT_TEXT);
}

/* Ctrl+Shift+V */
static void
paste_from_clipboard(State *st)
{
	vte_terminal_paste_clipboard(VTE_TERMINAL(st->terminal));
}

/*
 * Shift+Enter: put the highlighted file's path where the program in the terminal wants it.
 *   shell   -> quoted for the shell, with a space after it, typed as keys
 *   vim     -> the plain path, pasted (bracketed paste: lands in the text in any mode)
 *   nano    -> the plain path, pasted
 *   python  -> a quoted string literal 'path', pasted
 *   AI assistant (claude, codex...) -> `@relative/path ` where it has @-mentions, else the path
 * Any other program in front: do nothing, we would only type garbage into it.
 */
static void
paste_selected_path(State *st)
{
	Foreground fg = foreground_kind(st);
	if (fg == FG_OTHER) return;

	/* "selected item" is owned by the selection model: do not unref it */
	GFileInfo *info = gtk_single_selection_get_selected_item(st->selection);
	if (info == NULL) return;
	GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
	if (file == NULL) return;
	char *path = g_file_get_path(file);
	if (path == NULL) return;

	if (fg == FG_LLM) {
		const FhiLlmTool *tool = fhi_llm_detect(foreground_pid(st));
		char *cwd = fhi_llm_cwd(foreground_pid(st));   /* the assistant's own folder, where @ paths start */
		gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
		char *text = tool ? fhi_llm_format_path(tool, path, is_dir, cwd) : NULL;
		if (text) vte_terminal_paste_text(VTE_TERMINAL(st->terminal), text);
		g_free(text);
		g_free(cwd);
	} else if (fg == FG_SHELL) {
		char *quoted = g_shell_quote(path);
		char *text = g_strdup_printf(" %s ", quoted); /* trailing space for the next word */
		type_into_terminal(st, text);
		g_free(text);
		g_free(quoted);
	} else if (fg == FG_PYTHON) {
		char *lit = python_literal(path);
		vte_terminal_paste_text(VTE_TERMINAL(st->terminal), lit);
		g_free(lit);
	} else {
		vte_terminal_paste_text(VTE_TERMINAL(st->terminal), path);
	}
	g_free(path);

	/* The user will want to keep typing, so go back to the terminal */
	gtk_widget_grab_focus(st->terminal);
}


/* What the typed command previewed belongs to another tab / pane: forget it */
static void
preview_reset(State *st)
{
	g_free(st->last_buffer);
	st->last_buffer = NULL;
	if (st->preview_idle == 0) st->preview_idle = g_idle_add(preview_idle_cb, st);
}

/* tmux is no longer in front (detached, closed, another tab) */
static void
tmux_forget(State *st, gboolean back_to_shell_dir)
{
	if (st->tmux_client == 0 && st->tmux_tty == NULL) return;
	st->tmux_client = 0;
	st->tmux_pane_pid = 0;
	st->tmux_fhi = FALSE;
	g_clear_pointer(&st->tmux_tty, g_free);
	g_clear_pointer(&st->tmux_path, g_free);
	/* the terminal's own idea of the folder (from the shell's last prompt) is right again */
	const char *uri = back_to_shell_dir ? vte_terminal_get_current_directory_uri(VTE_TERMINAL(st->terminal)) : NULL;
	if (uri) {
		GFile *file = g_file_new_for_uri(uri);
		sidebar_set_dir(st, file);
		g_object_unref(file);
	}
}

/*
 * The answer of the tmux server: which pane has focus, its folder, and its terminal (so the
 * program in it can be looked up). Moving between panes (prefix + arrow...) lands here
 * within half a second and the file list follows.
 */
static void
on_tmux_pane(pid_t client, const FhiTmuxPane *pane, gpointer user_data)
{
	State *st = user_data;
	if (client != vte_fg_pid(st)) return;   /* tmux was left or the tab changed meanwhile */
	if (pane == NULL) {                     /* cannot ask (no tmux program?): act like plain tmux client */
		tmux_forget(st, FALSE);
		return;
	}

	gboolean new_pane = pane->pane_pid != st->tmux_pane_pid || g_strcmp0(pane->tty, st->tmux_tty) != 0;
	gboolean new_path = new_pane || g_strcmp0(pane->path, st->tmux_path) != 0;
	st->tmux_client = client;
	st->tmux_pane_pid = pane->pane_pid;
	if (new_pane) {
		g_free(st->tmux_tty);
		st->tmux_tty = g_strdup(pane->tty);
		st->tmux_fhi = fhi_tmux_pane_is_fhi(pane->pane_pid);
		g_hash_table_remove_all(st->llm_act);   /* the lit files were another pane's */
		preview_reset(st);
	}
	if (new_path) {
		g_free(st->tmux_path);
		st->tmux_path = g_strdup(pane->path);
		GFile *file = g_file_new_for_path(pane->path);
		sidebar_set_dir(st, file);
		g_object_unref(file);
		tab_set_title(st, st->terminal, pane->path);
	}
}

/* Remember that `key` (relative to the list's folder) was touched by the assistant */
static void
llm_mark(State *st, const char *key, gboolean created)
{
	LlmAct *act = g_hash_table_lookup(st->llm_act, key);
	if (act == NULL) {
		act = g_new0(LlmAct, 1);
		g_hash_table_insert(st->llm_act, g_strdup(key), act);
	}
	act->time = g_get_monotonic_time();
	act->created = act->created || created;   /* a file made and then edited is still "new" */
}

/* The watcher found files the assistant created or edited (runs on the main loop) */
static void
on_llm_changes(const char *root, GPtrArray *changes, gpointer user_data)
{
	State *st = user_data;
	char *dir = list_dir_path(st);
	gboolean rescan = FALSE;

	for (guint i = 0; dir && i < changes->len; i++) {
		const FhiLlmChange *c = changes->pdata[i];
		char *abs = g_build_filename(root, c->path, NULL);
		gsize dl = strlen(dir);
		if (strncmp(abs, dir, dl) == 0 && abs[dl] == '/' && abs[dl + 1] != '\0') {
			char *key = g_strdup(abs + dl + 1);
			if (!c->removed) llm_mark(st, key, c->created);
			/* every folder above it lights up too, so a change deep inside is visible */
			char *slash;
			gboolean first = TRUE;
			while ((slash = strrchr(key, '/')) != NULL) {
				*slash = '\0';
				if (first) {   /* an opened folder lists its files once: read it again */
					char *folder = g_build_filename(dir, key, NULL);
					if (g_hash_table_contains(st->expanded, folder)) rescan = TRUE;
					g_free(folder);
					first = FALSE;
				}
				if (!c->removed) llm_mark(st, key, FALSE);
			}
			g_free(key);
		}
		g_free(abs);
	}
	g_free(dir);
	g_ptr_array_unref(changes);

	if (rescan) peek_rescan(st);
	refresh_all_rows(st);
}

/* Start / stop / poll the watcher for the assistant in front of the terminal; fade old highlights */
static void
llm_update(State *st, pid_t pid, gboolean shell_in_front)
{
	const FhiLlmTool *tool = (pid > 0 && !shell_in_front) ? fhi_llm_detect(pid) : NULL;

	if (tool == NULL || pid != st->llm_pid) {
		if (st->llm_watch) {
			fhi_llm_watch_free(st->llm_watch);
			st->llm_watch = NULL;
		}
		st->llm = NULL;
		st->llm_pid = 0;
	}
	if (tool && st->llm == NULL) {
		st->llm = tool;
		st->llm_pid = pid;
		char *cwd = fhi_llm_cwd(pid);
		if (cwd) st->llm_watch = fhi_llm_watch_new(cwd, on_llm_changes, st);
		g_free(cwd);
		char *text = g_strdup_printf("\uf544 %s", tool->title);
		gtk_label_set_text(GTK_LABEL(st->llm_label), text);
		g_free(text);
		char *tip = g_strdup_printf("%s is running. Files it creates or edits light up in the list.\n"
		                            "Shift+Enter pastes the highlighted file%s.",
		                            tool->title, tool->mention ? " as an @mention" : "'s path");
		gtk_widget_set_tooltip_text(st->llm_label, tip);
		g_free(tip);
	}
	gtk_widget_set_visible(st->llm_label, st->llm != NULL);

	if (st->llm_watch && (st->llm_tick++ % 2 == 0)) {   /* every second */
		fhi_llm_watch_poll(st->llm_watch);
		/* Polling fallback for the top level: normally GTK's folder monitor shows new files by
		 * itself, but if it misses something, compare with the disk and reload (cheap). */
		if (st->resync_timer == 0) resync_directory(st);
	}

	/* highlights fade after a while */
	if (g_hash_table_size(st->llm_act) > 0) {
		gint64 limit = g_get_monotonic_time() - (gint64)LLM_LIT_SECONDS * G_USEC_PER_SEC;
		gboolean faded = FALSE;
		GHashTableIter it;
		gpointer value;
		g_hash_table_iter_init(&it, st->llm_act);
		while (g_hash_table_iter_next(&it, NULL, &value))
			if (((LlmAct *)value)->time < limit) {
				g_hash_table_iter_remove(&it);
				faded = TRUE;
			}
		if (faded) refresh_all_rows(st);
	}
}

/*
 * Every half second: is a shell other than zsh, or an ssh session, running in the
 * terminal? Nothing tells us when the foreground program changes, so we look. Then the
 * file list is dimmed, because its preview and cd features would not work (they are
 * about THIS machine), and a message or the ssh session card is shown over it.
 */
static gboolean
check_shell_support(gpointer user_data)
{
	State *st = user_data;

	/* tmux in front? Ask the server which pane has focus (answer arrives in on_tmux_pane) */
	pid_t outer = vte_fg_pid(st);
	if (outer > 0 && fhi_tmux_is_client(outer)) {
		if (st->tmux_client != 0 && st->tmux_client != outer) tmux_forget(st, FALSE);   /* another tmux */
		fhi_tmux_query(outer, on_tmux_pane, st);
	} else {
		tmux_forget(st, TRUE);
	}

	pid_t pid = foreground_pid(st);
	char *comm = foreground_comm(st);
	const char *bad = (comm && is_unsupported_shell(comm)) ? comm : NULL;

	if (g_strcmp0(bad, st->unsupported_shell) != 0) {
		g_free(st->unsupported_shell);
		st->unsupported_shell = g_strdup(bad);
		if (bad) {
			char *text = g_strdup_printf("%s is not supported", bad);
			gtk_label_set_text(GTK_LABEL(st->unsupported_label), text);
			g_free(text);
		}
		gtk_widget_set_visible(st->unsupported_label, bad != NULL);
	}

	/* ssh: start / stop watching it */
	if (comm && pid > 0 && strcmp(comm, "ssh") == 0) {
		if (st->ssh && fhi_ssh_pid(st->ssh) != pid) {   /* a new ssh replaced the old one */
			fhi_ssh_free(st->ssh);
			st->ssh = NULL;
		}
		if (st->ssh == NULL) st->ssh = fhi_ssh_new(pid);
	} else if (st->ssh) {
		fhi_ssh_free(st->ssh);
		st->ssh = NULL;
	}
	fhi_ssh_card_update(st->ssh_card, st->ssh);
	llm_update(st, pid, pid == effective_shell_pid(st) || st->ssh != NULL);

	gboolean dim = bad != NULL || st->ssh != NULL;
	gtk_widget_set_sensitive(st->left_panel, !dim);
	if (dim) gtk_widget_add_css_class(st->left_panel, "fhi-dim");
	else gtk_widget_remove_css_class(st->left_panel, "fhi-dim");

	g_free(comm);
	return G_SOURCE_CONTINUE;
}

/* ======================================================================== */
/* Tabs                                                                       */
/*                                                                            */
/* Every tab is a VteTerminal (a page of st->notebook) with its own shell. The  */
/* file list, the preview and everything else follow the tab in front, which   */
/* is st->terminal.                                                           */
/* ======================================================================== */

/* Start zsh in `terminal`, with FHI's startup files (see zsh/.zshrc) */
static void
spawn_shell(State *st, GtkWidget *terminal, const char *cwd)
{
	char *command[] = { "zsh", NULL };

	/*
	 * Make zsh load OUR startup files (see zsh/.zshrc), which in turn load the
	 * user's own ones and add the hook that reports the working directory.
	 *   - FHI_USER_ZDOTDIR: where the user's files live (their ZDOTDIR if they
	 *     set one, otherwise their home directory)
	 *   - FHI_ZDOTDIR / ZDOTDIR: our folder (the path is baked in by CMake)
	 */
	char **envp = g_get_environ();
	const char *user_zdotdir = g_environ_getenv(envp, "FHI_USER_ZDOTDIR");   /* set already when FHI runs inside FHI */
	char *own = NULL;
	if (user_zdotdir == NULL) {
		own = g_strdup(g_environ_getenv(envp, "ZDOTDIR"));
		if (!own) own = g_strdup(g_get_home_dir());
		user_zdotdir = own;
	}
	envp = g_environ_setenv(envp, "FHI_USER_ZDOTDIR", user_zdotdir, TRUE);
	envp = g_environ_setenv(envp, "FHI_ZDOTDIR", FHI_ZSH_DIR, TRUE);
	envp = g_environ_setenv(envp, "ZDOTDIR", FHI_ZSH_DIR, TRUE);
	g_free(own);

	vte_terminal_spawn_async(VTE_TERMINAL(terminal),
	    VTE_PTY_DEFAULT,
	    cwd,          /* working directory  */
	    command,      /* command */
	    envp,         /* environment */
	    G_SPAWN_SEARCH_PATH, /* spawn flags: look up zsh in PATH */
	    NULL, NULL, NULL, /* child setup, data, destroy */
	    -1,           /* timeout */
	    NULL,         /* cancellable */
	    child_ready,  /* callback */
	    st);          /* user_data */
	g_strfreev(envp); /* VTE made its own copy */
}

static void apply_terminal_theme_one(State *st, VteTerminal *term);
static void apply_terminal_font(State *st, VteTerminal *term);

/* The tab bar only shows when there is more than one tab */
static void
tabs_update_bar(State *st)
{
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(st->notebook), gtk_notebook_get_n_pages(GTK_NOTEBOOK(st->notebook)) > 1);
}

/* A new tab, starting in `cwd` (NULL = home). `select`: bring it to the front. */
static void
tab_new(State *st, const char *cwd, gboolean select)
{
	GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);
	GtkWidget *terminal = vte_terminal_new();
	gtk_widget_set_hexpand(terminal, TRUE);
	gtk_widget_set_vexpand(terminal, TRUE);
	g_signal_connect(terminal, "child-exited", G_CALLBACK(child_exited), st);
	g_signal_connect(terminal, "current-directory-uri-changed", G_CALLBACK(on_cwd_changed), st);
	/* The command being typed arrives through the terminal title (see zsh/.zshrc) */
	g_signal_connect(terminal, "window-title-changed", G_CALLBACK(on_title_changed), st);

	if (cwd == NULL) cwd = g_get_home_dir();
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 18);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	if (st->terminal == NULL) st->terminal = terminal;   /* the very first tab */
	int num = gtk_notebook_append_page(nb, terminal, label);
	gtk_notebook_set_tab_reorderable(nb, terminal, TRUE);
	tab_set_title(st, terminal, cwd);
	tabs_update_bar(st);

	apply_terminal_theme_one(st, VTE_TERMINAL(terminal));
	apply_terminal_font(st, VTE_TERMINAL(terminal));
	spawn_shell(st, terminal, cwd);
	if (select) {
		gtk_notebook_set_current_page(nb, num);
		gtk_widget_grab_focus(terminal);
	}
}

/* The tab in front changed (clicked, Ctrl+PageUp/Down, one was closed): everything follows it */
static void
on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint num, gpointer user_data)
{
	State *st = user_data;
	if (st->dir_list == NULL || page == st->terminal) return;

	gboolean list_focused = list_has_focus(st);
	st->terminal = page;
	st->shell_pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "fhi-shell-pid"));
	tmux_forget(st, FALSE);   /* the new tab may be in tmux too: the next check finds out */
	g_hash_table_remove_all(st->llm_act);

	const char *uri = vte_terminal_get_current_directory_uri(VTE_TERMINAL(page));
	if (uri) {
		GFile *file = g_file_new_for_uri(uri);
		sidebar_set_dir(st, file);
		g_object_unref(file);
	}
	preview_reset(st);
	check_shell_support(st);   /* ssh / llm / unsupported-shell state of this tab, right away */
	refresh_all_rows(st);
	if (!list_focused) gtk_widget_grab_focus(page);
}

/* Switch tabs, wrapping around: step is +1 or -1 */
static void
tab_step(State *st, int step)
{
	GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);
	int n = gtk_notebook_get_n_pages(nb);
	if (n < 2) return;
	gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + step + n) % n);
}

typedef struct { State *st; GtkWidget *terminal; } CloseJob;

static gboolean
tab_close_cb(gpointer data)
{
	CloseJob *job = data;
	State *st = job->st;
	GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);
	int num = gtk_notebook_page_num(nb, job->terminal);
	if (num >= 0) {
		if (gtk_notebook_get_n_pages(nb) <= 1) {
			gtk_window_destroy(GTK_WINDOW(st->window));   /* the last tab: leave */
		} else {
			/* move to a neighbour first, so st->terminal never points at a dead widget */
			if (job->terminal == st->terminal) gtk_notebook_set_current_page(nb, num > 0 ? num - 1 : 1);
			gtk_notebook_remove_page(nb, gtk_notebook_page_num(nb, job->terminal));
			tabs_update_bar(st);
		}
	}
	g_object_unref(job->terminal);
	g_free(job);
	return G_SOURCE_REMOVE;
}

/* Closing from inside the terminal's own signal is asking for trouble: do it a moment later */
static void
tab_close_later(State *st, GtkWidget *terminal)
{
	CloseJob *job = g_new(CloseJob, 1);
	job->st = st;
	job->terminal = g_object_ref(terminal);
	g_idle_add(tab_close_cb, job);
}

/* Ctrl+Shift+W: end the shell of the tab in front (the tab closes by itself when it is gone) */
static void
tab_close_current(State *st)
{
	if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(st->notebook)) < 2 || st->shell_pid <= 0) return;
	kill(st->shell_pid, SIGHUP);
}

static void
on_new_tab_clicked(GtkButton *button, gpointer user_data)
{
	State *st = user_data;
	char *dir = list_dir_path(st);
	tab_new(st, dir, TRUE);
	g_free(dir);
}

static void menu_set_open(State *st, gboolean open);
static void sort_cycle(State *st, int step);
static void sort_toggle_reverse(State *st);
static void filter_begin(State *st, const char *chars);
static void filter_end(State *st, gboolean clear);

/*
 * Keyboard shortcuts for the whole window. Connected in activate() to a
 * GtkEventControllerKey in the CAPTURE phase. Normally a key press travels
 * from the focused widget; in the capture phase the window sees it FIRST, so
 * the terminal never gets a chance to swallow Shift+Left for itself.
 *
 * Return TRUE  = "I handled this key, stop here".
 * Return FALSE = "not mine, carry on as normal".
 */
static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType modifiers, gpointer user_data)
{
	State *st = user_data;

	/* Keep only the "real" modifiers (Shift, Ctrl, Alt...) and ignore things
	 * like NumLock. */
	modifiers &= gtk_accelerator_get_default_mod_mask();

	/* Tab in the file list opens the hamburger menu (in the terminal it stays
	 * a normal Tab, for completion). */
	if (modifiers == 0 && keyval == GDK_KEY_Tab) {
		GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(st->window));
		if (focus != NULL && (focus == st->list_view ||
		                      gtk_widget_is_ancestor(focus, st->list_view))) {
			menu_set_open(st, TRUE);
			return TRUE;
		}
		return FALSE;
	}

	/* Esc closes the menu */
	if (modifiers == 0 && keyval == GDK_KEY_Escape && gtk_widget_get_visible(st->menu_layer)) {
		menu_set_open(st, FALSE);
		return TRUE;
	}

	GtkWidget *cur = gtk_window_get_focus(GTK_WINDOW(st->window));
	gboolean in_list = cur && (cur == st->list_view || gtk_widget_is_ancestor(cur, st->list_view));
	gboolean in_filter = cur && (cur == st->filter_entry || gtk_widget_is_ancestor(cur, st->filter_entry));

	/* Esc in the filter box, or in the list while a filter is showing: clear it and hide the box */
	if (modifiers == 0 && keyval == GDK_KEY_Escape &&
	    (in_filter || (in_list && gtk_widget_get_visible(st->filter_bar)))) {
		filter_end(st, TRUE);
		return TRUE;
	}
	/* Backspace in the file list = "cd ..". While a filter is showing it edits the
	 * filter instead, like any other typing in the list. */
	if (in_list && keyval == GDK_KEY_BackSpace && modifiers == 0) {
		if (gtk_widget_get_visible(st->filter_bar)) {
			filter_begin(st, NULL);
			GtkEditable *e = GTK_EDITABLE(st->filter_entry);
			glong len = g_utf8_strlen(gtk_editable_get_text(e), -1);
			if (len > 0) gtk_editable_delete_text(e, len - 1, -1);
		} else if (shell_is_foreground(st)) {
			run_in_shell(st, "cd -- ..\r");
		}
		return TRUE;
	}
	/* Right / Left in the file list: open / close a folder in place ("peek") */
	if (in_list && modifiers == 0 && (keyval == GDK_KEY_Right || keyval == GDK_KEY_Left)) {
		peek_key(st, keyval == GDK_KEY_Right);
		return TRUE;
	}
	if (in_list && keyval == GDK_KEY_r && modifiers == GDK_CONTROL_MASK) {
		sort_toggle_reverse(st);
		return TRUE;
	}
	/* Typing a letter, digit or symbol in the file list starts the filter. (Shift is
	 * allowed for capitals and for symbols like "?" and "{".) Space is left alone. */
	if (in_list && (modifiers == 0 || modifiers == GDK_SHIFT_MASK)) {
		gunichar ch = gdk_keyval_to_unicode(keyval);
		if (ch != 0 && ch != ' ' && g_unichar_isprint(ch)) {
			char utf8[8] = { 0 };
			g_unichar_to_utf8(ch, utf8);
			filter_begin(st, utf8);
			return TRUE;
		}
	}
	/* Tabs: Ctrl+Shift+T new, Ctrl+Shift+W close, Ctrl+PageUp/PageDown or Ctrl+(Shift+)Tab switch */
	if (modifiers == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && (keyval == GDK_KEY_t || keyval == GDK_KEY_T)) {
		on_new_tab_clicked(NULL, st);
		return TRUE;
	}
	if (modifiers == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && (keyval == GDK_KEY_w || keyval == GDK_KEY_W)) {
		tab_close_current(st);
		return TRUE;
	}
	if ((modifiers == GDK_CONTROL_MASK && (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_Tab)) ||
	    (modifiers == GDK_CONTROL_MASK && keyval == GDK_KEY_Page_Up) ||
	    (modifiers == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && keyval == GDK_KEY_ISO_Left_Tab)) {
		tab_step(st, (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_ISO_Left_Tab) ? -1 : +1);
		return TRUE;
	}
	/* Ctrl+Shift+C / V: copy and paste (plain Ctrl+C must stay SIGINT for the terminal) */
	if (modifiers == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
		switch (keyval) {
		case GDK_KEY_v:
		case GDK_KEY_V:
			paste_from_clipboard(st);
			return TRUE;
		case GDK_KEY_c:
		case GDK_KEY_C:
			copy_from_terminal(st);
			return TRUE;
		}
	}
	/* The rest are Shift+key shortcuts */
	if (modifiers != GDK_SHIFT_MASK) return FALSE;

	switch (keyval) {
	case GDK_KEY_Left:  /* Shift+Left: go to the file list */
		gtk_widget_grab_focus(st->list_view);
		return TRUE;
	case GDK_KEY_Right: /* Shift+Right: go to the terminal */
		gtk_widget_grab_focus(st->terminal);
		return TRUE;
	case GDK_KEY_Up:    /* Shift+Up / Down: change the sort mode, only in the file list */
	case GDK_KEY_Down:
		if (!in_list) return FALSE;
		sort_cycle(st, keyval == GDK_KEY_Up ? +1 : -1);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter: {
		/* Only when the file list has the focus; in the terminal, Shift+Enter
		 * keeps behaving normally. The focused widget is a row INSIDE the list. */
		GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(st->window));
		if (focus != NULL && (focus == st->list_view ||
		                      gtk_widget_is_ancestor(focus, st->list_view))) {
			paste_selected_path(st);
			return TRUE;
		}
		return FALSE;
	}
	default:
		return FALSE;
	}
}

/* ======================================================================== */
/* Hamburger menu: settings, theme, cheat sheet, file info                    */
/* ======================================================================== */

static void
settings_defaults(Settings *c)
{
	fhi_theme_init(&c->theme);
	c->show_hidden = TRUE;
	c->info_type = c->info_size = c->info_created = c->info_accessed = TRUE;
	c->info_wrap = FALSE;
	c->info_font = 9;
	c->sort_mode = SORT_NAME;
	c->sort_reverse = FALSE;
	c->folders_first = TRUE;
}

static char *
settings_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "fhi", "settings.ini", NULL);
}

static void
kf_get_bool(GKeyFile *kf, const char *key, gboolean *out)
{
	if (g_key_file_has_key(kf, "fhi", key, NULL))
		*out = g_key_file_get_boolean(kf, "fhi", key, NULL);
}

static void
load_settings(State *st)
{
	Settings *c = &st->cfg;
	GKeyFile *kf = g_key_file_new();
	char *path = settings_path();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		if (g_key_file_has_key(kf, "fhi", "info_font", NULL))
			c->info_font = CLAMP(g_key_file_get_integer(kf, "fhi", "info_font", NULL), 6, 32);
		/* The theme: colors, scheme ("system" / "light" / "dark", or the 0 / 1 / 2 older versions
		 * wrote), corners, opacity. */
		fhi_theme_load_group(&c->theme, kf, "fhi");
		/* Older versions always wrote the terminal colors and a switch for whether to use them */
		if (g_key_file_has_key(kf, "fhi", "custom_term", NULL) &&
		    !g_key_file_get_boolean(kf, "fhi", "custom_term", NULL))
			c->theme.set[TC_TERM_BG] = c->theme.set[TC_TERM_FG] = FALSE;
		g_free(st->theme_id);
		st->theme_id = g_key_file_get_string(kf, "fhi", "theme", NULL);
		if (st->theme_id && !*st->theme_id) g_clear_pointer(&st->theme_id, g_free);
		g_free(c->term_font);
		c->term_font = g_key_file_get_string(kf, "fhi", "term_font", NULL);
		if (c->term_font && !*c->term_font) g_clear_pointer(&c->term_font, g_free);
		kf_get_bool(kf, "show_hidden", &c->show_hidden);
		kf_get_bool(kf, "info_type", &c->info_type);
		kf_get_bool(kf, "info_size", &c->info_size);
		kf_get_bool(kf, "info_created", &c->info_created);
		kf_get_bool(kf, "info_accessed", &c->info_accessed);
		kf_get_bool(kf, "info_wrap", &c->info_wrap);
		kf_get_bool(kf, "sort_reverse", &c->sort_reverse);
		kf_get_bool(kf, "folders_first", &c->folders_first);
		if (g_key_file_has_key(kf, "fhi", "sort_mode", NULL))
			c->sort_mode = CLAMP(g_key_file_get_integer(kf, "fhi", "sort_mode", NULL), 0, SORT_COUNT - 1);
	}
	g_free(path);
	g_key_file_unref(kf);
}

static void
save_settings(State *st)
{
	Settings *c = &st->cfg;
	GKeyFile *kf = g_key_file_new();
	g_key_file_set_integer(kf, "fhi", "info_font", c->info_font);
	fhi_theme_save_group(&c->theme, kf, "fhi");
	if (st->theme_id) g_key_file_set_string(kf, "fhi", "theme", st->theme_id);
	if (c->term_font) g_key_file_set_string(kf, "fhi", "term_font", c->term_font);
	g_key_file_set_boolean(kf, "fhi", "show_hidden", c->show_hidden);
	g_key_file_set_boolean(kf, "fhi", "info_type", c->info_type);
	g_key_file_set_boolean(kf, "fhi", "info_size", c->info_size);
	g_key_file_set_boolean(kf, "fhi", "info_created", c->info_created);
	g_key_file_set_boolean(kf, "fhi", "info_accessed", c->info_accessed);
	g_key_file_set_boolean(kf, "fhi", "info_wrap", c->info_wrap);
	g_key_file_set_boolean(kf, "fhi", "sort_reverse", c->sort_reverse);
	g_key_file_set_boolean(kf, "fhi", "folders_first", c->folders_first);
	g_key_file_set_integer(kf, "fhi", "sort_mode", c->sort_mode);

	char *path = settings_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_key_file_save_to_file(kf, path, NULL);
	g_free(dir);
	g_free(path);
	g_key_file_unref(kf);
}

/* ---- file info bar ------------------------------------------------------ */

/* Work out what a file is from its first bytes, ignoring the file name. */
static char *
sniff_type(const guchar *b, gsize n)
{
	static const struct { const char *magic; gsize len; const char *name; } table[] = {
		{ "\x89PNG\r\n\x1a\n", 8, "PNG image" },
		{ "\xff\xd8\xff", 3, "JPEG image" },
		{ "GIF87a", 6, "GIF image" },
		{ "GIF89a", 6, "GIF image" },
		{ "II*\0", 4, "TIFF image" },
		{ "MM\0*", 4, "TIFF image" },
		{ "\0\0\1\0", 4, "ICO icon" },
		{ "%PDF-", 5, "PDF document" },
		{ "PK\3\4", 4, "ZIP archive" },
		{ "\x1f\x8b", 2, "gzip archive" },
		{ "BZh", 3, "bzip2 archive" },
		{ "\xfd" "7zXZ\0", 6, "xz archive" },
		{ "7z\xbc\xaf\x27\x1c", 6, "7-Zip archive" },
		{ "Rar!\x1a\7", 6, "RAR archive" },
		{ "\x7f" "ELF", 4, "ELF executable" },
		{ "SQLite format 3\0", 16, "SQLite database" },
		{ "OggS", 4, "Ogg media" },
		{ "fLaC", 4, "FLAC audio" },
		{ "ID3", 3, "MP3 audio" },
		{ "\x1a\x45\xdf\xa3", 4, "Matroska/WebM video" },
		{ "wOFF", 4, "WOFF font" },
		{ "wOF2", 4, "WOFF2 font" },
	};
	for (guint i = 0; i < G_N_ELEMENTS(table); i++)
		if (n >= table[i].len && memcmp(b, table[i].magic, table[i].len) == 0)
			return g_strdup(table[i].name);

	if (n >= 12 && memcmp(b, "RIFF", 4) == 0) {
		if (memcmp(b + 8, "WEBP", 4) == 0) return g_strdup("WebP image");
		if (memcmp(b + 8, "WAVE", 4) == 0) return g_strdup("WAV audio");
		if (memcmp(b + 8, "AVI ", 4) == 0) return g_strdup("AVI video");
	}
	if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0) return g_strdup("MP4/MOV video");
	if (n >= 2 && b[0] == '#' && b[1] == '!') return g_strdup("Script");

	/* Let the shared-mime-info database have a go, then fall back to text/binary */
	gboolean uncertain = FALSE;
	char *mime = g_content_type_guess(NULL, b, n, &uncertain);
	if (!uncertain && mime && !g_content_type_is_unknown(mime)) {
		char *desc = g_content_type_get_description(mime);
		g_free(mime);
		if (desc) return desc;
		return g_strdup("Known file");
	}
	g_free(mime);

	gboolean text = n > 0 && memchr(b, 0, n) == NULL;
	if (text) {
		/* a cut-off multi-byte character at the end is fine, so only check the start */
		const char *end;
		g_utf8_validate((const char *)b, n, &end);
		if ((gsize)((const guchar *)end - b) + 4 < n) text = FALSE;
	}
	return g_strdup(text ? "Text file" : "Binary data");
}

static char *
describe_type(GFile *file, GFileInfo *fi)
{
	switch (g_file_info_get_file_type(fi)) {
	case G_FILE_TYPE_DIRECTORY: return g_strdup("Folder");
	case G_FILE_TYPE_REGULAR: break;
	default: return g_strdup("Special file");   /* pipe, socket, device: never read these */
	}

	guchar buf[512];
	gsize n = 0;
	char *path = g_file_get_path(file);
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (f) { n = fread(buf, 1, sizeof buf, f); fclose(f); }
	g_free(path);

	if (!f) return g_strdup("Unreadable");
	if (n == 0) return g_strdup("Empty file");
	return sniff_type(buf, n);
}

static void
add_field(GString *out, const char *label, const char *value)
{
	if (out->len) g_string_append(out, "   ·   ");
	g_string_append_printf(out, "%s: %s", label, value);
}

static char *
format_time(GDateTime *dt)
{
	if (dt == NULL) return g_strdup("n/a");
	GDateTime *local = g_date_time_to_local(dt);
	char *text = g_date_time_format(local, "%Y-%m-%d %H:%M");
	g_date_time_unref(local);
	return text;
}

static void
update_info(State *st)
{
	if (st->info_label == NULL) return;
	Settings *c = &st->cfg;
	GtkLabel *label = GTK_LABEL(st->info_label);
	gboolean any = c->info_type || c->info_size || c->info_created || c->info_accessed;

	gtk_label_set_wrap(label, c->info_wrap);
	gtk_label_set_ellipsize(label, c->info_wrap ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END);
	gtk_widget_set_visible(st->info_label, any);
	if (!any) return;

	GString *out = g_string_new(NULL);
	GFileInfo *sel = st->selection ? gtk_single_selection_get_selected_item(st->selection) : NULL;
	GFile *file = sel ? G_FILE(g_file_info_get_attribute_object(sel, "standard::file")) : NULL;

	if (file == NULL) {
		g_string_assign(out, "No file selected");
	} else if (g_file_info_get_attribute_boolean(sel, "fhi::ghost")) {
		g_string_assign(out, "Does not exist yet");
	} else {
		GFileInfo *fi = g_file_query_info(file,
		    "standard::type,standard::size,time::created,time::access",
		    G_FILE_QUERY_INFO_NONE, NULL, NULL);
		if (fi == NULL) {
			g_string_assign(out, "Unavailable");
		} else {
			char *v;
			if (c->info_type) {
				v = describe_type(file, fi);
				add_field(out, "Type", v);
				g_free(v);
			}
			if (c->info_size && g_file_info_get_file_type(fi) != G_FILE_TYPE_DIRECTORY) {
				v = g_format_size(g_file_info_get_size(fi));
				add_field(out, "Size", v);
				g_free(v);
			}
			if (c->info_created) {
				GDateTime *dt = g_file_info_get_creation_date_time(fi);
				v = format_time(dt);
				add_field(out, "Created", v);
				g_free(v);
				if (dt) g_date_time_unref(dt);
			}
			if (c->info_accessed) {
				GDateTime *dt = g_file_info_get_access_date_time(fi);
				v = format_time(dt);
				add_field(out, "Accessed", v);
				g_free(v);
				if (dt) g_date_time_unref(dt);
			}
			g_object_unref(fi);
		}
	}
	gtk_label_set_text(label, out->str);
	gtk_widget_set_tooltip_text(st->info_label, out->str);   /* full text when cut off */
	g_string_free(out, TRUE);
}

static void
on_selection_changed(GObject *selection, GParamSpec *pspec, gpointer user_data)
{
	update_info(user_data);
}

static gboolean
hidden_filter_func(gpointer item, gpointer user_data)
{
	State *st = user_data;
	const char *name = g_file_info_get_name(G_FILE_INFO(item));
	if (name == NULL) return TRUE;
	if (!st->cfg.show_hidden && name[0] == '.') return FALSE;
	return fhi_filter_matches(st->filter, name);
}

/* ---- applying settings -------------------------------------------------- */

static char *
rgb_css(const GdkRGBA *c)
{
	GdkRGBA o = *c;
	o.alpha = 1.0;
	return gdk_rgba_to_string(&o);
}

/* ---- colors: everything the theme decides ----------------------------- */

static gboolean
display_composited(void)
{
	return gdk_display_is_composited(gdk_display_get_default());
}

/*
 * The terminal's colors. With nothing set this is exactly what FHI always did: VTE's
 * own defaults. Transparency is the only thing that needs an explicit background, because
 * the alpha lives in it.
 */
static void
apply_terminal_theme_one(State *st, VteTerminal *term)
{
	const FhiTheme *t = &st->cfg.theme;

	vte_terminal_set_default_colors(term);   /* also resets palette, cursor */

	GdkRGBA bg;
	const GdkRGBA *bgp = NULL, *fgp = t->set[TC_TERM_FG] ? &t->col[TC_TERM_FG] : NULL;
	if (t->set[TC_TERM_BG]) { bg = t->col[TC_TERM_BG]; bgp = &bg; }
	if (fhi_theme_is_transparent(t, display_composited())) {
		if (bgp == NULL) {   /* keep VTE's own background, only make it see-through */
			vte_terminal_get_color_background_for_draw(term, &bg);
			bgp = &bg;
		}
		bg.alpha = t->opacity / 100.0;
	}

	if (t->has_palette)
		vte_terminal_set_colors(term, fgp, bgp, t->palette, FHI_PALETTE_SIZE);
	else if (fgp)
		vte_terminal_set_color_foreground(term, fgp);
	/* Set on its own as well: this is the call that carries the alpha for sure */
	if (bgp) vte_terminal_set_color_background(term, bgp);
	if (t->set[TC_CURSOR]) vte_terminal_set_color_cursor(term, &t->col[TC_CURSOR]);
}

static void
apply_terminal_theme(State *st)
{
	if (st->notebook == NULL) return;
	GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);
	for (int i = 0; i < gtk_notebook_get_n_pages(nb); i++)
		apply_terminal_theme_one(st, VTE_TERMINAL(gtk_notebook_get_nth_page(nb, i)));
}

/* terminal font: the user's own choice if there is one, else plain Monospace.
 * Either way the bundled Nerd Font is the last family, so icons in prompts
 * (starship, powerlevel10k, eza, ...) show even without a patched font. */
static void
apply_terminal_font(State *st, VteTerminal *term)
{
	const Settings *c = &st->cfg;
	PangoFontDescription *fd = pango_font_description_from_string(c->term_font ? c->term_font : "Monospace 10");
	char *families = g_strdup_printf("%s, " FHI_ICON_FONT, pango_font_description_get_family(fd));
	pango_font_description_set_family(fd, families);
	vte_terminal_set_font(term, fd);
	pango_font_description_free(fd);
	g_free(families);
}

/* Light / dark, the style sheet (FHI's own rules, then the theme's) and the terminal colors */
static void
apply_theme(State *st)
{
	Settings *c = &st->cfg;
	FhiTheme *t = &c->theme;

	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
	             fhi_theme_is_dark(t, st->system_dark), NULL);

	/* style sheet: row highlight colors + info bar font */
	char *col[4];
	for (int i = 0; i < 4; i++) col[i] = rgb_css(&t->col[TC_TOUCH + i]);
	char *css = g_strdup_printf(
	    ".fhi-row    { padding: 2px 6px; border-radius: 4px; }"
	    ".fhi-touch  { background: alpha(%s, 0.30); }"
	    ".fhi-create { background: alpha(%s, 0.40); }"
	    ".fhi-dest   { background: alpha(%s, 0.40); }"
	    ".fhi-remove { background: alpha(%s, 0.40); }"
	    ".fhi-llm-new { background: alpha(%s, 0.55); }"
	    ".fhi-llm-mod { background: alpha(%s, 0.45); }"
	    ".fhi-ghost label { font-style: italic; }"
	    ".fhi-icon   { font-family: '" FHI_ICON_FONT "'; min-width: 1.4em; }"
	    ".fhi-info   { font-size: %dpt; padding: 4px 8px; opacity: 0.85; }"
	    ".fhi-key    { font-family: monospace; padding: 0 6px; border-radius: 4px;"
	    "              border: 1px solid alpha(currentColor, 0.35);"
	    "              background: alpha(currentColor, 0.08); }"
	    ".fhi-menu   { background-color: @theme_bg_color; padding: 8px 10px;"
	    "              border-right: 1px solid alpha(currentColor, 0.25);"
	    "              box-shadow: 4px 0 14px alpha(black, 0.35); }"
	    ".fhi-top    { padding: 4px; }"
	    ".fhi-unsupported { background: alpha(@theme_bg_color, 0.95); font-weight: bold; padding: 8px 12px; border-radius: 6px; }"
	    ".fhi-dim    { opacity: 0.35; }"
	    ".fhi-sshcard { background: alpha(@theme_bg_color, 0.985); padding: 12px 16px; border-radius: 8px;"
	    "               border: 1px solid alpha(currentColor, 0.25); box-shadow: 0 4px 16px alpha(black, 0.35); }"
	    ".fhi-sshnarrow { padding: 8px 10px; }"
	    ".fhi-sshtitle { font-weight: bold; font-size: 12pt; }"
	    ".fhi-square, .fhi-square > border, .fhi-square scrolledwindow { border-radius: 0; }"
	    ".fhi-sort   { font-size: 8pt; opacity: 0.5; padding: 4px 8px; }"
	    ".fhi-swatch { min-width: 14px; min-height: 14px; border-radius: 3px; }",
	    col[0], col[1], col[2], col[3], col[1], col[0], c->info_font);
	gboolean composited = display_composited();
	char *themecss = fhi_theme_css(t, composited);   /* after ours: same rule, last one wins */
	char *all = g_strconcat(css, themecss, NULL);
	gtk_css_provider_load_from_data(st->css, all, -1);
	g_free(all);
	g_free(themecss);
	g_free(css);
	for (int i = 0; i < 4; i++) g_free(col[i]);

	/* The class is only there while the window really is see-through */
	if (fhi_theme_is_transparent(t, composited)) gtk_widget_add_css_class(st->window, "fhi-transparent");
	else gtk_widget_remove_css_class(st->window, "fhi-transparent");

	apply_terminal_theme(st);
}

/* A slider moved many times a second: apply at once, write the file once it settles */
static gboolean
save_later(gpointer user_data)
{
	State *st = user_data;
	st->save_timer = 0;
	save_settings(st);
	return G_SOURCE_REMOVE;
}

static void
queue_save(State *st)
{
	if (st->save_timer) g_source_remove(st->save_timer);
	st->save_timer = g_timeout_add(400, save_later, st);
}

/* Push every setting to the UI. `save` is FALSE only for the first call at startup. */
static void
apply_all(State *st, gboolean save)
{
	Settings *c = &st->cfg;
	apply_theme(st);
	GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);
	for (int i = 0; i < gtk_notebook_get_n_pages(nb); i++)
		apply_terminal_font(st, VTE_TERMINAL(gtk_notebook_get_nth_page(nb, i)));

	gtk_filter_changed(st->hidden_filter, GTK_FILTER_CHANGE_DIFFERENT);
	if (st->sorter) gtk_sorter_changed(st->sorter, GTK_SORTER_CHANGE_DIFFERENT);
	if (st->sort_label) {
		/* Faint hint, and only when the sort is not the plain default */
		gboolean plain = c->sort_mode == SORT_NAME && !c->sort_reverse;
		char *text = g_strdup_printf("%s %s", sort_name[c->sort_mode], c->sort_reverse ? "↑" : "↓");
		char *tip = g_strdup_printf("Sorted by %s, %s%s\nShift+↑/↓ changes the mode, Ctrl+R reverses",
		    sort_name[c->sort_mode], sort_dir_text[c->sort_mode][c->sort_reverse],
		    c->folders_first ? ", folders first" : "");
		gtk_label_set_text(GTK_LABEL(st->sort_label), text);
		gtk_widget_set_tooltip_text(st->sort_label, tip);
		gtk_widget_set_visible(st->sort_label, !plain);
		g_free(text);
		g_free(tip);
	}
	update_info(st);
	if (save) save_settings(st);
}

/* ---- widgets ------------------------------------------------------------ */

static void
on_flag_changed(GObject *w, GParamSpec *pspec, gpointer user_data)
{
	State *st = user_data;
	gboolean *flag = g_object_get_data(w, "flag");
	gboolean value;
	g_object_get(w, "active", &value, NULL);
	*flag = value;
	apply_all(st, TRUE);
}

static GtkWidget *
switch_row(State *st, const char *text, gboolean *flag)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *label = gtk_label_new(text);
	GtkWidget *sw = gtk_switch_new();
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_switch_set_active(GTK_SWITCH(sw), *flag);
	gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(sw), "flag", flag);
	g_signal_connect(sw, "notify::active", G_CALLBACK(on_flag_changed), st);
	gtk_box_append(GTK_BOX(row), label);
	gtk_box_append(GTK_BOX(row), sw);
	return row;
}

static GtkWidget *
check_row(State *st, const char *text, gboolean *flag)
{
	GtkWidget *cb = gtk_check_button_new_with_label(text);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), *flag);
	g_object_set_data(G_OBJECT(cb), "flag", flag);
	g_signal_connect(cb, "notify::active", G_CALLBACK(on_flag_changed), st);
	return cb;
}

static void
on_nav(GtkButton *button, gpointer stack)
{
	gtk_stack_set_visible_child_name(GTK_STACK(stack), g_object_get_data(G_OBJECT(button), "page"));
}

/* "Theme  ›" style button that switches the menu to another page */
static GtkWidget *
nav_button(GtkWidget *stack, const char *text, const char *page, gboolean arrow)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(row), label);
	if (arrow) gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name("go-next-symbolic"));

	GtkWidget *button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(button), row);
	gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
	g_object_set_data(G_OBJECT(button), "page", (gpointer)page);
	g_signal_connect(button, "clicked", G_CALLBACK(on_nav), stack);
	return button;
}

/* A page of the menu with a "‹ Title" back button at the top */
static GtkWidget *
sub_page(GtkWidget *stack, const char *title)
{
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *back = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "heading");
	gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name("go-previous-symbolic"));
	gtk_box_append(GTK_BOX(row), label);
	gtk_button_set_child(GTK_BUTTON(back), row);
	gtk_button_set_has_frame(GTK_BUTTON(back), FALSE);
	gtk_widget_set_halign(back, GTK_ALIGN_START);
	g_object_set_data(G_OBJECT(back), "page", (gpointer)"main");
	g_signal_connect(back, "clicked", G_CALLBACK(on_nav), stack);
	gtk_box_append(GTK_BOX(page), back);
	gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	return page;
}

/* ---- the theme page: keeping the widgets and the settings in step ------- */

/* What an unset ("follow the system") color button shows: roughly the system's color */
static void
fallback_color(State *st, int idx, GdkRGBA *out)
{
	static const char *const light[] = { "#f6f5f4", "#2e3436", "#ffffff", "#3584e4", "#ffffff", "#2e3436", "#2e3436" };
	static const char *const dark[]  = { "#242424", "#ffffff", "#1e1e1e", "#3584e4", "#1e1e1e", "#d0d0d0", "#d0d0d0" };
	gboolean is_dark = fhi_theme_is_dark(&st->cfg.theme, st->system_dark);
	gdk_rgba_parse(out, idx < TC_TOUCH ? (is_dark ? dark : light)[idx] : "#808080");
}

/* The numbers next to the two sliders (the sliders themselves show no value: it would sit on the knob) */
static void
theme_sync_labels(State *st)
{
	const FhiTheme *t = &st->cfg.theme;
	char *r = t->radius >= 0 ? g_strdup_printf("Corners  %d px", t->radius) : g_strdup("Corners  (system)");
	char *o = g_strdup_printf("Opacity  %d%%", t->opacity);
	gtk_label_set_text(GTK_LABEL(st->radius_label), r);
	gtk_label_set_text(GTK_LABEL(st->opacity_label), o);
	g_free(r);
	g_free(o);
}

/* Make every widget of the page show the current settings. Their signals are ignored meanwhile. */
static void
theme_sync_widgets(State *st)
{
	const FhiTheme *t = &st->cfg.theme;
	st->syncing = TRUE;

	gtk_drop_down_set_selected(GTK_DROP_DOWN(st->scheme_dd), CLAMP(t->scheme, 0, 2));
	for (int i = 0; i < TC_COUNT; i++) {
		GdkRGBA c = t->col[i];
		if (!t->set[i]) fallback_color(st, i, &c);
		gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(st->opt_btn[i]), &c);
		gtk_widget_set_opacity(st->opt_btn[i], t->set[i] ? 1.0 : 0.4);   /* faded = the system's */
		if (st->opt_clear[i]) gtk_widget_set_sensitive(st->opt_clear[i], t->set[i]);
	}

	gtk_switch_set_active(GTK_SWITCH(st->pal_switch), t->has_palette);
	gtk_widget_set_visible(st->pal_box, t->has_palette);
	GdkRGBA def[FHI_PALETTE_SIZE];
	fhi_theme_default_palette(def);
	for (int i = 0; i < FHI_PALETTE_SIZE; i++)
		gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(st->pal_btn[i]), t->has_palette ? &t->palette[i] : &def[i]);

	gtk_range_set_value(GTK_RANGE(st->radius_scale), t->radius >= 0 ? t->radius : 6);
	gtk_widget_set_opacity(st->radius_scale, t->radius >= 0 ? 1.0 : 0.4);
	gtk_widget_set_sensitive(st->radius_clear, t->radius >= 0);
	gtk_range_set_value(GTK_RANGE(st->opacity_scale), t->opacity);
	theme_sync_labels(st);

	guint sel = 0;
	for (guint i = 0; st->themes && i < st->themes->len; i++)
		if (st->theme_id && strcmp(((FhiThemeInfo *)st->themes->pdata[i])->id, st->theme_id) == 0) sel = i + 1;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(st->theme_dd), sel);

	st->syncing = FALSE;
}

static void
on_opt_color_set(GtkColorButton *button, gpointer user_data)
{
	State *st = user_data;
	if (st->syncing) return;
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "idx"));
	gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &st->cfg.theme.col[i]);
	st->cfg.theme.col[i].alpha = 1.0;
	st->cfg.theme.set[i] = TRUE;
	theme_sync_widgets(st);
	apply_all(st, TRUE);
}

static void
on_opt_clear(GtkButton *button, gpointer user_data)
{
	State *st = user_data;
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "idx"));
	st->cfg.theme.set[i] = FALSE;
	theme_sync_widgets(st);
	apply_all(st, TRUE);
}

/* label, color button and (for colors that can follow the system) a button that unsets it */
static GtkWidget *
theme_color_row(State *st, int idx)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *label = gtk_label_new(fhi_theme_color_label(idx));
	GtkWidget *button = gtk_color_button_new();
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_hexpand(label, TRUE);
	g_object_set_data(G_OBJECT(button), "idx", GINT_TO_POINTER(idx));
	g_signal_connect(button, "color-set", G_CALLBACK(on_opt_color_set), st);
	gtk_box_append(GTK_BOX(row), label);
	gtk_box_append(GTK_BOX(row), button);
	st->opt_btn[idx] = button;
	if (idx < TC_TOUCH) {
		GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
		gtk_widget_set_tooltip_text(clear, "Follow the system");
		gtk_button_set_has_frame(GTK_BUTTON(clear), FALSE);
		g_object_set_data(G_OBJECT(clear), "idx", GINT_TO_POINTER(idx));
		g_signal_connect(clear, "clicked", G_CALLBACK(on_opt_clear), st);
		gtk_box_append(GTK_BOX(row), clear);
		st->opt_clear[idx] = clear;
	}
	return row;
}

static void
on_scheme_changed(GObject *dd, GParamSpec *pspec, gpointer user_data)
{
	State *st = user_data;
	if (st->syncing) return;
	st->cfg.theme.scheme = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	theme_sync_widgets(st);   /* the faded "system" colors depend on light / dark */
	apply_all(st, TRUE);
}

static void
on_pal_switch(GObject *sw, GParamSpec *pspec, gpointer user_data)
{
	State *st = user_data;
	if (st->syncing) return;
	FhiTheme *t = &st->cfg.theme;
	t->has_palette = gtk_switch_get_active(GTK_SWITCH(sw));
	if (t->has_palette) fhi_theme_default_palette(t->palette);
	theme_sync_widgets(st);
	apply_all(st, TRUE);
}

static void
on_pal_color_set(GtkColorButton *button, gpointer user_data)
{
	State *st = user_data;
	if (st->syncing) return;
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "idx"));
	gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &st->cfg.theme.palette[i]);
	st->cfg.theme.palette[i].alpha = 1.0;
	apply_all(st, TRUE);
}

static void
on_radius_changed(GtkRange *range, gpointer user_data)
{
	State *st = user_data;
	if (st->syncing) return;
	st->cfg.theme.radius = (int)gtk_range_get_value(range);
	gtk_widget_set_opacity(st->radius_scale, 1.0);
	gtk_widget_set_sensitive(st->radius_clear, TRUE);
	theme_sync_labels(st);
	apply_theme(st);
	queue_save(st);
}

static void
on_radius_clear(GtkButton *button, gpointer user_data)
{
	State *st = user_data;
	st->cfg.theme.radius = -1;
	theme_sync_widgets(st);
	apply_all(st, TRUE);
}

static void
on_opacity_changed(GtkRange *range, gpointer user_data)
{
	State *st = user_data;
	if (st->syncing) return;
	st->cfg.theme.opacity = (int)gtk_range_get_value(range);
	theme_sync_labels(st);
	apply_theme(st);   /* at 100 this takes the very same path as before the feature existed */
	queue_save(st);
}

static gboolean
only_monospace(const PangoFontFamily *family, const PangoFontFace *face, gpointer data)
{
	return pango_font_family_is_monospace((PangoFontFamily *)family);
}

static void
on_term_font_set(GtkFontButton *button, gpointer user_data)
{
	State *st = user_data;
	g_free(st->cfg.term_font);
	st->cfg.term_font = g_strdup(gtk_font_chooser_get_font(GTK_FONT_CHOOSER(button)));
	apply_all(st, TRUE);
}

static void
on_term_font_default(GtkButton *button, gpointer user_data)
{
	State *st = user_data;
	g_clear_pointer(&st->cfg.term_font, g_free);
	gtk_font_chooser_set_font(GTK_FONT_CHOOSER(st->font_btn), "Monospace 10");
	apply_all(st, TRUE);
}

/* Rebuild the theme list: "Default" first, then every theme file found */
static void
theme_list_refresh(State *st)
{
	if (st->themes) g_ptr_array_unref(st->themes);
	st->themes = fhi_theme_list(FHI_THEME_DIR);
	GPtrArray *names = g_ptr_array_new();
	g_ptr_array_add(names, (gpointer)"Default (system)");
	for (guint i = 0; i < st->themes->len; i++) {
		const FhiThemeInfo *info = st->themes->pdata[i];
		g_ptr_array_add(names, info->user ? g_strdup_printf("%s  (yours)", info->name) : g_strdup(info->name));
	}
	g_ptr_array_add(names, NULL);
	gboolean was = st->syncing;
	st->syncing = TRUE;
	gtk_string_list_splice(st->theme_names, 0, g_list_model_get_n_items(G_LIST_MODEL(st->theme_names)),
	                       (const char *const *)names->pdata);
	st->syncing = was;
	for (guint i = 1; i + 1 < names->len; i++) g_free(names->pdata[i]);
	g_ptr_array_free(names, TRUE);
}

static void
on_theme_selected(GObject *dd, GParamSpec *pspec, gpointer user_data)
{
	State *st = user_data;
	if (st->syncing) return;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	FhiTheme loaded;
	fhi_theme_init(&loaded);
	char *id = NULL;
	if (sel > 0 && st->themes && sel - 1 < st->themes->len) {
		const FhiThemeInfo *info = st->themes->pdata[sel - 1];
		if (!fhi_theme_load_file(&loaded, info)) { fhi_theme_clear(&loaded); return; }
		id = g_strdup(info->id);
	}
	/* How see-through the window is belongs to the screen, not to a look: keep it unless the theme insists */
	if (loaded.opacity == 100) loaded.opacity = st->cfg.theme.opacity;
	fhi_theme_copy(&st->cfg.theme, &loaded);
	fhi_theme_clear(&loaded);
	g_free(st->theme_id);
	st->theme_id = id;
	theme_sync_widgets(st);
	apply_all(st, TRUE);
}

static void
on_save_theme(GtkWidget *w, gpointer user_data)
{
	State *st = user_data;
	const char *name = gtk_editable_get_text(GTK_EDITABLE(st->theme_entry));
	char *id = fhi_theme_save_as(&st->cfg.theme, name);
	if (id == NULL) return;   /* empty name, or the folder is not writable */
	g_free(st->cfg.theme.name);
	st->cfg.theme.name = g_strstrip(g_strdup(name));
	g_free(st->theme_id);
	st->theme_id = id;
	theme_list_refresh(st);
	theme_sync_widgets(st);
	gtk_editable_set_text(GTK_EDITABLE(st->theme_entry), "");
	save_settings(st);
}

static void
open_with_default_app(const char *path)
{
	GFile *f = g_file_new_for_path(path);
	char *uri = g_file_get_uri(f);
	GError *error = NULL;
	if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
		g_printerr("could not open %s: %s\n", path, error->message);
		g_clear_error(&error);
	}
	g_free(uri);
	g_object_unref(f);
}

static void
on_open_themes_dir(GtkButton *button, gpointer user_data)
{
	char *dir = fhi_theme_user_dir();
	g_mkdir_with_parents(dir, 0755);
	open_with_default_app(dir);
	g_free(dir);
}

static char *
user_css_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "fhi", "user.css", NULL);
}

static const char user_css_template[] =
	"/* FHI user style sheet: plain GTK4 CSS. It is loaded LAST, so it beats the theme and\n"
	" * everything else, and it reloads the moment you save it. Mistakes are printed in the\n"
	" * terminal FHI was started from.\n"
	" *\n"
	" * Named colors (redefine one and every widget that uses it follows):\n"
	" *   @theme_bg_color  @theme_fg_color  @theme_base_color  @theme_text_color\n"
	" *   @theme_selected_bg_color  @theme_selected_fg_color\n"
	" *\n"
	" * FHI's own classes:\n"
	" *   .fhi-row       a row of the file list      .fhi-icon     its file icon\n"
	" *   .fhi-touch .fhi-create .fhi-dest .fhi-remove   command preview highlights\n"
	" *   .fhi-info      the info bar under the list .fhi-sort     the sort hint\n"
	" *   .fhi-menu      the menu panel              .fhi-key      keys in the cheat sheet\n"
	" *   .fhi-sshcard   the SSH session card        .fhi-unsupported  the \"bash is not supported\" note\n"
	" *\n"
	" * Examples (remove the comment marks to try them):\n"
	" *\n"
	" * .fhi-row { padding: 6px 10px; }\n"
	" * .fhi-info { font-family: monospace; }\n"
	" * listview row:selected { background: alpha(@theme_selected_bg_color, 0.5); }\n"
	" * headerbar { min-height: 28px; }\n"
	" */\n";

static void
on_open_user_css(GtkButton *button, gpointer user_data)
{
	char *path = user_css_path();
	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		char *dir = g_path_get_dirname(path);
		g_mkdir_with_parents(dir, 0700);
		g_file_set_contents(path, user_css_template, -1, NULL);
		g_free(dir);
	}
	open_with_default_app(path);
	g_free(path);
}

static void
on_reset_theme(GtkButton *button, gpointer user_data)
{
	State *st = user_data;
	int opacity = st->cfg.theme.opacity;
	fhi_theme_clear(&st->cfg.theme);
	fhi_theme_init(&st->cfg.theme);
	st->cfg.theme.opacity = opacity;   /* see on_theme_selected */
	g_clear_pointer(&st->theme_id, g_free);
	theme_sync_widgets(st);
	on_term_font_default(NULL, st);    /* applies and saves */
}

static void
section_label(GtkWidget *page, const char *text)
{
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_widget_set_margin_top(l, 6);
	gtk_box_append(GTK_BOX(page), l);
}

static GtkWidget *
hint_label(const char *text)
{
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 34);
	gtk_widget_add_css_class(l, "dim-label");
	return l;
}

/* label on the left, `widget` filling the rest */
static GtkWidget *
labeled(const char *text, GtkWidget *widget)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(row), label);
	gtk_box_append(GTK_BOX(row), widget);
	return row;
}

static GtkWidget *
slider(double min, double max)
{
	GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1);
	gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
	gtk_widget_set_hexpand(sc, TRUE);
	gtk_widget_set_size_request(sc, 110, -1);
	return sc;
}

static GtkWidget *
build_theme_page(State *st, GtkWidget *stack)
{
	GtkWidget *page = sub_page(stack, "Theme");

	/* --- which theme --- */
	st->theme_names = gtk_string_list_new(NULL);
	st->theme_dd = gtk_drop_down_new(G_LIST_MODEL(st->theme_names), NULL);
	gtk_widget_set_size_request(st->theme_dd, 150, -1);
	theme_list_refresh(st);
	gtk_box_append(GTK_BOX(page), labeled("Theme", st->theme_dd));

	const char *const schemes[] = { "System", "Light", "Dark", NULL };
	st->scheme_dd = gtk_drop_down_new_from_strings(schemes);
	gtk_box_append(GTK_BOX(page), labeled("Color scheme", st->scheme_dd));

	GtkWidget *save_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	st->theme_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(st->theme_entry), "Save these colors as…");
	gtk_widget_set_hexpand(st->theme_entry, TRUE);
	gtk_editable_set_width_chars(GTK_EDITABLE(st->theme_entry), 1);
	GtkWidget *save_btn = gtk_button_new_with_label("Save");
	g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_theme), st);
	g_signal_connect(st->theme_entry, "activate", G_CALLBACK(on_save_theme), st);
	gtk_box_append(GTK_BOX(save_row), st->theme_entry);
	gtk_box_append(GTK_BOX(save_row), save_btn);
	gtk_box_append(GTK_BOX(page), save_row);

	GtkWidget *files = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *open_dir = gtk_button_new_with_label("Themes folder");
	GtkWidget *open_css = gtk_button_new_with_label("Edit user.css");
	gtk_widget_set_hexpand(open_dir, TRUE);
	gtk_widget_set_hexpand(open_css, TRUE);
	gtk_widget_set_tooltip_text(open_dir, "Drop theme .ini files here (see the examples that came with FHI)");
	gtk_widget_set_tooltip_text(open_css, "Your own CSS, applied on top of everything and reloaded when you save it");
	g_signal_connect(open_dir, "clicked", G_CALLBACK(on_open_themes_dir), st);
	g_signal_connect(open_css, "clicked", G_CALLBACK(on_open_user_css), st);
	gtk_box_append(GTK_BOX(files), open_dir);
	gtk_box_append(GTK_BOX(files), open_css);
	gtk_box_append(GTK_BOX(page), files);
	gtk_box_append(GTK_BOX(page), hint_label("Faded colors follow your system theme. Click one to set it, "
	                                         "or the eraser to hand it back."));

	/* --- interface --- */
	section_label(page, "Interface");
	for (int i = TC_WINDOW_BG; i <= TC_ACCENT; i++) gtk_box_append(GTK_BOX(page), theme_color_row(st, i));

	GtkWidget *rrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	st->radius_scale = slider(0, 24);
	g_signal_connect(st->radius_scale, "value-changed", G_CALLBACK(on_radius_changed), st);
	st->radius_clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
	gtk_widget_set_tooltip_text(st->radius_clear, "Follow the system");
	gtk_button_set_has_frame(GTK_BUTTON(st->radius_clear), FALSE);
	g_signal_connect(st->radius_clear, "clicked", G_CALLBACK(on_radius_clear), st);
	st->radius_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(st->radius_label), 0);
	gtk_widget_set_size_request(st->radius_label, 120, -1);
	gtk_box_append(GTK_BOX(rrow), st->radius_label);
	gtk_box_append(GTK_BOX(rrow), st->radius_scale);
	gtk_box_append(GTK_BOX(rrow), st->radius_clear);
	gtk_box_append(GTK_BOX(page), rrow);

	st->opacity_scale = slider(20, 100);
	g_signal_connect(st->opacity_scale, "value-changed", G_CALLBACK(on_opacity_changed), st);
	GtkWidget *orow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	st->opacity_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(st->opacity_label), 0);
	gtk_widget_set_size_request(st->opacity_label, 120, -1);
	gtk_box_append(GTK_BOX(orow), st->opacity_label);
	gtk_box_append(GTK_BOX(orow), st->opacity_scale);
	gtk_box_append(GTK_BOX(page), orow);
	gtk_box_append(GTK_BOX(page), hint_label(display_composited()
	    ? "100 is solid and costs nothing. Below that the window blends with what is behind it."
	    : "Your screen has no compositor, so the window cannot be see-through here."));

	/* --- terminal --- */
	section_label(page, "Terminal");
	for (int i = TC_TERM_BG; i <= TC_CURSOR; i++) gtk_box_append(GTK_BOX(page), theme_color_row(st, i));
	st->pal_switch = gtk_switch_new();
	gtk_widget_set_valign(st->pal_switch, GTK_ALIGN_CENTER);
	g_signal_connect(st->pal_switch, "notify::active", G_CALLBACK(on_pal_switch), st);
	gtk_box_append(GTK_BOX(page), labeled("Custom ANSI colors", st->pal_switch));
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
	for (int i = 0; i < FHI_PALETTE_SIZE; i++) {
		st->pal_btn[i] = gtk_color_button_new();
		g_object_set_data(G_OBJECT(st->pal_btn[i]), "idx", GINT_TO_POINTER(i));
		g_signal_connect(st->pal_btn[i], "color-set", G_CALLBACK(on_pal_color_set), st);
		gtk_grid_attach(GTK_GRID(grid), st->pal_btn[i], i % 8, i / 8, 1, 1);
	}
	st->pal_box = grid;
	gtk_box_append(GTK_BOX(page), grid);

	GtkWidget *fl = gtk_label_new("Terminal font");
	gtk_label_set_xalign(GTK_LABEL(fl), 0);
	gtk_widget_add_css_class(fl, "dim-label");
	gtk_widget_set_margin_top(fl, 6);
	gtk_box_append(GTK_BOX(page), fl);
	GtkWidget *frow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	st->font_btn = gtk_font_button_new_with_font(st->cfg.term_font ? st->cfg.term_font : "Monospace 10");
	gtk_font_chooser_set_filter_func(GTK_FONT_CHOOSER(st->font_btn), only_monospace, NULL, NULL);
	gtk_widget_set_hexpand(st->font_btn, TRUE);
	g_signal_connect(st->font_btn, "font-set", G_CALLBACK(on_term_font_set), st);
	GtkWidget *fdef = gtk_button_new_with_label("Default");
	g_signal_connect(fdef, "clicked", G_CALLBACK(on_term_font_default), st);
	gtk_box_append(GTK_BOX(frow), st->font_btn);
	gtk_box_append(GTK_BOX(frow), fdef);
	gtk_box_append(GTK_BOX(page), frow);
	gtk_box_append(GTK_BOX(page), hint_label("Nerd Font icons always work as a fallback."));

	/* --- command preview --- */
	section_label(page, "Command preview highlights");
	for (int i = TC_TOUCH; i <= TC_REMOVE; i++) gtk_box_append(GTK_BOX(page), theme_color_row(st, i));

	GtkWidget *reset = gtk_button_new_with_label("Reset to defaults");
	gtk_widget_set_margin_top(reset, 6);
	g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_theme), st);
	gtk_box_append(GTK_BOX(page), reset);

	/* connect the two drop-downs last: filling them in must not look like the user choosing */
	st->syncing = TRUE;
	theme_sync_widgets(st);
	st->syncing = FALSE;
	g_signal_connect(st->theme_dd, "notify::selected", G_CALLBACK(on_theme_selected), st);
	g_signal_connect(st->scheme_dd, "notify::selected", G_CALLBACK(on_scheme_changed), st);
	return page;
}

static void
cheat_row(GtkWidget *grid, int row, const char *keys, const char *what)
{
	GtkWidget *k = gtk_label_new(keys);
	GtkWidget *w = gtk_label_new(what);
	gtk_widget_add_css_class(k, "fhi-key");
	gtk_widget_set_halign(k, GTK_ALIGN_START);
	gtk_widget_set_valign(k, GTK_ALIGN_START);
	gtk_label_set_xalign(GTK_LABEL(w), 0);
	gtk_label_set_wrap(GTK_LABEL(w), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(w), 28);
	gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
}

static GtkWidget *
build_cheat_page(State *st, GtkWidget *stack)
{
	GtkWidget *page = sub_page(stack, "Cheat sheet");
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

	static const char *const cheats[][2] = {
		{ "Shift + ←", "Move to the file list" },
		{ "Shift + →", "Move to the terminal" },
		{ "↑  ↓", "Move through the files (file list)" },
		{ "Enter", "Open the file, or cd into the folder; your typed command is kept. In vim / nano a text file is opened there instead, after saving the current one (file list)" },
		{ "→ / ←", "Open / close the highlighted folder in place (file list)" },
		{ "Backspace", "Go up one folder (cd ..) (file list)" },
		{ "Shift + Enter", "Paste the file's path into the terminal: quoted for the shell, plain in vim / nano, a string in python (file list)" },
		{ "Ctrl + Shift + C / V", "Copy the terminal selection / paste into the terminal" },
		{ "Double click", "Same as Enter" },
		{ "Drag a file", "Drop it into a browser, Discord, a file manager... (copies it)" },
		{ "Shift + ↑ / ↓", "Next / previous sort mode (file list)" },
		{ "Ctrl + R", "Reverse the sort order (file list)" },
		{ "Just type", "Filter the list with shell patterns: *.png  {a,b}*  !*.o  (file list)" },
		{ "Enter / Esc", "In the filter: keep it and go back to the list / clear it and close it" },
		{ "Tab", "Open this menu (file list)" },
		{ "Esc", "Close this menu" },
	};
	for (guint i = 0; i < G_N_ELEMENTS(cheats); i++) cheat_row(grid, i, cheats[i][0], cheats[i][1]);
	gtk_box_append(GTK_BOX(page), grid);

	GtkWidget *hl = gtk_label_new("Highlight colors while typing");
	gtk_label_set_xalign(GTK_LABEL(hl), 0);
	gtk_widget_add_css_class(hl, "dim-label");
	gtk_box_append(GTK_BOX(page), hl);
	const char *const classes[] = { "fhi-touch", "fhi-create", "fhi-dest", "fhi-remove" };
	const char *const meaning[] = { "mentioned / modified", "will be created",
	                                "destination of mv / cp / ln", "will be deleted" };
	for (int i = 0; i < 4; i++) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		GtkWidget *swatch = gtk_label_new(NULL);
		gtk_widget_add_css_class(swatch, "fhi-swatch");
		gtk_widget_add_css_class(swatch, classes[i]);
		gtk_widget_set_valign(swatch, GTK_ALIGN_CENTER);
		GtkWidget *l = gtk_label_new(meaning[i]);
		gtk_box_append(GTK_BOX(row), swatch);
		gtk_box_append(GTK_BOX(row), l);
		gtk_box_append(GTK_BOX(page), row);
	}
	return page;
}

static void
on_font_changed(GtkSpinButton *spin, gpointer user_data)
{
	State *st = user_data;
	st->cfg.info_font = gtk_spin_button_get_value_as_int(spin);
	apply_all(st, TRUE);
}

static GtkWidget *
build_info_page(State *st, GtkWidget *stack)
{
	GtkWidget *page = sub_page(stack, "File info bar");
	GtkWidget *l = gtk_label_new("Show for the selected file");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_box_append(GTK_BOX(page), l);
	gtk_box_append(GTK_BOX(page), check_row(st, "Type (detected from the file header)", &st->cfg.info_type));
	gtk_box_append(GTK_BOX(page), check_row(st, "Size", &st->cfg.info_size));
	gtk_box_append(GTK_BOX(page), check_row(st, "Date created", &st->cfg.info_created));
	gtk_box_append(GTK_BOX(page), check_row(st, "Last accessed", &st->cfg.info_accessed));
	gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(page), switch_row(st, "Wrap long info (off = cut off)", &st->cfg.info_wrap));

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *label = gtk_label_new("Font size (pt)");
	GtkWidget *spin = gtk_spin_button_new_with_range(6, 32, 1);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), st->cfg.info_font);
	g_signal_connect(spin, "value-changed", G_CALLBACK(on_font_changed), st);
	gtk_box_append(GTK_BOX(row), label);
	gtk_box_append(GTK_BOX(row), spin);
	gtk_box_append(GTK_BOX(page), row);
	return page;
}

static void
on_sort_changed(GObject *dd, GParamSpec *pspec, gpointer user_data)
{
	State *st = user_data;
	st->cfg.sort_mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	apply_all(st, TRUE);
}

/* Keyboard / button: go to the next (+1) or previous (-1) sort mode */
static void
sort_cycle(State *st, int step)
{
	int mode = (st->cfg.sort_mode + step + SORT_COUNT) % SORT_COUNT;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(st->sort_dd), mode);   /* fires on_sort_changed */
}

static void
sort_toggle_reverse(State *st)
{
	gtk_switch_set_active(GTK_SWITCH(st->reverse_switch), !st->cfg.sort_reverse);
}

static GtkWidget *
build_sort_page(State *st, GtkWidget *stack)
{
	GtkWidget *page = sub_page(stack, "Sorting");

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *label = gtk_label_new("Sort by");
	st->sort_dd = gtk_drop_down_new_from_strings(sort_name);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(st->sort_dd), st->cfg.sort_mode);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_hexpand(label, TRUE);
	g_signal_connect(st->sort_dd, "notify::selected", G_CALLBACK(on_sort_changed), st);
	gtk_box_append(GTK_BOX(row), label);
	gtk_box_append(GTK_BOX(row), st->sort_dd);
	gtk_box_append(GTK_BOX(page), row);

	GtkWidget *rev = switch_row(st, "Reverse order", &st->cfg.sort_reverse);
	st->reverse_switch = gtk_widget_get_last_child(rev);
	gtk_box_append(GTK_BOX(page), rev);
	gtk_box_append(GTK_BOX(page), switch_row(st, "Folders first", &st->cfg.folders_first));

	GtkWidget *hint = gtk_label_new("Size and dates start with the largest / newest.\n"
	                                "In the file list: Shift+↑ / Shift+↓ change the mode, Ctrl+R reverses.");
	gtk_label_set_xalign(GTK_LABEL(hint), 0);
	gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(hint), 34);
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_box_append(GTK_BOX(page), hint);
	return page;
}

/* The filter box changed: rebuild the filter from what was typed */
static void
on_filter_changed(GtkEditable *entry, gpointer user_data)
{
	State *st = user_data;
	fhi_filter_free(st->filter);
	const char *text = gtk_editable_get_text(entry);
	st->filter = *text ? fhi_filter_new(text) : NULL;
	gtk_filter_changed(st->hidden_filter, GTK_FILTER_CHANGE_DIFFERENT);
	update_info(st);

	/* Nothing left to filter by (backspaced to empty, or cleared): hide the box.
	 * Only move focus if the box had it, e.g. not when Esc already did. */
	if (*text == '\0' && gtk_widget_get_visible(st->filter_bar)) {
		GtkWidget *cur = gtk_window_get_focus(GTK_WINDOW(st->window));
		gboolean had_focus = cur && (cur == st->filter_entry || gtk_widget_is_ancestor(cur, st->filter_entry));
		gtk_widget_set_visible(st->filter_bar, FALSE);
		if (had_focus) gtk_widget_grab_focus(st->list_view);
	}
}

/* Show the filter box (it is hidden until you type) and put `chars` in it */
static void
filter_begin(State *st, const char *chars)
{
	gtk_widget_set_visible(st->filter_bar, TRUE);
	gtk_widget_grab_focus(st->filter_entry);
	gtk_editable_set_position(GTK_EDITABLE(st->filter_entry), -1);   /* grab_focus selects all: undo that */
	if (chars) {
		int pos = -1;
		gtk_editable_insert_text(GTK_EDITABLE(st->filter_entry), chars, -1, &pos);
		gtk_editable_set_position(GTK_EDITABLE(st->filter_entry), -1);
	}
}

/* Back to the list. `clear`: also drop the filter and hide the box. */
static void
filter_end(State *st, gboolean clear)
{
	if (clear) {
		gtk_editable_set_text(GTK_EDITABLE(st->filter_entry), "");   /* fires on_filter_changed */
		gtk_widget_set_visible(st->filter_bar, FALSE);
	}
	gtk_widget_grab_focus(st->list_view);
}

/* Enter in the filter box: keep the filter and go back to the list (an empty one just closes) */
static void
on_filter_activate(GtkWidget *entry, gpointer user_data)
{
	State *st = user_data;
	filter_end(st, *gtk_editable_get_text(GTK_EDITABLE(entry)) == '\0');
}

/* Remember whether the list or the terminal had focus, to give it back on close */
static void
on_focus_changed(GObject *window, GParamSpec *pspec, gpointer user_data)
{
	State *st = user_data;
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));
	if (focus == NULL) return;
	if (focus == st->list_view || gtk_widget_is_ancestor(focus, st->list_view))
		st->last_pane = st->list_view;
	else if (focus == st->terminal || gtk_widget_is_ancestor(focus, st->terminal))
		st->last_pane = st->terminal;
}

/* Show or hide the menu panel. It lives INSIDE the window (an overlay), so it
 * can never spill out of it the way a popup window can. */
static void
menu_set_open(State *st, gboolean open)
{
	if (gtk_widget_get_visible(st->menu_layer) == open) return;
	gtk_widget_set_visible(st->menu_layer, open);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->menu_button), open);   /* keep the button in sync */
	if (open) {
		gtk_stack_set_transition_type(GTK_STACK(st->menu_stack), GTK_STACK_TRANSITION_TYPE_NONE);
		gtk_stack_set_visible_child_name(GTK_STACK(st->menu_stack), "main");
		gtk_stack_set_transition_type(GTK_STACK(st->menu_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
		gtk_widget_grab_focus(st->menu_first);
	} else {
		gtk_widget_grab_focus(st->last_pane ? st->last_pane : st->terminal);
	}
}

static void
on_menu_button_toggled(GtkToggleButton *button, gpointer user_data)
{
	menu_set_open(user_data, gtk_toggle_button_get_active(button));
}

/* A click on the part of the window next to the panel closes it */
static void
on_scrim_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
	menu_set_open(user_data, FALSE);
}

/* The title bar button. build_menu_layer() makes the panel it opens. */
static GtkWidget *
build_menu_button(State *st)
{
	GtkWidget *button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(button), "open-menu-symbolic");
	gtk_widget_set_focus_on_click(button, FALSE);   /* keep focus where it was */
	g_signal_connect(button, "toggled", G_CALLBACK(on_menu_button_toggled), st);
	st->menu_button = button;
	return button;
}

static GtkWidget *
build_menu_layer(State *st)
{
	GtkWidget *stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	gtk_stack_set_vhomogeneous(GTK_STACK(stack), FALSE);
	st->menu_stack = stack;

	GtkWidget *main_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *first = nav_button(stack, "Theme", "theme", TRUE);
	st->menu_first = first;
	gtk_box_append(GTK_BOX(main_page), first);
	gtk_box_append(GTK_BOX(main_page), nav_button(stack, "Cheat sheet", "cheat", TRUE));
	gtk_box_append(GTK_BOX(main_page), nav_button(stack, "Sorting", "sort", TRUE));
	gtk_box_append(GTK_BOX(main_page), nav_button(stack, "File info bar", "info", TRUE));
	gtk_box_append(GTK_BOX(main_page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	GtkWidget *hidden = switch_row(st, "Show hidden files", &st->cfg.show_hidden);
	gtk_widget_set_margin_start(hidden, 12);
	gtk_widget_set_margin_end(hidden, 12);
	gtk_widget_set_margin_top(hidden, 4);
	gtk_widget_set_margin_bottom(hidden, 4);
	gtk_box_append(GTK_BOX(main_page), hidden);

	gtk_stack_add_named(GTK_STACK(stack), main_page, "main");
	gtk_stack_add_named(GTK_STACK(stack), build_theme_page(st, stack), "theme");
	gtk_stack_add_named(GTK_STACK(stack), build_cheat_page(st, stack), "cheat");
	gtk_stack_add_named(GTK_STACK(stack), build_sort_page(st, stack), "sort");
	gtk_stack_add_named(GTK_STACK(stack), build_info_page(st, stack), "info");

	/* The panel is as tall as the window; a tall page scrolls instead of spilling out */
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), stack);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), TRUE);
	gtk_widget_set_size_request(scroller, 300, -1);
	gtk_widget_add_css_class(scroller, "fhi-menu");

	/* transparent area to the right of the panel: clicking it closes the menu */
	GtkWidget *scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_hexpand(scrim, TRUE);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(on_scrim_pressed), st);
	gtk_widget_add_controller(scrim, GTK_EVENT_CONTROLLER(click));

	GtkWidget *layer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(layer), scroller);
	gtk_box_append(GTK_BOX(layer), scrim);
	gtk_widget_set_visible(layer, FALSE);
	st->menu_layer = layer;
	return layer;
}

/* ---- ~/.config/fhi/user.css: the user's own CSS, last word on everything ---- */
static void user_css_setup(State *st);

static void
on_user_css_error(GtkCssProvider *provider, GtkCssSection *section, GError *error, gpointer user_data)
{
	const GtkCssLocation *at = gtk_css_section_get_start_location(section);
	g_printerr("fhi: user.css line %u: %s\n", (unsigned)at->lines + 1, error->message);
}

static void
user_css_load(State *st)
{
	char *path = user_css_path();
	if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) gtk_css_provider_load_from_path(st->user_css, path);
	else gtk_css_provider_load_from_data(st->user_css, "", 0);
	g_free(path);
}

static void
on_user_css_changed(GFileMonitor *mon, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer user_data)
{
	if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED ||
	    ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_MOVED_IN ||
	    ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_OUT)
		user_css_load(user_data);
}

static void
user_css_setup(State *st)
{
	st->user_css = gtk_css_provider_new();
	g_signal_connect(st->user_css, "parsing-error", G_CALLBACK(on_user_css_error), NULL);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
	    GTK_STYLE_PROVIDER(st->user_css), GTK_STYLE_PROVIDER_PRIORITY_USER);
	user_css_load(st);

	char *path = user_css_path();
	GFile *f = g_file_new_for_path(path);
	st->user_css_mon = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);   /* NULL if it cannot: no live reload, nothing else lost */
	if (st->user_css_mon) g_signal_connect(st->user_css_mon, "changed", G_CALLBACK(on_user_css_changed), st);
	g_object_unref(f);
	g_free(path);
}

static void
activate(GtkApplication *app, gpointer user_data)
{
	GtkWidget *window = gtk_application_window_new(app);

	/* One State shared by all callbacks. (Never freed: it lives as long as the
	 * program, and the OS cleans up when we exit.) */
	State *st = g_new0(State, 1);
	st->window = window;
	st->notebook = gtk_notebook_new();   /* the tabs; see tab_new() */
	st->roles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	st->llm_act = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	st->bound_rows = g_hash_table_new(g_direct_hash, g_direct_equal);
	st->ghosts = g_list_store_new(G_TYPE_FILE_INFO);
	st->peeks = g_list_store_new(G_TYPE_FILE_INFO);
	st->expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	st->peek_info = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

	/* Settings, then the style sheet that depends on them (see apply_all) */
	settings_defaults(&st->cfg);
	load_settings(st);
	g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
	             &st->system_dark, NULL);
	st->css = gtk_css_provider_new();
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
	    GTK_STYLE_PROVIDER(st->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	user_css_setup(st);
	GtkWidget *pane = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
	GtkWidget *frame1 = gtk_frame_new (NULL);
	GtkWidget *frame2 = gtk_frame_new (NULL);

	gtk_widget_set_size_request (pane, 200, -1);

	gtk_paned_set_start_child (GTK_PANED (pane), frame1);
	gtk_paned_set_resize_start_child (GTK_PANED (pane), TRUE);
	gtk_paned_set_shrink_start_child (GTK_PANED (pane), FALSE);
	gtk_widget_set_size_request (frame1, 100, 300);

	gtk_paned_set_end_child (GTK_PANED (pane), frame2);
	gtk_paned_set_resize_end_child (GTK_PANED (pane), TRUE);
	gtk_paned_set_shrink_end_child (GTK_PANED (pane), FALSE);
	gtk_widget_set_size_request (frame2, 400, 300);

	
	gtk_window_set_title(GTK_WINDOW(window), "FHI-Terminal");

	/* Window icon: icons/fhi.png is found as an "unthemed" icon in that folder */
	gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), FHI_ICON_DIR);
	gtk_window_set_icon_name(GTK_WINDOW(window), "fhi");

	/* File list of the home directory */
	GFile *home = g_file_new_for_path(g_get_home_dir());
	GtkDirectoryList *dir_list = gtk_directory_list_new(FHI_ATTRS, home);
	/* watch the folder, so files created or deleted by commands show up by themselves */
	gtk_directory_list_set_monitored(dir_list, TRUE);
	g_object_unref(home);
	st->dir_list = dir_list;

	g_signal_connect(dir_list, "items-changed", G_CALLBACK(on_dir_items_changed), st);

	GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(compare_info, st, NULL));
	st->sorter = sorter;

	/* The list shows the real files AND the ghost rows, merged into one model */
	GListStore *sources = g_list_store_new(G_TYPE_OBJECT);   /* holds the two models */
	g_list_store_append(sources, dir_list);
	g_list_store_append(sources, st->ghosts);
	g_list_store_append(sources, st->peeks);
	GtkFlattenListModel *merged = gtk_flatten_list_model_new(G_LIST_MODEL(sources));

	/* dotfiles can be hidden from the menu; the filter is re-run by apply_all() */
	st->hidden_filter = GTK_FILTER(gtk_custom_filter_new(hidden_filter_func, st, NULL));
	GtkFilterListModel *visible = gtk_filter_list_model_new(G_LIST_MODEL(merged), st->hidden_filter);

	GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(visible), sorter);
	GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(sorted));
	st->selection = selection;

	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(setup_item), st);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_item), st);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_item), st);

	GtkWidget *list_view = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
	st->list_view = list_view;
	/* Enter or double click on a row */
	g_signal_connect(list_view, "activate", G_CALLBACK(on_row_activated), st);

	/* Window-wide shortcuts (Shift+Left/Right, Shift+Enter) */
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), st);
	gtk_widget_add_controller(window, keys);

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_view);
	gtk_widget_set_vexpand(scroller, TRUE);

	/* Left panel = file list on top, info bar about the selected file below it */
	st->info_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(st->info_label), 0);
	gtk_label_set_wrap_mode(GTK_LABEL(st->info_label), PANGO_WRAP_WORD_CHAR);
	gtk_widget_add_css_class(st->info_label, "fhi-info");
	/* Filter box: hidden until you type in the list */
	st->filter_entry = gtk_search_entry_new();
	gtk_widget_set_hexpand(st->filter_entry, TRUE);
	/* Ask for almost no width of its own: the box then fills the panel instead of
	 * widening it, and a long filter scrolls sideways inside the box. */
	gtk_editable_set_width_chars(GTK_EDITABLE(st->filter_entry), 1);
	gtk_editable_set_max_width_chars(GTK_EDITABLE(st->filter_entry), 1);
	gtk_widget_set_tooltip_text(st->filter_entry,
	    "Shell patterns: *.png  report?  {a,b}*  !*.o\nEnter keeps the filter, Esc clears it");
	g_signal_connect(st->filter_entry, "changed", G_CALLBACK(on_filter_changed), st);
	g_signal_connect(st->filter_entry, "activate", G_CALLBACK(on_filter_activate), st);
	st->filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(st->filter_bar, "fhi-top");
	gtk_box_append(GTK_BOX(st->filter_bar), st->filter_entry);
	gtk_widget_set_visible(st->filter_bar, FALSE);

	/* Bottom row: file info, and a faint sort hint on the right */
	gtk_widget_set_hexpand(st->info_label, TRUE);
	st->sort_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(st->sort_label, "fhi-sort");
	gtk_widget_set_visible(st->sort_label, FALSE);
	st->llm_label = gtk_label_new(NULL);   /* "Claude Code" while an assistant runs */
	gtk_widget_add_css_class(st->llm_label, "fhi-sort");
	gtk_widget_set_visible(st->llm_label, FALSE);
	GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(bottom), st->info_label);
	gtk_box_append(GTK_BOX(bottom), st->llm_label);
	gtk_box_append(GTK_BOX(bottom), st->sort_label);

	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(left), scroller);
	gtk_box_append(GTK_BOX(left), st->filter_bar);
	gtk_box_append(GTK_BOX(left), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(left), bottom);
	g_signal_connect(selection, "notify::selected-item", G_CALLBACK(on_selection_changed), st);

	/* Hamburger button in the title bar */
	GtkWidget *header = gtk_header_bar_new();
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), build_menu_button(st));
	gtk_window_set_titlebar(GTK_WINDOW(window), header);
	g_signal_connect(window, "notify::focus-widget", G_CALLBACK(on_focus_changed), st);

	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), pane);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), build_menu_layer(st));
	gtk_window_set_child(GTK_WINDOW(window), overlay);
	/* The panel sits in an overlay so a message can float over it when it is dimmed */
	st->left_panel = left;
	gtk_widget_add_css_class(left, "fhi-leftpanel");   /* the one layer that carries the panel's transparency */
	st->unsupported_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(st->unsupported_label, "fhi-unsupported");
	gtk_widget_set_halign(st->unsupported_label, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(st->unsupported_label, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(st->unsupported_label, FALSE);
	gtk_widget_set_visible(st->unsupported_label, FALSE);
	GtkWidget *left_overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(left_overlay), left);
	gtk_overlay_add_overlay(GTK_OVERLAY(left_overlay), st->unsupported_label);
	st->ssh_card = fhi_ssh_card_new();
	gtk_overlay_add_overlay(GTK_OVERLAY(left_overlay), st->ssh_card);
	g_timeout_add(500, check_shell_support, st);
	gtk_widget_add_css_class(frame1, "fhi-square");   /* no rounded corners on the two panels */
	gtk_widget_add_css_class(frame2, "fhi-square");
	gtk_frame_set_child(GTK_FRAME(frame1), left_overlay);
	/* Tabs: the first one now (it starts the shell), then the "+" button and the switch signal */
	GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);
	gtk_notebook_set_scrollable(nb, TRUE);
	gtk_notebook_set_show_border(nb, FALSE);
	tab_new(st, NULL, FALSE);
	GtkWidget *plus = gtk_button_new_from_icon_name("list-add-symbolic");
	gtk_button_set_has_frame(GTK_BUTTON(plus), FALSE);
	gtk_widget_set_tooltip_text(plus, "New tab (Ctrl+Shift+T)");
	g_signal_connect(plus, "clicked", G_CALLBACK(on_new_tab_clicked), st);
	gtk_notebook_set_action_widget(nb, plus, GTK_PACK_END);
	g_signal_connect(nb, "switch-page", G_CALLBACK(on_tab_switched), st);
	gtk_frame_set_child(GTK_FRAME(frame2), st->notebook);
	apply_all(st, FALSE);
	gtk_window_present(GTK_WINDOW(window));
	gtk_window_set_default_size (GTK_WINDOW(window), 800, 600);

}

int
main(int argc, char *argv[])
{
	fhi_icon_font_register(FHI_FONT_DIR "/SymbolsNerdFont-Regular.ttf");
	GtkApplication *app = gtk_application_new("dev.forcedev.myterm", G_APPLICATION_NON_UNIQUE);
	/* NON_UNIQUE: every launch is its own process. With the default, starting FHI
	 * while another one runs only opens a window inside the OLD process, so a
	 * freshly built version would silently never start. */
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
