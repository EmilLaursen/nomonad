/* SPDX-License-Identifier: 0BSD */
/*
 * river_window_manager_v1 listener and the manage / render cycle
 * implementation. wm_handle_manage_start and wm_handle_render_start
 * compose the per-cycle work: rule evaluation, layout, visibility,
 * borders, and per-seat state.
 */

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>

#include "binding.h"
#include "config.h"
#include "ctl.h"
#include "input.h"
#include "layout.h"
#include "nomonad.h"
#include "recorder.h"
#include "restore.h"
#include "status.h"
#include "timing.h"

static void wm_handle_unavailable(void *data,
				  struct river_window_manager_v1 *obj)
{
	fprintf(stderr, "error: another window manager is already running\n");
	exit(1);
}

static void wm_handle_finished(void *data, struct river_window_manager_v1 *obj)
{
	restore_exec();
	exit(0);
}

static bool window_in_op(const struct Window *w)
{
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->op_window == w &&
		    (seat->op == SEAT_OP_MOVE || seat->op == SEAT_OP_RESIZE))
			return true;
	}
	return false;
}

static void center_window(struct Window *w, const struct Output *out)
{
	s32 fw, fh;
	s32 fx, fy;

	if (out == NULL || out->width <= 0 || out->height <= 0)
		return;
	fw = (w->width > 0) ? w->width : out->width / 2;
	fh = (w->height > 0) ? w->height : out->height / 2;
	fx = out->x + (out->width - fw) / 2;
	fy = out->y + (out->height - fh) / 2;
	window_set_position(w, fx, fy);
	window_propose_dimensions(w, fw, fh);
	w->float_x = fx;
	w->float_y = fy;
	w->float_width = fw;
	w->float_height = fh;
}

static bool rule_matches(const struct FloatRule *r, const struct Window *w)
{
	const char *target = NULL;
	switch (r->kind) {
	case RULE_MATCH_APP_ID:
		target = w->app_id;
		break;
	case RULE_MATCH_TITLE:
		target = w->title;
		break;
	case RULE_MATCH_PARENT:
		return w->has_parent;
	}
	if (target == NULL || r->pattern == NULL)
		return false;
	if (r->glob)
		return fnmatch(r->pattern, target, 0) == 0;
	return strcmp(target, r->pattern) == 0;
}

static bool pad_matches(const struct ScratchpadDef *def, const struct Window *w)
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
 * Compute and stamp the scratchpad's float rect from the def's rational
 * geometry against the seat's current visible output. Falls back to the
 * existing center_window helper when the def has no geometry set. Only
 * called on the false -> true transition of is_scratchpad so a user
 * who manually moved the pad after an unstash does not get yanked back
 * to the configured rect every cycle.
 */
static void scratchpad_apply_geometry(struct Window *w,
				      const struct ScratchpadDef *def)
{
	struct Output *out;
	s32 fw, fh;
	s32 fx, fy;

	out = output_showing(w->workspace_index);
	if (out == NULL)
		return;
	if (!def->geom.set) {
		center_window(w, out);
		return;
	}
	fx = out->x + (s32)((float)out->width * def->geom.x);
	fy = out->y + (s32)((float)out->height * def->geom.y);
	fw = (s32)((float)out->width * def->geom.w);
	fh = (s32)((float)out->height * def->geom.h);
	if (fw < 1)
		fw = 1;
	if (fh < 1)
		fh = 1;
	window_set_position(w, fx, fy);
	window_propose_dimensions(w, fw, fh);
	w->float_x = fx;
	w->float_y = fy;
	w->float_width = fw;
	w->float_height = fh;
}

/*
 * Walk every window, evaluate defrules. First-match-wins: a window
 * matched by an (ignore ...) rule has its `ignored` flag set and is
 * thereafter skipped by layout, visibility, borders, and focus. A
 * window matched by a (float ...) rule gets `floating = true` and is
 * recentered if :center true and it just transitioned. The user's
 * explicit float override (toggle-float, or auto-float-on-drag) wins
 * against float rules but does NOT override an ignore match.
 */
/*
 * Match each window against the configured scratchpad defs. On the
 * false -> true transition of is_scratchpad we stamp the configured
 * geometry (or center fallback) onto the float rect; on subsequent
 * cycles the user-positioned rect is left alone. is_scratchpad implies
 * floating, so we also flip float_override to FORCE_FLOAT so the float
 * rule loop below cannot accidentally retile the pad.
 *
 * Pads matched here pre-empt float rules (a pad def names a more
 * specific intent than (float ...) and the user expects the pad
 * geometry to apply, not :center).
 */
static void scratchpad_drop_label(struct Window *w)
{
	if (!w->is_scratchpad || w->scratch_name == NULL)
		return;
	free(w->scratch_name);
	w->scratch_name = NULL;
	w->is_scratchpad = (w->workspace_index == wm_scratch_ws_index());
}

static void apply_scratchpads(struct Window *w)
{
	const struct ScratchpadDef *match = NULL;

	w->was_scratchpad = w->is_scratchpad;
	if (g_config == NULL || g_config->n_scratchpads == 0) {
		/*
		 * Drop the label if the def disappeared via a config
		 * reload. A stashed window becomes a dynamic stash that
		 * toggle-scratchpad reclaims, anything else an ordinary
		 * float.
		 */
		scratchpad_drop_label(w);
		return;
	}
	/*
	 * If the window already has a scratch name, prefer the def with
	 * that name over a fresh matcher pass: an unstashed pad whose
	 * title has changed should still resolve to itself.
	 */
	if (w->scratch_name != NULL) {
		for (size_t i = 0; i < g_config->n_scratchpads; i++) {
			if (strcmp(g_config->scratchpads[i].name,
				   w->scratch_name) == 0) {
				match = &g_config->scratchpads[i];
				break;
			}
		}
	}
	if (match == NULL) {
		for (size_t i = 0; i < g_config->n_scratchpads; i++) {
			if (pad_matches(&g_config->scratchpads[i], w)) {
				match = &g_config->scratchpads[i];
				break;
			}
		}
	}
	if (match == NULL) {
		/*
		 * The pad's def is gone (config reload removed it). Same
		 * label drop as above.
		 */
		scratchpad_drop_label(w);
		return;
	}
	if (!w->is_scratchpad || w->scratch_name == NULL ||
	    strcmp(w->scratch_name, match->name) != 0) {
		free(w->scratch_name);
		w->scratch_name = strdup(match->name);
	}
	w->is_scratchpad = true;
	w->float_override = FLOAT_FORCE_FLOAT;
	if (!w->was_scratchpad)
		scratchpad_apply_geometry(w, match);
}

