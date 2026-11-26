/* SPDX-License-Identifier: 0BSD */
/*
 * Session-trace replayer. Reads a trace produced by src/recorder.c
 * (see docs/session-format.md), drives the headless harness through
 * each event, and prints per-event-kind cycle latency stats at exit.
 *
 * Usage:
 *   ./test/harness/replay [--realtime] PATH
 *
 *   --realtime   sleep on T-deltas so replay tracks the recorded
 *                wall-clock cadence; default is run-as-fast-as-possible.
 *   --no-stats   suppress the per-event-kind summary at exit.
 *   --no-invariants  pass false to harness_set_invariants for perf
 *                    measurements that exclude the O(n^2) checks.
 *
 * The replayer reaches into the production listener structs to apply
 * metadata events (window_app_id, window_title, ...) that harness_lib
 * does not surface as helpers. That is safe because the harness side
 * is the consumer of those listeners anyway, and replay is a test
 * binary.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>

#include "action.h"
#include "harness_lib.h"
#include "nomonad.h"

extern const struct river_window_v1_listener river_window_listener;
extern const struct river_output_v1_listener river_output_listener;
extern const struct river_seat_v1_listener river_seat_listener;
extern const struct river_xkb_bindings_seat_v1_listener
	river_xkb_bindings_seat_listener;

#define NM_REPLAY_INITIAL_TABLE 64
#define NM_REPLAY_LINE_CAP	4096

/* ---- id table ------------------------------------------------------- */

struct id_table {
	void **slots;
	size_t cap;
};

static void table_init(struct id_table *t)
{
	t->cap = NM_REPLAY_INITIAL_TABLE;
	t->slots = calloc(t->cap, sizeof(void *));
	if (t->slots == NULL)
		abort();
}

static void table_free(struct id_table *t)
{
	free(t->slots);
	t->slots = NULL;
	t->cap = 0;
}

static void table_set(struct id_table *t, uint32_t id, void *v)
{
	if (id == 0)
		return;
	if ((size_t)id >= t->cap) {
		size_t new_cap = t->cap;

		while ((size_t)id >= new_cap)
			new_cap *= 2;
		t->slots = realloc(t->slots, new_cap * sizeof(void *));
		if (t->slots == NULL)
			abort();
		memset(t->slots + t->cap, 0,
		       (new_cap - t->cap) * sizeof(void *));
		t->cap = new_cap;
	}
	t->slots[id] = v;
}

static void *table_get(struct id_table *t, uint32_t id)
{
	if (id == 0 || (size_t)id >= t->cap)
		return NULL;
	return t->slots[id];
}

static void table_clear(struct id_table *t, uint32_t id)
{
	if (id == 0 || (size_t)id >= t->cap)
		return;
	t->slots[id] = NULL;
}

/* ---- event-kind enum + stats --------------------------------------- */

enum event_kind {
	EK_START,
	EK_END,
	EK_CONFIG_SET,
	EK_CONFIG_RELOAD,
	EK_OUTPUT_ADD,
	EK_OUTPUT_POSITION,
	EK_OUTPUT_DIMENSIONS,
	EK_OUTPUT_REMOVE,
	EK_OUTPUT_USABLE,
	EK_LAYER_FOCUS,
	EK_SEAT_ADD,
	EK_SEAT_REMOVE,
	EK_WINDOW_ADD,
	EK_WINDOW_APP_ID,
	EK_WINDOW_TITLE,
	EK_WINDOW_PARENT,
	EK_WINDOW_DIMENSIONS_HINT,
	EK_WINDOW_FULLSCREEN,
	EK_WINDOW_CLOSE,
	EK_POINTER_TO,
	EK_POINTER_POSITION,
	EK_POINTER_ENTER,
	EK_POINTER_LEAVE,
	EK_OP_DELTA,
	EK_OP_RELEASE,
	EK_WINDOW_INTERACTION,
	EK_ATE_UNBOUND,
	EK_PRESS,
	EK_POINTER_PRESS,
	EK_REPEAT_PRESS,
	EK_STOP_REPEAT,
	EK_SESSION_LOCKED,
	EK_SESSION_UNLOCKED,
	EK_UNKNOWN,
	EK__COUNT,
};

