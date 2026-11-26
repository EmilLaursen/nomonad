/* SPDX-License-Identifier: 0BSD */
/*
 * Headless harness library: globals, helpers, invariants, and the
 * main.c-equivalent stubs. Pair with fakewl.c, which link-wraps the
 * libwayland-client proxy entry points so the generated river_*_v1_*
 * request inlines become no-ops on synthetic proxies.
 *
 * Consumers (harness.c scenarios + fuzz.c LLVMFuzzerTestOneInput)
 * see only what harness_lib.h exposes; the rest is file-static.
 */

#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>
#include <river-input-management-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <river-xkb-config-v1-client-protocol.h>

#include "fakewl.h"
#include "harness_lib.h"
#include "input.h"
#include "keymap.h"

/* ---- main.c symbols the harness has to provide ----------------------- */

struct WindowManager wm;
struct river_window_manager_v1 *window_manager_v1;
struct river_xkb_bindings_v1 *xkb_bindings_v1;
struct river_layer_shell_v1 *layer_shell_v1;
struct Config *g_config;
volatile sig_atomic_t g_reload_pending;

struct Config *g_staged_config;

void schedule_reload(void)
{
	g_reload_pending = 1;
}

/*
 * Mirrors main.c's reload_now but pulls the new config from memory
 * (g_staged_config) instead of config_load(g_config_path). Both hand
 * the parsed config to wm_swap_config. Idempotent if nothing was
 * staged.
 */
void reload_now(void)
{
	struct Config *staging = g_staged_config;

	if (staging == NULL)
		return;
	g_staged_config = NULL;
	wm_swap_config(staging);
}

/* ---- listener vtables exposed by the WM modules ---------------------- */

extern const struct river_window_manager_v1_listener wm_listener;
extern const struct river_window_v1_listener river_window_listener;
extern const struct river_seat_v1_listener river_seat_listener;
extern const struct river_output_v1_listener river_output_listener;


#define HARNESS_FAKE_VERSION 4
#define HARNESS_NATURAL_W 640
#define HARNESS_NATURAL_H 480

static void *harness_proxy_new(const char *iface)
{
	struct fake_proxy *p;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		abort();
	p->version = HARNESS_FAKE_VERSION;
	p->iface = iface;
	return p;
}

/* ---- invariants ------------------------------------------------------ */

/*
 * Mirrors wm.c's window_in_op (file-static there). A window in an
 * active SEAT_OP_MOVE / SEAT_OP_RESIZE on any seat is excluded from
 * apply_layout so the layout does not fight the user's drag; the
 * window keeps its op-driven rect instead of the layout's.
 */
static bool harness_window_in_op(const struct Window *w)
{
	struct Seat *seat;

	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->op_window == w &&
		    (seat->op == SEAT_OP_MOVE || seat->op == SEAT_OP_RESIZE))
			return true;
	}
	return false;
}

/*
 * A tile rect on a workspace: laid out by apply_layout, not skipped by
 * the per-window filter (floating, ignored, in-op) or the workspace-
 * level fullscreen short-circuit. Mirrors wm.c apply_layout exactly.
 * requested_close stays tiled - production keeps drawing the window
 * until the server fires .closed; closed windows are reaped by
 * window_maybe_destroy before apply_layout, so they never appear here
 * in practice. visible_desired filters by workspace visibility
 * (apply_layout only touches the visible_workspace per output).
 * dimensions_synced is the layout's "I touched this" marker - false
 * on a window that just appeared and has not yet been through a
 * manage cycle.
 */
static bool harness_is_tiled(const struct Window *w)
{
	if (w == NULL)
		return false;
	if (w->ignored || w->floating || w->fullscreen)
		return false;
	if (harness_window_in_op(w))
		return false;
	if (!w->visible_desired)
		return false;
	return w->dimensions_synced;
}

static bool harness_rects_overlap(s32 ax, s32 ay, s32 aw, s32 ah, s32 bx,
				  s32 by, s32 bw, s32 bh)
{
	return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/*
 * Each window in workspace i must have workspace_index == i. Catches a
 * misrouted move-to-workspace, a missed workspace_link removal, or
 * a workspace_index getting out of sync with the list it lives in.
 */
static void harness_check_window_workspace(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			assert(w->workspace_index == (int)i);
		}
	}
}

/*
 * Each output's visible_workspace is in range and unique across outputs.
 * The "swap on workspace_switch" logic is the easiest place to violate
 * this; the assertion catches the moment two outputs claim the same
 * workspace.
 */
