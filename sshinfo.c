/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#include "sshinfo.h"
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ */
/* The data                                                                   */
/* ------------------------------------------------------------------------ */

/* What the remote machine told us, one entry per "key=value" line it printed */
enum { R_USER, R_UID, R_GROUP, R_GROUPS, R_HOME, R_HOMEPERM, R_UMASK, R_SHELL, R_OS, R_KERNEL,
       R_UPTIME, R_HOSTNAME, R_COUNT };
static const char *const remote_key[R_COUNT] = {
	"user", "uid", "group", "groups", "home", "homeperm", "umask", "shell", "os", "kernel",
	"uptime", "hostname",
};

typedef enum { Q_NONE, Q_RUNNING, Q_DONE, Q_FAILED } QueryState;

struct FhiSsh {
	pid_t pid;
	GStrv argv;                 /* the ssh command line, argv[0] included */
	GPtrArray *keep;            /* options worth repeating for our own queries (char *) */
	char *destination;          /* what the user typed: "user@host" or an alias */
	gboolean has_command;       /* `ssh host something`: not an interactive login */
	int tunnels;                /* number of -L / -R / -D */

	/* from `ssh -G` */
	GHashTable *cfg;            /* lower-case key -> first value */
	gboolean cfg_done;

	/* from the server */
	QueryState query;
	char *remote[R_COUNT];

	GCancellable *cancel;       /* stops the two background commands when we are freed */
};

/* ------------------------------------------------------------------------ */
/* Reading the ssh command line                                               */
/* ------------------------------------------------------------------------ */

/* Options that take a value (see ssh(1)) */
#define SSH_ARG_OPTS "BbcDEeFIiJLlmOopQRSWw"
/* Options we repeat when we ask the server something; the rest are about THIS session */
#define SSH_KEEP_FLAGS "46AaCKkXxYy"
#define SSH_KEEP_ARGS "BbcFIiJlop"

static gboolean
is_control_option(const char *kv)
{
	return g_ascii_strncasecmp(kv, "ControlMaster", 13) == 0 ||
	       g_ascii_strncasecmp(kv, "ControlPersist", 14) == 0;
}

