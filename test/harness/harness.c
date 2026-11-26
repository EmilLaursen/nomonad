/* SPDX-License-Identifier: 0BSD */
/*
 * Headless driver for the manage / render cycle. Hand-written
 * scenarios calling into harness_lib.h, plus a runner that either
 * forks per scenario (for assertion isolation) or runs them all
 * sequentially in one process (for ASan / libFuzzer-style leak
 * checks at exit).
 *
 * Pair with harness_lib.c (globals, helpers, invariants, cycle) and
 * fakewl.c (wl_proxy_* link-wraps). main.c is not linked here;
 * harness_lib.c provides the globals and reload stubs in its place.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>

#include <river-input-management-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <river-xkb-config-v1-client-protocol.h>

#include "ctl.h"
#include "harness_lib.h"
#include "recorder.h"
#include "restore.h"
#include "status.h"

/* ---- scenarios ------------------------------------------------------- */

static void scenario_focus_and_move(void)
{
	struct Window *w1;
	struct Window *w2;
	struct Window *w3;
	struct Output *out;
	struct Seat *seat;

	harness_init(NULL);

	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();
	assert(seat->output == out);
	assert(out->visible_workspace == 0);

	/*
	 * Three windows on workspace 0. Auto-focus follows the latest, and
	 * wl_list_insert puts new heads at front, so the list head -> tail
	 * is w3, w2, w1. ACTION_FOCUS_NEXT walks head->tail (w3 -> w2 -> w1
	 * -> wrap), ACTION_FOCUS_PREV the other direction.
	 */
	w1 = harness_add_window("kitty");
	w2 = harness_add_window("firefox");
	w3 = harness_add_window("kitty");
	harness_cycle();
	assert(harness_count_ws_windows(0) == 3);
	assert(seat->focused == w3);

	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	assert(seat->focused == w2);

	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	assert(seat->focused == w1);

	harness_press(seat, ACTION_MOVE_TO_WORKSPACE, "2");
	harness_cycle();
	assert(harness_count_ws_windows(0) == 2);
	assert(harness_count_ws_windows(1) == 1);
	assert(w1->workspace_index == 1);

	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	assert(out->visible_workspace == 1);
	assert(seat->focused == w1);

	harness_close_window(w1);
	harness_cycle();
	assert(harness_count_ws_windows(1) == 0);
	assert(seat->focused == NULL);

	(void)w2;
	(void)w3;
}

/*
 * Multi-output workspace swap: when one output is showing ws A and
 * the seat on the other output runs (workspace A), the two outputs
 * exchange visible_workspace. The unique-visible-workspace invariant
 * is exactly what must hold after the swap.
 */
static void scenario_workspace_swap_two_outputs(void)
{
	struct Output *out_a;
	struct Output *out_b;
	struct Seat *seat;

	harness_init(NULL);

	out_a = harness_add_output(0, 0, 1920, 1080);
	out_b = harness_add_output(1920, 0, 1920, 1080);
	seat = harness_add_seat(out_a);
	harness_cycle();
	assert(out_a->visible_workspace == 0);
	assert(out_b->visible_workspace == 1);

	harness_add_window("kitty");
	harness_cycle();
	assert(harness_count_ws_windows(0) == 1);

	/* Move pointer to the second output, switch ws there to ws 1. */
	river_seat_listener.pointer_position(seat, NULL,
					     out_b->x + out_b->width / 2,
					     out_b->y + out_b->height / 2);
	harness_press(seat, ACTION_WORKSPACE, "1");
	harness_cycle();
	/* out_b takes ws 0, out_a swaps onto ws 1. */
	assert(out_b->visible_workspace == 0);
	assert(out_a->visible_workspace == 1);
}

/*
 * BSP layout: a single window covers the active area; two windows
 * split into two non-overlapping rects; four windows produce four
 * non-overlapping rects. The harness_check_all calls inside
 * harness_cycle exercise no-overlap and tiles-inside-output
 * automatically; this scenario is here so the BSP code path is
 * actually run.
 */
static void scenario_bsp_layout(void)
{
	struct Window *windows[4];
	struct Output *out;
	struct Seat *seat;
	size_t i;

	harness_init("(defcfg (layout bsp))");

	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	for (i = 0; i < 4; i++) {
		windows[i] = harness_add_window("kitty");
		harness_cycle();
		assert(harness_count_ws_windows(0) == (int)(i + 1));
	}
	(void)seat;
	(void)windows;
}

/*
 * Hot-reload via ACTION_RELOAD_CONFIG. Schedules a reload; the next
 * cycle picks up g_reload_pending and runs reload_now in
 * wm_handle_manage_start. With nothing staged, the reload is a
 * no-op but the schedule path itself must not corrupt state - the
 * structural invariants run after every cycle and assert that.
 */
static void scenario_reload_action(void)
{
	struct Output *out;
	struct Seat *seat;

	harness_init(NULL);

	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_add_window("kitty");
	harness_cycle();

	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	/* Pending after press+cycle 1: action dispatch fired schedule_reload
	 * during seat_manage which is past manage_start, so reload_now will
	 * not run until the next cycle. */
	assert(g_reload_pending == 1);
	(void)out;
}

/*
 * Hot-reload mid-cycle: switch layout from TALL to BSP via
 * ACTION_RELOAD_CONFIG with a staged config that flips
 * (defcfg (layout bsp)). After the second cycle - one to dispatch
 * the action and schedule the reload, one to actually run reload_now
 * at manage_start - every workspace's layout has been re-init'd to
 * BSP, the existing windows have been inserted as fresh leaves, and
 * the BSP invariants hold.
 */
static void scenario_hot_reload_layout_swap(void)
{
	struct LayoutBspStats stats;
	struct Output *out;
	struct Seat *seat;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_add_window("a");
	harness_add_window("b");
	harness_add_window("c");
	harness_cycle();
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_TALL);

	harness_stage_reload("(defcfg (layout bsp))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	assert(g_reload_pending == 1);
	/* Staging is still set; reload_now has not run yet. */
	assert(g_staged_config != NULL);

	harness_cycle();
	/* Now reload_now has run: g_staged_config consumed, layout swapped. */
	assert(g_reload_pending == 0);
	assert(g_staged_config == NULL);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_BSP);

	/* Existing 3 windows became BSP leaves on the post-reload apply. */
	layout_bsp_stats(workspace_layout(&wm.workspaces[0]), &stats);
	assert(stats.well_formed);
	assert(stats.n_leaves == 3);
	assert(stats.n_splits == 2);
	(void)out;
}

/*
 * Hot-reload of the *binding table*: the action dispatched here was
 * registered against the old config. Reload tears down per-seat
 * bindings and re-registers from the new config; the freshly
 * dispatched action should still complete cleanly because the
 * pending_action was action_dup'd at press time (independent of
 * binding lifetime). The new config has no bindings at all but the
 * scheduled reload itself must complete.
 */
static void scenario_hot_reload_clears_bindings(void)
{
	struct Output *out;
	struct Seat *seat;

	harness_init(NULL); /* default config has 43 xkb bindings */
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();
	assert(!wl_list_empty(&seat->xkb_bindings));

	/* Stage a config with zero bindings. */
	harness_stage_reload("(defcfg (layout tall))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(wl_list_empty(&seat->xkb_bindings));
	assert(g_config->n_xkb == 0);
	(void)out;
}

/*
 * Output unplug + replug. Removing an output pushes its
 * visible_workspace onto wm.orphan_ws (LIFO); re-plugging pops the
 * top entry so the previously-displayed workspace is restored on the
 * new output. Windows on the orphaned workspace stay attached to
 * that workspace; they just become invisible while no output is
 * showing it.
 */
static void scenario_output_unplug_replug(void)
{
	struct Output *out_b;
	struct Output *out_a;
	struct Seat *seat;

	harness_init(NULL);
	out_a = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out_a);
	harness_cycle();
	assert(out_a->visible_workspace == 0);

	/* Move to ws 5 (1-based) so the orphan stack records 4 (0-based). */
	harness_press(seat, ACTION_WORKSPACE, "5");
	harness_cycle();
	assert(out_a->visible_workspace == 4);

	harness_add_window("kitty");
	harness_cycle();
	assert(harness_count_ws_windows(4) == 1);

	harness_remove_output(out_a);
	harness_cycle();
	/* out_a is freed; do not dereference. */
	assert(wl_list_empty(&wm.outputs));
	assert(seat->output == NULL);
	assert(seat->focused == NULL);
	assert(wm.n_orphan_ws == 1);
	assert(wm.orphan_ws[0] == 4);
	/* Window stays on its workspace; the workspace just has no output. */
	assert(harness_count_ws_windows(4) == 1);

	out_b = harness_add_output(0, 0, 1920, 1080);
	harness_cycle();
	assert(out_b->visible_workspace == 4);
	assert(wm.n_orphan_ws == 0);
	assert(harness_count_ws_windows(4) == 1);
}

/*
 * Two-output unplug of the focused output. As in xmonad's rescreen the
 * survivor takes over the focused workspace. A fresh output then gets
 * the workspace the survivor gave up.
 */
static void scenario_output_unplug_two_outputs(void)
{
	struct Output *out_b;
	struct Output *out_c;
	struct Output *out_a;
	struct Seat *seat;

	harness_init(NULL);
	out_a = harness_add_output(0, 0, 1920, 1080);
	out_b = harness_add_output(1920, 0, 1920, 1080);
	seat = harness_add_seat(out_a);
	harness_cycle();
	assert(out_a->visible_workspace == 0);
	assert(out_b->visible_workspace == 1);

	harness_remove_output(out_a);
	harness_cycle();
	assert(out_b->visible_workspace == 0);
	assert(seat->output == NULL);

	out_c = harness_add_output(0, 0, 1920, 1080);
	harness_cycle();
	assert(out_c->visible_workspace == 1);
	assert(out_b->visible_workspace == 0);
	assert(wm.n_orphan_ws == 0);
}

static void scenario_output_dock_new_before_removed(void)
{
	struct Output *out_a, *out_b;
	struct Seat *seat;
	struct Window *w;

	harness_init(NULL);
	out_a = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out_a);
	w = harness_add_window("w");
	harness_cycle();
	assert(w->workspace_index == 0 && w->visible_desired);

	out_b = harness_add_output(1920, 0, 2560, 1440);
	harness_cycle();
	assert(out_b->visible_workspace == 1);
	harness_remove_output(out_a);
	harness_cycle();
	harness_cycle();
	assert(out_b->visible_workspace == 0);
	assert(w->visible_desired);
	assert(seat->output == NULL || seat->output == out_b);
	harness_teardown();

	harness_init(NULL);
	out_a = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out_a);
	harness_cycle();
	out_b = harness_add_output(1920, 0, 2560, 1440);
	harness_remove_output(out_a);
	harness_cycle();
	assert(out_b->visible_workspace == 0);
}

/*
 * Pointer drag move: seat_pointer_move kicks the seat into SEAT_OP_MOVE
 * and promotes the target to floating; op_delta from the seat listener
 * supplies pointer movement, and seat_render in each render cycle
 * commits the new position. op_release ends the drag and the op
 * machinery clears.
 *
 * Bypasses the keybinding -> ACTION_MOVE path because that route
 * additionally needs seat->hovered set up via pointer_enter; the
 * client's pointer_move_requested event, which window_manage hands
 * to seat_pointer_move inside a manage sequence, is the meaningful
 * one to test.
 */
static void scenario_drag_move(void)
{
	struct Window *w;
	struct Output *out;
	struct Seat *seat;
	s32 start_x;
	s32 start_y;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	w = harness_add_window("kitty");
	harness_cycle();

	assert(!w->floating);
	start_x = w->x;
	start_y = w->y;

	river_window_listener.pointer_move_requested(w, NULL, seat->obj);
	harness_cycle();
	assert(seat->op == SEAT_OP_MOVE);
	assert(seat->op_window == w);
	assert(w->floating);

	harness_op_delta(seat, 100, 50);
	harness_cycle();
	assert(w->x == start_x + 100);
	assert(w->y == start_y + 50);

	harness_op_release(seat);
	harness_cycle();
	assert(seat->op == SEAT_OP_NONE);
	assert(seat->op_window == NULL);
	(void)out;
}

/*
 * Pointer drag resize on the bottom-right edges: width and height
 * grow by op_delta dx / dy; position stays put. The target is also
 * promoted to floating so the layout does not snap it back.
 */
static void scenario_drag_resize(void)
{
	struct Window *w;
	struct Output *out;
	struct Seat *seat;
	u32 edges;
	s32 start_w;
	s32 start_h;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	w = harness_add_window("kitty");
	harness_cycle();

	start_w = w->last_proposed_w;
	start_h = w->last_proposed_h;
	edges = RIVER_WINDOW_V1_EDGES_BOTTOM | RIVER_WINDOW_V1_EDGES_RIGHT;
	river_window_listener.pointer_resize_requested(w, NULL, seat->obj,
						       edges);
	harness_cycle();
	assert(seat->op == SEAT_OP_RESIZE);
	assert(seat->op_edges == edges);
	assert(w->floating);

	harness_op_delta(seat, 60, 40);
	harness_cycle();
	/* manage_start's SEAT_OP_RESIZE branch propose_dimensions'd the new
	 * size before the render. */
	assert(w->last_proposed_w == start_w + 60);
	assert(w->last_proposed_h == start_h + 40);

	harness_op_release(seat);
	harness_cycle();
	assert(seat->op == SEAT_OP_NONE);
	(void)out;
}

/*
 * defrules: (float ...) sets a window's floating flag, with :center
 * placing it at the output's centre on first appearance; (ignore ...)
 * sets the ignored flag so the window is skipped by layout, visibility,
 * borders, and focus. A third "kitty" window with no matching rule
 * stays as a normal tile.
 *
 * After one cycle:
 *   - Pavucontrol: floating, centered (x = (out.width - fw)/2, etc.)
 *   - desktop: ignored, proposed 0x0 so the client can map itself
 *   - dlg: floating without :center, proposed 0x0, centered once the
 *     client reports its own size
 *   - kitty: tiled, fills the inset-adjusted available area on its own
 *
 * The harness_check_tile_extent invariant catches the third condition
 * implicitly (kitty as the only tile must reach all four edges); this
 * scenario adds the explicit floating / ignored assertions.
 */
static void scenario_float_rules(void)
{
	struct Window *kitty;
	struct Window *panel;
	struct Window *pavu;
	struct Output *out;
	struct Window *dlg;
	struct Seat *seat;
	s32 expected_fx;
	s32 expected_fy;
	s32 expected_fw;
	s32 expected_fh;

	harness_init("(defrules\n"
		     "  (float (app-id \"Pavucontrol\") :center true)\n"
		     "  (float (app-id \"dlg\"))\n"
		     "  (ignore (app-id \"desktop\")))");

	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	pavu = harness_add_window("Pavucontrol");
	panel = harness_add_window("desktop");
	kitty = harness_add_window("kitty");
	dlg = harness_add_window("dlg");
	harness_cycle();
	harness_cycle();

	assert(dlg->floating);
	assert(dlg->dimensions_synced);
	assert(dlg->last_proposed_w == 0 && dlg->last_proposed_h == 0);
	assert(dlg->width == 640 && dlg->height == 480);
	assert(dlg->x == out->x + (out->width - 640) / 2);
	assert(dlg->y == out->y + (out->height - 480) / 2);
	assert(!dlg->float_needs_place);
	assert(panel->dimensions_synced);
	assert(panel->last_proposed_w == 0 && panel->last_proposed_h == 0);

	assert(pavu->floating);
	assert(!pavu->ignored);
	assert(panel->ignored);
	assert(!kitty->floating);
	assert(!kitty->ignored);

	/* :center true with no prior dimensions falls back to width/2 x
	 * height/2 (see center_window in wm.c). */
	expected_fw = out->width / 2;
	expected_fh = out->height / 2;
	expected_fx = out->x + (out->width - expected_fw) / 2;
	expected_fy = out->y + (out->height - expected_fh) / 2;
	assert(pavu->x == expected_fx);
	assert(pavu->y == expected_fy);
	assert(pavu->last_proposed_w == expected_fw);
	assert(pavu->last_proposed_h == expected_fh);

	/* Focus must land on a non-ignored window. seat_focus's default
	 * pick walks ws->windows skipping ignored, so focused settles on
	 * the most recently added non-ignored window: dlg. */
	assert(seat->focused == dlg);
	(void)kitty;
}

/*
 * Dynamic scratchpad: stash the focused window onto the scratch slot,
 * then toggle again to bring it back to the visible workspace as a
 * float. Asserts the round trip leaves the window on the original
 * workspace, focused, with float_override latched to FORCE_FLOAT.
 */
