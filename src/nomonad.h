/* SPDX-License-Identifier: 0BSD */
/*
 * nomonad - xmonad-style window manager for the river compositor.
 *
 * Shared types and globals. Subsystem headers (sexpr.h, config.h, ...) and
 * main.c all include this.
 */
#ifndef NOMONAD_H
#define NOMONAD_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wayland-client-core.h>

#include "layout.h"
#include "types.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Forward declarations for opaque protocol types we hold pointers to. */
struct river_window_manager_v1;
struct river_xkb_bindings_v1;
struct river_output_v1;
struct river_window_v1;
struct river_node_v1;
struct river_seat_v1;
struct river_xkb_binding_v1;
struct river_xkb_bindings_seat_v1;
struct river_pointer_binding_v1;
struct river_layer_shell_v1;
struct river_layer_shell_output_v1;
struct river_layer_shell_seat_v1;

struct Seat;
struct Window;

enum ActionKind {
	ACTION_NONE,
	ACTION_SPAWN,
	ACTION_CLOSE,
	ACTION_FOCUS_NEXT,
	ACTION_FOCUS_PREV,
	ACTION_FOCUS_MASTER,
	ACTION_SWAP_NEXT,
	ACTION_SWAP_PREV,
	ACTION_SHRINK_MASTER, /* TALL: master area narrows */
	ACTION_EXPAND_MASTER, /* TALL: master area widens */
	ACTION_INC_MASTER, /* TALL: nmaster += 1 */
	ACTION_DEC_MASTER, /* TALL: nmaster -= 1 (clamped >= 0) */
	ACTION_TOGGLE_FLOAT, /* flip focused window between tile/float */
	ACTION_TOGGLE_FULLSCREEN, /* NBFULL: focused window covers the output */
	ACTION_MOVE,
	ACTION_RESIZE,
	/*
	 * Discrete keyboard resize for the focused floating window. arg
	 * is "EDGE DELTA" (one space-separated string) where EDGE is
	 * left / right / up / down and DELTA is a signed pixel count;
	 * positive DELTA grows the window in that direction, negative
	 * shrinks. No-op on a tiled / ignored / unfocused target -
	 * tiled windows resize via shrink-master / expand-master or BSP
	 * verbs instead.
	 */
	ACTION_RESIZE_DELTA,
	ACTION_EXIT,
	ACTION_WORKSPACE, /* arg is "N" (1-based) */
	ACTION_MOVE_TO_WORKSPACE, /* arg is "N" (1-based) */
	ACTION_NEXT_WORKSPACE, /* cycle to N+1, wraps */
	ACTION_PREV_WORKSPACE, /* cycle to N-1, wraps */
	ACTION_FOCUS_OUTPUT, /* arg is "N" (0-based output index) */
	ACTION_MOVE_TO_OUTPUT, /* arg is "N" (0-based output index) */
	/*
	 * Layout-specific verb. arg holds the verb (e.g. "rotate",
	 * "expand-l", "balance"). Routed through the focused workspace's
	 * Layout.do_action vtable. No-op on layouts that lack the verb,
	 * or layouts whose do_action is NULL (e.g. TALL today).
	 */
	ACTION_LAYOUT_VERB,
	/*
	 * Cycle the focused workspace's layouts[] array forward
	 * (NEXT) or backward (PREV) modulo n_layouts. No-op when
	 * the cycle has a single entry. Sets dirty_layout +
	 * dirty_status; the next manage cycle re-runs apply_layout
	 * on the new kind.
	 */
	ACTION_NEXT_LAYOUT,
	ACTION_PREV_LAYOUT,
	ACTION_RELOAD_CONFIG,
	/*
	 * Stash the focused window onto the scratch workspace, or pull it
	 * back to the seat's visible workspace if it is already stashed.
	 * Mirrors xmonad's XMonad.Util.NamedScratchpad dynamic toggle: any
	 * window can become a scratchpad on the fly.
	 */
	ACTION_TOGGLE_SCRATCHPAD,
	/*
	 * Show / hide a named scratchpad. arg = pad name (matches a
	 * defscratchpads entry). Spawns the configured command if no
	 * matching window exists; otherwise toggles between the seat's
	 * visible workspace and the scratch stash.
	 */
	ACTION_NAMED_SCRATCHPAD,
	/*
	 * Enter a chorded keybinding submap. arg = submap name (matches a
	 * defsubmap entry). Replaces the seat's bindings with the submap's
	 * single-key entries and arms ensure_next_key_eaten so a press of
	 * an unbound key tears the submap back down. Mirrors xmonad's
	 * XMonad.Actions.Submap.
	 */
	ACTION_SUBMAP,
	ACTION_MOVE_TO_WORKSPACE_FOLLOW,
	ACTION_FOCUS_LAST,
	ACTION_RESTART,
	ACTION_TOGGLE_STICKY,
	ACTION_FOCUS_MRU_NEXT,
	ACTION_FOCUS_MRU_PREV,
};

