/* SPDX-License-Identifier: 0BSD */
/*
 * Built-in layouts: Tall and BSP. Each layout's apply() walks its
 * canonical window order and calls window_apply_cell() (which proposes
 * a rect to the compositor with inner-gap and border insets applied).
 *
 * Tall is stateless: apply lays out directly from the passed list.
 *
 * BSP keeps a binary-space-partition tree on the heap. Each apply
 * reconciles the tree against the passed list (removing leaves whose
 * windows are gone, inserting new windows) and walks the tree to
 * compute leaf rects.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client-core.h>
#include <river-window-management-v1-client-protocol.h>

#include "nomonad.h"
#include "layout.h"

/*
 * Inset a layout-computed cell by inner_gap on all sides and by border
 * on all sides (river positions/dimensions describe content; borders
 * are drawn outside that rect). Skips the call if the result has no
 * area. If the client advertised a minimum size via dimensions_hint,
 * propose at least that much, a sub-minimum terminal renders badly.
 * The content clip box crops anything larger than the cell (a minimum
 * size, or a client that ignores the proposal) so a tile never draws
 * over its neighbour. set_dimension_bounds still uses the unclamped
 * cell so the client knows the actual cap, and forwards via
 * xdg_toplevel.configure_bounds so GTK/browser-style clients can size
 * their swapchain to the cell rather than the full output.
 */
static void window_apply_cell(struct Window *w, s32 x, s32 y, s32 width,
			      s32 height, s32 inner, s32 border)
{
	s32 inset = inner + border;
	s32 cell_w;
	s32 cell_h;

	x += inset;
	y += inset;
	width -= 2 * inset;
	height -= 2 * inset;
	/*
	 * Cell smaller than 2 * inset on either axis: the window has no
	 * room. Hiding it via visible_desired is the only consistent
	 * behavior - leaving the rect untouched lets the prior layout's
	 * coordinates linger and overlap with whatever neighbor took the
	 * collapsed space, which the harness's no-overlap invariant
	 * catches as an apparent layout fault. Reachable from BSP when
	 * ratio-skewed splits squeeze a sibling subtree to zero width or
	 * height. update_visibility resets visible_desired to its
	 * geometry-independent value at the start of every manage cycle,
	 * so the window comes back the moment it has space again.
	 */
	if (width <= 0 || height <= 0) {
		w->visible_desired = false;
		return;
	}
	cell_w = width;
	cell_h = height;
	if (w->min_width > 0 && cell_w < w->min_width)
		cell_w = w->min_width;
	if (w->min_height > 0 && cell_h < w->min_height)
		cell_h = w->min_height;
	window_set_position(w, x, y);
	window_propose_dimensions(w, cell_w, cell_h);
	window_set_bounds(w, width, height);
	window_set_clip(w, width, height);
}

/* ---- Tall ------------------------------------------------------------- */

/*
 * Tall: master area on the left at `ratio` of the output's width,
 * stack column on the right. Both areas subdivide vertically; the last
 * window in each area absorbs leftover pixels from integer division.
 */
static void layout_tall_apply(struct Layout *l, const struct Output *out,
			      struct Window **windows, size_t n,
			      struct Window *focused)
{
	size_t nstack;
	s32 master_w;
	s32 stack_w;
	s32 nmaster;
	s32 border;
	s32 inner;

	(void)focused; /* Tall is stateless; focus is irrelevant. */
	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	nmaster = l->u.tall.nmaster;
	if (nmaster < 0)
		nmaster = 0;
	if ((size_t)nmaster > n)
		nmaster = (s32)n;
	nstack = n - (size_t)nmaster;

	if (nmaster == 0)
		master_w = 0;
	else if (nstack == 0)
		master_w = out->width;
	else
		master_w = (s32)((float)out->width * l->u.tall.ratio);
	stack_w = out->width - master_w;

	inner = l->inner_gap;
	border = l->border;
	if (nmaster > 0) {
		s32 row_h = out->height / nmaster;

		for (s32 i = 0; i < nmaster; i++) {
			s32 y = out->y + i * row_h;
			s32 h = (i == nmaster - 1) ? (out->height - i * row_h) :
						     row_h;
			window_apply_cell(windows[i], out->x, y, master_w, h,
					  inner, border);
		}
	}
	if (nstack > 0) {
		s32 row_h = out->height / (s32)nstack;

		for (size_t i = 0; i < nstack; i++) {
			s32 y = out->y + (s32)i * row_h;
			s32 h = (i == nstack - 1) ?
					(out->height - (s32)i * row_h) :
					row_h;
			window_apply_cell(windows[(size_t)nmaster + i],
					  out->x + master_w, y, stack_w, h,
					  inner, border);
		}
	}
}

void layout_init_tall(struct Layout *l, s32 nmaster, float ratio)
{
	l->kind = LAYOUT_TALL;
	l->u.tall.nmaster = nmaster;
	l->u.tall.ratio = ratio;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = false;
	l->apply = layout_tall_apply;
	l->destroy = NULL;
	l->do_action = NULL; /* TALL has no layout-specific actions yet */
	l->set_anchor = NULL; /* stateless, no anchor to track */
	l->drop_window = NULL;
	l->restack = NULL;
}

/* ---- Mirror Tall ------------------------------------------------------ */
#ifndef NM_NO_MIRROR_TALL

/*
 * Mirror Tall: TALL transposed 90 degrees. Master row on top at
 * `ratio` of the output's height, stack row on bottom. Each row
 * subdivides horizontally (columns within the row); the last column
 * in each row absorbs the leftover pixels from integer division, so
 * total width is preserved exactly even when n does not divide
 * evenly.
 *
 * Shares struct LayoutTallParams with LAYOUT_TALL via u.tall: the
 * master ratio and master count actions in seat.c read u.tall
 * directly without dispatching on kind. The only difference is
 * which axis the params drive.
 */
static void layout_mirror_tall_apply(struct Layout *l, const struct Output *out,
				     struct Window **windows, size_t n,
				     struct Window *focused)
{
	size_t nstack;
	s32 master_h;
	s32 stack_h;
	s32 nmaster;
	s32 border;
	s32 inner;

	(void)focused; /* Mirror Tall is stateless; focus is irrelevant. */
	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	nmaster = l->u.tall.nmaster;
	if (nmaster < 0)
		nmaster = 0;
	if ((size_t)nmaster > n)
		nmaster = (s32)n;
	nstack = n - (size_t)nmaster;

	if (nmaster == 0)
		master_h = 0;
	else if (nstack == 0)
		master_h = out->height;
	else
		master_h = (s32)((float)out->height * l->u.tall.ratio);
	stack_h = out->height - master_h;

	inner = l->inner_gap;
	border = l->border;
	if (nmaster > 0) {
		s32 col_w = out->width / nmaster;

		for (s32 i = 0; i < nmaster; i++) {
			s32 x = out->x + i * col_w;
			s32 w = (i == nmaster - 1) ? (out->width - i * col_w) :
						     col_w;
			window_apply_cell(windows[i], x, out->y, w, master_h,
					  inner, border);
		}
	}
	if (nstack > 0) {
		s32 col_w = out->width / (s32)nstack;

		for (size_t i = 0; i < nstack; i++) {
			s32 x = out->x + (s32)i * col_w;
			s32 w = (i == nstack - 1) ?
					(out->width - (s32)i * col_w) :
					col_w;
			window_apply_cell(windows[(size_t)nmaster + i], x,
					  out->y + master_h, w, stack_h, inner,
					  border);
		}
	}
}

void layout_init_mirror_tall(struct Layout *l, s32 nmaster, float ratio)
{
	l->kind = LAYOUT_MIRROR_TALL;
	l->u.tall.nmaster = nmaster;
	l->u.tall.ratio = ratio;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = false;
	l->apply = layout_mirror_tall_apply;
	l->destroy = NULL;
	l->do_action = NULL;
	l->set_anchor = NULL;
	l->drop_window = NULL;
	l->restack = NULL;
}

#endif /* NM_NO_MIRROR_TALL */

/* ---- Three Columns ---------------------------------------------------- */
#ifndef NM_NO_THREE_COLUMNS

/*
 * Lay out a single column of `count` tiles starting at windows[off]
 * with x-origin `col_x`, width `col_w`. Each tile gets out->height /
 * count, and the bottom tile absorbs leftover pixels so the column
 * fills out->height exactly.
 */
static void three_columns_layout_col(struct Window **windows, size_t off,
				     s32 count, const struct Output *out,
				     s32 col_x, s32 col_w, s32 inner, s32 border)
{
	s32 row_h;
	s32 i;

	if (count <= 0 || col_w <= 0)
		return;
	row_h = out->height / count;
	for (i = 0; i < count; i++) {
		s32 y = out->y + i * row_h;
		s32 h = (i == count - 1) ? (out->height - i * row_h) : row_h;
		window_apply_cell(windows[off + (size_t)i], col_x, y, col_w, h,
				  inner, border);
	}
}

/*
 * Three Columns: master column in the center at `ratio` of the
 * output's width, slaves split symmetrically into right (ceil) and
 * left (floor) columns at (1 - ratio) / 2 each. With nstack <= 1
 * the third column would be empty, so the layout falls back to a
 * two-column master + right shape (Tall-on-the-right). Stateless.
 *
 * Window order: windows[0..nmaster-1] -> master column,
 * windows[nmaster..nmaster+n_right-1] -> right column,
 * windows[nmaster+n_right..] -> left column. Slaves go right-first
 * to mirror reading direction (the eye lands on the master, then
 * on the column closest to the master's trailing edge).
 */
static void layout_three_columns_apply(struct Layout *l,
				       const struct Output *out,
				       struct Window **windows, size_t n,
				       struct Window *focused)
{
	size_t nstack;
	s32 master_w;
	s32 left_w;
	s32 right_w;
	s32 master_x;
	s32 nmaster;
	s32 n_right;
	s32 n_left;
	s32 border;
	s32 inner;

	(void)focused; /* stateless */
	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	nmaster = l->u.tall.nmaster;
	if (nmaster < 0)
		nmaster = 0;
	if ((size_t)nmaster > n)
		nmaster = (s32)n;
	nstack = n - (size_t)nmaster;

	inner = l->inner_gap;
	border = l->border;

