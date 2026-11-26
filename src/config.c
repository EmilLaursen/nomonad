/* SPDX-License-Identifier: 0BSD */
/*
 * AST -> Config translation. Token parsers (mods, keysym, button,
 * action) plus a small recursive walker over the top-level forms.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <river-window-management-v1-client-protocol.h>

#include "config.h"
#include "sexpr.h"

static void set_err(char *err, size_t err_len, const struct Sexpr *at,
		    const char *fmt, ...)
{
	va_list ap;
	int line;
	int col;
	int n;

	if (err == NULL || err_len == 0)
		return;
	line = at ? at->line : 0;
	col = at ? at->col : 0;
	n = snprintf(err, err_len, "line %d:%d: ", line, col);
	if (n < 0 || (size_t)n >= err_len)
		return;
	va_start(ap, fmt);
	vsnprintf(err + n, err_len - n, fmt, ap);
	va_end(ap);
}

static bool parse_int_range(const char *text, long lo, long hi, long *out)
{
	char *end;
	long v;

	if (text == NULL || *text == '\0')
		return false;
	errno = 0;
	v = strtol(text, &end, 10);
	if (errno != 0 || *end != '\0' || v < lo || v > hi)
		return false;
	*out = v;
	return true;
}

static int parse_mods(const struct Sexpr *s, u32 *out, char *err,
		      size_t err_len)
{
	u32 mask;

	if (s->kind == SEXPR_LIST) {
		if (s->u.list.n == 0) {
			*out = 0;
			return 0;
		}
		set_err(err, err_len, s,
			"modifier list must be empty (use atom for combos)");
		return -1;
	}
	if (s->kind != SEXPR_ATOM) {
		set_err(err, err_len, s, "expected modifier");
		return -1;
	}
	mask = 0;
	for (const char *p = s->u.atom; *p; p++) {
		u32 before = mask;

		switch (*p) {
		case 'M':
			mask |= RIVER_SEAT_V1_MODIFIERS_MOD4;
			break;
		case 'A':
			mask |= RIVER_SEAT_V1_MODIFIERS_MOD1;
			break;
		case 'C':
			mask |= RIVER_SEAT_V1_MODIFIERS_CTRL;
			break;
		case 'S':
			mask |= RIVER_SEAT_V1_MODIFIERS_SHIFT;
			break;
		case '-':
			continue;
		default:
			set_err(err, err_len, s,
				"unknown modifier '%c' in '%s'", *p, s->u.atom);
			return -1;
		}
		if (mask == before) {
			set_err(err, err_len, s,
				"repeated modifier '%c' in '%s'", *p,
				s->u.atom);
			return -1;
		}
	}
	if (mask == 0) {
		set_err(err, err_len, s,
			"'%s' names no modifier, use () for a bare key",
			s->u.atom);
		return -1;
	}
	*out = mask;
	return 0;
}

static int parse_keysym(const struct Sexpr *s, xkb_keysym_t *out, char *err,
			size_t err_len)
{
	xkb_keysym_t k;

	if (s->kind != SEXPR_ATOM) {
		set_err(err, err_len, s, "expected key name");
		return -1;
	}
	k = xkb_keysym_from_name(s->u.atom, XKB_KEYSYM_NO_FLAGS);
	if (k == XKB_KEY_NoSymbol) {
		set_err(err, err_len, s, "unknown key '%s'", s->u.atom);
		return -1;
	}
	*out = k;
	return 0;
}

static int parse_button(const struct Sexpr *s, u32 *out, char *err,
			size_t err_len)
{
	if (s->kind != SEXPR_ATOM) {
		set_err(err, err_len, s, "expected button name");
		return -1;
	}
	if (strcmp(s->u.atom, "left") == 0) {
		*out = BTN_LEFT;
		return 0;
	}
	if (strcmp(s->u.atom, "right") == 0) {
		*out = BTN_RIGHT;
		return 0;
	}
	if (strcmp(s->u.atom, "middle") == 0) {
		*out = BTN_MIDDLE;
		return 0;
	}
	set_err(err, err_len, s,
		"unknown button '%s' (expected left/right/middle)", s->u.atom);
	return -1;
}

enum ActionArg {
	ACTION_ARG_NONE,
	ACTION_ARG_STRING,
	ACTION_ARG_NAME,
	ACTION_ARG_INT,
	ACTION_ARG_EDGE_DELTA,
};

struct ActionSpec {
	const char *name;
	const char *arg_name;
	const char *arg_ref;
	const char *layout;
	const char *doc;
	enum ActionKind kind;
	enum ActionArg arg;
	size_t prefix_len;
	long lo;
	long hi;
};

#define ACT(n, k, d) { .name = n, .kind = k, .doc = d }
#define ACT_LAYOUT(n, l, p, d)        \
	{ .name = n,                  \
	  .kind = ACTION_LAYOUT_VERB, \
	  .layout = l,                \
	  .prefix_len = p,            \
	  .doc = d }
#define ACT_INT(n, k, a, l, h, d) \
	{ .name = n,              \
	  .kind = k,              \
	  .arg = ACTION_ARG_INT,  \
	  .arg_name = a,          \
	  .lo = l,                \
	  .hi = h,                \
	  .doc = d }
#define ACT_NAME(n, k, r, d)      \
	{ .name = n,              \
	  .kind = k,              \
	  .arg = ACTION_ARG_NAME, \
	  .arg_name = "name",     \
	  .arg_ref = r,           \
	  .doc = d }

static const struct ActionSpec action_specs[] = {
	{ .name = "spawn",
	  .kind = ACTION_SPAWN,
	  .arg = ACTION_ARG_STRING,
	  .arg_name = "command",
	  .doc = "Run COMMAND with /bin/sh -c, in its own systemd scope "
		 "when the user manager is reachable and NM_SPAWN_SCOPE is "
		 "not 0." },
	ACT("close", ACTION_CLOSE, "Ask the focused window to close."),
	ACT("focus-next", ACTION_FOCUS_NEXT,
	    "Focus the next window on the workspace, wrapping around."),
	ACT("focus-prev", ACTION_FOCUS_PREV,
	    "Focus the previous window on the workspace, wrapping around."),
	ACT("focus-master", ACTION_FOCUS_MASTER,
	    "Focus the first window on the workspace."),
	ACT("focus-last", ACTION_FOCUS_LAST,
	    "Focus the previously focused window. Repeating toggles "
	    "between the two."),
	ACT("focus-mru-next", ACTION_FOCUS_MRU_NEXT,
	    "Alt-tab: step to the next window in most recently used "
	    "order. The cycle ends when the binding's modifiers other "
	    "than Shift are released, or on any other focus change."),
	ACT("focus-mru-prev", ACTION_FOCUS_MRU_PREV,
	    "Alt-tab backwards: step to the previous window in most "
	    "recently used order."),
	ACT("swap-next", ACTION_SWAP_NEXT,
	    "Swap the focused window with the next one in the workspace "
	    "order."),
	ACT("swap-prev", ACTION_SWAP_PREV,
	    "Swap the focused window with the previous one in the "
	    "workspace order."),
	ACT("shrink-master", ACTION_SHRINK_MASTER,
	    "Shrink the master area by 3 percent. Tall, mirror-tall, "
	    "three-columns, two-pane and resizable-tall."),
	ACT("expand-master", ACTION_EXPAND_MASTER,
	    "Grow the master area by 3 percent. Same layouts as "
	    "shrink-master."),
	ACT("inc-master", ACTION_INC_MASTER,
	    "One more window in the master area. Same layouts as "
	    "shrink-master."),
	ACT("dec-master", ACTION_DEC_MASTER,
	    "One less window in the master area, not below 0."),
	ACT("toggle-float", ACTION_TOGGLE_FLOAT,
	    "Float the focused tile at its current geometry, or sink a "
	    "float back into the tiling."),
	ACT("toggle-fullscreen", ACTION_TOGGLE_FULLSCREEN,
	    "Make the focused window cover the usable area of its output, "
	    "bars stay visible. The client is not told it is fullscreen."),
	ACT("toggle-sticky", ACTION_TOGGLE_STICKY,
	    "Make the focused window sticky: it floats and follows its "
	    "output across workspace switches. Again to unstick."),
	ACT("toggle-scratchpad", ACTION_TOGGLE_SCRATCHPAD,
	    "Stash the focused window as the dynamic scratchpad, or bring "
	    "the stashed one back as a focused float."),
	ACT_NAME("scratchpad", ACTION_NAMED_SCRATCHPAD, "defscratchpads",
		 "Show or hide the named scratchpad, spawning it when it is "
		 "not running."),
	ACT_NAME("submap", ACTION_SUBMAP, "defsubmap",
		 "Enter the named submap. The next key runs one of its "
		 "bindings, any other key leaves the submap."),
	ACT_INT("workspace", ACTION_WORKSPACE, "workspace", 1,
		NM_DEFAULT_USER_WORKSPACES,
		"Show workspace N on the focused output."),
	ACT_INT("move-to-workspace", ACTION_MOVE_TO_WORKSPACE, "workspace", 1,
		NM_DEFAULT_USER_WORKSPACES,
		"Send the focused window to workspace N."),
	ACT_INT("move-to-workspace-follow", ACTION_MOVE_TO_WORKSPACE_FOLLOW,
		"workspace", 1, NM_DEFAULT_USER_WORKSPACES,
		"Send the focused window to workspace N and show it there."),
	ACT("next-workspace", ACTION_NEXT_WORKSPACE,
	    "Show the next workspace on the focused output, wrapping "
	    "around."),
	ACT("prev-workspace", ACTION_PREV_WORKSPACE,
	    "Show the previous workspace on the focused output, wrapping "
	    "around."),
	ACT_INT("focus-output", ACTION_FOCUS_OUTPUT, "output", 0,
		NM_MAX_OUTPUT_INDEX, "Focus output N, counting from 0."),
	ACT_INT("move-to-output", ACTION_MOVE_TO_OUTPUT, "output", 0,
		NM_MAX_OUTPUT_INDEX,
		"Send the focused window to the workspace shown on output N."),
	ACT("next-layout", ACTION_NEXT_LAYOUT,
	    "Switch the workspace to the next layout of the defcfg layouts "
	    "cycle."),
	ACT("prev-layout", ACTION_PREV_LAYOUT,
	    "Switch the workspace to the previous layout of the defcfg "
	    "layouts cycle."),
	ACT("move-window", ACTION_MOVE,
	    "Drag the window under the pointer. Meant for defmouse."),
	ACT("resize-window", ACTION_RESIZE,
	    "Resize the window under the pointer by its bottom right "
	    "corner. Meant for defmouse."),
	{ .name = "resize",
	  .kind = ACTION_RESIZE_DELTA,
	  .arg = ACTION_ARG_EDGE_DELTA,
	  .lo = -NM_MAX_RESIZE_DELTA,
	  .hi = NM_MAX_RESIZE_DELTA,
	  .doc = "Move EDGE of the focused float by DELTA pixels, positive "
		 "grows the window. Floats only." },
	ACT_LAYOUT("bsp-rotate", "bsp", 4,
		   "Flip the split holding the focused window between "
		   "side by side and stacked."),
	ACT_LAYOUT("bsp-swap", "bsp", 4,
		   "Swap the two halves of the split holding the focused "
		   "window."),
	ACT_LAYOUT("bsp-expand-l", "bsp", 4,
		   "Grow the focused window by moving its left edge left."),
	ACT_LAYOUT("bsp-expand-r", "bsp", 4,
		   "Grow the focused window by moving its right edge right."),
	ACT_LAYOUT("bsp-expand-u", "bsp", 4,
		   "Grow the focused window by moving its top edge up."),
	ACT_LAYOUT("bsp-expand-d", "bsp", 4,
		   "Grow the focused window by moving its bottom edge down."),
	ACT_LAYOUT("bsp-shrink-l", "bsp", 4,
		   "Shrink the focused window by moving its left edge right."),
	ACT_LAYOUT("bsp-shrink-r", "bsp", 4,
		   "Shrink the focused window by moving its right edge left."),
	ACT_LAYOUT("bsp-shrink-u", "bsp", 4,
		   "Shrink the focused window by moving its top edge down."),
	ACT_LAYOUT("bsp-shrink-d", "bsp", 4,
		   "Shrink the focused window by moving its bottom edge up."),
	ACT_LAYOUT("bsp-balance", "bsp", 4,
		   "Set every split of the tree to 50/50. Same as "
		   "bsp-equalize."),
	ACT_LAYOUT("bsp-equalize", "bsp", 4,
		   "Set every split of the tree to 50/50."),
	ACT_LAYOUT("bsp-balance-area", "bsp", 4,
		   "Set the splits so every window gets the same area."),
	ACT_LAYOUT("bsp-rotate-cw", "bsp", 4,
		   "Rotate the whole tree 90 degrees clockwise."),
	ACT_LAYOUT("bsp-rotate-ccw", "bsp", 4,
		   "Rotate the whole tree 90 degrees counterclockwise."),
	ACT_LAYOUT("rt-expand-cell", "resizable-tall", 3,
		   "Make the focused window 20 px taller, taken from its "
		   "column neighbour."),
	ACT_LAYOUT("rt-shrink-cell", "resizable-tall", 3,
		   "Make the focused window 20 px shorter, given to its "
		   "column neighbour."),
	ACT("reload-config", ACTION_RELOAD_CONFIG,
	    "Reload the config file. Edits are also picked up on their "
	    "own."),
	ACT("restart", ACTION_RESTART,
	    "Re-exec nomonad, keeping windows, workspaces, layouts and "
	    "focus."),
	ACT("exit", ACTION_EXIT, "End the river session."),
};

static const char *const resize_edges[] = { "left", "right", "up", "down" };

static const struct ActionSpec *action_spec_find(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(action_specs); i++)
		if (strcmp(name, action_specs[i].name) == 0)
			return &action_specs[i];
	return NULL;
}

static const char *action_name_text(const struct Sexpr *a)
{
	if (a->kind == SEXPR_ATOM)
		return a->u.atom;
	if (a->kind == SEXPR_STRING)
		return a->u.string;
	return NULL;
}

static bool resize_edge_valid(const char *edge)
{
	for (size_t i = 0; i < ARRAY_SIZE(resize_edges); i++)
		if (strcmp(edge, resize_edges[i]) == 0)
			return true;
	return false;
}

static int parse_action_set(const struct Sexpr *s, struct Action *out,
			    enum ActionKind kind, const char *arg, char *err,
			    size_t err_len)
{
	out->kind = kind;
	if (arg == NULL)
		return 0;
	out->arg = strdup(arg);
	if (out->arg == NULL) {
		set_err(err, err_len, s, "out of memory");
		out->kind = ACTION_NONE;
		return -1;
	}
	return 0;
}

static int parse_action_edge_delta(const struct Sexpr *s,
				   const struct ActionSpec *spec,
				   struct Action *out, char *err,
				   size_t err_len)
{
	const struct Sexpr *delta;
	const struct Sexpr *edge;
	char buf[64];
	long v;
	int n;

	if (s->u.list.n != 3) {
		set_err(err, err_len, s, "resize takes (resize EDGE DELTA)");
		return -1;
	}
	edge = s->u.list.items[1];
	delta = s->u.list.items[2];
	if (edge->kind != SEXPR_ATOM || !resize_edge_valid(edge->u.atom)) {
		set_err(err, err_len, edge,
			"resize edge must be left / right / up / down");
		return -1;
	}
	if (delta->kind != SEXPR_ATOM ||
	    !parse_int_range(delta->u.atom, spec->lo, spec->hi, &v)) {
		set_err(err, err_len, delta,
			"resize delta must be an integer in [%ld, %ld]",
			spec->lo, spec->hi);
		return -1;
	}
	n = snprintf(buf, sizeof(buf), "%s %s", edge->u.atom, delta->u.atom);
	if (n < 0 || (size_t)n >= sizeof(buf)) {
		set_err(err, err_len, s, "resize arg too long");
		return -1;
	}
	return parse_action_set(s, out, spec->kind, buf, err, err_len);
}

static int parse_action_list(const struct Sexpr *s,
			     const struct ActionSpec *spec, struct Action *out,
			     char *err, size_t err_len)
{
	const struct Sexpr *a;
	const char *text;
	long v;

	if (spec->arg == ACTION_ARG_EDGE_DELTA)
		return parse_action_edge_delta(s, spec, out, err, err_len);
	if (spec->arg == ACTION_ARG_STRING) {
		if (s->u.list.n != 2 ||
		    s->u.list.items[1]->kind != SEXPR_STRING) {
			set_err(err, err_len, s, "%s takes one string argument",
				spec->name);
			return -1;
		}
		return parse_action_set(s, out, spec->kind,
					s->u.list.items[1]->u.string, err,
					err_len);
	}
	if (s->u.list.n != 2) {
		set_err(err, err_len, s, "%s takes one argument", spec->name);
		return -1;
	}
	a = s->u.list.items[1];
	text = action_name_text(a);
	if (text == NULL) {
		set_err(err, err_len, a,
			"%s argument must be an atom or string", spec->name);
		return -1;
	}
	if (spec->arg == ACTION_ARG_INT &&
	    !parse_int_range(text, spec->lo, spec->hi, &v)) {
		set_err(err, err_len, a,
			"%s argument must be an integer in [%ld, %ld]",
			spec->name, spec->lo, spec->hi);
		return -1;
	}
	return parse_action_set(s, out, spec->kind, text, err, err_len);
}

static int parse_action(const struct Sexpr *s, struct Action *out, char *err,
			size_t err_len)
{
	const struct ActionSpec *spec;
	const char *name;

	out->kind = ACTION_NONE;
	out->arg = NULL;
	if (s->kind == SEXPR_ATOM) {
		name = s->u.atom;
		spec = action_spec_find(name);
		if (spec == NULL) {
			set_err(err, err_len, s, "unknown action '%s'", name);
			return -1;
		}
		if (spec->arg != ACTION_ARG_NONE) {
			set_err(err, err_len, s,
				"%s takes an argument, write (%s ...)", name,
				name);
			return -1;
		}
		return parse_action_set(s, out, spec->kind,
					spec->layout ? name + spec->prefix_len :
						       NULL,
					err, err_len);
	}
	if (s->kind == SEXPR_LIST) {
		if (s->u.list.n < 1 || s->u.list.items[0]->kind != SEXPR_ATOM) {
			set_err(err, err_len, s, "malformed action list");
			return -1;
		}
		name = s->u.list.items[0]->u.atom;
		spec = action_spec_find(name);
		if (spec == NULL) {
			set_err(err, err_len, s, "unknown action verb '%s'",
				name);
			return -1;
		}
		if (spec->arg == ACTION_ARG_NONE) {
			set_err(err, err_len, s,
				"%s takes no argument, write %s without parens",
				name, name);
			return -1;
		}
		return parse_action_list(s, spec, out, err, err_len);
	}
	set_err(err, err_len, s, "expected action atom or list");
	return -1;
}

static void describe_string(FILE *out, const char *s)
{
	fputc('"', out);
	for (; *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', out);
		fputc(*s, out);
	}
	fputc('"', out);
}

static void describe_action(FILE *out, const struct ActionSpec *spec)
{
	fprintf(out, "  (action %s", spec->name);
	switch (spec->arg) {
	case ACTION_ARG_NONE:
		break;
	case ACTION_ARG_STRING:
		fprintf(out, " :args ((%s string))", spec->arg_name);
		break;
	case ACTION_ARG_NAME:
		fprintf(out, " :args ((%s (name %s)))", spec->arg_name,
			spec->arg_ref);
		break;
	case ACTION_ARG_INT:
		fprintf(out, " :args ((%s (integer %ld %ld)))", spec->arg_name,
			spec->lo, spec->hi);
		break;
	case ACTION_ARG_EDGE_DELTA:
		fputs(" :args ((edge (one-of", out);
		for (size_t i = 0; i < ARRAY_SIZE(resize_edges); i++)
			fprintf(out, " %s", resize_edges[i]);
		fprintf(out, ")) (delta (integer %ld %ld)))", spec->lo,
			spec->hi);
		break;
	}
	if (spec->layout != NULL)
		fprintf(out, " :layout %s", spec->layout);
	fputs("\n    :doc ", out);
	describe_string(out, spec->doc);
	fputs(")", out);
}


/*
 * Reserve room for `extra` more entries in the array starting at *base
 * with current length n. Grows once per form (apply_defkeys, etc.)
 * rather than once per entry, turning the previous O(n^2) realloc-
 * every-append into O(n).
 */