static void harness_check_output_workspace(void)
{
	struct Output *a;
	struct Output *b;

	wl_list_for_each(a, &wm.outputs, link) {
		assert(a->visible_workspace >= 0);
		assert((size_t)a->visible_workspace < wm.n_workspaces);
		wl_list_for_each(b, &wm.outputs, link) {
			if (b == a)
				continue;
			if ((uintptr_t)a >= (uintptr_t)b)
				continue;
			assert(a->visible_workspace != b->visible_workspace);
		}
	}
}

/*
 * A non-NULL focused window must be focusable: not ignored, not in
 * teardown. Stronger workspace alignment is enforced lazily via
 * seat_focus_on_visible_ws inside seat_action; off-visible-ws focus is
 * legal between cycles, so we do not check that here.
 */
static void harness_check_focus(void)
{
	struct Seat *s;

	wl_list_for_each(s, &wm.seats, link) {
		if (s->focused == NULL)
			continue;
		assert(!s->focused->ignored);
		assert(!s->focused->closed);
		assert(!s->focused->requested_close);
	}
}

/*
 * No two tile rects on the same workspace overlap. Uses what is drawn:
 * the content clip box when one is set, so a minimum size larger than
 * the cell does not count as overlap, else the proposed dimensions.
 */
static s32 harness_drawn_w(const struct Window *w)
{
	return (w->clip_synced && w->last_clip_w > 0) ? w->last_clip_w :
							 w->last_proposed_w;
}

static s32 harness_drawn_h(const struct Window *w)
{
	return (w->clip_synced && w->last_clip_h > 0) ? w->last_clip_h :
							 w->last_proposed_h;
}

static void harness_check_no_tile_overlap(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *a;
		struct Window *b;

		wl_list_for_each(a, &wm.workspaces[i].windows, workspace_link) {
			if (!harness_is_tiled(a))
				continue;
			wl_list_for_each(b, &wm.workspaces[i].windows,
					 workspace_link) {
				if (b == a || !harness_is_tiled(b))
					continue;
				if ((uintptr_t)a >= (uintptr_t)b)
					continue;
				assert(!harness_rects_overlap(
					a->x, a->y, harness_drawn_w(a),
					harness_drawn_h(a), b->x, b->y,
					harness_drawn_w(b), harness_drawn_h(b)));
			}
		}
	}
}

/*
 * Every tile fits inside the output that is showing its workspace.
 * Catches layout bugs that propose negative dimensions or place a tile
 * past the right / bottom edge. Skips workspaces that are not currently
 * shown anywhere; their tiles are stale by design.
 */
static void harness_check_tiles_inside_output(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Output *out;
		struct Window *w;

		out = output_showing((int)i);
		if (out == NULL)
			continue;
		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			if (!harness_is_tiled(w))
				continue;
			assert(w->last_proposed_w > 0);
			assert(w->last_proposed_h > 0);
			assert(w->x >= out->x);
			assert(w->y >= out->y);
			assert(w->x + w->last_proposed_w <= out->x + out->width);
			assert(w->y + w->last_proposed_h <=
			       out->y + out->height);
		}
	}
}

/*
 * Same predicate as wm.c's apply_layout uses to filter the "wanted"
 * window set passed to BSP. Must match exactly so leaf-count /
 * tile-count comparisons hold. Production's filter is
 * `window_in_op || floating || ignored`; closed windows are reaped
 * before apply_layout so they never reach the filter, requested_close
 * stays in. Fullscreen is NOT excluded here even though it short-
 * circuits the layout: harness_check_bsp skips workspaces with a
 * fullscreen window separately.
 */
static bool harness_is_bsp_wanted(const struct Window *w)
{
	if (w->ignored || w->floating)
		return false;
	if (harness_window_in_op(w))
		return false;
	return true;
}

static struct Window *harness_workspace_fullscreen(struct Workspace *ws)
{
	struct Window *w;

	wl_list_for_each(w, &ws->windows, workspace_link) {
		if (w->fullscreen)
			return w;
	}
	return NULL;
}

/*
 * Tile-extent invariant: the bounding box of all tile rects on a
 * visible workspace fills the inset-adjusted available area exactly.
 *
 * Each cell that the layout produces is inset by border + inner_gap on
 * all sides (window_apply_cell in layout.c). So the leftmost tile's
 * left edge is adj.x + inset; the rightmost tile's right edge is
 * adj.x + adj.w - inset. Same for y. Any layout that loses pixels
 * because the last cell's height does not reach the bottom of the
 * available area, or the first cell's origin lands past the inset,
 * fails this check.
 *
 * Holes in the middle (gaps between adjacent tiles beyond 2 * inset)
 * are not caught by a bounding-box check; that would need a coverage
 * scan and is left for later if a real bug ever motivates it.
 *
 * Skipped on workspaces with no tiles, with a fullscreen window
 * (apply_layout bypasses the layout), or whose available area is too
 * small for the layout to produce any cell. Also skipped when at
 * least one wanted window had its cell collapse below 2 * inset on
 * either axis - window_apply_cell hides such windows by clearing
 * visible_desired, so the surviving tiles no longer cover the full
 * area by definition. Reachable from BSP under heavy ratio skew.
 */