static void scenario_dynamic_scratchpad_toggle(void)
{
	struct Window *w;
	struct Output *out;
	struct Seat *seat;
	int scratch_idx;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	w = harness_add_window("kitty");
	harness_cycle();

	scratch_idx = wm_scratch_ws_index();
	assert(scratch_idx > 0);
	assert(w->workspace_index == 0);
	assert(seat->focused == w);
	assert(harness_count_ws_windows(scratch_idx) == 0);

	harness_press(seat, ACTION_TOGGLE_SCRATCHPAD, NULL);
	harness_cycle();
	assert(w->workspace_index == scratch_idx);
	assert(w->is_scratchpad);
	assert(w->scratch_name == NULL);
	assert(w->floating);
	assert(harness_count_ws_windows(0) == 0);
	assert(harness_count_ws_windows(scratch_idx) == 1);
	assert(seat->focused == NULL);

	harness_press(seat, ACTION_TOGGLE_SCRATCHPAD, NULL);
	harness_cycle();
	assert(w->workspace_index == 0);
	assert(w->floating);
	assert(seat->focused == w);
	assert(harness_count_ws_windows(0) == 1);
	assert(harness_count_ws_windows(scratch_idx) == 0);
	(void)out;
}

/*
 * Named scratchpad: a defscratchpads entry matches a freshly-spawned
 * window by app_id. apply_rules labels the window and stamps the
 * configured rational-rect geometry; subsequent toggles move it
 * between the visible workspace and the scratch slot.
 */
static void scenario_named_scratchpad(void)
{
	struct Window *w;
	struct Output *out;
	struct Seat *seat;
	int scratch_idx;

	harness_init("(defscratchpads\n"
		     "  (term (spawn \"alacritty --class scratch\")\n"
		     "        (app-id \"scratch\")\n"
		     "        :geometry (0.1 0.1 0.8 0.8)))");
	out = harness_add_output(0, 0, 1000, 800);
	seat = harness_add_seat(out);
	w = harness_add_window("scratch");
	harness_cycle();

	scratch_idx = wm_scratch_ws_index();
	assert(w->is_scratchpad);
	assert(w->scratch_name != NULL && strcmp(w->scratch_name, "term") == 0);
	assert(w->floating);
	/* Geometry: 10% offsets, 80% size. */
	assert(w->x == 100);
	assert(w->y == 80);
	assert(w->last_proposed_w == 800);
	assert(w->last_proposed_h == 640);

	/* Toggle stash, then unstash; geometry stays put. */
	harness_press(seat, ACTION_NAMED_SCRATCHPAD, "term");
	harness_cycle();
	assert(w->workspace_index == scratch_idx);
	assert(harness_count_ws_windows(scratch_idx) == 1);

	harness_press(seat, ACTION_NAMED_SCRATCHPAD, "term");
	harness_cycle();
	assert(w->workspace_index == 0);
	assert(seat->focused == w);
	assert(w->floating);
	(void)out;
}

/*
 * Scratchpad windows on the stash slot must not be visible. Catches a
 * regression where update_visibility incorrectly marks a stashed pad
 * as visible_desired (e.g. a stale visible_workspace pointer or an
 * iterator that includes the scratch ws among "shown" workspaces).
 */
static void scenario_scratchpad_invisible_when_stashed(void)
{
	struct Window *w;
	struct Output *out;
	struct Seat *seat;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	w = harness_add_window("kitty");
	harness_cycle();

	harness_press(seat, ACTION_TOGGLE_SCRATCHPAD, NULL);
	harness_cycle();
	assert(w->workspace_index == wm_scratch_ws_index());
	assert(!w->visible_desired);
	(void)out;
}

static size_t harness_count_xkb_bindings(struct Seat *seat)
{
	struct XkbBinding *b;
	size_t n = 0;

	wl_list_for_each(b, &seat->xkb_bindings, link)
		n++;
	return n;
}

/*
 * Discrete keyboard resize on a floating window: each (resize EDGE
 * DELTA) press grows / shrinks the focused float in pixel-space along
 * the requested edge. Tiled and ignored windows are no-ops.
 */
static void scenario_resize_delta_float(void)
{
	struct Window *w;
	struct Output *out;
	struct Seat *seat;
	s32 start_x, start_y, start_w, start_h;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	w = harness_add_window("kitty");
	harness_cycle();

	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	assert(w->floating);
	start_x = w->x;
	start_y = w->y;
	start_w = w->last_proposed_w;
	start_h = w->last_proposed_h;

	/* Right edge +30: width grows, x untouched. */
	harness_press(seat, ACTION_RESIZE_DELTA, "right 30");
	harness_cycle();
	assert(w->x == start_x);
	assert(w->last_proposed_w == start_w + 30);
	assert(w->last_proposed_h == start_h);

	/* Left edge +20: x moves left, width grows by another 20. */
	harness_press(seat, ACTION_RESIZE_DELTA, "left 20");
	harness_cycle();
	assert(w->x == start_x - 20);
	assert(w->last_proposed_w == start_w + 30 + 20);

	/* Down edge -10: height shrinks. */
	harness_press(seat, ACTION_RESIZE_DELTA, "down -10");
	harness_cycle();
	assert(w->y == start_y);
	assert(w->last_proposed_h == start_h - 10);
	(void)out;
}

/*
 * Submap chord: pressing M-s enters a submap with two entries (g, w).
 * The seat's xkb bindings are wiped and replaced with the two
 * submap entries; firing one of them tears the submap back down.
 * Pressing a key not in the submap arrives via ate_unbound_key, which
 * also tears down. Mirrors xmonad's XMonad.Actions.Submap behavior.
 */