static gboolean
parse_cmdline(FhiSsh *s)
{
	char *file = g_strdup_printf("/proc/%d/cmdline", (int)s->pid);
	char *data = NULL;
	gsize len = 0;
	gboolean ok = g_file_get_contents(file, &data, &len, NULL);
	g_free(file);
	if (!ok || len == 0) return FALSE;

	GPtrArray *args = g_ptr_array_new();
	for (gsize i = 0; i < len;) {
		g_ptr_array_add(args, data + i);
		i += strlen(data + i) + 1;
	}
	s->argv = g_new0(char *, args->len + 1);
	for (guint i = 0; i < args->len; i++) s->argv[i] = g_strdup(g_ptr_array_index(args, i));
	guint n = args->len;
	g_ptr_array_free(args, TRUE);
	g_free(data);

	guint i = 1;
	for (; i < n; i++) {
		const char *a = s->argv[i];
		if (strcmp(a, "--") == 0) { i++; break; }
		if (a[0] != '-' || a[1] == '\0') break;            /* the destination */
		for (const char *c = a + 1; *c; c++) {
			if (strchr(SSH_ARG_OPTS, *c)) {
				const char *val = c[1] ? c + 1 : (i + 1 < n ? s->argv[++i] : NULL);
				if (val == NULL) return FALSE;
				if (strchr("LRD", *c)) s->tunnels++;
				if (strchr(SSH_KEEP_ARGS, *c) && !(*c == 'o' && is_control_option(val))) {
					g_ptr_array_add(s->keep, g_strdup_printf("-%c", *c));
					g_ptr_array_add(s->keep, g_strdup(val));
				}
				break;   /* the rest of this word was the value */
			}
			if (strchr(SSH_KEEP_FLAGS, *c)) g_ptr_array_add(s->keep, g_strdup_printf("-%c", *c));
		}
	}
	if (i >= n) return FALSE;                               /* ssh without a destination */
	s->destination = g_strdup(s->argv[i]);
	s->has_command = i + 1 < n;
	return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Background commands                                                        */
/* ------------------------------------------------------------------------ */

/* Start `ssh <our_opts> <kept options> <destination> <tail...>`. NULL on failure. */
static GSubprocess *
start_ssh(const FhiSsh *s, const char *const *our_opts, const char *const *tail)
{
	GPtrArray *a = g_ptr_array_new();
	g_ptr_array_add(a, (gpointer)"ssh");
	for (; our_opts && *our_opts; our_opts++) g_ptr_array_add(a, (gpointer)*our_opts);
	for (guint i = 0; i < s->keep->len; i++) g_ptr_array_add(a, g_ptr_array_index(s->keep, i));
	g_ptr_array_add(a, (gpointer)"--");
	g_ptr_array_add(a, s->destination);
	for (; tail && *tail; tail++) g_ptr_array_add(a, (gpointer)*tail);
	g_ptr_array_add(a, NULL);

	GSubprocess *p = g_subprocess_newv((const gchar *const *)a->pdata,
	                                   G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                   G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
	g_ptr_array_free(a, TRUE);
	return p;
}

static void
on_config(GObject *src, GAsyncResult *res, gpointer user_data)
{
	char *out = NULL;
	GError *err = NULL;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err);
	if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {   /* `s` may be gone */
		g_error_free(err);
		g_free(out);
		return;
	}
	g_clear_error(&err);
	FhiSsh *s = user_data;
	if (out) {
		char **lines = g_strsplit(out, "\n", -1);
		for (char **l = lines; *l; l++) {
			char *sp = strchr(*l, ' ');
			if (sp == NULL) continue;
			*sp = '\0';
			if (!g_hash_table_contains(s->cfg, *l))   /* identityfile etc. repeat: keep the first */
				g_hash_table_insert(s->cfg, g_strdup(*l), g_strdup(sp + 1));
		}
		g_strfreev(lines);
	}
	g_free(out);
	s->cfg_done = TRUE;
}

/* One printf per fact, so a missing command only loses its own line */
#define REMOTE_SCRIPT \
	"printf 'user=%s\\n' \"$(id -un)\";" \
	"printf 'uid=%s\\n' \"$(id -u)\";" \
	"printf 'group=%s\\n' \"$(id -gn)\";" \
	"printf 'groups=%s\\n' \"$(id -Gn)\";" \
	"printf 'home=%s\\n' \"$HOME\";" \
	"printf 'homeperm=%s\\n' \"$(stat -c '%A' \"$HOME\" 2>/dev/null)\";" \
	"printf 'umask=%s\\n' \"$(umask)\";" \
	"printf 'shell=%s\\n' \"$SHELL\";" \
	"printf 'os=%s\\n' \"$(. /etc/os-release 2>/dev/null && printf %s \"$PRETTY_NAME\")\";" \
	"printf 'kernel=%s\\n' \"$(uname -sr)\";" \
	"printf 'uptime=%s\\n' \"$(uptime -p 2>/dev/null)\";" \
	"printf 'hostname=%s\\n' \"$(hostname 2>/dev/null)\""

static void
on_remote(GObject *src, GAsyncResult *res, gpointer user_data)
{
	char *out = NULL;
	GError *err = NULL;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err);
	if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_error_free(err);
		g_free(out);
		return;
	}
	g_clear_error(&err);
	FhiSsh *s = user_data;
	int found = 0;
	if (out) {
		char **lines = g_strsplit(out, "\n", -1);
		for (char **l = lines; *l; l++) {
			char *eq = strchr(*l, '=');
			if (eq == NULL) continue;
			*eq = '\0';
			for (int k = 0; k < R_COUNT; k++)
				if (strcmp(*l, remote_key[k]) == 0 && eq[1] != '\0') {
					g_free(s->remote[k]);
					s->remote[k] = g_strdup(eq + 1);
					found++;
				}
		}
		g_strfreev(lines);
	}
	g_free(out);
	s->query = found > 0 ? Q_DONE : Q_FAILED;
}

