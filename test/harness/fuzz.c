/* SPDX-License-Identifier: 0BSD */
/*
 * libFuzzer entry point. Each input is a byte sequence over an action
 * alphabet that covers everything scenario_random_walk dispatches plus
 * the cheap-to-add actions identified in the spec roadmap. The first
 * byte of each step picks the op (mod N_OPS); ops that take an
 * argument consume one additional byte for it.
 *
 * Op table (all routed through harness_press unless noted):
 *   0  add a window (no arg, simulated server window event)
 *   1  close a window by index (arg byte selects index, simulated
 *      server-side close event - distinct from ACTION_CLOSE below)
 *   2  ACTION_FOCUS_NEXT
 *   3  ACTION_FOCUS_PREV
 *   4  ACTION_SWAP_NEXT
 *   5  ACTION_SWAP_PREV
 *   6  ACTION_WORKSPACE         (arg byte mod n_workspaces, 1-based)
 *   7  ACTION_MOVE_TO_WORKSPACE[_FOLLOW] (arg byte mod n_workspaces,
 *      1-based, high bit picks follow)
 *   8  ACTION_TOGGLE_FLOAT or ACTION_TOGGLE_FULLSCREEN (arg bit picks)
 *   9  ACTION_CLOSE             (close-focused; client-initiated path,
 *                                idempotent if already requested_close)
 *  10  ACTION_FOCUS_MASTER
 *  11  ACTION_NEXT_WORKSPACE
 *  12  ACTION_PREV_WORKSPACE
 *  13  ACTION_SHRINK_MASTER     (TALL only; no-op on BSP)
 *  14  ACTION_EXPAND_MASTER     (TALL only; no-op on BSP)
 *  15  ACTION_INC_MASTER        (TALL only; no-op on BSP)
 *  16  ACTION_DEC_MASTER        (TALL only; no-op on BSP)
 *  17  ACTION_LAYOUT_VERB       (arg byte mod N_LAYOUT_VERBS picks verb)
 *  18  ACTION_RELOAD_CONFIG     (no-op against empty staging slot)
 *  19  add_output               (arg byte mod N_OUTPUT_SPECS picks
 *                                a position/size from a table of
 *                                non-overlapping monitor layouts)
 *  20  remove_output            (arg byte mod n_outputs; allows
 *                                draining to zero outputs)
 *  21  pointer_to_output        (arg byte mod n_outputs; mirrors
 *                                the river pointer_position event
 *                                so seat->output flips for the next
 *                                action dispatch)
 *  22  ACTION_FOCUS_OUTPUT      (arg byte snprintf'd; production
 *                                handles out-of-range via NULL)
 *  23  ACTION_MOVE_TO_OUTPUT    (same)
 *  24  pointer_enter on a window (arg byte mod n_windows; sets
 *                                seat->hovered, unlocks ACTION_MOVE
 *                                and ACTION_RESIZE)
 *  25  pointer_leave            (clears seat->hovered)
 *  26  start drag               (arg byte: low nibble != 0 starts a
 *                                resize with those edges, low nibble
 *                                == 0 starts a move; high nibble of
 *                                a second arg byte mod n_windows
 *                                picks the target)
 *  27  op_delta                 (two arg bytes interpreted as int8
 *                                pointer dx/dy in pixels)
 *  28  op_release               (ends the in-progress op)
 *  29  ACTION_MOVE              (only fires if seat->hovered was set
 *                                via op 24 and seat is idle)
 *  30  ACTION_RESIZE            (same; resize edges hardcoded to
 *                                BOTTOM | RIGHT in the action handler)
 *  31  ACTION_TOGGLE_SCRATCHPAD (no spawn risk: stash/unstash the
 *                                focused window or the most recent
 *                                dynamic stash on the scratch slot)
 *  32  harness_set_usable       (two arg bytes: output, top reserve)
 *  33  harness_layer_focus      (arg byte mod 3)
 *  34  ACTION_FOCUS_LAST
 *  35  seat removed and re-added (arg: pointer_move_requested first)
 *  36  client fullscreen / exit_fullscreen request (arg: window, bit 7)
 *  37  age every close request past the grace period
 *  38  harness_interact (arg: window)
 *  39  ACTION_RESIZE_DELTA "right 30"
 *  40  ACTION_TOGGLE_STICKY
 *  41  MRU step as if from a binding holding mod1 (arg bit 0: prev)
 *  42  modifiers_update releasing mod1
 *
 * One invocation creates a fresh harness state, walks the input, and
 * tears down. harness_check_all runs inside harness_cycle after every
 * step, so any structural / layout invariant violation aborts and is
 * reported by libFuzzer with the offending input.
 *
 * Excluded by design and should stay excluded:
 *   - ACTION_SPAWN (would fork+exec a child)
 *   - ACTION_EXIT (would kill the harness)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>

#include "harness_lib.h"

enum {
	N_WINDOWS_MAX = 8,
	N_OUTPUTS_MAX = 4,
	N_OPS = 43,
	N_LAYOUT_VERBS = 13,
	N_OUTPUT_SPECS = 4,
};

/*
 * Verb table mirrors src/layout.c's bsp_adjust_verbs plus the tree-wide
 * ops (rotate, swap, balance, rotate-cw, rotate-ccw). Unknown verbs are
 * a documented no-op in layout_bsp_do_action, so the indexer can be
 * any byte.
 */
