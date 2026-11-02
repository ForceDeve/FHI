/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

/*
 * preview.c - work out which files a half-typed shell command will affect.
 *
 * The work happens in four steps:
 *   1. lex()          cut the text into words and operators (handles quotes)
 *   2. brace_expand() turn  file{1,2}.txt  into  file1.txt file2.txt
 *   3. resolve_one()  match a word (with * ? [a-f]) against the files that
 *                     really exist in the directory
 *   4. analyze()      decide, from the command name, whether each word is
 *                     being removed, created, used as a destination...
 *
 * This file uses only GLib, so it can be tested without opening a window.
 *
 * "pattern" strings: while lexing, every character that was QUOTED or
 * backslash-escaped and that would otherwise be special (* ? [ ] { } , \ ~)
 * gets a backslash put in front of it. So  "a*b"  becomes the pattern  a\*b
 * (a plain star, not a wildcard) while an unquoted  a*b  stays  a*b.
 * fnmatch() understands that same backslash convention.
 */

#include "preview.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* 1. Lexer: text -> tokens                                                  */
/* ======================================================================== */

typedef enum {
	TOK_WORD,        /* an ordinary word (a command name or an argument)  */
	TOK_SEP,         /* ; && || | &  newline ( )  - a new command starts   */
	TOK_REDIR_OUT,   /* > >> >| &>   the NEXT word is a file being written */
	TOK_REDIR_IN,    /* <            the NEXT word is a file being read    */
	TOK_REDIR_SKIP   /* << <<<       the NEXT word is not a file           */
} TokType;

typedef struct {
	TokType type;
	char *pattern;     /* see the "pattern strings" note at the top        */
	gboolean unknown;  /* contains $var, $(...) or `...`: we can't know it */
} Token;

static void
token_free(gpointer p)
{
	Token *t = p;
	g_free(t->pattern);
	g_free(t);
}

typedef struct {
	GPtrArray *tokens;
	GString *word;     /* the word being built */
	gboolean in_word;  /* TRUE once at least one character (or quote) was seen */
	gboolean unknown;
} Lexer;

static void
push_token(Lexer *lx, TokType type, const char *pattern, gboolean unknown)
{
	Token *t = g_new0(Token, 1);
	t->type = type;
	t->pattern = g_strdup(pattern ? pattern : "");
	t->unknown = unknown;
	g_ptr_array_add(lx->tokens, t);
}

/* Finish the word being built (if any) and emit it as a token. */
static void
end_word(Lexer *lx)
{
	if (lx->in_word) push_token(lx, TOK_WORD, lx->word->str, lx->unknown);
	g_string_truncate(lx->word, 0);
	lx->in_word = FALSE;
	lx->unknown = FALSE;
}

/* Append one character to the current word. Quoted special characters get a
 * backslash in front so that they are not mistaken for wildcards later. */
static void
add_char(Lexer *lx, char c, gboolean quoted)
{
	if (quoted && strchr("*?[]{}\\,~", c)) g_string_append_c(lx->word, '\\');
	g_string_append_c(lx->word, c);
	lx->in_word = TRUE;
}

/* Skip the "&1" in  2>&1  (a file-descriptor copy, not a file name). */
static const char *
skip_fd_dup(const char *p)
{
	p++; /* the '&' */
	while (*p && (isdigit((unsigned char)*p) || *p == '-')) p++;
	return p;
}

static GPtrArray *
lex(const char *s)
{
	Lexer lx = { g_ptr_array_new_with_free_func(token_free), g_string_new(NULL), FALSE, FALSE };
	const char *p = s;

	while (*p) {
		char c = *p++;   /* from now on *p is the NEXT character */

		switch (c) {
		case ' ':
		case '\t':
			end_word(&lx);
			break;

		case '\n': case ';': case '(': case ')':
			end_word(&lx);
			push_token(&lx, TOK_SEP, NULL, FALSE);
			break;

		case '|':
			end_word(&lx);
			if (*p == '|' || *p == '&') p++;
			push_token(&lx, TOK_SEP, NULL, FALSE);
			break;

		case '&':
			end_word(&lx);
			if (*p == '&') {                       /* && */
				p++;
				push_token(&lx, TOK_SEP, NULL, FALSE);
			} else if (*p == '>') {                /* &> and &>> */
				p++;
				if (*p == '>') p++;
				push_token(&lx, TOK_REDIR_OUT, NULL, FALSE);
			} else {                               /* background job */
				push_token(&lx, TOK_SEP, NULL, FALSE);
			}
			break;

		case '>':
		case '<': {
			/* "2>file": the 2 is a file descriptor number, not an argument */
			if (lx.in_word && strspn(lx.word->str, "0123456789") == lx.word->len) {
				g_string_truncate(lx.word, 0);
				lx.in_word = FALSE;
			}
			end_word(&lx);

			if (c == '>') {
				if (*p == '>' || *p == '|') p++;
				if (*p == '&') { p = skip_fd_dup(p); break; }   /* 2>&1 */
				push_token(&lx, TOK_REDIR_OUT, NULL, FALSE);
			} else if (*p == '<') {                              /* heredoc */
				while (*p == '<') p++;
				push_token(&lx, TOK_REDIR_SKIP, NULL, FALSE);
			} else if (*p == '&') {
				p = skip_fd_dup(p);
			} else {
				push_token(&lx, TOK_REDIR_IN, NULL, FALSE);
			}
			break;
		}

		case '\'':   /* single quotes: everything up to the next ' is literal */
			lx.in_word = TRUE;
			while (*p && *p != '\'') add_char(&lx, *p++, TRUE);
			if (*p) p++;
			break;

		case '"':    /* double quotes: literal, except \" \\ \$ \` and $ expansions */
			lx.in_word = TRUE;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1] && strchr("\"\\$`", p[1])) {
					add_char(&lx, p[1], TRUE);
					p += 2;
				} else {
					if (*p == '$' || *p == '`') lx.unknown = TRUE;
					add_char(&lx, *p++, TRUE);
				}
			}
			if (*p) p++;
			break;

		case '\\':   /* backslash: the next character is literal */
			lx.in_word = TRUE;
			if (*p == '\n') p++;                 /* line continuation */
			else if (*p) add_char(&lx, *p++, TRUE);
			break;

		case '$':    /* $var  $(cmd)  ${var}: we cannot know the value */
			lx.in_word = TRUE;
			lx.unknown = TRUE;
			if (*p == '(') {
				int depth = 0;
				while (*p) {
					if (*p == '(') depth++;
					else if (*p == ')' && --depth == 0) { p++; break; }
					p++;
				}
			} else if (*p == '{') {
				while (*p && *p != '}') p++;
				if (*p) p++;
			}
			break;

		case '`':
			lx.in_word = TRUE;
			lx.unknown = TRUE;
			while (*p && *p != '`') p++;
			if (*p) p++;
			break;

		default:
			add_char(&lx, c, FALSE);
			break;
		}
	}
	end_word(&lx);

	g_string_free(lx.word, TRUE);
	return lx.tokens;
}

