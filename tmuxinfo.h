/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#ifndef FHI_TMUXINFO_H
#define FHI_TMUXINFO_H

#include <glib.h>
#include <sys/types.h>

/*
 * tmux support. When `tmux` is the program in front of the terminal, the file list should
 * follow the PANE that has focus: its folder, and the program running in it (claude, ssh,
 * vim...). tmux hides all of that from the terminal, so it is asked from the tmux server:
 * `tmux list-clients` knows, for every client, which pane is active.
 */

typedef struct {
	pid_t client_pid;   /* the tmux client (the program in front of FHI's terminal)  */
	pid_t pane_pid;     /* the first process of the active pane (normally the shell) */
	char *tty;          /* the pane's terminal, /dev/pts/N                           */
	char *command;      /* what runs in the pane right now, as tmux says it          */
	char *path;         /* the pane's current folder                                 */
} FhiTmuxPane;

void fhi_tmux_pane_clear(FhiTmuxPane *pane);

/* Is process `pid` a tmux client (not the server)? */
gboolean fhi_tmux_is_client(pid_t pid);

/*
 * Ask the server which pane the client `client` shows. `func` is called on the main loop
 * with the pane, or NULL if the question failed (no tmux program, no such client...). The
 * pane is only valid during the call. A query while the last one is still running is
 * ignored (func is not called for it).
 */
typedef void (*FhiTmuxFunc)(pid_t client, const FhiTmuxPane *pane, gpointer user_data);
void fhi_tmux_query(pid_t client, FhiTmuxFunc func, gpointer user_data);

/* The output of `list-clients -F` (see tmuxinfo.c) -> the line for `client`. FALSE if absent. */
gboolean fhi_tmux_parse(const char *output, pid_t client, FhiTmuxPane *out);

/* The process in front of terminal `tty` (like tcgetpgrp), 0 if it cannot be found out */
pid_t fhi_tmux_tty_foreground(const char *tty);

/* Was the pane's shell started by FHI (so it has FHI's zsh hooks)? */
gboolean fhi_tmux_pane_is_fhi(pid_t pane_pid);

#endif