static void scenario_submap_enter_dispatch_exit(void)
{
	struct Output *out;
	struct Seat *seat;
	size_t base_count;

	harness_init("(defsubmap nav\n"
		     "  (g focus-next)\n"
		     "  (w focus-prev))\n"
		     "(defkeys (A s (submap nav)))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();
	base_count = harness_count_xkb_bindings(seat);
	assert(base_count == 1);
	assert(!seat->submap_active);

	/* Enter the submap. */
	harness_press(seat, ACTION_SUBMAP, "nav");
	harness_cycle();
	assert(seat->submap_active);
	assert(harness_count_xkb_bindings(seat) == 2);

	/* Fire one of the submap's entries: single-shot teardown. */
	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	assert(!seat->submap_active);
	assert(harness_count_xkb_bindings(seat) == base_count);

	/* Re-enter, then bail out via ate_unbound_key. */
	harness_press(seat, ACTION_SUBMAP, "nav");
	harness_cycle();
	assert(seat->submap_active);
	assert(harness_count_xkb_bindings(seat) == 2);

	river_xkb_bindings_seat_listener.ate_unbound_key(seat, NULL);
	assert(seat->submap_exit_pending);
	harness_cycle();
	assert(!seat->submap_active);
	assert(harness_count_xkb_bindings(seat) == base_count);
	(void)out;
}

/*
 * Hot-reload while a submap is active: the new config no longer has
 * the submap def, but the seat must end up on the new config's normal
 * bindings, not in a half-broken state holding pointers into freed
 * SubmapDef storage.
 */
static void scenario_submap_reload_drops_submap(void)
{
	struct Output *out;
	struct Seat *seat;

	harness_init("(defsubmap nav (g focus-next))\n"
		     "(defkeys (A s (submap nav)) (A q close))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	harness_press(seat, ACTION_SUBMAP, "nav");
	harness_cycle();
	assert(seat->submap_active);

	/* Stage a new config with no submaps. */
	harness_stage_reload("(defkeys (A q close))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(g_staged_config == NULL);
	assert(!seat->submap_active);
	assert(harness_count_xkb_bindings(seat) == 1);
	(void)out;
}

static void scenario_reload_drops_stashed_named_pad(void)
{
	struct Window *w;
	struct Output *out;
	struct Seat *seat;
	int scratch_idx;

	harness_init("(defscratchpads\n"
		     "  (term (spawn \"alacritty --class scratch\")\n"
		     "        (app-id \"scratch\")))");
	out = harness_add_output(0, 0, 1000, 800);
	seat = harness_add_seat(out);
	w = harness_add_window("scratch");
	harness_cycle();
	scratch_idx = wm_scratch_ws_index();

	harness_press(seat, ACTION_NAMED_SCRATCHPAD, "term");
	harness_cycle();
	assert(w->workspace_index == scratch_idx);

	harness_stage_reload("(defkeys (A q close))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(g_staged_config == NULL);
	assert(w->scratch_name == NULL);
	assert(w->is_scratchpad);

	harness_press(seat, ACTION_TOGGLE_SCRATCHPAD, NULL);
	harness_cycle();
	assert(w->workspace_index == 0);
	assert(seat->focused == w);
	(void)out;
}

static void scenario_reload_keeps_layout_state(void)
{
	struct Workspace *ws;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout tall))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_add_window("a");
	harness_add_window("b");
	harness_add_window("c");
	harness_cycle();
	ws = &wm.workspaces[0];

	harness_press(seat, ACTION_INC_MASTER, NULL);
	harness_cycle();
	assert(workspace_layout(ws)->u.tall.nmaster == 2);

	harness_stage_reload("(defcfg (layout tall) (borders :width 5))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(g_staged_config == NULL);
	assert(workspace_layout(ws)->u.tall.nmaster == 2);

	harness_stage_reload("(defcfg (layouts tall bsp))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(ws->n_layouts == 2);
	assert(workspace_layout(ws)->u.tall.nmaster == 1);
	(void)out;
}

static void scenario_layer_shell_usable_area(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *a;
	struct Window *b;
	u64 before;
	u64 idle;

	harness_init(NULL);
	harness_enable_layer_shell();
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	b = harness_add_window("b");
	harness_cycle();
	assert(out->ls_output != NULL);
	assert(seat->ls_seat != NULL);
	assert(wm.layer_default == out);
	assert(a->y < 30 && b->y < 30);

	harness_set_usable(out, 0, 30, 1920, 1050);
	harness_cycle();
	assert(a->y >= 30 && b->y >= 30);
	assert(a->y + a->last_proposed_h <= 1080);
	assert(b->y + b->last_proposed_h <= 1080);

	harness_interact(seat, a);
	harness_cycle();
	assert(a->clip_synced && a->last_clip_w < 1920);
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(a->fullscreen && a->x == 0 && a->y == 30);
	assert(a->last_proposed_w == 1920 && a->last_proposed_h == 1050);
	assert(a->last_clip_w == 1920 && a->last_clip_h == 1050);
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(!a->fullscreen);
	assert(a->last_clip_w == a->last_bounds_w);
	assert(a->last_clip_h == a->last_bounds_h);

	harness_layer_focus(seat, LAYER_FOCUS_NON_EXCLUSIVE);
	harness_cycle();
	assert(seat->layer_focus == LAYER_FOCUS_NON_EXCLUSIVE);
	before = harness_request_count_get();
	harness_cycle();
	idle = harness_request_count_get() - before;
	before = harness_request_count_get();
	seat->interacted = seat->focused;
	harness_cycle();
	assert(harness_request_count_get() - before > idle);

	harness_layer_focus(seat, LAYER_FOCUS_EXCLUSIVE);
	harness_cycle();
	harness_layer_focus(seat, LAYER_FOCUS_NONE);
	assert(seat->refocus_pending);
	harness_cycle();
	assert(!seat->refocus_pending);
	assert(seat->layer_focus == LAYER_FOCUS_NONE);
}

static void scenario_refused_close_recovers(void)
{
	struct Window *victim;
	struct Window *other;
	struct Output *out;
	struct Seat *seat;
	struct Window *a;
	struct Window *b;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	b = harness_add_window("b");
	harness_cycle();
	victim = seat->focused;
	assert(victim == a || victim == b);
	other = (victim == a) ? b : a;

	harness_press(seat, ACTION_CLOSE, NULL);
	harness_cycle();
	assert(victim->requested_close);
	assert(seat->focused == other);

	victim->close_requested_ns -= 2000000000ULL;
	harness_cycle();
	assert(!victim->requested_close);

	harness_press(seat, ACTION_CLOSE, NULL);
	harness_cycle();
	assert(other->requested_close);
	assert(seat->focused == victim);
	harness_interact(seat, other);
	assert(!other->requested_close);
	harness_cycle();
	assert(seat->focused == other);
	(void)out;
}

static void resizable_assert_column_fits(struct Window **ws, size_t n,
					 const struct Output *out)
{
	for (size_t i = 0; i < n; i++) {
		assert(ws[i]->last_proposed_h > 0);
		assert(ws[i]->y >= out->y);
		assert(ws[i]->y + ws[i]->last_proposed_h <=
		       out->y + out->height);
	}
}

static void scenario_resizable_tall_held_expand(void)
{
	struct Window *w[4];
	struct Output *out;
	struct Seat *seat;
	int i;

	harness_init("(defcfg (layout resizable-tall) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	w[0] = harness_add_window("a");
	w[1] = harness_add_window("b");
	w[2] = harness_add_window("c");
	w[3] = harness_add_window("d");
	harness_cycle();

	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	for (i = 0; i < 80; i++) {
		harness_press(seat, ACTION_LAYOUT_VERB, "expand-cell");
		harness_cycle();
	}
	resizable_assert_column_fits(w, 4, out);

	harness_press(seat, ACTION_INC_MASTER, NULL);
	harness_cycle();
	resizable_assert_column_fits(w, 4, out);

	harness_close_window(w[3]);
	harness_cycle();
	for (i = 0; i < 10; i++) {
		harness_press(seat, ACTION_LAYOUT_VERB, "shrink-cell");
		harness_cycle();
	}
	resizable_assert_column_fits(w, 3, out);
}

static void scenario_three_columns_no_master_single(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *a;

	harness_init("(defcfg (layout three-columns) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	harness_cycle();
	harness_press(seat, ACTION_DEC_MASTER, NULL);
	harness_cycle();
	assert(workspace_layout(&wm.workspaces[0])->u.tall.nmaster == 0);
	assert(a->x == out->x);
	assert(a->last_proposed_w == out->width);
}

static void scenario_float_follows_output(void)
{
	struct Output *a;
	struct Output *b;
	struct Seat *seat;
	struct Window *f;
	int ws_a;
	int ws_b;

	harness_init("(defrules (float (app-id \"f\")))");
	a = harness_add_output(0, 0, 1920, 1080);
	b = harness_add_output(1920, 0, 1280, 720);
	seat = harness_add_seat(a);
	f = harness_add_window("f");
	harness_cycle();
	harness_cycle();
	ws_a = a->visible_workspace;
	ws_b = b->visible_workspace;
	assert(f->workspace_index == ws_a);
	assert(f->x >= a->x && f->x + f->width <= a->x + a->width);

	harness_pointer_to(seat, b);
	harness_cycle();
	{
		char arg[16];

		snprintf(arg, sizeof(arg), "%d", ws_a + 1);
		harness_press(seat, ACTION_WORKSPACE, arg);
	}
	harness_cycle();
	assert(b->visible_workspace == ws_a);
	assert(f->x >= b->x && f->x + f->width <= b->x + b->width);
	assert(f->y >= b->y && f->y + f->height <= b->y + b->height);

	river_window_listener.pointer_move_requested(f, NULL, seat->obj);
	harness_cycle();
	harness_op_delta(seat, -1920, 0);
	harness_cycle();
	harness_op_release(seat);
	harness_cycle();
	harness_cycle();
	assert(seat->op == SEAT_OP_NONE);
	assert(f->workspace_index == a->visible_workspace);
	assert(f->x >= a->x && f->x < a->x + a->width);
	(void)ws_b;
}

static void scenario_fullscreen_float_restore_and_focus(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *f;
	struct Window *t;
	s32 fx, fy, fw, fh;

	harness_init("(defrules (float (app-id \"f\")))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	t = harness_add_window("t");
	f = harness_add_window("f");
	harness_cycle();
	harness_cycle();
	assert(f->floating && f->width == 640);
	fx = f->x;
	fy = f->y;
	fw = f->width;
	fh = f->height;

	harness_interact(seat, f);
	harness_cycle();
	assert(seat->focused == f);
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	harness_cycle();
	assert(f->fullscreen && !f->fullscreen_informed);
	assert(f->width == out->width && f->height == out->height);

	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	assert(seat->focused == f);
	harness_interact(seat, t);
	harness_cycle();
	assert(seat->focused == f);

	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	harness_cycle();
	assert(!f->fullscreen && !f->fullscreen_informed);
	assert(f->x == fx && f->y == fy);
	assert(f->width == fw && f->height == fh);
	(void)t;
}

static void scenario_output_replaced_in_one_batch(void)
{
	struct Output *a;
	struct Output *b;
	struct Seat *seat;
	struct Window *w;

	harness_init(NULL);
	a = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(a);
	w = harness_add_window("w");
	harness_cycle();
	assert(a->visible_workspace == 0);

	harness_remove_output(a);
	b = harness_add_output(0, 0, 2560, 1440);
	harness_cycle();
	assert(b->visible_workspace == 0);
	assert(w->visible_desired);
	(void)seat;
}

static void scenario_input_devices_configured(void)
{
	void *tp;
	u64 before;

	harness_init("(defcfg (repeat :rate 50 :delay 300)\n"
		     "        (touchpad :tap true :natural-scroll false))");
	harness_cycle();

	before = harness_request_count_get();
	harness_input_device(RIVER_INPUT_DEVICE_V1_TYPE_KEYBOARD);
	assert(harness_request_count_get() == before + 1);
	assert(harness_last_opcode() == RIVER_INPUT_DEVICE_V1_SET_REPEAT_INFO);

	before = harness_request_count_get();
	harness_input_device(RIVER_INPUT_DEVICE_V1_TYPE_POINTER);
	assert(harness_request_count_get() == before);

	harness_track_iface("river_libinput_result_v1");
	tp = harness_libinput_device(2);
	assert(harness_tracked_count() == 2);
	harness_finish_tracked();
	harness_libinput_defaults(
		tp, RIVER_LIBINPUT_DEVICE_V1_TAP_STATE_DISABLED,
		RIVER_LIBINPUT_DEVICE_V1_NATURAL_SCROLL_STATE_DISABLED);

	harness_track_iface("river_libinput_result_v1");
	harness_libinput_device(0);
	assert(harness_tracked_count() == 0);
	harness_track_iface(NULL);

	harness_stage_reload("(defkeys (A q close))\n"
			     "(defcfg (repeat :rate 30 :delay 200)\n"
			     "        (touchpad :dwt true))");
	harness_track_iface("river_libinput_result_v1");
	before = harness_request_count_get();
	reload_now();
	assert(harness_request_count_get() > before);
	assert(harness_tracked_count() == 3);
	harness_finish_tracked();

	harness_stage_reload("(defkeys (A q close))");
	harness_track_iface("river_libinput_result_v1");
	reload_now();
	assert(harness_tracked_count() == 0);
	harness_track_iface(NULL);
	assert(harness_last_opcode() == RIVER_INPUT_DEVICE_V1_SET_REPEAT_INFO);
}

static void scenario_move_to_workspace_follow(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *a;
	struct Window *b;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	b = harness_add_window("b");
	harness_cycle();
	harness_interact(seat, b);
	harness_cycle();
	assert(seat->focused == b);

	harness_press(seat, ACTION_MOVE_TO_WORKSPACE_FOLLOW, "3");
	harness_cycle();
	assert(b->workspace_index == 2);
	assert(out->visible_workspace == 2);
	assert(seat->focused == b);
	assert(b->visible_desired && !a->visible_desired);

	harness_press(seat, ACTION_MOVE_TO_WORKSPACE, "1");
	harness_cycle();
	assert(b->workspace_index == 0);
	assert(out->visible_workspace == 2);
	assert(seat->focused != b);
}

static void swap_check(struct Seat *seat, enum ActionKind kind)
{
	struct Workspace *ws = &wm.workspaces[0];
	struct Window *w;
	s32 x[8];
	s32 y[8];
	int i;

	i = 0;
	wl_list_for_each(w, &ws->windows, workspace_link) {
		x[i] = w->x;
		y[i] = w->y;
		i++;
	}
	harness_key_press(seat, kind);
	harness_cycle();
	harness_key_release(seat, kind);
	harness_cycle();
	i = 0;
	wl_list_for_each(w, &ws->windows, workspace_link) {
		assert(w->x == x[i] && w->y == y[i]);
		i++;
	}
}

static void scenario_bsp_swap_moves_tiles(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *a;
	struct Window *b;
	struct Window *c;
	s32 cx;
	s32 cy;

	harness_init("(defcfg (layout bsp))\n"
		     "(defkeys (A-S j swap-next) (A-S k swap-prev))");
	out = harness_add_output(0, 0, 2560, 1440);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	harness_cycle();
	b = harness_add_window("b");
	harness_cycle();
	c = harness_add_window("c");
	harness_cycle();
	harness_interact(seat, c);
	harness_cycle();
	assert(seat->focused == c);
	cx = c->x;
	cy = c->y;
	assert(!(a->x == b->x && a->y == b->y));

	swap_check(seat, ACTION_SWAP_NEXT);
	assert(c->x != cx || c->y != cy);
	swap_check(seat, ACTION_SWAP_NEXT);
	swap_check(seat, ACTION_SWAP_NEXT);
	swap_check(seat, ACTION_SWAP_PREV);
	swap_check(seat, ACTION_SWAP_PREV);
	swap_check(seat, ACTION_SWAP_PREV);
	assert(c->x == cx && c->y == cy);
	assert(seat->focused == c);
}

static struct Window *add_window_id(const char *app_id, const char *id)
{
	struct Window *w;

	w = harness_add_window(app_id);
	river_window_listener.identifier(w, NULL, id);
	return w;
}

static void restore_tmp_path(char *path)
{
	int fd;

	strcpy(path, "/tmp/nomonad-harness-XXXXXX");
	fd = mkstemp(path);
	assert(fd >= 0);
	close(fd);
}

static void scenario_restart_roundtrip(void)
{
	static const char *const ids[] = { "ida", "idb", "idc", "idd" };
	const char *cfg = "(defcfg (layouts tall bsp))";
	struct Window *win[4];
	struct Output *out0, *out1;
	s32 x[4], y[4], w[4], h[4];
	char order[4][8];
	struct Seat *seat;
	struct Window *it;
	char path[64];
	int n;

	harness_init(cfg);
	out0 = harness_add_output(0, 0, 1920, 1080);
	out1 = harness_add_output(1920, 0, 1920, 1080);
	seat = harness_add_seat(out0);
	for (int i = 0; i < 4; i++)
		win[i] = add_window_id(ids[i], ids[i]);
	harness_cycle();
	harness_press(seat, ACTION_INC_MASTER, NULL);
	harness_cycle();
	harness_press(seat, ACTION_NEXT_LAYOUT, NULL);
	harness_cycle();
	harness_interact(seat, win[2]);
	harness_cycle();
	harness_press(seat, ACTION_MOVE_TO_WORKSPACE, "3");
	harness_cycle();
	harness_interact(seat, win[3]);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	harness_cycle();
	harness_pointer_to(seat, out1);
	harness_cycle();
	harness_press(seat, ACTION_WORKSPACE, "3");
	harness_cycle();
	harness_interact(seat, win[2]);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	harness_pointer_to(seat, out0);
	harness_interact(seat, win[0]);
	harness_cycle();
	for (int i = 0; i < 3; i++) {
		harness_press(seat, ACTION_LAYOUT_VERB, "expand-r");
		harness_cycle();
	}
	harness_cycle();
	assert(win[0]->width != win[1]->width);
	assert(out1->visible_workspace == 2 && win[2]->fullscreen);
	assert(win[3]->floating && seat->focused == win[0]);
	assert(wm.workspaces[0].current_layout_index == 1);
	for (int i = 0; i < 4; i++) {
		x[i] = win[i]->x;
		y[i] = win[i]->y;
		w[i] = win[i]->width;
		h[i] = win[i]->height;
	}
	n = 0;
	wl_list_for_each(it, &wm.workspaces[0].windows, workspace_link)
		snprintf(order[n++], sizeof(order[0]), "%s", it->identifier);
	assert(n == 3);

	restore_tmp_path(path);
	assert(restore_save(path) == 0);
	harness_teardown();

	harness_init(cfg);
	out0 = harness_add_output(0, 0, 1920, 1080);
	out1 = harness_add_output(1920, 0, 1920, 1080);
	seat = harness_add_seat(out0);
	for (int i = 3; i >= 0; i--)
		win[i] = add_window_id("other", ids[i]);
	assert(restore_load(path) == 0);
	assert(access(path, F_OK) != 0);
	harness_cycle();
	harness_cycle();

	assert(win[0]->workspace_index == 0 && win[1]->workspace_index == 0);
	assert(win[2]->workspace_index == 2 && win[3]->workspace_index == 0);
	assert(out0->visible_workspace == 0 && out1->visible_workspace == 2);
	assert(win[2]->fullscreen && win[3]->floating);
	assert(wm.workspaces[0].current_layout_index == 1);
	assert(wm.workspaces[0].layouts[0].u.tall.nmaster == 2);
	assert(seat->focused == win[0] && seat->output == out0);
	n = 0;
	wl_list_for_each(it, &wm.workspaces[0].windows, workspace_link)
		assert(strcmp(it->identifier, order[n++]) == 0);
	for (int i = 0; i < 4; i++) {
		if (i == 2)
			continue;
		assert(win[i]->x == x[i] && win[i]->y == y[i]);
		assert(win[i]->width == w[i] && win[i]->height == h[i]);
	}
}

static void scenario_restart_saves_before_stop(void)
{
	static char *argv[] = { "harness", NULL };
	struct Window *a, *b;
	struct Output *out;
	struct Seat *seat;
	const char *path;
	char copy[256];

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = add_window_id("a", "ida");
	b = add_window_id("b", "idb");
	harness_cycle();
	harness_interact(seat, b);
	harness_cycle();
	harness_press(seat, ACTION_MOVE_TO_WORKSPACE, "4");
	harness_cycle();
	assert(b->workspace_index == 3);

	restore_set_argv(argv);
	assert(restore_state_path() == NULL);
	harness_press(seat, ACTION_RESTART, NULL);
	harness_cycle();
	path = restore_state_path();
	assert(path != NULL && path[0] != '\0');
	snprintf(copy, sizeof(copy), "%s", path);
	harness_close_window(a);
	harness_close_window(b);
	harness_remove_output(out);
	assert(!restore_take_request());
	harness_teardown();

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	b = add_window_id("b", "idb");
	a = add_window_id("a", "ida");
	assert(restore_load(copy) == 0);
	harness_cycle();
	assert(a->workspace_index == 0 && b->workspace_index == 3);
	assert(seat->focused == a);
}

static void scenario_floats_stay_above_tiles(void)
{
	struct Window *t1, *t2, *f1, *f2;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout bsp))\n"
		     "(defrules (float (app-id \"f\")))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	t1 = harness_add_window("t");
	f1 = harness_add_window("f");
	f2 = harness_add_window("f");
	t2 = harness_add_window("t");
	harness_cycle();
	harness_cycle();
	assert(f1->floating && f2->floating && !t1->floating);
	harness_interact(seat, f2);
	harness_cycle();
	harness_interact(seat, f1);
	harness_cycle();
	assert(f1->raise_seq > f2->raise_seq);
	harness_interact(seat, t1);
	harness_cycle();
	assert(seat->focused == t1);
	assert(f2->raise_seq > t1->raise_seq);
	assert(f1->raise_seq > f2->raise_seq);
	harness_interact(seat, t2);
	harness_cycle();
	assert(f2->raise_seq > t2->raise_seq);
	assert(f1->raise_seq > f2->raise_seq);

	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(t2->fullscreen);
	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	harness_press(seat, ACTION_WORKSPACE, "1");
	harness_cycle();
	assert(seat->focused == t2 && t2->raise_seq > f1->raise_seq);
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(!t2->fullscreen && seat->focused == t2);
	assert(f2->raise_seq > t2->raise_seq);
	assert(f1->raise_seq > f2->raise_seq);

	harness_interact(seat, f1);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	harness_cycle();
	assert(!f1->floating && f2->floating);
	assert(f2->raise_seq > f1->raise_seq);
}

static void ctl_pump(int rounds)
{
	struct pollfd pfds[16];
	size_t n;

	for (int i = 0; i < rounds; i++) {
		n = ctl_fill_pollfds(pfds, 16);
		(void)poll(pfds, n, 5);
		ctl_handle_events(pfds, n);
		harness_cycle();
	}
}

static void scenario_ctl_socket(void)
{
	static const char req[] = "list\n"
				  "focus-window idb\n"
				  "(workspace 3)\n"
				  "bogus\n"
				  "focus-window nope\n"
				  "close-window idc\n";
	struct sockaddr_un addr;
	struct Window *a, *b, *c;
	char dir[] = "/tmp/nmh.XXXXXX";
	struct Output *out;
	struct Seat *seat;
	char reply[4096];
	size_t len = 0;
	ssize_t n;
	int fd;

	assert(mkdtemp(dir) != NULL);
	setenv("XDG_RUNTIME_DIR", dir, 1);
	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = add_window_id("a", "ida");
	b = add_window_id("b", "idb");
	c = add_window_id("c", "idc");
	harness_cycle();
	assert(ctl_init(NULL) == 0);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/nomonad-ctl.sock",
		 dir);
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(write(fd, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1));
	shutdown(fd, SHUT_WR);
	ctl_pump(1);
	assert(seat->focused != b);
	ctl_pump(1);
	assert(seat->focused == b && out->visible_workspace == 0);
	ctl_pump(1);
	assert(out->visible_workspace == 2);
	ctl_pump(12);
	while ((n = read(fd, reply + len, sizeof(reply) - 1 - len)) > 0)
		len += (size_t)n;
	reply[len] = '\0';
	close(fd);

	assert(strstr(reply, "ida\t1\t") != NULL);
	assert(strstr(reply, "idb\t1\t") != NULL);
	assert(strstr(reply, "\t0\tc\t") != NULL);
	assert(strstr(reply, "ok\nok\nok\nerror ") != NULL);
	assert(strstr(reply, "error no such window\nok\n") != NULL);
	assert(out->visible_workspace == 2);
	assert(c->requested_close && !a->requested_close);
	ctl_shutdown();
	assert(access(addr.sun_path, F_OK) != 0);
	rmdir(dir);
}

static void scenario_focus_last_from_empty_ws(void)
{
	struct Window *a, *b;
	struct Output *out;
	struct Seat *seat;

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	b = harness_add_window("b");
	harness_cycle();
	harness_interact(seat, a);
	harness_cycle();
	harness_interact(seat, b);
	harness_cycle();
	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	assert(seat->focused == NULL && out->visible_workspace == 1);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->focused == b && out->visible_workspace == 0);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->focused == a);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->focused == b);
}

static void scenario_float_lows(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *t, *f, *g;

	harness_init("(defcfg (layout bsp))\n"
		     "(defrules (float (app-id \"f\")))");
	harness_enable_layer_shell();
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();
	harness_set_usable(out, 0, 400, 1920, 680);
	harness_cycle();
	t = harness_add_window("t");
	f = harness_add_window("f");
	harness_cycle();
	harness_cycle();
	harness_cycle();
	assert(f->floating && f->y >= 400);

	harness_interact(seat, t);
	harness_cycle();
	assert(t->bounds_synced && t->last_bounds_w > 0);
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	assert(t->floating && t->raise_seq > f->raise_seq);
	assert(t->last_bounds_w == 0 && t->last_bounds_h == 0);

	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(t->fullscreen && t->last_clip_w == 1920 &&
	       t->last_clip_h == 680);
	harness_cycle();
	harness_cycle();
	assert(t->last_clip_w == 1920 && t->last_clip_h == 680);
	g = harness_add_window("g");
	harness_cycle();
	assert(t->last_clip_w == 1920 && g->workspace_index == 0);
}

static void scenario_layer_default_survives_unplug(void)
{
	struct Output *a, *b;

	harness_init(NULL);
	harness_enable_layer_shell();
	a = harness_add_output(0, 0, 1920, 1080);
	(void)harness_add_seat(a);
	b = harness_add_output(1920, 0, 1920, 1080);
	harness_cycle();
	assert(wm.layer_default == a);
	harness_remove_output(a);
	harness_cycle();
	harness_cycle();
	assert(wm.layer_default == b);
}

static int ctl_connect(const char *dir)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/nomonad-ctl.sock",
		 dir);
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	return fd;
}

static void scenario_ctl_pipelined_and_stash(void)
{
	char dir[] = "/tmp/nmh.XXXXXX";
	static char reply[16384];
	struct Window *a, *pad;
	struct Output *out;
	struct Seat *seat;
	char id[64];
	size_t len = 0;
	int oks = 0;
	ssize_t n;
	int fd;

	assert(mkdtemp(dir) != NULL);
	setenv("XDG_RUNTIME_DIR", dir, 1);
	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = add_window_id("a", "ida");
	pad = add_window_id("p", "idp");
	harness_cycle();
	harness_interact(seat, pad);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_SCRATCHPAD, NULL);
	harness_cycle();
	assert(pad->workspace_index == wm_scratch_ws_index());
	assert(ctl_init(NULL) == 0);

	fd = ctl_connect(dir);
	for (int i = 0; i < 400; i++)
		memcpy(reply + i * 14, "(workspace 1)\n", 14);
	snprintf(id, sizeof(id), "focus-window idp\n");
	memcpy(reply + 400 * 14, id, strlen(id));
	len = 400 * 14 + strlen(id);
	assert(write(fd, reply, len) == (ssize_t)len);
	len = 0;
	shutdown(fd, SHUT_WR);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	for (int i = 0; i < 420; i++) {
		ctl_pump(1);
		while ((n = read(fd, reply + len, sizeof(reply) - 1 - len)) > 0)
			len += (size_t)n;
	}
	reply[len] = '\0';
	close(fd);
	for (char *p = reply; (p = strstr(p, "ok\n")) != NULL; p += 3)
		oks++;
	assert(oks == 401);
	assert(pad->workspace_index == 0 && pad->floating);
	assert(seat->focused == pad && !a->closed);
	ctl_shutdown();
	rmdir(dir);
}