static int reserve_array(void **base, size_t n, size_t extra, size_t elem_size,
			 char *err, size_t err_len)
{
	void *grown;

	if (extra == 0)
		return 0;
	grown = realloc(*base, (n + extra) * elem_size);
	if (grown == NULL) {
		set_err(err, err_len, NULL, "out of memory");
		return -1;
	}
	*base = grown;
	return 0;
}

static int apply_defkeys(struct Config *c, const struct Sexpr *form, char *err,
			 size_t err_len)
{
	size_t n_entries = (form->u.list.n > 0) ? form->u.list.n - 1 : 0;
	if (reserve_array((void **)&c->xkb, c->n_xkb, n_entries,
			  sizeof(*c->xkb), err, err_len) < 0)
		return -1;
	for (size_t i = 1; i < form->u.list.n; i++) {
		const struct Sexpr *e = form->u.list.items[i];
		struct XkbBindingDef d = { 0 };

		if (e->kind != SEXPR_LIST || e->u.list.n != 3) {
			set_err(err, err_len, e,
				"defkeys entry must be (mods key action)");
			return -1;
		}
		if (parse_mods(e->u.list.items[0], &d.mods, err, err_len) < 0)
			return -1;
		if (parse_keysym(e->u.list.items[1], &d.keysym, err, err_len) <
		    0)
			return -1;
		if (parse_action(e->u.list.items[2], &d.action, err, err_len) <
		    0)
			return -1;
		for (size_t j = 0; j < c->n_xkb; j++) {
			if (c->xkb[j].mods == d.mods &&
			    c->xkb[j].keysym == d.keysym) {
				free(d.action.arg);
				set_err(err, err_len, e,
					"key chord bound twice");
				return -1;
			}
		}
		c->xkb[c->n_xkb++] = d;
	}
	return 0;
}