/*
 * Float state model: the user's explicit toggle wins against float
 * rules. New windows default to FLOAT_AUTO (0, hence calloc-friendly);
 * apply_rules then computes w->floating from the rule list. Once the
 * user calls toggle-float or starts a move/resize op, float_override
 * latches to FORCE_FLOAT or FORCE_TILE and apply_rules respects it.
 */
enum FloatOverride {
	FLOAT_AUTO,
	FLOAT_FORCE_FLOAT,
	FLOAT_FORCE_TILE,
};

/*
 * An action with optional payload. `arg` is owned by whoever holds the
 * Action by value; copy with action_dup(), free with action_free_inner().
 */
struct Action {
	enum ActionKind kind;
	char *arg;
};

enum LayerFocus {
	LAYER_FOCUS_NONE,
	LAYER_FOCUS_EXCLUSIVE,
	LAYER_FOCUS_NON_EXCLUSIVE,
};

enum SeatOp {
	SEAT_OP_NONE,
	SEAT_OP_MOVE,
	SEAT_OP_RESIZE,
};

struct Output {
	struct river_output_v1 *obj;
	bool removed;
	s32 x;
	s32 y;
	s32 width;
	s32 height;
	int visible_workspace; /* index into wm.workspaces */
	struct river_layer_shell_output_v1 *ls_output;
	u32 capture_sessions;
	bool presentation_synced;
	u32 presentation_mode;
	bool has_usable;
	s32 usable_x;
	s32 usable_y;
	s32 usable_width;
	s32 usable_height;
	struct wl_list link; /* WindowManager.outputs */
};

struct Window {
	struct river_window_v1 *obj;
	struct river_node_v1 *node;

	bool is_new;
	bool closed; /* set by .closed event; window_maybe_destroy reaps */
	/*
	 * Set in ACTION_CLOSE the moment we send river_window_v1_close
	 * so focus advances and held auto-repeat targets a fresh sibling
	 * per tick, not the dying same. We must NOT reap the Window /
	 * destroy the proxy on this flag - the real window is still
	 * alive on river's side until .closed arrives. Reaping early
	 * orphans the xdg_toplevel: it stays visible on screen and we
	 * lose the handle to send any further requests to it, which
	 * looks to the user like A+q "does nothing".
	 */
	bool requested_close;
	u64 close_requested_ns;
	u32 serial;
	bool visible_desired; /* set in manage, applied in render */

	s32 x;
	s32 y;
	s32 width;
	s32 height;

	/*
	 * Last position/dimensions we asked the server for. Used to suppress
	 * redundant set_position / propose_dimensions on the steady-state
	 * manage path (apply_layout used to fire two requests per visible
	 * window per cycle even when nothing had changed). x/y already cache
	 * the last-set position; the bool guards the "0 == 0 on a new
	 * window" pitfall. propose tracks dimensions separately because
	 * width/height get overwritten by the server's dimensions event.
	 */
	bool position_synced;
	bool dimensions_synced;
	s32 last_proposed_w;
	s32 last_proposed_h;

	/*
	 * Last visibility / border state we asked the server for. Same
	 * idea as the position/dimensions caches: render-cycle apply_visi-
	 * bility and apply_borders walk every visible window every render
	 * regardless of state change. With ~27 windows that is ~54 redundant
	 * requests per pointer event. The set_borders args are pre-multi-
	 * plied 32-bit RGBA channels, hence cached after the math.
	 */
	bool visible_synced;
	bool last_shown;
	bool borders_synced;
	u32 last_border_edges;
	s32 last_border_width;
	u32 last_border_r;
	u32 last_border_g;
	u32 last_border_b;
	u32 last_border_a;

	/* Metadata from window events; owned strings, may be NULL. */
	char *app_id;
	char *title;
	char *identifier; /* river_window_v1.identifier (v4); persistent */
	s32 unreliable_pid; /* river_window_v1.unreliable_pid; 0 if unknown */
	bool has_parent;

