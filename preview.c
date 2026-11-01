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
	Kind kind = ((const Token *)words->pdata[i])->unknown ? KIND_GENERIC : classify(cmd);
	g_free(cmd);
	i++;

	/* collect the arguments, dropping -options (and everything is an argument after "--") */
	GPtrArray *args = g_ptr_array_new();   /* does not own the tokens */
	const Token *target_dir = NULL;        /* from  mv -t DIR  */
	gboolean options_done = FALSE;
	for (; i < words->len; i++) {
		const Token *t = words->pdata[i];
		if (!options_done && strcmp(t->pattern, "--") == 0) { options_done = TRUE; continue; }
		if (!options_done && is_option(t)) {
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
	g_ptr_array_unref(args);
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
	Ctx ctx;
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