static int apply_defmouse(struct Config *c, const struct Sexpr *form, char *err,
			  size_t err_len)
{
	size_t n_entries = (form->u.list.n > 0) ? form->u.list.n - 1 : 0;
	if (reserve_array((void **)&c->ptr, c->n_ptr, n_entries,
			  sizeof(*c->ptr), err, err_len) < 0)
		return -1;
	for (size_t i = 1; i < form->u.list.n; i++) {
		const struct Sexpr *e = form->u.list.items[i];
		struct PointerBindingDef d = { 0 };

		if (e->kind != SEXPR_LIST || e->u.list.n != 3) {
			set_err(err, err_len, e,
				"defmouse entry must be (mods button action)");
			return -1;
		}
		if (parse_mods(e->u.list.items[0], &d.mods, err, err_len) < 0)
			return -1;
		if (parse_button(e->u.list.items[1], &d.button, err, err_len) <
		    0)
			return -1;
		if (parse_action(e->u.list.items[2], &d.action, err, err_len) <
		    0)
			return -1;
		for (size_t j = 0; j < c->n_ptr; j++) {
			if (c->ptr[j].mods == d.mods &&
			    c->ptr[j].button == d.button) {
				free(d.action.arg);
				set_err(err, err_len, e,
					"pointer chord bound twice");
				return -1;
			}
		}
		c->ptr[c->n_ptr++] = d;
	}
	return 0;
}

static int parse_s32(const struct Sexpr *s, s32 *out, char *err, size_t err_len)
{
	const char *p;
	long acc;
	int sign;

	if (s->kind != SEXPR_ATOM) {
		set_err(err, err_len, s, "expected integer");
		return -1;
	}
	p = s->u.atom;
	sign = 1;
	if (*p == '-') {
		sign = -1;
		p++;
	} else if (*p == '+') {
		p++;
	}
	if (*p == '\0') {
		set_err(err, err_len, s, "bad integer '%s'", s->u.atom);
		return -1;
	}
	acc = 0;
	for (; *p; p++) {
		if (*p < '0' || *p > '9') {
			set_err(err, err_len, s, "bad integer '%s'", s->u.atom);
			return -1;
		}
		acc = acc * 10 + (*p - '0');
		if (acc > 0x7fffffffL) {
			set_err(err, err_len, s, "integer out of range");
			return -1;
		}
	}
	*out = (s32)(sign * acc);
	return 0;
}

/*
 * Parse "#RRGGBB" or "#RRGGBBAA" into a 32-bit RRGGBBAA value. Missing
 * alpha defaults to 0xff (fully opaque).
 */
static int parse_color(const struct Sexpr *s, u32 *out, char *err,
		       size_t err_len)
{
	const char *src = NULL;
	const char *hex;
	size_t len;
	u32 acc;

	if (s->kind == SEXPR_STRING)
		src = s->u.string;
	else if (s->kind == SEXPR_ATOM)
		src = s->u.atom;
	if (src == NULL || src[0] != '#') {
		set_err(err, err_len, s,
			"expected color like \"#RRGGBB\" or \"#RRGGBBAA\"");
		return -1;
	}
	hex = src + 1;
	len = strlen(hex);
	if (len != 6 && len != 8) {
		set_err(err, err_len, s,
			"color '%s' must have 6 or 8 hex digits", src);
		return -1;
	}
	acc = 0;
	for (size_t i = 0; i < len; i++) {
		char ch = hex[i];
		u32 nib;
		if (ch >= '0' && ch <= '9')
			nib = (u32)(ch - '0');
		else if (ch >= 'a' && ch <= 'f')
			nib = (u32)(ch - 'a' + 10);
		else if (ch >= 'A' && ch <= 'F')
			nib = (u32)(ch - 'A' + 10);
		else {
			set_err(err, err_len, s,
				"color '%s' has non-hex digit '%c'", src, ch);
			return -1;
		}
		acc = (acc << 4) | nib;
	}
	if (len == 6)
		acc = (acc << 8) | 0xff;
	*out = acc;
	return 0;
}

/*
 * Iterate over the `:keyword value :keyword value ...` tail of a form.
 * On each successful step, key and val are populated with the keyword
 * atom (including the leading ':') and the value sexpr. Returns 1 on
 * step, 0 on end, -1 on parse error (with err set). Shared by every
 * kv-form block (borders, gaps, defrules entries).
 */
struct KvIter {
	const struct Sexpr *form;
	size_t i;
};

static int kv_next(struct KvIter *it, const char **key,
		   const struct Sexpr **val, char *err, size_t err_len)
{
	const struct Sexpr *k;

	if (it->i >= it->form->u.list.n)
		return 0;
	k = it->form->u.list.items[it->i];
	if (k->kind != SEXPR_ATOM || k->u.atom[0] != ':') {
		set_err(err, err_len, k, "expected :keyword");
		return -1;
	}
	if (it->i + 1 >= it->form->u.list.n) {
		set_err(err, err_len, k, "keyword '%s' missing value",
			k->u.atom);
		return -1;
	}
	*key = k->u.atom;
	*val = it->form->u.list.items[it->i + 1];
	it->i += 2;
	return 1;
}

/*
 * Build the human-readable list of compiled-in layout names for
 * parser error messages. Walks the descriptor table once so the
 * message stays correct under per-layout compile-time opt-out.
 * Buffer is owned by the caller; truncation is fine - the message
 * only has to be informative.
 */
static void format_layout_list(char *buf, size_t buf_len)
{
	size_t i;
	size_t off = 0;

	if (buf_len == 0)
		return;
	buf[0] = '\0';
	for (i = 0; i < layout_descriptors_n; i++) {
		const char *sep;

		if (i == 0)
			sep = "";
		else if (i + 1 == layout_descriptors_n)
			sep = ", or ";
		else
			sep = ", ";
		off += (size_t)snprintf(buf + off, buf_len - off, "%s%s", sep,
					layout_descriptors[i].name);
		if (off >= buf_len)
			return;
	}
}

/*
 * Look up a layout name from the config dialect (kebab-case names
 * pulled from layout_descriptors[]). Error path lists the actual
 * compiled-in set so opt-out builds report exactly what the binary
 * accepts, not the shipping default.
 */
static int parse_layout_name(const struct Sexpr *atom,
			     enum LayoutKind *out, char *err, size_t err_len)
{
	const char *name = atom->u.atom;
	char list[160];
	size_t i;

	for (i = 0; i < layout_descriptors_n; i++) {
		if (strcmp(name, layout_descriptors[i].name) == 0) {
			*out = layout_descriptors[i].kind;
			return 0;
		}
	}
	format_layout_list(list, sizeof(list));
	set_err(err, err_len, atom, "unknown layout '%s' (expected %s)", name,
		list);
	return -1;
}

/*
 * (layout NAME): legacy single-entry form. Equivalent to
 * (layouts NAME) - both write a one-entry layout cycle that
 * cannot be advanced by next-layout / prev-layout. Kept so
 * existing configs do not break when cycling lands.
 */
static int apply_layout_block(struct Config *c, const struct Sexpr *form,
			      char *err, size_t err_len)
{
	enum LayoutKind kind;

	if (form->u.list.n != 2 || form->u.list.items[1]->kind != SEXPR_ATOM) {
		char list[160];

		format_layout_list(list, sizeof(list));
		set_err(err, err_len, form, "layout takes one atom (%s)", list);
		return -1;
	}
	if (parse_layout_name(form->u.list.items[1], &kind, err, err_len) < 0)
		return -1;
	c->layout_kinds[0] = kind;
	c->n_layout_kinds = 1;
	return 0;
}

/*
 * (layouts NAME...): multi-entry layout cycle. Each NAME is a layout
 * spelling parse_layout_name accepts; order is preserved so
 * next-layout / prev-layout walk it predictably. At least one NAME
 * is required - empty (layouts) is rejected to avoid silently
 * leaving every workspace without a tile-eligible layout, which
 * apply_layout would no-op on. Cap at NM_LAYOUTS_MAX entries.
 */
static int apply_layouts_block(struct Config *c, const struct Sexpr *form,
			       char *err, size_t err_len)
{
	size_t i;

	if (form->u.list.n < 2) {
		char list[160];

		format_layout_list(list, sizeof(list));
		set_err(err, err_len, form,
			"layouts takes one or more atoms (%s)", list);
		return -1;
	}
	if (form->u.list.n - 1 > NM_LAYOUTS_MAX) {
		set_err(err, err_len, form,
			"layouts list too long (max %d entries; recompile with -DNM_LAYOUTS_MAX=N to raise)",
			NM_LAYOUTS_MAX);
		return -1;
	}
	for (i = 1; i < form->u.list.n; i++) {
		const struct Sexpr *e = form->u.list.items[i];
		enum LayoutKind kind;

		if (e->kind != SEXPR_ATOM) {
			set_err(err, err_len, e,
				"layouts entries must be atoms");
			return -1;
		}
		if (parse_layout_name(e, &kind, err, err_len) < 0)
			return -1;
		c->layout_kinds[i - 1] = kind;
	}
	c->n_layout_kinds = form->u.list.n - 1;
	return 0;
}

static int apply_decorations_block(struct Config *c, const struct Sexpr *form,
				   char *err, size_t err_len)
{
	const char *name;

	if (form->u.list.n != 2 || form->u.list.items[1]->kind != SEXPR_ATOM) {
		set_err(err, err_len, form,
			"decorations takes one atom (ssd or csd)");
		return -1;
	}
	name = form->u.list.items[1]->u.atom;
	if (strcmp(name, "ssd") == 0)
		c->decoration = DECORATION_SSD;
	else if (strcmp(name, "csd") == 0)
		c->decoration = DECORATION_CSD;
	else {
		set_err(err, err_len, form->u.list.items[1],
			"unknown decorations '%s' (expected ssd or csd)", name);
		return -1;
	}
	return 0;
}

/* Caller has already reserved space via reserve_array. */
static void append_rule(struct Config *c, struct FloatRule r)
{
	c->rules[c->n_rules++] = r;
}

/*
 * Parse a matcher list like (app-id "Pavucontrol") or (parent) or
 * (kind dialog) into a FloatRule with kind/pattern set; center is
 * left untouched here.
 */