static const char *const layout_verbs[N_LAYOUT_VERBS] = {
	"expand-r", "expand-l", "expand-u", "expand-d",
	"shrink-r", "shrink-l", "shrink-u", "shrink-d",
	"rotate",   "swap",     "balance",  "rotate-cw",
	"rotate-ccw",
};

/*
 * Non-overlapping monitor layouts the fuzzer's add_output op draws
 * from. The positions form a 2x2 grid so every combination of one to
 * four outputs lands at distinct screen regions, which mirrors how
 * production receives outputs from river (compositor-config, distinct
 * coords). Sizes vary so harness_check_tiles_inside_output catches
 * anything that confuses output dimensions across instances.
 */
static const struct {
	int x;
	int y;
	int w;
	int h;
} output_specs[N_OUTPUT_SPECS] = {
	{    0,    0, 1920, 1080 },
	{ 1920,    0, 1280,  720 },
	{    0, 1080, 2560, 1440 },
	{ 2560, 1440, 1920, 1080 },
};

static uint8_t consume(const uint8_t *data, size_t size, size_t *pos)
{
	if (*pos >= size)
		return 0;
	return data[(*pos)++];
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct Output *outputs[N_OUTPUTS_MAX];
	struct Window *windows[N_WINDOWS_MAX];
	struct Seat *seat;
	int n_outputs;
	int n_windows;
	size_t pos;
	size_t k;

	for (k = 0; k < N_WINDOWS_MAX; k++)
		windows[k] = NULL;
	for (k = 0; k < N_OUTPUTS_MAX; k++)
		outputs[k] = NULL;
	n_windows = 0;
	n_outputs = 0;

	harness_init("(defcfg (layout bsp))");
	harness_enable_layer_shell();
	outputs[n_outputs++] = harness_add_output(
		output_specs[0].x, output_specs[0].y,
		output_specs[0].w, output_specs[0].h);
	seat = harness_add_seat(outputs[0]);
	harness_cycle();

	pos = 0;
	while (pos < size) {
		char arg[16];
		uint8_t op;
		uint8_t aux;

		op = consume(data, size, &pos) % N_OPS;
		switch (op) {
		case 0:
			if (n_windows < N_WINDOWS_MAX)
				windows[n_windows++] = harness_add_window("test");
			break;
		case 1: {
			int idx;

			if (n_windows == 0)
				break;
			aux = consume(data, size, &pos);
			idx = aux % n_windows;
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
			aux = consume(data, size, &pos);
			snprintf(arg, sizeof(arg), "%d",
				 (aux % (int)wm.n_workspaces) + 1);
			harness_press(seat, ACTION_WORKSPACE, arg);
			break;
		case 7:
			aux = consume(data, size, &pos);
			snprintf(arg, sizeof(arg), "%d",
				 (aux % (int)wm.n_workspaces) + 1);
			harness_press(seat,
				      (aux & 0x80) ?
					      ACTION_MOVE_TO_WORKSPACE_FOLLOW :
					      ACTION_MOVE_TO_WORKSPACE,
				      arg);
			break;
		case 8:
			aux = consume(data, size, &pos);
			harness_press(seat,
				      (aux & 1) ? ACTION_TOGGLE_FLOAT :
						  ACTION_TOGGLE_FULLSCREEN,
				      NULL);
			break;
		case 9:
			harness_press(seat, ACTION_CLOSE, NULL);
			break;
		case 10:
			harness_press(seat, ACTION_FOCUS_MASTER, NULL);
			break;
		case 11:
			harness_press(seat, ACTION_NEXT_WORKSPACE, NULL);
			break;
		case 12:
			harness_press(seat, ACTION_PREV_WORKSPACE, NULL);
			break;
		case 13:
			harness_press(seat, ACTION_SHRINK_MASTER, NULL);
			break;
		case 14:
			harness_press(seat, ACTION_EXPAND_MASTER, NULL);
			break;
		case 15:
			harness_press(seat, ACTION_INC_MASTER, NULL);
			break;
		case 16:
			harness_press(seat, ACTION_DEC_MASTER, NULL);
			break;
		case 17:
			aux = consume(data, size, &pos);
			harness_press(seat, ACTION_LAYOUT_VERB,
				      layout_verbs[aux % N_LAYOUT_VERBS]);
			break;
		case 18:
			harness_press(seat, ACTION_RELOAD_CONFIG, NULL);
			break;
		case 19:
			if (n_outputs < N_OUTPUTS_MAX) {
				size_t s;

				aux = consume(data, size, &pos);
				s = aux % N_OUTPUT_SPECS;
				outputs[n_outputs++] = harness_add_output(
					output_specs[s].x, output_specs[s].y,
					output_specs[s].w, output_specs[s].h);
			}
			break;
		case 20: {
			int idx;

			if (n_outputs == 0)
				break;
			aux = consume(data, size, &pos);
			idx = aux % n_outputs;
			harness_remove_output(outputs[idx]);
			outputs[idx] = outputs[--n_outputs];
			outputs[n_outputs] = NULL;
			break;
		}
		case 21: {
			int idx;

			if (n_outputs == 0)
				break;
			aux = consume(data, size, &pos);
			idx = aux % n_outputs;
			harness_pointer_to(seat, outputs[idx]);
			break;
		}
		case 22:
			aux = consume(data, size, &pos);
			snprintf(arg, sizeof(arg), "%u", (unsigned)aux);
			harness_press(seat, ACTION_FOCUS_OUTPUT, arg);
			break;
		case 23:
			aux = consume(data, size, &pos);
			snprintf(arg, sizeof(arg), "%u", (unsigned)aux);
			harness_press(seat, ACTION_MOVE_TO_OUTPUT, arg);
			break;
		case 24: {
			int idx;

			if (n_windows == 0)
				break;
			aux = consume(data, size, &pos);
			idx = aux % n_windows;
			harness_pointer_enter(seat, windows[idx]);
			break;
		}
		case 25:
			harness_pointer_leave(seat);
			break;
		case 26: {
			uint8_t aux2;
			int idx;
			unsigned edges;

			if (n_windows == 0)
				break;
			aux = consume(data, size, &pos);
			aux2 = consume(data, size, &pos);
			idx = aux2 % n_windows;
			edges = aux & 0xfu;
			if (edges == 0)
				river_window_listener.pointer_move_requested(
					windows[idx], NULL, seat->obj);
			else
				river_window_listener.pointer_resize_requested(
					windows[idx], NULL, seat->obj,
					(u32)edges);
			break;
		}
		case 27: {
			int8_t dx;
			int8_t dy;

			dx = (int8_t)consume(data, size, &pos);
			dy = (int8_t)consume(data, size, &pos);
			harness_op_delta(seat, dx, dy);
			break;
		}
		case 28:
			harness_op_release(seat);
			break;
		case 29:
			harness_press(seat, ACTION_MOVE, NULL);
			break;
		case 30:
			harness_press(seat, ACTION_RESIZE, NULL);
			break;
		case 31:
			harness_press(seat, ACTION_TOGGLE_SCRATCHPAD, NULL);
			break;
		case 32: {
			struct Output *o;
			s32 top;

			if (n_outputs == 0)
				break;
			aux = consume(data, size, &pos);
			o = outputs[aux % n_outputs];
			if (o->ls_output == NULL)
				break;
			top = (s32)(consume(data, size, &pos) % 64);
			harness_set_usable(o, o->x, o->y + top, o->width,
					   o->height - top);
			break;
		}
		case 33:
			if (seat->ls_seat == NULL)
				break;
			aux = consume(data, size, &pos);
			harness_layer_focus(seat, (enum LayerFocus)(aux % 3));
			break;
		case 34:
			harness_press(seat, ACTION_FOCUS_LAST, NULL);
			break;
		case 35:
			river_seat_listener.removed(seat, NULL);
			aux = consume(data, size, &pos);
			if (aux & 1 && n_windows > 0)
				river_window_listener.pointer_move_requested(
					windows[aux % n_windows], NULL,
					seat->obj);
			harness_cycle();
			seat = harness_add_seat(n_outputs > 0 ? outputs[0] :
								NULL);
			break;
		case 36:
			if (n_windows == 0)
				break;
			aux = consume(data, size, &pos);
			if (aux & 0x80)
				river_window_listener.exit_fullscreen_requested(
					windows[aux % n_windows], NULL);
			else
				river_window_listener.fullscreen_requested(
					windows[aux % n_windows], NULL, NULL);
			break;
		case 37:
			for (k = 0; k < (size_t)n_windows; k++)
				windows[k]->close_requested_ns -= 2000000000ULL;
			break;
		case 38:
			aux = consume(data, size, &pos);
			if (n_windows > 0)
				harness_interact(seat,
						 windows[aux % n_windows]);
			break;
		case 39:
			harness_press(seat, ACTION_RESIZE_DELTA, "right 30");
			break;
		case 40:
			harness_press(seat, ACTION_TOGGLE_STICKY, NULL);
			break;
		case 41:
			aux = consume(data, size, &pos);
			harness_press(seat,
				      (aux & 1) ? ACTION_FOCUS_MRU_PREV :
						  ACTION_FOCUS_MRU_NEXT,
				      NULL);
			seat->pending_mods = RIVER_SEAT_V1_MODIFIERS_MOD1;
			break;
		case 42:
			river_xkb_bindings_seat_listener.modifiers_update(
				seat, NULL, RIVER_SEAT_V1_MODIFIERS_MOD1, 0);
			break;
		}
		harness_cycle();
	}

	harness_teardown();
	return 0;
}
