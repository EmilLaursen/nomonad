/* SPDX-License-Identifier: 0BSD */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>

#include "ctl.h"
#include "layout.h"
#include "nomonad.h"
#include "restore.h"
#include "spawnd.h"
#include "status.h"

#define NM_RESTORE_MAGIC "nomonad-restore 1"
#define NM_RESTORE_MAX_LINES 4096

struct RestoreHandler {
	const char *prefix;
	void (*apply)(const char *line);
};

static char g_path[PATH_MAX];
static char g_exe[PATH_MAX];
static size_t g_n_lines;
static bool g_requested;
static bool g_stopping;
static char **g_lines;
static char **g_argv;

void restore_set_argv(char **argv)
{
	g_argv = argv;
}

void restore_request(void)
{
	static const char deleted[] = " (deleted)";
	const size_t dl = sizeof(deleted) - 1;
	ssize_t n;

	n = readlink("/proc/self/exe", g_exe, sizeof(g_exe) - 1);
	if (n <= 0) {
		fprintf(stderr, "nomonad: restart: cannot resolve binary: %s\n",
			strerror(errno));
		return;
	}
	g_exe[n] = '\0';
	if ((size_t)n > dl && strcmp(g_exe + n - dl, deleted) == 0)
		g_exe[n - dl] = '\0';
	if (g_argv == NULL || access(g_exe, X_OK) != 0) {
		fprintf(stderr, "nomonad: restart: %s is not executable\n",
			g_exe);
		return;
	}
	g_requested = true;
}

static int restore_path(char *buf, size_t cap)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	int n;

	if (dir != NULL && dir[0] == '/')
		n = snprintf(buf, cap, "%s/nomonad-restore-%ld", dir,
			     (long)getpid());
	else
		n = snprintf(buf, cap, "/tmp/nomonad-restore-%u-%ld",
			     (unsigned)getuid(), (long)getpid());
	return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static bool name_ok(const char *s)
{
	if (s == NULL || s[0] == '\0' || strlen(s) > 63)
		return false;
	for (; *s != '\0'; s++)
		if (*s <= ' ' || *s > '~')
			return false;
	return true;
}

static void save_window(FILE *f, const struct Window *w)
{
	s32 x, y, width, height;

	if (w->closed || !layout_id_ok(w->identifier))
		return;
	x = w->float_x;
	y = w->float_y;
	width = w->float_width;
	height = w->float_height;
	if (w->floating && !w->fullscreen) {
		x = w->x;
		y = w->y;
		width = w->width;
		height = w->height;
	}
	fprintf(f, "win %s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %s %d\n",
		w->identifier, w->workspace_index,
		w->fullscreen ? (w->fullscreen_client ? 2 : 1) : 0,
		(int)w->float_override, w->floating, x, y, width, height,
		w->float_anchored, w->anchor_x, w->anchor_y, w->anchor_width,
		w->anchor_height, w->is_scratchpad,
		name_ok(w->scratch_name) ? w->scratch_name : "-", w->sticky);
}

static void save_workspace(FILE *f, size_t i)
{
	const struct Workspace *ws = &wm.workspaces[i];
	const struct Layout *l;
	const struct Window *w;

	fprintf(f, "ws %zu %zu %zu\n", i, ws->current_layout_index,
		ws->n_layouts);
	for (size_t k = 0; k < ws->n_layouts; k++) {
		l = &ws->layouts[k];
		if (l->kind == LAYOUT_BSP) {
			fprintf(f, "bsp %zu %zu", i, k);
			layout_bsp_save(l, f);
			fputc('\n', f);
			continue;
		}
		fprintf(f, "tall %zu %zu %d %d %.6f\n", i, k, (int)l->kind,
			l->u.tall.nmaster, (double)l->u.tall.ratio);
	}
	wl_list_for_each(w, &ws->windows, workspace_link)
		save_window(f, w);
}

int restore_save(const char *path)
{
	const struct Window *focused;
	const struct Output *out;
	const struct Seat *seat;
	FILE *f;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
		  0600);
	if (fd < 0)
		return -1;
	f = fdopen(fd, "w");
	if (f == NULL) {
		close(fd);
		return -1;
	}
	fprintf(f, "%s\n", NM_RESTORE_MAGIC);
	wl_list_for_each(out, &wm.outputs, link) {
		if (!out->removed)
			fprintf(f, "output %d %d %d\n", out->x, out->y,
				out->visible_workspace);
	}
	for (size_t i = 0; i < wm.n_workspaces; i++)
		save_workspace(f, i);
	if (!wl_list_empty(&wm.seats)) {
		seat = wl_container_of(wm.seats.next, seat, link);
		if (seat->output != NULL)
			fprintf(f, "seatout %d %d\n", seat->output->x,
				seat->output->y);
		focused = seat->focused;
		if (focused != NULL && layout_id_ok(focused->identifier))
			fprintf(f, "focus %s\n", focused->identifier);
	}
	return fclose(f) == 0 ? 0 : -1;
}

