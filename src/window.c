/* SPDX-License-Identifier: 0BSD */
/*
 * river_window_v1 listener and Window state lifecycle. Windows are
 * created in wm_handle_window (wm.c), reaped at the start of each
 * manage cycle via window_maybe_destroy, and laid out via the cached
 * set_position / propose_dimensions / set_bounds wrappers.
 */

#include <stdlib.h>
#include <time.h>
#include <string.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>

#include "config.h"
#include "nomonad.h"
#include "recorder.h"

#define NM_CLOSE_GRACE_NS 1000000000ULL

static void window_handle_closed(void *data, struct river_window_v1 *obj)
{
	struct Window *window = data;
	window->closed = true;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_window_close(window);
}

static void window_handle_dimensions(void *data, struct river_window_v1 *obj,
				     s32 width, s32 height)
{
	struct Window *window = data;
	window->width = width;
	window->height = height;
}

static void
window_handle_pointer_move_requested(void *data, struct river_window_v1 *obj,
				     struct river_seat_v1 *river_seat)
{
	struct Window *window = data;
	window->pointer_move_requested =
		river_seat_v1_get_user_data(river_seat);
}

static void
window_handle_pointer_resize_requested(void *data, struct river_window_v1 *obj,
				       struct river_seat_v1 *river_seat,
				       u32 edges)
{
	struct Window *window = data;
	window->pointer_resize_requested =
		river_seat_v1_get_user_data(river_seat);
	window->pointer_resize_requested_edges = edges;
}

static void window_handle_dimensions_hint(void *data,
					  struct river_window_v1 *obj,
					  s32 min_width, s32 min_height,
					  s32 max_width, s32 max_height)
{
	struct Window *window = data;

	window->min_width = min_width;
	window->min_height = min_height;
	window->max_width = max_width;
	window->max_height = max_height;
	recorder_event_window_dimensions_hint(window);
}

/*
 * Some clients (terminals showing cursor coords, browsers) update title
 * on every keystroke. Skip the free+strdup churn when nothing changed.
 */
static void window_handle_app_id(void *data, struct river_window_v1 *obj,
				 const char *app_id)
{
	struct Window *window = data;
	if (window->app_id == NULL && app_id == NULL)
		return;
	if (window->app_id != NULL && app_id != NULL &&
	    strcmp(window->app_id, app_id) == 0)
		return;
	free(window->app_id);
	window->app_id = (app_id != NULL) ? strdup(app_id) : NULL;
	window->rules_dirty = true;
	wm.dirty_status = true;
	/*
	 * App-id changes can flip a float / ignore rule outcome, which
	 * moves the window into / out of the tile set. Conservative:
	 * mark layout dirty too. False positives are absorbed by the
	 * window setters' value caches in window.c.
	 */
	wm.dirty_layout = true;
	recorder_event_window_app_id(window);
}

static void window_handle_title(void *data, struct river_window_v1 *obj,
				const char *title)
{
	struct Window *window = data;
	if (window->title == NULL && title == NULL)
		return;
	if (window->title != NULL && title != NULL &&
	    strcmp(window->title, title) == 0)
		return;
	free(window->title);
	window->title = (title != NULL) ? strdup(title) : NULL;
	window->rules_dirty = true;
	wm.dirty_status = true;
	/* Same reasoning as app_id - title rules feed apply_rules. */
	wm.dirty_layout = true;
	recorder_event_window_title(window);
}

static void window_handle_parent(void *data, struct river_window_v1 *obj,
				 struct river_window_v1 *parent)
{
	struct Window *window = data;
	struct Window *parent_window;
	bool prev_has_parent;

	parent_window = (parent != NULL) ? river_window_v1_get_user_data(parent) :
					   NULL;
	prev_has_parent = window->has_parent;
	window->has_parent = (parent != NULL);
	if (window->has_parent != prev_has_parent) {
		window->rules_dirty = true;
		wm.dirty_status = true;
		wm.dirty_layout = true;
	}
	recorder_event_window_parent(window, parent_window);
}

static void window_handle_decoration_hint(void *data,
					  struct river_window_v1 *obj, u32 hint)
{
}

static void window_handle_show_window_menu_requested(
	void *data, struct river_window_v1 *obj, s32 x, s32 y)
{
}

static void window_handle_maximize_requested(void *data,
					     struct river_window_v1 *obj)
{
}

static void window_handle_unmaximize_requested(void *data,
					       struct river_window_v1 *obj)
{
}

static void
window_handle_fullscreen_requested(void *data, struct river_window_v1 *obj,
				   struct river_output_v1 *river_output)
{
	struct Window *window = data;
	struct Seat *seat;
	int wsi;

