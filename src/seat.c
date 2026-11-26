/* SPDX-License-Identifier: 0BSD */
/*
 * river_seat_v1 listener and Seat lifecycle: focus management, action
 * dispatch, the manage/render hooks the WM cycle calls into, and the
 * pointer-op kickoff helpers that window_manage forwards to.
 *
 * Auto-repeat lives here too: arm/disarm/drain wrap a per-seat
 * timerfd; the main loop drains it each iteration so an expired
 * repeat queues a pending action and asks the server for a manage
 * cycle.
 */

#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>

#include "action.h"
#include "binding.h"
#include "config.h"
#include "input.h"
#include "layout.h"
#include "nomonad.h"
#include "recorder.h"
#include "restore.h"
#include "spawnd.h"

/* Return the seat's currently-focused output, falling back to the
 * first known output. Returns NULL only if there are no outputs. */
static struct Output *seat_output(struct Seat *seat)
{
	struct Output *o;

	if (seat != NULL && seat->output != NULL)
		return seat->output;
	if (wl_list_empty(&wm.outputs))
		return NULL;
	o = wl_container_of(wm.outputs.next, o, link);
	return o;
}

/* Return the workspace currently visible on the seat's output. */
static struct Workspace *seat_ws(struct Seat *seat)
{
	struct Output *o = seat_output(seat);
	if (o == NULL || o->visible_workspace < 0 ||
	    (size_t)o->visible_workspace >= wm.n_workspaces)
		return NULL;
	return &wm.workspaces[o->visible_workspace];
}

static void seat_handle_removed(void *data, struct river_seat_v1 *obj)
{
	struct Seat *seat = data;
	seat->removed = true;
	recorder_event_seat_remove(seat);
}

static void seat_handle_pointer_enter(void *data, struct river_seat_v1 *obj,
				      struct river_window_v1 *river_window)
{
	struct Seat *seat = data;
	seat->hovered = river_window_v1_get_user_data(river_window);
	recorder_event_pointer_enter(seat, seat->hovered);
}

static void seat_handle_pointer_leave(void *data, struct river_seat_v1 *obj)
{
	struct Seat *seat = data;
	seat->hovered = NULL;
	recorder_event_pointer_leave(seat);
}

static void seat_handle_window_interaction(void *data,
					   struct river_seat_v1 *obj,
					   struct river_window_v1 *river_window)
{
	struct Seat *seat = data;
	seat->interacted = river_window_v1_get_user_data(river_window);
	if (seat->interacted != NULL)
		seat->interacted->requested_close = false;
	recorder_event_window_interaction(seat, seat->interacted);
}

static void seat_handle_op_delta(void *data, struct river_seat_v1 *obj, s32 dx,
				 s32 dy)
{
	struct Seat *seat = data;
	seat->op_dx = dx;
	seat->op_dy = dy;
	recorder_event_op_delta(seat, dx, dy);
}

static void seat_handle_op_release(void *data, struct river_seat_v1 *obj)
{
	struct Seat *seat = data;
	seat->op_release = true;
	recorder_event_op_release(seat);
}

static void seat_handle_wl_seat(void *data, struct river_seat_v1 *obj, u32 id)
{
}

static void seat_handle_shell_surface_interaction(
	void *data, struct river_seat_v1 *obj,
	struct river_shell_surface_v1 *river_shell_surface)
{
}

static void seat_handle_pointer_position(void *data, struct river_seat_v1 *obj,
					 s32 x, s32 y)
{
	struct Seat *seat = data;
	struct Output *prev = seat->output;
	struct Output *o;

	recorder_event_pointer_position(seat, x, y);
	wl_list_for_each(o, &wm.outputs, link) {
		if (o->removed)
			continue;
		if (x >= o->x && x < o->x + o->width && y >= o->y &&
		    y < o->y + o->height) {
			if (o == seat->pointer_output)
				return;
			seat->pointer_output = o;
			seat->output = o;
			if (o != prev) {
				wm.dirty_status = true;
				recorder_event_pointer_to(seat, o);
			}
			return;
		}
	}
}

const struct river_seat_v1_listener river_seat_listener = {
	.removed = seat_handle_removed,
	.wl_seat = seat_handle_wl_seat,
	.pointer_enter = seat_handle_pointer_enter,
	.pointer_leave = seat_handle_pointer_leave,
	.window_interaction = seat_handle_window_interaction,
	.shell_surface_interaction = seat_handle_shell_surface_interaction,
	.op_delta = seat_handle_op_delta,
	.op_release = seat_handle_op_release,
	.pointer_position = seat_handle_pointer_position,
};

/*
 * The user pressed a key not bound by the active submap. River has
 * eaten the press for us; we just need to tear the submap back down.
 * The teardown calls binding_seat_cleanup + binding_register_from_config
 * which issue protocol requests that must run inside a manage sequence,
 * so defer to seat_manage via the submap_exit_pending flag (same shape
 * as drain_repeat_timer).
 */
static void seat_handle_ate_unbound_key(void *data,
					struct river_xkb_bindings_seat_v1 *obj)
{
	struct Seat *seat = data;

	recorder_event_ate_unbound(seat);
	if (!seat->submap_active)
		return;
	seat->submap_exit_pending = true;
	if (window_manager_v1 != NULL)
		river_window_manager_v1_manage_dirty(window_manager_v1);
}

static void seat_handle_modifiers_update(void *data,
					 struct river_xkb_bindings_seat_v1 *obj,
					 u32 old, u32 now)
{
	struct Seat *seat = data;

	if (seat->mru_active && (now & seat->mru_watch) != seat->mru_watch)
		seat->mru_commit_pending = true;
}

const struct river_xkb_bindings_seat_v1_listener
	river_xkb_bindings_seat_listener = {
		.ate_unbound_key = seat_handle_ate_unbound_key,
		.modifiers_update = seat_handle_modifiers_update,
	};

static void
seat_handle_layer_focus_exclusive(void *data,
				  struct river_layer_shell_seat_v1 *obj)
{
	struct Seat *seat = data;

	seat->layer_focus = LAYER_FOCUS_EXCLUSIVE;
	wm.dirty_status = true;
	recorder_event_layer_focus(seat);
}

static void
seat_handle_layer_focus_non_exclusive(void *data,
				      struct river_layer_shell_seat_v1 *obj)
{
	struct Seat *seat = data;

	seat->layer_focus = LAYER_FOCUS_NON_EXCLUSIVE;
	wm.dirty_status = true;
	recorder_event_layer_focus(seat);
}

static void seat_handle_layer_focus_none(void *data,
					 struct river_layer_shell_seat_v1 *obj)
{
	struct Seat *seat = data;

	seat->layer_focus = LAYER_FOCUS_NONE;
	seat->refocus_pending = true;
	wm.dirty_status = true;
	recorder_event_layer_focus(seat);
}

const struct river_layer_shell_seat_v1_listener layer_shell_seat_listener = {
	.focus_exclusive = seat_handle_layer_focus_exclusive,
	.focus_non_exclusive = seat_handle_layer_focus_non_exclusive,
	.focus_none = seat_handle_layer_focus_none,
};

void seat_attach_layer_shell(struct Seat *seat)
{
	if (layer_shell_v1 == NULL || seat->ls_seat != NULL || seat->removed)
		return;
	seat->ls_seat =
		river_layer_shell_v1_get_seat(layer_shell_v1, seat->obj);
	if (seat->ls_seat != NULL)
		river_layer_shell_seat_v1_add_listener(
			seat->ls_seat, &layer_shell_seat_listener, seat);
}

