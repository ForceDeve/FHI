/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#ifndef FHI_SSHINFO_H
#define FHI_SSHINFO_H

#include <gtk/gtk.h>
#include <sys/types.h>

/*
 * Information about the `ssh` session that is in front of the terminal.
 *
 * Local facts come for free: the command line of the ssh process, how long it has
 * been running, and what `ssh -G` says the config resolves to (real host name, user,
 * port, jump host...).
 *
 * The remote account (uid, groups, home permissions, OS...) can only be asked of the
 * server, so ONE extra, silent `ssh` is started in the background with the same
 * options plus BatchMode (it never asks for a password: it works with keys, the agent
 * or a ControlMaster connection, and otherwise the card says "unavailable"). Set the
 * environment variable FHI_SSH_NO_QUERY=1 to never do that.
 */
typedef struct FhiSsh FhiSsh;

/* Start watching the ssh process `pid`. NULL if its command line cannot be understood. */
FhiSsh *fhi_ssh_new(pid_t pid);
void    fhi_ssh_free(FhiSsh *s);
pid_t   fhi_ssh_pid(const FhiSsh *s);

/* The card that shows the information; put it in an overlay over the dimmed file list */
GtkWidget *fhi_ssh_card_new(void);
/* Refresh the card (call about twice a second). `s` == NULL hides it. */
void fhi_ssh_card_update(GtkWidget *card, const FhiSsh *s);

#endif