/* ======================================================================== */
/* 2. Brace expansion:  a{1,2}b -> a1b a2b    f{1..3} -> f1 f2 f3            */
/* ======================================================================== */

static gboolean
is_integer(const char *s)
{
	if (*s == '-') s++;
	if (!*s) return FALSE;
	for (; *s; s++) if (!isdigit((unsigned char)*s)) return FALSE;
	return TRUE;
}

/* Handles the inside of {x..y}. Returns the generated items, or NULL if the
 * text is not a range. */
static GPtrArray *
expand_range(const char *body)
{
	const char *dots = strstr(body, "..");
	if (!dots) return NULL;
	char *a = g_strndup(body, dots - body);
	char *b = g_strdup(dots + 2);
	GPtrArray *items = NULL;

	if (is_integer(a) && is_integer(b)) {
		long from = strtol(a, NULL, 10), to = strtol(b, NULL, 10);
		/* {01..10} keeps the zero padding, like zsh does */
		int width = 0;
		if ((a[0] == '0' && a[1]) || (b[0] == '0' && b[1])) width = (int)MAX(strlen(a), strlen(b));
		if (labs(to - from) < 200) {
			items = g_ptr_array_new_with_free_func(g_free);
			long step = from <= to ? 1 : -1;
			for (long n = from; ; n += step) {
				g_ptr_array_add(items, g_strdup_printf("%0*ld", width, n));
				if (n == to) break;
			}
		}
	} else if (strlen(a) == 1 && strlen(b) == 1 && isalpha((unsigned char)*a) && isalpha((unsigned char)*b)) {
		items = g_ptr_array_new_with_free_func(g_free);
		int step = *a <= *b ? 1 : -1;
		for (int ch = *a; ; ch += step) {
			g_ptr_array_add(items, g_strdup_printf("%c", ch));
			if (ch == *b) break;
		}
	}
	g_free(a);
	g_free(b);
	return items;
}

/* Splits "a,b,{c,d}" at the commas that are not inside nested braces. */
static GPtrArray *
split_commas(const char *body)
{
	GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
	int depth = 0;
	const char *start = body;
	for (const char *q = body; ; q++) {
		if (*q == '\\' && q[1]) { q++; continue; }
		if (*q == '{') depth++;
		else if (*q == '}') depth--;
		if ((*q == ',' && depth == 0) || *q == '\0') {
			g_ptr_array_add(parts, g_strndup(start, q - start));
			start = q + 1;
		}
		if (*q == '\0') break;
	}
	return parts;
}

static void
brace_expand(const char *s, GPtrArray *out, int depth)
{
	/* safety limits: a typo like {1..99999} must not freeze the window */
	if (depth > 6 || out->len > 300) {
		g_ptr_array_add(out, g_strdup(s));
		return;
	}

	for (const char *p = s; *p; p++) {
		if (*p == '\\' && p[1]) { p++; continue; }
		if (*p != '{') continue;

		/* find the matching '}' */
		const char *close = NULL;
		int nest = 0;
		for (const char *q = p; *q; q++) {
			if (*q == '\\' && q[1]) { q++; continue; }
			if (*q == '{') nest++;
			else if (*q == '}' && --nest == 0) { close = q; break; }
		}
		if (!close) continue;   /* still typing: "{" without "}" yet */

		char *prefix = g_strndup(s, p - s);
		char *body = g_strndup(p + 1, close - p - 1);
		const char *suffix = close + 1;

		GPtrArray *alts = split_commas(body);
		if (alts->len < 2) {              /* no comma: maybe a range */
			g_ptr_array_unref(alts);
			alts = expand_range(body);
		}

		if (alts) {
			for (guint i = 0; i < alts->len; i++) {
				char *joined = g_strconcat(prefix, alts->pdata[i], suffix, NULL);
				brace_expand(joined, out, depth + 1);   /* the rest may hold more braces */
				g_free(joined);
			}
			g_ptr_array_unref(alts);
			g_free(prefix);
			g_free(body);
			return;
		}
		g_free(prefix);
		g_free(body);
		/* "{x}" is just text: keep scanning after it */
	}
	g_ptr_array_add(out, g_strdup(s));
}