	/*
	 * Min/max size hints from river_window_v1.dimensions_hint. A value
	 * of 0 means "no preference for that dimension" (per protocol).
	 * Used by the layout to clamp cell sizes against client minima so
	 * a deeply-split BSP does not propose 50px terminals.
	 */
	s32 min_width;
	s32 min_height;
	s32 max_width;
	s32 max_height;

	/*
	 * Last set_dimension_bounds we sent. The layout calls
	 * window_set_bounds with the cell ceiling each cycle; the cache
	 * elides the request when the value matches (same shape as the
	 * other cached protocol calls).
	 */
	bool bounds_synced;
	s32 last_bounds_w;
	s32 last_bounds_h;
	bool clip_synced;
	s32 last_clip_w;
	s32 last_clip_h;

	/* Float state, set by apply_rules each manage cycle. */
	bool floating;
	bool was_floating;
	bool float_needs_place;
	enum FloatOverride float_override;
	s32 float_x, float_y;
	s32 float_width, float_height;
	bool float_anchored;
	s32 anchor_x, anchor_y;
	s32 anchor_width, anchor_height;

	/*
	 * Fullscreen flag. Set by ACTION_TOGGLE_FULLSCREEN (one per
	 * workspace) or by the client via fullscreen_requested. While
	 * set, the window covers its workspace's output completely;
	 * other windows on the same workspace are hidden.
	 */
	bool fullscreen;
	bool fullscreen_client;
	struct Output *fullscreen_output;
	bool fullscreen_informed;
	bool fullscreen_seen;
	bool tiled_synced;
	u32 last_tiled;
	u32 presentation_hint;
	u32 capture_sessions;

	/*
	 * Set by apply_rules when an (ignore MATCHER) defrules entry
	 * matches. Ignored windows are skipped entirely by the layout,
	 * visibility, border, and focus paths; the WM keeps them in
	 * the workspace list but otherwise leaves them alone (used for
	 * desktop / panel surfaces).
	 */
	bool ignored;

	/*
	 * Per-window dirty bit for the rule scan. Inputs that affect
	 * apply_rules' output for a single window (app_id, title,
	 * has_parent, float_override, plus the rule list itself) flip
	 * this. apply_rules processes only dirty windows and clears the
	 * bit on exit; non-dirty windows keep their previous floating /
	 * ignored / scratchpad latches. wm_handle_window sets it true
	 * on creation, listener callbacks set it on prop change, the
	 * float-override mutation sites in seat.c set it after toggle /
	 * promote, and reload_now marks every window so the new rule
	 * list gets applied.
	 */
	bool rules_dirty;

	struct Seat *pointer_move_requested;
	struct Seat *pointer_resize_requested;
	u32 pointer_resize_requested_edges;

	int workspace_index;
	struct wl_list workspace_link; /* Workspace.windows */

	/*
	 * Scratchpad bookkeeping. is_scratchpad latches once apply_rules
	 * (or the dynamic toggle action) has marked the window as a
	 * scratchpad; it survives stash / restore moves and is what the
	 * named-pad lookup keys off of. scratch_name is owned and only
	 * non-NULL for named pads (dynamic stash leaves it NULL). Layered
	 * on top of the float machinery: a scratchpad always floats while
	 * managed, so apply_rules sets float_override = FORCE_FLOAT.
	 */
	bool sticky;
	bool is_scratchpad;
	bool was_scratchpad; /* prior is_scratchpad, mirrors was_floating */
	char *scratch_name;

	/*
	 * BSP layout's per-cycle "is this window in the wanted set?" mark.
	 * layout_bsp_apply bumps a static gen each call and writes the new
	 * value here for every w in want[]; bsp_drop_unwanted survives a
	 * leaf iff its window's bsp_want_gen matches the current cycle.
	 * No initialization needed - calloc gives 0, the gen counter
	 * starts at 1, so a fresh window is correctly "not wanted" until
	 * an apply marks it. Same monotonic counter works across multiple
	 * BSP trees because each apply uses its own current value to
	 * compare; older marks compare unequal and get dropped naturally.
	 */
	u64 bsp_want_gen;
	u64 raise_seq;
	u64 focus_seq;
};

/*
 * Cap on the per-workspace layout cycle. Each Workspace holds a
 * fixed-size array of struct Layout (embedded by value); n_layouts
 * tracks how many entries the active config populated, current
 * indexes the active one. 16 covers the six built-in kinds plus
 * generous headroom; override at compile time with -DNM_LAYOUTS_MAX=N
 * (the array becomes the per-workspace memory cost: 16 * sizeof(Layout)
 * is around 1.3 KB on x86-64). Must be at least 1 - the default
 * config seeds a single-entry array with TALL.
 */
