/* SPDX-License-Identifier: 0BSD */
/*
 * Layout policy: given an output and a window list, compute and apply
 * a rect (position + dimensions) per window. Layouts are tagged unions
 * with a vtable, embedded by value so there is no heap lifecycle for
 * the Layout itself (BSP keeps its tree on the heap behind an opaque
 * pointer).
 *
 * This header carries no dependency on nomonad.h so it can be embedded
 * by value inside types declared there (Workspace).
 */
#ifndef NOMONAD_LAYOUT_H
#define NOMONAD_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "types.h"

struct Output;
struct Window;

/*
 * Compile-time per-layout opt-out. Defining NM_NO_<KIND> for any of
 * BSP, DECK, MIRROR_TALL, THREE_COLUMNS, GRID, TABBED, SPIRAL,
 * TWO_PANE, RESIZABLE_TALL drops that layout's
 * implementation, descriptor table row, and dispatch case from the
 * built binary; the parser rejects the name and the linker drops
 * the unreachable apply / init code. Useful when targeting a
 * minimal nomonad build for a low-resource host. TALL is always
 * compiled in - it is the fallback for layout_init_from_kind on
 * unknown kinds and the default config.
 *
 * The enum values stay defined regardless so test code and tooling
 * have a stable namespace; only the implementations get gated.
 */
enum LayoutKind {
	LAYOUT_TALL,
	LAYOUT_BSP,
	/*
	 * Deck (a.k.a. monocle / Full): the focused tile-eligible
	 * window covers the entire inset-adjusted area; all other
	 * tiled windows on the workspace are hidden via
	 * visible_desired and the existing render hide / show path.
	 * Stateless. Inspired by xmonad's Full layout and zrwm's
	 * "deck" verb (zrwm/zrwm.c:1827-1953); same shape but
	 * simpler since we hide rather than fight to overlap.
	 */
	LAYOUT_DECK,
	/*
	 * Mirror Tall: TALL transposed 90 degrees. Master row on top
	 * at `ratio` of the output's height, stack row on bottom.
	 * Both rows subdivide horizontally (columns within each row);
	 * the last column in each row absorbs leftover pixels. Shares
	 * struct LayoutTallParams with LAYOUT_TALL: master ratio /
	 * count actions in seat.c read u.tall.{ratio,nmaster}
	 * regardless of orientation. Stateless. Mirrors xmonad's
	 * `Mirror Tall` modifier.
	 */
	LAYOUT_MIRROR_TALL,
	/*
	 * Three Columns (xmonad's ThreeColMid): master column in the
	 * center at `ratio` of the output's width holding nmaster
	 * tiles stacked vertically; remaining slaves split between
	 * right (ceil(rest/2)) and left (floor(rest/2)) columns at
	 * (1-ratio)/2 each. With <=1 slave the layout collapses to
	 * two columns (master + right) so the third column does not
	 * appear empty. Stateless. Shares struct LayoutTallParams
	 * with LAYOUT_TALL; the ratio governs the center column's
	 * width rather than a single master/stack split.
	 */
	LAYOUT_THREE_COLUMNS,
	/*
	 * Grid: roughly square NxN tile distribution. cols is the
	 * smallest c such that c*c >= n; rows = ceil(n/cols). Each
	 * row's cells fill the output width evenly, so when the last
	 * row is partial its tiles widen to absorb the leftover
	 * columns rather than leaving trailing empty space.
	 * Stateless. No nmaster / ratio - the parameter union arm is
	 * unused for this kind.
	 */
	LAYOUT_GRID,
	/*
	 * Tabbed: visually identical to Deck for v1 - the focused
	 * tile gets the whole area, others hide. xmonad's Tabbed
	 * draws a horizontal tab bar above the content; the river
	 * protocol does not give us a layer-shell surface without
	 * additional wiring, so the bar is deferred. Users who want
	 * tab visuals run waybar / yambar with the status broadcast
	 * subscriber, which already announces the focused window per
	 * workspace. Stateless. focus_changes_visibility = true.
	 */
	LAYOUT_TABBED,
	/*
	 * Spiral / Dwindle: each new window splits the previous one's
	 * remaining cell, alternating between vertical (left/right)
	 * and horizontal (top/bottom) axes. The result spirals into
	 * the bottom-right corner with each tile half the previous
	 * one's relevant dimension at the fixed 0.5 ratio. Stateless.
	 * Mirrors xmonad's XMonad.Layout.Dwindle.
	 */
	LAYOUT_SPIRAL,
	/*
	 * TwoPane: master on the left at u.tall.ratio of the output's
	 * width, the focused non-master on the right; every other
	 * non-master tile hides via visible_desired. Focus walks the
	 * stack via focus-next / focus-prev to pick which "other"
	 * occupies the right pane, giving an n-window workflow on a
	 * two-cell layout. focus_changes_visibility = true. nmaster
	 * is always 1 (the field exists for u.tall sharing but is
	 * unused; INC_MASTER / DEC_MASTER are no-ops on this kind).
	 */
	LAYOUT_TWO_PANE,
	/*
	 * ResizableTall: TALL plus per-cell height adjust. Master
	 * column on the left at u.tall.ratio of the output's width,
	 * stack column on the right (same shape as TALL); each cell
	 * carries a signed pixel delta on top of the column's even
	 * split. New verbs expand-cell / shrink-cell drive the
	 * focused cell's delta up / down by NM_RESIZE_CELL_STEP px
	 * and pair the change with the adjacent cell to keep the
	 * column total constant. State is heap-allocated and lives
	 * in struct Layout.resize_deltas (NULL on every other kind);
	 * the destroy / drop_window vtable hooks scrub it.
	 */
	LAYOUT_RESIZABLE_TALL,
};