static const char *const event_name[EK__COUNT] = {
	[EK_START] = "start",
	[EK_END] = "end",
	[EK_CONFIG_SET] = "config_set",
	[EK_CONFIG_RELOAD] = "config_reload",
	[EK_OUTPUT_ADD] = "output_add",
	[EK_OUTPUT_POSITION] = "output_position",
	[EK_OUTPUT_DIMENSIONS] = "output_dimensions",
	[EK_OUTPUT_REMOVE] = "output_remove",
	[EK_OUTPUT_USABLE] = "output_usable",
	[EK_LAYER_FOCUS] = "layer_focus",
	[EK_SEAT_ADD] = "seat_add",
	[EK_SEAT_REMOVE] = "seat_remove",
	[EK_WINDOW_ADD] = "window_add",
	[EK_WINDOW_APP_ID] = "window_app_id",
	[EK_WINDOW_TITLE] = "window_title",
	[EK_WINDOW_PARENT] = "window_parent",
	[EK_WINDOW_DIMENSIONS_HINT] = "window_dimensions_hint",
	[EK_WINDOW_FULLSCREEN] = "window_fullscreen",
	[EK_WINDOW_CLOSE] = "window_close",
	[EK_POINTER_TO] = "pointer_to",
	[EK_POINTER_POSITION] = "pointer_position",
	[EK_POINTER_ENTER] = "pointer_enter",
	[EK_POINTER_LEAVE] = "pointer_leave",
	[EK_OP_DELTA] = "op_delta",
	[EK_OP_RELEASE] = "op_release",
	[EK_WINDOW_INTERACTION] = "window_interaction",
	[EK_ATE_UNBOUND] = "ate_unbound",
	[EK_PRESS] = "press",
	[EK_POINTER_PRESS] = "pointer_press",
	[EK_REPEAT_PRESS] = "repeat_press",
	[EK_STOP_REPEAT] = "stop_repeat",
	[EK_SESSION_LOCKED] = "session_locked",
	[EK_SESSION_UNLOCKED] = "session_unlocked",
	[EK_UNKNOWN] = "unknown",
};

static enum event_kind kind_from_name(const char *s)
{
	size_t i;

	for (i = 0; i < EK__COUNT; i++) {
		if (event_name[i] != NULL && strcmp(s, event_name[i]) == 0)
			return (enum event_kind)i;
	}
	return EK_UNKNOWN;
}

struct kind_stats {
	uint64_t count;
	uint64_t total_ns;
	uint64_t min_ns;
	uint64_t max_ns;
	uint64_t *samples;
	size_t cap;
};

static struct kind_stats g_stats[EK__COUNT];

static uint64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void stats_record(enum event_kind k, uint64_t ns)
{
	struct kind_stats *s = &g_stats[k];

	if (s->count == s->cap) {
		size_t new_cap = (s->cap == 0) ? 64 : s->cap * 2;

		s->samples = realloc(s->samples, new_cap * sizeof(uint64_t));
		if (s->samples == NULL)
			abort();
		s->cap = new_cap;
	}
	s->samples[s->count] = ns;
	if (s->count == 0 || ns < s->min_ns)
		s->min_ns = ns;
	if (ns > s->max_ns)
		s->max_ns = ns;
	s->count++;
	s->total_ns += ns;
}

static int u64_cmp(const void *a, const void *b)
{
	uint64_t aa = *(const uint64_t *)a;
	uint64_t bb = *(const uint64_t *)b;

	if (aa < bb)
		return -1;
	return (aa > bb) ? 1 : 0;
}