/*
 * Dispatch ACTION_SPAWN. The fast path delegates to the long-lived
 * spawn helper (src/spawnd.c, forked at startup before libwayland
 * init). One writev to a pipe is ~1 us; the helper does the actual
 * fork+exec out of band, off the manage cycle. Real-session live
 * timing showed inline fork+exec was the dominant cost in the
 * manage long tail (every >=200 us cycle in a captured session was
 * SPAWN-triggered).
 *
 * The inline path below is the fallback for builds where spawnd_init
 * failed (no pipe, no helper). Same hygiene as before: the WM sets
 * SIGCHLD = SIG_IGN for zombie auto-reap, but POSIX has wait() return
 * ECHILD under that disposition - shells whose .zshrc forks prompt
 * pipelines (git status, kubectl context) hit the slow fallback unless
 * we restore SIGCHLD = SIG_DFL in the child. setsid() detaches the
 * controlling tty so SIGHUP from the launcher does not kill spawned
 * apps; stdin to /dev/null so the child cannot accidentally read from
 * the launcher's stdin.
 */
static void spawn_child(const char *cmd)
{
	if (cmd == NULL)
		return;
	if (spawnd_send_command(cmd) == 0)
		return;

	if (fork() != 0)
		return;

	spawnd_child_setup();
	spawnd_exec(cmd);
}

void seat_maybe_destroy(struct Seat *seat)
{
	if (!seat->removed)
		return;

	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			if (w->pointer_move_requested == seat)
				w->pointer_move_requested = NULL;
			if (w->pointer_resize_requested == seat)
				w->pointer_resize_requested = NULL;
		}
	}

	binding_seat_cleanup(seat);
	action_clear(&seat->pending_action);
	action_clear(&seat->repeat_action);
	free(seat->mru);
	if (seat->repeat_timer_fd >= 0) {
		close(seat->repeat_timer_fd);
		seat->repeat_timer_fd = -1;
	}

	if (seat->xkb_seat != NULL) {
		river_xkb_bindings_seat_v1_destroy(seat->xkb_seat);
		seat->xkb_seat = NULL;
	}
	if (seat->ls_seat != NULL) {
		river_layer_shell_seat_v1_destroy(seat->ls_seat);
		seat->ls_seat = NULL;
	}

	recorder_seat_freed(seat);
	river_seat_v1_destroy(seat->obj);
	wl_list_remove(&seat->link);
	free(seat);
}

/*
 * xkb keyboard repeat defaults: 250 ms before the first repeat, then
 * one fire every 40 ms (~25 Hz). Matches X11's typical defaults; close
 * enough to xmonad's daily-drive feel.
 */
#define NM_REPEAT_DELAY_MS 250
#define NM_REPEAT_INTERVAL_MS 40

void seat_repeat_arm(struct Seat *seat, const struct Action *action)
{
	struct itimerspec ts = { 0 };

	if (seat == NULL || seat->repeat_timer_fd < 0 || action == NULL)
		return;
	action_clear(&seat->repeat_action);
	seat->repeat_action = action_dup(action);
	ts.it_value.tv_sec = NM_REPEAT_DELAY_MS / 1000;
	ts.it_value.tv_nsec = (long)(NM_REPEAT_DELAY_MS % 1000) * 1000000L;
	ts.it_interval.tv_sec = NM_REPEAT_INTERVAL_MS / 1000;
	ts.it_interval.tv_nsec =
		(long)(NM_REPEAT_INTERVAL_MS % 1000) * 1000000L;
	timerfd_settime(seat->repeat_timer_fd, 0, &ts, NULL);
}

void seat_repeat_disarm(struct Seat *seat)
{
	struct itimerspec ts = { 0 };

	if (seat == NULL || seat->repeat_timer_fd < 0)
		return;
	timerfd_settime(seat->repeat_timer_fd, 0, &ts, NULL);
	action_clear(&seat->repeat_action);
}

/*
 * Non-blocking read of a seat's repeat timerfd. If at least one
 * expiration occurred since the last drain, queue the repeat action
 * for the next manage cycle. Multiple expirations collapse to one
 * fire so we never backlog if the WM was busy.
 */
void drain_repeat_timer(struct Seat *seat)
{
	u64 expirations;
	ssize_t r;

	if (seat == NULL || seat->repeat_timer_fd < 0)
		return;
	r = read(seat->repeat_timer_fd, &expirations, sizeof(expirations));
	if (r != (ssize_t)sizeof(expirations))
		return;
	if (seat->repeat_action.kind == ACTION_NONE)
		return;
	action_clear(&seat->pending_action);
	seat->pending_action = action_dup(&seat->repeat_action);
	recorder_event_repeat_press(seat, &seat->repeat_action);
	river_window_manager_v1_manage_dirty(window_manager_v1);
}

/*
 * Walk a workspace's window list starting from `from` (exclusive) in
 * the given direction, returning the first non-ignored, non-closed
 * window or NULL if every other entry is unfocusable. Wraps once if
 * needed. `from` may be the list head (sentinel) to scan from the
 * start without an anchor. If `wrapped_out` is non-NULL, it is set to
 * true when the walk crossed the list sentinel before finding a
 * candidate (the swap action uses this to decide between adjacent-
 * insert and far-end insert).
 *
 * Skipping `closed` matters when ACTION_CLOSE fires on a window whose
 * .closed event has not yet arrived: the window is still in the list
 * but in teardown, and focusing it would just re-route the next
 * keystroke to a dying target. Auto-repeat at speed (held A+q) used
 * to leave focus stuck on such a window when its sibling cells were
 * very small under BSP.
 */
static struct Window *next_focusable(struct wl_list *list, struct wl_list *from,
				     bool forward, bool *wrapped_out)
{
	struct wl_list *node = from;
	bool wrapped = false;
	struct Window *w;

	if (wrapped_out != NULL)
		*wrapped_out = false;
	if (wl_list_empty(list))
		return NULL;
	while (true) {
		node = forward ? node->next : node->prev;
		if (node == list) {
			if (wrapped)
				return NULL;
			wrapped = true;
			if (wrapped_out != NULL)
				*wrapped_out = true;
			continue;
		}
		if (node == from)
			return NULL;
		w = wl_container_of(node, w, workspace_link);
		if (!w->ignored && !w->closed && !w->requested_close)
			return w;
	}
}

static int raise_seq_cmp(const void *pa, const void *pb)
{
	const struct Window *a = *(struct Window *const *)pa;
	const struct Window *b = *(struct Window *const *)pb;

	return (a->raise_seq > b->raise_seq) - (a->raise_seq < b->raise_seq);
}

void seat_raise_floats(int wsi)
{
	struct Workspace *ws;
	struct Window **arr;
	struct Window *w;
	size_t n = 0;

	if (wsi < 0 || (size_t)wsi >= wm.n_workspaces)
		return;
	ws = &wm.workspaces[wsi];
	wl_list_for_each(w, &ws->windows, workspace_link)
		if (w->floating && !w->ignored && !w->closed)
			n++;
	if (n == 0)
		return;
	arr = malloc(n * sizeof(*arr));
	if (arr == NULL)
		return;
	n = 0;
	wl_list_for_each(w, &ws->windows, workspace_link)
		if (w->floating && !w->ignored && !w->closed)
			arr[n++] = w;
	qsort(arr, n, sizeof(*arr), raise_seq_cmp);
	for (size_t i = 0; i < n; i++)
		window_raise(arr[i]);
	free(arr);
}

static void mru_step(struct Seat *seat, int dir);
static void mru_commit(struct Seat *seat);