#ifndef NM_LAYOUTS_MAX
#define NM_LAYOUTS_MAX 16
#endif
#if NM_LAYOUTS_MAX < 1
#error "NM_LAYOUTS_MAX must be at least 1"
#endif

struct Workspace {
	int index;
	char *name; /* owned */
	/*
	 * Per-workspace layout cycle. layouts[0..n_layouts-1] are
	 * initialised from the active config's layout_kinds list (in
	 * order); current_layout_index points at the entry that
	 * apply_layout should run. ACTION_NEXT_LAYOUT / PREV_LAYOUT
	 * advance the index modulo n_layouts. Each entry owns its own
	 * Layout state: cycling between BSP and TALL preserves both
	 * the BSP tree and the TALL master ratio so flipping back and
	 * forth keeps each layout's settings.
	 */
	struct Layout layouts[NM_LAYOUTS_MAX];
	size_t n_layouts;
	size_t current_layout_index;
	struct wl_list windows; /* Window.workspace_link */
	/*
	 * Reusable scratch buffer for apply_layout's tile-window array.
	 * Grows on demand via realloc; never shrinks. NULL until first
	 * use. Goal: zero heap allocations on the steady-state manage
	 * cycle (first known target of the preallocation sweep).
	 */
	struct Window **layout_arr;
	size_t layout_arr_cap;
	size_t layout_arr_len;
};

/*
 * Active layout for a workspace: the entry pointed at by
 * current_layout_index. Returns &workspace->layouts[0] when
 * n_layouts == 0 (only briefly possible during teardown - the
 * default config installs a single TALL entry). Inline so the
 * indirection compiles to a single pointer arithmetic op on the
 * hot manage-cycle path.
 */
static inline struct Layout *workspace_layout(struct Workspace *ws)
{
	return &ws->layouts[ws->current_layout_index];
}

struct XkbBinding {
	struct river_xkb_binding_v1 *obj;
	struct Seat *seat;
	struct Action action; /* owns action.arg */
	u32 mods;
	struct wl_list link; /* Seat.xkb_bindings */
};

struct PointerBinding {
	struct river_pointer_binding_v1 *obj;
	struct Seat *seat;
	struct Action action; /* owns action.arg */
	struct wl_list link; /* Seat.pointer_bindings */
};

struct Seat {
	struct river_seat_v1 *obj;
	bool is_new;
	bool removed;

	struct Output *output; /* focused output */
	struct Output *pointer_output;
	struct Window *focused;
	struct Window *hovered;
	struct Window *interacted;
	struct Window *focus_cur;
	struct Window *focus_last;
	struct Window **mru;
	size_t mru_n;
	size_t mru_cap;
	size_t mru_pos;
	u32 mru_watch;
	u32 pending_mods;
	bool mru_active;
	bool mru_stepping;
	bool mru_commit_pending;

	struct wl_list xkb_bindings; /* XkbBinding */
	struct wl_list pointer_bindings; /* PointerBinding */
	struct Action pending_action; /* by value; kind=NONE means none */

	/*
	 * Submap state. xkb_seat is the per-seat xkb-bindings handle
	 * (river_xkb_bindings_v1.get_seat, v2) used to drive
	 * ensure_next_key_eaten / cancel_ensure_next_key_eaten and to
	 * receive ate_unbound_key. submap_active reflects "the seat's
	 * bindings have been temporarily replaced by a submap entry set."
	 * submap_exit_pending is set by the ate_unbound_key listener
	 * (which fires outside our manage cycle) and drained at the top
	 * of seat_manage so the submap teardown happens inside a manage
	 * sequence - same pattern as drain_repeat_timer.
	 */
	struct river_xkb_bindings_seat_v1 *xkb_seat;
	struct river_layer_shell_seat_v1 *ls_seat;
	enum LayerFocus layer_focus;
	bool refocus_pending;
	bool submap_active;
	bool submap_exit_pending;

	enum SeatOp op;
	/* SEAT_OP_MOVE and SEAT_OP_RESIZE */
	struct Window *op_window;
	int op_workspace;
	s32 op_start_x, op_start_y;
	s32 op_dx, op_dy;
	bool op_release;
	/* SEAT_OP_RESIZE only */
	s32 op_start_width, op_start_height;
	u32 op_edges;