static void
start_queries(FhiSsh *s)
{
	/* 1. what the config resolves to (no network) */
	static const char *const g_opts[] = { "-G", NULL };
	GSubprocess *p = start_ssh(s, g_opts, NULL);
	if (p) {
		g_subprocess_communicate_utf8_async(p, NULL, s->cancel, on_config, s);
		g_object_unref(p);
	} else {
		s->cfg_done = TRUE;
	}

	/* 2. who we are on the server. BatchMode: never ask anything, never hang. */
	const char *no = g_getenv("FHI_SSH_NO_QUERY");
	if (no && *no && strcmp(no, "0") != 0) return;
	static const char *const q_opts[] = {
		"-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "ControlMaster=no",
		"-o", "ControlPersist=no", "-o", "LogLevel=ERROR", NULL,
	};
	const char *tail[] = { REMOTE_SCRIPT, NULL };
	p = start_ssh(s, q_opts, tail);
	if (p) {
		s->query = Q_RUNNING;
		g_subprocess_communicate_utf8_async(p, NULL, s->cancel, on_remote, s);
		g_object_unref(p);
	} else {
		s->query = Q_FAILED;
	}
}

/* ------------------------------------------------------------------------ */
/* Public: creating / freeing                                                 */
/* ------------------------------------------------------------------------ */

FhiSsh *
fhi_ssh_new(pid_t pid)
{
	FhiSsh *s = g_new0(FhiSsh, 1);
	s->pid = pid;
	s->keep = g_ptr_array_new_with_free_func(g_free);
	s->cfg = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	s->cancel = g_cancellable_new();
	s->query = Q_NONE;
	if (!parse_cmdline(s)) {
		fhi_ssh_free(s);
		return NULL;
	}
	start_queries(s);
	return s;
}

void
fhi_ssh_free(FhiSsh *s)
{
	if (s == NULL) return;
	if (s->cancel) {
		g_cancellable_cancel(s->cancel);   /* callbacks see "cancelled" and leave `s` alone */
		g_object_unref(s->cancel);
	}
	g_strfreev(s->argv);
	if (s->keep) g_ptr_array_free(s->keep, TRUE);
	if (s->cfg) g_hash_table_unref(s->cfg);
	g_free(s->destination);
	for (int k = 0; k < R_COUNT; k++) g_free(s->remote[k]);
	g_free(s);
}

pid_t
fhi_ssh_pid(const FhiSsh *s)
{
	return s->pid;
}

/* ------------------------------------------------------------------------ */
/* Values for the card                                                        */
/* ------------------------------------------------------------------------ */

/* Seconds since the ssh process started, or -1 */
static double
process_age(pid_t pid)
{
	char *file = g_strdup_printf("/proc/%d/stat", (int)pid);
	char *stat = NULL, *up = NULL;
	double age = -1;
	if (g_file_get_contents(file, &stat, NULL, NULL) &&
	    g_file_get_contents("/proc/uptime", &up, NULL, NULL)) {
		/* The process name (field 2) may contain spaces: count fields after its ")" */
		const char *p = strrchr(stat, ')');
		if (p) {
			char **f = g_strsplit(p + 2, " ", -1);   /* f[0] is field 3 */
			if (g_strv_length(f) > 19) {
				double started = g_ascii_strtod(f[19], NULL) / (double)sysconf(_SC_CLK_TCK);
				age = g_ascii_strtod(up, NULL) - started;
			}
			g_strfreev(f);
		}
	}
	g_free(file);
	g_free(stat);
	g_free(up);
	return age < 0 ? -1 : age;
}

static char *
format_duration(double secs)
{
	long t = (long)secs;
	long d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60, sec = t % 60;
	if (d > 0) return g_strdup_printf("%ldd %ldh %02ldm", d, h, m);
	if (h > 0) return g_strdup_printf("%ldh %02ldm %02lds", h, m, sec);
	if (m > 0) return g_strdup_printf("%ldm %02lds", m, sec);
	return g_strdup_printf("%lds", sec);
}

static const char *
cfg_get(const FhiSsh *s, const char *key)
{
	return g_hash_table_lookup(s->cfg, key);
}