static void apply_rules_one(struct Window *w)
{
	w->was_floating = w->floating;
	w->ignored = false;
	apply_scratchpads(w);
	/* Ignore rules first: they pre-empt everything. */
	if (g_config != NULL) {
		for (size_t k = 0; k < g_config->n_rules; k++) {
			const struct FloatRule *r = &g_config->rules[k];
			if (r->verb != RULE_VERB_IGNORE)
				continue;
			if (rule_matches(r, w)) {
				w->ignored = true;
				break;
			}
		}
	}
	if (w->ignored) {
		w->floating = false;
		return;
	}
	if (w->float_override == FLOAT_FORCE_FLOAT) {
		w->floating = true;
		return;
	}
	if (w->float_override == FLOAT_FORCE_TILE) {
		w->floating = false;
		return;
	}
	/* FLOAT_AUTO: float rules decide. */
	w->floating = false;
	if (g_config == NULL)
		return;
	for (size_t k = 0; k < g_config->n_rules; k++) {
		const struct FloatRule *r = &g_config->rules[k];
		if (r->verb != RULE_VERB_FLOAT)
			continue;
		if (!rule_matches(r, w))
			continue;
		w->floating = true;
		if (r->center && !w->was_floating) {
			struct Output *o = output_showing(w->workspace_index);
			center_window(w, o);
		}
		break;
	}
}

static void apply_rules(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;
		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			/*
			 * Gate: rule inputs (app_id, title, has_parent,
			 * float_override) and the rule list itself only
			 * change at known points; non-dirty windows keep
			 * last cycle's floating / ignored / scratchpad
			 * latches. See the rules_dirty comment on
			 * struct Window for the set sites.
			 */
			if (!w->rules_dirty)
				continue;
			apply_rules_one(w);
			w->rules_dirty = false;
			if (w->was_floating != w->floating)
				wm.restack_floats = true;
			if ((w->floating || w->ignored) &&
			    !w->dimensions_synced) {
				window_propose_dimensions(w, 0, 0);
				w->float_needs_place = w->floating;
			}
		}
	}
}

struct Window *wm_workspace_fullscreen(struct Workspace *ws)
{
	struct Window *w;
	wl_list_for_each(w, &ws->windows, workspace_link) {
		if (w->fullscreen)
			return w;
	}
	return NULL;
}

/*
 * Mark each window's visible_desired. A window is visible iff its
 * workspace is the visible_workspace of any output, with one twist:
 * if the workspace has a fullscreen window, only that window is
 * visible (NBFULL semantics). Ignored windows are not touched here,
 * so apply_visibility() leaves their show/hide state alone. Single
 * pass: for each workspace, look up whether any output shows it and
 * compute visibility in line.
 */
static void update_visibility(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws = &wm.workspaces[i];
		bool shown = (output_showing((int)i) != NULL);
		struct Window *fs = shown ? wm_workspace_fullscreen(ws) : NULL;
		struct Window *w;
		wl_list_for_each(w, &ws->windows, workspace_link) {
			if (w->ignored)
				continue;
			w->visible_desired = shown && (fs == NULL || w == fs);
		}
	}
}

static s32 clamp_span(s32 pos, s32 len, s32 lo, s32 span)
{
	if (len <= 0 || len >= span)
		return lo;
	if (pos < lo)
		return lo;
	if (pos + len > lo + span)
		return lo + span - len;
	return pos;
}

static void float_follow_output(struct Window *w, const struct Output *out)
{
	s32 nx, ny;

	if (w->float_anchored && w->anchor_x == out->x &&
	    w->anchor_y == out->y && w->anchor_width == out->width &&
	    w->anchor_height == out->height)
		return;
	if (w->float_anchored && w->position_synced) {
		nx = clamp_span(out->x + (w->x - w->anchor_x), w->width, out->x,
				out->width);
		ny = clamp_span(out->y + (w->y - w->anchor_y), w->height,
				out->y, out->height);
		window_set_position(w, nx, ny);
		w->float_x = nx;
		w->float_y = ny;
	}
	w->float_anchored = true;
	w->anchor_x = out->x;
	w->anchor_y = out->y;
	w->anchor_width = out->width;
	w->anchor_height = out->height;
}

void wm_float_anchor_to(struct Window *w, const struct Output *out)
{
	if (out == NULL)
		return;
	w->float_anchored = true;
	w->anchor_x = out->x;
	w->anchor_y = out->y;
	w->anchor_width = out->width;
	w->anchor_height = out->height;
}

static struct Output usable_rect(const struct Output *out)
{
	struct Output r = *out;

	if (out->has_usable && out->usable_width > 0 &&
	    out->usable_height > 0) {
		r.x = out->usable_x;
		r.y = out->usable_y;
		r.width = out->usable_width;
		r.height = out->usable_height;
	}
	return r;
}

/*
 * Run each output's visible workspace through that workspace's
 * layout. Windows in an interactive move/resize op are skipped so
 * the layout does not fight the user's drag. Outer struts and the
 * inner-gap / border insets are sourced from the active config and
 * refreshed every cycle so hot reload picks up new values.
 */
