/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#include "gitinfo.h"
#include <string.h>

/* Run `git -C cwd <args...>` and return its output (one name per line), or NULL on failure */
static char *
run_git(const char *cwd, gsize *len, const char *const *args)
{
	GPtrArray *argv = g_ptr_array_new();
	g_ptr_array_add(argv, (gpointer)"git");
	g_ptr_array_add(argv, (gpointer)"--no-optional-locks");   /* do not disturb a running git */
	g_ptr_array_add(argv, (gpointer)"-C");
	g_ptr_array_add(argv, (gpointer)cwd);
	g_ptr_array_add(argv, (gpointer)"-c");
	g_ptr_array_add(argv, (gpointer)"core.quotepath=off");
	for (; *args; args++) g_ptr_array_add(argv, (gpointer)*args);
	g_ptr_array_add(argv, NULL);

	char *out = NULL;
	int status = 0;
	gboolean ok = g_spawn_sync(NULL, (char **)argv->pdata, NULL,
	                           G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_STDIN_FROM_DEV_NULL,
	                           NULL, NULL, &out, NULL, &status, NULL);
	g_ptr_array_unref(argv);
	if (ok && g_spawn_check_wait_status(status, NULL)) {
		*len = out ? strlen(out) : 0;
		return out;
	}
	g_free(out);
	return NULL;
}

/* Cut `text` into lines and keep the first path component of each (one entry per name) */
static void
add_names(GHashTable *set, GPtrArray *out, const char *text, const char *cwd)
{
	char **lines = g_strsplit(text, "\n", -1);
	for (char **l = lines; *l; l++) {
		if (**l == '\0') continue;
		char *first = g_strdup(*l);
		char *slash = strchr(first, '/');
		if (slash) *slash = '\0';
		char *full = g_build_filename(cwd, first, NULL);
		/* only things that still exist: a deleted file has no row to light up */
		if (!g_hash_table_contains(set, first) && g_file_test(full, G_FILE_TEST_EXISTS)) {
			g_hash_table_add(set, first);
			g_ptr_array_add(out, g_strdup(first));
		} else {
			g_free(first);
		}
		g_free(full);
	}
	g_strfreev(lines);
}

static GPtrArray *
query(const char *cwd, FhiGitKind kind)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	gsize len;
	char *text = NULL;

	switch (kind) {
	case FHI_GIT_STAGED: {
		const char *const a[] = { "diff", "--cached", "--name-only", "--relative", NULL };
		text = run_git(cwd, &len, a);
		break;
	}
	case FHI_GIT_TRACKED: {
		/* staged + unstaged changes of tracked files (before the first commit there is no HEAD) */
		const char *const a[] = { "diff", "HEAD", "--name-only", "--relative", NULL };
		text = run_git(cwd, &len, a);
		if (!text) {
			const char *const b[] = { "diff", "--cached", "--name-only", "--relative", NULL };
			text = run_git(cwd, &len, b);
		}
		break;
	}
	case FHI_GIT_UNPUSHED: {
		const char *const a[] = { "diff", "@{upstream}..HEAD", "--name-only", "--relative", NULL };
		text = run_git(cwd, &len, a);
		if (!text) {   /* no upstream yet: everything no remote branch has */
			const char *const b[] = { "log", "HEAD", "--not", "--remotes", "--name-only",
			                          "--pretty=format:", "--relative", NULL };
			text = run_git(cwd, &len, b);
		}
		break;
	}
	}
	if (text) add_names(set, out, text, cwd);
	g_free(text);
	g_hash_table_unref(set);
	return out;
}

static struct {
	char *cwd;
	FhiGitKind kind;
	gint64 when;
	GPtrArray *files;
} cache;

void
fhi_git_invalidate(void)
{
	g_clear_pointer(&cache.files, g_ptr_array_unref);
	g_clear_pointer(&cache.cwd, g_free);
}

GPtrArray *
fhi_git_files(const char *cwd, FhiGitKind kind)
{
	gint64 now = g_get_monotonic_time();
	if (!cache.files || cache.kind != kind || g_strcmp0(cache.cwd, cwd) != 0 ||
	    now - cache.when > 5 * G_USEC_PER_SEC) {
		fhi_git_invalidate();
		cache.cwd = g_strdup(cwd);
		cache.kind = kind;
		cache.files = query(cwd, kind);
		cache.when = g_get_monotonic_time();   /* after the query: it may be slow */
	}
	return g_ptr_array_ref(cache.files);
}