	/*
	 * No slaves: master column gets the whole output width. Matches
	 * Tall's behavior with nstack == 0.
	 */
	if (nstack == 0) {
		three_columns_layout_col(windows, 0, nmaster, out, out->x,
					 out->width, inner, border);
		return;
	}

	if (nmaster == 0 && nstack == 1) {
		three_columns_layout_col(windows, 0, 1, out, out->x, out->width,
					 inner, border);
		return;
	}
	if (nmaster == 0) {
		/*
		 * Degenerate: no master, all windows are slaves. Treat the
		 * whole output as the slave area split into two columns
		 * (right ceil, left floor) so we still produce a sensible
		 * three-columns-shaped result.
		 */
		master_w = 0;
		left_w = out->width / 2;
		right_w = out->width - left_w;
	} else if (nstack == 1) {
		/*
		 * Two columns only (master + right). Slave width matches
		 * Tall's stack column so the layout degrades gracefully on
		 * low window counts.
		 */
		master_w = (s32)((float)out->width * l->u.tall.ratio);
		right_w = out->width - master_w;
		left_w = 0;
	} else {
		master_w = (s32)((float)out->width * l->u.tall.ratio);
		left_w = (out->width - master_w) / 2;
		right_w = out->width - master_w - left_w;
	}

	master_x = out->x + left_w;
	n_right = (s32)((nstack + 1) / 2);
	n_left = (s32)(nstack - (size_t)n_right);

	if (nmaster > 0)
		three_columns_layout_col(windows, 0, nmaster, out, master_x,
					 master_w, inner, border);
	three_columns_layout_col(windows, (size_t)nmaster, n_right, out,
				 master_x + master_w, right_w, inner, border);
	three_columns_layout_col(windows, (size_t)nmaster + (size_t)n_right,
				 n_left, out, out->x, left_w, inner, border);
}

void layout_init_three_columns(struct Layout *l, s32 nmaster, float ratio)
{
	l->kind = LAYOUT_THREE_COLUMNS;
	l->u.tall.nmaster = nmaster;
	l->u.tall.ratio = ratio;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = false;
	l->apply = layout_three_columns_apply;
	l->destroy = NULL;
	l->do_action = NULL;
	l->set_anchor = NULL;
	l->drop_window = NULL;
	l->restack = NULL;
}

#endif /* NM_NO_THREE_COLUMNS */

/* ---- Grid ------------------------------------------------------------- */
#ifndef NM_NO_GRID

/*
 * Smallest c such that c * c >= n. Cheaper than pulling in <math.h>
 * for ceilf(sqrtf()) on values that never exceed a workspace's tile
 * count - the loop runs ceil(sqrt(n)) iterations, well under 100 for
 * any plausible session.
 */
static s32 grid_cols_for(size_t n)
{
	s32 c = 1;

	while ((size_t)c * (size_t)c < n)
		c++;
	return c;
}

/*
 * Grid: cols = ceil(sqrt(n)) via grid_cols_for; rows = ceil(n/cols).
 * Tiles fill row-major. The last row may be partial; in that case
 * its tiles widen to fill the full output width rather than leave
 * trailing empty space (matches xmonad's Grid). Last cell in each
 * row absorbs leftover pixels from integer division, same as the
 * tall-family layouts.
 */
static void layout_grid_apply(struct Layout *l, const struct Output *out,
			      struct Window **windows, size_t n,
			      struct Window *focused)
{
	s32 inner;
	s32 border;
	s32 row_h;
	s32 cols;
	s32 rows;
	s32 r;

	(void)focused; /* stateless */
	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	cols = grid_cols_for(n);
	rows = (s32)((n + (size_t)cols - 1) / (size_t)cols);
	inner = l->inner_gap;
	border = l->border;
	row_h = out->height / rows;

	for (r = 0; r < rows; r++) {
		size_t row_start = (size_t)r * (size_t)cols;
		size_t row_end = row_start + (size_t)cols;
		s32 count_in_row;
		s32 col_w;
		s32 y;
		s32 h;
		s32 cc;

		if (row_end > n)
			row_end = n;
		count_in_row = (s32)(row_end - row_start);
		col_w = out->width / count_in_row;
		y = out->y + r * row_h;
		h = (r == rows - 1) ? (out->height - r * row_h) : row_h;
		for (cc = 0; cc < count_in_row; cc++) {
			s32 x = out->x + cc * col_w;
			s32 w = (cc == count_in_row - 1) ?
					(out->width - cc * col_w) :
					col_w;
			window_apply_cell(windows[row_start + (size_t)cc], x, y,
					  w, h, inner, border);
		}
	}
}

void layout_init_grid(struct Layout *l)
{
	l->kind = LAYOUT_GRID;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = false;
	l->apply = layout_grid_apply;
	l->destroy = NULL;
	l->do_action = NULL;
	l->set_anchor = NULL;
	l->drop_window = NULL;
	l->restack = NULL;
}

#endif /* NM_NO_GRID */

/* ---- Deck / Tabbed shared apply ---------------------------------- */
#if !defined(NM_NO_DECK) || !defined(NM_NO_TABBED)

/*
 * Deck (monocle / Full): the focused tile-eligible window gets the
 * whole inset-adjusted area; every other tile-eligible window is
 * hidden via visible_desired so the render-side hide / show path
 * keeps it off-screen. Focus-cycle to bring a different window to
 * the top.
 *
 * Tiebreaker when no seat focus is on this workspace: pick
 * windows[0] (the workspace's list head). Mirrors how Tall
 * defaults its master cell when nothing is explicitly focused.
 *
 * Notes:
 *  - update_visibility runs before apply_layout each cycle and
 *    seeds w->visible_desired = (workspace shown). We override
 *    that here for non-top tiles. Hide and show go through the
 *    render phase, which makes this safe: render
 *    walks every window, not just tile candidates, and the
 *    river_window_v1.show / hide requests are idempotent.
 *  - Floating / ignored / fullscreen windows never reach the
 *    apply call (filtered in apply_layout's tile-candidate
 *    collector), so their visibility stays driven by other
 *    rules.
 */
static void layout_focused_only_apply(struct Layout *l,
				      const struct Output *out,
				      struct Window **windows, size_t n,
				      struct Window *focused)
{
	struct Window *top;
	s32 inner;
	s32 border;
	size_t i;

	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	top = NULL;
	if (focused != NULL) {
		for (i = 0; i < n; i++) {
			if (windows[i] == focused) {
				top = focused;
				break;
			}
		}
	}
	if (top == NULL)
		top = windows[0];

	inner = l->inner_gap;
	border = l->border;
	for (i = 0; i < n; i++) {
		if (windows[i] == top)
			continue;
		windows[i]->visible_desired = false;
	}
	window_apply_cell(top, out->x, out->y, out->width, out->height,
			  inner, border);
}

#endif

/* ---- Deck ------------------------------------------------------------- */
#ifndef NM_NO_DECK

void layout_init_deck(struct Layout *l)
{
	l->kind = LAYOUT_DECK;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = true;
	l->apply = layout_focused_only_apply;
	l->destroy = NULL;
	l->do_action = NULL;
	l->set_anchor = NULL;
	l->drop_window = NULL;
	l->restack = NULL;
}

#endif /* NM_NO_DECK */

/* ---- Tabbed ----------------------------------------------------------- */
#ifndef NM_NO_TABBED

/*
 * Tabbed: visually identical to Deck for v1. xmonad's
 * XMonad.Layout.Tabbed renders a tab bar above the content with one
 * tab per window, focused tab highlighted. The river protocol does
 * not expose a "draw a bar" surface to nomonad without pulling in
 * river_layer_shell_v1 plus a renderer (cairo / pixman), which is a
 * substantial dependency add. v1 ships the headless variant: the
 * focused window covers the whole inset-adjusted rect, every other
 * tile-eligible window is hidden via visible_desired. Users who want
 * tab visuals already get focused-window + workspace from the status
 * broadcast (src/status.c) and can render tabs in waybar / yambar.
 *
 * Same focus_changes_visibility = true semantics as Deck: focus
 * cycling within a Tabbed workspace re-runs apply_layout.
 */
void layout_init_tabbed(struct Layout *l)
{
	l->kind = LAYOUT_TABBED;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = true;
	l->apply = layout_focused_only_apply;
	l->destroy = NULL;
	l->do_action = NULL;
	l->set_anchor = NULL;
	l->drop_window = NULL;
	l->restack = NULL;
}

#endif /* NM_NO_TABBED */

/* ---- Spiral ----------------------------------------------------------- */
#ifndef NM_NO_SPIRAL

/*
 * Spiral / Dwindle: walk the window list 0..n-1. Each window except
 * the last takes a fixed-ratio (0.5) split of the current cell on
 * the alternating axis (V then H then V then ...) and the remaining
 * portion becomes the cell for the next iteration. The last window
 * receives whatever's left, so total area is preserved exactly. The
 * spiral grows into the bottom-right corner: a tile at index i has
 * area (output_area / 2^i) for i < n-1, and the final tile inherits
 * its predecessor's full remaining cell.
 *
 * Stateless. No params today (v1 hardcodes ratio = 0.5; xmonad's
 * Dwindle also uses 0.5 by default and the golden-ratio variant
 * lives behind a separate constructor). Adding a per-layout ratio
 * field would slot into the union arm cleanly when the use case
 * appears.
 */
static void layout_spiral_apply(struct Layout *l, const struct Output *out,
				struct Window **windows, size_t n,
				struct Window *focused)
{
	bool axis_vert;
	s32 cell_x;
	s32 cell_y;
	s32 cell_w;
	s32 cell_h;
	s32 inner;
	s32 border;
	size_t i;

	(void)focused; /* stateless */
	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	cell_x = out->x;
	cell_y = out->y;
	cell_w = out->width;
	cell_h = out->height;
	inner = l->inner_gap;
	border = l->border;
	axis_vert = true;

	for (i = 0; i < n - 1; i++) {
		s32 take_w = cell_w;
		s32 take_h = cell_h;

		if (axis_vert) {
			take_w = cell_w / 2;
			window_apply_cell(windows[i], cell_x, cell_y, take_w,
					  cell_h, inner, border);
			cell_x += take_w;
			cell_w -= take_w;
		} else {
			take_h = cell_h / 2;
			window_apply_cell(windows[i], cell_x, cell_y, cell_w,
					  take_h, inner, border);
			cell_y += take_h;
			cell_h -= take_h;
		}
		axis_vert = !axis_vert;
	}
	window_apply_cell(windows[n - 1], cell_x, cell_y, cell_w, cell_h, inner,
			  border);
}