static void apply_layout(void)
{
	const struct GapsConfig *gaps = (g_config != NULL) ? &g_config->gaps :
							     NULL;
	const struct BordersConfig *borders =
		(g_config != NULL) ? &g_config->borders : NULL;

	struct Output *out;
	wl_list_for_each(out, &wm.outputs, link) {
		struct Window *focused;
		struct Window **arr;
		struct Workspace *ws;
		struct Window *fs;
		struct Output adj;
		struct Window *w;
		bool grow_failed;
		bool smart_gaps;
		bool smart_borders;
		struct Seat *s;
		size_t n;

		if (out->visible_workspace < 0 ||
		    (size_t)out->visible_workspace >= wm.n_workspaces)
			continue;
		ws = &wm.workspaces[out->visible_workspace];
		ws->layout_arr_len = 0;
		if (wl_list_empty(&ws->windows))
			continue;
		wl_list_for_each(w, &ws->windows, workspace_link) {
			if (w->floating && !w->ignored && !window_in_op(w))
				float_follow_output(w, out);
		}

		/*
		 * Fullscreen takes precedence over the workspace layout:
		 * cover the whole output with no gap and no border. The
		 * other windows in the workspace are hidden by
		 * update_visibility, so we do not lay them out.
		 */
		fs = wm_workspace_fullscreen(ws);
		if (fs != NULL && fs->fullscreen_client) {
			window_set_clip(fs, 0, 0);
			continue;
		}
		if (fs != NULL) {
			adj = usable_rect(out);
			window_set_position(fs, adj.x, adj.y);
			window_propose_dimensions(fs, adj.width, adj.height);
			window_set_bounds(fs, adj.width, adj.height);
			window_set_clip(fs, adj.width, adj.height);
			continue;
		}

		/*
		 * Collect tile candidates in one pass. The layout_arr buffer
		 * grows on demand; doubling at the cap-exceeded boundary keeps
		 * the steady-state cycle allocation-free once cap stabilises.
		 * We need the tile count before computing the gap / border
		 * insets so :smart can drop them when n == 1.
		 */
		n = 0;
		grow_failed = false;
		wl_list_for_each(w, &ws->windows, workspace_link) {
			if (window_in_op(w) || w->floating || w->ignored)
				continue;
			if (n >= ws->layout_arr_cap) {
				struct Window **grown;
				size_t newcap;

				newcap = ws->layout_arr_cap ?
						 ws->layout_arr_cap * 2 :
						 8;
				grown = realloc(ws->layout_arr,
						newcap * sizeof(*grown));
				if (grown == NULL) {
					grow_failed = true;
					break;
				}
				ws->layout_arr = grown;
				ws->layout_arr_cap = newcap;
			}
			ws->layout_arr[n++] = w;
		}
		if (grow_failed)
			continue;
		ws->layout_arr_len = n;
		arr = ws->layout_arr;

		/*
		 * Smart suppression: a single tile-eligible window means the
		 * lone tile should look chromeless. :smart on the gaps block
		 * drops the outer struts and the per-cell inner inset; :smart
		 * on the borders block drops the per-cell border inset (the
		 * visual border on the sole tile is suppressed by
		 * apply_borders, which uses the same predicate). Independent
		 * toggles - if you only want one of the two effects, set only
		 * that block's :smart.
		 */
		smart_gaps = (gaps != NULL) && gaps->smart && n == 1;
		smart_borders = (borders != NULL) && borders->smart && n == 1;

		adj = usable_rect(out);
		if (gaps != NULL && !smart_gaps) {
			adj.x += gaps->left;
			adj.y += gaps->top;
			adj.width -= gaps->left + gaps->right;
			adj.height -= gaps->top + gaps->bottom;
		}
		if (adj.width <= 0 || adj.height <= 0)
			continue;

		struct Layout *active = workspace_layout(ws);

		active->inner_gap = (gaps != NULL && !smart_gaps) ?
					    gaps->inner :
					    0;
		active->border = (borders != NULL && !smart_borders) ?
					 borders->width :
					 0;
		/*
		 * Pick a seat focus for this output's workspace. Stateful
		 * layouts (BSP) use it to position new windows next to the
		 * leaf the user is currently looking at; stateless layouts
		 * ignore it. NULL when no seat is on this output, or when
		 * the focused window is on a different workspace.
		 */
		focused = NULL;
		wl_list_for_each(s, &wm.seats, link) {
			if (s->output != out)
				continue;
			if (s->focused == NULL ||
			    s->focused->workspace_index !=
				    out->visible_workspace)
				continue;
			focused = s->focused;
			break;
		}
		if (n > 0)
			active->apply(active, &adj, arr, n, focused);
	}
}

static bool ws_in(const int *set, size_t n, int wsi)
{
	for (size_t i = 0; i < n; i++)
		if (set[i] == wsi)
			return true;
	return false;
}

static void rescreen(void)
{
	size_t n_keep = 0, n_alive = 0;
	struct Output *focus = NULL;
	bool any_removed = false;
	struct Output *out;
	struct Seat *seat;
	int keep[64];
	int best;

	wl_list_for_each(out, &wm.outputs, link) {
		if (out->removed)
			any_removed = true;
		else
			n_alive++;
	}
	if (!any_removed || n_alive == 0 || n_alive > ARRAY_SIZE(keep))
		return;
	if (!wl_list_empty(&wm.seats)) {
		seat = wl_container_of(wm.seats.next, seat, link);
		focus = seat->output;
	}
	if (focus != NULL && focus->visible_workspace >= 0 &&
	    (size_t)focus->visible_workspace < wm_user_n_workspaces())
		keep[n_keep++] = focus->visible_workspace;
	while (n_keep < n_alive) {
		best = -1;
		wl_list_for_each(out, &wm.outputs, link) {
			int v = out->visible_workspace;

			if (v < 0 || (size_t)v >= wm_user_n_workspaces() ||
			    ws_in(keep, n_keep, v))
				continue;
			if (best < 0 || v < best)
				best = v;
		}
		if (best < 0)
			break;
		keep[n_keep++] = best;
	}
	wl_list_for_each(out, &wm.outputs, link) {
		if (out->removed || ws_in(keep, n_keep, out->visible_workspace))
			continue;
		for (size_t i = 0; i < n_keep; i++) {
			if (output_showing(keep[i]) == NULL) {
				out->visible_workspace = keep[i];
				break;
			}
		}
	}
	wm.dirty_layout = true;
	wm.dirty_status = true;
}

