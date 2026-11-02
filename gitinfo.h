/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#ifndef FHI_GITINFO_H
#define FHI_GITINFO_H

#include <glib.h>

typedef enum {
	FHI_GIT_STAGED,      /* what `git commit` would commit                    */
	FHI_GIT_TRACKED,     /* what `git commit -a` would commit                 */
	FHI_GIT_UNPUSHED     /* files changed by commits that `git push` would send */
} FhiGitKind;

/*
 * The entries of directory `cwd` (names, no slashes) that contain files of the given
 * kind. A change deep inside a folder names that top-level folder. Deleted files are
 * left out. Needs the `git` program; returns an empty array if `cwd` is not in a
 * repository. The result is cached until fhi_git_invalidate() (or a few seconds).
 * Free with g_ptr_array_unref().
 */
GPtrArray *fhi_git_files(const char *cwd, FhiGitKind kind);

/* Forget the cache: something probably changed (a command just finished) */
void fhi_git_invalidate(void);

#endif