static void scenario_restart_deep_bsp(void)
{
	struct Window *w[101];
	struct LayoutBspStats st;
	char path[64];
	char id[16];
	FILE *f;

	restore_tmp_path(path);
	f = fopen(path, "w");
	assert(f != NULL);
	fputs("nomonad-restore 1\nws 0 0 1\nbsp 0 0", f);
	for (int i = 0; i < 100; i++)
		fprintf(f, " S v 0.300000 L w%d", i);
	fputs(" L w100\n", f);
	fclose(f);

	harness_init("(defcfg (layout bsp))");
	(void)harness_add_seat(harness_add_output(0, 0, 1920, 1080));
	for (int i = 0; i < 101; i++) {
		snprintf(id, sizeof(id), "w%d", i);
		w[i] = add_window_id("t", id);
	}
	assert(restore_load(path) == 0);
	harness_set_invariants(false);
	harness_cycle();
	layout_bsp_stats(&wm.workspaces[0].layouts[0], &st);
	assert(st.n_leaves == 101 && st.max_depth == 101);
	assert(st.min_ratio < 0.4f && st.max_ratio < 0.4f);
	(void)w;
}

static void scenario_trace_lock(void)
{
	char dir[] = "/tmp/nmh.XXXXXX";
	char path[64], lock[80];
	FILE *f;
	pid_t pid;
	int st;

	assert(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/trace", dir);
	snprintf(lock, sizeof(lock), "%s.lock", path);
	f = recorder_open_private(path);
	assert(f != NULL);
	fputs("live", f);
	fflush(f);
	pid = fork();
	assert(pid >= 0);
	if (pid == 0)
		_exit(recorder_open_private(path) == NULL ? 0 : 1);
	assert(waitpid(pid, &st, 0) == pid);
	assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	assert(access(path, F_OK) == 0);
	fclose(f);
	unlink(path);
	unlink(lock);
	rmdir(dir);
}

static void scenario_client_fullscreen_lows(void)
{
	struct Window *a, *x, *y;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout bsp))");
	harness_enable_layer_shell();
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();
	a = harness_add_window("test");
	harness_cycle();
	river_window_listener.fullscreen_requested(a, NULL, NULL);
	harness_cycle();
	(void)harness_add_window("test");
	harness_cycle();
	harness_press(seat, ACTION_CLOSE, NULL);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	(void)harness_add_window("test");
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	harness_close_window(a);
	harness_cycle();
	harness_cycle();
	harness_teardown();

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	x = harness_add_window("x");
	y = harness_add_window("y");
	harness_cycle();
	harness_interact(seat, x);
	harness_cycle();
	assert(seat->focused == x);
	river_window_listener.fullscreen_requested(y, NULL, NULL);
	harness_cycle();
	harness_cycle();
	assert(y->fullscreen && seat->focused == y);
	assert(!x->visible_desired);

	river_window_listener.exit_fullscreen_requested(y, NULL);
	harness_cycle();
	harness_interact(seat, x);
	harness_cycle();
	y->requested_close = true;
	river_window_listener.fullscreen_requested(y, NULL, NULL);
	harness_cycle();
	assert(seat->focused == x);
}

static void scenario_sticky(void)
{
	struct Output *a, *b;
	struct Window *s, *t, *u;
	struct Seat *seat;
	char path[64];
	FILE *f;

	harness_init(NULL);
	a = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(a);
	s = add_window_id("s", "ids");
	harness_cycle();
	harness_interact(seat, s);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_STICKY, NULL);
	harness_cycle();
	assert(s->sticky && s->floating);
	t = add_window_id("t", "idt");
	harness_cycle();
	harness_interact(seat, t);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_STICKY, NULL);
	harness_cycle();
	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	assert(s->workspace_index == 1 && s->visible_desired);
	assert(wm.workspaces[1].windows.next == &s->workspace_link ||
	       wm.workspaces[1].windows.next == &t->workspace_link);
	{
		struct Window *first = wl_container_of(
			wm.workspaces[1].windows.next, first, workspace_link);
		struct Window *again;

		harness_press(seat, ACTION_WORKSPACE, "4");
		harness_cycle();
		again = wl_container_of(wm.workspaces[3].windows.next, again,
					workspace_link);
		assert(first == again);
		harness_press(seat, ACTION_WORKSPACE, "2");
		harness_cycle();
	}
	harness_interact(seat, t);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	assert(!t->floating && !t->sticky);
	harness_interact(seat, s);
	harness_cycle();
	harness_press(seat, ACTION_NEXT_WORKSPACE, NULL);
	harness_cycle();
	assert(s->workspace_index == 2 && s->visible_desired);
	harness_interact(seat, s);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_STICKY, NULL);
	harness_cycle();
	assert(!s->sticky && s->floating);
	harness_press(seat, ACTION_WORKSPACE, "1");
	harness_cycle();
	assert(s->workspace_index == 2 && !s->visible_desired);
	harness_teardown();

	harness_init(NULL);
	a = harness_add_output(0, 0, 1920, 1080);
	b = harness_add_output(1920, 0, 1920, 1080);
	seat = harness_add_seat(a);
	s = add_window_id("s", "ids");
	harness_cycle();
	harness_interact(seat, s);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_STICKY, NULL);
	harness_cycle();
	harness_pointer_to(seat, b);
	harness_cycle();
	t = add_window_id("t", "idt");
	harness_cycle();
	harness_interact(seat, t);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_STICKY, NULL);
	harness_cycle();
	assert(s->workspace_index == 0 && t->workspace_index == 1);
	harness_pointer_to(seat, a);
	harness_cycle();
	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	assert(a->visible_workspace == 1 && b->visible_workspace == 0);
	assert(s->workspace_index == 1 && t->workspace_index == 0);
	assert(s->visible_desired && t->visible_desired);

	restore_tmp_path(path);
	assert(restore_save(path) == 0);
	harness_teardown();
	harness_init(NULL);
	(void)harness_add_seat(harness_add_output(0, 0, 1920, 1080));
	s = add_window_id("s", "ids");
	u = add_window_id("u", "idu");
	assert(restore_load(path) == 0);
	harness_cycle();
	assert(s->sticky && !u->sticky);
	harness_teardown();

	restore_tmp_path(path);
	f = fopen(path, "w");
	assert(f != NULL);
	fputs("nomonad-restore 1\n"
	      "win idu 3 0 1 1 10 10 300 200 0 0 0 0 0 0 -\n",
	      f);
	fclose(f);
	harness_init(NULL);
	(void)harness_add_seat(harness_add_output(0, 0, 1920, 1080));
	u = add_window_id("u", "idu");
	assert(restore_load(path) == 0);
	harness_cycle();
	assert(u->workspace_index == 3 && u->floating && !u->sticky);
}

static void scenario_keymap(void)
{
	void *cfg, *km, *kb;
	u64 before;

	harness_init("(defcfg (keymap :layout \"us\"))");
	harness_track_iface("river_xkb_keymap_v1");
	cfg = harness_enable_xkb_config();
	assert(harness_tracked_count() == 1);
	km = harness_tracked(0);
	harness_track_iface(NULL);
	before = harness_request_count_get();
	kb = harness_xkb_keyboard(cfg);
	assert(harness_request_count_get() == before);
	harness_keymap_success(km);
	assert(harness_last_opcode() == RIVER_XKB_KEYBOARD_V1_SET_KEYMAP);
	before = harness_request_count_get();
	(void)harness_xkb_keyboard(cfg);
	assert(harness_request_count_get() == before + 1);
	assert(harness_last_opcode() == RIVER_XKB_KEYBOARD_V1_SET_KEYMAP);

	harness_stage_reload("(defcfg (keymap :layout \"us\"))");
	harness_track_iface("river_xkb_keymap_v1");
	reload_now();
	assert(harness_tracked_count() == 0);

	harness_stage_reload("(defcfg (keymap :layout \"de\"))");
	harness_track_iface("river_xkb_keymap_v1");
	reload_now();
	assert(harness_tracked_count() == 1);
	harness_keymap_success(harness_tracked(0));
	assert(harness_last_opcode() == RIVER_XKB_KEYBOARD_V1_SET_KEYMAP);

	harness_stage_reload("(defcfg (keymap :layout \"fr\"))");
	harness_track_iface("river_xkb_keymap_v1");
	reload_now();
	assert(harness_tracked_count() == 1);
	harness_keymap_failure(harness_tracked(0));
	harness_stage_reload("(defcfg (keymap :layout \"fr\"))");
	harness_track_iface("river_xkb_keymap_v1");
	reload_now();
	assert(harness_tracked_count() == 1);
	harness_keymap_success(harness_tracked(0));

	harness_stage_reload("(defkeys (A q close))");
	harness_track_iface("river_xkb_keymap_v1");
	reload_now();
	assert(harness_tracked_count() == 1);
	harness_stage_reload("(defkeys (A q close))");
	harness_track_iface("river_xkb_keymap_v1");
	reload_now();
	assert(harness_tracked_count() == 0);
	harness_track_iface(NULL);
	(void)kb;
}

static void mods_release(struct Seat *seat)
{
	river_xkb_bindings_seat_listener.modifiers_update(
		seat, NULL, RIVER_SEAT_V1_MODIFIERS_MOD1, 0);
	harness_cycle();
}

static void scenario_mru(void)
{
	struct Window *a, *b, *c, *d;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defkeys (A Tab focus-mru-next)\n"
		     "         (A-S Tab focus-mru-prev))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	harness_cycle();
	harness_interact(seat, c);
	harness_cycle();
	harness_press(seat, ACTION_MOVE_TO_WORKSPACE, "2");
	harness_cycle();
	harness_interact(seat, a);
	harness_cycle();
	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	harness_interact(seat, c);
	harness_cycle();
	harness_press(seat, ACTION_WORKSPACE, "1");
	harness_cycle();
	harness_interact(seat, b);
	harness_cycle();
	assert(seat->focused == b);

	harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
	harness_cycle();
	assert(seat->mru_active && seat->focused == c);
	assert(out->visible_workspace == 1);
	assert(seat->mru_watch == RIVER_SEAT_V1_MODIFIERS_MOD1);
	harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
	harness_cycle();
	assert(seat->focused == a && out->visible_workspace == 0);
	mods_release(seat);
	assert(!seat->mru_active && seat->focused == a);
	assert(seat->focus_last == b && seat->focus_cur == a);
	assert(seat->mru_watch == 0 && !seat->mru_commit_pending);

	harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
	harness_cycle();
	assert(seat->focused == b);
	mods_release(seat);
	harness_key_press(seat, ACTION_FOCUS_MRU_PREV);
	harness_cycle();
	assert(seat->focused == c);
	mods_release(seat);
	assert(seat->focused == c && seat->focus_last == b);

	harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
	harness_cycle();
	assert(seat->focused == b);
	harness_close_window(a);
	harness_cycle();
	harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
	harness_cycle();
	assert(seat->focused == c);
	mods_release(seat);

	d = harness_add_window("d");
	harness_cycle();
	harness_interact(seat, d);
	harness_cycle();
	harness_press(seat, ACTION_FOCUS_MRU_NEXT, NULL);
	harness_cycle();
	assert(!seat->mru_active && seat->focused == c);
	harness_press(seat, ACTION_FOCUS_MRU_NEXT, NULL);
	harness_cycle();
	assert(!seat->mru_active && seat->focused == d);

	harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
	harness_cycle();
	assert(seat->mru_active && seat->focused == c);
	harness_interact(seat, d);
	harness_cycle();
	assert(!seat->mru_active && seat->focused == d);
	assert(seat->focus_last == c);

	harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
	harness_cycle();
	assert(seat->mru_active);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(!seat->mru_active);

	harness_key_press(seat, ACTION_FOCUS_MRU_PREV);
	harness_cycle();
	assert(seat->mru_active &&
	       seat->mru_watch == RIVER_SEAT_V1_MODIFIERS_MOD1);
	mods_release(seat);
	assert(!seat->mru_active);

	{
		char dir[] = "/tmp/nmh.XXXXXX";
		struct Window *x, *y;
		char reply[256];
		ssize_t n;
		int fd;

		assert(mkdtemp(dir) != NULL);
		setenv("XDG_RUNTIME_DIR", dir, 1);
		assert(ctl_init(NULL) == 0);
		x = add_window_id("x", "idx");
		y = add_window_id("y", "idy");
		harness_cycle();
		harness_interact(seat, x);
		harness_cycle();
		harness_interact(seat, y);
		harness_cycle();
		assert(seat->focused == y);
		harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
		harness_cycle();
		assert(seat->mru_active && seat->focused == x);
		fd = ctl_connect(dir);
		assert(write(fd, "focus-window idy\n", 17) == 17);
		shutdown(fd, SHUT_WR);
		ctl_pump(3);
		n = read(fd, reply, sizeof(reply) - 1);
		assert(n > 0);
		reply[n] = '\0';
		close(fd);
		assert(strcmp(reply, "ok\n") == 0);
		assert(!seat->mru_active && seat->focused == y);
		assert(seat->focus_last == x);

		harness_key_press(seat, ACTION_FOCUS_MRU_NEXT);
		harness_cycle();
		assert(seat->mru_active);
		(void)harness_add_window("e");
		harness_cycle();
		assert(!seat->mru_active);
		ctl_shutdown();
		rmdir(dir);
	}
}

static void scenario_client_fullscreen_real(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *g, *t;
	char path[64];

	harness_init(NULL);
	harness_enable_layer_shell();
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();
	harness_set_usable(out, 0, 30, 1920, 1050);
	g = add_window_id("game", "idg");
	t = harness_add_window("t");
	harness_cycle();
	harness_interact(seat, g);
	harness_cycle();

	river_window_listener.fullscreen_requested(g, NULL, NULL);
	harness_cycle();
	assert(g->fullscreen && g->fullscreen_client);
	assert(g->fullscreen_output == out);
	assert(!t->visible_desired);

	restore_tmp_path(path);
	assert(restore_save(path) == 0);

	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	assert(g->fullscreen && g->fullscreen_output == out);
	assert(!g->visible_desired);
	harness_press(seat, ACTION_WORKSPACE, "1");
	harness_cycle();
	assert(g->fullscreen_output == out);

	river_window_listener.exit_fullscreen_requested(g, NULL);
	harness_cycle();
	assert(!g->fullscreen && g->fullscreen_output == NULL);
	assert(g->position_synced && g->dimensions_synced);
	assert(g->y + g->last_proposed_h <= 1080 && g->y >= 30);

	harness_interact(seat, g);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(g->fullscreen && !g->fullscreen_client);
	assert(g->fullscreen_output == NULL && !g->fullscreen_informed);
	assert(g->y == 30 && g->last_proposed_h == 1050);
	river_window_listener.exit_fullscreen_requested(g, NULL);
	harness_cycle();
	assert(g->fullscreen && g->y == 30 && g->last_proposed_h == 1050);
	harness_teardown();

	harness_init(NULL);
	out = harness_add_output(0, 0, 1920, 1080);
	(void)harness_add_seat(out);
	g = add_window_id("game", "idg");
	assert(restore_load(path) == 0);
	harness_cycle();
	assert(g->fullscreen && g->fullscreen_client);
	assert(g->fullscreen_output == out);
}

