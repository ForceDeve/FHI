/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#ifndef FHI_PREVIEW_H
#define FHI_PREVIEW_H

#include <glib.h>

/*
 * What is going to happen to a file if the user presses Enter.
 * The numbers matter: when one file is mentioned several times, the HIGHER
 * role wins (so `rm a; touch a` shows `a` as FHI_ROLE_REMOVE).
 */
typedef enum {
	FHI_ROLE_NONE = 0,
	FHI_ROLE_TOUCH,   /* just mentioned / read / modified   (yellow) */
	FHI_ROLE_CREATE,  /* will be created or written         (green)  */
	FHI_ROLE_DEST,    /* destination of a mv / cp / ln      (blue)   */
	FHI_ROLE_REMOVE   /* will be deleted                    (red)    */
} FhiRole;

typedef struct {
	char *name;       /* a name inside the current directory, no slashes */
	FhiRole role;
	gboolean exists;  /* FALSE: the file is not there yet ("ghost")      */
} FhiHit;

/*
 * Reads the command line `buffer` (exactly what the user has typed so far)
 * and returns the files of the directory `cwd` that it affects, sorted by name.
 *
 *   cwd       absolute path of the directory the shell is in
 *   existing  NULL-terminated array with the names of the files in `cwd`
 *
 * The result is a GPtrArray of FhiHit*; free it with g_ptr_array_unref().
 * Nothing is ever executed or expanded by a real shell: this only READS text.
 */
GPtrArray *fhi_preview_parse(const char *buffer, const char *cwd,
                             const char *const *existing);

/*
 * A file-list filter typed like shell words:
 *   *.png   report?   [a-c]*   file{1,2}.txt   "two words"   !*.o
 * Several words match if ANY of them does; a word starting with ! excludes.
 * A word without wildcards matches anywhere in the name ("png" finds a.png).
 * Matching ignores upper/lower case. An empty spec matches everything.
 */
typedef struct FhiFilter FhiFilter;
FhiFilter *fhi_filter_new(const char *spec);
gboolean   fhi_filter_matches(const FhiFilter *filter, const char *name);
void       fhi_filter_free(FhiFilter *filter);

#endif