void layout_init_spiral(struct Layout *l)
{
	l->kind = LAYOUT_SPIRAL;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = false;
	l->apply = layout_spiral_apply;
	l->destroy = NULL;
	l->do_action = NULL;
	l->set_anchor = NULL;
	l->drop_window = NULL;
	l->restack = NULL;
}

#endif /* NM_NO_SPIRAL */

/* ---- TwoPane ---------------------------------------------------------- */
#ifndef NM_NO_TWO_PANE

/*
 * TwoPane: master tile on the left at u.tall.ratio of the output's
 * width; the focused non-master tile claims the right pane; every
 * other non-master tile hides via visible_desired. focus_changes_
 * visibility = true so a focus-next press inside the workspace
 * re-runs apply_layout to swap the visible right tile.
 *
 * Tiebreaker when focus does not pick a non-master:
 *  - focused is the master itself: pick windows[1] (first non-master
 *    in the workspace's head->tail order).
 *  - focus is NULL or on another workspace: same fallback.
 *  - n == 1: master takes the full output, right pane is unused.
 *
 * Stateless. Shares u.tall with TALL so SHRINK_MASTER / EXPAND_MASTER
 * adjust the split ratio via the existing seat-side dispatch.
 * INC_MASTER / DEC_MASTER are accepted by the kind guard but the
 * apply function ignores nmaster - TwoPane is fixed at one master
 * by definition. The harmless no-op keeps the master action vocabulary
 * uniform across the master-family layouts.
 */
static void layout_two_pane_apply(struct Layout *l, const struct Output *out,
				  struct Window **windows, size_t n,
				  struct Window *focused)
{
	struct Window *right;
	s32 master_w;
	s32 right_w;
	s32 inner;
	s32 border;
	size_t i;

	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	inner = l->inner_gap;
	border = l->border;

	if (n == 1) {
		window_apply_cell(windows[0], out->x, out->y, out->width,
				  out->height, inner, border);
		return;
	}

	master_w = (s32)((float)out->width * l->u.tall.ratio);
	right_w = out->width - master_w;
	right = NULL;
	if (focused != NULL && focused != windows[0]) {
		for (i = 1; i < n; i++) {
			if (windows[i] == focused) {
				right = focused;
				break;
			}
		}
	}
	if (right == NULL)
		right = windows[1];

	for (i = 1; i < n; i++) {
		if (windows[i] == right)
			continue;
		windows[i]->visible_desired = false;
	}
	window_apply_cell(windows[0], out->x, out->y, master_w, out->height,
			  inner, border);
	window_apply_cell(right, out->x + master_w, out->y, right_w,
			  out->height, inner, border);
}

void layout_init_two_pane(struct Layout *l, float ratio)
{
	l->kind = LAYOUT_TWO_PANE;
	l->u.tall.nmaster = 1; /* TwoPane is fixed at one master. */
	l->u.tall.ratio = ratio;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = true;
	l->apply = layout_two_pane_apply;
	l->destroy = NULL;
	l->do_action = NULL;
	l->set_anchor = NULL;
	l->drop_window = NULL;
	l->restack = NULL;
}

#endif /* NM_NO_TWO_PANE */

/* ---- ResizableTall ---------------------------------------------------- */
#ifndef NM_NO_RESIZABLE_TALL

/*
 * Per-window height delta in pixels. Stored as a flat array keyed
 * on Window pointer; lookups are linear because resized cells are
 * rare (a typical session resizes 0-3 cells out of N) and the
 * array stays small. Linear scan beats a hashtable on this size
 * by a wide margin and keeps the code dense.
 *
 * Invariant maintained by the verb dispatch: any expand on cell i
 * is paired with an equal-magnitude shrink on the adjacent cell in
 * the same column, so the sum of deltas inside a column is zero
 * and the column total stays equal to the column's available
 * height. Apply does not enforce that invariant directly; if a
 * pairing partner is missing (e.g. focused tile is alone in its
 * column) the verb is a no-op so the imbalance never gets created.
 */
struct LayoutResizeDeltaEntry {
	struct Window *w;
	s32 height_delta;
};

struct LayoutResizeDeltas {
	struct LayoutResizeDeltaEntry *items;
	size_t n;
	size_t cap;
};

#define NM_RESIZE_CELL_STEP 20
#define NM_RESIZE_MIN_CONTENT 16

static struct LayoutResizeDeltas *resize_deltas_new(void)
{
	return calloc(1, sizeof(struct LayoutResizeDeltas));
}

static void resize_deltas_free(struct LayoutResizeDeltas *d)
{
	if (d == NULL)
		return;
	free(d->items);
	free(d);
}

static s32 resize_deltas_get(const struct LayoutResizeDeltas *d,
				 const struct Window *w)
{
	size_t i;

	if (d == NULL || w == NULL)
		return 0;
	for (i = 0; i < d->n; i++) {
		if (d->items[i].w == w)
			return d->items[i].height_delta;
	}
	return 0;
}

/*
 * Adjust w's delta by `step`. Creates an entry if one does not
 * exist; removes the entry when the running delta returns to zero
 * to keep the array small under churn.
 */
static void resize_deltas_bump(struct LayoutResizeDeltas *d, struct Window *w,
			       s32 step)
{
	size_t i;
	struct LayoutResizeDeltaEntry *grown;
	size_t newcap;

	if (d == NULL || w == NULL)
		return;
	for (i = 0; i < d->n; i++) {
		if (d->items[i].w != w)
			continue;
		d->items[i].height_delta += step;
		if (d->items[i].height_delta == 0) {
			d->items[i] = d->items[d->n - 1];
			d->n--;
		}
		return;
	}
	if (step == 0)
		return;
	if (d->n >= d->cap) {
		newcap = d->cap == 0 ? 4 : d->cap * 2;
		grown = realloc(d->items, newcap * sizeof(*grown));
		if (grown == NULL)
			return;
		d->items = grown;
		d->cap = newcap;
	}
	d->items[d->n].w = w;
	d->items[d->n].height_delta = step;
	d->n++;
}

static void resize_deltas_remove(struct LayoutResizeDeltas *d,
				 const struct Window *w)
{
	size_t i;

	if (d == NULL || w == NULL)
		return;
	for (i = 0; i < d->n; i++) {
		if (d->items[i].w != w)
			continue;
		d->items[i] = d->items[d->n - 1];
		d->n--;
		return;
	}
}

/*
 * Apply: TALL's master/stack column shape with each cell's height
 * adjusted by its (window-keyed) delta. Total column height is
 * preserved when the deltas sum to zero in a column (the verb
 * dispatch enforces that for every press); the final cell in each
 * column always absorbs whatever's left from integer division so
 * total column height matches `out->height` exactly.
 */
static s32 resizable_tall_base(s32 col_h, size_t count, size_t i)
{
	s32 row_h = col_h / (s32)count;

	return row_h + ((i == count - 1) ? col_h - row_h * (s32)count : 0);
}

static void resizable_tall_column(struct Layout *l, struct Window **windows,
				  size_t count, const struct Output *out, s32 x,
				  s32 width, s32 inner, s32 border)
{
	s32 col_h = out->height;
	s64 total = 0;
	s64 cum = 0;
	s32 used = 0;
	s32 min_h;
	size_t i;

	min_h = 2 * (inner + border) + NM_RESIZE_MIN_CONTENT;
	if (min_h > col_h / (s32)count)
		min_h = col_h / (s32)count;
	if (min_h < 1)
		min_h = 1;
	for (i = 0; i < count; i++) {
		s32 h = resizable_tall_base(col_h, count, i) +
			resize_deltas_get(l->resize_deltas, windows[i]);

		total += (h < min_h) ? min_h : h;
	}
	for (i = 0; i < count; i++) {
		struct Window *w = windows[i];
		s32 base = resizable_tall_base(col_h, count, i);
		s32 want = base + resize_deltas_get(l->resize_deltas, w);
		s32 next;
		s32 h;

		cum += (want < min_h) ? min_h : want;
		next = (i == count - 1) ? col_h : (s32)(cum * col_h / total);
		h = next - used;
		window_apply_cell(w, x, out->y + used, width, h, inner, border);
		resize_deltas_bump(
			l->resize_deltas, w,
			(h - base) - resize_deltas_get(l->resize_deltas, w));
		used = next;
	}
}

static void layout_resizable_tall_apply(struct Layout *l,
					const struct Output *out,
					struct Window **windows, size_t n,
					struct Window *focused)
{
	size_t nstack;
	s32 master_w;
	s32 stack_w;
	s32 nmaster;
	s32 border;
	s32 inner;

	(void)focused;
	if (n == 0)
		return;
	if (out->width <= 0 || out->height <= 0)
		return;

	nmaster = l->u.tall.nmaster;
	if (nmaster < 0)
		nmaster = 0;
	if ((size_t)nmaster > n)
		nmaster = (s32)n;
	nstack = n - (size_t)nmaster;

	if (nmaster == 0)
		master_w = 0;
	else if (nstack == 0)
		master_w = out->width;
	else
		master_w = (s32)((float)out->width * l->u.tall.ratio);
	stack_w = out->width - master_w;
	inner = l->inner_gap;
	border = l->border;

	if (nmaster > 0)
		resizable_tall_column(l, windows, (size_t)nmaster, out, out->x,
				      master_w, inner, border);
	if (nstack > 0)
		resizable_tall_column(l, windows + nmaster, nstack, out,
				      out->x + master_w, stack_w, inner,
				      border);
}

/*
 * Walk the workspace's Layout-visible window order to find the
 * focused window's column-mate: the cell directly below in the
 * same column (master or stack), wrapping to the cell above when
 * focused is the last in its column. NULL if focused is not in
 * the list, has no neighbour in its column (alone in master or
 * stack), or is not actually present. The caller is responsible
 * for ensuring `focused` is one of the workspace's tile-eligible
 * windows; ResizableTall verbs never act on hidden / floating /
 * fullscreen targets so the lookup is robust against typos.
 */
