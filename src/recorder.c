/* SPDX-License-Identifier: 0BSD */
/*
 * Session-trace recorder. See docs/session-format.md for the on-disk
 * format. Activated when NM_RECORD names a writable path; otherwise
 * recorder_init returns early and every recorder_event_* call is a
 * cheap NULL-check no-op.
 *
 * The recorder mints monotonic ids per object kind (window, output,
 * seat) on first sight, keyed by pointer, via a small open-addressed
 * tombstone table. Pointers can be reused after window_maybe_destroy
 * frees a window, so each *_freed hook removes the table entry; a
 * subsequent calloc that lands on the same address mints a fresh id.
 *
 * Output is stdio line-buffered: each event becomes one fputs that
 * ends with '\n', and the kernel handles writeback. No fsync. Per-event
 * syscall cost is ~200 ns; for nomonad's busiest workload (held A+enter,
 * ~50 events/sec) the overhead is below noise on the manage cycle.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "action.h"
#include "nomonad.h"
#include "recorder.h"

#define NM_RECORD_DEFAULT_MAX_BYTES (64ULL * 1024 * 1024)
#define NM_RECORD_INITIAL_CAP	    64
#define NM_RECORD_LOAD_NUM	    7
#define NM_RECORD_LOAD_DEN	    10

/*
 * Tombstone marker. Address 1 is misaligned and won't ever be returned
 * by a real allocator on Linux, so it cannot collide with a live key.
 */
#define NM_RECORD_TOMB ((void *)(uintptr_t)1)

enum titles_mode {
	TITLES_OFF,
	TITLES_HASH,
	TITLES_VERBATIM,
};

enum appid_mode {
	APPID_VERBATIM,
	APPID_HASH,
};

enum pointer_mode {
	POINTER_OUTPUT,
	POINTER_FULL,
};

struct PtrEntry {
	void *key;
	u32 id;
};

struct PtrTable {
	struct PtrEntry *entries;
	size_t cap;
	size_t size;
	size_t tomb;
	u32 next_id;
};

struct Recorder {
	FILE *out;
	struct timespec origin;
	struct PtrTable windows;
	struct PtrTable outputs;
	struct PtrTable seats;
	u64 max_bytes;
	bool capacity_warned;
	bool stopped;
	enum titles_mode titles;
	enum appid_mode appid;
	enum pointer_mode pointer;
};

static struct Recorder *g_rec;

/* ---- env helpers ----------------------------------------------------- */

static enum titles_mode parse_titles_mode(void)
{
	const char *v = getenv("NM_RECORD_TITLES");

	if (v == NULL || strcmp(v, "0") == 0)
		return TITLES_OFF;
	if (strcmp(v, "hash") == 0)
		return TITLES_HASH;
	if (strcmp(v, "1") == 0)
		return TITLES_VERBATIM;
	fprintf(stderr,
		"nomonad: NM_RECORD_TITLES must be 0/1/hash; got '%s'; using 0\n",
		v);
	return TITLES_OFF;
}

static enum appid_mode parse_appid_mode(void)
{
	const char *v = getenv("NM_RECORD_APPID");

	if (v == NULL || strcmp(v, "1") == 0)
		return APPID_VERBATIM;
	if (strcmp(v, "hash") == 0)
		return APPID_HASH;
	fprintf(stderr,
		"nomonad: NM_RECORD_APPID must be 1/hash; got '%s'; using 1\n",
		v);
	return APPID_VERBATIM;
}

static enum pointer_mode parse_pointer_mode(void)
{
	const char *v = getenv("NM_RECORD_POINTER");

	if (v == NULL || strcmp(v, "output") == 0)
		return POINTER_OUTPUT;
	if (strcmp(v, "full") == 0)
		return POINTER_FULL;
	fprintf(stderr,
		"nomonad: NM_RECORD_POINTER must be output/full; got '%s'; using output\n",
		v);
	return POINTER_OUTPUT;
}

static u64 parse_max_bytes(void)
{
	const char *v = getenv("NM_RECORD_MAX_BYTES");
	unsigned long long n;
	char *end;

	if (v == NULL || *v == '\0')
		return NM_RECORD_DEFAULT_MAX_BYTES;
	errno = 0;
	n = strtoull(v, &end, 10);
	if (errno != 0 || end == v || *end != '\0' || n == 0) {
		fprintf(stderr,
			"nomonad: NM_RECORD_MAX_BYTES must be a positive integer; got '%s'; using default\n",
			v);
		return NM_RECORD_DEFAULT_MAX_BYTES;
	}
	return (u64)n;
}