static int parse_matcher(const struct Sexpr *s, struct FloatRule *out,
			 char *err, size_t err_len)
{
	const char *verb;

	if (s->kind != SEXPR_LIST || s->u.list.n == 0 ||
	    s->u.list.items[0]->kind != SEXPR_ATOM) {
		set_err(err, err_len, s,
			"matcher must be (app-id ...), (title ...), "
			"(parent), or (kind dialog)");
		return -1;
	}
	verb = s->u.list.items[0]->u.atom;
	if (strcmp(verb, "app-id") == 0 || strcmp(verb, "title") == 0) {
		if (s->u.list.n != 2 ||
		    s->u.list.items[1]->kind != SEXPR_STRING) {
			set_err(err, err_len, s, "%s takes one string argument",
				verb);
			return -1;
		}
		out->kind = (strcmp(verb, "app-id") == 0) ? RULE_MATCH_APP_ID :
							    RULE_MATCH_TITLE;
		out->pattern = strdup(s->u.list.items[1]->u.string);
		if (out->pattern == NULL) {
			set_err(err, err_len, s, "out of memory");
			return -1;
		}
		return 0;
	}
	if (strcmp(verb, "parent") == 0) {
		if (s->u.list.n != 1) {
			set_err(err, err_len, s, "parent takes no arguments");
			return -1;
		}
		out->kind = RULE_MATCH_PARENT;
		out->pattern = NULL;
		return 0;
	}
	if (strcmp(verb, "kind") == 0) {
		if (s->u.list.n != 2 ||
		    s->u.list.items[1]->kind != SEXPR_ATOM ||
		    strcmp(s->u.list.items[1]->u.atom, "dialog") != 0) {
			set_err(err, err_len, s,
				"only (kind dialog) is supported");
			return -1;
		}
		out->kind = RULE_MATCH_PARENT;
		out->pattern = NULL;
		return 0;
	}
	set_err(err, err_len, s, "unknown matcher verb '%s'", verb);
	return -1;
}

static int parse_bool_atom(const struct Sexpr *v, bool *out, char *err,
			   size_t err_len, const char *kname)
{
	if (v->kind != SEXPR_ATOM || (strcmp(v->u.atom, "true") != 0 &&
				      strcmp(v->u.atom, "false") != 0)) {
		set_err(err, err_len, v, "%s expects true or false", kname);
		return -1;
	}
	*out = (strcmp(v->u.atom, "true") == 0);
	return 0;
}

static int parse_s32_range(const struct Sexpr *v, s32 *out, s32 lo, s32 hi,
			   const char *kname, char *err, size_t err_len)
{
	if (parse_s32(v, out, err, err_len) < 0)
		return -1;
	if (*out < lo || *out > hi) {
		set_err(err, err_len, v, "%s must be in [%d, %d]", kname, lo,
			hi);
		return -1;
	}
	return 0;
}

enum CfgType {
	CFG_INT,
	CFG_U32,
	CFG_BOOL,
	CFG_TRISTATE,
	CFG_COLOR,
	CFG_TEXT,
};

struct CfgKey {
	const char *name;
	const char *doc;
	enum CfgType type;
	size_t offset;
	size_t mark;
	s32 lo;
	s32 hi;
	bool required;
};

enum CfgShape {
	CFG_SHAPE_KV,
	CFG_SHAPE_LAYOUT,
	CFG_SHAPE_LAYOUTS,
	CFG_SHAPE_DECORATIONS,
};

struct CfgBlock {
	const char *name;
	const char *doc;
	const struct CfgKey *keys;
	size_t n_keys;
	enum CfgShape shape;
	void (*begin)(struct Config *c);
	int (*finish)(struct Config *c, const struct Sexpr *form, char *err,
		      size_t err_len);
	int (*apply)(struct Config *c, const struct Sexpr *form, char *err,
		     size_t err_len);
};

#define CFG_OFF(field) offsetof(struct Config, field)
#define CFG_INSET(n, f, d)      \
	{ .name = n,            \
	  .type = CFG_INT,      \
	  .offset = CFG_OFF(f), \
	  .lo = 0,              \
	  .hi = NM_MAX_INSET,   \
	  .doc = d }
#define CFG_KEY(n, t, f, d) \
	{ .name = n, .type = t, .offset = CFG_OFF(f), .doc = d }
#define CFG_XKB(n, i, d) CFG_KEY(n, CFG_TEXT, input.xkb[i], d)

static const struct CfgKey borders_keys[] = {
	CFG_INSET(":width", borders.width,
		  "Border width in pixels, 0 turns borders off."),
	CFG_KEY(":focused", CFG_COLOR, borders.focused_rgba,
		"Border color of the focused window."),
	CFG_KEY(":unfocused", CFG_COLOR, borders.unfocused_rgba,
		"Border color of the other windows."),
	{ .name = ":captured",
	  .type = CFG_COLOR,
	  .offset = CFG_OFF(borders.captured_rgba),
	  .mark = CFG_OFF(borders.captured_set) + 1,
	  .doc = "Border color of a window that is being screen captured, "
		 "or that sits on a captured output. Unset, captured windows "
		 "keep their normal color." },
	CFG_KEY(":smart", CFG_BOOL, borders.smart,
		"No border when the workspace has a single tile."),
};

static const struct CfgKey gaps_keys[] = {
	CFG_INSET(":top", gaps.top,
		  "Pixels kept free at the top of each output."),
	CFG_INSET(":bottom", gaps.bottom,
		  "Pixels kept free at the bottom of each output."),
	CFG_INSET(":left", gaps.left,
		  "Pixels kept free at the left of each output."),
	CFG_INSET(":right", gaps.right,
		  "Pixels kept free at the right of each output."),
	CFG_INSET(":inner", gaps.inner, "Padding in pixels around each tile."),
	CFG_KEY(":smart", CFG_BOOL, gaps.smart,
		"No outer or inner gaps when the workspace has a single "
		"tile."),
};

static const struct CfgKey repeat_keys[] = {
	{ .name = ":rate",
	  .type = CFG_INT,
	  .offset = CFG_OFF(input.repeat_rate),
	  .lo = 0,
	  .hi = 1000,
	  .doc = "Key repeats per second, 0 turns repeat off." },
	{ .name = ":delay",
	  .type = CFG_INT,
	  .offset = CFG_OFF(input.repeat_delay),
	  .lo = 0,
	  .hi = 10000,
	  .doc = "Milliseconds a key is held before it repeats." },
};

static const struct CfgKey cursor_keys[] = {
	{ .name = ":theme",
	  .type = CFG_TEXT,
	  .offset = CFG_OFF(input.cursor_theme),
	  .required = true,
	  .doc = "Xcursor theme name, also exported as XCURSOR_THEME." },
	{ .name = ":size",
	  .type = CFG_U32,
	  .offset = CFG_OFF(input.cursor_size),
	  .lo = 1,
	  .hi = 512,
	  .doc = "Cursor size in pixels, also exported as XCURSOR_SIZE." },
};

static const struct CfgKey touchpad_keys[] = {
	CFG_KEY(":tap", CFG_TRISTATE, input.tap, "Tap to click."),
	CFG_KEY(":natural-scroll", CFG_TRISTATE, input.natural_scroll,
		"Content follows the fingers when scrolling."),
	CFG_KEY(":dwt", CFG_TRISTATE, input.dwt,
		"Disable the touchpad while typing."),
	CFG_KEY(":middle-emulation", CFG_TRISTATE, input.middle_emulation,
		"Pressing left and right together is a middle click."),
};

static const struct CfgKey keymap_keys[] = {
	CFG_XKB(":rules", XKB_NAME_RULES, "XKB rules, usually evdev."),
	CFG_XKB(":model", XKB_NAME_MODEL, "XKB model, e.g. pc105."),
	CFG_XKB(":layout", XKB_NAME_LAYOUT,
		"XKB layouts, comma separated, e.g. \"us,dk\"."),
	CFG_XKB(":variant", XKB_NAME_VARIANT,
		"XKB variants, one per layout, comma separated."),
	CFG_XKB(":options", XKB_NAME_OPTIONS,
		"XKB options, e.g. \"ctrl:nocaps,grp:alt_shift_toggle\"."),
};

static void repeat_begin(struct Config *c)
{
	c->input.repeat_set = true;
	c->input.repeat_rate = 25;
	c->input.repeat_delay = 600;
}

static int keymap_finish(struct Config *c, const struct Sexpr *form, char *err,
			 size_t err_len)
{
	struct xkb_rule_names names = { 0 };
	struct xkb_context *ctx;
	struct xkb_keymap *km;

	names.rules = c->input.xkb[XKB_NAME_RULES];
	names.model = c->input.xkb[XKB_NAME_MODEL];
	names.layout = c->input.xkb[XKB_NAME_LAYOUT];
	names.variant = c->input.xkb[XKB_NAME_VARIANT];
	names.options = c->input.xkb[XKB_NAME_OPTIONS];
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx == NULL) {
		set_err(err, err_len, form, "cannot create an xkb context");
		return -1;
	}
	xkb_context_set_log_level(ctx, XKB_LOG_LEVEL_CRITICAL);
	km = xkb_keymap_new_from_names(ctx, &names,
				       XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(ctx);
	if (km == NULL) {
		set_err(err, err_len, form,
			"keymap does not compile, check :layout / :variant / "
			":options");
		return -1;
	}
	xkb_keymap_unref(km);
	c->input.keymap_set = true;
	return 0;
}

#define CFG_KV(n, k, d) .name = n, .keys = k, .n_keys = ARRAY_SIZE(k), .doc = d

static const struct CfgBlock cfg_blocks[] = {
	{ CFG_KV("borders", borders_keys,
		 "Window borders. Colors are \"#RRGGBB\" or \"#RRGGBBAA\".") },
	{ CFG_KV("gaps", gaps_keys,
		 "Outer gaps at the output edges and inner gaps around "
		 "tiles.") },
	{ .name = "layout",
	  .shape = CFG_SHAPE_LAYOUT,
	  .apply = apply_layout_block,
	  .doc = "A single layout for every workspace, same as (layouts "
		 "NAME)." },
	{ .name = "layouts",
	  .shape = CFG_SHAPE_LAYOUTS,
	  .apply = apply_layouts_block,
	  .doc = "The layout cycle of every workspace, in order. The first "
		 "is used at start, next-layout and prev-layout walk the "
		 "rest." },
	{ .name = "decorations",
	  .shape = CFG_SHAPE_DECORATIONS,
	  .apply = apply_decorations_block,
	  .doc = "ssd asks clients to drop their titlebars, csd lets them "
		 "draw their own. Clients that only support csd keep "
		 "theirs." },
	{ CFG_KV("repeat", repeat_keys,
		 "Keyboard repeat. Without this block river's defaults "
		 "apply."),
	  .begin = repeat_begin },
	{ CFG_KV("cursor", cursor_keys, "Cursor theme and size.") },
	{ CFG_KV("touchpad", touchpad_keys,
		 "Touchpad settings for every touchpad. A key that is not "
		 "given keeps the device default.") },
	{ CFG_KV("keymap", keymap_keys,
		 "XKB keymap for every keyboard. Keys not given use the "
		 "xkbcommon defaults. The keymap must compile."),
	  .finish = keymap_finish },
};

static void *cfg_field(struct Config *c, size_t offset)
{
	return (char *)c + offset;
}