bool restore_take_request(void)
{
	if (!g_requested || g_stopping)
		return false;
	g_stopping = true;
	if (restore_path(g_path, sizeof(g_path)) != 0 ||
	    restore_save(g_path) != 0) {
		fprintf(stderr,
			"nomonad: restart: cannot write state, windows will "
			"not be restored\n");
		g_path[0] = '\0';
	}
	return true;
}

const char *restore_state_path(void)
{
	return g_stopping ? g_path : NULL;
}

static void restore_free_lines(void)
{
	for (size_t i = 0; i < g_n_lines; i++)
		free(g_lines[i]);
	free(g_lines);
	g_lines = NULL;
	g_n_lines = 0;
}

static int restore_read(FILE *f)
{
	size_t cap = 0;
	char *line = NULL;
	size_t n_alloc = 0;
	bool first = true;
	bool magic = false;
	ssize_t n;
	char **grown;

	while ((n = getline(&line, &cap, f)) > 0) {
		if (line[n - 1] == '\n')
			line[--n] = '\0';
		if (first) {
			first = false;
			magic = strcmp(line, NM_RESTORE_MAGIC) == 0;
			if (!magic)
				break;
			continue;
		}
		if (g_n_lines >= NM_RESTORE_MAX_LINES)
			break;
		if (g_n_lines == n_alloc) {
			n_alloc = n_alloc ? n_alloc * 2 : 64;
			grown = realloc(g_lines, n_alloc * sizeof(*grown));
			if (grown == NULL)
				break;
			g_lines = grown;
		}
		g_lines[g_n_lines] = strdup(line);
		if (g_lines[g_n_lines] == NULL)
			break;
		g_n_lines++;
	}
	free(line);
	return magic ? 0 : -1;
}

int restore_load(const char *path)
{
	struct stat st;
	FILE *f;
	int fd;
	int r;

	restore_free_lines();
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_uid != getuid()) {
		close(fd);
		return -1;
	}
	(void)unlink(path);
	f = fdopen(fd, "r");
	if (f == NULL) {
		close(fd);
		return -1;
	}
	r = restore_read(f);
	fclose(f);
	return r;
}

static struct Window *find_window(const char *id)
{
	struct Window *w;

	for (size_t i = 0; i < wm.n_workspaces; i++) {
		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			if (!w->closed && w->identifier != NULL &&
			    strcmp(w->identifier, id) == 0)
				return w;
		}
	}
	return NULL;
}

static struct Output *find_output(int x, int y)
{
	struct Output *out;

	wl_list_for_each(out, &wm.outputs, link) {
		if (!out->removed && out->x == x && out->y == y)
			return out;
	}
	return NULL;
}

static struct Window *bsp_lookup(void *ctx, const char *id)
{
	const int *wsi = ctx;
	struct Window *w;

	w = find_window(id);
	return (w != NULL && w->workspace_index == *wsi) ? w : NULL;
}

static void apply_output(const char *line)
{
	struct Output *out;
	struct Output *o;
	int x, y, wsi;

	if (sscanf(line, "output %d %d %d", &x, &y, &wsi) != 3)
		return;
	if (wsi < 0 || (size_t)wsi >= wm_user_n_workspaces())
		return;
	out = find_output(x, y);
	if (out == NULL || out->visible_workspace == wsi)
		return;
	wl_list_for_each(o, &wm.outputs, link) {
		if (o != out && !o->removed && o->visible_workspace == wsi)
			o->visible_workspace = out->visible_workspace;
	}
	out->visible_workspace = wsi;
}

static void apply_window(const char *line)
{
	int wsi, fs, ovr, flt, x, y, width, height, anc, ax, ay, aw, ah, pad;
	int sticky = 0;
	int n;
	char name[64];
	struct Window *w;
	char id[33];

	n = sscanf(line,
		   "win %32s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %63s %d",
		   id, &wsi, &fs, &ovr, &flt, &x, &y, &width, &height, &anc,
		   &ax, &ay, &aw, &ah, &pad, name, &sticky);
	if (n != 16 && n != 17)
		return;
	w = find_window(id);
	if (w == NULL || wsi < 0 || (size_t)wsi >= wm.n_workspaces ||
	    ovr < FLOAT_AUTO || ovr > FLOAT_FORCE_TILE)
		return;
	wl_list_remove(&w->workspace_link);
	wl_list_insert(wm.workspaces[wsi].windows.prev, &w->workspace_link);
	w->workspace_index = wsi;
	w->float_override = (enum FloatOverride)ovr;
	w->floating = flt != 0;
	w->float_x = x;
	w->float_y = y;
	w->float_width = width;
	w->float_height = height;
	w->float_anchored = anc != 0;
	w->anchor_x = ax;
	w->anchor_y = ay;
	w->anchor_width = aw;
	w->anchor_height = ah;
	w->is_scratchpad = pad != 0;
	w->sticky = sticky != 0;
	free(w->scratch_name);
	w->scratch_name = NULL;
	if (pad != 0 && strcmp(name, "-") != 0)
		w->scratch_name = strdup(name);
	w->rules_dirty = true;
	if (fs != 0 && !w->fullscreen_seen) {
		w->fullscreen = true;
		w->fullscreen_seen = true;
		w->fullscreen_client = fs == 2;
		if (w->fullscreen_client) {
			w->fullscreen_informed = true;
			river_window_v1_inform_fullscreen(w->obj);
		}
	}
	if (flt != 0 && fs == 0 && width > 0 && height > 0) {
		window_set_position(w, x, y);
		window_propose_dimensions(w, width, height);
	}
}