static void seat_focus(struct Seat *seat, struct Window *window)
{
	struct Workspace *ws = seat_ws(seat);
	bool take_from_layer;

	if (window != NULL && window->ignored)
		window = NULL;
	take_from_layer = window != NULL &&
			  seat->layer_focus == LAYER_FOCUS_NON_EXCLUSIVE;
	/*
	 * NULL means "no explicit target, pick a sensible default." If the
	 * seat already focuses a valid window on the visible workspace, keep
	 * it. xmonad-style: focus-next, focus-prev, swap-* set seat->focused
	 * directly; the manage cycle that follows must NOT snap focus back
	 * to the master. Only fall back to the head when the prior focus is
	 * stale, e.g. NULL, on a different workspace, just-closed (in which
	 * case window_maybe_destroy cleared seat->focused), or has just
	 * become ignored via a rule change.
	 */
	if (window == NULL) {
		if (seat->focused != NULL && !seat->focused->ignored &&
		    !seat->focused->closed && !seat->focused->requested_close &&
		    ws != NULL &&
		    seat->focused->workspace_index == ws->index)
			return;
		if (ws != NULL)
			window = next_focusable(&ws->windows, &ws->windows,
						true, NULL);
	}

	if (window != NULL && ws != NULL &&
	    window->workspace_index == ws->index) {
		struct Window *fs = wm_workspace_fullscreen(ws);

		if (fs != NULL && !fs->closed && !fs->requested_close)
			window = fs;
	}
	if (seat->focused == window && !take_from_layer)
		return;
	if (seat->mru_active && !seat->mru_stepping && window != seat->focused)
		mru_commit(seat);

	if (window != NULL) {
		river_seat_v1_focus_window(seat->obj, window->obj);
		window_raise(window);
		if (!window->floating && !window->fullscreen)
			seat_raise_floats(window->workspace_index);
	} else {
		river_seat_v1_clear_focus(seat->obj);
	}

	seat->focused = window;
	if (window != NULL && window != seat->focus_cur && !seat->mru_active) {
		if (seat->focus_cur != NULL)
			seat->focus_last = seat->focus_cur;
		if (seat->focus_last == window)
			seat->focus_last = NULL;
		seat->focus_cur = window;
	}
	if (window != NULL && !seat->mru_active)
		window->focus_seq = ++wm.next_focus_seq;
	wm.dirty_status = true;
	/*
	 * Layouts whose visible-set depends on focus (deck / monocle)
	 * need apply_layout to re-run. Tall and BSP do not - their
	 * tile rects are independent of which window inside the
	 * workspace has the focus indicator. The flag is per-layout
	 * so the focus-only fast path stays cheap on non-deck
	 * workspaces.
	 */
	if (ws != NULL && workspace_layout(ws)->focus_changes_visibility)
		wm.dirty_layout = true;

	/*
	 * Inform the workspace's layout of the new focus so stateful
	 * layouts (BSP) can update their split anchor. Critical for the
	 * "click leaf X then spawn" flow: river does not auto-advance
	 * seat->focused on auto-spawned windows, so without this hook
	 * BSP would either always re-target the original click (focused
	 * wins in bsp_insert) or always chain off the most recent spawn
	 * (last_inserted wins) - neither matches "explicit focus
	 * overrides, otherwise chain". This sets last_inserted to the
	 * focused leaf, and bsp_insert advances it on each spawn.
	 */
	if (window != NULL && ws != NULL &&
	    window->workspace_index == ws->index &&
	    workspace_layout(ws)->set_anchor != NULL) {
		struct Layout *active = workspace_layout(ws);

		active->set_anchor(active, window);
	}
}

/*
 * Promote a tiled window to floating so a drag/resize op does not
 * fight the layout. Without this, the layout's apply_layout pass each
 * cycle would snap the window back to its tile position. Idempotent
 * for already-floating windows.
 */
static void window_promote_floating(struct Window *w)
{
	if (w->floating && w->float_override == FLOAT_FORCE_FLOAT)
		return;
	w->float_override = FLOAT_FORCE_FLOAT;
	w->floating = true;
	wm.restack_floats = true;
	w->rules_dirty = true;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	w->float_x = w->x;
	w->float_y = w->y;
	if (w->width > 0)
		w->float_width = w->width;
	if (w->height > 0)
		w->float_height = w->height;
}

static void window_relink_ws(struct Window *w, int n)
{
	wl_list_remove(&w->workspace_link);
	wl_list_insert(&wm.workspaces[n].windows, &w->workspace_link);
	w->workspace_index = n;
	wm.dirty_layout = true;
	wm.dirty_status = true;
	wm.restack_floats = true;
}

static struct Output *output_containing(s32 x, s32 y)
{
	struct Output *o;

	wl_list_for_each(o, &wm.outputs, link) {
		if (!o->removed && x >= o->x && x < o->x + o->width &&
		    y >= o->y && y < o->y + o->height)
			return o;
	}
	return NULL;
}

static void seat_drop_float(struct Seat *seat)
{
	struct Window *w = seat->op_window;
	struct Output *o;

	o = output_containing(w->x + w->width / 2, w->y + w->height / 2);
	if (o == NULL || o->visible_workspace < 0 ||
	    o->visible_workspace == w->workspace_index)
		return;
	window_relink_ws(w, o->visible_workspace);
	wm_float_anchor_to(w, o);
}

static void seat_op_end(struct Seat *seat)
{
	if (seat->op == SEAT_OP_NONE)
		return;
	if (seat->op == SEAT_OP_RESIZE && seat->op_window != NULL)
		river_window_v1_inform_resize_end(seat->op_window->obj);
	river_seat_v1_op_end(seat->obj);
	seat->op = SEAT_OP_NONE;
	seat->op_window = NULL;
	seat->op_release = false;
}

static bool seat_op_valid(const struct Seat *seat)
{
	const struct Window *w = seat->op_window;

	return w != NULL && w->floating && !w->fullscreen && !w->ignored &&
	       w->workspace_index == seat->op_workspace;
}

void seat_pointer_move(struct Seat *seat, struct Window *window)
{
	/*
	 * Refuse to drag a dying or unfocusable window. Mirrors the
	 * ACTION_CLOSE guard in seat_action: requested_close windows are
	 * "in teardown" (.closed event pending from the server), and
	 * seat_focus below would otherwise pin focus on the dying target,
	 * which is the same anti-pattern next_focusable explicitly skips.
	 * Closed windows likewise must not become op_window because
	 * window_maybe_destroy may free them at the next manage cycle
	 * while seat->op_window still references them.
	 */
	if (window == NULL || window->closed || window->requested_close)
		return;
	seat_op_end(seat);
	seat_focus(seat, window);
	window_promote_floating(window);
	river_seat_v1_op_start_pointer(seat->obj);
	seat->op = SEAT_OP_MOVE;
	seat->op_window = window;
	seat->op_workspace = window->workspace_index;
	seat->op_start_x = window->x;
	seat->op_start_y = window->y;
	seat->op_dx = 0;
	seat->op_dy = 0;
}

void seat_pointer_resize(struct Seat *seat, struct Window *window, u32 edges)
{
	if (window == NULL || window->closed || window->requested_close)
		return;
	seat_op_end(seat);
	seat_focus(seat, window);
	window_promote_floating(window);
	river_window_v1_inform_resize_start(window->obj);
	river_seat_v1_op_start_pointer(seat->obj);
	seat->op = SEAT_OP_RESIZE;
	seat->op_window = window;
	seat->op_workspace = window->workspace_index;
	seat->op_edges = edges;
	seat->op_start_x = window->x;
	seat->op_start_y = window->y;
	seat->op_start_width = window->width;
	seat->op_start_height = window->height;
	seat->op_dx = 0;
	seat->op_dy = 0;
	/*
	 * No per-seat throttle: window_propose_dimensions itself skips
	 * redundant calls (last_proposed_w/h cache on the window). That
	 * breaks the propose-triggers-dimensions-triggers-manage_start
	 * feedback loop that pinned river/alacritty CPU during stationary
	 * holds before the throttle was added.
	 */
}