static void stats_print(FILE *out)
{
	size_t i;

	fputs("\n=== per-event-kind cycle latency (us) ===\n", out);
	fprintf(out,
		"%-26s %8s %8s %8s %8s %8s %8s\n",
		"event", "count", "mean", "p50", "p95", "p99", "max");
	for (i = 0; i < EK__COUNT; i++) {
		struct kind_stats *s = &g_stats[i];
		uint64_t mean;
		uint64_t p50;
		uint64_t p95;
		uint64_t p99;

		if (s->count == 0)
			continue;
		qsort(s->samples, s->count, sizeof(uint64_t), u64_cmp);
		mean = s->total_ns / s->count;
		p50 = s->samples[s->count / 2];
		p95 = s->samples[(s->count * 95) / 100];
		p99 = s->samples[(s->count * 99) / 100];
		fprintf(out,
			"%-26s %8llu %8llu %8llu %8llu %8llu %8llu\n",
			event_name[i], (unsigned long long)s->count,
			(unsigned long long)(mean / 1000),
			(unsigned long long)(p50 / 1000),
			(unsigned long long)(p95 / 1000),
			(unsigned long long)(p99 / 1000),
			(unsigned long long)(s->max_ns / 1000));
	}
}

static void stats_free(void)
{
	size_t i;

	for (i = 0; i < EK__COUNT; i++)
		free(g_stats[i].samples);
}

/* ---- field parsing -------------------------------------------------- */

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/*
 * Parse an unsigned decimal integer from p, advance *out_end past it.
 * Returns true on success, false if no digits were consumed.
 */