static int harness_count_tile_candidates(struct Workspace *ws)
{
	struct Window *w;
	int n;

	if (harness_workspace_fullscreen(ws) != NULL)
		return 0;
	n = 0;
	wl_list_for_each(w, &ws->windows, workspace_link) {
		if (harness_window_in_op(w) || w->floating || w->ignored)
			continue;
		n++;
	}
	return n;
}

static void harness_check_tile_extent(void)
{
	struct Output *out;

	if (g_config == NULL)
		return;

	wl_list_for_each(out, &wm.outputs, link) {
		s32 expected_max_x;
		s32 expected_max_y;
		s32 expected_min_x;
		s32 expected_min_y;
		s32 base_x;
		s32 base_y;
		s32 base_w;
		s32 base_h;
		struct Workspace *ws;
		struct Window *w;
		bool layout_collapsed;
		bool found_tile;
		bool smart_gaps;
		bool smart_borders;
		s32 inset;
		s32 max_x, max_y;
		s32 min_x, min_y;
		int n_tiles;
		int wsi;

		wsi = out->visible_workspace;
		if (wsi < 0 || (size_t)wsi >= wm.n_workspaces)
			continue;
		ws = &wm.workspaces[wsi];
		if (harness_workspace_fullscreen(ws) != NULL)
			continue;

		layout_collapsed = false;
		wl_list_for_each(w, &ws->windows, workspace_link) {
			if (w->ignored || w->floating)
				continue;
			if (harness_window_in_op(w))
				continue;
			if (!w->visible_desired) {
				layout_collapsed = true;
				break;
			}
		}
		if (layout_collapsed)
			continue;

		found_tile = false;
		min_x = INT32_MAX;
		min_y = INT32_MAX;
		max_x = INT32_MIN;
		max_y = INT32_MIN;
		wl_list_for_each(w, &ws->windows, workspace_link) {
			if (!harness_is_tiled(w))
				continue;
			found_tile = true;
			if (w->x < min_x)
				min_x = w->x;
			if (w->y < min_y)
				min_y = w->y;
			if (w->x + w->last_proposed_w > max_x)
				max_x = w->x + w->last_proposed_w;
			if (w->y + w->last_proposed_h > max_y)
				max_y = w->y + w->last_proposed_h;
		}
		if (!found_tile)
			continue;

		/*
		 * Mirror apply_layout's smart-suppression: a single tile
		 * candidate with :smart on the relevant block drops the gap
		 * struts / inner inset (smart_gaps) or the layout border
		 * inset (smart_borders). The expected bounding box collapses
		 * to the raw output rect minus only the still-active insets.
		 */
		n_tiles = harness_count_tile_candidates(ws);
		smart_gaps = g_config->gaps.smart && n_tiles == 1;
		smart_borders = g_config->borders.smart && n_tiles == 1;
		inset = (smart_gaps ? 0 : g_config->gaps.inner) +
			(smart_borders ? 0 : g_config->borders.width);

		base_x = out->has_usable ? out->usable_x : out->x;
		base_y = out->has_usable ? out->usable_y : out->y;
		base_w = out->has_usable ? out->usable_width : out->width;
		base_h = out->has_usable ? out->usable_height : out->height;
		expected_min_x =
			base_x + (smart_gaps ? 0 : g_config->gaps.left) + inset;
		expected_min_y =
			base_y + (smart_gaps ? 0 : g_config->gaps.top) + inset;
		expected_max_x = base_x + base_w -
				 (smart_gaps ? 0 : g_config->gaps.right) -
				 inset;
		expected_max_y = base_y + base_h -
				 (smart_gaps ? 0 : g_config->gaps.bottom) -
				 inset;

		assert(min_x == expected_min_x);
		assert(min_y == expected_min_y);
		assert(max_x == expected_max_x);
		assert(max_y == expected_max_y);
	}
}

/*
 * BSP tree well-formedness on every output's visible workspace. Skips
 * workspaces with a fullscreen window because apply_layout bypasses
 * the layout entirely in that case, so the tree is intentionally stale
 * (the same leaves it had before the fullscreen toggle). Likewise
 * skips non-visible workspaces: they have not had apply_layout run
 * since their last edit.
 *
 * Asserts: tree is internally well-formed; every split's ratio is in
 * [BSP_RATIO_MIN, BSP_RATIO_MAX]; the set of leaves equals the set of
 * tile-eligible windows on the workspace, both in count and as
 * pointers.
 */
