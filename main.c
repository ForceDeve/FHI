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
	const char *shell = g_getenv("SHELL");
	char *command[] = { (char *)(shell ? shell : "/bin/sh"), NULL };

	vte_terminal_spawn_async(VTE_TERMINAL(terminal),
	    VTE_PTY_DEFAULT,
	    g_get_home_dir(), /* working directory  */
	    command,      /* command */
	    NULL,         /* environment */
	    G_SPAWN_DEFAULT, /* spawn flags */
	    NULL, NULL, NULL, /* child setup, data, destroy */
	    -1,           /* timeout */
	    NULL,         /* cancellable */
	    child_ready,  /* callback */
	    window);      /* user_data */

	g_signal_connect(terminal, "child-exited", G_CALLBACK(child_exited), window);

	/* File list of the home directory */
	GFile *home = g_file_new_for_path(g_get_home_dir());
	GtkDirectoryList *dir_list = gtk_directory_list_new("standard::display-name,standard::icon", home);
	g_object_unref(home);

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