static void restack_floats(void)
{
	if (!wm.restack_floats)
		return;
	wm.restack_floats = false;
	for (size_t i = 0; i < wm.n_workspaces; i++)
		if (wm_workspace_fullscreen(&wm.workspaces[i]) == NULL)
			seat_raise_floats((int)i);
}

static void sync_client_fullscreen(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			struct Output *want = NULL;

			if (w->closed)
				continue;
			if (w->fullscreen && w->fullscreen_client) {
				want = output_showing(w->workspace_index);
				if (want == NULL)
					want = w->fullscreen_output;
			}
			if (want == w->fullscreen_output)
				continue;
			if (want != NULL)
				river_window_v1_fullscreen(w->obj, want->obj);
			else
				river_window_v1_exit_fullscreen(w->obj);
			w->fullscreen_output = want;
			w->position_synced = false;
			w->dimensions_synced = false;
		}
	}
}

static void sync_fullscreen(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			bool inform;

			if (w->closed)
				continue;
			if (w->fullscreen && !w->fullscreen_seen &&
			    w->floating && w->width > 0 && w->height > 0) {
				w->float_x = w->x;
				w->float_y = w->y;
				w->float_width = w->width;
				w->float_height = w->height;
			}
			if (!w->fullscreen && w->fullscreen_seen) {
				wm.restack_floats = true;
				if (w->floating && w->float_width > 0 &&
				    w->float_height > 0) {
					window_set_position(w, w->float_x,
							    w->float_y);
					window_propose_dimensions(
						w, w->float_width,
						w->float_height);
				}
				wm.dirty_layout = true;
			}
			w->fullscreen_seen = w->fullscreen;
			inform = w->fullscreen && w->fullscreen_client;
			if (inform == w->fullscreen_informed)
				continue;
			w->fullscreen_informed = inform;
			if (inform)
				river_window_v1_inform_fullscreen(w->obj);
			else
				river_window_v1_inform_not_fullscreen(w->obj);
		}
	}
}

static void sync_tiled(void)
{
	const u32 all =
		RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;

	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			u32 edges;

			if (w->closed)
				continue;
			if ((w->floating || w->ignored) && !w->fullscreen) {
				window_set_clip(w, 0, 0);
				window_set_bounds(w, 0, 0);
			}
			edges = (w->floating || w->ignored) ?
					RIVER_WINDOW_V1_EDGES_NONE :
					all;
			if (w->tiled_synced && w->last_tiled == edges)
				continue;
			river_window_v1_set_tiled(w->obj, edges);
			w->last_tiled = edges;
			w->tiled_synced = true;
		}
	}
}

static void apply_presentation(void)
{
	struct Output *out;

	wl_list_for_each(out, &wm.outputs, link) {
		struct Window *fs = NULL;
		u32 mode;

		if (out->removed)
			continue;
		if (out->visible_workspace >= 0 &&
		    (size_t)out->visible_workspace < wm.n_workspaces)
			fs = wm_workspace_fullscreen(
				&wm.workspaces[out->visible_workspace]);
		mode = (fs != NULL &&
			fs->presentation_hint ==
				RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC) ?
			       RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC :
			       RIVER_OUTPUT_V1_PRESENTATION_MODE_VSYNC;
		if (out->presentation_synced && out->presentation_mode == mode)
			continue;
		river_output_v1_set_presentation_mode(out->obj, mode);
		out->presentation_mode = mode;
		out->presentation_synced = true;
	}
}

static void place_new_floats(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			const struct Output *out;
			struct Output area;
			s32 fx, fy;

			if (!w->float_needs_place)
				continue;
			if (!w->floating || w->ignored) {
				w->float_needs_place = false;
				continue;
			}
			if (w->width <= 0 || w->height <= 0 || window_in_op(w))
				continue;
			out = output_showing(w->workspace_index);
			if (out == NULL)
				continue;
			area = usable_rect(out);
			fx = area.x + (area.width - w->width) / 2;
			fy = area.y + (area.height - w->height) / 2;
			if (fx < area.x)
				fx = area.x;
			if (fy < area.y)
				fy = area.y;
			window_set_position(w, fx, fy);
			w->float_x = fx;
			w->float_y = fy;
			w->float_width = w->width;
			w->float_height = w->height;
			w->float_needs_place = false;
		}
	}
}

static void apply_visibility(void)
{
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws = &wm.workspaces[i];
		struct Window *w;
		wl_list_for_each(w, &ws->windows, workspace_link) {
			if (w->ignored)
				continue;
			if (w->visible_synced &&
			    w->last_shown == w->visible_desired)
				continue;
			if (w->visible_desired)
				river_window_v1_show(w->obj);
			else
				river_window_v1_hide(w->obj);
			w->last_shown = w->visible_desired;
			w->visible_synced = true;
		}
	}
}

static bool window_focused_anywhere(const struct Window *w)
{
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->focused == w)
			return true;
	}
	return false;
}

/*
 * Stretch an 8-bit channel into the protocol's 32-bit channel and
 * pre-multiply by the 8-bit alpha. The 0x01010101 factor expands a
 * single byte to a uniform u32 value (xx -> xxxxxxxx).
 */
static u32 channel_premul(u8 c, u8 a)
{
	if (a == 0xff)
		return (u32)c * 0x01010101u;
	return (u32)((u64)c * a * 0x01010101u / 0xffu);
}