/* ======================================================================== */
/* 3. Matching words against the files of the directory                      */
/* ======================================================================== */

typedef struct {
	const char *cwd;
	GHashTable *existing;  /* set of names present in cwd */
	GHashTable *hits;      /* name -> FhiHit* */
	FhiGitAction *git;     /* filled in when the line contains git commit / push (may be NULL) */
} Ctx;

static void
hit_free(gpointer p)
{
	FhiHit *h = p;
	g_free(h->name);
	g_free(h);
}

static void
add_hit(Ctx *ctx, const char *name, FhiRole role)
{
	FhiHit *h = g_hash_table_lookup(ctx->hits, name);
	if (!h) {
		h = g_new0(FhiHit, 1);
		h->name = g_strdup(name);
		h->exists = g_hash_table_contains(ctx->existing, name);
		g_hash_table_insert(ctx->hits, h->name, h);
	}
	if (role > h->role) h->role = role;   /* the stronger role wins */
}

/* Does the pattern contain an UNESCAPED * ? or [ ? */
static gboolean
has_glob(const char *pat)
{
	for (const char *p = pat; *p; p++) {
		if (*p == '\\' && p[1]) { p++; continue; }
		if (*p == '*' || *p == '?' || *p == '[') return TRUE;
	}
	return FALSE;
}

