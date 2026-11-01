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
activate(GtkApplication *app, gpointer user_data)
{
	GtkWidget *window = gtk_application_window_new(app);
	GtkWidget *terminal = vte_terminal_new();
	gtk_window_set_title(GTK_WINDOW(window), "FHI-Terminal");

	/* Start a new shell */
	const char *shell = g_getenv("SHELL");
	char *command[] = { (char *)(shell ? shell : "/bin/sh"), NULL };
	vte_terminal_spawn_async(VTE_TERMINAL(terminal),
	    VTE_PTY_DEFAULT,
	    NULL,         /* working directory  */
	    command,      /* command */
	    NULL,         /* environment */
	    G_SPAWN_DEFAULT, /* spawn flags */
	    NULL, NULL, NULL, /* child setup, data, destroy */
	    -1,           /* timeout */
	    NULL,         /* cancellable */
	    child_ready,  /* callback */
	    window);      /* user_data */

	g_signal_connect(terminal, "child-exited", G_CALLBACK(child_exited), window);

	gtk_window_set_child(GTK_WINDOW(window), terminal);
	gtk_window_present(GTK_WINDOW(window));
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
