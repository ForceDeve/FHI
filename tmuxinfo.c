/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#include "tmuxinfo.h"

#include <fcntl.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

/* Separates the fields of the answer; a path could hold a space or a tab, hardly this */
#define SEP "|FHI|"

void
fhi_tmux_pane_clear(FhiTmuxPane *pane)
{
	g_free(pane->tty);
	g_free(pane->command);
	g_free(pane->path);
	memset(pane, 0, sizeof *pane);
}

static char *
read_proc(pid_t pid, const char *what, gsize *len)
{
	char *file = g_strdup_printf("/proc/%d/%s", (int)pid, what);
	char *data = NULL;
	if (!g_file_get_contents(file, &data, len, NULL)) data = NULL;
	g_free(file);
	return data;
}

gboolean
fhi_tmux_is_client(pid_t pid)
{
	if (pid <= 0) return FALSE;
	char *comm = read_proc(pid, "comm", NULL);
	/* "tmux: client" or just "tmux"; the server is "tmux: server" */
	gboolean yes = comm && g_str_has_prefix(comm, "tmux") && !g_str_has_prefix(comm, "tmux: server");
	g_free(comm);
	return yes;
}

/*
 * Which server does this client talk to? `tmux -L name` / `tmux -S path` are in its command
 * line; when tmux renames itself, the socket is in the title: "tmux: client (/tmp/tmux-1000/default)".
 * Returns the arguments to put after `tmux` (NULL-terminated, free with g_strfreev).
 */
static char **
server_args(pid_t client)
{
	GPtrArray *out = g_ptr_array_new();
	gsize len = 0;
	char *raw = read_proc(client, "cmdline", &len);
	if (raw) {
		const char *args[64];
		int n = 0;
		for (gsize off = 0; off < len && n < 64; off += strlen(raw + off) + 1) args[n++] = raw + off;

		gboolean found = FALSE;
		for (int i = 1; i < n && args[i][0] == '-'; i++) {
			if ((strcmp(args[i], "-L") == 0 || strcmp(args[i], "-S") == 0) && i + 1 < n) {
				g_ptr_array_add(out, g_strdup(args[i]));
				g_ptr_array_add(out, g_strdup(args[i + 1]));
				found = TRUE;
				break;
			}
			if (strcmp(args[i], "-f") == 0 || strcmp(args[i], "-c") == 0 || strcmp(args[i], "-T") == 0) i++;
		}
		if (!found && n >= 1) {
			const char *open = strstr(args[0], " (/");
			const char *close = open ? strrchr(open, ')') : NULL;
			if (open && close && close > open + 2) {
				g_ptr_array_add(out, g_strdup("-S"));
				g_ptr_array_add(out, g_strndup(open + 2, close - open - 2));
			}
		}
		g_free(raw);
	}
	g_ptr_array_add(out, NULL);
	return (char **)g_ptr_array_free(out, FALSE);
}

gboolean
fhi_tmux_parse(const char *output, pid_t client, FhiTmuxPane *out)
{
	gboolean found = FALSE;
	char **lines = g_strsplit(output ? output : "", "\n", -1);
	for (int i = 0; lines[i] && !found; i++) {
		/* client pid | pane pid | pane tty | pane command | pane path (last: may hold anything) */
		char **f = g_strsplit(lines[i], SEP, 5);
		if (g_strv_length(f) == 5 && atoi(f[0]) == (int)client && client > 0) {
			memset(out, 0, sizeof *out);
			out->client_pid = client;
			out->pane_pid = atoi(f[1]);
			out->tty = g_strdup(f[2]);
			out->command = g_strdup(f[3]);
			out->path = g_strdup(f[4]);
			found = out->pane_pid > 0 && out->tty[0] && out->path[0] == '/';
			if (!found) fhi_tmux_pane_clear(out);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	return found;
}

typedef struct {
	pid_t client;
	FhiTmuxFunc func;
	gpointer user_data;
} Query;

static gboolean query_busy;

static void
query_done(GObject *source, GAsyncResult *res, gpointer data)
{
	Query *q = data;
	char *out = NULL;
	GError *error = NULL;
	FhiTmuxPane pane = { 0 };
	gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, NULL, &error);
	if (error) g_error_free(error);
	query_busy = FALSE;
	if (ok && g_subprocess_get_successful(G_SUBPROCESS(source)) && fhi_tmux_parse(out, q->client, &pane))
		q->func(q->client, &pane, q->user_data);
	else
		q->func(q->client, NULL, q->user_data);
	fhi_tmux_pane_clear(&pane);
	g_free(out);
	g_object_unref(source);
	g_free(q);
}

void
fhi_tmux_query(pid_t client, FhiTmuxFunc func, gpointer user_data)
{
	if (query_busy) return;

	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup("tmux"));
	char **sock = server_args(client);
	for (int i = 0; sock[i]; i++) g_ptr_array_add(argv, g_strdup(sock[i]));
	g_strfreev(sock);
	g_ptr_array_add(argv, g_strdup("list-clients"));
	g_ptr_array_add(argv, g_strdup("-F"));
	g_ptr_array_add(argv, g_strdup("#{client_pid}" SEP "#{pane_pid}" SEP "#{pane_tty}" SEP
	                               "#{pane_current_command}" SEP "#{pane_current_path}"));
	g_ptr_array_add(argv, NULL);

	GError *error = NULL;
	GSubprocess *p = g_subprocess_newv((const gchar *const *)argv->pdata,
	                                   G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
	g_ptr_array_unref(argv);
	if (p == NULL) {   /* no tmux program */
		if (error) g_error_free(error);
		func(client, NULL, user_data);
		return;
	}
	Query *q = g_new(Query, 1);
	q->client = client;
	q->func = func;
	q->user_data = user_data;
	query_busy = TRUE;
	g_subprocess_communicate_utf8_async(p, NULL, NULL, query_done, q);
}

pid_t
fhi_tmux_tty_foreground(const char *tty)
{
	if (tty == NULL || !g_str_has_prefix(tty, "/dev/")) return 0;
	int fd = open(tty, O_RDONLY | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) return 0;
	pid_t pgid = tcgetpgrp(fd);
	close(fd);
	return pgid > 0 ? pgid : 0;
}

gboolean
fhi_tmux_pane_is_fhi(pid_t pane_pid)
{
	gsize len = 0;
	char *env = read_proc(pane_pid, "environ", &len);
	gboolean yes = FALSE;
	for (gsize off = 0; env && off < len && !yes; off += strlen(env + off) + 1)
		yes = g_str_has_prefix(env + off, "FHI_ZDOTDIR=");
	g_free(env);
	return yes;
}