static int cfg_set_key(struct Config *c, const struct CfgKey *k,
		       const struct Sexpr *v, char *err, size_t err_len)
{
	const char *text;
	char **slot;
	s32 n;
	bool b;

	switch (k->type) {
	case CFG_INT:
		return parse_s32_range(v, cfg_field(c, k->offset), k->lo, k->hi,
				       k->name, err, err_len);
	case CFG_U32:
		if (parse_s32_range(v, &n, k->lo, k->hi, k->name, err,
				    err_len) < 0)
			return -1;
		*(u32 *)cfg_field(c, k->offset) = (u32)n;
		return 0;
	case CFG_BOOL:
		return parse_bool_atom(v, cfg_field(c, k->offset), err, err_len,
				       k->name);
	case CFG_TRISTATE:
		if (parse_bool_atom(v, &b, err, err_len, k->name) < 0)
			return -1;
		*(s8 *)cfg_field(c, k->offset) = b ? 1 : 0;
		return 0;
	case CFG_COLOR:
		return parse_color(v, cfg_field(c, k->offset), err, err_len);
	case CFG_TEXT:
		if (v->kind != SEXPR_STRING && v->kind != SEXPR_ATOM) {
			set_err(err, err_len, v, "%s expects a string",
				k->name);
			return -1;
		}
		text = (v->kind == SEXPR_STRING) ? v->u.string : v->u.atom;
		slot = cfg_field(c, k->offset);
		free(*slot);
		*slot = strdup(text);
		if (*slot == NULL) {
			set_err(err, err_len, v, "out of memory");
			return -1;
		}
		return 0;
	}
	return -1;
}

static int apply_kv_block(struct Config *c, const struct CfgBlock *blk,
			  const struct Sexpr *form, char *err, size_t err_len)
{
	struct KvIter it = { .form = form, .i = 1 };
	const struct CfgKey *k;
	const struct Sexpr *v;
	const char *key;
	size_t i;
	int r;

	if (blk->begin != NULL)
		blk->begin(c);
	while ((r = kv_next(&it, &key, &v, err, err_len)) == 1) {
		for (i = 0; i < blk->n_keys; i++)
			if (strcmp(key, blk->keys[i].name) == 0)
				break;
		if (i == blk->n_keys) {
			set_err(err, err_len, v, "unknown %s keyword '%s'",
				blk->name, key);
			return -1;
		}
		k = &blk->keys[i];
		if (cfg_set_key(c, k, v, err, err_len) < 0)
			return -1;
		if (k->mark != 0)
			*(bool *)cfg_field(c, k->mark - 1) = true;
	}
	if (r < 0)
		return -1;
	for (i = 0; i < blk->n_keys; i++) {
		k = &blk->keys[i];
		if (k->required && *(char **)cfg_field(c, k->offset) == NULL) {
			set_err(err, err_len, form, "%s block needs %s",
				blk->name, k->name);
			return -1;
		}
	}
	if (blk->finish != NULL)
		return blk->finish(c, form, err, err_len);
	return 0;
}

static int apply_defcfg(struct Config *c, const struct Sexpr *form, char *err,
			size_t err_len)
{
	const struct CfgBlock *blk;
	const struct Sexpr *e;
	const char *verb;
	size_t b;

	for (size_t i = 1; i < form->u.list.n; i++) {
		e = form->u.list.items[i];
		if (e->kind != SEXPR_LIST || e->u.list.n == 0 ||
		    e->u.list.items[0]->kind != SEXPR_ATOM) {
			set_err(err, err_len, e,
				"defcfg entry must be (block :keyword value ...)");
			return -1;
		}
		verb = e->u.list.items[0]->u.atom;
		for (b = 0; b < ARRAY_SIZE(cfg_blocks); b++)
			if (strcmp(verb, cfg_blocks[b].name) == 0)
				break;
		if (b == ARRAY_SIZE(cfg_blocks)) {
			set_err(err, err_len, e, "unknown defcfg block '%s'",
				verb);
			return -1;
		}
		blk = &cfg_blocks[b];
		if (blk->shape != CFG_SHAPE_KV) {
			if (blk->apply(c, e, err, err_len) < 0)
				return -1;
		} else if (apply_kv_block(c, blk, e, err, err_len) < 0) {
			return -1;
		}
	}
	return 0;
}

static const char *layout_kind_name(enum LayoutKind kind)
{
	for (size_t i = 0; i < layout_descriptors_n; i++)
		if (layout_descriptors[i].kind == kind)
			return layout_descriptors[i].name;
	return "?";
}

static void describe_layout_names(FILE *out)
{
	fputs("(one-of", out);
	for (size_t i = 0; i < layout_descriptors_n; i++)
		fprintf(out, " %s", layout_descriptors[i].name);
	fputs(")", out);
}

static void describe_bool(FILE *out, bool b)
{
	fputs(b ? "true" : "false", out);
}

static void describe_key(FILE *out, const struct CfgKey *k, struct Config *d)
{
	void *f = cfg_field(d, k->offset);
	bool has_default;

	fprintf(out, "      (%s ", k->name);
	switch (k->type) {
	case CFG_INT:
	case CFG_U32:
		fprintf(out, "(integer %d %d)", k->lo, k->hi);
		break;
	case CFG_BOOL:
	case CFG_TRISTATE:
		fputs("bool", out);
		break;
	case CFG_COLOR:
		fputs("color", out);
		break;
	case CFG_TEXT:
		fputs("text", out);
		break;
	}
	if (k->required)
		fputs(" :required true", out);
	has_default = true;
	if (k->mark != 0 && !*(bool *)cfg_field(d, k->mark - 1))
		has_default = false;
	if (k->type == CFG_TRISTATE && *(s8 *)f < 0)
		has_default = false;
	if (k->type == CFG_TEXT && *(char **)f == NULL)
		has_default = false;
	if (has_default) {
		fputs(" :default ", out);
		switch (k->type) {
		case CFG_INT:
			fprintf(out, "%d", *(s32 *)f);
			break;
		case CFG_U32:
			fprintf(out, "%u", (unsigned)*(u32 *)f);
			break;
		case CFG_BOOL:
			describe_bool(out, *(bool *)f);
			break;
		case CFG_TRISTATE:
			describe_bool(out, *(s8 *)f != 0);
			break;
		case CFG_COLOR:
			fprintf(out, "\"#%08x\"", (unsigned)*(u32 *)f);
			break;
		case CFG_TEXT:
			describe_string(out, *(char **)f);
			break;
		}
	}
	fputs("\n        :doc ", out);
	describe_string(out, k->doc);
	fputs(")", out);
}

static void describe_block(FILE *out, const struct CfgBlock *blk,
			   struct Config *d)
{
	fprintf(out, "  (defcfg %s", blk->name);
	switch (blk->shape) {
	case CFG_SHAPE_KV:
		fputs(" :keys (\n", out);
		for (size_t i = 0; i < blk->n_keys; i++) {
			describe_key(out, &blk->keys[i], d);
			fputs(i + 1 < blk->n_keys ? "\n" : ")", out);
		}
		break;
	case CFG_SHAPE_LAYOUT:
		fputs(" :args ((name ", out);
		describe_layout_names(out);
		fprintf(out, ")) :default %s",
			layout_kind_name(d->layout_kinds[0]));
		break;
	case CFG_SHAPE_LAYOUTS:
		fputs(" :args ((names (rest-of ", out);
		describe_layout_names(out);
		fprintf(out, " 1 %d))) :default (", NM_LAYOUTS_MAX);
		for (size_t i = 0; i < d->n_layout_kinds; i++)
			fprintf(out, "%s%s", i ? " " : "",
				layout_kind_name(d->layout_kinds[i]));
		fputs(")", out);
		break;
	case CFG_SHAPE_DECORATIONS:
		fprintf(out, " :args ((mode (one-of ssd csd))) :default %s",
			d->decoration == DECORATION_CSD ? "csd" : "ssd");
		break;
	}
	fputs("\n    :doc ", out);
	describe_string(out, blk->doc);
	fputs(")", out);
}

/*
 * defrules entry: (float MATCHER [:keyword value]...)
 *                 (ignore MATCHER [:keyword value]...)
 * Keywords: :center true|false (float only), :glob true|false (both).
 */
static int apply_rule_entry(struct Config *c, const struct Sexpr *e,
			    enum RuleVerb verb, char *err, size_t err_len)
{
	const char *vname = (verb == RULE_VERB_IGNORE) ? "ignore" : "float";
	const struct Sexpr *v;
	struct FloatRule r = { 0 };
	struct KvIter it = { .form = e, .i = 2 };
	const char *key;
	int kr;

	if (e->u.list.n < 2) {
		set_err(err, err_len, e, "%s entry needs a matcher", vname);
		return -1;
	}
	r.verb = verb;
	if (parse_matcher(e->u.list.items[1], &r, err, err_len) < 0)
		return -1;

	while ((kr = kv_next(&it, &key, &v, err, err_len)) == 1) {
		if (strcmp(key, ":center") == 0) {
			if (verb == RULE_VERB_IGNORE) {
				free(r.pattern);
				set_err(err, err_len, v,
					":center is only valid on (float ...)");
				return -1;
			}
			if (parse_bool_atom(v, &r.center, err, err_len,
					    ":center") < 0) {
				free(r.pattern);
				return -1;
			}
		} else if (strcmp(key, ":glob") == 0) {
			if (parse_bool_atom(v, &r.glob, err, err_len, ":glob") <
			    0) {
				free(r.pattern);
				return -1;
			}
		} else {
			free(r.pattern);
			set_err(err, err_len, v, "unknown keyword '%s'", key);
			return -1;
		}
	}
	if (kr < 0) {
		free(r.pattern);
		return -1;
	}
	append_rule(c, r);
	return 0;
}

/*
 * Parse a 0..1 fraction from an atom like "0.1" or "1". The grammar is
 * intentionally narrow: optional leading digits, optional dot, optional
 * trailing digits. Negative numbers and exponents are rejected. Used
 * only by ScratchpadGeom which strictly wants 0..1 inclusive.
 */
static int parse_unit_fraction(const struct Sexpr *s, float *out, char *err,
			       size_t err_len)
{
	double whole;
	double frac;
	double scale;
	const char *p;
	int digits;

	if (s->kind != SEXPR_ATOM) {
		set_err(err, err_len, s, "expected number 0..1");
		return -1;
	}
	p = s->u.atom;
	whole = 0.0;
	digits = 0;
	while (*p >= '0' && *p <= '9') {
		whole = whole * 10.0 + (*p - '0');
		p++;
		digits++;
	}
	frac = 0.0;
	scale = 1.0;
	if (*p == '.') {
		p++;
		if (*p < '0' || *p > '9')
			digits = -1;
		while (*p >= '0' && *p <= '9') {
			if (digits >= 0)
				digits++;
			scale *= 10.0;
			frac = frac * 10.0 + (*p - '0');
			p++;
		}
	}
	if (*p != '\0' || digits <= 0) {
		set_err(err, err_len, s, "bad fraction '%s'", s->u.atom);
		return -1;
	}
	*out = (float)(whole + frac / scale);
	if (*out < 0.0f || *out > 1.0f) {
		set_err(err, err_len, s, "fraction '%s' must be 0..1", s->u.atom);
		return -1;
	}
	return 0;
}