/* Remove the protective backslashes again: a\*b -> a*b */
static char *
unescape(const char *pat)
{
	GString *out = g_string_new(NULL);
	for (const char *p = pat; *p; p++) {
		if (*p == '\\' && p[1]) p++;
		g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}

/*
 * One fully brace-expanded word. `role_existing` is used if the file is
 * already there, `role_new` if it is not (FHI_ROLE_NONE = ignore it).
 */
static void
resolve_one(Ctx *ctx, const char *word, FhiRole role_existing, FhiRole role_new)
{
	char *expanded = NULL;

	/* ~ and ~/x mean the home directory */
	if (word[0] == '~' && (word[1] == '\0' || word[1] == '/'))
		word = expanded = g_strconcat(g_get_home_dir(), word + 1, NULL);

	const char *p = word;

	if (*p == '/') {
		/* absolute path: only interesting if it is inside the current directory */
		char *prefix = g_str_has_suffix(ctx->cwd, "/") ? g_strdup(ctx->cwd)
		                                              : g_strconcat(ctx->cwd, "/", NULL);
		gboolean inside = g_str_has_prefix(p, prefix);
		if (inside) p += strlen(prefix);
		g_free(prefix);
		if (!inside) goto done;
	}

	while (p[0] == '.' && p[1] == '/') { p += 2; while (*p == '/') p++; }   /* ./x */
	if (*p == '\0' || strcmp(p, ".") == 0) goto done;
	if (strcmp(p, "..") == 0 || g_str_has_prefix(p, "../")) goto done;       /* outside */

	/* head = first path component, rest = whatever comes after it */
	const char *slash = strchr(p, '/');
	char *head = slash ? g_strndup(p, slash - p) : g_strdup(p);
	const char *rest = slash ? slash + 1 : "";
	while (*rest == '/') rest++;
	/* "docs/a.txt": we can only show `docs` in this list, and only as "touched" */
	gboolean inside_dir = *rest != '\0';

	if (has_glob(head)) {
		GHashTableIter it;
		gpointer key;
		g_hash_table_iter_init(&it, ctx->existing);
		while (g_hash_table_iter_next(&it, &key, NULL))
			/* FNM_PERIOD: a leading dot must be matched explicitly, like in a shell */
			if (fnmatch(head, key, FNM_PERIOD) == 0)
				add_hit(ctx, key, inside_dir ? FHI_ROLE_TOUCH : role_existing);
	} else {
		char *name = unescape(head);
		if (g_hash_table_contains(ctx->existing, name))
			add_hit(ctx, name, inside_dir ? FHI_ROLE_TOUCH : role_existing);
		else if (role_new != FHI_ROLE_NONE)
			add_hit(ctx, name, role_new);
		g_free(name);
	}
	g_free(head);

done:
	g_free(expanded);
}

static void
add_word(Ctx *ctx, const Token *tok, FhiRole role_existing, FhiRole role_new)
{
	if (tok->unknown) return;   /* $var or $(...): we do not know what it is */

	GPtrArray *alts = g_ptr_array_new_with_free_func(g_free);
	brace_expand(tok->pattern, alts, 0);
	for (guint i = 0; i < alts->len; i++)
		resolve_one(ctx, alts->pdata[i], role_existing, role_new);
	g_ptr_array_unref(alts);
}

/* ======================================================================== */
/* 4. Understanding commands                                                 */
/* ======================================================================== */

typedef enum { KIND_GENERIC, KIND_REMOVE, KIND_CREATE, KIND_TEE, KIND_MOVE } Kind;

static Kind
classify(const char *cmd)
{
	static const struct { const char *name; Kind kind; } table[] = {
		{ "rm", KIND_REMOVE }, { "rmdir", KIND_REMOVE }, { "unlink", KIND_REMOVE },
		{ "shred", KIND_REMOVE },
		{ "touch", KIND_CREATE }, { "mkdir", KIND_CREATE },
		{ "tee", KIND_TEE },
		{ "mv", KIND_MOVE }, { "cp", KIND_MOVE }, { "ln", KIND_MOVE },
	};
	for (guint i = 0; i < G_N_ELEMENTS(table); i++)
		if (strcmp(cmd, table[i].name) == 0) return table[i].kind;
	return KIND_GENERIC;
}

/* Words that run another command: `sudo rm x` is really `rm x`. */
static gboolean
is_wrapper(const char *cmd)
{
	static const char *const wrappers[] = {
		"sudo", "doas", "command", "builtin", "exec", "nohup", "time", "env", "nice", "noglob",
		"fakeroot", "fakeroot-sysv", "ionice",
	};
	for (guint i = 0; i < G_N_ELEMENTS(wrappers); i++)
		if (strcmp(cmd, wrappers[i]) == 0) return TRUE;
	return FALSE;
}

/* FOO=bar at the start of a command */
static gboolean
is_assignment(const char *pat)
{
	if (!(isalpha((unsigned char)*pat) || *pat == '_')) return FALSE;
	while (isalnum((unsigned char)*pat) || *pat == '_') pat++;
	return *pat == '=';
}

static gboolean
is_option(const Token *t)
{
	return t->pattern[0] == '-' && t->pattern[1] != '\0';
}


/* ---- git ---------------------------------------------------------------- */

/* Which short options of `git commit` take a value (-m "text", -F file, ...) */
static gboolean
commit_short_takes_arg(char c)
{
	return strchr("mFCct", c) != NULL;
}

static gboolean
commit_long_takes_arg(const char *o)
{
	static const char *const list[] = {
		"--message", "--file", "--reuse-message", "--reedit-message", "--template", "--author",
		"--date", "--cleanup", "--fixup", "--squash", "--trailer", "--pathspec-from-file",
	};
	for (guint i = 0; i < G_N_ELEMENTS(list); i++)
		if (strcmp(o, list[i]) == 0) return TRUE;
	return FALSE;
}

/*
 * `words[from..]` are the arguments after the word "git". Skip git's own options,
 * find the sub-command, and note what a commit / push is about to do.
 */
static void
git_scan(Ctx *ctx, GPtrArray *words, guint from)
{
	if (ctx->git == NULL) return;
	guint i = from;
	/* git -c key=val  --no-pager ... subcommand */
	for (; i < words->len; i++) {
		const Token *t = words->pdata[i];
		if (!is_option(t)) break;
		if (strcmp(t->pattern, "-C") == 0 || strcmp(t->pattern, "--git-dir") == 0 ||
		    strcmp(t->pattern, "--work-tree") == 0)
			return;   /* works on another directory: we can't tell */
		if (strcmp(t->pattern, "-c") == 0) i++;
	}
	if (i >= words->len) return;
	const Token *sub = words->pdata[i++];
	if (sub->unknown) return;

	if (strcmp(sub->pattern, "push") == 0) {
		ctx->git->push = TRUE;
	} else if (strcmp(sub->pattern, "commit") == 0) {
		ctx->git->commit = TRUE;
		gboolean options_done = FALSE;
		for (; i < words->len; i++) {
			const Token *t = words->pdata[i];
			const char *o = t->pattern;
			if (!options_done && strcmp(o, "--") == 0) { options_done = TRUE; continue; }
			if (!options_done && o[0] == '-' && o[1] != '\0') {
				if (o[1] == '-') {
					if (strcmp(o, "--all") == 0) ctx->git->commit_all = TRUE;
					else if (commit_long_takes_arg(o)) i++;
				} else {
					for (const char *c = o + 1; *c; c++) {
						if (*c == 'a') ctx->git->commit_all = TRUE;
						if (commit_short_takes_arg(*c)) {
							if (c[1] == '\0') i++;   /* the value is the next word */
							break;                   /* else it is attached: -m"text" */
						}
					}
				}
				continue;
			}
			ctx->git->has_paths = TRUE;
		}
	}
}

/* ---- build tools and packaging ------------------------------------------ */

/*
 * `make` does not name Makefile, `makepkg` does not name PKGBUILD, `cargo build` does
 * not name Cargo.toml, but those are the files the command is about to read, and it
 * may create or delete folders (build/, target/, src/ pkg/ ...). This table says which.
 *
 *   inputs   files that are read (shown as "touched"). Space separated; a|b|c means
 *            "the first one that exists" (like make picking GNUmakefile before Makefile).
 *   outputs  names that get written (shown as "created", ghosts if they are not there yet).
 *   removes  names that get deleted (only if they exist).
 *   skip     if one of these options is given the command works somewhere else
 *            (make -C dir, cmake --build dir ...), so the inputs are not in this folder.
 *
 * `sub` is the sub-command (`cargo build`); NULL means the rule applies to any.
 */
typedef struct {
	const char *cmd, *sub;
	const char *inputs, *outputs, *removes, *skip;
} BuildRule;

#define MAKEFILES "GNUmakefile|makefile|Makefile"
#define COMPOSE "compose.yaml|compose.yml|docker-compose.yaml|docker-compose.yml"
#define GRADLE "build.gradle|build.gradle.kts settings.gradle|settings.gradle.kts"

static const BuildRule build_rules[] = {
	{ "make",  NULL, MAKEFILES, NULL, NULL, "-C -f --directory --file --makefile" },
	{ "gmake", NULL, MAKEFILES, NULL, NULL, "-C -f --directory --file --makefile" },
	{ "ninja", NULL, "build.ninja", NULL, NULL, "-C -f" },
	{ "cmake", NULL, "CMakeLists.txt CMakePresets.json CMakeUserPresets.json CMakeCache.txt", NULL, NULL,
	  "--build --install --open -E -P --workflow" },
	{ "ctest", NULL, "CTestTestfile.cmake", NULL, NULL, "--test-dir" },
	{ "meson", "setup",   "meson.build meson.options meson_options.txt", NULL, NULL, NULL },
	{ "meson", "compile", "meson.build", NULL, NULL, "-C" },
	{ "just",  NULL, "justfile|Justfile|.justfile", NULL, NULL, "-f --justfile" },

	/* packaging */
	{ "makepkg",     NULL, "PKGBUILD", "src pkg", NULL, "-p" },
	{ "updpkgsums",  NULL, "PKGBUILD", NULL, NULL, NULL },
	{ "dpkg-buildpackage", NULL, "debian", NULL, NULL, NULL },
	{ "debuild",     NULL, "debian", NULL, NULL, NULL },
	{ "dh_make",     NULL, NULL, "debian", NULL, NULL },
	{ "docker",      "build",   "Dockerfile|Containerfile .dockerignore", NULL, NULL, "-f --file" },
	{ "podman",      "build",   "Containerfile|Dockerfile .dockerignore", NULL, NULL, "-f --file" },
	{ "docker",      "compose", COMPOSE, NULL, NULL, "-f --file" },
	{ "docker-compose", NULL,   COMPOSE, NULL, NULL, "-f --file" },

	/* language build tools */
	{ "cargo", "build", "Cargo.toml Cargo.lock", "target", NULL, "--manifest-path" },
	{ "cargo", "run",   "Cargo.toml Cargo.lock", "target", NULL, "--manifest-path" },
	{ "cargo", "test",  "Cargo.toml Cargo.lock", "target", NULL, "--manifest-path" },
	{ "cargo", "check", "Cargo.toml Cargo.lock", "target", NULL, "--manifest-path" },
	{ "cargo", "clean", "Cargo.toml", NULL, "target", "--manifest-path" },
	{ "go",    "build", "go.mod go.sum", NULL, NULL, NULL },
	{ "go",    "run",   "go.mod go.sum", NULL, NULL, NULL },
	{ "go",    "test",  "go.mod go.sum", NULL, NULL, NULL },
	{ "go",    "mod",   "go.mod go.sum", NULL, NULL, NULL },
	{ "npm",   "install", "package.json package-lock.json", "node_modules", NULL, "--prefix" },
	{ "npm",   "i",       "package.json package-lock.json", "node_modules", NULL, "--prefix" },
	{ "npm",   "ci",      "package.json package-lock.json", "node_modules", NULL, "--prefix" },
	{ "npm",   "run",     "package.json", NULL, NULL, "--prefix" },
	{ "npm",   "test",    "package.json", NULL, NULL, "--prefix" },
	{ "npm",   "start",   "package.json", NULL, NULL, "--prefix" },
	{ "pnpm",  "install", "package.json pnpm-lock.yaml", "node_modules", NULL, "--dir" },
	{ "yarn",  "install", "package.json yarn.lock", "node_modules", NULL, "--cwd" },
	{ "mvn",   NULL, "pom.xml", "target", NULL, "-f --file" },
	{ "gradle",  NULL, GRADLE, "build", NULL, "-p --project-dir" },
	{ "gradlew", NULL, GRADLE, "build", NULL, "-p --project-dir" },
};

/* Is `opt` (as typed: "-C", "-Cdir", "--directory=dir") one of the options in the
 * space separated `list`? */
static gboolean
option_listed(const char *list, const char *opt)
{
	gboolean found = FALSE;
	char **v = g_strsplit(list, " ", -1);
	for (char **w = v; *w && !found; w++) {
		gsize n = strlen(*w);
		if (strcmp(opt, *w) == 0) found = TRUE;
		else if ((*w)[1] == '-') found = strncmp(opt, *w, n) == 0 && opt[n] == '=';   /* --long=value */
		else found = strncmp(opt, *w, n) == 0 && opt[1] != '-';                       /* -Cvalue */
	}
	g_strfreev(v);
	return found;
}

/* ---- PKGBUILD: what package file will makepkg produce? ---- */

/* Words of `key=value` or `key=(a b 'c d')` in a PKGBUILD. Values that run code are left out. */
static GPtrArray *
pkgbuild_values(const char *text, const char *key)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	char *needle = g_strdup_printf("%s=", key);
	const char *p = text;
	while ((p = strstr(p, needle)) != NULL) {
		if (p != text && p[-1] != '\n') { p += strlen(needle); continue; }   /* not at line start */
		p += strlen(needle);
		gboolean array = *p == '(';
		if (array) p++;
		GString *w = g_string_new(NULL);
		char quote = 0;
		gboolean has_word = FALSE;
		for (; *p; p++) {
			if (quote) {
				if (*p == quote) quote = 0; else g_string_append_c(w, *p);
			} else if (*p == '\'' || *p == '"') {
				quote = *p; has_word = TRUE;
			} else if (*p == '#' && !has_word && w->len == 0) {
				while (p[1] && p[1] != '\n') p++;              /* comment */
			} else if (g_ascii_isspace(*p) || (array && *p == ')')) {
				if (has_word || w->len) {
					if (!strstr(w->str, "$(") && !strchr(w->str, '`'))
						g_ptr_array_add(out, g_strdup(w->str));
					g_string_truncate(w, 0);
					has_word = FALSE;
				}
				if (array ? *p == ')' : *p == '\n') break;
			} else {
				g_string_append_c(w, *p);
			}
		}
		if (has_word || w->len) g_ptr_array_add(out, g_strdup(w->str));
		g_string_free(w, TRUE);
		break;   /* the first assignment is the one that counts */
	}
	g_free(needle);
	return out;
}

