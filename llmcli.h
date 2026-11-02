/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#ifndef FHI_LLMCLI_H
#define FHI_LLMCLI_H

#include <glib.h>
#include <sys/types.h>

/*
 * Support for AI coding assistants that run inside the terminal (Claude Code, Codex,
 * Gemini CLI, opencode, aider...). Two things:
 *
 *   1. Recognising them: fhi_llm_detect() looks at the program in front of the terminal.
 *   2. Writing a file's path the way that tool wants it (`@src/main.c` where the tool has
 *      @-mentions, the plain path elsewhere), see fhi_llm_format_path().
 *   3. Watching the project for files the tool creates or edits (FhiLlmWatch), so the
 *      file list can light them up while the assistant works.
 */

typedef enum {
	FHI_LLM_SPACES_QUOTE,       /* @"my file.c"  */
	FHI_LLM_SPACES_BACKSLASH    /* @my\ file.c   */
} FhiLlmSpaces;

typedef struct {
	const char *title;    /* shown to the user: "Claude Code"                              */
	const char *names;    /* comma separated program / package names that identify it      */
	gboolean mention;     /* has @file mentions: the path is pasted as `@path`             */
	FhiLlmSpaces spaces;  /* how a path with spaces is written                             */
} FhiLlmTool;

/* Is process `pid` (the program in front of the terminal) a known assistant? NULL if not. */
const FhiLlmTool *fhi_llm_detect(pid_t pid);

/* Working directory of process `pid` (new string, free it), NULL if unknown */
char *fhi_llm_cwd(pid_t pid);

/*
 * The text to paste for `path`: relative to `cwd` when it is inside it (that is what the
 * assistant sees), `@` in front for tools with mentions, a space after. NULL if the path
 * holds control characters (it would be typed as keys). Free the result.
 */
char *fhi_llm_format_path(const FhiLlmTool *tool, const char *path, gboolean is_dir, const char *cwd);

/* --- watching a project for files an assistant creates or edits --- */

typedef struct FhiLlmWatch FhiLlmWatch;

typedef struct {
	char *path;           /* relative to the watched root */
	gboolean created;     /* TRUE: new file / folder */
	gboolean removed;     /* TRUE: it is gone. Neither flag: an existing file was edited */
} FhiLlmChange;

/* Called on the main loop with FhiLlmChange* items (free with g_ptr_array_unref) */
typedef void (*FhiLlmWatchFunc)(const char *root, GPtrArray *changes, gpointer user_data);

/*
 * The first poll takes a snapshot of `root` (the baseline); every later poll reports what
 * differs from the previous one. The folder is scanned in a background thread, so poll
 * as often as you like: a poll while the last scan is still running does nothing.
 * Folders like .git, node_modules and build output are skipped, and huge trees are cut off.
 */
FhiLlmWatch *fhi_llm_watch_new(const char *root, FhiLlmWatchFunc func, gpointer user_data);
void         fhi_llm_watch_poll(FhiLlmWatch *w);
void         fhi_llm_watch_free(FhiLlmWatch *w);   /* no callback comes after this */

#endif