static int parse_geometry(const struct Sexpr *v, struct ScratchpadGeom *out,
			  char *err, size_t err_len)
{
	if (v->kind != SEXPR_LIST || v->u.list.n != 4) {
		set_err(err, err_len, v,
			":geometry takes a 4-element list (x y w h)");
		return -1;
	}
	if (parse_unit_fraction(v->u.list.items[0], &out->x, err, err_len) < 0)
		return -1;
	if (parse_unit_fraction(v->u.list.items[1], &out->y, err, err_len) < 0)
		return -1;
	if (parse_unit_fraction(v->u.list.items[2], &out->w, err, err_len) < 0)
		return -1;
	if (parse_unit_fraction(v->u.list.items[3], &out->h, err, err_len) < 0)
		return -1;
	if (out->w <= 0.0f || out->h <= 0.0f) {
		set_err(err, err_len, v, ":geometry width and height must be > 0");
		return -1;
	}
	if (out->x + out->w > 1.0f || out->y + out->h > 1.0f) {
		set_err(err, err_len, v,
			":geometry must fit the output, x + w and y + h at most 1");
		return -1;
	}
	out->set = true;
	return 0;
}

/*
 * defscratchpads entry: (NAME (spawn "cmd") MATCHER [:keyword value]...)
 * where MATCHER is (app-id "...") or (title "...") and supported
 * keywords are :geometry (X Y W H) and :glob true|false.
 */
static int apply_scratchpad_entry(struct Config *c, const struct Sexpr *e,
				  char *err, size_t err_len)
{
	const struct Sexpr *name_node;
	const struct Sexpr *spawn_node;
	const struct Sexpr *match_node;
	struct ScratchpadDef d = { 0 };
	struct FloatRule tmp = { 0 };
	struct KvIter it;
	const char *key;
	const struct Sexpr *v;
	int kr;

	if (e->kind != SEXPR_LIST || e->u.list.n < 3) {
		set_err(err, err_len, e,
			"scratchpad entry must be (NAME (spawn ...) MATCHER ...)");
		return -1;
	}
	name_node = e->u.list.items[0];
	spawn_node = e->u.list.items[1];
	match_node = e->u.list.items[2];

	if (name_node->kind != SEXPR_ATOM &&
	    name_node->kind != SEXPR_STRING) {
		set_err(err, err_len, name_node,
			"scratchpad name must be an atom or string");
		return -1;
	}
	d.name = strdup((name_node->kind == SEXPR_ATOM) ? name_node->u.atom :
							  name_node->u.string);
	if (d.name == NULL) {
		set_err(err, err_len, name_node, "out of memory");
		return -1;
	}

	if (spawn_node->kind != SEXPR_LIST || spawn_node->u.list.n != 2 ||
	    spawn_node->u.list.items[0]->kind != SEXPR_ATOM ||
	    strcmp(spawn_node->u.list.items[0]->u.atom, "spawn") != 0 ||
	    spawn_node->u.list.items[1]->kind != SEXPR_STRING) {
		free(d.name);
		set_err(err, err_len, spawn_node,
			"scratchpad needs a (spawn \"command\") form");
		return -1;
	}
	d.spawn = strdup(spawn_node->u.list.items[1]->u.string);
	if (d.spawn == NULL) {
		free(d.name);
		set_err(err, err_len, spawn_node, "out of memory");
		return -1;
	}

	if (parse_matcher(match_node, &tmp, err, err_len) < 0) {
		free(d.spawn);
		free(d.name);
		return -1;
	}
	if (tmp.kind != RULE_MATCH_APP_ID && tmp.kind != RULE_MATCH_TITLE) {
		free(tmp.pattern);
		free(d.spawn);
		free(d.name);
		set_err(err, err_len, match_node,
			"scratchpad matcher must be (app-id ...) or (title ...)");
		return -1;
	}
	d.match_kind = tmp.kind;
	d.match_pattern = tmp.pattern;

	it.form = e;
	it.i = 3;
	while ((kr = kv_next(&it, &key, &v, err, err_len)) == 1) {
		if (strcmp(key, ":geometry") == 0) {
			if (parse_geometry(v, &d.geom, err, err_len) < 0)
				goto fail;
		} else if (strcmp(key, ":glob") == 0) {
			if (parse_bool_atom(v, &d.glob, err, err_len, ":glob") < 0)
				goto fail;
		} else {
			set_err(err, err_len, v, "unknown keyword '%s'", key);
			goto fail;
		}
	}
	if (kr < 0)
		goto fail;

	for (size_t j = 0; j < c->n_scratchpads; j++) {
		if (strcmp(c->scratchpads[j].name, d.name) == 0) {
			set_err(err, err_len, e,
				"defscratchpads '%s' redefined", d.name);
			goto fail;
		}
	}
	c->scratchpads[c->n_scratchpads++] = d;
	return 0;

fail:
	free(d.match_pattern);
	free(d.spawn);
	free(d.name);
	return -1;
}

static int apply_defscratchpads(struct Config *c, const struct Sexpr *form,
				char *err, size_t err_len)
{
	size_t n_entries = (form->u.list.n > 0) ? form->u.list.n - 1 : 0;
	if (reserve_array((void **)&c->scratchpads, c->n_scratchpads, n_entries,
			  sizeof(*c->scratchpads), err, err_len) < 0)
		return -1;
	for (size_t i = 1; i < form->u.list.n; i++) {
		if (apply_scratchpad_entry(c, form->u.list.items[i], err,
					   err_len) < 0)
			return -1;
	}
	return 0;
}

/*
 * defsubmap entry: (KEY ACTION). KEY is an xkbcommon keysym name (no
 * modifiers - the submap is entered via a regular binding chord, the
 * entry keys are matched bare while it is active). ACTION is parsed
 * by the same parse_action used for defkeys, so any action verb
 * (spawn, focus-next, scratchpad, etc.) works inside a submap.
 */
static int apply_submap_entry(struct SubmapDef *def, const struct Sexpr *e,
			      char *err, size_t err_len)
{
	struct SubmapEntry *slot;
	struct SubmapEntry tmp = { 0 };

	if (e->kind != SEXPR_LIST || e->u.list.n != 2) {
		set_err(err, err_len, e, "submap entry must be (KEY ACTION)");
		return -1;
	}
	if (parse_keysym(e->u.list.items[0], &tmp.keysym, err, err_len) < 0)
		return -1;
	if (parse_action(e->u.list.items[1], &tmp.action, err, err_len) < 0)
		return -1;
	for (size_t i = 0; i < def->n_entries; i++) {
		if (def->entries[i].keysym == tmp.keysym) {
			free(tmp.action.arg);
			set_err(err, err_len, e, "submap key bound twice");
			return -1;
		}
	}

	slot = realloc(def->entries,
		       (def->n_entries + 1) * sizeof(*def->entries));
	if (slot == NULL) {
		free(tmp.action.arg);
		set_err(err, err_len, e, "out of memory");
		return -1;
	}
	def->entries = slot;
	def->entries[def->n_entries++] = tmp;
	return 0;
}

static int apply_defsubmap(struct Config *c, const struct Sexpr *form,
			   char *err, size_t err_len)
{
	const struct Sexpr *name_node;
	struct SubmapDef *grown;
	struct SubmapDef def = { 0 };
	const char *raw;

	if (form->u.list.n < 2) {
		set_err(err, err_len, form,
			"defsubmap takes a name plus zero or more (KEY ACTION) entries");
		return -1;
	}
	name_node = form->u.list.items[1];
	if (name_node->kind != SEXPR_ATOM && name_node->kind != SEXPR_STRING) {
		set_err(err, err_len, name_node,
			"defsubmap name must be an atom or string");
		return -1;
	}
	raw = (name_node->kind == SEXPR_ATOM) ? name_node->u.atom :
					        name_node->u.string;
	for (size_t i = 0; i < c->n_submaps; i++) {
		if (strcmp(c->submaps[i].name, raw) == 0) {
			set_err(err, err_len, name_node,
				"defsubmap '%s' redefined", raw);
			return -1;
		}
	}
	def.name = strdup(raw);
	if (def.name == NULL) {
		set_err(err, err_len, name_node, "out of memory");
		return -1;
	}
	for (size_t i = 2; i < form->u.list.n; i++) {
		if (apply_submap_entry(&def, form->u.list.items[i], err,
				       err_len) < 0) {
			for (size_t k = 0; k < def.n_entries; k++)
				free(def.entries[k].action.arg);
			free(def.entries);
			free(def.name);
			return -1;
		}
	}

	grown = realloc(c->submaps,
			(c->n_submaps + 1) * sizeof(*c->submaps));
	if (grown == NULL) {
		for (size_t k = 0; k < def.n_entries; k++)
			free(def.entries[k].action.arg);
		free(def.entries);
		free(def.name);
		set_err(err, err_len, form, "out of memory");
		return -1;
	}
	c->submaps = grown;
	c->submaps[c->n_submaps++] = def;
	return 0;
}

static int apply_defrules(struct Config *c, const struct Sexpr *form, char *err,
			  size_t err_len)
{
	size_t n_entries = (form->u.list.n > 0) ? form->u.list.n - 1 : 0;
	if (reserve_array((void **)&c->rules, c->n_rules, n_entries,
			  sizeof(*c->rules), err, err_len) < 0)
		return -1;
	for (size_t i = 1; i < form->u.list.n; i++) {
		const struct Sexpr *e = form->u.list.items[i];
		const char *verb;

		if (e->kind != SEXPR_LIST || e->u.list.n == 0 ||
		    e->u.list.items[0]->kind != SEXPR_ATOM) {
			set_err(err, err_len, e,
				"defrules entry must start with a verb");
			return -1;
		}
		verb = e->u.list.items[0]->u.atom;
		if (strcmp(verb, "float") == 0) {
			if (apply_rule_entry(c, e, RULE_VERB_FLOAT, err,
					     err_len) < 0)
				return -1;
		} else if (strcmp(verb, "ignore") == 0) {
			if (apply_rule_entry(c, e, RULE_VERB_IGNORE, err,
					     err_len) < 0)
				return -1;
		} else {
			set_err(err, err_len, e, "unknown defrules verb '%s'",
				verb);
			return -1;
		}
	}
	return 0;
}

static int apply_defalias(struct Config *c, const struct Sexpr *form, char *err,
			  size_t err_len)
{
	(void)c;
	(void)form;
	(void)err;
	(void)err_len;
	return 0;
}

struct FormSpec {
	const char *name;
	const char *shape;
	const char *doc;
	int (*apply)(struct Config *c, const struct Sexpr *form, char *err,
		     size_t err_len);
};