/*
 * Return the seat's focused window iff it is non-ignored and lives on
 * the seat's visible workspace; otherwise NULL. ws_out (optional)
 * receives the visible workspace regardless. Centralises the "validate
 * focus is on the workspace we're about to mutate" prelude shared by
 * focus-cycle, swap, toggle-fullscreen, and the layout-verb dispatch.
 */
static struct Window *seat_focus_on_visible_ws(struct Seat *seat,
					       struct Workspace **ws_out)
{
	struct Workspace *ws = seat_ws(seat);
	if (ws_out != NULL)
		*ws_out = ws;
	if (ws == NULL || seat->focused == NULL || seat->focused->ignored ||
	    seat->focused->closed || seat->focused->requested_close)
		return NULL;
	if (seat->focused->workspace_index != (int)(ws - wm.workspaces))
		return NULL;
	return seat->focused;
}

/*
 * Route a layout-specific verb through the focused workspace's layout
 * vtable. Most BSP verbs are anchored to focused (rotate, swap,
 * expand-*, shrink-*); the tree-wide ones (balance, rotate-cw,
 * rotate-ccw) tolerate NULL focused, which is why we dispatch even
 * when seat_focus_on_visible_ws returned NULL. No-op if the layout
 * has no do_action hook (e.g. TALL today).
 */
static void layout_verb_dispatch(struct Seat *seat, const char *verb)
{
	struct Window *focused;
	struct Workspace *ws;

	if (verb == NULL)
		return;
	focused = seat_focus_on_visible_ws(seat, &ws);
	if (ws == NULL)
		return;
	{
		struct Layout *active = workspace_layout(ws);

		if (active->do_action == NULL)
			return;
		active->do_action(active, focused, verb);
	}
}

static struct Window **workspace_snapshot(struct Workspace *ws)
{
	struct Window **order;
	struct Window *w;
	size_t n;
	size_t i;

	n = (size_t)wl_list_length(&ws->windows);
	order = malloc((n + 1) * sizeof(*order));
	if (order == NULL)
		return NULL;
	i = 0;
	wl_list_for_each(w, &ws->windows, workspace_link)
		order[i++] = w;
	return order;
}

static void workspace_restack(struct Workspace *ws, struct Window **before)
{
	struct Window **after;
	size_t n;
	size_t i;

	after = workspace_snapshot(ws);
	n = (size_t)wl_list_length(&ws->windows);
	if (before != NULL && after != NULL)
		for (i = 0; i < ws->n_layouts; i++)
			if (ws->layouts[i].restack != NULL)
				ws->layouts[i].restack(&ws->layouts[i], before,
						       after, n);
	free(after);
	free(before);
}

/*
 * Switch this seat's output to display workspace `target`. If another
 * output is already showing target, the two outputs swap workspaces;
 * otherwise the seat's output simply changes visible_workspace.
 * Drops focus so seat_focus(NULL) repicks a default on the new view.
 * No-op if target is invalid, already showing on this seat's output, or
 * the seat has no real output yet (avoid mutating an arbitrary output's
 * visible_workspace via seat_output's first-output fallback).
 */
static void sticky_move(struct wl_list *into, int from, int to)
{
	struct Window *w, *tmp;

	wl_list_for_each_safe(w, tmp, &wm.workspaces[from].windows,
			      workspace_link) {
		if (!w->sticky || w->closed)
			continue;
		wl_list_remove(&w->workspace_link);
		wl_list_insert(into->prev, &w->workspace_link);
		w->workspace_index = to;
		wm.restack_floats = true;
	}
}

static void sticky_follow(int prev, int target, bool swapped)
{
	struct wl_list moved_back, moved_on;

	wl_list_init(&moved_back);
	wl_list_init(&moved_on);
	if (swapped)
		sticky_move(&moved_back, target, prev);
	sticky_move(&moved_on, prev, target);
	wl_list_insert_list(&wm.workspaces[target].windows, &moved_on);
	wl_list_insert_list(&wm.workspaces[prev].windows, &moved_back);
}

static void workspace_switch(struct Seat *seat, int target)
{
	struct Output *other;
	struct Output *self;
	int prev;

	self = seat_output(seat);
	/*
	 * Reject the scratch slot - it never goes onto an output. Bounded
	 * by user range, not n_workspaces, for the same reason.
	 */
	if (self == NULL || target < 0 ||
	    (size_t)target >= wm_user_n_workspaces() ||
	    self->visible_workspace == target)
		return;
	other = output_showing(target);
	prev = self->visible_workspace;
	if (prev >= 0 && (size_t)prev < wm_user_n_workspaces())
		sticky_follow(prev, target, other != NULL && other != self);
	self->visible_workspace = target;
	if (other != NULL && other != self)
		other->visible_workspace = prev;
	seat->focused = NULL;
	seat_focus(seat, NULL);
	wm.dirty_status = true;
	wm.dirty_layout = true;
}

static bool scratchpad_def_matches(const struct ScratchpadDef *def,
				   const struct Window *w)
{
	const char *target = NULL;

	if (def->match_pattern == NULL)
		return false;
	switch (def->match_kind) {
	case RULE_MATCH_APP_ID:
		target = w->app_id;
		break;
	case RULE_MATCH_TITLE:
		target = w->title;
		break;
	case RULE_MATCH_PARENT:
		return false;
	}
	if (target == NULL)
		return false;
	if (def->glob)
		return fnmatch(def->match_pattern, target, 0) == 0;
	return strcmp(target, def->match_pattern) == 0;
}

/*
 * Walk the scratch workspace and return the most-recently-stashed
 * dynamic (no scratch_name) pad. wl_list_insert puts new entries at the
 * head, so the head is "most recent." Used by ACTION_TOGGLE_SCRATCHPAD
 * to decide between "stash focused" and "restore the most recent
 * dynamic stash".
 */
static struct Window *find_dynamic_stash(void)
{
	struct Workspace *ws;
	struct Window *w;
	int idx = wm_scratch_ws_index();

	if (idx < 0)
		return NULL;
	ws = &wm.workspaces[idx];
	wl_list_for_each(w, &ws->windows, workspace_link) {
		if (w->is_scratchpad && w->scratch_name == NULL && !w->closed &&
		    !w->requested_close)
			return w;
	}
	return NULL;
}

/*
 * Look up a managed window already labeled with this scratchpad name.
 * Walks every workspace because the pad may be on the visible ws (about
 * to be stashed) or on the scratch ws (about to be restored).
 */
static struct Window *find_named_pad_window(const char *name)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			if (w->closed || w->requested_close)
				continue;
			if (!w->is_scratchpad || w->scratch_name == NULL)
				continue;
			if (strcmp(w->scratch_name, name) == 0)
				return w;
		}
	}
	return NULL;
}

/*
 * Fallback for the keypress-races-app_id case: a (scratchpad NAME)
 * action fires before apply_rules has labeled the freshly-spawned
 * window. Match against the def directly so we still find it. Returns
 * the first unlabeled window matching `def`'s matcher across every
 * workspace.
 */
static struct Window *find_unlabeled_match(const struct ScratchpadDef *def)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			if (w->closed || w->requested_close)
				continue;
			if (w->is_scratchpad)
				continue;
			if (scratchpad_def_matches(def, w))
				return w;
		}
	}
	return NULL;
}