/* Replace $pkgname ${pkgver} ... in a source=() entry. NULL if something unknown is left. */
static char *
pkgbuild_expand(const char *s, const char *name, const char *ver, const char *rel)
{
	static const char *const keys[] = { "pkgname", "pkgver", "pkgrel" };
	const char *vals[] = { name, ver, rel };
	GString *out = g_string_new(NULL);
	for (const char *c = s; *c; c++) {
		if (*c != '$') { g_string_append_c(out, *c); continue; }
		const char *v = c + 1;
		gboolean brace = *v == '{';
		if (brace) v++;
		gboolean ok = FALSE;
		for (guint i = 0; i < 3 && !ok; i++) {
			gsize n = strlen(keys[i]);
			if (strncmp(v, keys[i], n) == 0 && (!brace || v[n] == '}') && vals[i]) {
				g_string_append(out, vals[i]);
				c = v + n - 1 + (brace ? 1 : 0);
				ok = TRUE;
			}
		}
		if (!ok) { g_string_free(out, TRUE); return NULL; }
	}
	return g_string_free(out, FALSE);
}

/* makepkg: the package file(s) it will write, and the local source files it reads */
static void
pkgbuild_preview(Ctx *ctx)
{
	char *path = g_build_filename(ctx->cwd, "PKGBUILD", NULL);
	char *text = NULL;
	gsize len = 0;
	gboolean ok = g_file_get_contents(path, &text, &len, NULL) && len <= 1024 * 1024;
	g_free(path);
	if (!ok) { g_free(text); return; }

	GPtrArray *names = pkgbuild_values(text, "pkgname");
	GPtrArray *ver = pkgbuild_values(text, "pkgver");
	GPtrArray *rel = pkgbuild_values(text, "pkgrel");
	GPtrArray *arch = pkgbuild_values(text, "arch");
	GPtrArray *src = pkgbuild_values(text, "source");
	const char *v = ver->len ? ver->pdata[0] : NULL, *r = rel->len ? rel->pdata[0] : NULL;

	/* "any" if the package says so, else the machine we build on if it is allowed, else the first */
	const char *machine = "x86_64";
#ifdef __aarch64__
	machine = "aarch64";
#endif
	const char *a = arch->len ? arch->pdata[0] : NULL;
	for (guint i = 0; i < arch->len; i++) {
		if (strcmp(arch->pdata[i], "any") == 0) { a = "any"; break; }
		if (strcmp(arch->pdata[i], machine) == 0) a = machine;
	}

	if (v && r && a && !strchr(v, '$')) {
		for (guint i = 0; i < names->len; i++) {
			const char *n = names->pdata[i];
			if (strchr(n, '$') || strchr(n, '/')) continue;
			char *pkg = g_strdup_printf("%s-%s-%s-%s.pkg.tar.zst", n, v, r, a);
			add_hit(ctx, pkg, FHI_ROLE_CREATE);
			g_free(pkg);
		}
	}
	for (guint i = 0; i < src->len; i++) {
		const char *e = src->pdata[i];
		if (strstr(e, "://") || strstr(e, "::")) continue;               /* downloaded, not local */
		char *file = pkgbuild_expand(e, names->len ? names->pdata[0] : NULL, v, r);
		if (file && !strchr(file, '/') && g_hash_table_contains(ctx->existing, file))
			add_hit(ctx, file, FHI_ROLE_TOUCH);
		g_free(file);
	}
	g_ptr_array_unref(names); g_ptr_array_unref(ver); g_ptr_array_unref(rel);
	g_ptr_array_unref(arch); g_ptr_array_unref(src);
	g_free(text);
}