static void harness_check_bsp(void)
{
	struct Output *out;

	wl_list_for_each(out, &wm.outputs, link) {
		struct LayoutBspStats stats;
		struct Workspace *ws;
		struct Window *w;
		size_t n_wanted;
		int wsi;

		wsi = out->visible_workspace;
		if (wsi < 0 || (size_t)wsi >= wm.n_workspaces)
			continue;
		ws = &wm.workspaces[wsi];
		if (workspace_layout(ws)->kind != LAYOUT_BSP)
			continue;
		if (harness_workspace_fullscreen(ws) != NULL)
			continue;

		layout_bsp_stats(workspace_layout(ws), &stats);
		assert(stats.well_formed);
		if (stats.n_splits > 0) {
			assert(stats.min_ratio >= 0.05f - 1e-6f);
			assert(stats.max_ratio <= 0.95f + 1e-6f);
		}

		n_wanted = 0;
		wl_list_for_each(w, &ws->windows, workspace_link) {
			if (!harness_is_bsp_wanted(w))
				continue;
			n_wanted++;
			assert(layout_bsp_contains(workspace_layout(ws), w));
		}
		/*
		 * apply_layout in wm.c only invokes the layout when at least
		 * one window is tile-eligible (line `if (n > 0)`). With zero
		 * wanted, neither bsp_drop_unwanted nor bsp_insert runs, so
		 * the tree is allowed to retain whatever leaves it last had.
		 */
		if (n_wanted > 0)
			assert(stats.n_leaves == n_wanted);
	}
}

/* ---- runtime toggles + per-cycle stats ------------------------------- */

/*
 * Bench mode disables the invariants so timing measurements isolate
 * production code cost. The check functions are O(n^2) on overlap +
 * tile-extent and would otherwise dominate microbenchmarks. Default
 * is enabled (correctness over speed).
 */
static bool g_invariants_enabled = true;

void harness_set_invariants(bool enabled)
{
	g_invariants_enabled = enabled;
}

static void harness_check_float_stacking(void)
{
	struct Window *focused;
	struct Seat *seat;
	struct Window *w;

	if (wm.restack_floats || wl_list_empty(&wm.seats))
		return;
	seat = wl_container_of(wm.seats.next, seat, link);
	focused = seat->focused;
	if (focused == NULL || focused->floating || focused->ignored ||
	    focused->closed || focused->fullscreen ||
	    output_showing(focused->workspace_index) == NULL ||
	    wm_workspace_fullscreen(&wm.workspaces[focused->workspace_index]) !=
		    NULL)
		return;
	wl_list_for_each(w, &wm.workspaces[focused->workspace_index].windows,
			 workspace_link) {
		if (w->floating && !w->ignored && !w->closed)
			assert(w->raise_seq > focused->raise_seq);
	}
}

static void harness_check_all(void)
{
	if (!g_invariants_enabled)
		return;
	harness_check_window_workspace();
	harness_check_output_workspace();
	harness_check_focus();
	harness_check_no_tile_overlap();
	harness_check_tiles_inside_output();
	harness_check_tile_extent();
	harness_check_bsp();
	harness_check_float_stacking();
}

/*
 * Per-cycle stats: timing in nanoseconds + libwayland-client request
 * deltas. samples grow on demand via realloc-doubling. harness_cycle
 * records one sample per invocation while enabled; harness_stats_print
 * sorts and reports min / mean / p50 / p95 / p99 / max for both
 * metrics. Off by default so non-bench scenarios pay nothing.
 */
struct CycleStats {
	bool enabled;
	size_t count;
	size_t cap;
	uint64_t *time_ns;
	uint64_t *requests;
};

static struct CycleStats g_stats;