static const struct FormSpec form_specs[] = {
	{ .name = "defkeys",
	  .apply = apply_defkeys,
	  .shape = ":entries (((MODS KEY ACTION)))\n"
		   "    :types ((MODS mods) (KEY keysym) (ACTION action))",
	  .doc = "Key bindings, one (MODS KEY ACTION) per entry. Binding the "
		 "same chord twice is an error." },
	{ .name = "defmouse",
	  .apply = apply_defmouse,
	  .shape =
		  ":entries (((MODS BUTTON ACTION)))\n"
		  "    :types ((MODS mods) (BUTTON (one-of left right middle))\n"
		  "            (ACTION action))",
	  .doc = "Pointer button bindings, one (MODS BUTTON ACTION) per "
		 "entry. Binding the same chord twice is an error." },
	{ .name = "defsubmap",
	  .apply = apply_defsubmap,
	  .shape = ":head (NAME)\n"
		   "    :entries (((KEY ACTION)))\n"
		   "    :types ((NAME text) (KEY keysym) (ACTION action))",
	  .doc = "A keymap entered with (submap NAME). Its keys are pressed "
		 "without modifiers, any other key leaves it. Redefining a "
		 "name is an error." },
	{ .name = "defrules",
	  .apply = apply_defrules,
	  .shape = ":entries (((float MATCHER)\n"
		   "               :keys ((:center bool :default false\n"
		   "                        :doc \"Center the window on its "
		   "output when it starts floating.\")\n"
		   "                      (:glob bool :default false\n"
		   "                        :doc \"Match the pattern as an "
		   "fnmatch(3) glob instead of exactly.\")))\n"
		   "              ((ignore MATCHER)\n"
		   "               :keys ((:glob bool :default false\n"
		   "                        :doc \"Match the pattern as an "
		   "fnmatch(3) glob instead of exactly.\"))))\n"
		   "    :types ((MATCHER matcher))",
	  .doc = "Window rules. float makes matching windows float. ignore "
		 "leaves them unmanaged: not tiled, no border, never "
		 "focused. A keyword given twice keeps the last value." },
	{ .name = "defscratchpads",
	  .apply = apply_defscratchpads,
	  .shape = ":entries (((NAME (spawn COMMAND) MATCHER)\n"
		   "               :keys ((:geometry (list-of fraction 4 4)\n"
		   "                        :doc \"(X Y W H) as fractions of "
		   "the whole output, bars included. W and H above 0, X + W "
		   "and Y + H at most 1. Unset, the window is centered.\")\n"
		   "                      (:glob bool :default false\n"
		   "                        :doc \"Match the pattern as an "
		   "fnmatch(3) glob instead of exactly.\"))))\n"
		   "    :types ((NAME text) (COMMAND string)\n"
		   "            (MATCHER (patterns (app-id PATTERN) "
		   "(title PATTERN)))\n"
		   "            (PATTERN string))",
	  .doc = "Scratchpads for (scratchpad NAME): the command that starts "
		 "the window and the matcher that recognizes it. Redefining "
		 "a name is an error. A keyword given twice keeps the last "
		 "value." },
	{ .name = "defcfg",
	  .apply = apply_defcfg,
	  .shape = ":entries ((BLOCK))\n"
		   "    :types ((BLOCK (ref defcfg)))",
	  .doc = "Settings, one block per entry, see the defcfg entries. "
		 "Blocks apply in order, a later block wins for what it "
		 "sets, and a keyword given twice keeps the last value." },
	{ .name = "defalias",
	  .apply = apply_defalias,
	  .shape = ":head (NAME BODY)\n"
		   "    :types ((NAME atom) (BODY any))",
	  .doc = "Every @NAME atom elsewhere in the config is replaced by "
		 "BODY before parsing. Bodies do not expand other aliases. "
		 "Redefining a name is an error." },
};

struct TypeSpec {
	const char *name;
	const char *def;
	const char *doc;
};

static const struct TypeSpec type_specs[] = {
	{ .name = "string", .doc = "A double quoted string." },
	{ .name = "atom", .doc = "A bare word." },
	{ .name = "text", .doc = "A string or a bare word." },
	{ .name = "any", .doc = "Any s-expression." },
	{ .name = "bool", .doc = "true or false." },
	{ .name = "color", .doc = "\"#RRGGBB\" or \"#RRGGBBAA\"." },
	{ .name = "fraction", .doc = "A decimal from 0 to 1, e.g. 0.25." },
	{ .name = "mods",
	  .doc = "Modifiers in one atom, M Super, A Alt, C Control, S Shift, "
		 "optionally joined by -, e.g. M-S. () for none." },
	{ .name = "keysym",
	  .doc = "An xkbcommon key name, case sensitive, e.g. Return, a, "
		 "F1, XF86AudioMute." },
	{ .name = "action",
	  .doc = "One of the action entries: NAME, or (NAME ARG ...) for an "
		 "action with :args." },
	{ .name = "matcher",
	  .def = "(patterns (app-id PATTERN) (title PATTERN) (parent) "
		 "(kind dialog)) :types ((PATTERN string))",
	  .doc = "app-id and title compare the window's app id or title with "
		 "PATTERN. (parent) and (kind dialog) both match windows that "
		 "have a parent, such as dialogs." },
};

static int apply_form(struct Config *c, const struct Sexpr *form, char *err,
		      size_t err_len)
{
	const char *head;

	if (form->kind != SEXPR_LIST || form->u.list.n == 0 ||
	    form->u.list.items[0]->kind != SEXPR_ATOM) {
		set_err(err, err_len, form,
			"top-level form must be a list starting with an atom");
		return -1;
	}
	head = form->u.list.items[0]->u.atom;
	for (size_t i = 0; i < ARRAY_SIZE(form_specs); i++)
		if (strcmp(head, form_specs[i].name) == 0)
			return form_specs[i].apply(c, form, err, err_len);
	set_err(err, err_len, form, "unknown top-level form '%s'", head);
	return -1;
}

/*
 * Alias substitution. Pre-pass: collect every (defalias NAME BODY)
 * top-level form into a name -> body table, then walk the rest of
 * the tree replacing @name atoms with deep-copied bodies. Aliases
 * do NOT nest: an alias body is stored verbatim, so referencing
 * @other inside an alias body leaves it as a literal @other atom
 * after substitution (which the action parser will then reject as
 * an unknown atom). Define aliases at full granularity to avoid
 * this. Top-level only - (defalias ...) inside (defkeys ...) is not
 * recognised.
 */
struct Alias {
	char *name; /* owned, sans leading '@' */
	struct Sexpr *body; /* owned (deep-copy of the alias body) */
};

struct AliasTable {
	struct Alias *items;
	size_t n;
};

static const struct Sexpr *alias_lookup(const struct AliasTable *t,
					const char *name)
{
	for (size_t i = 0; i < t->n; i++)
		if (strcmp(t->items[i].name, name) == 0)
			return t->items[i].body;
	return NULL;
}

static void alias_table_free(struct AliasTable *t)
{
	for (size_t i = 0; i < t->n; i++) {
		free(t->items[i].name);
		sexpr_free(t->items[i].body);
	}
	free(t->items);
	t->items = NULL;
	t->n = 0;
}

/* Caller has already reserved t->items via reserve_array. */
static int alias_table_add(struct AliasTable *t, const char *name,
			   const struct Sexpr *body, const struct Sexpr *at,
			   char *err, size_t err_len)
{
	struct Alias *e = &t->items[t->n];
	e->name = strdup(name);
	if (e->name == NULL) {
		set_err(err, err_len, at, "out of memory");
		return -1;
	}
	e->body = sexpr_dup(body);
	if (e->body == NULL) {
		free(e->name);
		set_err(err, err_len, at, "out of memory");
		return -1;
	}
	t->n++;
	return 0;
}

static int collect_aliases(const struct Sexpr *root, struct AliasTable *t,
			   char *err, size_t err_len)
{
	size_t n_defalias = 0;
	for (size_t i = 0; i < root->u.list.n; i++) {
		const struct Sexpr *form = root->u.list.items[i];
		if (form->kind == SEXPR_LIST && form->u.list.n > 0 &&
		    form->u.list.items[0]->kind == SEXPR_ATOM &&
		    strcmp(form->u.list.items[0]->u.atom, "defalias") == 0)
			n_defalias++;
	}
	if (reserve_array((void **)&t->items, 0, n_defalias, sizeof(*t->items),
			  err, err_len) < 0)
		return -1;
	for (size_t i = 0; i < root->u.list.n; i++) {
		const struct Sexpr *form = root->u.list.items[i];
		const struct Sexpr *name_node;
		const struct Sexpr *body_node;

		if (form->kind != SEXPR_LIST || form->u.list.n == 0 ||
		    form->u.list.items[0]->kind != SEXPR_ATOM)
			continue;
		if (strcmp(form->u.list.items[0]->u.atom, "defalias") != 0)
			continue;
		if (form->u.list.n != 3) {
			set_err(err, err_len, form,
				"defalias takes (defalias NAME BODY)");
			return -1;
		}
		name_node = form->u.list.items[1];
		body_node = form->u.list.items[2];
		if (name_node->kind != SEXPR_ATOM) {
			set_err(err, err_len, name_node,
				"defalias name must be an atom");
			return -1;
		}
		if (name_node->u.atom[0] == '@') {
			set_err(err, err_len, name_node,
				"defalias name must not start with '@'");
			return -1;
		}
		if (alias_lookup(t, name_node->u.atom) != NULL) {
			set_err(err, err_len, name_node,
				"defalias '%s' redefined", name_node->u.atom);
			return -1;
		}
		if (alias_table_add(t, name_node->u.atom, body_node, name_node,
				    err, err_len) < 0)
			return -1;
	}
	return 0;
}

/*
 * Walk the tree rooted at *slot, replacing every @name atom with a
 * deep copy of the matching alias body. An undefined @name is an
 * error (caught here so users get a precise diagnostic with line/col
 * pointing at the @name itself, rather than a downstream "unknown
 * action" further along).
 */
static int substitute_aliases(struct Sexpr **slot, const struct AliasTable *t,
			      char *err, size_t err_len)
{
	struct Sexpr *s = *slot;
	if (s == NULL)
		return 0;
	if (s->kind == SEXPR_ATOM && s->u.atom != NULL && s->u.atom[0] == '@' &&
	    s->u.atom[1] != '\0') {
		const struct Sexpr *body;
		struct Sexpr *replacement;
		const char *name;

		name = s->u.atom + 1;
		body = alias_lookup(t, name);
		if (body == NULL) {
			set_err(err, err_len, s, "undefined alias '@%s'", name);
			return -1;
		}
		replacement = sexpr_dup(body);
		if (replacement == NULL) {
			set_err(err, err_len, s, "out of memory");
			return -1;
		}
		/* Preserve source position from the call site so any
		 * later parser error blames the @name reference, not
		 * the alias definition. */
		replacement->line = s->line;
		replacement->col = s->col;
		sexpr_free(s);
		*slot = replacement;
		return 0;
	}
	if (s->kind == SEXPR_LIST) {
		for (size_t i = 0; i < s->u.list.n; i++) {
			if (substitute_aliases(&s->u.list.items[i], t, err,
					       err_len) < 0)
				return -1;
		}
	}
	return 0;
}