/* ---- pointer-keyed table -------------------------------------------- */

static size_t ptr_hash(void *p, size_t cap)
{
	uintptr_t x = (uintptr_t)p >> 4;
	return (size_t)((x * 2654435761ULL) & (cap - 1));
}

static void table_init(struct PtrTable *t)
{
	t->entries = calloc(NM_RECORD_INITIAL_CAP, sizeof(*t->entries));
	t->cap = (t->entries != NULL) ? NM_RECORD_INITIAL_CAP : 0;
	t->size = 0;
	t->tomb = 0;
	t->next_id = 1;
}

static void table_free(struct PtrTable *t)
{
	free(t->entries);
	t->entries = NULL;
	t->cap = 0;
	t->size = 0;
	t->tomb = 0;
}

static void table_grow(struct PtrTable *t)
{
	struct PtrEntry *old = t->entries;
	size_t old_cap = t->cap;
	size_t k;

	t->cap = (t->cap == 0) ? NM_RECORD_INITIAL_CAP : t->cap * 2;
	t->entries = calloc(t->cap, sizeof(*t->entries));
	if (t->entries == NULL) {
		t->entries = old;
		t->cap = old_cap;
		return;
	}
	t->size = 0;
	t->tomb = 0;
	for (k = 0; k < old_cap; k++) {
		size_t i;

		if (old[k].key == NULL || old[k].key == NM_RECORD_TOMB)
			continue;
		i = ptr_hash(old[k].key, t->cap);
		while (t->entries[i].key != NULL)
			i = (i + 1) & (t->cap - 1);
		t->entries[i] = old[k];
		t->size++;
	}
	free(old);
}

static u32 table_find_or_assign(struct PtrTable *t, void *key)
{
	size_t first_tomb;
	size_t i;

	if (key == NULL)
		return 0;
	if (t->entries == NULL)
		table_init(t);
	if (t->entries == NULL)
		return 0;
	if ((t->size + t->tomb) * NM_RECORD_LOAD_DEN >=
	    t->cap * NM_RECORD_LOAD_NUM)
		table_grow(t);
	if (t->size + t->tomb >= t->cap)
		return 0;
	i = ptr_hash(key, t->cap);
	first_tomb = (size_t)-1;
	while (t->entries[i].key != NULL) {
		if (t->entries[i].key == NM_RECORD_TOMB) {
			if (first_tomb == (size_t)-1)
				first_tomb = i;
		} else if (t->entries[i].key == key) {
			return t->entries[i].id;
		}
		i = (i + 1) & (t->cap - 1);
	}
	if (first_tomb != (size_t)-1) {
		i = first_tomb;
		t->tomb--;
	}
	t->entries[i].key = key;
	t->entries[i].id = t->next_id++;
	t->size++;
	return t->entries[i].id;
}

static u32 table_lookup(struct PtrTable *t, void *key)
{
	size_t i;

	if (key == NULL || t->entries == NULL)
		return 0;
	i = ptr_hash(key, t->cap);
	while (t->entries[i].key != NULL) {
		if (t->entries[i].key == key)
			return t->entries[i].id;
		i = (i + 1) & (t->cap - 1);
	}
	return 0;
}

static void table_remove(struct PtrTable *t, void *key)
{
	size_t i;

	if (key == NULL || t->entries == NULL)
		return;
	i = ptr_hash(key, t->cap);
	while (t->entries[i].key != NULL) {
		if (t->entries[i].key == key) {
			t->entries[i].key = NM_RECORD_TOMB;
			t->entries[i].id = 0;
			t->size--;
			t->tomb++;
			return;
		}
		i = (i + 1) & (t->cap - 1);
	}
}

/* ---- emit helpers --------------------------------------------------- */

static long long recorder_now_ns(void)
{
	struct timespec ts;
	long long delta_ns;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	delta_ns = (long long)(ts.tv_sec - g_rec->origin.tv_sec) *
			   1000000000LL +
		   (long long)(ts.tv_nsec - g_rec->origin.tv_nsec);
	return delta_ns;
}