static void scenario_restart_bad_state(void)
{
	static const char *const lines[] = {
		"nomonad-restore 1\n",
		"output 0 0 99\n",
		"output 5 5 1\n",
		"ws 0 7 2\n",
		"ws 99 0 1\n",
		"tall 0 0 0 -4 0.5\n",
		"tall 0 0 0 1 nan\n",
		"tall 0 1 0 1 0.5\n",
		"bsp 0 1 S v 0.5 L ida\n",
		"bsp 0 1 S x 0.5 L ida L idb\n",
		"bsp 0 1 S v 2.0 L ida L idb\n",
		"bsp 0 1 S v 0.5 L ida L idb trailing\n",
		"bsp 0 1 S v 0.5 L ida L ida\n",
		"win ida 99 0 0 0 0 0 0 0 0 0 0 0 0 0 -\n",
		"win ida 0 0 7 0 0 0 0 0 0 0 0 0 0 0 -\n",
		"win nope 0 0 0 0 0 0 0 0 0 0 0 0 0 0 -\n",
		"win idb 0 0\n",
		"focus nope\n",
		"seatout 1 1\n",
		"garbage\n",
	};
	struct Window *a, *b;
	struct Output *out;
	struct Seat *seat;
	char path[64];
	FILE *f;

	restore_tmp_path(path);
	f = fopen(path, "w");
	assert(f != NULL);
	for (size_t i = 0; i < ARRAY_SIZE(lines); i++)
		fputs(lines[i], f);
	fputs("bsp 0 1", f);
	for (int i = 0; i < 200; i++)
		fputs(" S v 0.5", f);
	fputs(" L ida L idb\n", f);
	fclose(f);

	harness_init("(defcfg (layouts tall bsp))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = add_window_id("a", "ida");
	b = add_window_id("b", "idb");
	assert(restore_load(path) == 0);
	harness_cycle();
	harness_cycle();
	assert(a->workspace_index == 0 && b->workspace_index == 0);
	assert(wm.workspaces[0].current_layout_index == 0);
	assert(wm.workspaces[0].layouts[0].u.tall.nmaster == 1);
	assert(out->visible_workspace == 0 && seat->output == out);

	restore_tmp_path(path);
	f = fopen(path, "w");
	assert(f != NULL);
	fputs("not a restore file\nws 0 1 2\n", f);
	fclose(f);
	assert(restore_load(path) != 0);
	harness_cycle();
	assert(wm.workspaces[0].current_layout_index == 0);
}

static void scenario_min_size_clipped_to_tile(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Window *a;
	struct Window *b;

	harness_init("(defcfg (layout bsp))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	harness_cycle();
	b = harness_add_window("steam");
	river_window_listener.dimensions_hint(b, NULL, 1500, 0, 0, 0);
	harness_cycle();
	harness_cycle();
	assert(b->last_proposed_w == 1500);
	assert(b->clip_synced && b->last_clip_w == b->last_bounds_w);
	assert(b->last_clip_w < 1500 && b->last_clip_h == b->last_bounds_h);
	assert(a->clip_synced && a->last_clip_w == a->last_proposed_w);

	harness_interact(seat, b);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	assert(b->floating && b->last_clip_w == 0 && b->last_clip_h == 0);
}

static void scenario_focus_last(void)
{
	struct Output *oa;
	struct Output *ob;
	struct Seat *seat;
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Window *d;

	harness_init(NULL);
	oa = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(oa);
	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	harness_cycle();
	harness_interact(seat, a);
	harness_cycle();
	harness_interact(seat, c);
	harness_cycle();
	assert(seat->focused == c);

	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->focused == a);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->focused == c);

	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	d = harness_add_window("d");
	harness_cycle();
	harness_interact(seat, d);
	harness_cycle();
	assert(d->workspace_index == 1 && seat->focused == d);

	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(oa->visible_workspace == 0 && seat->focused == c);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(oa->visible_workspace == 1 && seat->focused == d);

	ob = harness_add_output(1920, 0, 1280, 720);
	harness_cycle();
	assert(ob->visible_workspace == 0);
	harness_press(seat, ACTION_FOCUS_OUTPUT, "1");
	harness_cycle();
	assert(seat->output == ob && seat->focused != NULL);
	assert(seat->focused->workspace_index == 0);
	harness_interact(seat, b);
	harness_cycle();
	assert(seat->focused == b);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->output == ob && seat->focused != b);

	harness_press(seat, ACTION_FOCUS_OUTPUT, "0");
	harness_cycle();
	assert(seat->output == oa && seat->focused == d);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->output == ob && seat->focused->workspace_index == 0);
	assert(oa->visible_workspace == 1 && ob->visible_workspace == 0);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->output == oa && seat->focused == d);

	harness_close_window(seat->focus_last);
	harness_cycle();
	assert(seat->focus_last == NULL);
	harness_press(seat, ACTION_FOCUS_LAST, NULL);
	harness_cycle();
	assert(seat->focused == d && seat->output == oa);
}

static void scenario_output_focus_sticks_until_crossing(void)
{
	struct Output *a;
	struct Output *b;
	struct Seat *seat;

	harness_init(NULL);
	a = harness_add_output(0, 0, 1920, 1080);
	b = harness_add_output(1920, 0, 1280, 720);
	seat = harness_add_seat(a);
	harness_cycle();
	assert(seat->output == a);

	harness_press(seat, ACTION_FOCUS_OUTPUT, "1");
	harness_cycle();
	assert(seat->output == b);
	harness_pointer_at(seat, 961, 541);
	harness_cycle();
	assert(seat->output == b);

	harness_pointer_at(seat, 2000, 100);
	harness_cycle();
	assert(seat->output == b);
	harness_pointer_at(seat, 100, 100);
	harness_cycle();
	assert(seat->output == a);
}

static void scenario_window_hints_and_capture(void)
{
	const u32 all =
		RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;
	struct Output *out;
	struct Seat *seat;
	struct Window *t;
	struct Window *f;

	harness_init("(defrules (float (app-id \"f\")))\n"
		     "(defcfg (borders :width 2 :captured \"#ff0000\"))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	t = harness_add_window("t");
	f = harness_add_window("f");
	harness_cycle();
	harness_cycle();
	assert(t->tiled_synced && t->last_tiled == all);
	assert(f->tiled_synced && f->last_tiled == RIVER_WINDOW_V1_EDGES_NONE);

	harness_interact(seat, f);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FLOAT, NULL);
	harness_cycle();
	assert(!f->floating && f->last_tiled == all);

	assert(t->last_border_g != 0);
	harness_window_capture(t, 1);
	harness_cycle();
	assert(t->last_border_r == 0xffffffffu && t->last_border_g == 0);
	harness_window_capture(t, 0);
	harness_cycle();
	assert(t->last_border_g != 0);

	harness_output_capture(out, 1);
	harness_cycle();
	assert(t->last_border_g == 0 && f->last_border_g == 0);
	harness_output_capture(out, 0);
	harness_cycle();

	assert(out->presentation_synced &&
	       out->presentation_mode ==
		       RIVER_OUTPUT_V1_PRESENTATION_MODE_VSYNC);
	harness_window_presentation(t, RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC);
	harness_interact(seat, t);
	harness_cycle();
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(t->fullscreen);
	assert(out->presentation_mode ==
	       RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC);
	harness_press(seat, ACTION_TOGGLE_FULLSCREEN, NULL);
	harness_cycle();
	assert(out->presentation_mode ==
	       RIVER_OUTPUT_V1_PRESENTATION_MODE_VSYNC);
}

static void scenario_repeat_one_shot_and_reload(void)
{
	struct Output *out;
	struct Seat *seat;

	harness_init("(defkeys (A j focus-next) (A r reload-config))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	harness_key_press(seat, ACTION_FOCUS_NEXT);
	assert(seat->repeat_action.kind == ACTION_FOCUS_NEXT);
	harness_key_release(seat, ACTION_FOCUS_NEXT);
	assert(seat->repeat_action.kind == ACTION_NONE);
	harness_cycle();

	harness_key_press(seat, ACTION_FOCUS_NEXT);
	assert(seat->repeat_action.kind == ACTION_FOCUS_NEXT);
	harness_key_press(seat, ACTION_RELOAD_CONFIG);
	assert(seat->repeat_action.kind == ACTION_NONE);
	harness_cycle();

	harness_key_press(seat, ACTION_FOCUS_NEXT);
	harness_cycle();
	assert(seat->repeat_action.kind == ACTION_FOCUS_NEXT);
	harness_stage_reload("(defkeys (A j focus-next))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(g_staged_config == NULL);
	assert(seat->repeat_action.kind == ACTION_NONE);
	(void)out;
}

static void scenario_repeat_disarmed_on_submap_exit(void)
{
	struct Output *out;
	struct Seat *seat;

	harness_init("(defsubmap nav (g focus-next))\n"
		     "(defkeys (A s (submap nav)))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	harness_press(seat, ACTION_SUBMAP, "nav");
	harness_cycle();
	assert(seat->submap_active);

	harness_key_press(seat, ACTION_FOCUS_NEXT);
	assert(seat->repeat_action.kind == ACTION_FOCUS_NEXT);
	harness_cycle();
	assert(!seat->submap_active);
	assert(seat->repeat_action.kind == ACTION_NONE);
	(void)out;
}

/*
 * BSP split-axis stability: spawning then closing a window on the same
 * cell repeatedly should always pick the same split axis (perpendicular
 * to the longer side), not alternate. Pre-fix the layout persisted a
 * "last axis" flag across tree-collapse events, so a wide cell would
 * V-split, then on the next spawn after a close, alternate to H.
 */
static void scenario_bsp_split_axis_stable(void)
{
	struct Window *base;
	struct Window *peer;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout bsp))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);

	base = harness_add_window("base");
	harness_cycle();
	assert(harness_count_ws_windows(0) == 1);

	for (int i = 0; i < 4; i++) {
		peer = harness_add_window("peer");
		harness_cycle();
		assert(harness_count_ws_windows(0) == 2);
		/*
		 * On a wide cell (1920x1080) the rect-based pick is V:
		 * the two leaves end up side-by-side (same y, different
		 * x). After my fix this must hold every iteration; pre-fix
		 * iterations 2 and 4 flipped to H (stacked, different y).
		 */
		assert(base->y == peer->y);
		assert(base->x != peer->x);

		harness_close_window(peer);
		harness_cycle();
		assert(harness_count_ws_windows(0) == 1);
	}
	(void)seat;
}

/*
 * Smart borders + smart gaps: with both :smart true and a single tile,
 * the lone tile fills the output edge-to-edge with no border. Add a
 * second window and the gaps + borders snap back. Mirrors xmonad's
 * combined NoBorders.smartBorders + Spacing smart-spacing behavior.
 */
static void scenario_smart_chromeless_single_tile(void)
{
	struct Window *a;
	struct Window *b;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg\n"
		     "  (gaps :top 25 :bottom 1 :left 1 :right 1 :inner 5\n"
		     "        :smart true)\n"
		     "  (borders :width 3 :smart true))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	harness_cycle();

	/*
	 * Single tile: smart on both blocks => zero gap, zero per-cell
	 * inset. tile rect must equal the full output rect, and the
	 * border was suppressed in apply_borders too.
	 */
	assert(a->x == out->x);
	assert(a->y == out->y);
	assert(a->last_proposed_w == out->width);
	assert(a->last_proposed_h == out->height);
	assert(a->last_border_width == 0);

	/* Adding a second tile re-enables struts + inner gaps + borders. */
	b = harness_add_window("b");
	harness_cycle();
	assert(a->last_border_width == 3);
	assert(b->last_border_width == 3);
	assert(a->x > out->x);
	assert(a->y > out->y);
	(void)seat;
}

/*
 * Smart borders without smart gaps: a single tile keeps its inner
 * struts and inner gap but loses its border. Verifies the two :smart
 * flags are independent.
 */
static void scenario_smart_borders_only(void)
{
	struct Window *a;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg\n"
		     "  (gaps :top 10 :bottom 10 :left 10 :right 10 :inner 4)\n"
		     "  (borders :width 3 :smart true))");
	out = harness_add_output(0, 0, 1000, 800);
	seat = harness_add_seat(out);
	a = harness_add_window("a");
	harness_cycle();

	/* gaps still applied, but no border so cell inset is gaps.inner only. */
	assert(a->last_border_width == 0);
	assert(a->x == out->x + 10 + 4); /* gaps.left + inner */
	assert(a->y == out->y + 10 + 4); /* gaps.top + inner */
	(void)seat;
}

/*
 * Deck (monocle) layout: focused window covers the whole output;
 * everything else is hidden. focus-next moves the visible cell to
 * the next window. Stateless, so a workspace with N windows has
 * exactly one rect on screen and N-1 hidden tiles.
 */
static void scenario_deck_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Output *out;
	struct Seat *seat;

	/*
	 * Borders 0 so the focused tile lines up flush with the output
	 * rect. The default config gives a 3px border which would offset
	 * the cell by inner+border = 3 - testable but obscures the intent
	 * of "deck = full output for the focused window."
	 */
	harness_init("(defcfg (layout deck) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_DECK);

	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	harness_cycle();

	/*
	 * Auto-focus follows the most recently spawned window: c is the
	 * head of the workspace list and apply targets it. a and b stay
	 * hidden, c covers the whole output rect.
	 */
	assert(seat->focused == c);
	assert(c->visible_desired);
	assert(!a->visible_desired);
	assert(!b->visible_desired);
	assert(c->x == out->x);
	assert(c->y == out->y);
	assert(c->last_proposed_w == out->width);
	assert(c->last_proposed_h == out->height);

	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	/* Focus walks head->tail (c then b). The visible cell follows. */
	assert(seat->focused == b);
	assert(b->visible_desired);
	assert(!a->visible_desired);
	assert(!c->visible_desired);
	assert(b->x == out->x);
	assert(b->y == out->y);
	assert(b->last_proposed_w == out->width);
	assert(b->last_proposed_h == out->height);

	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	assert(seat->focused == a);
	assert(a->visible_desired);
	assert(!b->visible_desired);
	assert(!c->visible_desired);
}

/*
 * Tabbed: visually identical to Deck for v1 (headless variant). The
 * focused tile gets the whole rect, others hide. Asserting the same
 * shape Deck does locks in that the tabbed-as-deck implementation
 * stays equivalent until / unless tab visuals land via layer-shell.
 */
static void scenario_tabbed_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout tabbed) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_TABBED);
	assert(workspace_layout(&wm.workspaces[0])->focus_changes_visibility);

	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	harness_cycle();

	/* Most recent (c) auto-focuses; only it is visible. */
	assert(seat->focused == c);
	assert(c->visible_desired);
	assert(!a->visible_desired);
	assert(!b->visible_desired);
	assert(c->x == out->x && c->y == out->y);
	assert(c->last_proposed_w == out->width);
	assert(c->last_proposed_h == out->height);

	/* Focus walks head->tail; the visible cell follows the focused tab. */
	harness_press(seat, ACTION_FOCUS_NEXT, NULL);
	harness_cycle();
	assert(seat->focused == b);
	assert(b->visible_desired);
	assert(!a->visible_desired);
	assert(!c->visible_desired);
}

/*
 * Spiral / Dwindle: walk the window list, alternating V then H
 * splits at the fixed 0.5 ratio. With 4 windows on 1920x1080 the
 * tiles spiral into the bottom-right corner:
 *
 *   index 0 -> (0, 0,    960, 1080)         left half, full height
 *   index 1 -> (960, 0,  960, 540)          top of the right half
 *   index 2 -> (960, 540, 480, 540)         bottom-left of the BR quad
 *   index 3 -> (1440, 540, 480, 540)        bottom-right corner
 *
 * head->tail walk through ws->windows (LIFO) means
 * arr[0]=d, arr[1]=c, arr[2]=b, arr[3]=a after add(a..d).
 */
static void scenario_spiral_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Window *d;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout spiral) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_SPIRAL);

	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	d = harness_add_window("d");
	harness_cycle();

	assert(d->x == out->x && d->y == out->y);
	assert(d->last_proposed_w == 960 && d->last_proposed_h == 1080);

	assert(c->x == out->x + 960 && c->y == out->y);
	assert(c->last_proposed_w == 960 && c->last_proposed_h == 540);

	assert(b->x == out->x + 960 && b->y == out->y + 540);
	assert(b->last_proposed_w == 480 && b->last_proposed_h == 540);

	assert(a->x == out->x + 1440 && a->y == out->y + 540);
	assert(a->last_proposed_w == 480 && a->last_proposed_h == 540);
	(void)seat;
}

/*
 * TwoPane: master on the left, focused non-master on the right,
 * everyone else hidden. Three windows on a 1920x1080 output, ratio
 * 0.5 -> master_w=right_w=960. Focus drives which non-master shows.
 */