/*
 * `cmd` is the command name, `args` its words without options, `opts` its options
 * (plain strings). The sub-command is the first word (`cargo build`).
 */
static void
build_preview(Ctx *ctx, const char *cmd, GPtrArray *args, GPtrArray *opts)
{
	const char *sub = NULL;
	if (args->len > 0 && !((const Token *)args->pdata[0])->unknown)
		sub = ((const Token *)args->pdata[0])->pattern;

	/* building another folder ("cmake ..", "make -C x"): its files are not in this list */
	gboolean elsewhere = FALSE;
	for (guint i = 0; i < args->len; i++) {
		const char *a = ((const Token *)args->pdata[i])->pattern;
		if (strcmp(a, "..") == 0 || g_str_has_prefix(a, "../")) elsewhere = TRUE;
	}

	for (guint r = 0; r < G_N_ELEMENTS(build_rules); r++) {
		const BuildRule *br = &build_rules[r];
		if (strcmp(br->cmd, cmd) != 0) continue;
		if (br->sub && (sub == NULL || strcmp(br->sub, sub) != 0)) continue;

		gboolean skip = elsewhere;
		for (guint i = 0; br->skip && i < opts->len && !skip; i++)
			skip = option_listed(br->skip, opts->pdata[i]);
		if (skip) return;

		if (br->inputs) {
			char **groups = g_strsplit(br->inputs, " ", -1);
			for (char **g = groups; *g; g++) {
				char **alts = g_strsplit(*g, "|", -1);
				for (char **al = alts; *al; al++)
					if (g_hash_table_contains(ctx->existing, *al)) { add_hit(ctx, *al, FHI_ROLE_TOUCH); break; }
				g_strfreev(alts);
			}
			g_strfreev(groups);
		}
		if (strcmp(cmd, "makepkg") == 0) pkgbuild_preview(ctx);
		if (br->outputs) {
			char **v = g_strsplit(br->outputs, " ", -1);
			for (char **o = v; *o; o++) add_hit(ctx, *o, FHI_ROLE_CREATE);
			g_strfreev(v);
		}
		if (br->removes) {
			char **v = g_strsplit(br->removes, " ", -1);
			for (char **o = v; *o; o++)
				if (g_hash_table_contains(ctx->existing, *o)) add_hit(ctx, *o, FHI_ROLE_REMOVE);
			g_strfreev(v);
		}
		return;
	}
}

