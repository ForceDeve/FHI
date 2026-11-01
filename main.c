#include <gtk/gtk.h>
#include <vte/vte.h>
#include <string.h>
#include <unistd.h> /* tcgetpgrp() */
#include "preview.h"

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
	GPid shell_pid;                  /* process id of zsh, 0 until it started */

	/* --- command preview (highlighting files the typed command affects) --- */
	GListStore *ghosts;              /* fake rows for files that do not exist yet */
	GHashTable *roles;               /* file name -> FhiRole, for the highlighting */
	GHashTable *bound_rows;          /* the row widgets currently on screen */
	char *last_buffer;               /* the last command line text we received */
	char *ghost_key;                 /* names of the current ghosts, to spot changes */
	guint preview_idle;              /* id of a scheduled update_preview(), 0 if none */
	guint resync_timer;              /* id of a scheduled resync_directory(), 0 if none */
} State;

static void
child_ready(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
	State *st = user_data;
	if (error) {
		g_printerr("failed to spawn shell: %s\n", error->message);
		g_clear_error(&error);
	}
	if (pid == -1) gtk_window_destroy(GTK_WINDOW(st->window));
	else st->shell_pid = pid; /* remember zsh's PID, see shell_is_foreground() */
}

static void
child_exited(VteTerminal *terminal, int status, gpointer user_data)
{
	gtk_window_destroy(GTK_WINDOW(user_data));
}

/* The CSS classes used to color a row, indexed by FhiRole (see style in activate) */
static const char *const role_class[] = {
	NULL, "fhi-touch", "fhi-create", "fhi-dest", "fhi-remove"
};

/* Give one row the color that matches what the typed command does to its file. */
static void
apply_row_style(State *st, GtkWidget *box)
{
	for (guint i = 1; i < G_N_ELEMENTS(role_class); i++)
		gtk_widget_remove_css_class(box, role_class[i]);

	const char *name = g_object_get_data(G_OBJECT(box), "fhi-name");
	if (name == NULL) return;
	FhiRole role = GPOINTER_TO_INT(g_hash_table_lookup(st->roles, name));
	if (role != FHI_ROLE_NONE) gtk_widget_add_css_class(box, role_class[role]);
}

static void
refresh_all_rows(State *st)
{
	GHashTableIter it;
	gpointer box;
	g_hash_table_iter_init(&it, st->bound_rows);
	while (g_hash_table_iter_next(&it, &box, NULL)) apply_row_style(st, box);
}

/* "setup": GTK needs an empty row. Called once per row WIDGET (they get reused). */
static void
setup_item(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *icon = gtk_image_new();
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(box, "fhi-row");
	gtk_box_append(GTK_BOX(box), icon);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(item, box);
}

/* "bind": a row widget now has to show this particular file. */
static void
bind_item(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	State *st = user_data;
	GFileInfo *info = G_FILE_INFO(gtk_list_item_get_item(item));
	GtkWidget *box = gtk_list_item_get_child(item);
	GtkWidget *icon = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_last_child(box);

	gtk_image_set_from_gicon(GTK_IMAGE(icon), g_file_info_get_icon(info));
	gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));

	/* Remember which file this row shows, and keep track of the row, so that
	 * update_preview() can recolor it later without GTK asking again. */
	g_object_set_data_full(G_OBJECT(box), "fhi-name",
	                       g_strdup(g_file_info_get_name(info)), g_free);
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

static int
compare_info(gconstpointer a, gconstpointer b, gpointer user_data)
{
	return g_utf8_collate(g_file_info_get_display_name(G_FILE_INFO((gpointer)a)),
	                      g_file_info_get_display_name(G_FILE_INFO((gpointer)b)));
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

	GPtrArray *hits = fhi_preview_parse(copy, cwd, (const char *const *)names->pdata);

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
	const char *title = vte_terminal_get_window_title(terminal);
	if (title == NULL) return;

	/* "FHI-PROMPT:n" is sent by zsh each time a command has finished and the
	 * prompt is back. Wait a moment, so GTK can finish its own file watching
	 * first, then check that the list is right. */
	if (g_str_has_prefix(title, "FHI-PROMPT:")) {
		if (st->resync_timer == 0) st->resync_timer = g_timeout_add(300, resync_directory, st);
		return;
	}

	if (!g_str_has_prefix(title, "FHI-BUF:")) return;

	char *buffer = g_uri_unescape_string(title + strlen("FHI-BUF:"), NULL);   /* undo %0A etc. */
	if (buffer == NULL) return;
	update_preview(st, buffer);
	g_free(buffer);
}