static void scenario_two_pane_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout two-pane) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_TWO_PANE);
	assert(workspace_layout(&wm.workspaces[0])->focus_changes_visibility);

	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	harness_cycle();

	/*
	 * arr head->tail: c, b, a. Most recent (c) auto-focuses; since
	 * c == windows[0] (master) the right pane falls back to
	 * windows[1] = b. a is the hidden non-master.
	 */
	assert(seat->focused == c);
	assert(c->visible_desired);
	assert(b->visible_desired);
	assert(!a->visible_desired);
	assert(c->x == out->x && c->y == out->y);
	assert(c->last_proposed_w == 960);
	assert(c->last_proposed_h == out->height);
	assert(b->x == out->x + 960 && b->y == out->y);
	assert(b->last_proposed_w == 960);
	assert(b->last_proposed_h == out->height);

	/*
	 * Focus the hidden non-master. Apply re-runs (focus_changes_
	 * visibility = true), so a takes the right pane, b hides.
	 * Master c stays put.
	 */
	harness_press(seat, ACTION_FOCUS_NEXT, NULL); /* c -> b */
	harness_cycle();
	assert(seat->focused == b);
	assert(b->visible_desired);
	assert(!a->visible_desired);

	harness_press(seat, ACTION_FOCUS_NEXT, NULL); /* b -> a */
	harness_cycle();
	assert(seat->focused == a);
	assert(a->visible_desired);
	assert(!b->visible_desired);
	assert(a->x == out->x + 960 && a->y == out->y);
	assert(a->last_proposed_w == 960);

	/*
	 * EXPAND_MASTER drives the same u.tall.ratio that TALL family
	 * uses; TwoPane reads it on the width axis.
	 */
	harness_press(seat, ACTION_EXPAND_MASTER, NULL);
	harness_cycle();
	assert(c->last_proposed_w == (s32)((float)out->width * 0.53f));
}

/*
 * ResizableTall: TALL plus per-cell height delta. Default 4-window
 * shape on 1080 with nmaster=1 has master full height and three
 * stack cells at row_h=360 each. expand-cell on the middle stack
 * tile (focused = arr[2]) bumps its delta by +20 and shrinks its
 * column-mate (arr[3], the next cell in the stack) by 20, so the
 * stack heights become 360 / 380 / 340. The pairing keeps the
 * column total at 1080 with no leftover.
 *
 * Also asserts that closing the resized cell scrubs its delta
 * via the drop_window vtable so a future cycle does not reach
 * a freed Window pointer.
 */
static void scenario_resizable_tall_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Window *d;
	struct Output *out;
	struct Seat *seat;
	struct Workspace *ws;

	harness_init("(defcfg (layout resizable-tall) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	ws = &wm.workspaces[0];
	assert(workspace_layout(ws)->kind == LAYOUT_RESIZABLE_TALL);

	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	d = harness_add_window("d");
	harness_cycle();

	/*
	 * arr head->tail: d, c, b, a. Master = d (full height).
	 * Stack: c, b, a at row_h = 1080/3 = 360 each.
	 */
	assert(d->last_proposed_h == 1080);
	assert(c->last_proposed_h == 360);
	assert(b->last_proposed_h == 360);
	assert(a->last_proposed_h == 360);

	/*
	 * Walk focus to b (the middle stack cell): d -> c -> b.
	 * expand-cell pairs b (+20) with a (the next cell after b in
	 * the stack column = -20). Stack heights become 360 / 380 / 340.
	 */
	harness_press(seat, ACTION_FOCUS_NEXT, NULL); /* d -> c */
	harness_cycle();
	harness_press(seat, ACTION_FOCUS_NEXT, NULL); /* c -> b */
	harness_cycle();
	assert(seat->focused == b);

	harness_press(seat, ACTION_LAYOUT_VERB, "expand-cell");
	harness_cycle();
	assert(c->last_proposed_h == 360);
	assert(b->last_proposed_h == 380);
	assert(a->last_proposed_h == 340);

	/* shrink-cell on the same focused cell undoes the bump. */
	harness_press(seat, ACTION_LAYOUT_VERB, "shrink-cell");
	harness_cycle();
	assert(c->last_proposed_h == 360);
	assert(b->last_proposed_h == 360);
	assert(a->last_proposed_h == 360);

	/*
	 * Re-skew, then close the pair partner. b keeps its delta and
	 * resizable_tall_column rescales the column so it still fills
	 * the output exactly.
	 */
	harness_press(seat, ACTION_LAYOUT_VERB, "expand-cell");
	harness_cycle();
	harness_close_window(a);
	harness_cycle();
	assert(harness_count_ws_windows(0) == 3);
	assert(c->last_proposed_h + b->last_proposed_h == 1080);
	assert(b->last_proposed_h > c->last_proposed_h);
	(void)d;
}

/*
 * Layout cycling: ACTION_NEXT_LAYOUT and ACTION_PREV_LAYOUT walk a
 * workspace's layouts[] cycle modulo n_layouts. Verifies forward
 * walk, wraparound, reverse walk, single-entry no-op, and that
 * each workspace tracks its own index independently.
 */
static void scenario_layout_cycle(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Workspace *ws0;
	struct Workspace *ws1;

	harness_init("(defcfg (layouts tall bsp deck))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	ws0 = &wm.workspaces[0];
	ws1 = &wm.workspaces[1];
	assert(ws0->n_layouts == 3);
	assert(ws0->layouts[0].kind == LAYOUT_TALL);
	assert(ws0->layouts[1].kind == LAYOUT_BSP);
	assert(ws0->layouts[2].kind == LAYOUT_DECK);
	assert(ws0->current_layout_index == 0);
	assert(workspace_layout(ws0)->kind == LAYOUT_TALL);

	harness_press(seat, ACTION_NEXT_LAYOUT, NULL);
	harness_cycle();
	assert(ws0->current_layout_index == 1);
	assert(workspace_layout(ws0)->kind == LAYOUT_BSP);

	harness_press(seat, ACTION_NEXT_LAYOUT, NULL);
	harness_cycle();
	assert(ws0->current_layout_index == 2);
	assert(workspace_layout(ws0)->kind == LAYOUT_DECK);

	/* Wrap forward: deck -> tall. */
	harness_press(seat, ACTION_NEXT_LAYOUT, NULL);
	harness_cycle();
	assert(ws0->current_layout_index == 0);
	assert(workspace_layout(ws0)->kind == LAYOUT_TALL);

	/* Wrap backward: tall -> deck. */
	harness_press(seat, ACTION_PREV_LAYOUT, NULL);
	harness_cycle();
	assert(ws0->current_layout_index == 2);
	assert(workspace_layout(ws0)->kind == LAYOUT_DECK);

	/*
	 * Per-workspace independence: switching to ws1, cycling, switching
	 * back must not reset ws0's index. ws1 starts at 0 (tall).
	 */
	assert(ws1->current_layout_index == 0);
	harness_press(seat, ACTION_WORKSPACE, "2");
	harness_cycle();
	harness_press(seat, ACTION_NEXT_LAYOUT, NULL);
	harness_cycle();
	assert(ws1->current_layout_index == 1);
	assert(workspace_layout(ws1)->kind == LAYOUT_BSP);
	harness_press(seat, ACTION_WORKSPACE, "1");
	harness_cycle();
	assert(ws0->current_layout_index == 2);
	assert(workspace_layout(ws0)->kind == LAYOUT_DECK);
	(void)out;
}

/*
 * Single-entry cycle: NEXT_LAYOUT and PREV_LAYOUT are no-ops. The
 * default config seeds (layouts tall) and the explicit single-entry
 * (layout NAME) form sets n_layouts == 1 - both must reject cycling
 * cleanly so the action does not divide-by-zero or wrap into bogus
 * indices.
 */
static void scenario_layout_cycle_single_entry_noop(void)
{
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout bsp))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	assert(wm.workspaces[0].n_layouts == 1);
	assert(wm.workspaces[0].current_layout_index == 0);
	harness_press(seat, ACTION_NEXT_LAYOUT, NULL);
	harness_cycle();
	assert(wm.workspaces[0].current_layout_index == 0);
	harness_press(seat, ACTION_PREV_LAYOUT, NULL);
	harness_cycle();
	assert(wm.workspaces[0].current_layout_index == 0);
	(void)out;
}

/*
 * Hot reload preserves the active layout when its kind is still
 * present in the new cycle. Start at index 1 (bsp) under
 * (layouts tall bsp deck); reload to (layouts deck bsp tall).
 * The active kind (bsp) is now at index 1 in the new array, so
 * current_layout_index stays 1.
 *
 * Second reload shrinks the cycle and removes the prior active
 * kind: (layouts tall) does not contain bsp, so the index snaps
 * to 0 (tall) rather than indexing past the end of the new array.
 */
static void scenario_layout_cycle_hot_reload(void)
{
	struct Output *out;
	struct Seat *seat;
	struct Workspace *ws;

	harness_init("(defcfg (layouts tall bsp deck))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	ws = &wm.workspaces[0];
	harness_press(seat, ACTION_NEXT_LAYOUT, NULL);
	harness_cycle();
	assert(ws->current_layout_index == 1);
	assert(workspace_layout(ws)->kind == LAYOUT_BSP);

	/*
	 * Reorder the cycle. bsp moves from index 1 to index 1 in the new
	 * array (deck bsp tall), so the kind is preserved across reload.
	 */
	harness_stage_reload("(defcfg (layouts deck bsp tall))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(ws->n_layouts == 3);
	assert(ws->current_layout_index == 1);
	assert(workspace_layout(ws)->kind == LAYOUT_BSP);

	/*
	 * Shrink the cycle and drop bsp entirely. The reload must clamp
	 * the index back to 0 since the prior active kind is gone.
	 */
	harness_stage_reload("(defcfg (layouts tall))");
	harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
	harness_cycle();
	harness_cycle();
	assert(ws->n_layouts == 1);
	assert(ws->current_layout_index == 0);
	assert(workspace_layout(ws)->kind == LAYOUT_TALL);
	(void)out;
}

/*
 * Mirror Tall: TALL transposed. Master row on top at ratio*height,
 * stack row on the bottom. With nmaster=1, ratio=0.5, three windows
 * on a 1920x1080 output and borders / gaps zeroed: the master
 * (workspace head, latest spawn) covers the top half full-width;
 * the two stack tiles split the bottom half left/right with the
 * right-most absorbing leftover pixels from integer division.
 *
 * Also exercises ACTION_EXPAND_MASTER on a Mirror Tall workspace to
 * confirm the seat-side master-ratio dispatch accepts the kind:
 * LayoutTallParams is shared with TALL, so the ratio adjust is the
 * same code path - this scenario just guards against the kind
 * guard regressing.
 */
static void scenario_mirror_tall_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Output *out;
	struct Seat *seat;
	s32 master_h;

	harness_init("(defcfg (layout mirror-tall) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_MIRROR_TALL);

	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	harness_cycle();

	/*
	 * ws->windows order is head->tail = c, b, a (LIFO insert).
	 * arr[0] = c -> master row (top half, full width).
	 * arr[1] = b, arr[2] = a -> stack row (bottom half, two columns).
	 */
	master_h = (s32)((float)out->height * 0.5f);
	assert(c->x == out->x);
	assert(c->y == out->y);
	assert(c->last_proposed_w == out->width);
	assert(c->last_proposed_h == master_h);

	assert(b->x == out->x);
	assert(b->y == out->y + master_h);
	assert(b->last_proposed_w == out->width / 2);
	assert(b->last_proposed_h == out->height - master_h);

	assert(a->x == out->x + out->width / 2);
	assert(a->y == out->y + master_h);
	assert(a->last_proposed_w == out->width - out->width / 2);
	assert(a->last_proposed_h == out->height - master_h);

	/*
	 * Master ratio adjust drives the same u.tall.ratio that TALL uses;
	 * Mirror Tall reads it on the height axis. After EXPAND_MASTER the
	 * master row is taller and the stack row shrinks correspondingly.
	 */
	harness_press(seat, ACTION_EXPAND_MASTER, NULL);
	harness_cycle();
	master_h = (s32)((float)out->height * 0.53f);
	assert(c->last_proposed_h == master_h);
	assert(b->last_proposed_h == out->height - master_h);
}

/*
 * Three Columns: master column in the center, slaves split into
 * right-then-left columns. Five windows on a 1920x1080 output with
 * default nmaster=1, ratio=0.5: master (head of list) takes the
 * 480..1440 slab, the two newest-after-master slaves take the right
 * column, the two oldest take the left. Also asserts the single-
 * window case where the master column fills the whole output width.
 */
static void scenario_three_columns_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Window *d;
	struct Window *e;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout three-columns) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_THREE_COLUMNS);

	/* One window: master column gets the full output rect. */
	a = harness_add_window("a");
	harness_cycle();
	assert(a->x == out->x);
	assert(a->y == out->y);
	assert(a->last_proposed_w == out->width);
	assert(a->last_proposed_h == out->height);

	/*
	 * Five windows: list head->tail = e, d, c, b, a. With nmaster=1,
	 * ratio=0.5 and a 1920-wide output, the layout is:
	 *   master_w = 960 (center column at x=480)
	 *   left_w  = 480 (columns at x=0)
	 *   right_w = 480 (columns at x=1440)
	 * Slave order: arr[1..2] = d, c -> right (top, bottom);
	 *              arr[3..4] = b, a -> left (top, bottom).
	 */
	b = harness_add_window("b");
	c = harness_add_window("c");
	d = harness_add_window("d");
	e = harness_add_window("e");
	harness_cycle();

	assert(e->x == out->x + 480);
	assert(e->y == out->y);
	assert(e->last_proposed_w == 960);
	assert(e->last_proposed_h == out->height);

	assert(d->x == out->x + 1440);
	assert(d->y == out->y);
	assert(d->last_proposed_w == 480);
	assert(d->last_proposed_h == 540);

	assert(c->x == out->x + 1440);
	assert(c->y == out->y + 540);
	assert(c->last_proposed_w == 480);
	assert(c->last_proposed_h == 540);

	assert(b->x == out->x);
	assert(b->y == out->y);
	assert(b->last_proposed_w == 480);
	assert(b->last_proposed_h == 540);

	assert(a->x == out->x);
	assert(a->y == out->y + 540);
	assert(a->last_proposed_w == 480);
	assert(a->last_proposed_h == 540);

	/*
	 * Master ratio adjust drives the same u.tall.ratio used by TALL
	 * and Mirror Tall; the dispatch in seat.c accepts this kind too.
	 * After EXPAND_MASTER the center column is wider (3% bump).
	 */
	harness_press(seat, ACTION_EXPAND_MASTER, NULL);
	harness_cycle();
	assert(e->last_proposed_w == (s32)((float)out->width * 0.53f));
}

/*
 * Grid: roughly square row-major distribution. Two cases:
 *
 * 4 windows on 1920x1080: cols=2, rows=2, every cell is 960x540.
 *
 * 5 windows on 1920x1080: cols=3 (smallest c with c*c >= 5),
 * rows=2. The first row holds 3 cells of 640x540; the second row
 * is partial and its 2 cells widen to 960x540 each, absorbing the
 * leftover columns. Asserting both row shapes guards against a
 * regression where the partial-row width fallback gets dropped
 * (which would leave a 640-wide blank to the right of the bottom
 * row).
 */