static void scratchpad_stash(struct Window *w)
{
	int target = wm_scratch_ws_index();

	if (target < 0 || w->workspace_index == target)
		return;
	wl_list_remove(&w->workspace_link);
	wl_list_insert(&wm.workspaces[target].windows, &w->workspace_link);
	w->workspace_index = target;
	wm.restack_floats = true;
}

static void scratchpad_unstash(struct Seat *seat, struct Window *w)
{
	struct Workspace *ws = seat_ws(seat);
	int target;

	if (ws == NULL)
		return;
	target = ws->index;
	if (w->workspace_index != target) {
		wl_list_remove(&w->workspace_link);
		wl_list_insert(&wm.workspaces[target].windows,
			       &w->workspace_link);
		w->workspace_index = target;
		wm.restack_floats = true;
	}
	w->float_override = FLOAT_FORCE_FLOAT;
	w->floating = true;
	wm.restack_floats = true;
	w->rules_dirty = true;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	seat_focus(seat, w);
}

static void scratchpad_toggle_dynamic(struct Seat *seat)
{
	struct Window *stashed;
	struct Window *w;

	if (wm_scratch_ws_index() < 0)
		return;
	/*
	 * If a dynamic pad is already stashed, restore it first. The
	 * common idiom is "stash this thing, ..., bring it back" via the
	 * same key, which only works if "bring it back" wins when a
	 * stash exists.
	 */
	stashed = find_dynamic_stash();
	if (stashed != NULL) {
		scratchpad_unstash(seat, stashed);
		return;
	}
	w = seat->focused;
	if (w == NULL || w->ignored || w->closed || w->requested_close)
		return;
	w->is_scratchpad = true;
	free(w->scratch_name);
	w->scratch_name = NULL;
	w->float_override = FLOAT_FORCE_FLOAT;
	w->floating = true;
	wm.restack_floats = true;
	w->rules_dirty = true;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	scratchpad_stash(w);
	seat->focused = NULL;
	seat_focus(seat, NULL);
}

static const struct ScratchpadDef *scratchpad_def_lookup(const char *name)
{
	if (name == NULL || g_config == NULL)
		return NULL;
	for (size_t i = 0; i < g_config->n_scratchpads; i++) {
		if (strcmp(g_config->scratchpads[i].name, name) == 0)
			return &g_config->scratchpads[i];
	}
	return NULL;
}

static void scratchpad_toggle_named(struct Seat *seat, const char *name)
{
	const struct ScratchpadDef *def;
	struct Window *w;
	int scratch_idx;

	if (name == NULL)
		return;
	scratch_idx = wm_scratch_ws_index();
	if (scratch_idx < 0)
		return;
	def = scratchpad_def_lookup(name);
	w = find_named_pad_window(name);
	if (w == NULL && def != NULL)
		w = find_unlabeled_match(def);

	if (w != NULL) {
		/*
		 * Latch the name on a pre-existing window we just promoted
		 * via the matcher fallback; subsequent toggles take the
		 * fast labeled-lookup path.
		 */
		if (!w->is_scratchpad || w->scratch_name == NULL ||
		    strcmp(w->scratch_name, name) != 0) {
			w->is_scratchpad = true;
			free(w->scratch_name);
			w->scratch_name = strdup(name);
		}
		if (w->workspace_index == scratch_idx) {
			scratchpad_unstash(seat, w);
		} else {
			seat->focused = NULL;
			scratchpad_stash(w);
			seat_focus(seat, NULL);
		}
		return;
	}
	/*
	 * No managed window. Spawn the configured command; apply_rules
	 * will label and place the window on the next manage cycle once
	 * its app_id / title arrives.
	 */
	if (def != NULL)
		spawn_child(def->spawn);
}

static const struct SubmapDef *submap_def_lookup(const char *name)
{
	if (name == NULL || g_config == NULL)
		return NULL;
	for (size_t i = 0; i < g_config->n_submaps; i++) {
		if (strcmp(g_config->submaps[i].name, name) == 0)
			return &g_config->submaps[i];
	}
	return NULL;
}

/*
 * Enter a submap: the seat's xkb bindings are wiped and replaced with
 * the submap's per-key entries (no modifiers). ensure_next_key_eaten is
 * armed so that a key not present in the submap is reported as
 * ate_unbound_key (which fires the teardown). Single-shot; the next
 * binding to fire while submap_active flips us back to normal bindings.
 *
 * No-op on a NULL def; previously-active submap is torn down first so
 * (submap A) inside an entry of (submap B) replaces the binding set
 * cleanly instead of stacking.
 */
static void submap_enter(struct Seat *seat, const struct SubmapDef *def)
{
	if (def == NULL)
		return;
	binding_seat_cleanup(seat);
	binding_register_submap(seat, def);
	seat->submap_active = true;
	seat->submap_exit_pending = false;
	if (seat->xkb_seat != NULL)
		river_xkb_bindings_seat_v1_ensure_next_key_eaten(seat->xkb_seat);
}

static void submap_exit(struct Seat *seat)
{
	if (!seat->submap_active)
		return;
	binding_seat_cleanup(seat);
	binding_register_from_config(seat, g_config);
	seat->submap_active = false;
	seat->submap_exit_pending = false;
}