static struct Window *resize_pair_partner(struct Layout *l,
					  const struct Workspace *ws,
					  struct Window *focused)
{
	struct Window **arr = ws->layout_arr;
	size_t n = ws->layout_arr_len;
	size_t idx;
	s32 nmaster;
	size_t col_start;
	size_t col_end;

	if (focused == NULL || arr == NULL || n == 0)
		return NULL;
	idx = SIZE_MAX;
	for (size_t i = 0; i < n; i++) {
		if (arr[i] == focused) {
			idx = i;
			break;
		}
	}
	if (idx == SIZE_MAX)
		return NULL;

	nmaster = l->u.tall.nmaster;
	if (nmaster < 0)
		nmaster = 0;
	if ((size_t)nmaster > n)
		nmaster = (s32)n;
	if (idx < (size_t)nmaster) {
		col_start = 0;
		col_end = (size_t)nmaster;
	} else {
		col_start = (size_t)nmaster;
		col_end = n;
	}
	if (col_end - col_start < 2)
		return NULL;
	if (idx + 1 < col_end)
		return arr[idx + 1];
	return arr[idx - 1];
}

/*
 * Verb dispatch. expand-cell adds NM_RESIZE_CELL_STEP px to the
 * focused cell and removes the same from its column-mate; shrink
 * does the inverse. No-op if focused is alone in its column or
 * the focused window is not in the layout's tile-eligible set
 * (e.g. focus is on a floating window).
 */
static void layout_resizable_tall_do_action(struct Layout *l,
					    struct Window *focused,
					    const char *verb)
{
	struct Window *partner;
	s32 step;

	if (l == NULL || focused == NULL || verb == NULL)
		return;
	if (strcmp(verb, "expand-cell") == 0)
		step = NM_RESIZE_CELL_STEP;
	else if (strcmp(verb, "shrink-cell") == 0)
		step = -NM_RESIZE_CELL_STEP;
	else
		return;

	{
		const struct Workspace *ws =
			(focused->workspace_index >= 0 &&
			 (size_t)focused->workspace_index < wm.n_workspaces) ?
				&wm.workspaces[focused->workspace_index] :
				NULL;
		if (ws == NULL)
			return;
		partner = resize_pair_partner(l, ws, focused);
	}
	if (partner == NULL)
		return;
	resize_deltas_bump(l->resize_deltas, focused, step);
	resize_deltas_bump(l->resize_deltas, partner, -step);
}

static void layout_resizable_tall_destroy(struct Layout *l)
{
	if (l == NULL)
		return;
	resize_deltas_free(l->resize_deltas);
	l->resize_deltas = NULL;
}

/*
 * Drop the window's delta entry. Surviving deltas no longer need to
 * sum to zero: resizable_tall_column rescales each column to fit.
 */
static void layout_resizable_tall_drop_window(struct Layout *l,
					      struct Window *w)
{
	if (l == NULL || l->resize_deltas == NULL)
		return;
	resize_deltas_remove(l->resize_deltas, w);
}

void layout_init_resizable_tall(struct Layout *l, s32 nmaster, float ratio)
{
	l->kind = LAYOUT_RESIZABLE_TALL;
	l->u.tall.nmaster = nmaster;
	l->u.tall.ratio = ratio;
	l->inner_gap = 0;
	l->border = 0;
	l->focus_changes_visibility = false;
	l->resize_deltas = resize_deltas_new();
	l->apply = layout_resizable_tall_apply;
	l->destroy = layout_resizable_tall_destroy;
	l->do_action = layout_resizable_tall_do_action;
	l->set_anchor = NULL;
	l->drop_window = layout_resizable_tall_drop_window;
	l->restack = NULL;
}

#endif /* NM_NO_RESIZABLE_TALL */

/* ---- BSP -------------------------------------------------------------- */
#ifndef NM_NO_BSP

enum BspKind {
	BSP_LEAF,
	BSP_SPLIT,
};

enum BspAxis {
	BSP_AXIS_HORIZ, /* split into top/bottom children */
	BSP_AXIS_VERT, /* split into left/right children */
};

/*
 * Each node caches its last-applied rect so a future insert can pick a
 * sensible split axis (perpendicular to the leaf's longer dimension)
 * without re-walking the tree.
 */
struct BspNode {
	enum BspKind kind;
	s32 x, y, w, h;
	union {
		struct Window *leaf;
		struct {
			enum BspAxis axis;
			float ratio;
			struct BspNode *a, *b;
		} split;
	} u;
};

/*
 * Open-addressing hashtable keyed by Window pointer. Linear probing,
 * power-of-two cap, grown when load factor would exceed 50%. NULL
 * slots are empty; backshift-on-remove keeps probe chains intact
 * without needing tombstones. One instance per LayoutBspState tracks
 * which windows are currently leaves in the tree, replacing the
 * recursive bsp_contains walk with O(1) bucket lookups. The
 * complementary per-cycle wanted-set check lives on Window itself
 * via bsp_want_gen (see nomonad.h) so the mark phase is a field
 * write rather than a hashtable insert.
 */
struct BspWindowSet {
	struct Window **slots;
	size_t cap;
	size_t n;
};

struct LayoutBspState {
	struct BspNode *root;
	/*
	 * The leaf containing this window is the next split target. Lets
	 * fresh windows pile on next to the most recently inserted one,
	 * which is the closest match to "split focused" without the layout
	 * having to know about the seat.
	 */
	struct Window *last_inserted;
	/*
	 * Axis used by the most recent split. Subsequent splits alternate
	 * from it instead of recomputing axis from the leaf's cached
	 * rect. Without alternation, once the chain reaches a degenerate
	 * cell (rect 0x0 or 1x1) the rect-based decision (n->w >= n->h
	 * picks V on ties / on 0>=0) cascades into an all-V comb that is
	 * invisible while small but expands into many narrow strips when
	 * ancestors collapse during a close session.
	 *
	 * has_last_axis is false until the first split commits; the very
	 * first split picks axis from rect (so a wide screen splits
	 * left/right first, matching xmonad's typical first-cell choice).
	 */
	bool has_last_axis;
	enum BspAxis last_axis;
	/*
	 * Tree membership: w is a leaf in this->root iff the set contains w.
	 * Maintained inline by bsp_make_leaf (add) and bsp_drop_unwanted
	 * (remove on free); cleared wholesale on bsp_insert reset and
	 * layout_bsp_destroy. bsp_contains is now a single bucket lookup.
	 */
	struct BspWindowSet members;
	/*
	 * Per-apply context: bsp_split_at and bsp_insert push fresh rects
	 * for newly-created leaves directly via window_apply_cell, so the
	 * post-insert tree walk in layout_bsp_apply is unnecessary. These
	 * fields are seeded at the top of layout_bsp_apply and read by the
	 * insert path; they are stale outside an in-progress apply call.
	 */
	s32 apply_x, apply_y, apply_w, apply_h;
	s32 apply_inner, apply_border;
};

/*
 * Set NM_BSP_DEBUG=1 to dump the BSP tree on every apply. Goes to
 * stderr; cheap on idle (returns false fast). Exists to track down
 * "BSP layout converges to a vertical comb" bugs by making the
 * insert/drop sequence and the chosen split axes inspectable.
 */
static bool bsp_debug_enabled(void)
{
	static int v = -1;

	if (v == -1) {
		const char *s = getenv("NM_BSP_DEBUG");
		v = (s != NULL && s[0] != '\0' && s[0] != '0') ? 1 : 0;
	}
	return v == 1;
}

static void bsp_dump_node(const struct BspNode *n, int depth)
{
	for (int i = 0; i < depth; i++)
		fputs("  ", stderr);
	if (n == NULL) {
		fputs("(null)\n", stderr);
		return;
	}
	if (n->kind == BSP_LEAF) {
		fprintf(stderr, "leaf w=%p rect=[%d,%d %dx%d]\n",
			(void *)n->u.leaf, n->x, n->y, n->w, n->h);
	} else {
		fprintf(stderr, "split %s r=%.2f rect=[%d,%d %dx%d]\n",
			n->u.split.axis == BSP_AXIS_VERT ? "V" : "H",
			(double)n->u.split.ratio, n->x, n->y, n->w, n->h);
		bsp_dump_node(n->u.split.a, depth + 1);
		bsp_dump_node(n->u.split.b, depth + 1);
	}
}

static void bsp_dump(const char *tag, const struct LayoutBspState *st)
{
	fprintf(stderr, "[bsp] %s: root=\n", tag);
	bsp_dump_node(st->root, 1);
}

/*
 * BspWindowSet: open-addressing hash set keyed by Window pointer.
 *
 * Hash mixes the pointer with a 64-bit Knuth multiplier to redistribute
 * the malloc-aligned low bits before masking. Linear probing keeps the
 * code dense and cache-friendly; remove uses the standard backshift
 * algorithm so the probe chain stays consistent without tombstones.
 *
 * cap is always a power of two (or 0 before first reserve). Grown when
 * adding the next entry would exceed 50% load. Reserve / clear use
 * memset rather than tracking individual slots so a heavy churn cycle
 * (close all windows, spawn N more) does not accumulate cost.
 */

static size_t bsp_set_hash(struct Window *w, size_t cap)
{
	u64 k = (u64)(uintptr_t)w;
	k *= 0x9E3779B97F4A7C15ULL;
	return (size_t)(k & (cap - 1));
}

static bool bsp_set_reserve(struct BspWindowSet *s, size_t need)
{
	struct Window **fresh;
	size_t newcap;
	size_t i;

	if (s->cap >= 2 * need + 1)
		return true;
	newcap = s->cap ? s->cap : 8;
	while (newcap <= 2 * need)
		newcap *= 2;
	fresh = calloc(newcap, sizeof(*fresh));
	if (fresh == NULL)
		return false;
	for (i = 0; i < s->cap; i++) {
		struct Window *w;
		size_t b;

		w = s->slots[i];
		if (w == NULL)
			continue;
		b = bsp_set_hash(w, newcap);
		while (fresh[b] != NULL)
			b = (b + 1) & (newcap - 1);
		fresh[b] = w;
	}
	free(s->slots);
	s->slots = fresh;
	s->cap = newcap;
	return true;
}

static void bsp_set_clear(struct BspWindowSet *s)
{
	if (s->slots != NULL && s->cap > 0)
		memset(s->slots, 0, s->cap * sizeof(*s->slots));
	s->n = 0;
}