/*
 * Count tile-eligible windows on a workspace. Used by smart-borders
 * and the harness extent invariant to detect the single-tile case.
 * Mirrors the predicate apply_layout uses to populate ws->layout_arr.
 * Returns 0 for workspaces whose layout is short-circuited by a
 * fullscreen window, so smart suppression does not fire on them.
 */
static int count_tile_candidates(struct Workspace *ws)
{
	struct Window *w;
	int n;

	if (wm_workspace_fullscreen(ws) != NULL)
		return 0;
	n = 0;
	wl_list_for_each(w, &ws->windows, workspace_link) {
		if (window_in_op(w) || w->floating || w->ignored)
			continue;
		n++;
	}
	return n;
}

/*
 * Set borders on every visible window. Windows focused on any seat
 * receive the focused color; others receive the unfocused color.
 * Called from the render sequence. Per-
 * window cache: skip if the seven-tuple (edges, width, r, g, b, a)
 * matches the last call. The same focus/borders/colors per cycle is
 * the steady-state case, so the cache normally elides the entire pass.
 */
static bool window_captured(const struct Window *w)
{
	const struct Output *out;

	if (w->capture_sessions > 0)
		return true;
	out = output_showing(w->workspace_index);
	return out != NULL && out->capture_sessions > 0;
}

static void apply_borders(void)
{
	const struct BordersConfig *b;
	u32 edges;

	if (g_config == NULL)
		return;
	b = &g_config->borders;
	edges = RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;
	if (b->width <= 0)
		edges = RIVER_WINDOW_V1_EDGES_NONE;

	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws = &wm.workspaces[i];
		struct Window *w;
		bool smart_active;

		smart_active = b->smart && count_tile_candidates(ws) == 1;

		wl_list_for_each(w, &ws->windows, workspace_link) {
			u32 r_pre, g_pre, b_pre, a_pre;
			u32 w_edges;
			u32 rgba;
			s32 w_width;
			u8 r8, g8, b8, a8;
			bool is_tile;

			if (!w->visible_desired || w->ignored)
				continue;
			is_tile = !w->floating && !w->fullscreen &&
				  !window_in_op(w);
			w_edges = w->fullscreen ? RIVER_WINDOW_V1_EDGES_NONE :
						  edges;
			w_width = w->fullscreen ? 0 : b->width;
			/*
			 * Smart border suppression only fires on the lone
			 * tile - a coexisting float keeps its border so the
			 * user can still tell it apart visually.
			 */
			if (smart_active && is_tile) {
				w_edges = RIVER_WINDOW_V1_EDGES_NONE;
				w_width = 0;
			}
			rgba = window_focused_anywhere(w) ? b->focused_rgba :
							    b->unfocused_rgba;
			if (b->captured_set && window_captured(w)) {
				rgba = b->captured_rgba;
				w_edges = RIVER_WINDOW_V1_EDGES_TOP |
					  RIVER_WINDOW_V1_EDGES_BOTTOM |
					  RIVER_WINDOW_V1_EDGES_LEFT |
					  RIVER_WINDOW_V1_EDGES_RIGHT;
				if (w_width <= 0)
					w_width = (b->width > 0) ? b->width : 3;
			}
			r8 = (rgba >> 24) & 0xff;
			g8 = (rgba >> 16) & 0xff;
			b8 = (rgba >> 8) & 0xff;
			a8 = rgba & 0xff;
			r_pre = channel_premul(r8, a8);
			g_pre = channel_premul(g8, a8);
			b_pre = channel_premul(b8, a8);
			a_pre = (u32)a8 * 0x01010101u;
			if (w->borders_synced &&
			    w->last_border_edges == w_edges &&
			    w->last_border_width == w_width &&
			    w->last_border_r == r_pre &&
			    w->last_border_g == g_pre &&
			    w->last_border_b == b_pre &&
			    w->last_border_a == a_pre)
				continue;
			river_window_v1_set_borders(w->obj, w_edges, w_width,
						    r_pre, g_pre, b_pre, a_pre);
			w->last_border_edges = w_edges;
			w->last_border_width = w_width;
			w->last_border_r = r_pre;
			w->last_border_g = g_pre;
			w->last_border_b = b_pre;
			w->last_border_a = a_pre;
			w->borders_synced = true;
		}
	}
}