	if (window == NULL)
		return;
	wsi = window->workspace_index;
	if (wsi >= 0 && (size_t)wsi < wm.n_workspaces) {
		struct Window *w;
		wl_list_for_each(w, &wm.workspaces[wsi].windows,
				 workspace_link) {
			if (w != window)
				w->fullscreen = false;
		}
	}
	window->fullscreen = true;
	window->fullscreen_client = true;
	if (!window->closed && !window->requested_close && !window->ignored) {
		wl_list_for_each(seat, &wm.seats, link) {
			if (seat->focused != NULL && seat->focused != window &&
			    seat->focused->workspace_index == wsi)
				seat->interacted = window;
		}
	}
	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_window_fullscreen(window, true);
}

static void window_handle_exit_fullscreen_requested(void *data,
						    struct river_window_v1 *obj)
{
	struct Window *window = data;
	if (window == NULL || !window->fullscreen_client)
		return;
	window->fullscreen = false;
	window->fullscreen_client = false;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_window_fullscreen(window, false);
}

static void window_handle_minimize_requested(void *data,
					     struct river_window_v1 *obj)
{
}

static void window_handle_unreliable_pid(void *data,
					 struct river_window_v1 *obj,
					 s32 unreliable_pid)
{
	struct Window *window = data;

	window->unreliable_pid = unreliable_pid;
}

static void window_handle_capture_sessions(void *data,
					   struct river_window_v1 *obj,
					   u32 count)
{
	struct Window *window = data;

	window->capture_sessions = count;
}

static void window_handle_presentation_hint(void *data,
					    struct river_window_v1 *obj,
					    u32 hint)
{
	struct Window *window = data;

	window->presentation_hint = hint;
}

static void window_handle_identifier(void *data, struct river_window_v1 *obj,
				     const char *identifier)
{
	struct Window *window = data;

	/*
	 * Per protocol, identifier is sent once per window and never
	 * reused. Free anyway in case the compositor sends it twice;
	 * cheap defensive belt+suspenders.
	 */
	free(window->identifier);
	window->identifier = (identifier != NULL) ? strdup(identifier) : NULL;
}

const struct river_window_v1_listener river_window_listener = {
	.closed = window_handle_closed,
	.dimensions_hint = window_handle_dimensions_hint,
	.dimensions = window_handle_dimensions,
	.app_id = window_handle_app_id,
	.title = window_handle_title,
	.parent = window_handle_parent,
	.decoration_hint = window_handle_decoration_hint,
	.pointer_move_requested = window_handle_pointer_move_requested,
	.pointer_resize_requested = window_handle_pointer_resize_requested,
	.show_window_menu_requested = window_handle_show_window_menu_requested,
	.maximize_requested = window_handle_maximize_requested,
	.unmaximize_requested = window_handle_unmaximize_requested,
	.fullscreen_requested = window_handle_fullscreen_requested,
	.exit_fullscreen_requested = window_handle_exit_fullscreen_requested,
	.minimize_requested = window_handle_minimize_requested,
	.unreliable_pid = window_handle_unreliable_pid,
	.presentation_hint = window_handle_presentation_hint,
	.capture_sessions = window_handle_capture_sessions,
	.identifier = window_handle_identifier,
};

void window_maybe_destroy(struct Window *window)
{
	struct Seat *seat;

	if (!window->closed)
		return;
	if (window->fullscreen)
		wm.restack_floats = true;

	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->focused == window)
			seat->focused = NULL;
		if (seat->focus_cur == window)
			seat->focus_cur = NULL;
		if (seat->focus_last == window)
			seat->focus_last = NULL;
		for (size_t i = 0; i < seat->mru_n; i++)
			if (seat->mru[i] == window)
				seat->mru[i] = NULL;
		if (seat->op_window == window) {
			river_seat_v1_op_end(seat->obj);
			seat->op = SEAT_OP_NONE;
			seat->op_window = NULL;
		}
		/*
		 * river fires pointer_leave + window_interaction-clear before a
		 * window destroys in normal operation, so these are usually
		 * already NULL by the time we get here. The fuzz harness can
		 * destroy a window while it is still hovered/interacted (no
		 * server to sequence the events), and seat_pointer_resize
		 * reads seat->hovered through the action path, which is then
		 * a use-after-free. Clear defensively.
		 */
		if (seat->hovered == window)
			seat->hovered = NULL;
		if (seat->interacted == window)
			seat->interacted = NULL;
	}

	/*
	 * Destroy the node before the window. river_window_v1.get_node
	 * minted it as a separate object; the protocol does not promise
	 * that destroying the parent window cleans up the node, and the
	 * proxy memory leaks across long-running sessions otherwise. Also
	 * lets a stubbed harness account for every fake proxy it created.
	 */
	if (window->node != NULL)
		river_node_v1_destroy(window->node);
	river_window_v1_destroy(window->obj);
	wl_list_remove(&window->workspace_link);
	/*
	 * Scrub layout state that keys on the Window pointer before we
	 * free it. Necessary for layouts (BSP) that retain pointer-keyed
	 * tree nodes across cycles: a workspace that was not visible
	 * when the window died otherwise hangs onto a stale leaf, and
	 * the next apply_layout there reads the freed Window. The drop
	 * hook compares pointers only, never dereferences.
	 *
	 * We sweep every workspace, not just the window's current one:
	 * ACTION_MOVE_TO_WORKSPACE / ACTION_MOVE_TO_OUTPUT can leave a
	 * stale leaf on the source workspace's BSP tree if that
	 * workspace was not visible at the time of the move (its
	 * apply_layout never ran to drop the now-unwanted leaf).
	 * layout_drop_window on a workspace that never had this window
	 * is an O(1) hashtable miss inside layout_bsp_drop_window, so
	 * the sweep is cheap even at 9 workspaces.
	 */
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws = &wm.workspaces[i];

		ws->layout_arr_len = 0;
		/*
		 * Fan out across every layout in the cycle, not just the
		 * current one. A non-active BSP layout can still hold a
		 * leaf for this Window from a previous spell as the active
		 * kind; without scrubbing it now, the next ACTION_NEXT_LAYOUT
		 * back to BSP would dereference the freed Window.
		 */
		for (size_t j = 0; j < ws->n_layouts; j++)
			layout_drop_window(&ws->layouts[j], window);
	}
	recorder_window_freed(window);
	free(window->app_id);
	free(window->title);
	free(window->identifier);
	free(window->scratch_name);
	free(window);
}