/* "none" and friends mean the option is off */
static gboolean
cfg_yes(const FhiSsh *s, const char *key)
{
	const char *v = cfg_get(s, key);
	return v && (strcmp(v, "yes") == 0 || strcmp(v, "true") == 0);
}

/* What the permission string of the home directory means for the others, in words */
static char *
describe_home(const FhiSsh *s)
{
	const char *home = s->remote[R_HOME], *perm = s->remote[R_HOMEPERM];
	if (home == NULL) return NULL;
	if (perm == NULL || strlen(perm) < 10) return g_strdup(home);
	const char *who = (perm[4] != '-' || perm[7] != '-') ? "  (readable by others)" : "";
	return g_strdup_printf("%s  %s%s", home, perm, who);
}

/* ------------------------------------------------------------------------ */
/* The card                                                                   */
/* ------------------------------------------------------------------------ */

enum { ROW_HOST, ROW_PORT, ROW_TIME, ROW_JUMP, ROW_FWD, ROW_PID,
       ROW_STATUS, ROW_ACCOUNT, ROW_GROUP, ROW_GROUPS, ROW_HOME, ROW_UMASK, ROW_SHELL,
       ROW_OS, ROW_KERNEL, ROW_UPTIME, ROW_COUNT };
static const char *const row_title[ROW_COUNT] = {
	"Host", "Port", "Connected", "Jump host", "Forwarding", "Local PID",
	"Remote", "Account", "Group", "Also in", "Home", "Umask", "Shell",
	"System", "Kernel", "Host up",
};

/* Below this width the labels go ABOVE their values instead of beside them */
#define NARROW_PX 300

typedef struct {
	GtkWidget *outer;          /* the scrolled window around the grid */
	GtkWidget *grid;
	GtkWidget *title;
	GtkWidget *subtitle;
	GtkWidget *key[ROW_COUNT];
	GtkWidget *val[ROW_COUNT];
	GtkWidget *sep;            /* line between the connection and the account */
	int narrow;                /* -1 = not laid out yet, else 0 / 1 */
} Card;

static void
card_free(gpointer p)
{
	g_free(p);
}

/* Move a widget inside the grid without removing it */
static void
place(GtkWidget *grid, GtkWidget *w, int col, int row, int span)
{
	GtkLayoutManager *lm = gtk_widget_get_layout_manager(grid);
	GtkGridLayoutChild *lc = GTK_GRID_LAYOUT_CHILD(gtk_layout_manager_get_layout_child(lm, w));
	gtk_grid_layout_child_set_column(lc, col);
	gtk_grid_layout_child_set_row(lc, row);
	gtk_grid_layout_child_set_column_span(lc, span);
}

/*
 * Wide:    Label   value            (two columns, labels right-aligned)
 * Narrow:  Label                    (one column: the value gets the whole width)
 *          value
 */
static void
card_layout(Card *c, gboolean narrow)
{
	c->narrow = narrow;
	int r = 2;
	for (int i = 0; i < ROW_COUNT; i++) {
		if (i == ROW_STATUS) place(c->grid, c->sep, 0, r++, 2);
		if (narrow) {
			place(c->grid, c->key[i], 0, r++, 2);
			place(c->grid, c->val[i], 0, r++, 2);
		} else {
			place(c->grid, c->key[i], 0, r, 1);
			place(c->grid, c->val[i], 1, r, 1);
			r++;
		}
		gtk_label_set_xalign(GTK_LABEL(c->key[i]), narrow ? 0 : 1);
		gtk_widget_set_margin_top(c->key[i], narrow ? 4 : 0);
	}
	gtk_grid_set_column_spacing(GTK_GRID(c->grid), narrow ? 0 : 12);
	gtk_grid_set_row_spacing(GTK_GRID(c->grid), narrow ? 0 : 3);
	if (narrow) gtk_widget_add_css_class(c->outer, "fhi-sshnarrow");
	else gtk_widget_remove_css_class(c->outer, "fhi-sshnarrow");
}