/* `words` holds the Token*s of ONE simple command, redirections excluded. */
static void
analyze(Ctx *ctx, GPtrArray *words)
{
	guint i = 0;
	char *cmd = NULL;

	/* skip  VAR=1  and  sudo / env / ...  (plus their -options) */
	while (i < words->len) {
		const Token *t = words->pdata[i];
		if (is_assignment(t->pattern)) { i++; continue; }

		char *plain = unescape(t->pattern);
		cmd = g_path_get_basename(plain);   /* /bin/rm -> rm */
		g_free(plain);
		if (!t->unknown && is_wrapper(cmd)) {
			g_free(cmd);
			cmd = NULL;
			i++;
			while (i < words->len && is_option(words->pdata[i])) i++;
			continue;
		}
		break;
	}
	if (!cmd) return;
	gboolean unknown_cmd = ((const Token *)words->pdata[i])->unknown;
	Kind kind = unknown_cmd ? KIND_GENERIC : classify(cmd);
	if (!unknown_cmd && strcmp(cmd, "git") == 0) git_scan(ctx, words, i + 1);
	/* `./build.sh`: the script you are about to run is in this list too */
	if (!unknown_cmd && g_str_has_prefix(((const Token *)words->pdata[i])->pattern, "./"))
		add_word(ctx, words->pdata[i], FHI_ROLE_TOUCH, FHI_ROLE_NONE);
	i++;
	GPtrArray *opts = g_ptr_array_new();   /* the -options, as plain strings */

	/* collect the arguments, dropping -options (and everything is an argument after "--") */
	GPtrArray *args = g_ptr_array_new();   /* does not own the tokens */
	const Token *target_dir = NULL;        /* from  mv -t DIR  */
	gboolean options_done = FALSE;
	for (; i < words->len; i++) {
		const Token *t = words->pdata[i];
		if (!options_done && strcmp(t->pattern, "--") == 0) { options_done = TRUE; continue; }
		if (!options_done && is_option(t)) {
			g_ptr_array_add(opts, t->pattern);
			if (kind == KIND_MOVE && i + 1 < words->len &&
			    (strcmp(t->pattern, "-t") == 0 || strcmp(t->pattern, "--target-directory") == 0))
				target_dir = words->pdata[++i];
			continue;
		}
		g_ptr_array_add(args, (gpointer)t);
	}

	switch (kind) {
	case KIND_REMOVE:
		for (guint k = 0; k < args->len; k++) add_word(ctx, args->pdata[k], FHI_ROLE_REMOVE, FHI_ROLE_NONE);
		break;
	case KIND_CREATE:
		for (guint k = 0; k < args->len; k++) add_word(ctx, args->pdata[k], FHI_ROLE_TOUCH, FHI_ROLE_CREATE);
		break;
	case KIND_TEE:
		for (guint k = 0; k < args->len; k++) add_word(ctx, args->pdata[k], FHI_ROLE_CREATE, FHI_ROLE_CREATE);
		break;
	case KIND_MOVE: {
		/* mv a b c DEST  ->  the LAST word is the destination (unless -t was used) */
		guint sources = args->len;
		if (target_dir) add_word(ctx, target_dir, FHI_ROLE_DEST, FHI_ROLE_DEST);
		else if (args->len >= 2) {
			sources--;
			add_word(ctx, args->pdata[sources], FHI_ROLE_DEST, FHI_ROLE_DEST);
		}
		for (guint k = 0; k < sources; k++) add_word(ctx, args->pdata[k], FHI_ROLE_TOUCH, FHI_ROLE_NONE);
		break;
	}
	default:
		for (guint k = 0; k < args->len; k++) add_word(ctx, args->pdata[k], FHI_ROLE_TOUCH, FHI_ROLE_NONE);
		break;
	}
	if (!unknown_cmd && kind == KIND_GENERIC) build_preview(ctx, cmd, args, opts);
	g_ptr_array_unref(opts);
	g_ptr_array_unref(args);
	g_free(cmd);
}

static gint
compare_hits(gconstpointer a, gconstpointer b)
{
	const FhiHit *x = *(FhiHit *const *)a, *y = *(FhiHit *const *)b;
	return strcmp(x->name, y->name);
}

GPtrArray *
fhi_preview_parse(const char *buffer, const char *cwd, const char *const *existing)
{
	return fhi_preview_parse_ex(buffer, cwd, existing, NULL);
}