static void seat_action(struct Seat *seat, const struct Action *action)
{
	if (seat->mru_active && action->kind != ACTION_FOCUS_MRU_NEXT &&
	    action->kind != ACTION_FOCUS_MRU_PREV)
		mru_commit(seat);
	/*
	 * Classify the action so we can set the right dirty flags before
	 * the dispatch runs. The rule is straightforward: anything that
	 * could change the layout pass's output (tile rects, the visible
	 * window set, fullscreen, scratchpad stash / restore, layout
	 * config like nmaster / mfact) sets dirty_layout (and dirty_status
	 * by implication). Pure focus / submap / op-start / spawn /
	 * close-request actions only set dirty_status because they do not
	 * change tile rects or the visible set this cycle:
	 *  - FOCUS_*: routes through seat_focus which already sets
	 *    dirty_status; no tile change.
	 *  - MOVE / RESIZE: kicks off a pointer op. The first op_delta
	 *    will call window_promote_floating, which itself sets both
	 *    flags; the press alone is layout-clean.
	 *  - SUBMAP: rebinds the seat's bindings, no layout effect.
	 *  - CLOSE: sends close + advances focus. requested_close does
	 *    not affect tile selection (apply_layout's predicate ignores
	 *    it). The actual layout change happens when the .closed
	 *    listener fires and that one sets both flags.
	 *  - SPAWN / RELOAD_CONFIG / EXIT: WM state unchanged this
	 *    cycle. Reload is a separate edge driven by g_reload_pending
	 *    in wm_handle_manage_start.
	 *  - NONE: do nothing.
	 * False positives on this side only cost an extra status
	 * snapshot build (dedupe absorbs); false negatives stale the
	 * layout, so we err on the side of marking too much.
	 */
	switch (action->kind) {
	case ACTION_NONE:
	case ACTION_SPAWN:
		break;
	case ACTION_FOCUS_NEXT:
	case ACTION_FOCUS_PREV:
	case ACTION_FOCUS_MASTER:
	case ACTION_FOCUS_OUTPUT:
	case ACTION_MOVE:
	case ACTION_RESIZE:
	case ACTION_SUBMAP:
	case ACTION_CLOSE:
	case ACTION_RELOAD_CONFIG:
	case ACTION_RESTART:
	case ACTION_EXIT:
		wm.dirty_status = true;
		break;
	default:
		wm.dirty_status = true;
		wm.dirty_layout = true;
		break;
	}

	switch (action->kind) {
	case ACTION_NONE:
		break;
	case ACTION_SPAWN:
		spawn_child(action->arg);
		break;
	case ACTION_CLOSE: {
		struct Workspace *ws;
		struct Window *target;
		struct Window *w;

		w = seat->focused;
		if (w == NULL || w->closed || w->requested_close)
			break;
		river_window_v1_close(w->obj);
		/*
		 * Mark the request locally and advance focus to a sibling
		 * so a held auto-repeat targets a fresh window per tick
		 * instead of close-spamming the same dying one. We do NOT
		 * set w->closed here - that flag drives window_maybe_destroy,
		 * which would tear down the proxy before river has actually
		 * closed the window. requested_close is purely for focus
		 * skip (next_focusable, seat_focus); the .closed event sets
		 * `closed` definitively, after which the next manage cycle
		 * reaps cleanly.
		 */
		w->requested_close = true;
		w->close_requested_ns = window_monotonic_ns();
		ws = seat_ws(seat);
		if (ws != NULL) {
			struct wl_list *anchor = &ws->windows;

			if (w->workspace_index == (int)(ws - wm.workspaces))
				anchor = &w->workspace_link;
			target = next_focusable(&ws->windows, anchor, true,
						NULL);
			if (target != NULL)
				seat_focus(seat, target);
			else
				seat_focus(seat, NULL);
		}
		break;
	}
	case ACTION_FOCUS_NEXT:
	case ACTION_FOCUS_PREV: {
		struct wl_list *anchor;
		struct Window *focused;
		struct Window *target;
		struct Workspace *ws;
		bool forward;

		ws = seat_ws(seat);
		if (ws == NULL || wl_list_empty(&ws->windows))
			break;
		forward = (action->kind == ACTION_FOCUS_NEXT);
		focused = seat_focus_on_visible_ws(seat, NULL);
		anchor = (focused != NULL) ? &focused->workspace_link :
					     &ws->windows;
		target = next_focusable(&ws->windows, anchor, forward, NULL);
		if (target != NULL)
			seat_focus(seat, target);
		break;
	}
	case ACTION_FOCUS_LAST: {
		struct Window *from;
		struct Window *w;

		if (seat->focused != seat->focus_cur &&
		    seat_focus_window(seat, seat->focus_cur))
			break;
		w = seat->focus_last;
		from = seat->focus_cur;
		if (seat_focus_window(seat, w) && from != w)
			seat->focus_last = from;
		break;
	}
	case ACTION_FOCUS_MASTER: {
		struct Window *target;
		struct Workspace *ws;

		ws = seat_ws(seat);
		if (ws == NULL || wl_list_empty(&ws->windows))
			break;
		target = next_focusable(&ws->windows, &ws->windows, true, NULL);
		if (target != NULL)
			seat_focus(seat, target);
		break;
	}
	case ACTION_SWAP_NEXT:
	case ACTION_SWAP_PREV: {
		struct Window **before;
		struct Window *neighbor;
		struct Window *focused;
		struct Workspace *ws;
		struct wl_list *list;
		struct wl_list *me;
		bool forward;
		bool wrapped;

		focused = seat_focus_on_visible_ws(seat, &ws);
		if (focused == NULL)
			break;
		forward = (action->kind == ACTION_SWAP_NEXT);
		list = &ws->windows;
		me = &focused->workspace_link;
		wrapped = false;
		neighbor = next_focusable(list, me, forward, &wrapped);
		if (neighbor == NULL)
			break;
		before = workspace_snapshot(ws);
		wl_list_remove(me);
		/* On wrap, jump to the far end; otherwise insert adjacent
		 * to the neighbour the walk landed on. */
		if (wrapped)
			wl_list_insert(forward ? list : list->prev, me);
		else if (forward)
			wl_list_insert(&neighbor->workspace_link, me);
		else
			wl_list_insert(neighbor->workspace_link.prev, me);
		workspace_restack(ws, before);
		break;
	}
	case ACTION_SHRINK_MASTER:
	case ACTION_EXPAND_MASTER: {
		struct Workspace *ws;
		struct Layout *active;
		float delta;
		float r;

		ws = seat_ws(seat);
		if (ws == NULL)
			break;
		active = workspace_layout(ws);
		if (active->kind != LAYOUT_TALL &&
		    active->kind != LAYOUT_MIRROR_TALL &&
		    active->kind != LAYOUT_THREE_COLUMNS &&
		    active->kind != LAYOUT_TWO_PANE &&
		    active->kind != LAYOUT_RESIZABLE_TALL)
			break;
		delta = (action->kind == ACTION_EXPAND_MASTER) ? 0.03f : -0.03f;
		r = active->u.tall.ratio + delta;
		if (r < 0.05f)
			r = 0.05f;
		if (r > 0.95f)
			r = 0.95f;
		active->u.tall.ratio = r;
		break;
	}
	case ACTION_INC_MASTER:
	case ACTION_DEC_MASTER: {
		struct Workspace *ws;
		struct Layout *active;
		s32 delta;
		s32 nm;

		ws = seat_ws(seat);
		if (ws == NULL)
			break;
		active = workspace_layout(ws);
		if (active->kind != LAYOUT_TALL &&
		    active->kind != LAYOUT_MIRROR_TALL &&
		    active->kind != LAYOUT_THREE_COLUMNS &&
		    active->kind != LAYOUT_TWO_PANE &&
		    active->kind != LAYOUT_RESIZABLE_TALL)
			break;
		delta = (action->kind == ACTION_INC_MASTER) ? 1 : -1;
		nm = active->u.tall.nmaster + delta;
		if (nm < 0)
			nm = 0;
		active->u.tall.nmaster = nm;
		break;
	}
	case ACTION_NEXT_LAYOUT:
	case ACTION_PREV_LAYOUT: {
		struct Workspace *ws;
		size_t cur;

		ws = seat_ws(seat);
		if (ws == NULL || ws->n_layouts <= 1)
			break;
		cur = ws->current_layout_index;
		if (action->kind == ACTION_NEXT_LAYOUT)
			cur = (cur + 1) % ws->n_layouts;
		else
			cur = (cur + ws->n_layouts - 1) % ws->n_layouts;
		ws->current_layout_index = cur;
		break;
	}
	case ACTION_TOGGLE_FULLSCREEN: {
		struct Window *focused;
		struct Workspace *ws;

		focused = seat_focus_on_visible_ws(seat, &ws);
		if (focused == NULL)
			break;
		focused->fullscreen_client = false;
		if (focused->fullscreen) {
			focused->fullscreen = false;
		} else {
			/* One fullscreen at a time per workspace. */
			struct Window *w;

			wl_list_for_each(w, &ws->windows, workspace_link)
				w->fullscreen = false;
			focused->fullscreen = true;
		}
		break;
	}
	case ACTION_TOGGLE_FLOAT: {
		struct Window *w;

		if (seat->focused == NULL)
			break;
		w = seat->focused;
		if (w->floating) {
			w->float_override = FLOAT_FORCE_TILE;
			w->floating = false;
			w->sticky = false;
		} else {
			w->float_override = FLOAT_FORCE_FLOAT;
			w->floating = true;
			window_raise(w);
			/* Snapshot current tile rect so the float lands
			 * somewhere visible until the user drags it. */
			w->float_x = w->x;
			w->float_y = w->y;
			if (w->width > 0)
				w->float_width = w->width;
			if (w->height > 0)
				w->float_height = w->height;
		}
		w->rules_dirty = true;
		wm.restack_floats = true;
		break;
	}
	case ACTION_MOVE:
		if (seat->op == SEAT_OP_NONE && seat->hovered != NULL)
			seat_pointer_move(seat, seat->hovered);
		break;
	case ACTION_RESIZE:
		if (seat->op == SEAT_OP_NONE && seat->hovered != NULL)
			seat_pointer_resize(
				seat, seat->hovered,
				RIVER_WINDOW_V1_EDGES_BOTTOM |
					RIVER_WINDOW_V1_EDGES_RIGHT);
		break;
	case ACTION_RESIZE_DELTA: {
		char edge[16];
		struct Window *w;
		s32 x, y, ww, hh;
		int delta;

		if (action->arg == NULL ||
		    sscanf(action->arg, "%15s %d", edge, &delta) != 2)
			break;
		w = seat->focused;
		if (w == NULL || w->ignored || w->closed ||
		    w->requested_close || !w->floating)
			break;
		/*
		 * Read live geometry rather than the float_* remember:
		 * float_x/y/w/h is set on float-promotion and on (resize),
		 * but a pointer drag updates only w->x/y (see seat_render).
		 */
		x = w->x;
		y = w->y;
		ww = (w->last_proposed_w > 0) ? w->last_proposed_w : w->width;
		hh = (w->last_proposed_h > 0) ? w->last_proposed_h : w->height;
		if (strcmp(edge, "right") == 0) {
			ww += delta;
		} else if (strcmp(edge, "left") == 0) {
			x -= delta;
			ww += delta;
		} else if (strcmp(edge, "down") == 0) {
			hh += delta;
		} else if (strcmp(edge, "up") == 0) {
			y -= delta;
			hh += delta;
		} else {
			break;
		}
		if (ww < 1)
			ww = 1;
		if (hh < 1)
			hh = 1;
		window_set_position(w, x, y);
		window_propose_dimensions(w, ww, hh);
		w->float_x = x;
		w->float_y = y;
		w->float_width = ww;
		w->float_height = hh;
		break;
	}
	case ACTION_EXIT:
		river_window_manager_v1_exit_session(window_manager_v1);
		break;
	case ACTION_RESTART:
		restore_request();
		break;
	case ACTION_FOCUS_MRU_NEXT:
		mru_step(seat, 1);
		break;
	case ACTION_FOCUS_MRU_PREV:
		mru_step(seat, -1);
		break;
	case ACTION_TOGGLE_STICKY: {
		struct Window *w = seat->focused;

		if (w == NULL || w->ignored)
			break;
		w->sticky = !w->sticky;
		if (w->sticky && !w->floating) {
			w->float_override = FLOAT_FORCE_FLOAT;
			w->floating = true;
			w->float_x = w->x;
			w->float_y = w->y;
			if (w->width > 0)
				w->float_width = w->width;
			if (w->height > 0)
				w->float_height = w->height;
			w->rules_dirty = true;
			wm.restack_floats = true;
			window_raise(w);
		}
		break;
	}
	case ACTION_WORKSPACE: {
		int n = action->arg ? atoi(action->arg) - 1 : -1;

		workspace_switch(seat, n);
		break;
	}
	case ACTION_MOVE_TO_WORKSPACE:
	case ACTION_MOVE_TO_WORKSPACE_FOLLOW: {
		struct Window *w;
		int n;

		n = action->arg ? atoi(action->arg) - 1 : -1;
		/* Scratch is the WM's, not the user's; refuse to land here. */
		if (n < 0 || (size_t)n >= wm_user_n_workspaces() ||
		    seat->focused == NULL ||
		    seat->focused->workspace_index == n)
			break;
		w = seat->focused;
		window_relink_ws(w, n);
		seat->focused = NULL;
		if (action->kind == ACTION_MOVE_TO_WORKSPACE_FOLLOW) {
			workspace_switch(seat, n);
			seat_focus(seat, w);
		} else {
			seat_focus(seat, NULL);
		}
		break;
	}
	case ACTION_NEXT_WORKSPACE:
	case ACTION_PREV_WORKSPACE: {
		struct Output *self;
		int n_ws;
		int delta;
		int target;

		self = seat_output(seat);
		if (self == NULL || wm_user_n_workspaces() == 0)
			break;
		/* Cycle the user range only - scratch never gets shown. */
		n_ws = (int)wm_user_n_workspaces();
		delta = (action->kind == ACTION_NEXT_WORKSPACE) ? 1 : -1;
		target = (self->visible_workspace + delta + n_ws) % n_ws;
		workspace_switch(seat, target);
		break;
	}
	case ACTION_FOCUS_OUTPUT: {
		struct Output *target;
		int n;

		n = action->arg ? atoi(action->arg) : -1;
		target = output_at_index(n);
		if (target == NULL || target == seat->output)
			break;
		seat->output = target;
		seat->focused = NULL;
		seat_focus(seat, NULL);
		break;
	}
	case ACTION_MOVE_TO_OUTPUT: {
		struct Output *target;
		struct Window *w;
		int wsi;
		int n;

		n = action->arg ? atoi(action->arg) : -1;
		target = output_at_index(n);
		if (target == NULL || seat->focused == NULL)
			break;
		wsi = target->visible_workspace;
		if (wsi < 0 || (size_t)wsi >= wm_user_n_workspaces() ||
		    seat->focused->workspace_index == wsi)
			break;
		w = seat->focused;
		wl_list_remove(&w->workspace_link);
		wl_list_insert(&wm.workspaces[wsi].windows, &w->workspace_link);
		w->workspace_index = wsi;
		wm.restack_floats = true;
		seat->focused = NULL;
		seat_focus(seat, NULL);
		break;
	}
	case ACTION_LAYOUT_VERB:
		layout_verb_dispatch(seat, action->arg);
		break;
	case ACTION_TOGGLE_SCRATCHPAD:
		scratchpad_toggle_dynamic(seat);
		break;
	case ACTION_NAMED_SCRATCHPAD:
		scratchpad_toggle_named(seat, action->arg);
		break;
	case ACTION_SUBMAP:
		/*
		 * Replacing one submap with another (chain) is allowed; the
		 * existing seat bindings are wiped wholesale either way.
		 * Submap teardown after dispatch happens in seat_manage and
		 * skips ACTION_SUBMAP itself so we do not immediately undo
		 * the submap we just entered.
		 */
		if (seat->submap_active)
			submap_exit(seat);
		submap_enter(seat, submap_def_lookup(action->arg));
		break;
	case ACTION_RELOAD_CONFIG:
		/*
		 * We are inside a manage cycle right now, but mid-cycle is
		 * still the wrong place to reload: apply_rules has already
		 * run with the old config this cycle, and a partial swap
		 * mixes old-rule floating with new-config layout. Schedule
		 * a fresh manage cycle and let wm_handle_manage_start swap
		 * the config in cleanly at step 0 of the next pass.
		 */
		schedule_reload();
		break;
	}
}