/* Apply alias substitution to root's top-level forms, skipping the
 * defalias forms themselves (whose bodies are already in the table
 * and whose @-references would be needless and potentially error). */
static int apply_aliases(struct Sexpr *root, char *err, size_t err_len)
{
	struct AliasTable t = { 0 };
	if (collect_aliases(root, &t, err, err_len) < 0) {
		alias_table_free(&t);
		return -1;
	}
	if (t.n == 0) {
		alias_table_free(&t);
		return 0;
	}
	for (size_t i = 0; i < root->u.list.n; i++) {
		struct Sexpr *form = root->u.list.items[i];
		if (form != NULL && form->kind == SEXPR_LIST &&
		    form->u.list.n > 0 &&
		    form->u.list.items[0]->kind == SEXPR_ATOM &&
		    strcmp(form->u.list.items[0]->u.atom, "defalias") == 0)
			continue;
		if (substitute_aliases(&root->u.list.items[i], &t, err,
				       err_len) < 0) {
			alias_table_free(&t);
			return -1;
		}
	}
	alias_table_free(&t);
	return 0;
}

static int check_action_ref(const struct Config *c, const struct Action *a,
			    char *err, size_t err_len)
{
	size_t i;

	if (a->kind == ACTION_SUBMAP) {
		for (i = 0; i < c->n_submaps; i++) {
			if (strcmp(c->submaps[i].name, a->arg) == 0)
				return 0;
		}
		set_err(err, err_len, NULL, "unknown submap '%s'", a->arg);
		return -1;
	}
	if (a->kind == ACTION_NAMED_SCRATCHPAD) {
		for (i = 0; i < c->n_scratchpads; i++) {
			if (strcmp(c->scratchpads[i].name, a->arg) == 0)
				return 0;
		}
		set_err(err, err_len, NULL, "unknown scratchpad '%s'", a->arg);
		return -1;
	}
	return 0;
}

static int check_action_refs(const struct Config *c, char *err, size_t err_len)
{
	size_t i, j;

	for (i = 0; i < c->n_xkb; i++) {
		if (check_action_ref(c, &c->xkb[i].action, err, err_len) < 0)
			return -1;
	}
	for (i = 0; i < c->n_ptr; i++) {
		if (check_action_ref(c, &c->ptr[i].action, err, err_len) < 0)
			return -1;
	}
	for (i = 0; i < c->n_submaps; i++) {
		for (j = 0; j < c->submaps[i].n_entries; j++) {
			if (check_action_ref(c,
					     &c->submaps[i].entries[j].action,
					     err, err_len) < 0)
				return -1;
		}
	}
	return 0;
}

struct Config *config_parse(const char *src, char *err, size_t err_len)
{
	struct Sexpr *root;
	struct Config *c;

	if (err != NULL && err_len > 0)
		err[0] = '\0';
	root = sexpr_parse(src, err, err_len);
	if (root == NULL)
		return NULL;
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		sexpr_free(root);
		set_err(err, err_len, NULL, "out of memory");
		return NULL;
	}
	/* Defaults: green focused border, gray unfocused, no gaps. */
	c->borders.width = 3;
	c->borders.focused_rgba = 0x21fc0dffu;
	c->borders.unfocused_rgba = 0x4c566affu;
	c->gaps.top = 0;
	c->gaps.bottom = 0;
	c->gaps.left = 0;
	c->gaps.right = 0;
	c->gaps.inner = 0;
	c->input.cursor_size = 24;
	c->input.tap = -1;
	c->input.natural_scroll = -1;
	c->input.dwt = -1;
	c->input.middle_emulation = -1;
	c->layout_kinds[0] = LAYOUT_TALL;
	c->n_layout_kinds = 1;
	c->decoration = DECORATION_SSD;
	if (apply_aliases(root, err, err_len) < 0) {
		config_free(c);
		sexpr_free(root);
		return NULL;
	}
	for (size_t i = 0; i < root->u.list.n; i++) {
		if (apply_form(c, root->u.list.items[i], err, err_len) < 0) {
			config_free(c);
			sexpr_free(root);
			return NULL;
		}
	}
	sexpr_free(root);
	if (check_action_refs(c, err, err_len) < 0) {
		config_free(c);
		return NULL;
	}
	return c;
}

struct Config *config_load(const char *path, char *err, size_t err_len)
{
	struct Config *c;
	size_t read;
	char *buf;
	FILE *f;
	long sz;

	if (err != NULL && err_len > 0)
		err[0] = '\0';
	f = fopen(path, "rb");
	if (f == NULL) {
		snprintf(err, err_len, "%s: %s", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		snprintf(err, err_len, "%s: seek failed", path);
		fclose(f);
		return NULL;
	}
	sz = ftell(f);
	if (sz < 0) {
		snprintf(err, err_len, "%s: tell failed", path);
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = malloc((size_t)sz + 1);
	if (buf == NULL) {
		fclose(f);
		snprintf(err, err_len, "%s: out of memory", path);
		return NULL;
	}
	read = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[read] = '\0';
	if (memchr(buf, '\0', read) != NULL) {
		snprintf(err, err_len, "%s: contains a NUL byte", path);
		free(buf);
		return NULL;
	}

	c = config_parse(buf, err, err_len);
	free(buf);
	return c;
}

struct Config *config_default(void)
{
	static const char default_src[] =
		"(defcfg\n"
		"  (layouts tall mirror-tall deck))\n"
		"\n"
		"(defkeys\n"
		"  (M-S Return  (spawn \"${TERMINAL:-foot}\"))\n"
		"  (M p         (spawn \"fuzzel\"))\n"
		"  (M-S c       close)\n"
		"  (M space     next-layout)\n"
		"  (M-S space   prev-layout)\n"
		"  (M Tab       focus-next)\n"
		"  (M-S Tab     focus-prev)\n"
		"  (M j         focus-next)\n"
		"  (M k         focus-prev)\n"
		"  (M m         focus-master)\n"
		"  (M-S j       swap-next)\n"
		"  (M-S k       swap-prev)\n"
		"  (M h         shrink-master)\n"
		"  (M l         expand-master)\n"
		"  (M t         toggle-float)\n"
		"  (M comma     inc-master)\n"
		"  (M period    dec-master)\n"
		"  (M q         restart)\n"
		"  (M-S q       exit)\n"
		"  (M 1 (workspace 1)) (M 2 (workspace 2)) (M 3 (workspace 3))\n"
		"  (M 4 (workspace 4)) (M 5 (workspace 5)) (M 6 (workspace 6))\n"
		"  (M 7 (workspace 7)) (M 8 (workspace 8)) (M 9 (workspace 9))\n"
		"  (M-S 1 (move-to-workspace 1)) (M-S 2 (move-to-workspace 2))\n"
		"  (M-S 3 (move-to-workspace 3)) (M-S 4 (move-to-workspace 4))\n"
		"  (M-S 5 (move-to-workspace 5)) (M-S 6 (move-to-workspace 6))\n"
		"  (M-S 7 (move-to-workspace 7)) (M-S 8 (move-to-workspace 8))\n"
		"  (M-S 9 (move-to-workspace 9))\n"
		"  (M w   (focus-output 0)) (M e   (focus-output 1)) (M r   (focus-output 2))\n"
		"  (M-S w (move-to-output 0)) (M-S e (move-to-output 1)) (M-S r (move-to-output 2)))\n"
		"\n"
		"(defmouse\n"
		"  (M left   move-window)\n"
		"  (M right  resize-window))\n";
	return config_parse(default_src, NULL, 0);
}

void config_free(struct Config *c)
{
	if (c == NULL)
		return;
	free(c->input.cursor_theme);
	for (size_t i = 0; i < 5; i++)
		free(c->input.xkb[i]);
	for (size_t i = 0; i < c->n_xkb; i++)
		free(c->xkb[i].action.arg);
	for (size_t i = 0; i < c->n_ptr; i++)
		free(c->ptr[i].action.arg);
	for (size_t i = 0; i < c->n_rules; i++)
		free(c->rules[i].pattern);
	for (size_t i = 0; i < c->n_scratchpads; i++) {
		free(c->scratchpads[i].name);
		free(c->scratchpads[i].spawn);
		free(c->scratchpads[i].match_pattern);
	}
	for (size_t i = 0; i < c->n_submaps; i++) {
		struct SubmapDef *d = &c->submaps[i];
		for (size_t k = 0; k < d->n_entries; k++)
			free(d->entries[k].action.arg);
		free(d->entries);
		free(d->name);
	}
	free(c->xkb);
	free(c->ptr);
	free(c->rules);
	free(c->scratchpads);
	free(c->submaps);
	free(c);
}

int config_parse_action_text(const struct Config *c, const char *src,
			     struct Action *out, char *err, size_t err_len)
{
	struct Sexpr *top;
	int r = -1;

	out->kind = ACTION_NONE;
	out->arg = NULL;
	top = sexpr_parse(src, err, err_len);
	if (top == NULL)
		return -1;
	if (top->kind != SEXPR_LIST || top->u.list.n != 1) {
		snprintf(err, err_len, "expected exactly one action");
		goto out;
	}
	if (parse_action(top->u.list.items[0], out, err, err_len) < 0)
		goto out;
	if (c != NULL && check_action_ref(c, out, err, err_len) < 0) {
		free(out->arg);
		out->arg = NULL;
		out->kind = ACTION_NONE;
		goto out;
	}
	r = 0;
out:
	sexpr_free(top);
	return r;
}

void config_describe(FILE *out)
{
	struct Config *d;

	fputs("(nomonad-schema\n  (version 1)\n", out);
	for (size_t i = 0; i < ARRAY_SIZE(type_specs); i++) {
		fprintf(out, "  (type %s", type_specs[i].name);
		if (type_specs[i].def != NULL)
			fprintf(out, " %s", type_specs[i].def);
		fputs("\n    :doc ", out);
		describe_string(out, type_specs[i].doc);
		fputs(")\n", out);
	}
	for (size_t i = 0; i < ARRAY_SIZE(form_specs); i++) {
		fprintf(out, "  (form %s\n    %s\n    :doc ",
			form_specs[i].name, form_specs[i].shape);
		describe_string(out, form_specs[i].doc);
		fputs(")\n", out);
	}
	for (size_t i = 0; i < ARRAY_SIZE(action_specs); i++) {
		describe_action(out, &action_specs[i]);
		fputs("\n", out);
	}
	for (size_t i = 0; i < ARRAY_SIZE(cfg_blocks); i++) {
		d = config_parse("", NULL, 0);
		if (d == NULL)
			break;
		if (cfg_blocks[i].begin != NULL)
			cfg_blocks[i].begin(d);
		describe_block(out, &cfg_blocks[i], d);
		fputs(i + 1 < ARRAY_SIZE(cfg_blocks) ? "\n" : "", out);
		config_free(d);
	}
	fputs(")\n", out);
}
