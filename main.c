#include <gtk/gtk.h>
#include <vte/vte.h>



static void
child_ready(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
	if (error) {
		g_printerr("failed to spawn shell: %s\n", error->message);
		g_clear_error(&error);
	}
	if (pid == -1) gtk_window_destroy(GTK_WINDOW(user_data));
}

static void
child_exited(VteTerminal *terminal, int status, gpointer user_data)
{
	gtk_window_destroy(GTK_WINDOW(user_data));
}

static void
setup_item(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *icon = gtk_image_new();
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), icon);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(item, box);
}

static void
bind_item(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GFileInfo *info = G_FILE_INFO(gtk_list_item_get_item(item));
	GtkWidget *box = gtk_list_item_get_child(item);
	GtkWidget *icon = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_last_child(box);

	gtk_image_set_from_gicon(GTK_IMAGE(icon), g_file_info_get_icon(info));
	gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));
}

static int
compare_info(gconstpointer a, gconstpointer b, gpointer user_data)
{
	return g_utf8_collate(g_file_info_get_display_name(G_FILE_INFO((gpointer)a)),
	                      g_file_info_get_display_name(G_FILE_INFO((gpointer)b)));
}

/*
 * YOUR TURN: called by VTE whenever the shell reports a new working directory.
 *
 * Who calls it?  GTK does, from inside the main loop. It runs because
 * activate() connected it to the terminal's "current-directory-uri-changed"
 * signal, and that signal fires when zsh prints the OSC 7 escape sequence
 * from zsh/.zshrc (once before every prompt).
 *
 * Arguments:
 *   terminal  - the VteTerminal that emitted the signal
 *   user_data - whatever was passed as the last argument of g_signal_connect().
 *               In activate() that is the GtkDirectoryList that feeds the file
 *               list on the left.
 */
static void
on_cwd_changed(VteTerminal *terminal, gpointer user_data)
{
	GtkDirectoryList *list = GTK_DIRECTORY_LIST(user_data);

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

static void
activate(GtkApplication *app, gpointer user_data)
{
	GtkWidget *window = gtk_application_window_new(app);
	GtkWidget *terminal = vte_terminal_new();
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
	    window);      /* user_data */
	g_strfreev(envp); /* VTE made its own copy */

	g_signal_connect(terminal, "child-exited", G_CALLBACK(child_exited), window);

	/* File list of the home directory */
	GFile *home = g_file_new_for_path(g_get_home_dir());
	GtkDirectoryList *dir_list = gtk_directory_list_new("standard::name,standard::display-name,standard::icon", home);
	g_object_unref(home);

	/* Update the list whenever the shell reports a new directory */
	g_signal_connect(terminal, "current-directory-uri-changed", G_CALLBACK(on_cwd_changed), dir_list);

	GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(compare_info, NULL, NULL));
	GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(dir_list), sorter);
	GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(sorted));

	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(setup_item), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_item), NULL);

	GtkWidget *list_view = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
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