static void scenario_grid_layout(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Window *d;
	struct Window *e;
	struct Output *out;
	struct Seat *seat;

	harness_init("(defcfg (layout grid) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	assert(workspace_layout(&wm.workspaces[0])->kind == LAYOUT_GRID);

	a = harness_add_window("a");
	b = harness_add_window("b");
	c = harness_add_window("c");
	d = harness_add_window("d");
	harness_cycle();

	/*
	 * 4 windows: head->tail = d, c, b, a.
	 * Row 0: d(0,0,960,540)  c(960,0,960,540)
	 * Row 1: b(0,540,960,540) a(960,540,960,540)
	 */
	assert(d->x == out->x && d->y == out->y);
	assert(d->last_proposed_w == 960 && d->last_proposed_h == 540);
	assert(c->x == out->x + 960 && c->y == out->y);
	assert(c->last_proposed_w == 960 && c->last_proposed_h == 540);
	assert(b->x == out->x && b->y == out->y + 540);
	assert(b->last_proposed_w == 960 && b->last_proposed_h == 540);
	assert(a->x == out->x + 960 && a->y == out->y + 540);
	assert(a->last_proposed_w == 960 && a->last_proposed_h == 540);

	/*
	 * Add a fifth window: now cols=3, rows=2. head->tail = e, d, c, b, a.
	 * Row 0: e(0,0,640,540) d(640,0,640,540) c(1280,0,640,540)
	 * Row 1: b(0,540,960,540) a(960,540,960,540) - partial row widens.
	 */
	e = harness_add_window("e");
	harness_cycle();

	assert(e->x == out->x && e->y == out->y);
	assert(e->last_proposed_w == 640 && e->last_proposed_h == 540);
	assert(d->x == out->x + 640 && d->last_proposed_w == 640);
	assert(c->x == out->x + 1280 && c->last_proposed_w == 640);

	assert(b->x == out->x && b->y == out->y + 540);
	assert(b->last_proposed_w == 960 && b->last_proposed_h == 540);
	assert(a->x == out->x + 960 && a->y == out->y + 540);
	assert(a->last_proposed_w == 960);
	(void)seat;
}

/*
 * BSP balance: skew several splits via expand-r, then run balance and
 * assert every ratio is back to 0.5. Asserting the skew was actually
 * applied first guards against a regression where expand-r becomes a
 * no-op (which would also pass a balance assertion vacuously).
 */
static void scenario_bsp_balance(void)
{
	struct LayoutBspStats stats;
	struct Output *out;
	struct Seat *seat;
	bool skewed;
	int i;

	harness_init("(defcfg (layout bsp))");

	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	for (i = 0; i < 4; i++)
		harness_add_window("kitty");
	harness_cycle();

	layout_bsp_stats(workspace_layout(&wm.workspaces[0]), &stats);
	assert(stats.n_leaves == 4);
	assert(stats.n_splits == 3);
	assert(fabsf(stats.min_ratio - 0.5f) < 1e-6f);
	assert(fabsf(stats.max_ratio - 0.5f) < 1e-6f);

	/*
	 * The 4 add_window calls landed in a single manage cycle (no cycle
	 * between them), so bsp_insert runs through the want array in
	 * list-head order: the newest window goes in first as the root
	 * leaf, and each subsequent window splits the previous leaf onto
	 * side A. The result is that the auto-focused (newest) window
	 * sits on side B of root, so expand-r (which seeks a VERT
	 * ancestor where focused is on side A) is a no-op. Use expand-l
	 * which targets side B and is guaranteed to skew root's ratio.
	 */
	for (i = 0; i < 5; i++) {
		harness_press(seat, ACTION_LAYOUT_VERB, "expand-l");
		harness_cycle();
	}
	layout_bsp_stats(workspace_layout(&wm.workspaces[0]), &stats);
	skewed = stats.min_ratio < 0.49f || stats.max_ratio > 0.51f;
	assert(skewed);

	harness_press(seat, ACTION_LAYOUT_VERB, "balance");
	harness_cycle();
	layout_bsp_stats(workspace_layout(&wm.workspaces[0]), &stats);
	assert(fabsf(stats.min_ratio - 0.5f) < 1e-6f);
	assert(fabsf(stats.max_ratio - 0.5f) < 1e-6f);
	(void)out;
}

/*
 * BSP balance-area: equal-area variant. Three windows spawned in
 * one cycle on a 1920x1080 output produce an asymmetric tree.
 * ws->windows head->tail is [c, b, a] (LIFO insert), so the
 * apply walk feeds them to bsp_insert in that order: c becomes
 * the root leaf, b splits c (V axis, b on the left, c on the
 * right), a splits b (H axis, a on top, b on bottom).
 *
 *   root V (0.5)
 *   |- A: split H (0.5)        left half of root
 *   |  |- A: leaf_a (top)
 *   |  '- B: leaf_b (bottom)
 *   '- B: leaf_c                right half of root
 *
 * Pre-balance: leaf_a and leaf_b each occupy 960*540 = 518400
 * px^2; leaf_c sits on the un-subdivided right half at
 * 960*1080 = 1036800. After bsp-balance-area the root ratio
 * becomes 2/3 (two leaves on side A, one on side B), so leaf_c
 * shrinks to 640*1080 and leaf_a / leaf_b widen to 1280*540 -
 * each leaf at 691200 px^2 = total_area / 3.
 *
 * Asserting via window areas rather than ratios so a future
 * tree-shape change in bsp_insert (which would land different
 * leaves on each side) does not silently invalidate the test.
 */
static void scenario_bsp_balance_area(void)
{
	struct Window *a;
	struct Window *b;
	struct Window *c;
	struct Output *out;
	struct Seat *seat;
	int total_area;
	int per_leaf;
	int area_a;
	int area_b;
	int area_c;

	harness_init("(defcfg (layout bsp) (borders :width 0))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();

	a = harness_add_window("kitty");
	b = harness_add_window("kitty");
	c = harness_add_window("kitty");
	harness_cycle();

	area_a = a->last_proposed_w * a->last_proposed_h;
	area_b = b->last_proposed_w * b->last_proposed_h;
	area_c = c->last_proposed_w * c->last_proposed_h;
	assert(area_a == 960 * 540);
	assert(area_b == 960 * 540);
	assert(area_c == 960 * 1080);

	harness_press(seat, ACTION_LAYOUT_VERB, "balance-area");
	harness_cycle();

	/*
	 * Post-balance: each leaf gets total_area / n_leaves. Allow
	 * a few pixels of slack on each axis to absorb integer
	 * rounding of the 2/3 ratio at sizes that do not divide
	 * evenly. With 1920 the math comes out exact: 1920*2/3 =
	 * 1280, leaving each leaf at exactly 691200.
	 */
	total_area = out->width * out->height;
	per_leaf = total_area / 3;
	area_a = a->last_proposed_w * a->last_proposed_h;
	area_b = b->last_proposed_w * b->last_proposed_h;
	area_c = c->last_proposed_w * c->last_proposed_h;
	assert(area_a >= per_leaf - 2 * out->height);
	assert(area_a <= per_leaf + 2 * out->height);
	assert(area_b >= per_leaf - 2 * out->height);
	assert(area_b <= per_leaf + 2 * out->height);
	assert(area_c >= per_leaf - 2 * out->height);
	assert(area_c <= per_leaf + 2 * out->height);
}

/*
 * Random walk over the action alphabet. Deterministic given a fixed
 * seed and step count, so any regression has a one-line reproducer
 * (./harness <seed> <steps>). harness_cycle runs harness_check_all
 * after every step, so the value here is in the structural and layout
 * invariants - the scenario itself asserts nothing window-specific.
 *
 * Action set is restricted to the ones safe to invoke without further
 * setup: spawn / close, focus, swap, workspace move / switch, toggle
 * float / fullscreen. ACTION_MOVE / ACTION_RESIZE need seat->hovered
 * to be non-NULL and are skipped here.
 */
static unsigned g_walk_seed = 0x1209a5u;
static int g_walk_steps = 400;

static void scenario_random_walk(void)
{
	enum {
		N_WINDOWS_MAX = 8,
	};
	struct Window *windows[N_WINDOWS_MAX];
	struct Output *out;
	struct Seat *seat;
	int n_windows;
	int step;

	srand(g_walk_seed);
	memset(windows, 0, sizeof(windows));
	harness_init("(defcfg (layout bsp))");

	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	harness_cycle();
	(void)out;

	n_windows = 0;
	for (step = 0; step < g_walk_steps; step++) {
		int op = rand() % 10;
		char arg[16];

		switch (op) {
		case 0:
			if (n_windows < N_WINDOWS_MAX) {
				windows[n_windows++] =
					harness_add_window("test");
			}
			break;
		case 1: {
			int idx;

			if (n_windows == 0)
				break;
			idx = rand() % n_windows;
			harness_close_window(windows[idx]);
			windows[idx] = windows[--n_windows];
			windows[n_windows] = NULL;
			break;
		}
		case 2:
			harness_press(seat, ACTION_FOCUS_NEXT, NULL);
			break;
		case 3:
			harness_press(seat, ACTION_FOCUS_PREV, NULL);
			break;
		case 4:
			harness_press(seat, ACTION_SWAP_NEXT, NULL);
			break;
		case 5:
			harness_press(seat, ACTION_SWAP_PREV, NULL);
			break;
		case 6:
			snprintf(arg, sizeof(arg), "%d",
				 (rand() % (int)wm.n_workspaces) + 1);
			harness_press(seat, ACTION_WORKSPACE, arg);
			break;
		case 7:
			snprintf(arg, sizeof(arg), "%d",
				 (rand() % (int)wm.n_workspaces) + 1);
			harness_press(seat, ACTION_MOVE_TO_WORKSPACE, arg);
			break;
		case 8:
			harness_press(seat,
				      (rand() & 1) ? ACTION_TOGGLE_FLOAT :
						     ACTION_TOGGLE_FULLSCREEN,
				      NULL);
			break;
		case 9:
			harness_press(seat, ACTION_TOGGLE_SCRATCHPAD, NULL);
			break;
		}
		harness_cycle();
	}
}

/* ---- perf storm ------------------------------------------------------ */

/*
 * Mirrors the holding-A+enter-then-A+q workload that surfaces the
 * river "imperfect frames" warning: spawn N windows one per cycle
 * (cycle K touches K windows), then ACTION_CLOSE the focused one per
 * cycle (each press marks requested_close on focused and the next
 * cycle's apply_layout re-tiles the survivors), then deliver the
 * server-side .closed event one per cycle (each fires
 * window_maybe_destroy and a re-layout of the remaining tiles). Three
 * phases x N cycles each.
 *
 * Invariants are disabled (O(n^2) overlap + tile-extent checks would
 * dominate timing). Cycle stats accumulate across all three phases
 * so the caller can see how request count and wall-clock scale with
 * N. ./harness --bench [N] runs this with N defaulting to 50.
 */
static int g_storm_n = 50;

static void scenario_perf_storm(void)
{
	struct Window **windows;
	struct Output *out;
	struct Seat *seat;
	int i;

	harness_set_invariants(false);
	harness_stats_enable();

	harness_init("(defcfg (layout bsp))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	(void)out;
	harness_cycle();

	windows = calloc((size_t)g_storm_n, sizeof(*windows));
	if (windows == NULL)
		abort();

	for (i = 0; i < g_storm_n; i++) {
		windows[i] = harness_add_window("term");
		harness_cycle();
	}

	for (i = 0; i < g_storm_n; i++) {
		harness_press(seat, ACTION_CLOSE, NULL);
		harness_cycle();
	}

	for (i = 0; i < g_storm_n; i++) {
		harness_close_window(windows[i]);
		harness_cycle();
	}

	free(windows);
}

/* ---- status broadcast IPC -------------------------------------------- */

/*
 * Drive the status broadcast end-to-end: set a temp socket path,
 * call status_init, connect a subscriber over AF_UNIX, run a few
 * cycles that mutate publishable state, and verify that the
 * subscriber receives a logfmt frame on each change with the
 * frame_start / frame_end markers.
 *
 * Also covers the dedupe path: a second cycle with the same state
 * should not re-broadcast.
 */
static void scenario_status_broadcast(void)
{
	static const char sock_path[] =
		"/tmp/nomonad-status-harness.sock";
	struct sockaddr_un addr;
	struct Output *out;
	struct Seat *seat;
	struct Window *w1;
	int client;
	ssize_t n;
	char buf[4096];
	size_t total;
	int flags;

	/*
	 * status_init looks at XDG_RUNTIME_DIR and falls back to
	 * /tmp. Pin XDG_RUNTIME_DIR to the harness path's directory
	 * so the path lands at sock_path. /tmp is not writable from
	 * the path-build helper directly, but the fallback formats
	 * /tmp/nomonad-status-<uid>.sock - we avoid that by exporting
	 * a private XDG_RUNTIME_DIR.
	 */
	(void)unlink(sock_path);
	setenv("XDG_RUNTIME_DIR", "/tmp", 1);
	(void)unlink("/tmp/nomonad-status.sock");

	harness_init("(defcfg (layout tall))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	(void)out;
	(void)seat;
	harness_cycle();

	if (status_init() != 0) {
		fprintf(stderr,
			"scenario_status_broadcast: status_init failed; skipping\n");
		return;
	}

	client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(client >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, "/tmp/nomonad-status.sock",
		sizeof(addr.sun_path) - 1);
	if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr,
			"scenario_status_broadcast: connect failed: %s\n",
			strerror(errno));
		close(client);
		status_shutdown();
		return;
	}
	flags = fcntl(client, F_GETFL, 0);
	if (flags >= 0)
		fcntl(client, F_SETFL, flags | O_NONBLOCK);

	/*
	 * Drive the accept side: nudge harness_cycle so the next
	 * publish has somewhere to send. status_handle_events on the
	 * server fd accepts the pending connection.
	 */
	{
		/* 1 server fd + up to 16 subscribers; 32 is comfortable. */
		struct pollfd pfds[32];
		size_t k = status_fill_pollfds(pfds, 32);
		if (k > 0) {
			(void)poll(pfds, k, 0);
			status_handle_events(pfds, k);
		}
	}

	/*
	 * Add a window and cycle. Publishable state changed (window
	 * appeared, focus follows). Read back from the subscriber and
	 * assert we saw a frame.
	 */
	w1 = harness_add_window("foot");
	(void)w1;
	harness_cycle();

	/*
	 * Drain the publish bytes fully. Loop reads until EAGAIN so a
	 * partial first read does not leave bytes in the socket that
	 * the second-cycle assertion would mistake for a re-publish.
	 */
	total = 0;
	for (;;) {
		n = read(client, buf + total, sizeof(buf) - total - 1);
		if (n > 0) {
			total += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		break;
	}
	buf[total] = '\0';
	assert(total > 0);
	assert(strstr(buf, "type=frame_start") != NULL);
	assert(strstr(buf, "type=frame_end") != NULL);
	assert(strstr(buf, "type=window") != NULL);
	assert(strstr(buf, "app_id=foot") != NULL);
	/*
	 * Timestamps: must be present, must be monotonically ordered
	 * (start <= end), and must be plausible wall-clock unix_ns
	 * (i.e. > 1.7e18 = 2024-01-01).
	 */
	{
		const char *p_start, *p_end;
		unsigned long long t_start, t_end;

		p_start = strstr(buf, "time_start_unix_ns=");
		p_end = strstr(buf, "time_end_unix_ns=");
		assert(p_start != NULL);
		assert(p_end != NULL);
		assert(sscanf(p_start, "time_start_unix_ns=%llu", &t_start) == 1);
		assert(sscanf(p_end, "time_end_unix_ns=%llu", &t_end) == 1);
		assert(t_start >= 1700000000000000000ull);
		assert(t_end >= t_start);
	}

	/*
	 * Second cycle with no state change: dedupe should swallow
	 * the publish. Read should immediately return EAGAIN.
	 */
	harness_cycle();
	n = read(client, buf, sizeof(buf));
	if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
		buf[n > 0 ? n : 0] = '\0';
		fprintf(stderr,
			"scenario_status_broadcast: dedupe failed; got n=%zd:\n%s\n",
			n, buf);
		abort();
	}

	close(client);
	status_shutdown();
	(void)unlink("/tmp/nomonad-status.sock");
}

/* ---- quiet-cycle bench ----------------------------------------------- */

/*
 * scenario_perf_quiet measures the steady-state floor: hold focus-next
 * on a populated workspace and see what each manage / render cycle
 * costs when no window add / close / app-id / title / parent change
 * fires.
 *
 * The headless cycle covers the same code paths a real WAYLAND
 * cycle does: manage_start runs the reaper sweep, window_manage,
 * apply_rules, seat_manage (which dispatches the focus-next action
 * and updates seat->focused), update_visibility, apply_layout. No
 * real wayland fd is involved, but every wl_proxy_marshal_flags
 * call still routes through the harness's fakewl wrapper, so the
 * request-count axis remains honest.
 *
 * Used as a baseline for the dirty-status pruning work: this is the
 * "before" number; gating apply_rules / update_visibility /
 * apply_layout on the dirty flag
 * should bring it down. ./harness --bench-quiet [N] runs with N
 * defaulting to 50 and 4096 cycle iterations.
 */

static int g_quiet_n = 50;
static int g_quiet_iters = 4096;

static void scenario_perf_quiet(void)
{
	struct Window **windows;
	struct Output *out;
	struct Seat *seat;
	int i;

	harness_set_invariants(false);

	/*
	 * BSP rather than TALL: matches scenario_perf_storm so the two
	 * benches share a layout choice and stay directly comparable.
	 */
	harness_init("(defcfg (layout bsp))");
	out = harness_add_output(0, 0, 1920, 1080);
	seat = harness_add_seat(out);
	(void)out;
	harness_cycle();

	windows = calloc((size_t)g_quiet_n, sizeof(*windows));
	if (windows == NULL)
		abort();

	for (i = 0; i < g_quiet_n; i++) {
		windows[i] = harness_add_window("term");
		harness_cycle();
	}

	/*
	 * Stats start AFTER the windows are settled so the spawn cycles
	 * (which legitimately do work) do not pollute the steady-state
	 * percentiles. Every recorded cycle from here is a focus-next
	 * dispatch with a fully populated layout and no other work.
	 */
	harness_stats_enable();
	for (i = 0; i < g_quiet_iters; i++) {
		harness_press(seat, ACTION_FOCUS_NEXT, NULL);
		harness_cycle();
	}

	free(windows);
}

/* ---- mixed-workload session bench ------------------------------------ */

/*
 * scenario_perf_storm in 30 lines is a microbench tuned to spot
 * algorithmic O(N^2) growth on the spawn / close hot path. It runs on
 * one output, one workspace, all windows app_id="term", no rule
 * matches, no focus changes, no manipulations - so it covers exactly
 * one code path well. scenario_perf_session is the complementary
 * benchmark for "real-shaped" workloads:
 *
 *   Phase 1  spawn N windows distributed across 4 user workspaces, with
 *            short focus-next chains between spawns so the BSP tree
 *            subdivides at varied leaves rather than chaining off
 *            last_inserted. Switches between workspaces on output A,
 *            forcing workspace_swap between A and B repeatedly.
 *   Phase 2  per-workspace BSP manipulations: balance, expand-l, swap,
 *            rotate, equalize. Exercises layout->do_action verbs.
 *   Phase 3  move N/4 windows to a different output via
 *            move-to-output. The source workspace becomes invisible
 *            (since it is the previously-visible-on-A swapped out),
 *            stranding stale leaves in its BSP tree until the next
 *            visible-apply reaps them. Touches the multi-tree
 *            residency property the layout has by design.
 *   Phase 4  visit every workspace via cycling so each invisible
 *            workspace's apply_layout runs and bsp_drop_unwanted reaps
 *            the stranded leaves. Surfaces "huge cleanup cost on
 *            workspace switch" if drop is hot under move churn.
 *   Phase 5  close every window one per cycle (server-side .closed
 *            event path, not the ACTION_CLOSE request path).
 *
 * Per-phase latency / request-count stats are printed between phases
 * so the tradeoff is visible: phase 2 stresses do_action, phase 3
 * stresses move + invisible-workspace tree growth, phase 4 stresses
 * drop_unwanted on stale-leaf-heavy trees, etc.
 */

enum {
	SESSION_OUTPUTS = 2,
	SESSION_USER_WS = 4,
};

static int g_session_n = 40;

static void session_print_phase(const char *label)
{
	fprintf(stdout, "[%s]\n", label);
	harness_stats_print(stdout);
	harness_stats_enable(); /* resets the sample buffer for the next phase */
}

static void scenario_perf_session(void)
{
	struct Output *outputs[SESSION_OUTPUTS];
	struct Window **windows;
	struct Seat *seat;
	int i;

	harness_set_invariants(false);

	harness_init("(defcfg (layout bsp))");
	outputs[0] = harness_add_output(0, 0, 1920, 1080);
	outputs[1] = harness_add_output(1920, 0, 1920, 1080);
	seat = harness_add_seat(outputs[0]);
	harness_cycle();
	/*
	 * Stats start AFTER setup so the one-time workspace / output
	 * initialisation does not pollute phase 1 numbers.
	 */
	harness_stats_enable();

	windows = calloc((size_t)g_session_n, sizeof(*windows));
	if (windows == NULL)
		abort();

	/* Phase 1: spawn-with-focus across workspaces. */
	{
		int wsi = 0;
		for (i = 0; i < g_session_n; i++) {
			int target = i % SESSION_USER_WS;
			char arg[8];

			if (target != wsi) {
				snprintf(arg, sizeof(arg), "%d", target + 1);
				harness_press(seat, ACTION_WORKSPACE, arg);
				harness_cycle();
				wsi = target;
			}
			windows[i] = harness_add_window("term");
			harness_cycle();
			/*
			 * Three focus-next presses between spawns: enough to
			 * advance last_inserted past the just-spawned leaf so
			 * the next spawn does not always chain off the most
			 * recent. With small per-workspace counts (N/4) most
			 * cycles wrap, but each press still updates
			 * set_anchor on the BSP layout.
			 */
			for (int k = 0; k < 3; k++) {
				harness_press(seat, ACTION_FOCUS_NEXT, NULL);
				harness_cycle();
			}
		}
	}
	session_print_phase("phase 1: spawn + focus chains");

	/* Phase 2: BSP manipulations on each user workspace. */
	{
		static const char *const verbs[] = {
			"balance", "expand-l", "swap", "rotate", "equalize",
		};
		size_t n_verbs = sizeof(verbs) / sizeof(verbs[0]);

		for (i = 0; i < SESSION_USER_WS; i++) {
			char arg[8];

			snprintf(arg, sizeof(arg), "%d", i + 1);
			harness_press(seat, ACTION_WORKSPACE, arg);
			harness_cycle();
			for (size_t k = 0; k < n_verbs; k++) {
				harness_press(seat, ACTION_LAYOUT_VERB,
					      verbs[k]);
				harness_cycle();
			}
		}
	}
	session_print_phase("phase 2: BSP manipulations");

	/* Phase 3: move N/4 windows to the other output. */
	{
		int n_moves = g_session_n / 4;
		harness_pointer_to(seat, outputs[0]);
		for (i = 0; i < n_moves; i++) {
			harness_press(seat, ACTION_MOVE_TO_OUTPUT, "1");
			harness_cycle();
			harness_press(seat, ACTION_FOCUS_NEXT, NULL);
			harness_cycle();
		}
	}
	session_print_phase("phase 3: move-to-output churn");

	/* Phase 4: cycle through every user workspace so invisible-tree
	 * stale leaves reach apply_layout and get reaped. */
	{
		for (i = 0; i < SESSION_USER_WS; i++) {
			char arg[8];

			snprintf(arg, sizeof(arg), "%d", i + 1);
			harness_press(seat, ACTION_WORKSPACE, arg);
			harness_cycle();
		}
	}
	session_print_phase("phase 4: workspace cycle (cleanup)");

	/* Phase 5: close all. */
	for (i = 0; i < g_session_n; i++) {
		if (windows[i] != NULL && !windows[i]->closed) {
			harness_close_window(windows[i]);
			harness_cycle();
		}
	}
	session_print_phase("phase 5: close all");

	free(windows);
}

/* ---- runner ---------------------------------------------------------- */

typedef void (*scenario_fn)(void);

struct scenario {
	const char *name;
	scenario_fn fn;
};

static int harness_run(const struct scenario *s)
{
	int status;
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: fork: %s\n", s->name, strerror(errno));
		return 1;
	}
	if (pid == 0) {
		s->fn();
		_exit(0);
	}
	status = 0;
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "%s: waitpid: %s\n", s->name, strerror(errno));
		return 1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stdout, "%s: FAIL\n", s->name);
		return 1;
	}
	fprintf(stdout, "%s: ok\n", s->name);
	return 0;
}

