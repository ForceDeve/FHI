/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#include "llmcli.h"

#include <gio/gio.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ======================================================================== */
/* Recognising the tools                                                      */
/* ======================================================================== */

/*
 * `names`: the file name the program is started as (`claude`), plus the names of its
 * npm / pip package when it is started through node or python (`claude-code`).
 * `mention`: the tool has @file mentions. Tools not marked get the plain path.
 */
static const FhiLlmTool tools[] = {
	/* the well known ones */
	{ "Claude Code",    "claude,claude-code",        TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Codex",          "codex,codex-cli",           TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Gemini CLI",     "gemini,gemini-cli",         TRUE,  FHI_LLM_SPACES_BACKSLASH },
	{ "GitHub Copilot", "copilot,copilot-cli",       TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Cursor Agent",   "cursor-agent",              TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Amp",            "amp",                       TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Droid",          "droid",                     TRUE,  FHI_LLM_SPACES_QUOTE     },
	/* open source */
	{ "Qwen Code",      "qwen,qwen-code",            TRUE,  FHI_LLM_SPACES_BACKSLASH },
	{ "opencode",       "opencode,opencode-ai",      TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Kimi CLI",       "kimi,kimi-cli",             TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Kilo Code",      "kilo,kilocode",             TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Mistral Vibe",   "vibe",                      TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Continue",       "cn",                        TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Auggie",         "auggie",                    TRUE,  FHI_LLM_SPACES_QUOTE     },
	{ "Crush",          "crush",                     FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "aider",          "aider,aider-chat",          FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "goose",          "goose",                     FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "Cline",          "cline",                     FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "OpenHands",      "openhands",                 FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "Plandex",        "plandex,pdx",               FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "Open Interpreter", "interpreter",             FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "gptme",          "gptme",                     FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "RA.Aid",         "ra-aid",                    FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "Forge",          "forge",                     FALSE, FHI_LLM_SPACES_QUOTE     },
	/* plain chat clients: no file access, but pasting a path is still handy */
	{ "Ollama",         "ollama",                    FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "llm",            "llm",                       FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "aichat",         "aichat",                    FALSE, FHI_LLM_SPACES_QUOTE     },
	{ "ShellGPT",       "sgpt",                      FALSE, FHI_LLM_SPACES_QUOTE     },
};

static gboolean
name_in_list(const char *list, const char *name)
{
	gsize len = strlen(name);
	if (len == 0) return FALSE;
	for (const char *p = list; p && *p; ) {
		const char *end = strchr(p, ',');
		gsize n = end ? (gsize)(end - p) : strlen(p);
		if (n == len && strncmp(p, name, n) == 0) return TRUE;
		p = end ? end + 1 : NULL;
	}
	return FALSE;
}

static const FhiLlmTool *
tool_by_name(const char *name)
{
	for (guint i = 0; i < G_N_ELEMENTS(tools); i++)
		if (name_in_list(tools[i].names, name)) return &tools[i];
	return NULL;
}

/* "cli.js" -> "cli", "/a/b/aider" -> "aider" (new string) */
static char *
base_name(const char *path)
{
	char *base = g_path_get_basename(path);
	char *dot = strrchr(base, '.');
	if (dot && dot != base && (strcmp(dot, ".js") == 0 || strcmp(dot, ".mjs") == 0 ||
	                           strcmp(dot, ".cjs") == 0 || strcmp(dot, ".py") == 0))
		*dot = '\0';
	return base;
}

static gboolean
is_interpreter(const char *base)
{
	return strcmp(base, "node") == 0 || strcmp(base, "nodejs") == 0 || strcmp(base, "bun") == 0 ||
	       strcmp(base, "deno") == 0 || strcmp(base, "uv") == 0 || g_str_has_prefix(base, "python");
}

const FhiLlmTool *
fhi_llm_detect(pid_t pid)
{
	if (pid <= 0) return NULL;

	char *file = g_strdup_printf("/proc/%d/cmdline", (int)pid);
	char *raw = NULL;
	gsize len = 0;
	gboolean ok = g_file_get_contents(file, &raw, &len, NULL);
	g_free(file);
	if (!ok || len == 0) {
		g_free(raw);
		return NULL;
	}

	/* the arguments are separated by NUL bytes: look at the first few */
	const char *args[6] = { 0 };
	int n = 0;
	for (gsize off = 0; off < len && n < 6; ) {
		args[n++] = raw + off;
		off += strlen(raw + off) + 1;
	}

	const FhiLlmTool *found = NULL;
	char *prog = base_name(args[0][0] == '-' ? args[0] + 1 : args[0]);   /* login shells start with - */
	found = tool_by_name(prog);

	/* started under another name (a symlink, a renamed binary): ask the executable itself */
	if (!found) {
		char *link = g_strdup_printf("/proc/%d/exe", (int)pid);
		char *exe = g_file_read_link(link, NULL);
		g_free(link);
		if (exe) {
			char *eb = base_name(exe);
			found = tool_by_name(eb);
			g_free(eb);
			g_free(exe);
		}
	}

	/* `node …/@anthropic-ai/claude-code/cli.js`, `python …/bin/aider` */
	if (!found && is_interpreter(prog)) {
		for (int i = 1; i < n && !found; i++) {
			if (args[i][0] == '-') continue;          /* an option of the interpreter */
			char *script = base_name(args[i]);
			found = tool_by_name(script);
			g_free(script);
			if (!found && strstr(args[i], "/node_modules/")) {
				char **parts = g_strsplit(args[i], "/", -1);
				for (int k = 0; parts[k] && !found; k++)
					if (parts[k][0]) found = tool_by_name(parts[k]);
				g_strfreev(parts);
			}
			break;   /* only the script argument counts */
		}
	}

	g_free(prog);
	g_free(raw);
	return found;
}

char *
fhi_llm_cwd(pid_t pid)
{
	char *link = g_strdup_printf("/proc/%d/cwd", (int)pid);
	char *cwd = g_file_read_link(link, NULL);
	g_free(link);
	return cwd;
}

/* ======================================================================== */
/* Writing a path                                                             */
/* ======================================================================== */

char *
fhi_llm_format_path(const FhiLlmTool *tool, const char *path, gboolean is_dir, const char *cwd)
{
	for (const char *c = path; *c; c++)
		if ((unsigned char)*c < 0x20 || *c == 0x7f) return NULL;

	/* inside the project: relative, like the assistant itself writes it */
	const char *shown = path;
	if (cwd && cwd[0]) {
		gsize cl = strlen(cwd);
		while (cl > 1 && cwd[cl - 1] == '/') cl--;
		if (strncmp(path, cwd, cl) == 0 && path[cl] == '/' && path[cl + 1] != '\0') shown = path + cl + 1;
	}

	GString *out = g_string_new(NULL);
	if (tool->mention) g_string_append_c(out, '@');

	gboolean spaced = strpbrk(shown, " \t") != NULL;
	gboolean quote = spaced && (tool->spaces == FHI_LLM_SPACES_QUOTE || !tool->mention) && !strchr(shown, '"');
	if (quote) g_string_append_c(out, '"');
	for (const char *c = shown; *c; c++) {
		if (!quote && spaced && (*c == ' ' || *c == '\t')) g_string_append_c(out, '\\');
		g_string_append_c(out, *c);
	}
	if (is_dir && !g_str_has_suffix(shown, "/")) g_string_append_c(out, '/');
	if (quote) g_string_append_c(out, '"');
	g_string_append_c(out, ' ');   /* ready for the next word */
	return g_string_free(out, FALSE);
}

/* ======================================================================== */
/* Watching a project                                                         */
/* ======================================================================== */

#define SCAN_MAX_ENTRIES 8000
#define MAX_CHANGES      300

typedef struct {
	gint64 mtime;      /* ns */
	gint64 size;
	gboolean dir;
} Entry;

struct FhiLlmWatch {
	gint refs;                 /* the owner + every running scan */
	gboolean dead;             /* the owner let go: do not call back */
	gboolean busy;             /* a scan is running */
	char *root;
	GHashTable *prev;          /* last snapshot (relative path -> Entry), NULL before the first scan */
	FhiLlmWatchFunc func;
	gpointer user_data;
};

typedef struct {
	FhiLlmWatch *w;
	GHashTable *prev;          /* our reference to the snapshot to compare with, may be NULL */
	GHashTable *now;
	GPtrArray *changes;
} ScanJob;

/* Not worth walking: caches, dependencies and build output are noise to the user */
static gboolean
skip_dir(const char *name)
{
	static const char *const skip[] = {
		".git", "node_modules", "target", "__pycache__", ".venv", "venv", ".cache", "dist", "build",
		".idea", ".next", ".gradle", ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox", ".svn", ".hg"
	};
	for (guint i = 0; i < G_N_ELEMENTS(skip); i++)
		if (strcmp(name, skip[i]) == 0) return TRUE;
	return g_str_has_prefix(name, "cmake-build-");
}

static int
cmp_str(gconstpointer a, gconstpointer b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Snapshot of `root`, breadth first so a cut-off tree still has its top levels */
static GHashTable *
scan_tree(const char *root)
{
	GHashTable *snap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GQueue todo = G_QUEUE_INIT;
	g_queue_push_tail(&todo, g_strdup(""));

	while (!g_queue_is_empty(&todo) && g_hash_table_size(snap) < SCAN_MAX_ENTRIES) {
		char *rel = g_queue_pop_head(&todo);
		char *abs = rel[0] ? g_build_filename(root, rel, NULL) : g_strdup(root);
		DIR *d = opendir(abs);
		if (d) {
			GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
			struct dirent *e;
			while ((e = readdir(d)) != NULL) {
				if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
				g_ptr_array_add(names, g_strdup(e->d_name));
			}
			closedir(d);
			g_ptr_array_sort(names, cmp_str);   /* the same order every time, so a cut-off stays put */

			for (guint i = 0; i < names->len && g_hash_table_size(snap) < SCAN_MAX_ENTRIES; i++) {
				const char *name = names->pdata[i];
				char *child = rel[0] ? g_strconcat(rel, "/", name, NULL) : g_strdup(name);
				char *full = g_build_filename(abs, name, NULL);
				struct stat sb;
				if (lstat(full, &sb) != 0) {
					g_free(child);
					g_free(full);
					continue;
				}
				Entry *en = g_new(Entry, 1);
				en->dir = S_ISDIR(sb.st_mode);
				en->mtime = (gint64)sb.st_mtim.tv_sec * 1000000000 + sb.st_mtim.tv_nsec;
				en->size = sb.st_size;
				g_hash_table_replace(snap, child, en);
				if (en->dir && !skip_dir(name)) g_queue_push_tail(&todo, g_strdup(child));
				g_free(full);
			}
			g_ptr_array_unref(names);
		}
		g_free(abs);
		g_free(rel);
	}
	g_queue_clear_full(&todo, g_free);
	return snap;
}

static void
change_free(gpointer p)
{
	FhiLlmChange *c = p;
	g_free(c->path);
	g_free(c);
}

static void
scan_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	ScanJob *job = data;
	job->now = scan_tree(job->w->root);
	job->changes = g_ptr_array_new_with_free_func(change_free);
	if (job->prev) {
		GHashTableIter it;
		gpointer k, v;
		g_hash_table_iter_init(&it, job->now);
		while (g_hash_table_iter_next(&it, &k, &v) && job->changes->len < MAX_CHANGES) {
			const Entry *now = v;
			const Entry *old = g_hash_table_lookup(job->prev, k);
			gboolean created = old == NULL;
			/* a folder's own mtime changes whenever something in it does: only its birth counts */
			gboolean edited = old && !now->dir && (old->mtime != now->mtime || old->size != now->size);
			if (!created && !edited) continue;
			FhiLlmChange *c = g_new0(FhiLlmChange, 1);
			c->path = g_strdup(k);
			c->created = created;
			g_ptr_array_add(job->changes, c);
		}
		/* things that disappeared */
		g_hash_table_iter_init(&it, job->prev);
		while (g_hash_table_iter_next(&it, &k, &v) && job->changes->len < MAX_CHANGES) {
			if (g_hash_table_contains(job->now, k)) continue;
			FhiLlmChange *c = g_new0(FhiLlmChange, 1);
			c->path = g_strdup(k);
			c->removed = TRUE;
			g_ptr_array_add(job->changes, c);
		}
	}
	g_task_return_boolean(task, TRUE);
}

static void
watch_unref(FhiLlmWatch *w)
{
	if (!g_atomic_int_dec_and_test(&w->refs)) return;
	if (w->prev) g_hash_table_unref(w->prev);
	g_free(w->root);
	g_free(w);
}

static void
job_free(gpointer data)
{
	ScanJob *job = data;
	if (job->prev) g_hash_table_unref(job->prev);
	if (job->now) g_hash_table_unref(job->now);
	if (job->changes) g_ptr_array_unref(job->changes);
	watch_unref(job->w);
	g_free(job);
}

static void
scan_done(GObject *source, GAsyncResult *res, gpointer unused)
{
	ScanJob *job = g_task_get_task_data(G_TASK(res));
	FhiLlmWatch *w = job->w;
	w->busy = FALSE;
	if (!w->dead) {
		GHashTable *old = w->prev;
		w->prev = job->now;       /* the watch takes the snapshot over */
		job->now = NULL;
		if (old) g_hash_table_unref(old);
		if (job->changes->len > 0 && w->func) {
			GPtrArray *changes = job->changes;
			job->changes = NULL;
			w->func(w->root, changes, w->user_data);
		}
	}
}

FhiLlmWatch *
fhi_llm_watch_new(const char *root, FhiLlmWatchFunc func, gpointer user_data)
{
	FhiLlmWatch *w = g_new0(FhiLlmWatch, 1);
	w->refs = 1;
	w->root = g_strdup(root);
	w->func = func;
	w->user_data = user_data;
	return w;
}

void
fhi_llm_watch_poll(FhiLlmWatch *w)
{
	if (w->busy || w->dead) return;
	w->busy = TRUE;
	ScanJob *job = g_new0(ScanJob, 1);
	job->w = w;
	g_atomic_int_inc(&w->refs);
	job->prev = w->prev ? g_hash_table_ref(w->prev) : NULL;
	GTask *task = g_task_new(NULL, NULL, scan_done, NULL);
	g_task_set_task_data(task, job, job_free);
	g_task_run_in_thread(task, scan_thread);
	g_object_unref(task);   /* the running task keeps its own reference */
}

void
fhi_llm_watch_free(FhiLlmWatch *w)
{
	if (w == NULL) return;
	w->dead = TRUE;
	watch_unref(w);
}