static int focus_seq_cmp(const void *pa, const void *pb)
{
	const struct Window *a = *(struct Window *const *)pa;
	const struct Window *b = *(struct Window *const *)pb;

	return (a->focus_seq < b->focus_seq) - (a->focus_seq > b->focus_seq);
}

static bool mru_eligible(const struct Window *w)
{
	return !w->closed && !w->requested_close && !w->ignored &&
	       w->workspace_index >= 0 &&
	       (size_t)w->workspace_index < wm_user_n_workspaces();
}

static size_t mru_build(struct Seat *seat)
{
	struct Window **grown;
	struct Window *w;
	size_t n = 0;

	for (size_t i = 0; i < wm.n_workspaces; i++)
		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link)
			if (mru_eligible(w))
				n++;
	if (n > seat->mru_cap) {
		grown = realloc(seat->mru, n * sizeof(*grown));
		if (grown == NULL)
			return 0;
		seat->mru = grown;
		seat->mru_cap = n;
	}
	seat->mru_n = 0;
	for (size_t i = 0; i < wm.n_workspaces; i++)
		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link)
			if (mru_eligible(w))
				seat->mru[seat->mru_n++] = w;
	qsort(seat->mru, seat->mru_n, sizeof(*seat->mru), focus_seq_cmp);
	if (seat->focused != NULL) {
		for (size_t i = 1; i < seat->mru_n; i++) {
			if (seat->mru[i] == seat->focused) {
				seat->mru[i] = seat->mru[0];
				seat->mru[0] = seat->focused;
				break;
			}
		}
	}
	return seat->mru_n;
}

