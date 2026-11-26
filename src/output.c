/* SPDX-License-Identifier: 0BSD */
/*
 * river_output_v1 listener and Output state lifecycle. Outputs are
 * created in wm_handle_output (wm.c) and reaped at the start of each
 * manage cycle via output_maybe_destroy.
 */

#include <stdlib.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>

#include "nomonad.h"
#include "recorder.h"

static void output_push_orphan(struct Output *output)
{
	int wsi = output->visible_workspace;

	/*
	 * Remember the workspace this output was showing. Next new output
	 * will prefer it over "lowest unused", so unplug + re-plug
	 * restores the previous workspace instead of orphaning windows on
	 * a workspace nobody is looking at.
	 */
	if (wsi >= 0 && (size_t)wsi < wm_user_n_workspaces() &&
	    wm.n_orphan_ws < ARRAY_SIZE(wm.orphan_ws))
		wm.orphan_ws[wm.n_orphan_ws++] = wsi;
}

static void output_handle_removed(void *data, struct river_output_v1 *obj)
{
	struct Output *output = data;
	if (!output->removed)
		output_push_orphan(output);
	output->removed = true;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_output_remove(output);
}

static void output_handle_wl_output(void *data, struct river_output_v1 *obj,
				    u32 name)
{
}

static void output_handle_position(void *data, struct river_output_v1 *obj,
				   s32 x, s32 y)
{
	struct Output *output = data;
	if (output->x == x && output->y == y)
		return;
	output->x = x;
	output->y = y;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_output_position(output);
}

static void output_handle_dimensions(void *data, struct river_output_v1 *obj,
				     s32 width, s32 height)
{
	struct Output *output = data;
	if (output->width == width && output->height == height)
		return;
	output->width = width;
	output->height = height;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_output_dimensions(output);
}

static void output_handle_capture_sessions(void *data,
					   struct river_output_v1 *obj,
					   u32 count)
{
	struct Output *output = data;

	output->capture_sessions = count;
}

const struct river_output_v1_listener river_output_listener = {
	.removed = output_handle_removed,
	.wl_output = output_handle_wl_output,
	.position = output_handle_position,
	.dimensions = output_handle_dimensions,
	.capture_sessions = output_handle_capture_sessions,
};

static void
output_handle_non_exclusive_area(void *data,
				 struct river_layer_shell_output_v1 *obj, s32 x,
				 s32 y, s32 width, s32 height)
{
	struct Output *output = data;

	if (output->has_usable && output->usable_x == x &&
	    output->usable_y == y && output->usable_width == width &&
	    output->usable_height == height)
		return;
	output->has_usable = true;
	output->usable_x = x;
	output->usable_y = y;
	output->usable_width = width;
	output->usable_height = height;
	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_output_usable(output);
}

const struct river_layer_shell_output_v1_listener layer_shell_output_listener = {
	.non_exclusive_area = output_handle_non_exclusive_area,
};

void output_attach_layer_shell(struct Output *output)
{
	if (layer_shell_v1 == NULL || output->ls_output != NULL ||
	    output->removed)
		return;
	output->ls_output =
		river_layer_shell_v1_get_output(layer_shell_v1, output->obj);
	if (output->ls_output != NULL)
		river_layer_shell_output_v1_add_listener(
			output->ls_output, &layer_shell_output_listener,
			output);
}

void output_set_layer_default(struct Output *output)
{
	if (output == NULL || output->ls_output == NULL ||
	    wm.layer_default == output)
		return;
	river_layer_shell_output_v1_set_default(output->ls_output);
	wm.layer_default = output;
}

void output_maybe_destroy(struct Output *output)
{
	struct Seat *seat;

	if (!output->removed)
		return;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->output == output)
			seat->output = NULL;
		if (seat->pointer_output == output)
			seat->pointer_output = NULL;
	}
	if (wm.layer_default == output)
		wm.layer_default = NULL;
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link)
			if (w->fullscreen_output == output)
				w->fullscreen_output = NULL;
	}
	if (output->ls_output != NULL)
		river_layer_shell_output_v1_destroy(output->ls_output);
	recorder_output_freed(output);
	river_output_v1_destroy(output->obj);
	wl_list_remove(&output->link);
	free(output);
}

/* Return any output already showing the given workspace, or NULL. */
struct Output *output_showing(int workspace_index)
{
	struct Output *o;
	wl_list_for_each(o, &wm.outputs, link) {
		if (!o->removed && o->visible_workspace == workspace_index)
			return o;
	}
	return NULL;
}

/*
 * Return the Nth output in registration order (0-based), or NULL if
 * the index is out of range. Used by focus-output / move-to-output
 * to translate the user's "screen 0", "screen 1" into our Output*.
 */
struct Output *output_at_index(int n)
{
	struct Output *o;
	int i = 0;

	if (n < 0)
		return NULL;
	wl_list_for_each(o, &wm.outputs, link) {
		if (i++ == n)
			return o;
	}
	return NULL;
}