static void recorder_check_capacity(void)
{
	long pos;

	if (g_rec->stopped || g_rec->capacity_warned)
		return;
	pos = ftell(g_rec->out);
	if (pos < 0 || (u64)pos < g_rec->max_bytes)
		return;
	g_rec->capacity_warned = true;
	g_rec->stopped = true;
	fprintf(stderr,
		"nomonad: recorder hit %llu byte cap; recording stopped\n",
		(unsigned long long)g_rec->max_bytes);
	fflush(g_rec->out);
}

static void recorder_putq(const char *s)
{
	fputc('"', g_rec->out);
	if (s != NULL) {
		const char *p;

		for (p = s; *p != '\0'; p++) {
			unsigned char c = (unsigned char)*p;

			if (c == '\\') {
				fputs("\\\\", g_rec->out);
			} else if (c == '"') {
				fputs("\\\"", g_rec->out);
			} else if (c == '\n') {
				fputs("\\n", g_rec->out);
			} else if (c == '\t') {
				fputs("\\t", g_rec->out);
			} else if (c >= 0x20 && c < 0x7F) {
				fputc((int)c, g_rec->out);
			} else {
				fprintf(g_rec->out, "\\x%02x", (unsigned)c);
			}
		}
	}
	fputc('"', g_rec->out);
}

static u64 fnv1a64(const char *s)
{
	u64 h = 0xcbf29ce484222325ULL;
	const char *p;

	if (s == NULL)
		return h;
	for (p = s; *p != '\0'; p++) {
		h ^= (u64)(unsigned char)*p;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static void recorder_put_string_field(const char *name, const char *s,
				      bool hash)
{
	if (s == NULL) {
		fprintf(g_rec->out, "%s=none", name);
		return;
	}
	fprintf(g_rec->out, "%s=", name);
	if (hash)
		fprintf(g_rec->out, "\"%016llx\"",
			(unsigned long long)fnv1a64(s));
	else
		recorder_putq(s);
}

static void recorder_begin(const char *kind)
{
	fprintf(g_rec->out, "T %lld %s", recorder_now_ns(), kind);
}

static void recorder_end_line(void)
{
	fputc('\n', g_rec->out);
	recorder_check_capacity();
}

/* ---- id helpers ----------------------------------------------------- */

static u32 window_id(struct Window *w)
{
	if (g_rec == NULL || w == NULL)
		return 0;
	return table_find_or_assign(&g_rec->windows, w);
}

static u32 window_id_lookup(struct Window *w)
{
	if (g_rec == NULL || w == NULL)
		return 0;
	return table_lookup(&g_rec->windows, w);
}

static u32 output_id(struct Output *o)
{
	if (g_rec == NULL || o == NULL)
		return 0;
	return table_find_or_assign(&g_rec->outputs, o);
}

static u32 seat_id(struct Seat *s)
{
	if (g_rec == NULL || s == NULL)
		return 0;
	return table_find_or_assign(&g_rec->seats, s);
}

/* ---- lifecycle ------------------------------------------------------ */

static void recorder_emit_header(void)
{
	time_t now = time(NULL);

	fputs("nomonad-trace v=1\n", g_rec->out);
	fprintf(g_rec->out, "T 0 start unix=%lld\n", (long long)now);
}

/*
 * Best-effort `mkdir -p` for the parent directory of `path`. Mistyped
 * paths like NM_RECORD=$UNSET_VAR/foo expand to /foo, which fopen
 * cannot create; make the common "I forgot to mkdir" case work.
 * Anything we cannot create silently falls through to the fopen error
 * with a clear stderr line.
 */
static void recorder_make_parent(const char *path)
{
	const char *slash;
	char buf[4096];
	size_t i;

	slash = strrchr(path, '/');
	if (slash == NULL || slash == path)
		return;
	if ((size_t)(slash - path) >= sizeof(buf))
		return;
	memcpy(buf, path, (size_t)(slash - path));
	buf[slash - path] = '\0';
	for (i = 1; buf[i] != '\0'; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0700) != 0 && errno != EEXIST)
			return;
		buf[i] = '/';
	}
	mkdir(buf, 0700);
}