static void apply_ws(const char *line)
{
	size_t wsi, cur, n;

	if (sscanf(line, "ws %zu %zu %zu", &wsi, &cur, &n) != 3)
		return;
	if (wsi >= wm.n_workspaces || n != wm.workspaces[wsi].n_layouts ||
	    cur >= n)
		return;
	wm.workspaces[wsi].current_layout_index = cur;
}

static void apply_tall(const char *line)
{
	struct Layout *l;
	size_t wsi, k;
	int kind, nmaster;
	float ratio;

	if (sscanf(line, "tall %zu %zu %d %d %f", &wsi, &k, &kind, &nmaster,
		   &ratio) != 5)
		return;
	if (wsi >= wm.n_workspaces || k >= wm.workspaces[wsi].n_layouts)
		return;
	l = &wm.workspaces[wsi].layouts[k];
	if ((int)l->kind != kind || l->kind == LAYOUT_BSP || nmaster < 0 ||
	    nmaster > 1000 || !(ratio > 0.0f && ratio < 1.0f))
		return;
	l->u.tall.nmaster = nmaster;
	l->u.tall.ratio = ratio;
}

static void apply_bsp(const char *line)
{
	const char *rest;
	struct Layout *l;
	size_t wsi, k;
	int wsi_int;
	int off = 0;

	if (sscanf(line, "bsp %zu %zu%n", &wsi, &k, &off) != 2 || off == 0)
		return;
	if (wsi >= wm.n_workspaces || k >= wm.workspaces[wsi].n_layouts)
		return;
	l = &wm.workspaces[wsi].layouts[k];
	rest = line + off;
	while (*rest == ' ')
		rest++;
	if (l->kind != LAYOUT_BSP || *rest == '\0')
		return;
	wsi_int = (int)wsi;
	if (!layout_bsp_load(l, rest, bsp_lookup, &wsi_int))
		fprintf(stderr, "nomonad: restore: bad bsp tree for ws %zu\n",
			wsi + 1);
}

static struct Seat *first_seat(void)
{
	struct Seat *seat;

	if (wl_list_empty(&wm.seats))
		return NULL;
	return wl_container_of(wm.seats.next, seat, link);
}

static void apply_seatout(const char *line)
{
	struct Output *out;
	struct Seat *seat;
	int x, y;

	seat = first_seat();
	if (seat == NULL || sscanf(line, "seatout %d %d", &x, &y) != 2)
		return;
	out = find_output(x, y);
	if (out != NULL)
		seat->output = out;
}

static void apply_focus(const char *line)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *w;
	char id[33];

	seat = first_seat();
	if (seat == NULL || sscanf(line, "focus %32s", id) != 1)
		return;
	w = find_window(id);
	if (w == NULL)
		return;
	out = output_showing(w->workspace_index);
	if (out == NULL)
		return;
	seat->output = out;
	seat->interacted = w;
}

void restore_apply(void)
{
	static const struct RestoreHandler handlers[] = {
		{ "output ", apply_output }, { "win ", apply_window },
		{ "ws ", apply_ws },	     { "tall ", apply_tall },
		{ "bsp ", apply_bsp },	     { "seatout ", apply_seatout },
		{ "focus ", apply_focus },
	};
	size_t plen;

	if (g_lines == NULL)
		return;
	for (size_t h = 0; h < ARRAY_SIZE(handlers); h++) {
		plen = strlen(handlers[h].prefix);
		for (size_t i = 0; i < g_n_lines; i++)
			if (strncmp(g_lines[i], handlers[h].prefix, plen) == 0)
				handlers[h].apply(g_lines[i]);
	}
	restore_free_lines();
	wm.dirty_layout = true;
	wm.dirty_status = true;
}

void restore_exec(void)
{
	if (!g_stopping)
		return;
	fflush(NULL);
	status_shutdown();
	ctl_shutdown();
	spawnd_shutdown();
	if (g_path[0] != '\0')
		setenv("NOMONAD_RESTORE", g_path, 1);
	fprintf(stderr, "nomonad: restarting %s\n", g_exe);
	execv(g_exe, g_argv);
	fprintf(stderr, "nomonad: restart: exec %s failed: %s\n", g_exe,
		strerror(errno));
	if (g_path[0] != '\0')
		(void)unlink(g_path);
	exit(1);
}