struct LayoutTallParams {
	s32 nmaster; /* count of windows in the master area */
	float ratio;     /* master width / output width, 0..1 */
};

struct LayoutBspState; /* opaque tree, defined in layout.c */
struct LayoutResizeDeltas; /* opaque per-cell delta map, defined in layout.c */

struct Layout {
	enum LayoutKind kind;
	union {
		struct LayoutTallParams tall;
		struct LayoutBspState *bsp; /* owned heap state */
	} u;
	/*
	 * Per-cell height deltas, used only by LAYOUT_RESIZABLE_TALL.
	 * Owned heap state; NULL on every other kind. Sits outside
	 * the union so RESIZABLE_TALL can keep nmaster / ratio in
	 * u.tall (sharing the master-action dispatch with the rest
	 * of the tall family). layout_init_resizable_tall allocates
	 * the table; layout_deinit -> destroy frees it.
	 */
	struct LayoutResizeDeltas *resize_deltas;
	/*
	 * Per-cycle parameters refreshed by apply_layout from the active
	 * config. Layouts read these when computing window rects so border
	 * and inner-gap settings hot-reload without re-initializing.
	 */
	s32 inner_gap;
	s32 border;
	/*
	 * Set by layouts whose visible-set or tile rects change when the
	 * seat's focus moves within the workspace. Deck (monocle) is the
	 * obvious case: only the focused window is shown, others are
	 * hidden via visible_desired. Tall and BSP do not flip this -
	 * focus changes their place_top z-order via seat_focus directly,
	 * not anything apply_layout cares about. seat_focus reads this
	 * to decide whether to set wm.dirty_layout on a focus change.
	 * False on stateless layouts.
	 */
	bool focus_changes_visibility;
	/*
	 * apply takes the layout's available rect plus the canonical
	 * tiled-window list for that workspace. `focused` is the seat
	 * focus on this workspace (NULL if none) and is consumed by
	 * stateful layouts to choose split / insert sites. Stateless
	 * layouts (Tall) ignore it. Stateful layouts (BSP) reconcile
	 * their internal state against the list, then walk it.
	 */
	void (*apply)(struct Layout *l, const struct Output *out,
		      struct Window **windows, size_t n,
		      struct Window *focused);
	/* Optional, may be NULL. Frees any heap state owned by Layout. */
	void (*destroy)(struct Layout *l);
	/*
	 * Optional. Called from ACTION_BSP_* dispatch (and any future
	 * layout-specific action). `verb` is a stable short string like
	 * "rotate", "swap", "expand-l", "shrink-r", "balance", etc.
	 * Layouts that do not implement an action ignore it. NULL means
	 * the layout has no actions at all.
	 */
	void (*do_action)(struct Layout *l, struct Window *focused,
			  const char *verb);
	/*
	 * Optional. Called whenever seat_focus moves explicit focus to a
	 * window on this workspace (user click, focus-next, focus-master,
	 * etc.). Stateful layouts use this to track the "next split
	 * anchor" so a click followed by a spawn lands the new window
	 * next to the click target. Without this, BSP would rely on
	 * seat->focused which can lag behind the spawn rate when river
	 * does not auto-fire window_interaction for newly created
	 * windows. NULL on stateless layouts.
	 */
	void (*set_anchor)(struct Layout *l, struct Window *w);
	/*
	 * Optional. Called from window_maybe_destroy just before a Window
	 * is freed so layouts that hold pointer-keyed state (BSP's leaf
	 * tree) can drop the entry. Without this, an unvisible workspace
	 * keeps a stale leaf pointing at the freed Window, and the next
	 * apply_layout on that workspace dereferences it. The callee
	 * must use w only as a key (pointer comparison); the Window may
	 * be in late teardown. NULL on layouts with no per-window state.
	 */
	void (*drop_window)(struct Layout *l, struct Window *w);
	void (*restack)(struct Layout *l, struct Window *const *before,
			struct Window *const *after, size_t n);
};