static void bsp_set_destroy(struct BspWindowSet *s)
{
	free(s->slots);
	s->slots = NULL;
	s->cap = 0;
	s->n = 0;
}

static bool bsp_set_contains(const struct BspWindowSet *s, struct Window *w)
{
	size_t mask;
	size_t b;

	if (w == NULL || s->cap == 0)
		return false;
	mask = s->cap - 1;
	b = bsp_set_hash(w, s->cap);
	while (s->slots[b] != NULL) {
		if (s->slots[b] == w)
			return true;
		b = (b + 1) & mask;
	}
	return false;
}

static bool bsp_set_add(struct BspWindowSet *s, struct Window *w)
{
	size_t mask;
	size_t b;

	if (w == NULL)
		return true;
	if (!bsp_set_reserve(s, s->n + 1))
		return false;
	mask = s->cap - 1;
	b = bsp_set_hash(w, s->cap);
	while (s->slots[b] != NULL) {
		if (s->slots[b] == w)
			return true; /* idempotent on duplicate add */
		b = (b + 1) & mask;
	}
	s->slots[b] = w;
	s->n++;
	return true;
}

/*
 * Returns true iff target lies in the cyclic interval [from, to). When
 * from == to the interval is empty. Used by bsp_set_remove's backshift
 * walk to decide whether a probe-chain entry can be moved into the
 * vacated bucket without breaking anyone else's lookup chain.
 */
static bool bsp_set_cyclic_in(size_t target, size_t from, size_t to)
{
	if (from == to)
		return false;
	if (from < to)
		return target >= from && target < to;
	return target >= from || target < to;
}

static void bsp_set_remove(struct BspWindowSet *s, struct Window *w)
{
	size_t mask;
	size_t hole;
	size_t i;

	if (w == NULL || s->cap == 0)
		return;
	mask = s->cap - 1;
	hole = bsp_set_hash(w, s->cap);
	while (s->slots[hole] != NULL && s->slots[hole] != w)
		hole = (hole + 1) & mask;
	if (s->slots[hole] != w)
		return;
	i = (hole + 1) & mask;
	while (s->slots[i] != NULL) {
		size_t home;

		home = bsp_set_hash(s->slots[i], s->cap);
		if (bsp_set_cyclic_in(hole, home, i)) {
			s->slots[hole] = s->slots[i];
			hole = i;
		}
		i = (i + 1) & mask;
	}
	s->slots[hole] = NULL;
	s->n--;
}

static struct BspNode *bsp_make_leaf(struct LayoutBspState *st,
				     struct Window *w)
{
	struct BspNode *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		return NULL;
	n->kind = BSP_LEAF;
	n->u.leaf = w;
	if (!bsp_set_add(&st->members, w)) {
		free(n);
		return NULL;
	}
	return n;
}

static void bsp_free(struct BspNode *n)
{
	if (n == NULL)
		return;
	if (n->kind == BSP_SPLIT) {
		bsp_free(n->u.split.a);
		bsp_free(n->u.split.b);
	}
	free(n);
}

/* Returns true if the tree rooted at st contains a leaf for w. */
static bool bsp_contains(const struct LayoutBspState *st,
			 const struct Window *w)
{
	return bsp_set_contains(&st->members, (struct Window *)w);
}

/*
 * Replace the leaf containing `target` with a split of (existing leaf,
 * new leaf for w). Returns the new subtree (or unchanged subtree if
 * target was not in this subtree).
 */
static struct BspNode *bsp_split_at(struct BspNode *n, struct Window *target,
				    struct Window *w,
				    struct LayoutBspState *state)
{
	if (n == NULL)
		return NULL;
	if (n->kind == BSP_LEAF) {
		struct BspNode *new_leaf;
		struct BspNode *split;

		if (n->u.leaf != target)
			return n;
		new_leaf = bsp_make_leaf(state, w);
		if (new_leaf == NULL)
			return n;
		split = calloc(1, sizeof(*split));
		if (split == NULL) {
			bsp_set_remove(&state->members, w);
			free(new_leaf);
			return n;
		}
		split->kind = BSP_SPLIT;
		split->x = n->x;
		split->y = n->y;
		split->w = n->w;
		split->h = n->h;
		/*
		 * Rect-based: split perpendicular to the longer dimension,
		 * matching xmonad's BSP. Degenerate rects (a leaf inserted
		 * before its first bsp_apply_node has populated w/h) would
		 * tie-break n->w >= n->h on 0>=0 and lock onto V forever,
		 * producing a vertical comb that expands ugly when ancestors
		 * collapse on close. The last-axis fallback handles only that
		 * degenerate case; sane rects always get the rect-based pick
		 * so e.g. spawn / close / spawn on a stable cell repeatedly
		 * picks the same axis instead of alternating.
		 */
		if (n->w >= 2 && n->h >= 2)
			split->u.split.axis = (n->w >= n->h) ?
						      BSP_AXIS_VERT :
						      BSP_AXIS_HORIZ;
		else if (state->has_last_axis)
			split->u.split.axis =
				(state->last_axis == BSP_AXIS_VERT) ?
					BSP_AXIS_HORIZ :
					BSP_AXIS_VERT;
		else
			split->u.split.axis = BSP_AXIS_VERT;
		state->last_axis = split->u.split.axis;
		state->has_last_axis = true;
		split->u.split.ratio = 0.5f;
		/*
		 * New window goes on side `a` (top for H, left for V) so
		 * the tree grows toward the top-left corner, matching
		 * xmonad's BSP. bsp_apply_node maps `a` to the smaller
		 * x/y origin, so this is purely a placement choice.
		 */
		split->u.split.a = new_leaf;
		split->u.split.b = n;
		if (bsp_debug_enabled()) {
			fprintf(stderr,
				"[bsp]   split target=%p new=%p axis=%s "
				"on rect=[%dx%d]\n",
				(void *)target, (void *)w,
				split->u.split.axis == BSP_AXIS_VERT ? "V" :
								      "H",
				n->w, n->h);
		}
		/*
		 * Set child rects from the just-decided split. Both halves
		 * are also pushed to the protocol via window_apply_cell so
		 * the layout_bsp_apply caller does not need a second tree
		 * walk to refresh leaves whose rect just changed: the only
		 * leaves whose rect changed are the just-split pair.
		 *
		 * With new_leaf on side a, the new window takes the
		 * top/left half and the original leaf shifts to the
		 * bottom/right half.
		 */
		if (split->u.split.axis == BSP_AXIS_VERT) {
			s32 left = n->w / 2;
			new_leaf->x = n->x;
			new_leaf->y = n->y;
			new_leaf->w = left;
			new_leaf->h = n->h;
			n->x += left;
			n->w -= left;
		} else {
			s32 top = n->h / 2;
			new_leaf->x = n->x;
			new_leaf->y = n->y;
			new_leaf->w = n->w;
			new_leaf->h = top;
			n->y += top;
			n->h -= top;
		}
		window_apply_cell(new_leaf->u.leaf, new_leaf->x, new_leaf->y,
				  new_leaf->w, new_leaf->h,
				  state->apply_inner, state->apply_border);
		window_apply_cell(n->u.leaf, n->x, n->y, n->w, n->h,
				  state->apply_inner, state->apply_border);
		return split;
	}
	n->u.split.a = bsp_split_at(n->u.split.a, target, w, state);
	n->u.split.b = bsp_split_at(n->u.split.b, target, w, state);
	return n;
}

/* Returns any leaf in the subtree, or NULL if subtree has none. */
static struct Window *bsp_first_leaf(const struct BspNode *n)
{
	struct Window *w;

	if (n == NULL)
		return NULL;
	if (n->kind == BSP_LEAF)
		return n->u.leaf;
	w = bsp_first_leaf(n->u.split.a);
	if (w != NULL)
		return w;
	return bsp_first_leaf(n->u.split.b);
}


/*
 * Pick a leaf to split for the new window. Priority: the seat's
 * currently focused window (so users can position the cursor on the
 * leaf they want subdivided, then spawn). Falls back to last_inserted
 * (so a chain of unfocused spawns piles together) and finally to any
 * leaf in the tree. The chosen target must already be a leaf in the
 * tree and must not be `w` itself (defensive against re-insertions).
 */
static void bsp_insert(struct LayoutBspState *st, struct Window *w,
		       struct Window *focused)
{
	struct Window *target;

	if (st->root == NULL) {
		st->root = bsp_make_leaf(st, w);
		if (st->root == NULL)
			return;
		/*
		 * Seed the root leaf's rect from the layout's available area
		 * so the leaf is positioned without needing a separate tree
		 * walk. Subsequent bsp_split_at calls in this cycle will
		 * subdivide from these dimensions.
		 */
		st->root->x = st->apply_x;
		st->root->y = st->apply_y;
		st->root->w = st->apply_w;
		st->root->h = st->apply_h;
		window_apply_cell(w, st->apply_x, st->apply_y, st->apply_w,
				  st->apply_h, st->apply_inner,
				  st->apply_border);
		st->last_inserted = w;
		return;
	}
	/*
	 * last_inserted is the canonical "next split anchor": each spawn
	 * advances it via this function, and explicit focus changes
	 * (focus-next, click) advance it via the set_anchor vtable hook.
	 * `focused` is a fallback for the case where last_inserted has
	 * been cleared (e.g. the anchor window was just closed).
	 *
	 * Preferring focused first would re-target every spawn at the
	 * seat's last-clicked window during a held A+enter flood: river
	 * does not auto-fire window_interaction for newly-spawned
	 * windows, so seat->focused stays put and every new window
	 * splits the same leaf, halving its rect on each split until it
	 * collapses to 0x0 - the vertical-comb degradation users hit.
	 */
	target = st->last_inserted;
	if (target == w || target == NULL || !bsp_contains(st, target))
		target = focused;
	if (target == w || target == NULL || !bsp_contains(st, target))
		target = bsp_first_leaf(st->root);
	if (target == NULL) {
		bsp_free(st->root);
		bsp_set_clear(&st->members);
		st->root = bsp_make_leaf(st, w);
		if (st->root == NULL)
			return;
		st->root->x = st->apply_x;
		st->root->y = st->apply_y;
		st->root->w = st->apply_w;
		st->root->h = st->apply_h;
		window_apply_cell(w, st->apply_x, st->apply_y, st->apply_w,
				  st->apply_h, st->apply_inner,
				  st->apply_border);
		st->last_inserted = w;
		return;
	}
	st->root = bsp_split_at(st->root, target, w, st);
	st->last_inserted = w;
}