static bool recorder_lock(const char *path)
{
	struct flock fl = { 0 };
	char lock[4096];
	int fd;

	if (snprintf(lock, sizeof(lock), "%s.lock", path) >= (int)sizeof(lock))
		return false;
	fd = open(lock, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	if (fcntl(fd, F_SETLK, &fl) != 0) {
		fprintf(stderr, "nomonad: %s is in use by another instance\n",
			path);
		close(fd);
		return false;
	}
	return true;
}

FILE *recorder_open_private(const char *path)
{
	char old[4096];
	FILE *f;
	int fd;

	if (!recorder_lock(path))
		return NULL;
	if (snprintf(old, sizeof(old), "%s.1", path) < (int)sizeof(old))
		(void)rename(path, old);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return NULL;
	f = fdopen(fd, "w");
	if (f == NULL)
		close(fd);
	return f;
}

int recorder_init(void)
{
	const char *path = getenv("NM_RECORD");
	struct Recorder *r;

	if (path == NULL || *path == '\0')
		return 0;
	r = calloc(1, sizeof(*r));
	if (r == NULL)
		return -1;
	recorder_make_parent(path);
	r->out = recorder_open_private(path);
	if (r->out == NULL) {
		fprintf(stderr,
			"nomonad: NM_RECORD='%s': %s; recording disabled\n",
			path, strerror(errno));
		free(r);
		return 0;
	}
	setvbuf(r->out, NULL, _IOLBF, 0);
	fprintf(stderr, "nomonad: recording session trace to %s\n", path);
	clock_gettime(CLOCK_MONOTONIC, &r->origin);
	r->max_bytes = parse_max_bytes();
	r->titles = parse_titles_mode();
	r->appid = parse_appid_mode();
	r->pointer = parse_pointer_mode();
	g_rec = r;
	recorder_emit_header();
	atexit(recorder_shutdown);
	return 0;
}

void recorder_shutdown(void)
{
	if (g_rec == NULL)
		return;
	if (!g_rec->stopped) {
		fprintf(g_rec->out, "T %lld end\n", recorder_now_ns());
	}
	fflush(g_rec->out);
	fclose(g_rec->out);
	table_free(&g_rec->windows);
	table_free(&g_rec->outputs);
	table_free(&g_rec->seats);
	free(g_rec);
	g_rec = NULL;
}

/* ---- meta events ---------------------------------------------------- */

static void recorder_emit_blob(const char *kind, const char *body)
{
	size_t len = (body != NULL) ? strlen(body) : 0;

	if (g_rec == NULL || g_rec->stopped)
		return;
	fprintf(g_rec->out, "T %lld %s bytes=%zu\n", recorder_now_ns(), kind,
		len);
	if (len > 0)
		fwrite(body, 1, len, g_rec->out);
	fputc('\n', g_rec->out);
	recorder_check_capacity();
}

void recorder_event_config_set(const char *text)
{
	recorder_emit_blob("config_set", text);
}

void recorder_event_config_reload(const char *text)
{
	recorder_emit_blob("config_reload", text);
}

/* ---- output events -------------------------------------------------- */

void recorder_event_output_add(struct Output *output)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || output == NULL)
		return;
	id = output_id(output);
	recorder_begin("output_add");
	fprintf(g_rec->out, " id=%u x=%d y=%d w=%d h=%d", id, (int)output->x,
		(int)output->y, (int)output->width, (int)output->height);
	recorder_end_line();
}

void recorder_event_output_position(struct Output *output)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || output == NULL)
		return;
	id = output_id(output);
	recorder_begin("output_position");
	fprintf(g_rec->out, " id=%u x=%d y=%d", id, (int)output->x,
		(int)output->y);
	recorder_end_line();
}

void recorder_event_output_dimensions(struct Output *output)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || output == NULL)
		return;
	id = output_id(output);
	recorder_begin("output_dimensions");
	fprintf(g_rec->out, " id=%u w=%d h=%d", id, (int)output->width,
		(int)output->height);
	recorder_end_line();
}

void recorder_event_output_remove(struct Output *output)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || output == NULL)
		return;
	id = output_id(output);
	recorder_begin("output_remove");
	fprintf(g_rec->out, " id=%u", id);
	recorder_end_line();
}