void harness_stats_enable(void)
{
	g_stats.enabled = true;
	g_stats.count = 0;
	harness_request_count_reset();
}

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void stats_grow(void)
{
	uint64_t *grown_t;
	uint64_t *grown_r;
	size_t newcap;

	newcap = g_stats.cap ? g_stats.cap * 2 : 1024;
	grown_t = realloc(g_stats.time_ns, newcap * sizeof(*grown_t));
	if (grown_t == NULL)
		abort();
	g_stats.time_ns = grown_t;
	grown_r = realloc(g_stats.requests, newcap * sizeof(*grown_r));
	if (grown_r == NULL)
		abort();
	g_stats.requests = grown_r;
	g_stats.cap = newcap;
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

void harness_stats_print(FILE *out)
{
	uint64_t *time_sorted;
	uint64_t *req_sorted;
	uint64_t time_sum;
	uint64_t req_sum;
	size_t n;
	size_t i;

	if (!g_stats.enabled || g_stats.count == 0)
		return;
	n = g_stats.count;
	time_sorted = malloc(n * sizeof(*time_sorted));
	req_sorted = malloc(n * sizeof(*req_sorted));
	if (time_sorted == NULL || req_sorted == NULL)
		abort();
	memcpy(time_sorted, g_stats.time_ns, n * sizeof(*time_sorted));
	memcpy(req_sorted, g_stats.requests, n * sizeof(*req_sorted));
	qsort(time_sorted, n, sizeof(*time_sorted), compare_u64);
	qsort(req_sorted, n, sizeof(*req_sorted), compare_u64);
	time_sum = 0;
	req_sum = 0;
	for (i = 0; i < n; i++) {
		time_sum += g_stats.time_ns[i];
		req_sum += g_stats.requests[i];
	}
	fprintf(out, "harness cycle stats: n=%zu\n", n);
	fprintf(out,
		"  cycle latency (us): "
		"min=%.2f mean=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f\n",
		time_sorted[0] / 1000.0,
		(double)time_sum / (double)n / 1000.0,
		time_sorted[n / 2] / 1000.0,
		time_sorted[(n * 95) / 100] / 1000.0,
		time_sorted[(n * 99) / 100] / 1000.0,
		time_sorted[n - 1] / 1000.0);
	fprintf(out,
		"  requests/cycle:     "
		"min=%llu mean=%.1f p50=%llu p95=%llu p99=%llu max=%llu "
		"(total=%llu)\n",
		(unsigned long long)req_sorted[0],
		(double)req_sum / (double)n,
		(unsigned long long)req_sorted[n / 2],
		(unsigned long long)req_sorted[(n * 95) / 100],
		(unsigned long long)req_sorted[(n * 99) / 100],
		(unsigned long long)req_sorted[n - 1],
		(unsigned long long)req_sum);
	free(time_sorted);
	free(req_sorted);
}

/* ---- injection helpers ----------------------------------------------- */

struct Output *harness_add_output(int x, int y, int w, int h)
{
	struct fake_proxy *proxy;
	struct Output *out;

	proxy = harness_proxy_new("river_output_v1");
	wm_listener.output(NULL, NULL, (struct river_output_v1 *)proxy);
	out = proxy->user_data;
	river_output_listener.position(out, NULL, x, y);
	river_output_listener.dimensions(out, NULL, w, h);
	return out;
}

struct Seat *harness_add_seat(struct Output *primary)
{
	struct fake_proxy *proxy;
	struct Seat *seat;

	proxy = harness_proxy_new("river_seat_v1");
	wm_listener.seat(NULL, NULL, (struct river_seat_v1 *)proxy);
	seat = proxy->user_data;
	if (primary != NULL)
		harness_pointer_to(seat, primary);
	return seat;
}

/*
 * Move the seat's pointer to the centre of `out`. Mirrors the river
 * pointer_position event that fires when the cursor crosses an output
 * boundary; production reads seat->output from this so subsequent
 * ACTION_WORKSPACE / ACTION_MOVE_TO_WORKSPACE target the new output's
 * visible_workspace, and the seat-bound action dispatch follows.
 */
void harness_pointer_to(struct Seat *s, struct Output *out)
{
	if (s == NULL || out == NULL)
		return;
	river_seat_listener.pointer_position(s, NULL,
					     out->x + out->width / 2,
					     out->y + out->height / 2);
}

/*
 * Pointer crosses into a window. Drives the river pointer_enter event
 * so seat->hovered = w; ACTION_MOVE / ACTION_RESIZE pull the op target
 * from there. seat->hovered is also read by some focus-follows-mouse
 * paths; fuzz drivers that use this should expect those side effects.
 */
void harness_pointer_enter(struct Seat *s, struct Window *w)
{
	if (s == NULL || w == NULL)
		return;
	river_seat_listener.pointer_enter(s, NULL, w->obj);
}

void harness_pointer_leave(struct Seat *s)
{
	if (s == NULL)
		return;
	river_seat_listener.pointer_leave(s, NULL);
}

/*
 * Pointer movement during an in-progress op. The op state machine in
 * seat.c reads op_dx / op_dy at the next manage_start (move) or render
 * cycle (resize), so the caller must run a harness_cycle for the delta
 * to actually take effect on the op_window.
 */
void harness_op_delta(struct Seat *s, int dx, int dy)
{
	if (s == NULL)
		return;
	river_seat_listener.op_delta(s, NULL, (s32)dx, (s32)dy);
}

/*
 * End the in-progress op. seat->op_release is observed at manage_start
 * which clears the op state and resets op_window. A cycle is needed
 * for the seat to actually return to SEAT_OP_NONE.
 */
void harness_op_release(struct Seat *s)
{
	if (s == NULL)
		return;
	river_seat_listener.op_release(s, NULL);
}

struct Window *harness_add_window(const char *app_id)
{
	struct fake_proxy *proxy;
	struct Window *w;

	proxy = harness_proxy_new("river_window_v1");
	wm_listener.window(NULL, NULL, (struct river_window_v1 *)proxy);
	w = proxy->user_data;
	if (app_id != NULL)
		river_window_listener.app_id(w, NULL, app_id);
	return w;
}

void harness_close_window(struct Window *w)
{
	river_window_listener.closed(w, NULL);
}

/*
 * Models a monitor unplug. The river_output_v1.removed event sets the
 * Output's removed flag; reaping (free, list removal, orphan_ws push,
 * seat->output clear) is deferred to wm_handle_manage_start. Caller
 * must run a cycle before referencing the Output pointer again.
 */
void harness_remove_output(struct Output *out)
{
	river_output_listener.removed(out, NULL);
}

/*
 * Parse `config_src` and stash it in g_staged_config for the next
 * reload. ACTION_RELOAD_CONFIG (or schedule_reload directly) sets
 * g_reload_pending; the next manage_start drains the pending flag and
 * calls reload_now, which swaps g_config to g_staged_config. Aborts
 * on parse failure - tests should hand valid configs.
 */
void harness_stage_reload(const char *config_src)
{
	char err[256];

	g_staged_config = config_parse(config_src, err, sizeof(err));
	if (g_staged_config == NULL) {
		fprintf(stderr, "harness_stage_reload: %s\n", err);
		abort();
	}
}

void harness_press(struct Seat *s, enum ActionKind kind, const char *arg)
{
	s->pending_action.kind = kind;
	s->pending_action.arg = (arg != NULL) ? strdup(arg) : NULL;
}

static struct XkbBinding *harness_find_binding(struct Seat *s,
					       enum ActionKind kind)
{
	struct XkbBinding *b;

	wl_list_for_each(b, &s->xkb_bindings, link) {
		if (b->action.kind == kind)
			return b;
	}
	return NULL;
}

static const struct river_xkb_binding_v1_listener *
harness_binding_listener(struct XkbBinding *b)
{
	struct fake_proxy *p = (struct fake_proxy *)b->obj;

	assert(p != NULL && p->implementation != NULL);
	return (const struct river_xkb_binding_v1_listener *)p->implementation;
}

extern const struct river_input_manager_v1_listener input_manager_listener;
extern const struct river_libinput_config_v1_listener input_libinput_listener;

void *harness_input_device(u32 type)
{
	const struct river_input_device_v1_listener *l;
	struct fake_proxy *p = harness_proxy_new("river_input_device_v1");

	input_manager_listener.input_device(NULL, NULL,
					    (struct river_input_device_v1 *)p);
	l = (const struct river_input_device_v1_listener *)p->implementation;
	l->type(p->user_data, (struct river_input_device_v1 *)p, type);
	return p;
}

void *harness_libinput_device(s32 tap_fingers)
{
	const struct river_libinput_device_v1_listener *l;
	struct fake_proxy *p = harness_proxy_new("river_libinput_device_v1");

	input_libinput_listener.libinput_device(
		NULL, NULL, (struct river_libinput_device_v1 *)p);
	l = (const struct river_libinput_device_v1_listener *)p->implementation;
	l->tap_support(p->user_data, (struct river_libinput_device_v1 *)p,
		       tap_fingers);
	return p;
}

void harness_libinput_defaults(void *dev, u32 tap, u32 natural_scroll)
{
	const struct river_libinput_device_v1_listener *l;
	struct fake_proxy *p = dev;

	l = (const struct river_libinput_device_v1_listener *)p->implementation;
	l->tap_default(p->user_data, (struct river_libinput_device_v1 *)p, tap);
	l->natural_scroll_default(p->user_data,
				  (struct river_libinput_device_v1 *)p,
				  natural_scroll);
}

void *harness_enable_xkb_config(void)
{
	struct fake_proxy *p = harness_proxy_new("river_xkb_config_v1");

	keymap_attach((struct river_xkb_config_v1 *)p);
	return p;
}

void *harness_xkb_keyboard(void *xkb_config)
{
	const struct river_xkb_config_v1_listener *l;
	struct fake_proxy *cfg = xkb_config;
	struct fake_proxy *kb = harness_proxy_new("river_xkb_keyboard_v1");

	l = (const struct river_xkb_config_v1_listener *)cfg->implementation;
	l->xkb_keyboard(cfg->user_data, (struct river_xkb_config_v1 *)cfg,
			(struct river_xkb_keyboard_v1 *)kb);
	return kb;
}

void harness_keymap_success(void *keymap)
{
	const struct river_xkb_keymap_v1_listener *l;
	struct fake_proxy *p = keymap;

	l = (const struct river_xkb_keymap_v1_listener *)p->implementation;
	l->success(p->user_data, (struct river_xkb_keymap_v1 *)p);
}

void harness_keymap_failure(void *keymap)
{
	const struct river_xkb_keymap_v1_listener *l;
	struct fake_proxy *p = keymap;

	l = (const struct river_xkb_keymap_v1_listener *)p->implementation;
	l->failure(p->user_data, (struct river_xkb_keymap_v1 *)p, "test");
}

void harness_finish_tracked(void)
{
	const struct river_libinput_result_v1_listener *l;

	for (size_t i = 0; i < harness_tracked_count(); i++) {
		struct fake_proxy *p = (struct fake_proxy *)harness_tracked(i);

		l = (const struct river_libinput_result_v1_listener *)
			    p->implementation;
		l->success(p->user_data, (struct river_libinput_result_v1 *)p);
	}
	harness_track_iface(NULL);
}

void harness_pointer_at(struct Seat *s, s32 x, s32 y)
{
	river_seat_listener.pointer_position(s, NULL, x, y);
}

void harness_window_capture(struct Window *w, u32 count)
{
	river_window_listener.capture_sessions(w, NULL, count);
}

void harness_output_capture(struct Output *o, u32 count)
{
	river_output_listener.capture_sessions(o, NULL, count);
}

void harness_window_presentation(struct Window *w, u32 hint)
{
	river_window_listener.presentation_hint(w, NULL, hint);
}

void harness_interact(struct Seat *s, struct Window *w)
{
	river_seat_listener.window_interaction(s, NULL, w->obj);
}

void harness_enable_layer_shell(void)
{
	layer_shell_v1 = harness_proxy_new("river_layer_shell_v1");
}

void harness_set_usable(struct Output *o, s32 x, s32 y, s32 w, s32 h)
{
	const struct river_layer_shell_output_v1_listener *l;
	struct fake_proxy *p = (struct fake_proxy *)o->ls_output;

	assert(p != NULL && p->implementation != NULL);
	l = (const struct river_layer_shell_output_v1_listener *)
		    p->implementation;
	l->non_exclusive_area(o, o->ls_output, x, y, w, h);
}

void harness_layer_focus(struct Seat *s, enum LayerFocus focus)
{
	const struct river_layer_shell_seat_v1_listener *l;
	struct fake_proxy *p = (struct fake_proxy *)s->ls_seat;

	assert(p != NULL && p->implementation != NULL);
	l = (const struct river_layer_shell_seat_v1_listener *)p->implementation;
	if (focus == LAYER_FOCUS_EXCLUSIVE)
		l->focus_exclusive(s, s->ls_seat);
	else if (focus == LAYER_FOCUS_NON_EXCLUSIVE)
		l->focus_non_exclusive(s, s->ls_seat);
	else
		l->focus_none(s, s->ls_seat);
}

void harness_key_press(struct Seat *s, enum ActionKind kind)
{
	struct XkbBinding *b;

	b = harness_find_binding(s, kind);
	assert(b != NULL);
	harness_binding_listener(b)->pressed(b, b->obj);
}

void harness_key_release(struct Seat *s, enum ActionKind kind)
{
	struct XkbBinding *b;

	b = harness_find_binding(s, kind);
	assert(b != NULL);
	harness_binding_listener(b)->released(b, b->obj);
}

/*
 * Simulate the server's window.dimensions event for any window that
 * had propose_dimensions called this cycle. The real river commits
 * the proposed size (possibly clamped against client size hints) and
 * fires the dimensions event back; with only proxy stubs there is no
 * server, so the harness echoes the propose value through the
 * listener directly. Without this, w->width / w->height stay 0 and
 * code that reads them - seat_pointer_resize captures op_start_width
 * from w->width on drag start - sees the wrong initial state.
 */
static void harness_echo_dimensions(void)
{
	size_t i;

	for (i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			s32 ew, eh;

			if (!w->dimensions_synced)
				continue;
			ew = (w->last_proposed_w > 0) ? w->last_proposed_w :
							HARNESS_NATURAL_W;
			eh = (w->last_proposed_h > 0) ? w->last_proposed_h :
							HARNESS_NATURAL_H;
			if (w->width == ew && w->height == eh)
				continue;
			river_window_listener.dimensions(w, NULL, ew, eh);
		}
	}
}

