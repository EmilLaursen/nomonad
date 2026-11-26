/* SPDX-License-Identifier: 0BSD */
/*
 * Config: AST -> binding tables.
 *
 * Holds the parsed config: defcfg blocks, key and mouse bindings,
 * rules, scratchpads, submaps.
 *
 * The Config owns everything inside it: each action.arg is a strdup'd
 * string, freed by config_free().
 */
#ifndef NOMONAD_CONFIG_H
#define NOMONAD_CONFIG_H

#include <stddef.h>
#include <stdio.h>

#include <xkbcommon/xkbcommon.h>

#include "nomonad.h"

#define NM_DEFAULT_USER_WORKSPACES 9
#define NM_MAX_OUTPUT_INDEX 64
#define NM_MAX_RESIZE_DELTA 10000
#define NM_MAX_INSET 1000

struct XkbBindingDef {
	u32 mods;
	xkb_keysym_t keysym;
	struct Action action;
};

struct PointerBindingDef {
	u32 mods;
	u32 button;
	struct Action action;
};

enum RuleMatchKind {
	RULE_MATCH_APP_ID,
	RULE_MATCH_TITLE,
	RULE_MATCH_PARENT,
};

/*
 * What to do with a window the matcher selects. RULE_VERB_FLOAT is the
 * default (calloc-friendly): float the window, optionally :center on
 * first appearance. RULE_VERB_IGNORE leaves the window untouched: the
 * layout, visibility, border, and focus paths all skip it. Used for
 * desktop / panel surfaces that the WM should not manage.
 */
enum RuleVerb {
	RULE_VERB_FLOAT,
	RULE_VERB_IGNORE,
};

struct FloatRule {
	enum RuleVerb verb;
	enum RuleMatchKind kind;
	char *pattern; /* owned; used for APP_ID and TITLE, NULL otherwise */
	bool center;   /* only meaningful for RULE_VERB_FLOAT */
	bool glob;     /* if true, fnmatch(pattern, target, 0) instead of strcmp */
};

/*
 * Rational-rect geometry for scratchpads, mirroring xmonad's
 * W.RationalRect: x/y/w/h are fractions of the output's content area
 * (0..1). When `set` is false, fall back to the same center-on-output
 * behavior float rules use with :center true.
 */
struct ScratchpadGeom {
	float x, y, w, h;
	bool set;
};

/*
 * Single entry in a submap. KEY -> ACTION pairing with no modifier; the
 * submap is entered by another binding chord (e.g. M-s) and the entry
 * keys are matched bare while the submap is active.
 */
struct SubmapEntry {
	xkb_keysym_t keysym;
	struct Action action;
};

struct SubmapDef {
	char *name; /* owned; matches (submap NAME) action arg */
	struct SubmapEntry *entries;
	size_t n_entries;
};

/*
 * Named scratchpad definition: spawn command + matcher + first-spawn
 * geometry. The matcher (RULE_MATCH_APP_ID or RULE_MATCH_TITLE - parent
 * is not useful here) latches a freshly-spawned window to this pad on
 * the first manage cycle that sees its app_id / title; the scratch
 * action toggles that window between the visible workspace and the
 * scratch stash. Glob-matching uses fnmatch(3) like FloatRule.
 */
struct ScratchpadDef {
	char *name; /* owned; matches (scratchpad NAME) action arg */
	char *spawn; /* owned; argv to /bin/sh -c, or NULL */
	enum RuleMatchKind match_kind; /* APP_ID or TITLE */
	char *match_pattern; /* owned */
	bool glob;
	struct ScratchpadGeom geom;
};

/*
 * Borders. width=0 disables the borders entirely. Colors are stored as
 * 32-bit RRGGBBAA, scaled to the protocol's 32-bit channels at the call
 * site (with pre-multiplied alpha). `smart` mirrors xmonad's
 * XMonad.Layout.NoBorders.smartBorders: when only one window on the
 * workspace is a tile candidate, suppress its border so the lone tile
 * looks chromeless.
 */
struct BordersConfig {
	s32 width;
	u32 focused_rgba;
	u32 unfocused_rgba;
	u32 captured_rgba;
	bool captured_set;
	bool smart;
};

/*
 * Outer struts are subtracted from each output's rect before the layout
 * runs. `inner` is per-window padding inside the layout's chosen rect.
 * `smart` mirrors xmonad's XMonad.Layout.Spacing smart-spacing flag:
 * when the workspace has exactly one tile candidate, all gap insets
 * (outer struts + inner gap) are skipped so the lone tile fills the
 * output.
 */
struct GapsConfig {
	s32 top, bottom, left, right;
	s32 inner;
	bool smart;
};

struct InputConfig {
	bool repeat_set;
	s32 repeat_rate;
	s32 repeat_delay;
	char *cursor_theme;
	u32 cursor_size;
	s8 tap;
	s8 natural_scroll;
	s8 dwt;
	s8 middle_emulation;
	bool keymap_set;
	char *xkb[5];
};

enum XkbName {
	XKB_NAME_RULES,
	XKB_NAME_MODEL,
	XKB_NAME_LAYOUT,
	XKB_NAME_VARIANT,
	XKB_NAME_OPTIONS,
};

/*
 * Decoration mode tells SSD-capable clients whether to draw their own
 * titlebar / close button / etc. SSD = no client decorations (we do
 * not draw anything either, so windows end up bare like xmonad's).
 * CSD = client draws its own. Clients whose decoration_hint is
 * only_supports_csd ignore SSD and keep their CSD anyway. Default
 * SSD (DECORATION_SSD = 0, calloc-friendly) for the xmonad-like feel.
 */
enum DecorationMode {
	DECORATION_SSD,
	DECORATION_CSD,
};

struct Config {
	struct XkbBindingDef *xkb;
	size_t n_xkb;
	struct PointerBindingDef *ptr;
	size_t n_ptr;
	struct FloatRule *rules;
	size_t n_rules;
	struct ScratchpadDef *scratchpads;
	size_t n_scratchpads;
	struct SubmapDef *submaps;
	size_t n_submaps;
	struct BordersConfig borders;
	struct GapsConfig gaps;
	struct InputConfig input;
	/*
	 * The layout cycle for every workspace, in order. n_layout_kinds
	 * is at least 1 - the default config seeds a single TALL entry,
	 * and (defcfg (layouts)) without arguments errors out at parse
	 * time. (layout NAME) and (layouts NAME ...) both populate this
	 * list; the legacy single-arg form is just (layouts NAME).
	 */
	enum LayoutKind layout_kinds[NM_LAYOUTS_MAX];
	size_t n_layout_kinds;
	enum DecorationMode decoration;
};

/*
 * Parse a config source buffer. Returns NULL on error and writes a
 * "line:col: message" into errbuf if errbuf_len > 0.
 */
struct Config *config_parse(const char *src, char *errbuf, size_t errbuf_len);

/*
 * Read `path` and parse it. Returns NULL with an "errno: ..." or parser
 * diagnostic on failure.
 */
struct Config *config_load(const char *path, char *errbuf, size_t errbuf_len);

/* Built-in fallback config (matches the tinyrwm-derived defaults). */
struct Config *config_default(void);
int config_parse_action_text(const struct Config *c, const char *src,
			     struct Action *out, char *err, size_t err_len);

void config_free(struct Config *c);
void config_describe(FILE *out);

#endif /* NOMONAD_CONFIG_H */