void recorder_output_freed(struct Output *output)
{
	if (g_rec == NULL || output == NULL)
		return;
	table_remove(&g_rec->outputs, output);
}

/* ---- seat events ---------------------------------------------------- */

void recorder_event_seat_add(struct Seat *seat)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	id = seat_id(seat);
	recorder_begin("seat_add");
	fprintf(g_rec->out, " id=%u", id);
	recorder_end_line();
}

void recorder_event_output_usable(struct Output *output)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || output == NULL)
		return;
	id = output_id(output);
	recorder_begin("output_usable");
	fprintf(g_rec->out, " id=%u x=%d y=%d w=%d h=%d", id,
		(int)output->usable_x, (int)output->usable_y,
		(int)output->usable_width, (int)output->usable_height);
	recorder_end_line();
}

void recorder_event_layer_focus(struct Seat *seat)
{
	static const char *const names[] = {
		[LAYER_FOCUS_NONE] = "none",
		[LAYER_FOCUS_EXCLUSIVE] = "exclusive",
		[LAYER_FOCUS_NON_EXCLUSIVE] = "non_exclusive",
	};
	u32 id;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	id = seat_id(seat);
	recorder_begin("layer_focus");
	fprintf(g_rec->out, " id=%u focus=%s", id, names[seat->layer_focus]);
	recorder_end_line();
}

void recorder_event_seat_remove(struct Seat *seat)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	id = seat_id(seat);
	recorder_begin("seat_remove");
	fprintf(g_rec->out, " id=%u", id);
	recorder_end_line();
}

void recorder_seat_freed(struct Seat *seat)
{
	if (g_rec == NULL || seat == NULL)
		return;
	table_remove(&g_rec->seats, seat);
}

/* ---- window events -------------------------------------------------- */

void recorder_event_window_add(struct Window *window)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || window == NULL)
		return;
	id = window_id(window);
	recorder_begin("window_add");
	fprintf(g_rec->out, " id=%u", id);
	recorder_end_line();
}

void recorder_event_window_app_id(struct Window *window)
{
	bool hash;
	u32 id;

	if (g_rec == NULL || g_rec->stopped || window == NULL)
		return;
	id = window_id(window);
	hash = (g_rec->appid == APPID_HASH);
	recorder_begin("window_app_id");
	fprintf(g_rec->out, " id=%u ", id);
	recorder_put_string_field("app_id", window->app_id, hash);
	recorder_end_line();
}

void recorder_event_window_title(struct Window *window)
{
	u32 id;
	bool hash;

	if (g_rec == NULL || g_rec->stopped || window == NULL)
		return;
	if (g_rec->titles == TITLES_OFF)
		return;
	hash = (g_rec->titles == TITLES_HASH);
	id = window_id(window);
	recorder_begin("window_title");
	fprintf(g_rec->out, " id=%u ", id);
	recorder_put_string_field("title", window->title, hash);
	recorder_end_line();
}

void recorder_event_window_parent(struct Window *window, struct Window *parent)
{
	u32 pid;
	u32 id;

	if (g_rec == NULL || g_rec->stopped || window == NULL)
		return;
	id = window_id(window);
	pid = (parent != NULL) ? window_id(parent) : 0;
	recorder_begin("window_parent");
	if (pid != 0)
		fprintf(g_rec->out, " id=%u parent=%u", id, pid);
	else
		fprintf(g_rec->out, " id=%u parent=none", id);
	recorder_end_line();
}

void recorder_event_window_dimensions_hint(struct Window *window)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || window == NULL)
		return;
	id = window_id(window);
	recorder_begin("window_dimensions_hint");
	fprintf(g_rec->out, " id=%u min_w=%d min_h=%d max_w=%d max_h=%d", id,
		(int)window->min_width, (int)window->min_height,
		(int)window->max_width, (int)window->max_height);
	recorder_end_line();
}

void recorder_event_window_fullscreen(struct Window *window, bool on)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || window == NULL)
		return;
	id = window_id(window);
	recorder_begin("window_fullscreen");
	fprintf(g_rec->out, " id=%u on=%d", id, on ? 1 : 0);
	recorder_end_line();
}