void harness_cycle(void)
{
	uint64_t start_ns;
	uint64_t start_req;

	if (g_stats.enabled) {
		start_ns = monotonic_ns();
		start_req = harness_request_count_get();
	} else {
		start_ns = 0;
		start_req = 0;
	}

	fakewl_phase = FAKEWL_MANAGE;
	wm_listener.manage_start(NULL, NULL);
	fakewl_phase = FAKEWL_RENDER;
	wm_listener.render_start(NULL, NULL);
	fakewl_phase = FAKEWL_IDLE;
	harness_echo_dimensions();
	harness_check_all();

	if (g_stats.enabled) {
		if (g_stats.count == g_stats.cap)
			stats_grow();
		g_stats.time_ns[g_stats.count] = monotonic_ns() - start_ns;
		g_stats.requests[g_stats.count] =
			harness_request_count_get() - start_req;
		g_stats.count++;
	}
}

int harness_count_ws_windows(int wsi)
{
	struct Window *w;
	int n;

	n = 0;
	if (wsi < 0 || (size_t)wsi >= wm.n_workspaces)
		return 0;
	wl_list_for_each(w, &wm.workspaces[wsi].windows, workspace_link)
		n++;
	return n;
}

/*
 * Each scenario starts from a fresh process so wm and g_config are
 * zeroed. config_src lets a scenario opt into a non-default layout
 * (deflayout bsp) without rebuilding the whole config from scratch.
 * NULL falls back to config_default(), which uses LAYOUT_TALL.
 */