/*
 * Walk the tree at *slot, dropping leaves whose windows are not in
 * the wanted set. Splits collapse to their surviving sibling when one
 * child disappears, or to NULL when both go. last_inserted is cleared
 * if it pointed at a dropped leaf. Each freed leaf is also removed
 * from st->members so the membership hashtable stays in sync with the
 * tree.
 *
 * Wanted-set membership is encoded via Window->bsp_want_gen: the
 * caller bumps a process-wide gen counter, writes it onto every w in
 * want[], and passes it here. A leaf survives iff its window's mark
 * matches; older marks (from previous cycles or a different tree)
 * compare unequal and the leaf gets dropped. No allocation, no
 * hashing, no per-cycle setup beyond the field writes.
 */
static void bsp_drop_unwanted(struct LayoutBspState *st, struct BspNode **slot,
			      u64 want_gen)
{
	struct BspNode *n = *slot;
	if (n == NULL)
		return;
	if (n->kind == BSP_LEAF) {
		if (n->u.leaf != NULL && n->u.leaf->bsp_want_gen == want_gen)
			return;
		if (st->last_inserted == n->u.leaf)
			st->last_inserted = NULL;
		bsp_set_remove(&st->members, n->u.leaf);
		free(n);
		*slot = NULL;
		return;
	}
	bsp_drop_unwanted(st, &n->u.split.a, want_gen);
	bsp_drop_unwanted(st, &n->u.split.b, want_gen);
	if (n->u.split.a == NULL && n->u.split.b == NULL) {
		free(n);
		*slot = NULL;
	} else if (n->u.split.a == NULL) {
		struct BspNode *survivor = n->u.split.b;
		free(n);
		*slot = survivor;
	} else if (n->u.split.b == NULL) {
		struct BspNode *survivor = n->u.split.a;
		free(n);
		*slot = survivor;
	}
}

/*
 * Recursive rect walk. Each split divides its rect along its axis at
 * `ratio`; children inherit the resulting halves. Leaves push a rect to
 * the compositor (after inner-gap and border insets).
 */
static void bsp_apply_node(struct BspNode *n, s32 x, s32 y, s32 w, s32 h,
			   s32 inner, s32 border)
{
	float r;

	n->x = x;
	n->y = y;
	n->w = w;
	n->h = h;
	if (n->kind == BSP_LEAF) {
		window_apply_cell(n->u.leaf, x, y, w, h, inner, border);
		return;
	}
	r = n->u.split.ratio;
	if (r < 0.05f)
		r = 0.05f;
	if (r > 0.95f)
		r = 0.95f;
	if (n->u.split.axis == BSP_AXIS_VERT) {
		s32 left = (s32)((float)w * r);

		bsp_apply_node(n->u.split.a, x, y, left, h, inner, border);
		bsp_apply_node(n->u.split.b, x + left, y, w - left, h, inner,
			       border);
	} else {
		s32 top = (s32)((float)h * r);

		bsp_apply_node(n->u.split.a, x, y, w, top, inner, border);
		bsp_apply_node(n->u.split.b, x, y + top, w, h - top, inner,
			       border);
	}
}

static void layout_bsp_apply(struct Layout *l, const struct Output *out,
			     struct Window **want, size_t n_want,
			     struct Window *focused)
{
	struct LayoutBspState *st = l->u.bsp;
	if (st == NULL)
		return;

	/*
	 * Seed per-apply context for bsp_insert / bsp_split_at. They push
	 * fresh rects via window_apply_cell as new leaves are created, so
	 * the cycle does not need a second tree walk after the insert
	 * loop.
	 */
	st->apply_x = out->x;
	st->apply_y = out->y;
	st->apply_w = out->width;
	st->apply_h = out->height;
	st->apply_inner = l->inner_gap;
	st->apply_border = l->border;

	/*
	 * Reconcile: drop leaves whose windows are no longer wanted, then
	 * insert any wanted window not already present. The drop walk is
	 * in-tree (no scratch buffer); the insert pass prefers the seat's
	 * focused leaf so the user can position the cursor on the tile
	 * they want subdivided, then spawn.
	 */
	if (bsp_debug_enabled()) {
		fprintf(stderr,
			"[bsp] apply: out=[%d,%d %dx%d] n_want=%zu focused=%p\n",
			out->x, out->y, out->width, out->height, n_want,
			(void *)focused);
		for (size_t i = 0; i < n_want; i++)
			fprintf(stderr, "[bsp]   want[%zu]=%p\n", i,
				(void *)want[i]);
		bsp_dump("before drop", st);
	}

	/*
	 * Mark phase: bump a process-wide cycle counter and stamp every
	 * window in want[] with it. bsp_drop_unwanted then survives a
	 * leaf in O(1) by reading w->bsp_want_gen and comparing against
	 * the current cycle's value - no hashtable build, no probing,
	 * just a field write per want[i] and a field read per leaf.
	 *
	 * The counter is static-local so it persists across calls but
	 * cannot wrap in any reachable timeframe (u64 at 1 cycle
	 * per ms wraps in ~584M years). Using a global counter rather
	 * than a per-state one is fine: each apply uses its own current
	 * value to compare leaves against, so a stale BSP tree from
	 * another workspace will see all its leaves' marks compare
	 * unequal and drop them when that workspace next becomes
	 * visible - the exact correct behavior.
	 */
	{
		static u64 apply_gen;
		size_t i;

		apply_gen++;
		for (i = 0; i < n_want; i++)
			want[i]->bsp_want_gen = apply_gen;
		bsp_drop_unwanted(st, &st->root, apply_gen);
	}

	if (bsp_debug_enabled())
		bsp_dump("after drop", st);

	/*
	 * Refresh node rects after the drop so any inserts that follow in
	 * this cycle (a held A+enter spawning a batch of windows that all
	 * land before the next manage cycle) see post-drop dimensions when
	 * picking split axes. Without this, every split past the first
	 * picked V axis (the new leaf's cached w/h is calloc-zero) and
	 * the layout converged to a vertical comb of narrow strips.
	 * window_apply_cell is cached so unchanged cells emit no requests;
	 * the second pass below handles the inserted windows.
	 */
	if (st->root != NULL && out->width > 0 && out->height > 0)
		bsp_apply_node(st->root, out->x, out->y, out->width,
			       out->height, l->inner_gap, l->border);

	{
		for (size_t i = 0; i < n_want; i++) {
			if (bsp_contains(st, want[i]))
				continue;
			if (bsp_debug_enabled())
				fprintf(stderr,
					"[bsp] inserting want[%zu]=%p target_hint=%p\n",
					i, (void *)want[i], (void *)focused);
			bsp_insert(st, want[i], focused);
			/*
			 * Chain subsequent in-batch inserts off the
			 * just-inserted window. Without this, when N
			 * windows arrive in a single manage cycle
			 * (held A+enter spawning a flood), every
			 * bsp_insert sees the same `focused` (river
			 * does not auto-fire window_interaction for
			 * each spawn - focus only updates on user
			 * action). bsp_insert then routes every
			 * insert to the same target leaf, halving its
			 * rect on each split until it bottoms out at
			 * 0x0; from there every split is axis V (the
			 * 0>=0 default), and the layout converges to
			 * a vertical comb on one branch of the tree.
			 * Chaining gives each new window a fresh
			 * sibling leaf with valid rect.
			 */
			focused = want[i];
		}
	}

	if (bsp_debug_enabled())
		bsp_dump("after inserts", st);
	/*
	 * No second bsp_apply_node walk: bsp_split_at and bsp_insert
	 * already pushed rects for newly-created leaves via
	 * window_apply_cell. Surviving leaves' rects were refreshed by
	 * the post-drop apply_node above, and inserts only changed the
	 * rects of the leaves they split.
	 */
}

static void layout_bsp_destroy(struct Layout *l)
{
	if (l->u.bsp == NULL)
		return;
	bsp_free(l->u.bsp->root);
	bsp_set_destroy(&l->u.bsp->members);
	free(l->u.bsp);
	l->u.bsp = NULL;
}

/*
 * Walk the tree, appending nodes onto path[] as we descend toward the
 * leaf containing target. On success, path[0..*depth-1] is root..leaf
 * and *depth is the count. Returns false if target is not in the tree
 * or if cap is too small.
 */
static bool bsp_path_to_target(struct BspNode *n, const struct Window *t,
			       struct BspNode **path, size_t cap, size_t *depth)
{
	if (n == NULL || *depth >= cap)
		return false;
	path[(*depth)++] = n;
	if (n->kind == BSP_LEAF) {
		if (n->u.leaf == t)
			return true;
		(*depth)--;
		return false;
	}
	if (bsp_path_to_target(n->u.split.a, t, path, cap, depth))
		return true;
	if (bsp_path_to_target(n->u.split.b, t, path, cap, depth))
		return true;
	(*depth)--;
	return false;
}

/*
 * Find the nearest ancestor split (closest to leaf) whose axis is
 * `axis` and whose `target_on_a` side contains the leaf for `target`.
 * NULL if no such ancestor exists or target is not in the tree.
 */
#define BSP_PATH_MAX 128
static struct BspNode *bsp_find_axis_ancestor(struct BspNode *root,
					      const struct Window *target,
					      enum BspAxis axis,
					      bool target_on_a)
{
	struct BspNode *path[BSP_PATH_MAX];
	size_t depth = 0;
	if (!bsp_path_to_target(root, target, path, BSP_PATH_MAX, &depth))
		return NULL;
	/* Walk from leaf's parent up to root. */
	for (size_t i = depth - 1; i > 0; i--) {
		struct BspNode *p = path[i - 1];
		bool t_on_a;

		if (p->kind != BSP_SPLIT)
			continue;
		if (p->u.split.axis != axis)
			continue;
		t_on_a = (p->u.split.a == path[i]);
		if (t_on_a == target_on_a)
			return p;
	}
	return NULL;
}

#define BSP_RATIO_DELTA 0.05f
#define BSP_RATIO_MIN 0.05f
#define BSP_RATIO_MAX 0.95f