/* Every frame while visible: switch layout when the panel crosses the width limit */
static gboolean
card_tick(GtkWidget *outer, GdkFrameClock *clock, gpointer data)
{
	Card *c = g_object_get_data(G_OBJECT(outer), "fhi-card");
	int w = gtk_widget_get_width(outer);
	if (c && w > 0) {
		int narrow = w < NARROW_PX;
		if (narrow != c->narrow) card_layout(c, narrow);
	}
	return G_SOURCE_CONTINUE;
}

static GtkWidget *
make_label(const char *text, gboolean wrap)
{
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	if (wrap) {
		/* A wrapping label takes the width it is given, however small; long
		 * words and paths break anywhere instead of pushing the card wider. */
		gtk_label_set_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
		gtk_label_set_width_chars(GTK_LABEL(l), 1);
		gtk_label_set_max_width_chars(GTK_LABEL(l), -1);
	}
	return l;
}

GtkWidget *
fhi_ssh_card_new(void)
{
	Card *c = g_new0(Card, 1);
	c->narrow = -1;
	GtkWidget *grid = gtk_grid_new();
	c->grid = grid;
	gtk_widget_set_hexpand(grid, TRUE);
	gtk_widget_set_valign(grid, GTK_ALIGN_START);

	/* One line: a long host name is cut in the middle (it is shown whole in the Host row) */
	c->title = make_label(NULL, FALSE);
	gtk_label_set_ellipsize(GTK_LABEL(c->title), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_add_css_class(c->title, "fhi-sshtitle");
	gtk_grid_attach(GTK_GRID(grid), c->title, 0, 0, 2, 1);
	c->subtitle = make_label(NULL, TRUE);
	gtk_widget_add_css_class(c->subtitle, "dim-label");
	gtk_widget_set_margin_bottom(c->subtitle, 4);
	gtk_grid_attach(GTK_GRID(grid), c->subtitle, 0, 1, 2, 1);

	c->sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_margin_top(c->sep, 6);
	gtk_widget_set_margin_bottom(c->sep, 6);
	gtk_grid_attach(GTK_GRID(grid), c->sep, 0, 0, 2, 1);
	for (int i = 0; i < ROW_COUNT; i++) {
		c->key[i] = make_label(row_title[i], FALSE);
		gtk_widget_set_valign(c->key[i], GTK_ALIGN_START);
		gtk_widget_add_css_class(c->key[i], "dim-label");
		c->val[i] = make_label(NULL, TRUE);
		gtk_widget_set_hexpand(c->val[i], TRUE);
		gtk_widget_set_valign(c->val[i], GTK_ALIGN_START);
		gtk_grid_attach(GTK_GRID(grid), c->key[i], 0, 0, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), c->val[i], 1, 0, 1, 1);
	}

	/* The grid sits in a scrolled window, so a tall card scrolls instead of being cut off.
	 * The window fills the width of the panel; it is as tall as its content, up to the panel. */
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
	gtk_widget_add_css_class(scroll, "fhi-sshcard");
	gtk_widget_set_halign(scroll, GTK_ALIGN_FILL);
	gtk_widget_set_valign(scroll, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start(scroll, 8);
	gtk_widget_set_margin_end(scroll, 8);
	gtk_widget_set_margin_top(scroll, 8);
	gtk_widget_set_margin_bottom(scroll, 8);
	gtk_widget_set_visible(scroll, FALSE);

	c->outer = scroll;
	g_object_set_data_full(G_OBJECT(scroll), "fhi-card", c, card_free);
	card_layout(c, FALSE);
	gtk_widget_add_tick_callback(scroll, card_tick, NULL, NULL);
	return scroll;
}

/* Show `text` in row `i`, or hide the row if there is none */
static void
set_row(Card *c, int i, const char *text)
{
	gboolean show = text != NULL && *text != '\0';
	gtk_widget_set_visible(c->key[i], show);
	gtk_widget_set_visible(c->val[i], show);
	if (show) {
		const char *old = gtk_label_get_text(GTK_LABEL(c->val[i]));
		if (strcmp(old, text) != 0) gtk_label_set_text(GTK_LABEL(c->val[i]), text);
	}
}

void
fhi_ssh_card_update(GtkWidget *card, const FhiSsh *s)
{
	Card *c = g_object_get_data(G_OBJECT(card), "fhi-card");
	if (s == NULL) {
		gtk_widget_set_visible(card, FALSE);
		return;
	}
	gtk_widget_set_visible(card, TRUE);

	/* --- connection (known from our side) --- */
	const char *host = cfg_get(s, "hostname");
	const char *user = s->remote[R_USER] ? s->remote[R_USER] : cfg_get(s, "user");
	char *title = g_strdup_printf("%s%s%s", user ? user : "", user ? "@" : "", host ? host : s->destination);
	gtk_label_set_text(GTK_LABEL(c->title), title);
	g_free(title);

	if (s->has_command) gtk_label_set_text(GTK_LABEL(c->subtitle), "ssh is running a single command");
	else gtk_label_set_text(GTK_LABEL(c->subtitle), "SSH session: the file list is paused");

	char *hostline = NULL;
	if (host && strcmp(host, s->destination) != 0 && strchr(s->destination, '@') == NULL)
		hostline = g_strdup_printf("%s  (as \"%s\")", host, s->destination);
	set_row(c, ROW_HOST, hostline ? hostline : host ? host : s->destination);
	g_free(hostline);
	set_row(c, ROW_PORT, cfg_get(s, "port"));

	double age = process_age(s->pid);
	char *dur = age >= 0 ? format_duration(age) : NULL;
	set_row(c, ROW_TIME, dur);
	g_free(dur);

	const char *jump = cfg_get(s, "proxyjump");
	set_row(c, ROW_JUMP, jump && strcmp(jump, "none") != 0 ? jump : NULL);

	GString *fwd = g_string_new(NULL);
	if (cfg_yes(s, "forwardagent")) g_string_append(fwd, "agent, ");
	if (cfg_yes(s, "forwardx11") || cfg_yes(s, "forwardx11trusted")) g_string_append(fwd, "X11, ");
	if (s->tunnels > 0) g_string_append_printf(fwd, "%d tunnel%s, ", s->tunnels, s->tunnels == 1 ? "" : "s");
	if (cfg_yes(s, "compression")) g_string_append(fwd, "compressed, ");
	if (fwd->len > 2) g_string_truncate(fwd, fwd->len - 2);
	set_row(c, ROW_FWD, fwd->str);
	g_string_free(fwd, TRUE);

	char *pid = g_strdup_printf("%d", (int)s->pid);
	set_row(c, ROW_PID, pid);
	g_free(pid);

	/* --- the account on the server --- */
	const char *status = NULL;
	if (s->query == Q_RUNNING) status = "asking the server…";
	else if (s->query == Q_FAILED) status = "unavailable (needs key, agent or ControlMaster login)";
	else if (s->query == Q_NONE) status = "not asked (FHI_SSH_NO_QUERY is set)";
	set_row(c, ROW_STATUS, status);
	gtk_widget_set_visible(c->sep, TRUE);

	char *account = NULL;
	if (s->remote[R_USER]) {
		const char *uid = s->remote[R_UID];
		account = g_strdup_printf("%s%s%s%s", s->remote[R_USER],
		                          uid ? "  (uid " : "", uid ? uid : "", uid ? ")" : "");
		if (uid && strcmp(uid, "0") == 0) {
			char *tmp = g_strconcat(account, "  ROOT", NULL);
			g_free(account);
			account = tmp;
		}
	}
	set_row(c, ROW_ACCOUNT, account);
	g_free(account);
	set_row(c, ROW_GROUP, s->remote[R_GROUP]);
	set_row(c, ROW_GROUPS, s->remote[R_GROUPS]);
	char *home = describe_home(s);
	set_row(c, ROW_HOME, home);
	g_free(home);
	set_row(c, ROW_UMASK, s->remote[R_UMASK]);
	set_row(c, ROW_SHELL, s->remote[R_SHELL]);
	set_row(c, ROW_OS, s->remote[R_OS]);
	set_row(c, ROW_KERNEL, s->remote[R_KERNEL]);
	set_row(c, ROW_UPTIME, s->remote[R_UPTIME]);
}