void harness_init(const char *config_src)
{
	if (config_src != NULL) {
		char err[256];

		g_config = config_parse(config_src, err, sizeof(err));
		if (g_config == NULL) {
			fprintf(stderr, "harness_init: %s\n", err);
			abort();
		}
	} else {
		g_config = config_default();
	}
	xkb_bindings_v1 = harness_proxy_new("river_xkb_bindings_v1");
	wm_init();
}

/*
 * Tear every harness allocation back down so a subsequent harness_init
 * starts from clean global state. Used by the in-process runner
 * (./harness --inproc) and intended for libFuzzer's
 * LLVMFuzzerTestOneInput, where each input must start fresh without
 * forking.
 *
 * Order is load-bearing:
 *   1. Outputs first - output_maybe_destroy iterates seats to clear
 *      seat->output, so seats must still be alive.
 *   2. Windows next - window_maybe_destroy iterates seats to clear
 *      seat->focused, same reason.
 *   3. Seats last.
 *
 * Frees that production never reaches because workspaces live for
 * the lifetime of the process - wm_init's allocations stay until
 * the WM exits - so the strdup'd names and the realloc'd
 * layout_arr scratch buffers accumulate across init/teardown cycles
 * unless this routine sweeps them.
 */
void harness_teardown(void)
{
	struct Output *o, *otmp;
	struct Seat *s, *stmp;
	size_t i;

	fakewl_phase = FAKEWL_TEARDOWN;

	wl_list_for_each_safe(o, otmp, &wm.outputs, link) {
		o->removed = true;
		output_maybe_destroy(o);
	}

	for (i = 0; i < wm.n_workspaces; i++) {
		struct Window *w, *wtmp;
		struct Workspace *ws = &wm.workspaces[i];

		wl_list_for_each_safe(w, wtmp, &ws->windows, workspace_link) {
			w->closed = true;
			window_maybe_destroy(w);
		}
		for (size_t k = 0; k < ws->n_layouts; k++)
			layout_deinit(&ws->layouts[k]);
		ws->n_layouts = 0;
		free(ws->name);
		free(ws->layout_arr);
	}
	free(wm.workspaces);
	wm.workspaces = NULL;
	wm.n_workspaces = 0;

	wl_list_for_each_safe(s, stmp, &wm.seats, link) {
		s->removed = true;
		seat_maybe_destroy(s);
	}

	free(layer_shell_v1);
	layer_shell_v1 = NULL;
	input_shutdown();
	keymap_shutdown();
	free(xkb_bindings_v1);
	xkb_bindings_v1 = NULL;

	config_free(g_config);
	config_free(g_staged_config);
	g_config = NULL;
	g_staged_config = NULL;

	memset(&wm, 0, sizeof(wm));
	g_reload_pending = 0;

	free(g_stats.time_ns);
	free(g_stats.requests);
	g_stats.time_ns = NULL;
	g_stats.requests = NULL;
	g_stats.count = 0;
	g_stats.cap = 0;
	g_stats.enabled = false;
	harness_request_count_reset();
	fakewl_reap();
	fakewl_phase = FAKEWL_IDLE;
}