static void bsp_clamp_ratio(struct BspNode *split)
{
	if (split->u.split.ratio < BSP_RATIO_MIN)
		split->u.split.ratio = BSP_RATIO_MIN;
	if (split->u.split.ratio > BSP_RATIO_MAX)
		split->u.split.ratio = BSP_RATIO_MAX;
}

/*
 * Adjust the nearest matching ancestor's ratio by `delta`. Returns
 * silently if no ancestor matches (e.g. the focused window is the
 * root leaf, or already on the desired edge of the tree).
 */
static void bsp_adjust(struct LayoutBspState *st, struct Window *t,
		       enum BspAxis axis, bool t_on_a, float delta)
{
	struct BspNode *p;

	if (st == NULL || st->root == NULL)
		return;
	p = bsp_find_axis_ancestor(st->root, t, axis, t_on_a);
	if (p == NULL)
		return;
	p->u.split.ratio += delta;
	bsp_clamp_ratio(p);
}

/*
 * Look up the parent split of the leaf containing target. Used by
 * rotate (toggle axis) and swap (sibling swap), both of which act on
 * the immediate parent.
 */
static struct BspNode *bsp_parent_of(struct BspNode *root,
				     const struct Window *target)
{
	struct BspNode *path[BSP_PATH_MAX];
	size_t depth = 0;
	if (!bsp_path_to_target(root, target, path, BSP_PATH_MAX, &depth))
		return NULL;
	if (depth < 2)
		return NULL; /* target is the root leaf, no parent */
	return path[depth - 2];
}

/* Set every split's ratio to 0.5. Used by balance and equalize. */
static void bsp_equalize_walk(struct BspNode *n)
{
	if (n == NULL || n->kind != BSP_SPLIT)
		return;
	n->u.split.ratio = 0.5f;
	bsp_equalize_walk(n->u.split.a);
	bsp_equalize_walk(n->u.split.b);
}

/*
 * Count leaves in the subtree rooted at n. Cheap: O(n_leaves) per
 * call, recurses both sides. Used by bsp_balance_area_walk to
 * compute per-split ratios that give every leaf equal pixel area.
 */
static int bsp_count_leaves(const struct BspNode *n)
{
	if (n == NULL)
		return 0;
	if (n->kind == BSP_LEAF)
		return 1;
	return bsp_count_leaves(n->u.split.a) +
	       bsp_count_leaves(n->u.split.b);
}

/*
 * Equal-area balance (mirrors bspwm's `bspc node -B`): preserves
 * the tree's topology but sets each split's ratio to
 * leaves_left / (leaves_left + leaves_right). The recursive
 * argument is induction on the rect: at each split the rect
 * divides at that ratio, so the left subtree's slice is
 * proportional to its leaf count, and recursion in each subtree
 * keeps the same property all the way down. Result: every leaf
 * lands on an equal share of the parent rect.
 *
 * Ratio gets clamped to [0.05, 0.95] to keep tiles sane on
 * extreme imbalance (e.g. 1-vs-38 leaves would otherwise produce
 * a ~2.5% slice that collapses below window minima). The clamp
 * means equal-area is approximate at the limit; in practice
 * trees with that much asymmetry should be rebalanced by
 * topology, not by ratio.
 */
static void bsp_balance_area_walk(struct BspNode *n)
{
	int la;
	int lb;
	int total;

	if (n == NULL || n->kind != BSP_SPLIT)
		return;
	la = bsp_count_leaves(n->u.split.a);
	lb = bsp_count_leaves(n->u.split.b);
	total = la + lb;
	if (total > 0)
		n->u.split.ratio = (float)la / (float)total;
	bsp_clamp_ratio(n);
	bsp_balance_area_walk(n->u.split.a);
	bsp_balance_area_walk(n->u.split.b);
}

/*
 * Rotate the entire tree 90 degrees. Every split flips axis. The CW /
 * CCW cases differ only in which axis transition swaps children +
 * complements the ratio: CW does it on HORIZ -> VERT (the bottom half
 * rotates to the left), CCW on VERT -> HORIZ. Equivalently: swap when
 * was_vert == ccw.
 */
static void bsp_rotate_walk(struct BspNode *n, bool ccw)
{
	bool was_vert;

	if (n == NULL || n->kind != BSP_SPLIT)
		return;
	was_vert = (n->u.split.axis == BSP_AXIS_VERT);
	n->u.split.axis = was_vert ? BSP_AXIS_HORIZ : BSP_AXIS_VERT;
	if (was_vert == ccw) {
		struct BspNode *tmp = n->u.split.a;

		n->u.split.a = n->u.split.b;
		n->u.split.b = tmp;
		n->u.split.ratio = 1.0f - n->u.split.ratio;
	}
	bsp_rotate_walk(n->u.split.a, ccw);
	bsp_rotate_walk(n->u.split.b, ccw);
}

/*
 * Expand/shrink verbs each map to a (axis, t_on_a, +/-delta) tuple
 * passed to bsp_adjust. Table-driven so adding a new directional
 * adjust verb is one row.
 */
static const struct {
	const char *name;
	enum BspAxis axis;
	bool t_on_a;
	float delta;
} bsp_adjust_verbs[] = {
	{ "expand-r", BSP_AXIS_VERT, true, +BSP_RATIO_DELTA },
	{ "expand-l", BSP_AXIS_VERT, false, -BSP_RATIO_DELTA },
	{ "expand-d", BSP_AXIS_HORIZ, true, +BSP_RATIO_DELTA },
	{ "expand-u", BSP_AXIS_HORIZ, false, -BSP_RATIO_DELTA },
	{ "shrink-r", BSP_AXIS_VERT, true, -BSP_RATIO_DELTA },
	{ "shrink-l", BSP_AXIS_VERT, false, +BSP_RATIO_DELTA },
	{ "shrink-d", BSP_AXIS_HORIZ, true, -BSP_RATIO_DELTA },
	{ "shrink-u", BSP_AXIS_HORIZ, false, +BSP_RATIO_DELTA },
};

static void layout_bsp_do_action(struct Layout *l, struct Window *focused,
				 const char *verb)
{
	struct LayoutBspState *st = l->u.bsp;
	if (st == NULL || st->root == NULL || verb == NULL)
		return;
	/*
	 * focused may be NULL for tree-wide ops (balance, rotate-cw,
	 * rotate-ccw). The leaf-anchored ops bail out via NULL on the
	 * path-search helpers.
	 */
	for (size_t i = 0;
	     i < sizeof(bsp_adjust_verbs) / sizeof(bsp_adjust_verbs[0]); i++) {
		if (strcmp(verb, bsp_adjust_verbs[i].name) == 0) {
			bsp_adjust(st, focused, bsp_adjust_verbs[i].axis,
				   bsp_adjust_verbs[i].t_on_a,
				   bsp_adjust_verbs[i].delta);
			return;
		}
	}
	if (strcmp(verb, "rotate") == 0) {
		struct BspNode *p = bsp_parent_of(st->root, focused);
		if (p != NULL && p->kind == BSP_SPLIT)
			p->u.split.axis = (p->u.split.axis == BSP_AXIS_VERT) ?
						  BSP_AXIS_HORIZ :
						  BSP_AXIS_VERT;
	} else if (strcmp(verb, "swap") == 0) {
		struct BspNode *p = bsp_parent_of(st->root, focused);
		if (p != NULL && p->kind == BSP_SPLIT) {
			struct BspNode *tmp = p->u.split.a;
			p->u.split.a = p->u.split.b;
			p->u.split.b = tmp;
		}
	} else if (strcmp(verb, "balance") == 0 ||
		   strcmp(verb, "equalize") == 0) {
		bsp_equalize_walk(st->root);
	} else if (strcmp(verb, "balance-area") == 0) {
		bsp_balance_area_walk(st->root);
	} else if (strcmp(verb, "rotate-cw") == 0) {
		bsp_rotate_walk(st->root, false);
	} else if (strcmp(verb, "rotate-ccw") == 0) {
		bsp_rotate_walk(st->root, true);
	}
	/* Unknown verbs ignored (forward-compat with future verbs). */
}

/*
 * Set the next split anchor. Called from seat_focus when an explicit
 * focus change happens so click-then-spawn / focus-next-then-spawn
 * route the new window to the user's intended leaf even when river
 * has not advanced seat->focused (which it does not for autospawns).
 * No-op if the window is not in this layout's tree (e.g. focus moved
 * to a window on a different workspace, but the focus event is
 * delivered to every workspace's layout).
 */
static void layout_bsp_set_anchor(struct Layout *l, struct Window *w)
{
	struct LayoutBspState *st = l->u.bsp;

	if (st == NULL || w == NULL)
		return;
	if (!bsp_contains(st, w))
		return;
	st->last_inserted = w;
}

/*
 * Recursive single-window drop. Returns the (possibly NULL or
 * collapsed) replacement subtree rooted at `n` after removing the
 * leaf for `w`. Same collapse rules as bsp_drop_unwanted: when one
 * side of a split goes empty the other side promotes; when both go
 * empty the split itself disappears. Uses w only as a key (pointer
 * comparison) - safe to call after the Window has begun teardown.
 */
static struct BspNode *bsp_drop_one(struct LayoutBspState *st,
				    struct BspNode *n, struct Window *w)
{
	struct BspNode *survivor;

	if (n == NULL)
		return NULL;
	if (n->kind == BSP_LEAF) {
		if (n->u.leaf != w)
			return n;
		if (st->last_inserted == w)
			st->last_inserted = NULL;
		free(n);
		return NULL;
	}
	n->u.split.a = bsp_drop_one(st, n->u.split.a, w);
	n->u.split.b = bsp_drop_one(st, n->u.split.b, w);
	if (n->u.split.a == NULL && n->u.split.b == NULL) {
		free(n);
		return NULL;
	}
	if (n->u.split.a == NULL) {
		survivor = n->u.split.b;
		free(n);
		return survivor;
	}
	if (n->u.split.b == NULL) {
		survivor = n->u.split.a;
		free(n);
		return survivor;
	}
	return n;
}

/*
 * Vtable hook: scrub layout state that keys on `w` before
 * window_maybe_destroy frees the Window. Drops any leaf for `w`
 * from the tree (collapsing splits as needed) and removes the
 * member-set entry. No-op if `w` was never in this tree, which is
 * the common case during whole-tree teardown.
 */