/* Runs once GTK is idle, i.e. after it has finished handling the file change. */
static gboolean
preview_idle_cb(gpointer user_data)
{
	State *st = user_data;
	st->preview_idle = 0;
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

static void
on_cwd_changed(VteTerminal *terminal, gpointer user_data)
{
	State *st = user_data;
	GtkDirectoryList *list = st->dir_list;

	/* VTE owns this string, so we must not free it. NULL means "unknown". */
	const char *uri = vte_terminal_get_current_directory_uri(terminal);
	if (uri == NULL) return;

	/* g_file_new_for_uri() decodes "file://" and the %20-style escapes for us */
	GFile *file = g_file_new_for_uri(uri);

	/* The signal fires before every prompt, even if we did not change folder.
	 * Only reload the list when the directory really is a different one. */
	GFile *current = gtk_directory_list_get_file(list); /* not ours: do not unref */
	if (current == NULL || !g_file_equal(current, file))
		gtk_directory_list_set_file(list, file);

	/* We created "file", so we release it. The list took its own reference.
	 * (We never unref "list": we did not create it, the sort model owns it.) */
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
static gboolean
shell_is_foreground(State *st)
{
	VtePty *pty = vte_terminal_get_pty(VTE_TERMINAL(st->terminal));
	if (pty == NULL || st->shell_pid <= 0) return FALSE;
	return tcgetpgrp(vte_pty_get_fd(pty)) == st->shell_pid;
}

/* Send text to the shell exactly as if the user had typed it. */
static void
type_into_terminal(State *st, const char *text)
{
	vte_terminal_feed_child(VTE_TERMINAL(st->terminal), text, -1);
}

/*
 * Enter (or double click) on a row of the file list.
 * GtkListView emits "activate" with the row number ("position").
 *   - directory -> make the terminal cd into it (the list follows by itself,
 *                  through on_cwd_changed, once the next prompt appears)
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
				type_into_terminal(st, text);
				g_free(text);
				g_free(quoted);
				g_free(path);
			}
		} else {
			char *uri = g_file_get_uri(file);
			GError *error = NULL;
			if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
				g_printerr("could not open %s: %s\n", uri, error->message);
				g_clear_error(&error);
			}
			g_free(uri);
		}
	}

	g_object_unref(info);
}

/* Shift+Enter: type the highlighted file's path at the end of the command line. */
static void
paste_selected_path(State *st)
{
	if (!shell_is_foreground(st)) return;

	/* "selected item" is owned by the selection model: do not unref it */
	GFileInfo *info = gtk_single_selection_get_selected_item(st->selection);
	if (info == NULL) return;
	GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
	if (file == NULL) return;

	char *path = g_file_get_path(file);
	char *quoted = g_shell_quote(path);
	char *text = g_strdup_printf(" %s ", quoted); /* trailing space for the next word */
	type_into_terminal(st, text);
	g_free(text);
	g_free(quoted);
	g_free(path);

	/* The user will want to keep typing the command, so go back to the terminal */
	gtk_widget_grab_focus(st->terminal);
}

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
	 * like NumLock, then require that Shift is the only one held down. */
	modifiers &= gtk_accelerator_get_default_mod_mask();
	if (modifiers != GDK_SHIFT_MASK) return FALSE;

	switch (keyval) {
	case GDK_KEY_Left:  /* Shift+Left: go to the file list */
		gtk_widget_grab_focus(st->list_view);
		return TRUE;
	case GDK_KEY_Right: /* Shift+Right: go to the terminal */
		gtk_widget_grab_focus(st->terminal);
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

static void
activate(GtkApplication *app, gpointer user_data)
{
	GtkWidget *window = gtk_application_window_new(app);
	GtkWidget *terminal = vte_terminal_new();

	/* One State shared by all callbacks. (Never freed: it lives as long as the
	 * program, and the OS cleans up when we exit.) */
	State *st = g_new0(State, 1);
	st->window = window;
	st->terminal = terminal;
	st->roles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	st->bound_rows = g_hash_table_new(g_direct_hash, g_direct_equal);
	st->ghosts = g_list_store_new(G_TYPE_FILE_INFO);

	/* Colors for the command preview. alpha() makes them see-through, so they
	 * look right in light and dark themes. */
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css,
	    ".fhi-row    { padding: 2px 6px; border-radius: 4px; }"
	    ".fhi-touch  { background: alpha(#e5a50a, 0.30); }"   /* yellow: mentioned     */
	    ".fhi-create { background: alpha(#26a269, 0.40); }"   /* green:  created       */
	    ".fhi-dest   { background: alpha(#3584e4, 0.40); }"   /* blue:   destination   */
	    ".fhi-remove { background: alpha(#e01b24, 0.40); }"   /* red:    removed       */
	    ".fhi-ghost label { font-style: italic; }", -1);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
	    GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
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

	/* Start a new shell */
	char *command[] = { "zsh", NULL };

	/*
	 * Make zsh load OUR startup files (see zsh/.zshrc), which in turn load the
	 * user's own ones and add the hook that reports the working directory.
	 *   - FHI_USER_ZDOTDIR: where the user's files live (their ZDOTDIR if they
	 *     set one, otherwise their home directory)
	 *   - FHI_ZDOTDIR / ZDOTDIR: our folder (the path is baked in by CMake)
	 */
	char **envp = g_get_environ();
	char *user_zdotdir = g_strdup(g_environ_getenv(envp, "ZDOTDIR"));
	if (!user_zdotdir) user_zdotdir = g_strdup(g_get_home_dir());
	envp = g_environ_setenv(envp, "FHI_USER_ZDOTDIR", user_zdotdir, TRUE);
	envp = g_environ_setenv(envp, "FHI_ZDOTDIR", FHI_ZSH_DIR, TRUE);
	envp = g_environ_setenv(envp, "ZDOTDIR", FHI_ZSH_DIR, TRUE);
	g_free(user_zdotdir);

	vte_terminal_spawn_async(VTE_TERMINAL(terminal),
	    VTE_PTY_DEFAULT,
	    g_get_home_dir(), /* working directory  */
	    command,      /* command */
	    envp,         /* environment */
	    G_SPAWN_SEARCH_PATH, /* spawn flags: look up zsh in PATH */
	    NULL, NULL, NULL, /* child setup, data, destroy */
	    -1,           /* timeout */
	    NULL,         /* cancellable */
	    child_ready,  /* callback */
	    st);          /* user_data */
	g_strfreev(envp); /* VTE made its own copy */

	g_signal_connect(terminal, "child-exited", G_CALLBACK(child_exited), window);

	/* File list of the home directory */
	GFile *home = g_file_new_for_path(g_get_home_dir());
	GtkDirectoryList *dir_list = gtk_directory_list_new("standard::name,standard::display-name,standard::icon", home);
	/* watch the folder, so files created or deleted by commands show up by themselves */
	gtk_directory_list_set_monitored(dir_list, TRUE);
	g_object_unref(home);
	st->dir_list = dir_list;

	/* Update the list whenever the shell reports a new directory */
	g_signal_connect(terminal, "current-directory-uri-changed", G_CALLBACK(on_cwd_changed), st);

	/* The command being typed arrives through the terminal title (see zsh/.zshrc) */
	g_signal_connect(terminal, "window-title-changed", G_CALLBACK(on_title_changed), st);
	g_signal_connect(dir_list, "items-changed", G_CALLBACK(on_dir_items_changed), st);

	GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(compare_info, NULL, NULL));

	/* The list shows the real files AND the ghost rows, merged into one model */
	GListStore *sources = g_list_store_new(G_TYPE_OBJECT);   /* holds the two models */
	g_list_store_append(sources, dir_list);
	g_list_store_append(sources, st->ghosts);
	GtkFlattenListModel *merged = gtk_flatten_list_model_new(G_LIST_MODEL(sources));

	GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(merged), sorter);
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

	gtk_window_set_child(GTK_WINDOW(window), pane);
	gtk_frame_set_child(GTK_FRAME(frame1), scroller);
	gtk_frame_set_child(GTK_FRAME(frame2), terminal);
	gtk_window_present(GTK_WINDOW(window));
	gtk_window_set_default_size (GTK_WINDOW(window), 800, 600);

}

int
main(int argc, char *argv[])
{
	GtkApplication *app = gtk_application_new("dev.forcedev.myterm", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