static u64 realtime_unix_ns(void)
{
	struct timespec ts;

	/*
	 * CLOCK_REALTIME via vDSO: ~25 ns on x86_64, no syscall.
	 * Used for the status broadcast timestamps, which need to
	 * correlate with journald / syslog (those are wall-clock).
	 * The recorder and NM_TIMING use CLOCK_MONOTONIC instead -
	 * different consumers, different invariants.
	 */
	clock_gettime(CLOCK_REALTIME, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void wm_handle_manage_start(void *data,
				   struct river_window_manager_v1 *obj)
{
	struct Output *output, *output_tmp;
	struct Seat *seat, *seat_tmp;
	bool entry_dirty;
	bool entry_layout;
	u64 time_start_ns;

	timing_begin_manage();
	time_start_ns = realtime_unix_ns();

	/*
	 * Snapshot dirty_status / dirty_layout as they were on entry,
	 * then clear so the cycle's own work (reload, seat_manage's
	 * action dispatch, in-listener mutation) can re-set them. The
	 * apply_layout gate and the timing-log dirty observation both
	 * read entry-vs-end of the cycle; the broadcast publish in step
	 * 5 will gate on dirty_status the same way.
	 */
	entry_dirty = wm.dirty_status;
	entry_layout = wm.dirty_layout;
	wm.dirty_status = false;
	wm.dirty_layout = false;

	/*
	 * Reload runs here (not from the poll loop) so the binding
	 * destroy/create + xkb_binding enable calls happen inside a
	 * manage sequence. SIGUSR1, inotify, and ACTION_RELOAD_CONFIG
	 * all schedule via schedule_reload() / manage_dirty.
	 */
	if (g_reload_pending) {
		g_reload_pending = 0;
		reload_now();
		entry_dirty = true;
		entry_layout = true;
	}

	rescreen();
	wl_list_for_each_safe(output, output_tmp, &wm.outputs, link) {
		output_maybe_destroy(output);
	}

	/* Reap closed windows across every workspace. */
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *window, *window_tmp;
		wl_list_for_each_safe(window, window_tmp,
				      &wm.workspaces[i].windows,
				      workspace_link) {
			window_maybe_destroy(window);
		}
	}

	restore_apply();

	wl_list_for_each_safe(seat, seat_tmp, &wm.seats, link) {
		seat_maybe_destroy(seat);
	}

	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *window;
		wl_list_for_each(window, &wm.workspaces[i].windows,
				 workspace_link) {
			window_manage(window);
		}
	}

	apply_rules();

	wl_list_for_each(output, &wm.outputs, link) {
		output_attach_layer_shell(output);
	}
	ctl_dispatch();
	wl_list_for_each(seat, &wm.seats, link) {
		seat_attach_layer_shell(seat);
		seat_manage(seat);
	}
	sync_fullscreen();
	sync_tiled();
	restack_floats();

	/*
	 * Gate the visibility + layout passes on the layout-dirty signal
	 * specifically. entry_layout captures listener-side layout edges
	 * (window add / close, app_id / title / parent / fullscreen,
	 * output geometry, reload) that arrived between manage cycles;
	 * wm.dirty_layout reflects in-cycle layout edges from action
	 * dispatch (workspace switch, swap, toggle-float, layout verbs,
	 * scratchpad). A pure focus or pointer-output cross sets
	 * dirty_status without dirty_layout, so a held focus-next on a
	 * populated workspace skips both passes here. The window setters
	 * in window.c short-circuit on identical values anyway, so the
	 * gate is an optimisation for the compute side; correctness still
	 * holds if we always ran the passes.
	 */
	bool layout_observed = entry_layout || wm.dirty_layout;
	if (layout_observed) {
		update_visibility();
		sync_client_fullscreen();
		apply_layout();
	}

	/*
	 * Status observation captures the broader signal: anything
	 * publishable. Drives both the timing log dirty marker and the
	 * status broadcast publish hook. status_publish hashes the
	 * snapshot so a false positive here only costs the build, not
	 * IO; manage_start is the right cycle moment because every
	 * dirty source has fired by now and the snapshot reflects the
	 * commit-state river will see at manage_finish.
	 */
	bool status_observed = entry_dirty || wm.dirty_status;
	timing_observe_dirty(status_observed);
	status_publish(status_observed, time_start_ns, realtime_unix_ns());

	river_window_manager_v1_manage_finish(window_manager_v1);
	if (restore_take_request())
		river_window_manager_v1_stop(window_manager_v1);

	timing_end_manage();
}

static void wm_handle_render_start(void *data,
				   struct river_window_manager_v1 *obj)
{
	struct Seat *seat;

	timing_begin_render();

	place_new_floats();
	apply_presentation();
	apply_visibility();
	apply_borders();

	wl_list_for_each(seat, &wm.seats, link) {
		seat_render(seat);
	}

	river_window_manager_v1_render_finish(window_manager_v1);

	timing_end_render();
}

static void wm_handle_window(void *data, struct river_window_manager_v1 *obj,
			     struct river_window_v1 *river_window)
{
	struct Window *window;
	struct Output *target;
	struct Seat *seat;
	int wsi;

	window = calloc(1, sizeof(*window));
	if (window == NULL)
		return;
	window->serial = ++wm.next_window_serial;
	window->obj = river_window;
	window->node = river_window_v1_get_node(window->obj);
	window->is_new = true;
	/*
	 * apply_rules' gate skips windows where rules_dirty is false.
	 * A freshly minted window has no app_id / title yet (those
	 * arrive on subsequent listener callbacks, each of which also
	 * sets rules_dirty), but the rule scan still needs to run on
	 * the empty-string case so ignore / float defaults latch.
	 */
	window->rules_dirty = true;

	/*
	 * Pick the focused output's visible workspace. Falls back to the
	 * first output, and finally to workspace 0 if no outputs exist
	 * yet (rare; brief moment before the first output event).
	 */
	target = NULL;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->output != NULL) {
			target = seat->output;
			break;
		}
	}
	if (target == NULL && !wl_list_empty(&wm.outputs))
		target = wl_container_of(wm.outputs.next, target, link);
	wsi = (target != NULL) ? target->visible_workspace : 0;
	if (wsi < 0 || (size_t)wsi >= wm.n_workspaces)
		wsi = 0;
	window->workspace_index = wsi;
	wm.restack_floats = true;

	river_window_v1_add_listener(window->obj, &river_window_listener,
				     window);

	wl_list_insert(&wm.workspaces[wsi].windows, &window->workspace_link);

	/*
	 * Auto-focus the new window for any seat sitting on this
	 * window's workspace, so spawning a window puts focus on it.
	 * river fires window_interaction only on user actions (clicks,
	 * etc.); without this hook seat->focused stays on whatever was
	 * focused before, and the user's mental model "spawn -> focus
	 * lands on the new window" (xmonad-style) breaks. Setting
	 * `interacted` rather than `focused` directly defers the
	 * commit to seat_manage which already handles the river-side
	 * focus_window request and the set_anchor wiring.
	 */
	wl_list_for_each(seat, &wm.seats, link) {
		struct Output *seat_out = (seat->output != NULL) ?
						  seat->output : target;
		if (seat_out == NULL)
			continue;
		if (seat_out->visible_workspace == wsi)
			seat->interacted = window;
	}

	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_window_add(window);
}

static void wm_handle_output(void *data, struct river_window_manager_v1 *obj,
			     struct river_output_v1 *river_output)
{
	struct Output *output;