	/*
	 * Auto-repeat. river's xkb-bindings protocol does not auto-fire
	 * bindings; the WM is responsible. timerfd_create'd in
	 * wm_handle_seat, armed by xkb_binding_handle_pressed at the xkb
	 * defaults (250 ms delay, 40 ms interval), disarmed on
	 * released or stop_repeat. repeat_action is action_dup'd from
	 * the active binding so it survives a config reload.
	 */
	int repeat_timer_fd;
	struct Action repeat_action;

	struct wl_list link; /* WindowManager.seats */
};

struct WindowManager {
	struct wl_list outputs; /* Output */
	/*
	 * The workspace array is laid out as
	 *   [0 .. n_workspaces - 2]   user-addressable workspaces
	 *   [n_workspaces - 1]        scratch stash (xmonad's "NSP"
	 *                             workspace - hidden, hosts stashed
	 *                             scratchpads, never selected by the
	 *                             user via (workspace N) /
	 *                             next-workspace / prev-workspace,
	 *                             never assigned to an output)
	 * wm_user_n_workspaces() / wm_scratch_ws_index() name the split
	 * so call sites that cycle or address the user range do not have
	 * to know about the off-by-one.
	 */
	struct Workspace *workspaces; /* owned, length n_workspaces */
	size_t n_workspaces;
	struct wl_list seats; /* Seat */
	/*
	 * LIFO stack of recently-orphaned workspace indices: when an
	 * output goes away, the workspace it was showing is pushed here;
	 * when a new output appears, the top entry (if still unshown) is
	 * preferred over "lowest unused." Restores the typical xmonad
	 * unplug-replug case without needing per-output identity tracking
	 * (we'd need wl_output.name + a side table). Capacity matches the
	 * max plausible simultaneously-orphaned count.
	 */
	int orphan_ws[16];
	size_t n_orphan_ws;

	/*
	 * Two independent dirty flags, both cleared at the top of
	 * wm_handle_manage_start.
	 *
	 * dirty_status: any change that an external status consumer
	 * would care about. Focus moves, the visible workspace on an
	 * output, window add / remove, app_id / title / parent /
	 * fullscreen, layout-mutating actions, output geometry, the
	 * pointer crossing between outputs. Read by the broadcast
	 * publish hook (step 5) to skip the snapshot build when
	 * nothing observable changed.
	 *
	 * dirty_layout: a strict subset that means the layout pass's
	 * output may differ from the previous cycle. Excludes pure
	 * focus changes and pointer-output crosses (those route
	 * through place_top / seat->output without touching tile
	 * rects). Read by the update_visibility + apply_layout gate
	 * at the bottom of the manage cycle.
	 *
	 * A site that mutates layout state always sets both, so it is
	 * fine to think of dirty_layout as "implies dirty_status." The
	 * focus / pointer-cross sites only set dirty_status.
	 */
	bool dirty_status;
	bool dirty_layout;
	struct Output *layer_default;
	u32 next_window_serial;
	u64 next_raise_seq;
	bool restack_floats;
	u64 next_focus_seq;
};

struct Config;

extern struct WindowManager wm;
extern struct river_window_manager_v1 *window_manager_v1;
extern struct river_xkb_bindings_v1 *xkb_bindings_v1;
extern struct river_layer_shell_v1 *layer_shell_v1;
extern struct Config *g_config;

/* Last workspace index, reserved for stashed scratchpads. */
static inline int wm_scratch_ws_index(void)
{
	return (wm.n_workspaces > 0) ? (int)(wm.n_workspaces - 1) : -1;
}

/* User-facing workspace count: n_workspaces minus the scratch slot. */
static inline size_t wm_user_n_workspaces(void)
{
	return (wm.n_workspaces > 0) ? wm.n_workspaces - 1 : 0;
}

/*
 * Auto-repeat. seat_repeat_arm sets the seat's pending repeat to a
 * copy of `action` and (re)starts the seat's timerfd at the xkb
 * default delay+interval. seat_repeat_disarm cancels and clears.
 * Both are no-ops on seats whose timerfd never came up. Defined in
 * main.c; called from binding.c on pressed / released / stop_repeat.
 */
void seat_repeat_arm(struct Seat *seat, const struct Action *action);
void seat_repeat_disarm(struct Seat *seat);

/*
 * Push a position / dimensions to the server, but only if the value
 * differs from the last one we pushed for this window. Steady-state
 * apply_layout used to emit two requests per visible window per cycle
 * regardless of state change; with these wrappers, an unchanged tile
 * cycle is zero requests. The first call after window creation always
 * goes through (the *_synced bools start false). Defined in main.c;
 * called from main.c and layout.c.
 */