static bool parse_u32(const char *p, uint32_t *out, const char **out_end)
{
	unsigned long n;
	char *end;

	errno = 0;
	n = strtoul(p, &end, 10);
	if (end == p || errno != 0 || n > UINT32_MAX)
		return false;
	*out = (uint32_t)n;
	if (out_end != NULL)
		*out_end = end;
	return true;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

/*
 * Parse a quoted string starting at p[0] == '"'. Writes bytes into
 * `out` (always NUL-terminated, capped to out_cap-1 bytes), advances
 * *out_end past the closing quote. Returns true on success.
 */
static bool parse_quoted(const char *p, char *out, size_t out_cap,
			 const char **out_end)
{
	size_t w = 0;

	if (out_cap == 0 || *p != '"')
		return false;
	p++;
	while (*p != '\0' && *p != '"') {
		int c;

		if (*p == '\\') {
			p++;
			switch (*p) {
			case '\\':
				c = '\\';
				p++;
				break;
			case '"':
				c = '"';
				p++;
				break;
			case 'n':
				c = '\n';
				p++;
				break;
			case 't':
				c = '\t';
				p++;
				break;
			case 'x': {
				int hi;
				int lo;

				p++;
				hi = hexval((unsigned char)p[0]);
				lo = hi >= 0 ? hexval((unsigned char)p[1]) : -1;
				if (hi < 0 || lo < 0)
					return false;
				c = (hi << 4) | lo;
				p += 2;
				break;
			}
			default:
				return false;
			}
		} else {
			c = (unsigned char)*p;
			p++;
		}
		if (w + 1 < out_cap)
			out[w++] = (char)c;
	}
	if (*p != '"')
		return false;
	out[w] = '\0';
	if (out_end != NULL)
		*out_end = p + 1;
	return true;
}

/*
 * Find a "key=" token in the rest of the line and return a pointer to
 * the byte right after the '='. Returns NULL if not found. Matches on
 * whitespace boundaries only - "x=" inside a quoted string is skipped.
 */
static const char *find_field(const char *p, const char *key)
{
	size_t key_len = strlen(key);
	bool in_string = false;
	bool at_token_start = true;

	while (*p != '\0') {
		if (in_string) {
			if (*p == '\\' && p[1] != '\0') {
				p += 2;
				continue;
			}
			if (*p == '"')
				in_string = false;
			at_token_start = false;
			p++;
			continue;
		}
		if (*p == '"') {
			in_string = true;
			at_token_start = false;
			p++;
			continue;
		}
		if (at_token_start && strncmp(p, key, key_len) == 0 &&
		    p[key_len] == '=')
			return p + key_len + 1;
		at_token_start = (*p == ' ' || *p == '\t');
		p++;
	}
	return NULL;
}

/* ---- replay state --------------------------------------------------- */

struct ctx {
	struct id_table windows;
	struct id_table outputs;
	struct id_table seats;
	bool init_done;
	bool stats_enabled;
	bool realtime;
	bool invariants;
	struct timespec replay_origin;
	uint64_t event_count;
};

static struct Seat *first_seat(struct ctx *c)
{
	size_t i;

	for (i = 1; i < c->seats.cap; i++) {
		if (c->seats.slots[i] != NULL)
			return c->seats.slots[i];
	}
	return NULL;
}

static struct Output *first_output(struct ctx *c)
{
	size_t i;

	for (i = 1; i < c->outputs.cap; i++) {
		if (c->outputs.slots[i] != NULL)
			return c->outputs.slots[i];
	}
	return NULL;
}

static void ensure_init(struct ctx *c, const char *config_text)
{
	if (c->init_done)
		return;
	harness_init((config_text != NULL && *config_text != '\0') ?
			     config_text : NULL);
	harness_set_invariants(c->invariants);
	c->init_done = true;
	clock_gettime(CLOCK_MONOTONIC, &c->replay_origin);
}

static void wait_until_realtime(struct ctx *c, uint64_t event_ns)
{
	struct timespec target;
	long sec;
	long nsec;

	if (!c->realtime)
		return;
	sec = (long)(event_ns / 1000000000ULL);
	nsec = (long)(event_ns % 1000000000ULL);
	target.tv_sec = c->replay_origin.tv_sec + sec;
	target.tv_nsec = c->replay_origin.tv_nsec + nsec;
	if (target.tv_nsec >= 1000000000L) {
		target.tv_sec++;
		target.tv_nsec -= 1000000000L;
	}
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
}

/* ---- per-kind handlers ---------------------------------------------- */

static void apply_output_add(struct ctx *c, const char *rest)
{
	struct Output *o;
	uint32_t id;
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;

	if (sscanf(rest, " id=%u x=%d y=%d w=%d h=%d", &id, &x, &y, &w, &h) !=
	    5)
		return;
	o = harness_add_output((int)x, (int)y, (int)w, (int)h);
	table_set(&c->outputs, id, o);
}

static void apply_output_position(struct ctx *c, const char *rest)
{
	struct Output *o;
	uint32_t id;
	int32_t x;
	int32_t y;

	if (sscanf(rest, " id=%u x=%d y=%d", &id, &x, &y) != 3)
		return;
	o = table_get(&c->outputs, id);
	if (o == NULL)
		return;
	river_output_listener.position(o, NULL, x, y);
}

static void apply_output_dimensions(struct ctx *c, const char *rest)
{
	struct Output *o;
	uint32_t id;
	int32_t w;
	int32_t h;

	if (sscanf(rest, " id=%u w=%d h=%d", &id, &w, &h) != 3)
		return;
	o = table_get(&c->outputs, id);
	if (o == NULL)
		return;
	river_output_listener.dimensions(o, NULL, w, h);
}

static void replay_attach_layer_shell(void)
{
	if (layer_shell_v1 == NULL) {
		harness_enable_layer_shell();
		harness_cycle();
	}
}

static void apply_output_usable(struct ctx *c, const char *rest)
{
	struct Output *o;
	uint32_t id;
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;

	if (sscanf(rest, " id=%u x=%d y=%d w=%d h=%d", &id, &x, &y, &w, &h) !=
	    5)
		return;
	o = table_get(&c->outputs, id);
	if (o == NULL)
		return;
	replay_attach_layer_shell();
	if (o->ls_output == NULL)
		harness_cycle();
	if (o->ls_output != NULL)
		harness_set_usable(o, x, y, w, h);
}

static void apply_layer_focus(struct ctx *c, const char *rest)
{
	enum LayerFocus focus;
	char name[32];
	struct Seat *s;
	uint32_t id;

	if (sscanf(rest, " id=%u focus=%31s", &id, name) != 2)
		return;
	s = table_get(&c->seats, id);
	if (s == NULL)
		return;
	if (strcmp(name, "exclusive") == 0)
		focus = LAYER_FOCUS_EXCLUSIVE;
	else if (strcmp(name, "non_exclusive") == 0)
		focus = LAYER_FOCUS_NON_EXCLUSIVE;
	else
		focus = LAYER_FOCUS_NONE;
	replay_attach_layer_shell();
	if (s->ls_seat == NULL)
		harness_cycle();
	if (s->ls_seat != NULL)
		harness_layer_focus(s, focus);
}

static void apply_output_remove(struct ctx *c, const char *rest)
{
	struct Output *o;
	uint32_t id;

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	o = table_get(&c->outputs, id);
	if (o == NULL)
		return;
	harness_remove_output(o);
	table_clear(&c->outputs, id);
}

static void apply_seat_add(struct ctx *c, const char *rest)
{
	struct Seat *s;
	uint32_t id;

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	s = harness_add_seat(first_output(c));
	table_set(&c->seats, id, s);
}

static void apply_seat_remove(struct ctx *c, const char *rest)
{
	struct Seat *s;
	uint32_t id;

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	s = table_get(&c->seats, id);
	if (s == NULL)
		return;
	river_seat_listener.removed(s, NULL);
	table_clear(&c->seats, id);
}

static void apply_window_add(struct ctx *c, const char *rest)
{
	struct Window *w;
	uint32_t id;

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	w = harness_add_window(NULL);
	table_set(&c->windows, id, w);
}

static void apply_window_app_id(struct ctx *c, const char *rest)
{
	struct Window *w;
	const char *fp;
	uint32_t id;
	char buf[1024];

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	w = table_get(&c->windows, id);
	if (w == NULL)
		return;
	fp = find_field(rest, "app_id");
	if (fp == NULL)
		return;
	if (strncmp(fp, "none", 4) == 0) {
		river_window_listener.app_id(w, NULL, NULL);
		return;
	}
	if (parse_quoted(fp, buf, sizeof(buf), NULL))
		river_window_listener.app_id(w, NULL, buf);
}

static void apply_window_title(struct ctx *c, const char *rest)
{
	struct Window *w;
	const char *fp;
	uint32_t id;
	char buf[1024];

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	w = table_get(&c->windows, id);
	if (w == NULL)
		return;
	fp = find_field(rest, "title");
	if (fp == NULL)
		return;
	if (strncmp(fp, "none", 4) == 0) {
		river_window_listener.title(w, NULL, NULL);
		return;
	}
	if (parse_quoted(fp, buf, sizeof(buf), NULL))
		river_window_listener.title(w, NULL, buf);
}

static void apply_window_parent(struct ctx *c, const char *rest)
{
	struct river_window_v1 *parent_obj;
	struct Window *parent;
	struct Window *w;
	const char *fp;
	uint32_t id;
	uint32_t pid;

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	w = table_get(&c->windows, id);
	if (w == NULL)
		return;
	fp = find_field(rest, "parent");
	if (fp == NULL)
		return;
	if (strncmp(fp, "none", 4) == 0) {
		river_window_listener.parent(w, NULL, NULL);
		return;
	}
	if (!parse_u32(fp, &pid, NULL))
		return;
	parent = table_get(&c->windows, pid);
	parent_obj = (parent != NULL) ? parent->obj : NULL;
	river_window_listener.parent(w, NULL, parent_obj);
}

static void apply_window_dimensions_hint(struct ctx *c, const char *rest)
{
	struct Window *w;
	uint32_t id;
	int32_t mw;
	int32_t mh;
	int32_t Mw;
	int32_t Mh;

	if (sscanf(rest, " id=%u min_w=%d min_h=%d max_w=%d max_h=%d", &id, &mw,
		   &mh, &Mw, &Mh) != 5)
		return;
	w = table_get(&c->windows, id);
	if (w == NULL)
		return;
	river_window_listener.dimensions_hint(w, NULL, mw, mh, Mw, Mh);
}

static void apply_window_fullscreen(struct ctx *c, const char *rest)
{
	struct Window *w;
	uint32_t id;
	int on;

	if (sscanf(rest, " id=%u on=%d", &id, &on) != 2)
		return;
	w = table_get(&c->windows, id);
	if (w == NULL)
		return;
	if (on)
		river_window_listener.fullscreen_requested(w, NULL, NULL);
	else
		river_window_listener.exit_fullscreen_requested(w, NULL);
}

static void apply_window_close(struct ctx *c, const char *rest)
{
	struct Window *w;
	uint32_t id;

	if (sscanf(rest, " id=%u", &id) != 1)
		return;
	w = table_get(&c->windows, id);
	if (w == NULL)
		return;
	harness_close_window(w);
	table_clear(&c->windows, id);
}

static void apply_pointer_to(struct ctx *c, const char *rest)
{
	struct Output *o;
	struct Seat *s;
	uint32_t sid;
	uint32_t oid;

	if (sscanf(rest, " seat=%u output=%u", &sid, &oid) != 2)
		return;
	s = table_get(&c->seats, sid);
	o = table_get(&c->outputs, oid);
	if (s == NULL || o == NULL)
		return;
	harness_pointer_to(s, o);
}

static void apply_pointer_position(struct ctx *c, const char *rest)
{
	struct Seat *s;
	uint32_t sid;
	int32_t x;
	int32_t y;

	if (sscanf(rest, " seat=%u x=%d y=%d", &sid, &x, &y) != 3)
		return;
	s = table_get(&c->seats, sid);
	if (s == NULL)
		return;
	river_seat_listener.pointer_position(s, NULL, x, y);
}

static void apply_pointer_enter(struct ctx *c, const char *rest)
{
	struct Window *w;
	struct Seat *s;
	uint32_t sid;
	uint32_t wid;

	if (sscanf(rest, " seat=%u window=%u", &sid, &wid) != 2)
		return;
	s = table_get(&c->seats, sid);
	w = table_get(&c->windows, wid);
	if (s == NULL || w == NULL)
		return;
	harness_pointer_enter(s, w);
}

static void apply_pointer_leave(struct ctx *c, const char *rest)
{
	struct Seat *s;
	uint32_t sid;

	if (sscanf(rest, " seat=%u", &sid) != 1)
		return;
	s = table_get(&c->seats, sid);
	if (s == NULL)
		return;
	harness_pointer_leave(s);
}

static void apply_op_delta(struct ctx *c, const char *rest)
{
	struct Seat *s;
	uint32_t sid;
	int32_t dx;
	int32_t dy;

	if (sscanf(rest, " seat=%u dx=%d dy=%d", &sid, &dx, &dy) != 3)
		return;
	s = table_get(&c->seats, sid);
	if (s == NULL)
		return;
	harness_op_delta(s, (int)dx, (int)dy);
}

static void apply_op_release(struct ctx *c, const char *rest)
{
	struct Seat *s;
	uint32_t sid;

	if (sscanf(rest, " seat=%u", &sid) != 1)
		return;
	s = table_get(&c->seats, sid);
	if (s == NULL)
		return;
	harness_op_release(s);
}

static void apply_window_interaction(struct ctx *c, const char *rest)
{
	struct Window *w;
	struct Seat *s;
	uint32_t sid;
	uint32_t wid;

	if (sscanf(rest, " seat=%u window=%u", &sid, &wid) != 2)
		return;
	s = table_get(&c->seats, sid);
	w = table_get(&c->windows, wid);
	if (s == NULL || w == NULL)
		return;
	river_seat_listener.window_interaction(s, NULL, w->obj);
}

static void apply_ate_unbound(struct ctx *c, const char *rest)
{
	struct Seat *s;
	uint32_t sid;

	if (sscanf(rest, " seat=%u", &sid) != 1)
		return;
	s = table_get(&c->seats, sid);
	if (s == NULL)
		return;
	river_xkb_bindings_seat_listener.ate_unbound_key(s, NULL);
}

static void apply_press(struct ctx *c, const char *rest)
{
	enum ActionKind kind;
	struct Seat *s;
	const char *fp;
	uint32_t sid;
	char arg_buf[512];
	char kind_buf[64];

	if (sscanf(rest, " seat=%u kind=%63s", &sid, kind_buf) != 2)
		return;
	s = table_get(&c->seats, sid);
	if (s == NULL)
		return;
	if (!action_kind_from_name(kind_buf, &kind))
		return;
	/*
	 * The harness lacks a path for ACTION_SPAWN / ACTION_EXIT that
	 * does not fork or kill us; replay records them for fidelity but
	 * skips dispatch.
	 */
	if (kind == ACTION_SPAWN || kind == ACTION_EXIT)
		return;
	fp = find_field(rest, "arg");
	if (fp == NULL)
		return;
	if (strncmp(fp, "none", 4) == 0) {
		harness_press(s, kind, NULL);
		return;
	}
	if (parse_quoted(fp, arg_buf, sizeof(arg_buf), NULL))
		harness_press(s, kind, arg_buf);
}

/* ---- main dispatch -------------------------------------------------- */

static char *read_blob(FILE *in, size_t bytes)
{
	char *buf;
	size_t got;
	int ch;

	buf = malloc(bytes + 1);
	if (buf == NULL)
		abort();
	got = fread(buf, 1, bytes, in);
	buf[got] = '\0';
	/* Consume the trailing newline that follows the body. */
	ch = fgetc(in);
	(void)ch;
	return buf;
}

static void handle_event(struct ctx *c, FILE *in, enum event_kind kind,
			 const char *rest, uint64_t event_ns)
{
	uint64_t cycle_start;
	uint64_t cycle_ns;

	wait_until_realtime(c, event_ns);

	switch (kind) {
	case EK_START:
		return;
	case EK_END:
		return;
	case EK_CONFIG_SET: {
		size_t bytes = 0;
		char *body;

		(void)sscanf(rest, " bytes=%zu", &bytes);
		body = read_blob(in, bytes);
		ensure_init(c, body);
		free(body);
		return;
	}
	case EK_CONFIG_RELOAD: {
		size_t bytes = 0;
		struct Seat *seat;
		char *body;

		(void)sscanf(rest, " bytes=%zu", &bytes);
		body = read_blob(in, bytes);
		if (!c->init_done) {
			ensure_init(c, body);
			free(body);
			return;
		}
		harness_stage_reload(body);
		free(body);
		seat = first_seat(c);
		if (seat != NULL) {
			cycle_start = mono_ns();
			harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
			harness_cycle();
			harness_cycle();
			cycle_ns = mono_ns() - cycle_start;
			stats_record(kind, cycle_ns);
		}
		c->event_count++;
		return;
	}
	case EK_SESSION_LOCKED:
	case EK_SESSION_UNLOCKED:
	case EK_STOP_REPEAT:
		/* No harness state to mutate; no cycle. */
		return;
	default:
		break;
	}

	if (!c->init_done)
		ensure_init(c, NULL);

	switch (kind) {
	case EK_OUTPUT_ADD:
		apply_output_add(c, rest);
		break;
	case EK_OUTPUT_POSITION:
		apply_output_position(c, rest);
		break;
	case EK_OUTPUT_DIMENSIONS:
		apply_output_dimensions(c, rest);
		break;
	case EK_OUTPUT_REMOVE:
		apply_output_remove(c, rest);
		break;
	case EK_OUTPUT_USABLE:
		apply_output_usable(c, rest);
		break;
	case EK_LAYER_FOCUS:
		apply_layer_focus(c, rest);
		break;
	case EK_SEAT_ADD:
		apply_seat_add(c, rest);
		break;
	case EK_SEAT_REMOVE:
		apply_seat_remove(c, rest);
		break;
	case EK_WINDOW_ADD:
		apply_window_add(c, rest);
		break;
	case EK_WINDOW_APP_ID:
		apply_window_app_id(c, rest);
		break;
	case EK_WINDOW_TITLE:
		apply_window_title(c, rest);
		break;
	case EK_WINDOW_PARENT:
		apply_window_parent(c, rest);
		break;
	case EK_WINDOW_DIMENSIONS_HINT:
		apply_window_dimensions_hint(c, rest);
		break;
	case EK_WINDOW_FULLSCREEN:
		apply_window_fullscreen(c, rest);
		break;
	case EK_WINDOW_CLOSE:
		apply_window_close(c, rest);
		break;
	case EK_POINTER_TO:
		apply_pointer_to(c, rest);
		break;
	case EK_POINTER_POSITION:
		apply_pointer_position(c, rest);
		break;
	case EK_POINTER_ENTER:
		apply_pointer_enter(c, rest);
		break;
	case EK_POINTER_LEAVE:
		apply_pointer_leave(c, rest);
		break;
	case EK_OP_DELTA:
		apply_op_delta(c, rest);
		break;
	case EK_OP_RELEASE:
		apply_op_release(c, rest);
		break;
	case EK_WINDOW_INTERACTION:
		apply_window_interaction(c, rest);
		break;
	case EK_ATE_UNBOUND:
		apply_ate_unbound(c, rest);
		break;
	case EK_PRESS:
	case EK_POINTER_PRESS:
	case EK_REPEAT_PRESS:
		apply_press(c, rest);
		break;
	default:
		return;
	}

	cycle_start = mono_ns();
	harness_cycle();
	cycle_ns = mono_ns() - cycle_start;
	stats_record(kind, cycle_ns);
	c->event_count++;
}

static int run_replay(FILE *in, struct ctx *c)
{
	char line[NM_REPLAY_LINE_CAP];

	if (fgets(line, sizeof(line), in) == NULL) {
		fprintf(stderr, "replay: empty trace\n");
		return 1;
	}
	if (strncmp(line, "nomonad-trace v=1", 17) != 0) {
		fprintf(stderr, "replay: bad header: %s", line);
		return 1;
	}

	while (fgets(line, sizeof(line), in) != NULL) {
		enum event_kind kind;
		uint64_t event_ns;
		const char *p;
		const char *rest;
		char kind_buf[64];

		if (line[0] != 'T' || line[1] != ' ')
			continue;
		p = line + 2;
		{
			char *end;
			unsigned long long t;

			errno = 0;
			t = strtoull(p, &end, 10);
			if (end == p || errno != 0)
				continue;
			event_ns = (uint64_t)t;
			p = end;
		}
		p = skip_ws(p);
		if (sscanf(p, "%63s", kind_buf) != 1)
			continue;
		rest = p + strlen(kind_buf);
		kind = kind_from_name(kind_buf);
		handle_event(c, in, kind, rest, event_ns);
	}
	return 0;
}

/* ---- main ----------------------------------------------------------- */

static void usage(FILE *out)
{
	fputs("Usage: replay [--realtime] [--no-stats] [--no-invariants] PATH\n",
	      out);
}

int main(int argc, char **argv)
{
	struct ctx c = { 0 };
	const char *path = NULL;
	int i;

	c.stats_enabled = true;
	c.invariants = true;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--realtime") == 0) {
			c.realtime = true;
		} else if (strcmp(argv[i], "--no-stats") == 0) {
			c.stats_enabled = false;
		} else if (strcmp(argv[i], "--no-invariants") == 0) {
			c.invariants = false;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "replay: unknown option '%s'\n",
				argv[i]);
			usage(stderr);
			return 2;
		} else if (path == NULL) {
			path = argv[i];
		} else {
			fprintf(stderr,
				"replay: unexpected argument '%s'\n",
				argv[i]);
			return 2;
		}
	}
	if (path == NULL) {
		usage(stderr);
		return 2;
	}

	table_init(&c.windows);
	table_init(&c.outputs);
	table_init(&c.seats);

	{
		FILE *in;
		int rc;

		in = fopen(path, "r");
		if (in == NULL) {
			fprintf(stderr, "replay: %s: %s\n", path,
				strerror(errno));
			return 1;
		}
		rc = run_replay(in, &c);
		fclose(in);
		if (rc != 0)
			return rc;
	}

	if (c.init_done) {
		fprintf(stdout, "replay: %llu events\n",
			(unsigned long long)c.event_count);
		if (c.stats_enabled)
			stats_print(stdout);
		harness_teardown();
	}

	table_free(&c.windows);
	table_free(&c.outputs);
	table_free(&c.seats);
	stats_free();
	return 0;
}