	output = calloc(1, sizeof(*output));
	output->obj = river_output;
	output->visible_workspace = -1;

	/*
	 * Prefer the most recently orphaned workspace (LIFO). If a user
	 * just unplugged a monitor showing workspace 5 and re-plugged it,
	 * the new output gets workspace 5 back rather than the lowest
	 * unused one. Skip entries that have since become visible on
	 * another output (could happen via workspace_switch).
	 */
	while (wm.n_orphan_ws > 0) {
		int candidate = wm.orphan_ws[--wm.n_orphan_ws];
		if (candidate < 0 || (size_t)candidate >= wm_user_n_workspaces())
			continue;
		if (output_showing(candidate) == NULL) {
			output->visible_workspace = candidate;
			break;
		}
	}
	/*
	 * Fall back to the lowest-numbered workspace not already shown.
	 * Only the user range; the scratch slot must never appear on an
	 * output (and the unique-visible-workspace invariant would still
	 * hold even if it did, but the user would see the scratchpads).
	 */
	if (output->visible_workspace < 0) {
		output->visible_workspace = 0;
		for (size_t i = 0; i < wm_user_n_workspaces(); i++) {
			if (output_showing((int)i) == NULL) {
				output->visible_workspace = (int)i;
				break;
			}
		}
	}

	river_output_v1_add_listener(output->obj, &river_output_listener,
				     output);

	wl_list_insert(wm.outputs.prev, &output->link);

	wm.dirty_status = true;
	wm.dirty_layout = true;
	recorder_event_output_add(output);
}

static void wm_handle_seat(void *data, struct river_window_manager_v1 *obj,
			   struct river_seat_v1 *river_seat)
{
	struct Seat *seat;
	int tfd;

	seat = calloc(1, sizeof(*seat));
	seat->obj = river_seat;
	seat->is_new = true;
	wl_list_init(&seat->xkb_bindings);
	wl_list_init(&seat->pointer_bindings);
	seat->repeat_timer_fd = -1; /* explicit; calloc zero == stdin */
	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd >= 0)
		seat->repeat_timer_fd = tfd;

	river_seat_v1_add_listener(seat->obj, &river_seat_listener, seat);

	/*
	 * Per-seat xkb-bindings handle: needed for ensure_next_key_eaten /
	 * cancel_ensure_next_key_eaten (submap entry / cancel) and to
	 * receive ate_unbound_key (submap teardown). xkb_bindings_v1 may
	 * be NULL in the headless harness; the proxy stub returns a fake
	 * proxy regardless, but guard for clarity.
	 */
	if (xkb_bindings_v1 != NULL) {
		seat->xkb_seat = river_xkb_bindings_v1_get_seat(
			xkb_bindings_v1, seat->obj);
		if (seat->xkb_seat != NULL)
			river_xkb_bindings_seat_v1_add_listener(
				seat->xkb_seat,
				&river_xkb_bindings_seat_listener, seat);
	}

	wl_list_insert(wm.seats.prev, &seat->link);

	wm.dirty_status = true;
	recorder_event_seat_add(seat);
}

static void wm_handle_session_locked(void *data,
				     struct river_window_manager_v1 *obj)
{
	recorder_event_session_locked();
}
static void wm_handle_session_unlocked(void *data,
				       struct river_window_manager_v1 *obj)
{
	recorder_event_session_unlocked();
}

const struct river_window_manager_v1_listener wm_listener = {
	.unavailable = wm_handle_unavailable,
	.finished = wm_handle_finished,
	.manage_start = wm_handle_manage_start,
	.render_start = wm_handle_render_start,
	.session_locked = wm_handle_session_locked,
	.session_unlocked = wm_handle_session_unlocked,
	.window = wm_handle_window,
	.output = wm_handle_output,
	.seat = wm_handle_seat,
};

/*
 * Nine user-addressable workspaces matching xmonad-defaults, plus one
 * trailing scratch slot for stashed scratchpads. The scratch slot is
 * never assigned to an output and is not reachable via numbered
 * workspace switches; it only ever holds windows that the scratchpad
 * actions parked there.
 */
#define NM_DEFAULT_WORKSPACES (NM_DEFAULT_USER_WORKSPACES + 1)

void layout_init_from_kind(struct Layout *l, enum LayoutKind kind)
{
	/*
	 * Each case installs that layout's documented defaults
	 * (nmaster=1, ratio=0.5 for the tall family; empty tree for
	 * BSP). Per-layout compile-time opt-out gates both this case
	 * and the descriptor-table row in layout_names.c with the same
	 * NM_NO_<KIND> macro - if a config asks for a disabled kind
	 * the parser rejects the name, so reaching this default branch
	 * normally means a stale on-disk index after a layout was
	 * removed. Falling back to TALL keeps the workspace tileable.
	 */
	switch (kind) {
#ifndef NM_NO_BSP
	case LAYOUT_BSP:
		layout_init_bsp(l);
		break;
#endif
#ifndef NM_NO_DECK
	case LAYOUT_DECK:
		layout_init_deck(l);
		break;
#endif
#ifndef NM_NO_MIRROR_TALL
	case LAYOUT_MIRROR_TALL:
		layout_init_mirror_tall(l, 1, 0.5f);
		break;
#endif
#ifndef NM_NO_THREE_COLUMNS
	case LAYOUT_THREE_COLUMNS:
		layout_init_three_columns(l, 1, 0.5f);
		break;
#endif
#ifndef NM_NO_GRID
	case LAYOUT_GRID:
		layout_init_grid(l);
		break;
#endif
#ifndef NM_NO_TABBED
	case LAYOUT_TABBED:
		layout_init_tabbed(l);
		break;
#endif
#ifndef NM_NO_SPIRAL
	case LAYOUT_SPIRAL:
		layout_init_spiral(l);
		break;
#endif
#ifndef NM_NO_TWO_PANE
	case LAYOUT_TWO_PANE:
		layout_init_two_pane(l, 0.5f);
		break;
#endif
#ifndef NM_NO_RESIZABLE_TALL
	case LAYOUT_RESIZABLE_TALL:
		layout_init_resizable_tall(l, 1, 0.5f);
		break;
#endif
	case LAYOUT_TALL:
	default:
		layout_init_tall(l, 1, 0.5f);
		break;
	}
}