void window_set_position(struct Window *window, s32 x, s32 y);
void window_propose_dimensions(struct Window *window, s32 width, s32 height);

/*
 * Recommend a size ceiling to the client (forwarded to xdg_toplevel as
 * configure_bounds). GTK/browsers use this to size their swapchain,
 * avoiding "alloc full-screen texture, get cropped to a small cell"
 * waste. Cached identically to position/dimensions; only emits the
 * request when the bound changed. Defined in main.c.
 */
void window_set_bounds(struct Window *window, s32 width, s32 height);
void window_set_clip(struct Window *window, s32 width, s32 height);
void window_raise(struct Window *window);

/*
 * Output module (output.c). The listener struct is wired up by
 * wm_handle_output when a new output appears; the helpers query and
 * reap output state.
 */
extern const struct river_output_v1_listener river_output_listener;
void output_maybe_destroy(struct Output *output);
struct Output *output_showing(int workspace_index);
void output_attach_layer_shell(struct Output *output);
void output_set_layer_default(struct Output *output);
struct Output *output_at_index(int n);

/*
 * Window module (window.c). The listener struct is wired up by
 * wm_handle_window. window_manage / window_maybe_destroy run as part
 * of the manage cycle (wm.c). window_apply_decoration is also called
 * from reload_now when the global decoration mode changes.
 */
extern const struct river_window_v1_listener river_window_listener;
void window_maybe_destroy(struct Window *window);
void window_apply_decoration(struct Window *w);
void window_manage(struct Window *window);
u64 window_monotonic_ns(void);

/*
 * Seat module (seat.c). The listener struct is wired up by
 * wm_handle_seat. Lifecycle / cycle hooks (maybe_destroy, manage,
 * render) are called from the manage and render cycles. The
 * pointer-op kickoffs (seat_pointer_move, seat_pointer_resize) are
 * dispatched to from window_manage when a client posts a move /
 * resize request.
 */
extern const struct river_seat_v1_listener river_seat_listener;
extern const struct river_xkb_bindings_seat_v1_listener
	river_xkb_bindings_seat_listener;
void seat_maybe_destroy(struct Seat *seat);
void seat_manage(struct Seat *seat);
void seat_raise_floats(int wsi);
bool seat_focus_window(struct Seat *seat, struct Window *w);
bool seat_close_window(struct Seat *seat, struct Window *w);
void seat_run_action(struct Seat *seat, const struct Action *action);
void seat_attach_layer_shell(struct Seat *seat);
void seat_render(struct Seat *seat);
void seat_pointer_move(struct Seat *seat, struct Window *window);
void seat_pointer_resize(struct Seat *seat, struct Window *window, u32 edges);
void drain_repeat_timer(struct Seat *seat);

/*
 * Reload signaling. schedule_reload sets the pending flag and asks
 * river for a fresh manage cycle; the actual config swap happens in
 * wm_handle_manage_start (wm.c) by checking g_reload_pending and
 * calling reload_now. SIGUSR1 / inotify drains and
 * ACTION_RELOAD_CONFIG (seat.c) all funnel through schedule_reload.
 */
extern volatile sig_atomic_t g_reload_pending;
void schedule_reload(void);
void reload_now(void);

/*
 * Window manager module (wm.c). wm_init builds the workspace array
 * and seeds the lists. wm_listener is the per-cycle hook the server
 * drives.
 */
extern const struct river_window_manager_v1_listener wm_listener;
void wm_init(void);
void wm_swap_config(struct Config *staging);
struct Window *wm_workspace_fullscreen(struct Workspace *ws);
void wm_float_anchor_to(struct Window *w, const struct Output *out);

/*
 * layout_init_from_kind initialises the Layout in place to the named
 * kind with that kind's defaults (e.g. nmaster=1 ratio=0.5 for the
 * tall family, empty tree for BSP). Defined in wm.c; called from
 * wm_init and reload_now to populate each entry of a workspace's
 * layouts[] array.
 */
struct Layout;
void layout_init_from_kind(struct Layout *l, enum LayoutKind kind);

/*
 * Reseed ws->layouts[] from the active config. Caller is responsible
 * for layout_deinit'ing each prior entry first (the function neither
 * inspects nor frees them; reusing it on initialised entries leaks).
 * current_layout_index is clamped into the new array length.
 */
struct Workspace;
void workspace_init_layouts(struct Workspace *ws);

#endif /* NOMONAD_H */