/*
 * In-process runner: each scenario calls its body, then harness_teardown,
 * then the next scenario. No forks, so an assertion failure aborts the
 * whole process - lose isolation but gain ASan leak detection at
 * normal exit (fork-then-_exit skips the leak check).
 */
static int harness_run_inproc(const struct scenario *scenarios, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		scenarios[i].fn();
		harness_teardown();
		fprintf(stdout, "%s: ok\n", scenarios[i].name);
	}
	return 0;
}

/*
 * Usage:
 *   ./harness [--inproc | --bench [N] | --bench-session [N] |
 *              --bench-quiet [N [iters]]] [seed [steps]]
 *
 * --inproc runs scenarios sequentially in one process with
 * harness_teardown between each, so ASan / libFuzzer-style leak checks
 * see the whole picture. Without it, each scenario is forked off so
 * an assertion in one does not abort the rest.
 *
 * --bench runs only scenario_perf_storm at the given N (default 50)
 * with invariants disabled and per-cycle timing + request stats
 * printed at exit. Microbench tuned to spot O(N^2) on the spawn /
 * close hot path; uniform workload, single output, single workspace.
 *
 * --bench-session runs scenario_perf_session at the given N (default
 * 40). Mixed workload: 2 outputs, 4 workspaces, spawn-with-focus
 * chains, BSP manipulations, move-to-output churn, workspace cycle
 * cleanup, mass close. Per-phase stats so each kind of work shows up
 * separately. Use this for "is the realistic mix expensive?" rather
 * than "where is O(N^2)?".
 *
 * --bench-quiet runs scenario_perf_quiet at the given N (default 50)
 * and iters (default 4096). Spawns N windows once, then loops iters
 * cycles each pressing focus-next-only. Measures the steady-state
 * floor: per-cycle cost when the user is just navigating focus on a
 * populated workspace. Used as the "before" number for the
 * dirty-status pruning work.
 *
 * --inproc, --bench, --bench-session, --bench-quiet are mutually
 * exclusive. Pair any of the bench modes with `make perf` and
 * `perf record` to find hot spots.
 *
 * Otherwise: every scenario runs with its default config; the random
 * walk uses a baked-in seed and 400 steps. Pass a seed (decimal or
 * 0xhex) to reproduce a specific random walk; optionally a step count
 * to extend it. The other scenarios are unaffected.
 */
int main(int argc, char **argv)
{
	static const struct scenario scenarios[] = {
		{ "scenario_focus_and_move", scenario_focus_and_move },
		{ "scenario_workspace_swap_two_outputs",
		  scenario_workspace_swap_two_outputs },
		{ "scenario_bsp_layout", scenario_bsp_layout },
		{ "scenario_bsp_balance", scenario_bsp_balance },
		{ "scenario_bsp_balance_area", scenario_bsp_balance_area },
		{ "scenario_output_unplug_replug",
		  scenario_output_unplug_replug },
		{ "scenario_output_unplug_two_outputs",
		  scenario_output_unplug_two_outputs },
		{ "scenario_float_rules", scenario_float_rules },
		{ "scenario_drag_move", scenario_drag_move },
		{ "scenario_drag_resize", scenario_drag_resize },
		{ "scenario_reload_action", scenario_reload_action },
		{ "scenario_hot_reload_layout_swap",
		  scenario_hot_reload_layout_swap },
		{ "scenario_hot_reload_clears_bindings",
		  scenario_hot_reload_clears_bindings },
		{ "scenario_dynamic_scratchpad_toggle",
		  scenario_dynamic_scratchpad_toggle },
		{ "scenario_named_scratchpad", scenario_named_scratchpad },
		{ "scenario_scratchpad_invisible_when_stashed",
		  scenario_scratchpad_invisible_when_stashed },
		{ "scenario_smart_chromeless_single_tile",
		  scenario_smart_chromeless_single_tile },
		{ "scenario_smart_borders_only", scenario_smart_borders_only },
		{ "scenario_deck_layout", scenario_deck_layout },
		{ "scenario_mirror_tall_layout", scenario_mirror_tall_layout },
		{ "scenario_three_columns_layout",
		  scenario_three_columns_layout },
		{ "scenario_grid_layout", scenario_grid_layout },
		{ "scenario_tabbed_layout", scenario_tabbed_layout },
		{ "scenario_spiral_layout", scenario_spiral_layout },
		{ "scenario_two_pane_layout", scenario_two_pane_layout },
		{ "scenario_resizable_tall_layout",
		  scenario_resizable_tall_layout },
		{ "scenario_layout_cycle", scenario_layout_cycle },
		{ "scenario_layout_cycle_single_entry_noop",
		  scenario_layout_cycle_single_entry_noop },
		{ "scenario_layout_cycle_hot_reload",
		  scenario_layout_cycle_hot_reload },
		{ "scenario_bsp_split_axis_stable",
		  scenario_bsp_split_axis_stable },
		{ "scenario_submap_enter_dispatch_exit",
		  scenario_submap_enter_dispatch_exit },
		{ "scenario_submap_reload_drops_submap",
		  scenario_submap_reload_drops_submap },
		{ "scenario_reload_drops_stashed_named_pad",
		  scenario_reload_drops_stashed_named_pad },
		{ "scenario_reload_keeps_layout_state",
		  scenario_reload_keeps_layout_state },
		{ "scenario_layer_shell_usable_area",
		  scenario_layer_shell_usable_area },
		{ "scenario_refused_close_recovers",
		  scenario_refused_close_recovers },
		{ "scenario_resizable_tall_held_expand",
		  scenario_resizable_tall_held_expand },
		{ "scenario_three_columns_no_master_single",
		  scenario_three_columns_no_master_single },
		{ "scenario_float_follows_output",
		  scenario_float_follows_output },
		{ "scenario_fullscreen_float_restore_and_focus",
		  scenario_fullscreen_float_restore_and_focus },
		{ "scenario_output_replaced_in_one_batch",
		  scenario_output_replaced_in_one_batch },
		{ "scenario_input_devices_configured",
		  scenario_input_devices_configured },
		{ "scenario_move_to_workspace_follow",
		  scenario_move_to_workspace_follow },
		{ "scenario_focus_last", scenario_focus_last },
		{ "scenario_min_size_clipped_to_tile",
		  scenario_min_size_clipped_to_tile },
		{ "scenario_bsp_swap_moves_tiles",
		  scenario_bsp_swap_moves_tiles },
		{ "scenario_restart_roundtrip", scenario_restart_roundtrip },
		{ "scenario_restart_bad_state", scenario_restart_bad_state },
		{ "scenario_restart_saves_before_stop",
		  scenario_restart_saves_before_stop },
		{ "scenario_floats_stay_above_tiles",
		  scenario_floats_stay_above_tiles },
		{ "scenario_ctl_socket", scenario_ctl_socket },
		{ "scenario_float_lows", scenario_float_lows },
		{ "scenario_layer_default_survives_unplug",
		  scenario_layer_default_survives_unplug },
		{ "scenario_ctl_pipelined_and_stash",
		  scenario_ctl_pipelined_and_stash },
		{ "scenario_restart_deep_bsp", scenario_restart_deep_bsp },
		{ "scenario_trace_lock", scenario_trace_lock },
		{ "scenario_client_fullscreen_lows",
		  scenario_client_fullscreen_lows },
		{ "scenario_sticky", scenario_sticky },
		{ "scenario_keymap", scenario_keymap },
		{ "scenario_mru", scenario_mru },
		{ "scenario_client_fullscreen_real",
		  scenario_client_fullscreen_real },
		{ "scenario_output_dock_new_before_removed",
		  scenario_output_dock_new_before_removed },
		{ "scenario_focus_last_from_empty_ws",
		  scenario_focus_last_from_empty_ws },
		{ "scenario_output_focus_sticks_until_crossing",
		  scenario_output_focus_sticks_until_crossing },
		{ "scenario_window_hints_and_capture",
		  scenario_window_hints_and_capture },
		{ "scenario_repeat_one_shot_and_reload",
		  scenario_repeat_one_shot_and_reload },
		{ "scenario_repeat_disarmed_on_submap_exit",
		  scenario_repeat_disarmed_on_submap_exit },
		{ "scenario_resize_delta_float", scenario_resize_delta_float },
		{ "scenario_status_broadcast", scenario_status_broadcast },
		{ "scenario_random_walk", scenario_random_walk },
	};
	size_t n_scenarios;
	bool inproc;
	bool bench;
	bool bench_session;
	bool bench_quiet;
	size_t n_fail;
	size_t i;
	int argi;

	n_scenarios = sizeof(scenarios) / sizeof(scenarios[0]);
	inproc = false;
	bench = false;
	bench_session = false;
	bench_quiet = false;
	argi = 1;
	if (argi < argc && strcmp(argv[argi], "--inproc") == 0) {
		inproc = true;
		argi++;
	} else if (argi < argc && strcmp(argv[argi], "--bench") == 0) {
		bench = true;
		argi++;
		if (argi < argc)
			g_storm_n = atoi(argv[argi++]);
	} else if (argi < argc &&
		   strcmp(argv[argi], "--bench-session") == 0) {
		bench_session = true;
		argi++;
		if (argi < argc)
			g_session_n = atoi(argv[argi++]);
	} else if (argi < argc &&
		   strcmp(argv[argi], "--bench-quiet") == 0) {
		bench_quiet = true;
		argi++;
		if (argi < argc)
			g_quiet_n = atoi(argv[argi++]);
		if (argi < argc)
			g_quiet_iters = atoi(argv[argi++]);
	}
	if (!bench && !bench_session && !bench_quiet && argi < argc)
		g_walk_seed = (unsigned)strtoul(argv[argi++], NULL, 0);
	if (!bench && !bench_session && !bench_quiet && argi < argc)
		g_walk_steps = atoi(argv[argi++]);

	if (bench_session) {
		fprintf(stdout, "perf session: N=%d\n", g_session_n);
		scenario_perf_session();
		harness_stats_print(stdout);
		harness_teardown();
		return 0;
	}
	if (bench) {
		fprintf(stdout, "perf storm: N=%d\n", g_storm_n);
		scenario_perf_storm();
		harness_stats_print(stdout);
		harness_teardown();
		return 0;
	}
	if (bench_quiet) {
		fprintf(stdout, "perf quiet: N=%d iters=%d\n", g_quiet_n,
			g_quiet_iters);
		scenario_perf_quiet();
		harness_stats_print(stdout);
		harness_teardown();
		return 0;
	}

	if (inproc)
		return harness_run_inproc(scenarios, n_scenarios);

	n_fail = 0;
	for (i = 0; i < n_scenarios; i++)
		n_fail += (size_t)harness_run(&scenarios[i]);
	if (n_fail > 0) {
		fprintf(stderr, "harness: %zu/%zu scenarios failed\n", n_fail,
			n_scenarios);
		return 1;
	}
	return 0;
}