static void mru_commit(struct Seat *seat)
{
	struct Window *origin;
	struct Window *final;

	if (!seat->mru_active)
		return;
	origin = seat->mru_n > 0 ? seat->mru[0] : NULL;
	final = seat->focused;
	seat->mru_active = false;
	seat->mru_commit_pending = false;
	seat->mru_n = 0;
	if (seat->mru_watch != 0 && seat->xkb_seat != NULL)
		river_xkb_bindings_seat_v1_modifiers_watch(seat->xkb_seat, 0);
	seat->mru_watch = 0;
	if (final == NULL)
		return;
	final->focus_seq = ++wm.next_focus_seq;
	if (final != seat->focus_cur) {
		seat->focus_last = (origin != final) ? origin : seat->focus_cur;
		seat->focus_cur = final;
	}
}

static void mru_step(struct Seat *seat, int dir)
{
	struct Window *w;
	bool oneshot;
	bool ok;

	if (!seat->mru_active) {
		if (mru_build(seat) < 2)
			return;
		seat->mru_active = true;
		seat->mru_pos = 0;
		seat->mru_watch = seat->pending_mods &
				  ~(u32)RIVER_SEAT_V1_MODIFIERS_SHIFT;
		if (seat->mru_watch != 0 && seat->xkb_seat != NULL &&
		    river_xkb_bindings_seat_v1_get_version(seat->xkb_seat) >= 3)
			river_xkb_bindings_seat_v1_modifiers_watch(
				seat->xkb_seat, seat->mru_watch);
		else
			seat->mru_watch = 0;
	}
	oneshot = seat->mru_watch == 0;
	for (size_t tries = 0; tries < seat->mru_n; tries++) {
		seat->mru_pos = (seat->mru_pos + seat->mru_n + (size_t)dir) %
				seat->mru_n;
		w = seat->mru[seat->mru_pos];
		if (w == NULL || !mru_eligible(w))
			continue;
		seat->mru_stepping = true;
		ok = seat_focus_window(seat, w);
		seat->mru_stepping = false;
		if (ok)
			break;
	}
	if (oneshot)
		mru_commit(seat);
}

bool seat_focus_window(struct Seat *seat, struct Window *w)
{
	struct Output *out;
	int wsi;

	if (w == NULL || w->ignored || w->closed || w->requested_close)
		return false;
	wsi = w->workspace_index;
	if (wsi >= 0 && wsi == wm_scratch_ws_index()) {
		scratchpad_unstash(seat, w);
		return seat->focused == w;
	}
	if (wsi < 0 || (size_t)wsi >= wm_user_n_workspaces())
		return false;
	out = output_showing(wsi);
	if (out == NULL) {
		workspace_switch(seat, wsi);
		if (output_showing(wsi) == NULL)
			return false;
	} else if (out != seat_output(seat)) {
		seat->output = out;
	}
	seat_focus(seat, w);
	wm.dirty_status = true;
	wm.dirty_layout = true;
	return seat->focused == w;
}

bool seat_close_window(struct Seat *seat, struct Window *w)
{
	struct Action close = { ACTION_CLOSE, NULL };

	if (w == NULL || w->closed || w->requested_close)
		return false;
	if (seat != NULL && seat->focused == w) {
		seat_action(seat, &close);
		return true;
	}
	river_window_v1_close(w->obj);
	w->requested_close = true;
	w->close_requested_ns = window_monotonic_ns();
	wm.dirty_status = true;
	return true;
}

void seat_run_action(struct Seat *seat, const struct Action *action)
{
	seat_action(seat, action);
}

void seat_manage(struct Seat *seat)
{
	if (seat->is_new) {
		seat->is_new = false;
		binding_register_from_config(seat, g_config);
		input_apply_cursor(seat);
	}
	if (seat->mru_commit_pending)
		mru_commit(seat);

	/*
	 * Drain a deferred submap teardown from the ate_unbound_key
	 * listener (which fires outside the manage cycle and only sets
	 * the flag).
	 */
	if (seat->submap_exit_pending)
		submap_exit(seat);

	/*
	 * If no window was interacted with in the current manage sequence,
	 * intentionally pass NULL to ensure the window on top has focus.
	 * This is necessary to handle new windows for example.
	 */
	if (seat->mru_active && seat->interacted != NULL &&
	    seat->interacted != seat->focused)
		mru_commit(seat);
	seat_focus(seat, seat->interacted);
	seat->interacted = NULL;
	if (seat->refocus_pending) {
		seat->refocus_pending = false;
		if (seat->focused != NULL &&
		    seat->layer_focus == LAYER_FOCUS_NONE)
			river_seat_v1_focus_window(seat->obj,
						   seat->focused->obj);
	}
	output_set_layer_default(seat_output(seat));

	if (seat->pending_action.kind != ACTION_NONE) {
		enum ActionKind kind = seat->pending_action.kind;

		seat_action(seat, &seat->pending_action);
		action_clear(&seat->pending_action);
		seat->pending_mods = 0;
		/*
		 * Single-shot submap: any binding that fires while the
		 * submap is active triggers teardown after dispatch.
		 * ACTION_SUBMAP is the obvious exception (it just entered
		 * a new submap), and ACTION_NONE means nothing fired.
		 */
		if (seat->submap_active && kind != ACTION_SUBMAP &&
		    kind != ACTION_NONE)
			submap_exit(seat);
	}

	if (seat->op == SEAT_OP_MOVE && seat->op_release && seat_op_valid(seat))
		seat_drop_float(seat);
	if (seat->op != SEAT_OP_NONE &&
	    (seat->op_release || !seat_op_valid(seat)))
		seat_op_end(seat);

	switch (seat->op) {
	case SEAT_OP_NONE:
	case SEAT_OP_MOVE:
		break;
	case SEAT_OP_RESIZE: {
		s32 width;
		s32 height;

		width = seat->op_start_width;
		height = seat->op_start_height;
		if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) != 0)
			width -= seat->op_dx;
		if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_RIGHT) != 0)
			width += seat->op_dx;
		if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_TOP) != 0)
			height -= seat->op_dy;
		if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_BOTTOM) != 0)
			height += seat->op_dy;
		if (width < 1)
			width = 1;
		if (height < 1)
			height = 1;
		window_propose_dimensions(seat->op_window, width, height);
		break;
	}
	}
	seat->op_release = false;
}

void seat_render(struct Seat *seat)
{
	if (seat->op != SEAT_OP_NONE && !seat_op_valid(seat))
		return;
	switch (seat->op) {
	case SEAT_OP_NONE:
		break;
	case SEAT_OP_MOVE:
		window_set_position(seat->op_window,
				    seat->op_start_x + seat->op_dx,
				    seat->op_start_y + seat->op_dy);
		break;
	case SEAT_OP_RESIZE: {
		s32 x = seat->op_start_x;
		s32 y = seat->op_start_y;
		if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) != 0)
			x += seat->op_start_width - seat->op_window->width;
		if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_TOP) != 0)
			y += seat->op_start_height - seat->op_window->height;
		window_set_position(seat->op_window, x, y);
		break;
	}
	}
}