/*
 * Populate ws->layouts[] from the active config's layout_kinds list.
 * On hot reload the caller is expected to layout_deinit each prior
 * entry (range [0, ws->n_layouts)) before calling - this just
 * fills the array. Always installs at least one entry; an empty
 * config (g_config NULL or zero kinds) seeds a single TALL slot
 * so apply_layout always has something to dispatch to.
 */
void workspace_init_layouts(struct Workspace *ws)
{
	const struct Config *cfg = g_config;
	size_t n;
	size_t i;

	n = (cfg != NULL && cfg->n_layout_kinds > 0) ? cfg->n_layout_kinds : 1;
	if (n > NM_LAYOUTS_MAX)
		n = NM_LAYOUTS_MAX;
	for (i = 0; i < n; i++) {
		enum LayoutKind k = (cfg != NULL && cfg->n_layout_kinds > 0) ?
					    cfg->layout_kinds[i] :
					    LAYOUT_TALL;
		layout_init_from_kind(&ws->layouts[i], k);
	}
	ws->n_layouts = n;
	if (ws->current_layout_index >= ws->n_layouts)
		ws->current_layout_index = 0;
}

static bool workspace_layouts_match_config(const struct Workspace *ws)
{
	const struct Config *cfg = g_config;
	size_t n;

	n = (cfg != NULL && cfg->n_layout_kinds > 0) ? cfg->n_layout_kinds : 1;
	if (n > NM_LAYOUTS_MAX)
		n = NM_LAYOUTS_MAX;
	if (ws->n_layouts != n)
		return false;
	for (size_t i = 0; i < n; i++) {
		enum LayoutKind k = (cfg != NULL && cfg->n_layout_kinds > 0) ?
					    cfg->layout_kinds[i] :
					    LAYOUT_TALL;
		if (ws->layouts[i].kind != k)
			return false;
	}
	return true;
}

void wm_swap_config(struct Config *staging)
{
	enum DecorationMode old_decoration;
	struct Seat *seat;

	old_decoration = (g_config != NULL) ? g_config->decoration :
					      DECORATION_SSD;
	wl_list_for_each(seat, &wm.seats, link) {
		/*
		 * Old config's SubmapDef pointers go away below; clear any
		 * active submap so submap_def_lookup does not chase a freed
		 * entry. binding_seat_cleanup wipes the submap's installed
		 * entries; the post-swap binding_register_from_config gives
		 * the seat its normal bindings back.
		 */
		if (seat->submap_active && seat->xkb_seat != NULL)
			river_xkb_bindings_seat_v1_cancel_ensure_next_key_eaten(
				seat->xkb_seat);
		seat->submap_active = false;
		seat->submap_exit_pending = false;
		binding_seat_cleanup(seat);
	}
	config_free(g_config);
	g_config = staging;
	wl_list_for_each(seat, &wm.seats, link) {
		if (!seat->is_new)
			binding_register_from_config(seat, g_config);
	}
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws = &wm.workspaces[i];
		size_t prev_index = ws->current_layout_index;
		enum LayoutKind prev_kind =
			(ws->n_layouts > 0) ? ws->layouts[prev_index].kind :
					      LAYOUT_TALL;
		size_t j;

		if (workspace_layouts_match_config(ws))
			continue;
		/*
		 * Re-init the cycle wholesale: deinit each old entry, fill
		 * from the new config. Then re-aim current_layout_index at
		 * whichever new slot still has the prior active kind so the
		 * user does not lose their place. If the prior kind is no
		 * longer in the cycle, snap to slot 0.
		 */
		for (j = 0; j < ws->n_layouts; j++)
			layout_deinit(&ws->layouts[j]);
		ws->n_layouts = 0;
		ws->current_layout_index = 0;
		workspace_init_layouts(ws);
		for (j = 0; j < ws->n_layouts; j++) {
			if (ws->layouts[j].kind == prev_kind) {
				ws->current_layout_index = j;
				break;
			}
		}
	}
	/*
	 * The rule list just changed wholesale; every window has to run
	 * through apply_rules again or stale ignore / float / scratchpad
	 * latches survive the swap. Mark all dirty before the next
	 * manage cycle. The cycle's manage_start has already cleared the
	 * WM-wide flags by the time we get here (reload runs near the
	 * top), so set both back true so the apply_layout / status
	 * publish gates downstream both fire.
	 */
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link)
			w->rules_dirty = true;
	}
	wm.dirty_status = true;
	wm.dirty_layout = true;
	input_apply_all();
	if (old_decoration != g_config->decoration) {
		for (size_t i = 0; i < wm.n_workspaces; i++) {
			struct Window *w;

			wl_list_for_each(w, &wm.workspaces[i].windows,
					 workspace_link) {
				window_apply_decoration(w);
			}
		}
	}
}

void wm_init(void)
{
	wl_list_init(&wm.outputs);
	wl_list_init(&wm.seats);

	wm.n_workspaces = NM_DEFAULT_WORKSPACES;
	wm.workspaces = calloc(wm.n_workspaces, sizeof(*wm.workspaces));
	for (size_t i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws = &wm.workspaces[i];
		char name[24];

		ws->index = (int)i;
		if ((int)i == wm_scratch_ws_index())
			snprintf(name, sizeof(name), "scratch");
		else
			snprintf(name, sizeof(name), "%zu", i + 1);
		ws->name = strdup(name);
		ws->n_layouts = 0;
		ws->current_layout_index = 0;
		workspace_init_layouts(ws);
		wl_list_init(&ws->windows);
	}
}