GPtrArray *
fhi_preview_parse_ex(const char *buffer, const char *cwd, const char *const *existing,
                     FhiGitAction *git)
{
	Ctx ctx;
	if (git) memset(git, 0, sizeof *git);
	ctx.git = git;
	ctx.cwd = cwd;
	ctx.existing = g_hash_table_new(g_str_hash, g_str_equal);
	for (; existing && *existing; existing++) g_hash_table_add(ctx.existing, (gpointer)*existing);
	ctx.hits = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, hit_free);

	GPtrArray *tokens = lex(buffer);
	GPtrArray *words = g_ptr_array_new();   /* words of the command being read */
	TokType pending = TOK_WORD;             /* a redirection waiting for its file name */

	for (guint i = 0; i <= tokens->len; i++) {
		const Token *t = i < tokens->len ? tokens->pdata[i] : NULL;

		if (t == NULL || t->type == TOK_SEP) {          /* the command is complete */
			analyze(&ctx, words);
			g_ptr_array_set_size(words, 0);
			pending = TOK_WORD;
		} else if (t->type == TOK_WORD && pending != TOK_WORD) {
			if (pending == TOK_REDIR_OUT) add_word(&ctx, t, FHI_ROLE_CREATE, FHI_ROLE_CREATE);
			else if (pending == TOK_REDIR_IN) add_word(&ctx, t, FHI_ROLE_TOUCH, FHI_ROLE_NONE);
			pending = TOK_WORD;
		} else if (t->type == TOK_WORD) {
			g_ptr_array_add(words, (gpointer)t);
		} else {
			pending = t->type;                          /* > < << */
		}
	}

	GPtrArray *result = g_ptr_array_new_with_free_func(hit_free);
	GHashTableIter it;
	gpointer value;
	g_hash_table_iter_init(&it, ctx.hits);
	while (g_hash_table_iter_next(&it, NULL, &value)) {
		g_hash_table_iter_steal(&it);   /* move the hit into the result array */
		g_ptr_array_add(result, value);
	}
	g_ptr_array_sort(result, compare_hits);

	g_ptr_array_unref(words);
	g_ptr_array_unref(tokens);
	g_hash_table_unref(ctx.hits);
	g_hash_table_unref(ctx.existing);
	return result;
}

/* ======================================================================== */
/* 5. File-list filter (reuses the lexer and brace expansion above)          */
/* ======================================================================== */

typedef struct {
	char *text;        /* case-folded; for a glob still holds its backslashes */
	gboolean glob;
} FilterPat;

struct FhiFilter {
	GPtrArray *include;   /* FilterPat*: a name must match one of these (if any) */
	GPtrArray *exclude;   /* FilterPat*: a name must match none of these */
};

static void
filter_pat_free(gpointer p)
{
	FilterPat *fp = p;
	g_free(fp->text);
	g_free(fp);
}

static char *
fold(const char *s)
{
	return g_utf8_validate(s, -1, NULL) ? g_utf8_casefold(s, -1) : g_strdup(s);
}

FhiFilter *
fhi_filter_new(const char *spec)
{
	FhiFilter *f = g_new0(FhiFilter, 1);
	f->include = g_ptr_array_new_with_free_func(filter_pat_free);
	f->exclude = g_ptr_array_new_with_free_func(filter_pat_free);

	GPtrArray *tokens = lex(spec ? spec : "");
	for (guint i = 0; i < tokens->len; i++) {
		const Token *t = tokens->pdata[i];
		if (t->type != TOK_WORD || t->unknown) continue;

		const char *pattern = t->pattern;
		gboolean negate = pattern[0] == '!';
		if (negate) pattern++;

		GPtrArray *alts = g_ptr_array_new_with_free_func(g_free);
		brace_expand(pattern, alts, 0);
		for (guint k = 0; k < alts->len; k++) {
			const char *alt = alts->pdata[k];
			if (*alt == '\0') continue;
			FilterPat *fp = g_new0(FilterPat, 1);
			fp->glob = has_glob(alt);
			char *base = fp->glob ? g_strdup(alt) : unescape(alt);
			fp->text = fold(base);
			g_free(base);
			g_ptr_array_add(negate ? f->exclude : f->include, fp);
		}
		g_ptr_array_unref(alts);
	}
	g_ptr_array_unref(tokens);
	return f;
}

static gboolean
filter_pat_matches(const FilterPat *fp, const char *folded_name)
{
	if (fp->glob) return fnmatch(fp->text, folded_name, 0) == 0;
	return strstr(folded_name, fp->text) != NULL;
}

gboolean
fhi_filter_matches(const FhiFilter *f, const char *name)
{
	if (f == NULL || (f->include->len == 0 && f->exclude->len == 0)) return TRUE;

	char *folded = fold(name);
	gboolean ok = TRUE;
	for (guint i = 0; ok && i < f->exclude->len; i++)
		if (filter_pat_matches(f->exclude->pdata[i], folded)) ok = FALSE;
	if (ok && f->include->len > 0) {
		ok = FALSE;
		for (guint i = 0; !ok && i < f->include->len; i++)
			ok = filter_pat_matches(f->include->pdata[i], folded);
	}
	g_free(folded);
	return ok;
}

void
fhi_filter_free(FhiFilter *f)
{
	if (f == NULL) return;
	g_ptr_array_unref(f->include);
	g_ptr_array_unref(f->exclude);
	g_free(f);
}