void layout_init_tall(struct Layout *l, s32 nmaster, float ratio);
void layout_init_bsp(struct Layout *l);
void layout_init_deck(struct Layout *l);
void layout_init_mirror_tall(struct Layout *l, s32 nmaster, float ratio);
void layout_init_three_columns(struct Layout *l, s32 nmaster, float ratio);
void layout_init_grid(struct Layout *l);
void layout_init_tabbed(struct Layout *l);
void layout_init_spiral(struct Layout *l);
void layout_init_two_pane(struct Layout *l, float ratio);
void layout_init_resizable_tall(struct Layout *l, s32 nmaster, float ratio);

/*
 * Layout descriptor table: single source of truth tying a config
 * token to an enum LayoutKind. config.c uses it to parse (layout
 * NAME); status.c reverse-maps `kind` back to `name` for the
 * broadcast. wm.c's layout_init_from_kind reads it as the kind
 * iterator and dispatches via a switch on `kind` (the per-layout
 * inits live in layout.c next to their state). Adding a layout
 * means one row here, one case in layout_init_from_kind, and one
 * apply / init pair in layout.c. The compile-time per-layout
 * opt-out hooks `#ifndef`s onto that triple.
 *
 * Kept in its own TU (layout_names.c) so the test binaries that
 * only need the parser path can link against it without pulling in
 * layout.c's full apply / init chain (which transitively reaches
 * window.c and the wayland-client protocol code).
 */
struct LayoutDescriptor {
	const char *name;
	enum LayoutKind kind;
};

extern const struct LayoutDescriptor layout_descriptors[];
extern const size_t layout_descriptors_n;

/* Calls l->destroy if set, then zeroes the struct. */
void layout_deinit(struct Layout *l);

/*
 * BSP introspection for tests / harness invariants. Aggregates over
 * every node in the tree. On a non-BSP layout or an empty BSP tree
 * (no windows yet inserted) all counts are zero, ratios default to
 * 0.5, well_formed is true. Bypasses the opaque LayoutBspState
 * pointer so callers do not need access to struct BspNode.
 */
struct LayoutBspStats {
	size_t n_leaves;
	size_t n_splits;
	int max_depth;
	float min_ratio;
	float max_ratio;
	bool well_formed;
};

void layout_bsp_stats(const struct Layout *l, struct LayoutBspStats *out);

/*
 * True if `w` corresponds to a leaf in the BSP tree. Returns false on
 * a non-BSP layout, an empty tree, or any other layout kind.
 */
bool layout_bsp_contains(const struct Layout *l, const struct Window *w);

/*
 * Forward to the layout's drop_window vtable hook. Called from
 * window_maybe_destroy to scrub layout-internal state that keys on
 * the Window pointer (BSP leaf tree, member set) before the Window
 * is freed. No-op on layouts that do not implement the hook.
 */
void layout_drop_window(struct Layout *l, struct Window *w);

bool layout_id_ok(const char *id);
void layout_bsp_save(const struct Layout *l, FILE *f);
bool layout_bsp_load(struct Layout *l, const char *text,
		     struct Window *(*lookup)(void *ctx, const char *id),
		     void *ctx);

#endif /* NOMONAD_LAYOUT_H */