static void layout_bsp_drop_window(struct Layout *l, struct Window *w)
{
	struct LayoutBspState *st = l->u.bsp;

	if (st == NULL || w == NULL)
		return;
	if (!bsp_set_contains(&st->members, w))
		return;
	st->root = bsp_drop_one(st, st->root, w);
	bsp_set_remove(&st->members, w);
}

/*
 * BSP introspection. Walks the tree once aggregating leaf / split
 * counts, depth, and ratio extrema. well_formed is the per-node
 * sanity check: every split has two non-NULL children, every leaf
 * holds a non-NULL window, no node has an unknown kind. Used by
 * tests; production code does not call this.
 */
static void bsp_stats_walk(const struct BspNode *n, int depth,
			   struct LayoutBspStats *out)
{
	float ratio;

	if (n == NULL) {
		out->well_formed = false;
		return;
	}
	if (depth > out->max_depth)
		out->max_depth = depth;
	if (n->kind == BSP_LEAF) {
		out->n_leaves++;
		if (n->u.leaf == NULL)
			out->well_formed = false;
		return;
	}
	if (n->kind != BSP_SPLIT) {
		out->well_formed = false;
		return;
	}
	ratio = n->u.split.ratio;
	if (out->n_splits == 0) {
		out->min_ratio = ratio;
		out->max_ratio = ratio;
	} else {
		if (ratio < out->min_ratio)
			out->min_ratio = ratio;
		if (ratio > out->max_ratio)
			out->max_ratio = ratio;
	}
	out->n_splits++;
	if (n->u.split.a == NULL || n->u.split.b == NULL) {
		out->well_formed = false;
		return;
	}
	bsp_stats_walk(n->u.split.a, depth + 1, out);
	bsp_stats_walk(n->u.split.b, depth + 1, out);
}

void layout_bsp_stats(const struct Layout *l, struct LayoutBspStats *out)
{
	out->n_leaves = 0;
	out->n_splits = 0;
	out->max_depth = 0;
	out->min_ratio = 0.5f;
	out->max_ratio = 0.5f;
	out->well_formed = true;
	if (l == NULL || l->kind != LAYOUT_BSP || l->u.bsp == NULL)
		return;
	if (l->u.bsp->root == NULL)
		return;
	bsp_stats_walk(l->u.bsp->root, 1, out);
}

bool layout_bsp_contains(const struct Layout *l, const struct Window *w)
{
	if (l == NULL || l->kind != LAYOUT_BSP || l->u.bsp == NULL)
		return false;
	if (l->u.bsp->root == NULL)
		return false;
	return bsp_contains(l->u.bsp, w);
}

static struct BspNode *bsp_find_leaf(struct BspNode *n, const struct Window *w)
{
	struct BspNode *hit;

	if (n == NULL)
		return NULL;
	if (n->kind == BSP_LEAF)
		return n->u.leaf == w ? n : NULL;
	hit = bsp_find_leaf(n->u.split.a, w);
	return hit != NULL ? hit : bsp_find_leaf(n->u.split.b, w);
}

static void layout_bsp_restack(struct Layout *l, struct Window *const *before,
			       struct Window *const *after, size_t n)
{
	struct LayoutBspState *st = l->u.bsp;
	struct BspNode **leaves;
	size_t k;
	size_t m;
	size_t i;

	if (st == NULL || st->root == NULL || n == 0)
		return;
	leaves = malloc(n * sizeof(*leaves));
	if (leaves == NULL)
		return;
	k = 0;
	for (i = 0; i < n; i++)
		if (bsp_contains(st, before[i]))
			leaves[k++] = bsp_find_leaf(st->root, before[i]);
	m = 0;
	for (i = 0; i < n && m < k; i++)
		if (bsp_contains(st, after[i]))
			leaves[m++]->u.leaf = after[i];
	free(leaves);
}

void layout_init_bsp(struct Layout *l)
{
	l->kind = LAYOUT_BSP;
	l->u.bsp = calloc(1, sizeof(*l->u.bsp));
	l->inner_gap = 0;
	l->border = 0;
	l->apply = layout_bsp_apply;
	l->destroy = layout_bsp_destroy;
	l->do_action = layout_bsp_do_action;
	l->set_anchor = layout_bsp_set_anchor;
	l->drop_window = layout_bsp_drop_window;
	l->restack = layout_bsp_restack;
}

#define NM_BSP_LOAD_DEPTH BSP_PATH_MAX

struct BspLoad {
	struct Window *(*lookup)(void *ctx, const char *id);
	struct LayoutBspState *st;
	const char *p;
	void *ctx;
	bool bad;
};

static void bsp_save_node(const struct BspNode *n, FILE *f)
{
	const char *id;

	if (n->kind == BSP_LEAF) {
		id = n->u.leaf != NULL ? n->u.leaf->identifier : NULL;
		fprintf(f, " L %s", layout_id_ok(id) ? id : "-");
		return;
	}
	fprintf(f, " S %c %.6f", n->u.split.axis == BSP_AXIS_VERT ? 'v' : 'h',
		(double)n->u.split.ratio);
	bsp_save_node(n->u.split.a, f);
	bsp_save_node(n->u.split.b, f);
}

void layout_bsp_save(const struct Layout *l, FILE *f)
{
	if (l->kind != LAYOUT_BSP || l->u.bsp == NULL || l->u.bsp->root == NULL)
		return;
	bsp_save_node(l->u.bsp->root, f);
}

static bool bsp_token(struct BspLoad *ld, char *buf, size_t cap)
{
	size_t n = 0;

	while (*ld->p == ' ')
		ld->p++;
	if (*ld->p == '\0' || *ld->p == '\n')
		return false;
	while (*ld->p != '\0' && *ld->p != ' ' && *ld->p != '\n') {
		if (n + 1 >= cap)
			return false;
		buf[n++] = *ld->p++;
	}
	buf[n] = '\0';
	return true;
}

static struct BspNode *bsp_load_node(struct BspLoad *ld, int depth)
{
	struct BspNode *a, *b, *n;
	struct Window *w;
	char tok[40];
	float ratio;
	char *end;
	char axis;

	if (depth > NM_BSP_LOAD_DEPTH || !bsp_token(ld, tok, sizeof(tok)))
		goto bad;
	if (strcmp(tok, "L") == 0) {
		if (!bsp_token(ld, tok, sizeof(tok)))
			goto bad;
		w = ld->lookup(ld->ctx, tok);
		if (w == NULL || bsp_set_contains(&ld->st->members, w))
			return NULL;
		return bsp_make_leaf(ld->st, w);
	}
	if (strcmp(tok, "S") != 0 || !bsp_token(ld, tok, sizeof(tok)) ||
	    (strcmp(tok, "v") != 0 && strcmp(tok, "h") != 0))
		goto bad;
	axis = tok[0];
	if (!bsp_token(ld, tok, sizeof(tok)))
		goto bad;
	ratio = strtof(tok, &end);
	if (*end != '\0' || !(ratio > 0.0f && ratio < 1.0f))
		goto bad;
	a = bsp_load_node(ld, depth + 1);
	b = ld->bad ? NULL : bsp_load_node(ld, depth + 1);
	if (ld->bad) {
		bsp_free(a);
		bsp_free(b);
		return NULL;
	}
	if (a == NULL)
		return b;
	if (b == NULL)
		return a;
	n = calloc(1, sizeof(*n));
	if (n == NULL) {
		bsp_free(a);
		bsp_free(b);
		goto bad;
	}
	n->kind = BSP_SPLIT;
	n->u.split.axis = axis == 'v' ? BSP_AXIS_VERT : BSP_AXIS_HORIZ;
	n->u.split.ratio = ratio;
	n->u.split.a = a;
	n->u.split.b = b;
	return n;
bad:
	ld->bad = true;
	return NULL;
}

bool layout_bsp_load(struct Layout *l, const char *text,
		     struct Window *(*lookup)(void *ctx, const char *id),
		     void *ctx)
{
	struct BspLoad ld = { 0 };
	struct LayoutBspState *st;
	struct BspNode *root;
	char tok[40];

	if (l->kind != LAYOUT_BSP || l->u.bsp == NULL)
		return false;
	st = l->u.bsp;
	bsp_free(st->root);
	st->root = NULL;
	st->last_inserted = NULL;
	bsp_set_clear(&st->members);
	ld.lookup = lookup;
	ld.st = st;
	ld.p = text;
	ld.ctx = ctx;
	root = bsp_load_node(&ld, 0);
	if (!ld.bad && bsp_token(&ld, tok, sizeof(tok)))
		ld.bad = true;
	if (ld.bad) {
		bsp_free(root);
		bsp_set_clear(&st->members);
		return false;
	}
	st->root = root;
	return true;
}

#else /* NM_NO_BSP */

void layout_bsp_save(const struct Layout *l, FILE *f)
{
	(void)l;
	(void)f;
}

bool layout_bsp_load(struct Layout *l, const char *text,
		     struct Window *(*lookup)(void *ctx, const char *id),
		     void *ctx)
{
	(void)l;
	(void)text;
	(void)lookup;
	(void)ctx;
	return false;
}

void layout_bsp_stats(const struct Layout *l, struct LayoutBspStats *out)
{
	(void)l;
	out->n_leaves = 0;
	out->n_splits = 0;
	out->max_depth = 0;
	out->min_ratio = 0.5f;
	out->max_ratio = 0.5f;
	out->well_formed = true;
}

bool layout_bsp_contains(const struct Layout *l, const struct Window *w)
{
	(void)l;
	(void)w;
	return false;
}

#endif /* NM_NO_BSP */

bool layout_id_ok(const char *id)
{
	if (id == NULL || id[0] == '\0' || strlen(id) > 32)
		return false;
	for (; *id != '\0'; id++)
		if (*id <= ' ' || *id > '~')
			return false;
	return true;
}

void layout_deinit(struct Layout *l)
{
	if (l == NULL)
		return;
	if (l->destroy != NULL)
		l->destroy(l);
	memset(l, 0, sizeof(*l));
}

void layout_drop_window(struct Layout *l, struct Window *w)
{
	if (l == NULL || w == NULL)
		return;
	if (l->drop_window != NULL)
		l->drop_window(l, w);
}