void recorder_event_window_close(struct Window *window)
{
	u32 id;

	if (g_rec == NULL || g_rec->stopped || window == NULL)
		return;
	id = window_id_lookup(window);
	if (id == 0)
		return;
	recorder_begin("window_close");
	fprintf(g_rec->out, " id=%u", id);
	recorder_end_line();
}

void recorder_window_freed(struct Window *window)
{
	if (g_rec == NULL || window == NULL)
		return;
	table_remove(&g_rec->windows, window);
}

/* ---- pointer / seat runtime events ---------------------------------- */

void recorder_event_pointer_to(struct Seat *seat, struct Output *output)
{
	u32 sid;
	u32 oid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	oid = output_id(output);
	recorder_begin("pointer_to");
	fprintf(g_rec->out, " seat=%u output=%u", sid, oid);
	recorder_end_line();
}

void recorder_event_pointer_position(struct Seat *seat, s32 x, s32 y)
{
	u32 sid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	if (g_rec->pointer != POINTER_FULL)
		return;
	sid = seat_id(seat);
	recorder_begin("pointer_position");
	fprintf(g_rec->out, " seat=%u x=%d y=%d", sid, (int)x, (int)y);
	recorder_end_line();
}

void recorder_event_pointer_enter(struct Seat *seat, struct Window *window)
{
	u32 sid;
	u32 wid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	wid = window_id(window);
	recorder_begin("pointer_enter");
	fprintf(g_rec->out, " seat=%u window=%u", sid, wid);
	recorder_end_line();
}

void recorder_event_pointer_leave(struct Seat *seat)
{
	u32 sid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	recorder_begin("pointer_leave");
	fprintf(g_rec->out, " seat=%u", sid);
	recorder_end_line();
}

void recorder_event_op_delta(struct Seat *seat, s32 dx, s32 dy)
{
	u32 sid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	recorder_begin("op_delta");
	fprintf(g_rec->out, " seat=%u dx=%d dy=%d", sid, (int)dx, (int)dy);
	recorder_end_line();
}

void recorder_event_op_release(struct Seat *seat)
{
	u32 sid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	recorder_begin("op_release");
	fprintf(g_rec->out, " seat=%u", sid);
	recorder_end_line();
}

void recorder_event_window_interaction(struct Seat *seat, struct Window *window)
{
	u32 sid;
	u32 wid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	wid = window_id(window);
	recorder_begin("window_interaction");
	fprintf(g_rec->out, " seat=%u window=%u", sid, wid);
	recorder_end_line();
}

void recorder_event_ate_unbound(struct Seat *seat)
{
	u32 sid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	recorder_begin("ate_unbound");
	fprintf(g_rec->out, " seat=%u", sid);
	recorder_end_line();
}

/* ---- bindings / actions --------------------------------------------- */

static void recorder_emit_press(const char *kind, struct Seat *seat,
				const struct Action *action)
{
	u32 sid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL || action == NULL)
		return;
	sid = seat_id(seat);
	recorder_begin(kind);
	fprintf(g_rec->out, " seat=%u kind=%s ", sid,
		action_kind_name(action->kind));
	recorder_put_string_field("arg", action->arg, false);
	recorder_end_line();
}

void recorder_event_press(struct Seat *seat, const struct Action *action)
{
	recorder_emit_press("press", seat, action);
}

void recorder_event_pointer_press(struct Seat *seat,
				  const struct Action *action)
{
	recorder_emit_press("pointer_press", seat, action);
}

void recorder_event_repeat_press(struct Seat *seat, const struct Action *action)
{
	recorder_emit_press("repeat_press", seat, action);
}

void recorder_event_stop_repeat(struct Seat *seat)
{
	u32 sid;

	if (g_rec == NULL || g_rec->stopped || seat == NULL)
		return;
	sid = seat_id(seat);
	recorder_begin("stop_repeat");
	fprintf(g_rec->out, " seat=%u", sid);
	recorder_end_line();
}

/* ---- session lock --------------------------------------------------- */

void recorder_event_session_locked(void)
{
	if (g_rec == NULL || g_rec->stopped)
		return;
	recorder_begin("session_locked");
	recorder_end_line();
}

void recorder_event_session_unlocked(void)
{
	if (g_rec == NULL || g_rec->stopped)
		return;
	recorder_begin("session_unlocked");
	recorder_end_line();
}