void window_raise(struct Window *window)
{
	river_node_v1_place_top(window->node);
	window->raise_seq = ++wm.next_raise_seq;
}

void window_set_position(struct Window *window, s32 x, s32 y)
{
	if (window->position_synced && window->x == x && window->y == y)
		return;
	river_node_v1_set_position(window->node, x, y);
	window->x = x;
	window->y = y;
	window->position_synced = true;
}

void window_propose_dimensions(struct Window *window, s32 width, s32 height)
{
	if (window->dimensions_synced && window->last_proposed_w == width &&
	    window->last_proposed_h == height)
		return;
	river_window_v1_propose_dimensions(window->obj, width, height);
	window->last_proposed_w = width;
	window->last_proposed_h = height;
	window->dimensions_synced = true;
}

void window_set_bounds(struct Window *window, s32 width, s32 height)
{
	if (window->bounds_synced && window->last_bounds_w == width &&
	    window->last_bounds_h == height)
		return;
	river_window_v1_set_dimension_bounds(window->obj, width, height);
	window->last_bounds_w = width;
	window->last_bounds_h = height;
	window->bounds_synced = true;
}

void window_set_clip(struct Window *window, s32 width, s32 height)
{
	if (window->clip_synced && window->last_clip_w == width &&
	    window->last_clip_h == height)
		return;
	river_window_v1_set_content_clip_box(window->obj, 0, 0, width, height);
	window->last_clip_w = width;
	window->last_clip_h = height;
	window->clip_synced = true;
}

void window_apply_decoration(struct Window *w)
{
	enum DecorationMode mode = (g_config != NULL) ? g_config->decoration :
							DECORATION_SSD;
	if (mode == DECORATION_CSD)
		river_window_v1_use_csd(w->obj);
	else
		river_window_v1_use_ssd(w->obj);
}

u64 window_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

void window_manage(struct Window *window)
{
	if (window->requested_close && !window->closed &&
	    window_monotonic_ns() - window->close_requested_ns >
		    NM_CLOSE_GRACE_NS) {
		window->requested_close = false;
		wm.dirty_status = true;
	}
	if (window->is_new) {
		window->is_new = false;
		/*
		 * Default is SSD: tell the client not to draw a titlebar /
		 * close buttons. We do not draw any either, so windows end
		 * up bare like xmonad's. Override via
		 * (defcfg (decorations csd)).
		 */
		window_apply_decoration(window);
		river_window_v1_set_capabilities(
			window->obj, RIVER_WINDOW_V1_CAPABILITIES_FULLSCREEN);
	}
	/*
	 * Layout proposes dimensions and position for every window each
	 * cycle, including new ones, so window_manage no longer needs the
	 * (0,0) placeholder it used in the tinyrwm scaffold.
	 */
	if (window->pointer_move_requested != NULL) {
		seat_pointer_move(window->pointer_move_requested, window);
		window->pointer_move_requested = NULL;
	}
	if (window->pointer_resize_requested != NULL) {
		seat_pointer_resize(window->pointer_resize_requested, window,
				    window->pointer_resize_requested_edges);
		window->pointer_resize_requested = NULL;
	}
}
