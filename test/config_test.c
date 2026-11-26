/* SPDX-License-Identifier: 0BSD */
/*
 * Tests for config.c. Links sexpr.o, config.o, and xkbcommon (for
 * xkb_keysym_from_name).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "config.h"
#include "sexpr.h"

static int g_run;
static int g_passed;

#define ASSERT(cond, ...)                                                      \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__);      \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			return 0;                                              \
		}                                                              \
	} while (0)

#define RUN(name)                                                              \
	do {                                                                   \
		g_run++;                                                       \
		if (test_##name())                                             \
			g_passed++;                                            \
		else                                                           \
			fprintf(stderr, "FAIL: %s\n", #name);                  \
	} while (0)

/* river protocol modifier values, hardcoded so the test does not need
 * the generated header. They are baked into the protocol and will not
 * change. */
#define MOD_SHIFT 1u
#define MOD_CTRL  4u
#define MOD_ALT   8u
#define MOD_SUPER 64u

static int test_default_config_parses(void)
{
	struct Config *c = config_default();
	ASSERT(c != NULL, "config_default returned NULL");
	/* xmonad's default keys: 19 base, 9 workspace, 9 move-to-workspace,
	 * 6 output. */
	ASSERT(c->n_xkb == 43, "expected 43 xkb bindings, got %zu", c->n_xkb);
	ASSERT(c->n_layout_kinds == 3, "expected tall mirror-tall deck");
	ASSERT(c->n_ptr == 2, "expected 2 ptr bindings, got %zu", c->n_ptr);
	config_free(c);
	return 1;
}

static int test_action_reload_config(void)
{
	struct Config *c = config_parse(
		"(defkeys (A-S r reload-config))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_RELOAD_CONFIG,
	       "kind: got %d, expected %d", (int)c->xkb[0].action.kind,
	       (int)ACTION_RELOAD_CONFIG);
	ASSERT(c->xkb[0].action.arg == NULL, "arg should be NULL");
	config_free(c);
	return 1;
}

static int test_action_workspace(void)
{
	struct Config *c = config_parse(
		"(defkeys (A 1 (workspace 1)) (A 2 (workspace \"2\")))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_WORKSPACE, "kind");
	ASSERT(strcmp(c->xkb[0].action.arg, "1") == 0, "arg = %s",
	       c->xkb[0].action.arg);
	ASSERT(c->xkb[1].action.kind == ACTION_WORKSPACE, "kind 2");
	ASSERT(strcmp(c->xkb[1].action.arg, "2") == 0, "arg 2 = %s",
	       c->xkb[1].action.arg);
	config_free(c);
	return 1;
}

static int test_action_move_to_workspace(void)
{
	struct Config *c = config_parse(
		"(defkeys (A-S 1 (move-to-workspace 1)))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_MOVE_TO_WORKSPACE, "kind");
	ASSERT(strcmp(c->xkb[0].action.arg, "1") == 0, "arg = %s",
	       c->xkb[0].action.arg);
	config_free(c);
	return 1;
}

static int test_actions_workspace_and_output(void)
{
	struct Config *c = config_parse(
		"(defkeys"
		"  (A Tab    next-workspace)"
		"  (A-S Tab  prev-workspace)"
		"  (A w      (focus-output 0))"
		"  (A e      (focus-output 1))"
		"  (A-S w    (move-to-output 0))"
		"  (A-S e    (move-to-output 1)))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_NEXT_WORKSPACE, "next");
	ASSERT(c->xkb[0].action.arg == NULL, "next no arg");
	ASSERT(c->xkb[1].action.kind == ACTION_PREV_WORKSPACE, "prev");
	ASSERT(c->xkb[1].action.arg == NULL, "prev no arg");
	ASSERT(c->xkb[2].action.kind == ACTION_FOCUS_OUTPUT, "focus-output 0");
	ASSERT(strcmp(c->xkb[2].action.arg, "0") == 0, "focus-output arg");
	ASSERT(c->xkb[3].action.kind == ACTION_FOCUS_OUTPUT, "focus-output 1");
	ASSERT(strcmp(c->xkb[3].action.arg, "1") == 0, "focus-output arg 1");
	ASSERT(c->xkb[4].action.kind == ACTION_MOVE_TO_OUTPUT, "move 0");
	ASSERT(strcmp(c->xkb[4].action.arg, "0") == 0, "move-to-output arg");
	ASSERT(c->xkb[5].action.kind == ACTION_MOVE_TO_OUTPUT, "move 1");
	ASSERT(strcmp(c->xkb[5].action.arg, "1") == 0, "move-to-output arg 1");
	config_free(c);
	return 1;
}

static int test_err_workspace_no_arg(void)
{
	char err[256];
	struct Config *c = config_parse("(defkeys (A 1 (workspace)))",
					err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "workspace") != NULL, "err = %s", err);
	return 1;
}

static int test_mods_no_modifier(void)
{
	const char *src = "(defkeys (() Escape exit))";
	struct Config *c = config_parse(src, NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 1, "n_xkb");
	ASSERT(c->xkb[0].mods == 0, "expected mods=0, got %u", c->xkb[0].mods);
	config_free(c);
	return 1;
}

static int test_mods_single(void)
{
	struct Config *c = config_parse(
		"(defkeys (M q close) (A q close) (C q close) (S q close))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 4, "n_xkb");
	ASSERT(c->xkb[0].mods == MOD_SUPER, "M");
	ASSERT(c->xkb[1].mods == MOD_ALT,   "A");
	ASSERT(c->xkb[2].mods == MOD_CTRL,  "C");
	ASSERT(c->xkb[3].mods == MOD_SHIFT, "S");
	config_free(c);
	return 1;
}

static int test_mods_combined(void)
{
	struct Config *c = config_parse(
		"(defkeys (M-S q close) (C-A q close) (M-A-S q close))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].mods == (MOD_SUPER | MOD_SHIFT), "M-S");
	ASSERT(c->xkb[1].mods == (MOD_CTRL | MOD_ALT), "C-A");
	ASSERT(c->xkb[2].mods == (MOD_SUPER | MOD_ALT | MOD_SHIFT), "M-A-S");
	config_free(c);
	return 1;
}

static int test_keysym_resolution(void)
{
	struct Config *c = config_parse(
		"(defkeys"
		"  (A Return       close)"
		"  (A space        close)"
		"  (A q            close)"
		"  (A 1            close)"
		"  (A XF86AudioMute close))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].keysym == XKB_KEY_Return, "Return");
	ASSERT(c->xkb[1].keysym == XKB_KEY_space, "space");
	ASSERT(c->xkb[2].keysym == XKB_KEY_q, "q");
	ASSERT(c->xkb[3].keysym == XKB_KEY_1, "1");
	ASSERT(c->xkb[4].keysym == XKB_KEY_XF86AudioMute, "XF86AudioMute");
	config_free(c);
	return 1;
}

static int test_actions_bare(void)
{
	struct Config *c = config_parse(
		"(defkeys"
		"  (A 1 close)"
		"  (A 2 focus-next)"
		"  (A 3 exit)"
		"  (A 4 move-window)"
		"  (A 5 resize-window))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_CLOSE, "close");
	ASSERT(c->xkb[1].action.kind == ACTION_FOCUS_NEXT, "focus-next");
	ASSERT(c->xkb[2].action.kind == ACTION_EXIT, "exit");
	ASSERT(c->xkb[3].action.kind == ACTION_MOVE, "move-window");
	ASSERT(c->xkb[4].action.kind == ACTION_RESIZE, "resize-window");
	for (size_t i = 0; i < c->n_xkb; i++)
		ASSERT(c->xkb[i].action.arg == NULL, "arg should be NULL");
	config_free(c);
	return 1;
}

static int test_actions_master_and_float(void)
{
	struct Config *c = config_parse(
		"(defkeys"
		"  (A h      shrink-master)"
		"  (A l      expand-master)"
		"  (A comma  inc-master)"
		"  (A period dec-master)"
		"  (A-S t    toggle-float)"
		"  (A f      toggle-fullscreen))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_SHRINK_MASTER, "shrink-master");
	ASSERT(c->xkb[1].action.kind == ACTION_EXPAND_MASTER, "expand-master");
	ASSERT(c->xkb[2].action.kind == ACTION_INC_MASTER, "inc-master");
	ASSERT(c->xkb[3].action.kind == ACTION_DEC_MASTER, "dec-master");
	ASSERT(c->xkb[4].action.kind == ACTION_TOGGLE_FLOAT, "toggle-float");
	ASSERT(c->xkb[5].action.kind == ACTION_TOGGLE_FULLSCREEN,
	       "toggle-fullscreen");
	for (size_t i = 0; i < c->n_xkb; i++)
		ASSERT(c->xkb[i].action.arg == NULL, "arg should be NULL");
	config_free(c);
	return 1;
}

static int test_actions_bsp(void)
{
	struct Config *c = config_parse(
		"(defkeys"
		"  (A r       bsp-rotate)"
		"  (A s       bsp-swap)"
		"  (A-C l     bsp-expand-r)"
		"  (A-C h     bsp-expand-l)"
		"  (A-C j     bsp-expand-d)"
		"  (A-C k     bsp-expand-u)"
		"  (A-C-S l   bsp-shrink-r)"
		"  (A-C-S h   bsp-shrink-l)"
		"  (A-C-S j   bsp-shrink-d)"
		"  (A-C-S k   bsp-shrink-u)"
		"  (A a       bsp-balance)"
		"  (A-S a     bsp-equalize)"
		"  (A-C a     bsp-balance-area)"
		"  (A-C Right bsp-rotate-cw)"
		"  (A-C Left  bsp-rotate-ccw))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 15, "expected 15, got %zu", c->n_xkb);
	const char *expected_verbs[] = {
		"rotate",  "swap",
		"expand-r", "expand-l", "expand-d", "expand-u",
		"shrink-r", "shrink-l", "shrink-d", "shrink-u",
		"balance", "equalize", "balance-area",
		"rotate-cw", "rotate-ccw",
	};
	for (size_t i = 0; i < c->n_xkb; i++) {
		ASSERT(c->xkb[i].action.kind == ACTION_LAYOUT_VERB,
		       "binding %zu kind", i);
		ASSERT(c->xkb[i].action.arg != NULL,
		       "binding %zu arg should be set", i);
		ASSERT(strcmp(c->xkb[i].action.arg, expected_verbs[i]) == 0,
		       "binding %zu verb '%s' (want '%s')", i,
		       c->xkb[i].action.arg, expected_verbs[i]);
	}
	config_free(c);
	return 1;
}

static int test_actions_focus_swap(void)
{
	struct Config *c = config_parse(
		"(defkeys"
		"  (A j      focus-next)"
		"  (A k      focus-prev)"
		"  (A m      focus-master)"
		"  (A-S j    swap-next)"
		"  (A-S k    swap-prev)"
		"  (A o      focus-last))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_FOCUS_NEXT, "focus-next");
	ASSERT(c->xkb[1].action.kind == ACTION_FOCUS_PREV, "focus-prev");
	ASSERT(c->xkb[2].action.kind == ACTION_FOCUS_MASTER, "focus-master");
	ASSERT(c->xkb[3].action.kind == ACTION_SWAP_NEXT, "swap-next");
	ASSERT(c->xkb[4].action.kind == ACTION_SWAP_PREV, "swap-prev");
	ASSERT(c->xkb[5].action.kind == ACTION_FOCUS_LAST, "focus-last");
	for (size_t i = 0; i < c->n_xkb; i++)
		ASSERT(c->xkb[i].action.arg == NULL, "arg should be NULL");
	config_free(c);
	return 1;
}

static int test_action_spawn(void)
{
	struct Config *c = config_parse(
		"(defkeys (A space (spawn \"alacritty -e htop\")))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_SPAWN, "spawn");
	ASSERT(c->xkb[0].action.arg != NULL, "arg should be set");
	ASSERT(strcmp(c->xkb[0].action.arg, "alacritty -e htop") == 0,
	       "arg = %s", c->xkb[0].action.arg);
	config_free(c);
	return 1;
}

static int test_buttons(void)
{
	struct Config *c = config_parse(
		"(defmouse"
		"  (A left   move-window)"
		"  (A right  resize-window)"
		"  (M middle close))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_ptr == 3, "n_ptr");
	ASSERT(c->ptr[0].button == BTN_LEFT, "left");
	ASSERT(c->ptr[1].button == BTN_RIGHT, "right");
	ASSERT(c->ptr[2].button == BTN_MIDDLE, "middle");
	config_free(c);
	return 1;
}

static int test_combined_forms(void)
{
	struct Config *c = config_parse(
		"(defkeys (A space (spawn \"foo\")) (A q close))\n"
		"(defmouse (A left move-window))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 2, "xkb count");
	ASSERT(c->n_ptr == 1, "ptr count");
	config_free(c);
	return 1;
}

static int test_comments(void)
{
	struct Config *c = config_parse(
		";; whole line comment\n"
		"(defkeys ; trailing\n"
		"  (A q close)) ; another\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 1, "xkb count");
	config_free(c);
	return 1;
}

static int test_empty_input(void)
{
	struct Config *c = config_parse("", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 0, "no xkb");
	ASSERT(c->n_ptr == 0, "no ptr");
	config_free(c);
	return 1;
}

static int test_err_unknown_form(void)
{
	char err[256];
	struct Config *c = config_parse("(notadef foo)", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "notadef") != NULL, "err = %s", err);
	return 1;
}

static int test_err_bad_mod(void)
{
	char err[256];
	struct Config *c = config_parse("(defkeys (X q close))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "modifier") != NULL, "err = %s", err);
	return 1;
}

static int test_err_unknown_key(void)
{
	char err[256];
	struct Config *c = config_parse("(defkeys (A NoSuchKey close))",
					err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "NoSuchKey") != NULL, "err = %s", err);
	return 1;
}

static int test_err_unknown_action(void)
{
	char err[256];
	struct Config *c = config_parse("(defkeys (A q nonsense))",
					err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "nonsense") != NULL, "err = %s", err);
	return 1;
}

static int test_err_unknown_button(void)
{
	char err[256];
	struct Config *c = config_parse("(defmouse (A scroll close))",
					err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "scroll") != NULL, "err = %s", err);
	return 1;
}

static int test_err_spawn_missing_arg(void)
{
	char err[256];
	struct Config *c = config_parse("(defkeys (A q (spawn)))",
					err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "spawn") != NULL, "err = %s", err);
	return 1;
}

static int test_err_spawn_atom_arg(void)
{
	/* spawn arg must be a string literal, not a bare atom */
	char err[256];
	struct Config *c = config_parse("(defkeys (A q (spawn alacritty)))",
					err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "spawn") != NULL, "err = %s", err);
	return 1;
}

static int test_err_malformed_entry(void)
{
	char err[256];
	struct Config *c = config_parse("(defkeys (A q))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "defkeys") != NULL, "err = %s", err);
	return 1;
}

static int test_defrules_app_id(void)
{
	struct Config *c = config_parse(
		"(defrules (float (app-id \"Pavucontrol\")))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_rules == 1, "expected 1 rule, got %zu", c->n_rules);
	ASSERT(c->rules[0].kind == RULE_MATCH_APP_ID, "kind");
	ASSERT(strcmp(c->rules[0].pattern, "Pavucontrol") == 0, "pattern");
	ASSERT(c->rules[0].center == false, "center default false");
	config_free(c);
	return 1;
}

static int test_defrules_title_with_center(void)
{
	struct Config *c = config_parse(
		"(defrules (float (title \"Save As...\") :center true))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_rules == 1, "n_rules");
	ASSERT(c->rules[0].kind == RULE_MATCH_TITLE, "kind");
	ASSERT(strcmp(c->rules[0].pattern, "Save As...") == 0, "pattern");
	ASSERT(c->rules[0].center == true, "center true");
	config_free(c);
	return 1;
}

static int test_defrules_parent_and_kind_dialog(void)
{
	struct Config *c = config_parse(
		"(defrules"
		"  (float (parent) :center true)"
		"  (float (kind dialog) :center true))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_rules == 2, "n_rules");
	ASSERT(c->rules[0].kind == RULE_MATCH_PARENT, "rule 0 parent");
	ASSERT(c->rules[1].kind == RULE_MATCH_PARENT, "rule 1 dialog->parent");
	ASSERT(c->rules[0].pattern == NULL, "pattern null for parent");
	config_free(c);
	return 1;
}

static int test_defrules_multiple(void)
{
	struct Config *c = config_parse(
		"(defrules"
		"  (float (app-id \"Pavucontrol\"))"
		"  (float (app-id \"Galculator\"))"
		"  (float (app-id \"mpv\") :center true))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_rules == 3, "n_rules");
	ASSERT(c->rules[2].center == true, "mpv has :center");
	ASSERT(c->rules[0].center == false, "Pavu has no :center");
	config_free(c);
	return 1;
}

static int test_defrules_ignore(void)
{
	struct Config *c = config_parse(
		"(defrules"
		"  (ignore (app-id \"desktop_window\"))"
		"  (float  (app-id \"mpv\") :center true))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_rules == 2, "n_rules");
	ASSERT(c->rules[0].verb == RULE_VERB_IGNORE, "rule 0 verb");
	ASSERT(c->rules[0].kind == RULE_MATCH_APP_ID, "rule 0 kind");
	ASSERT(strcmp(c->rules[0].pattern, "desktop_window") == 0, "rule 0 pat");
	ASSERT(c->rules[0].center == false, "rule 0 center");
	ASSERT(c->rules[0].glob == false, "rule 0 glob");
	ASSERT(c->rules[1].verb == RULE_VERB_FLOAT, "rule 1 verb");
	ASSERT(c->rules[1].center == true, "rule 1 center");
	config_free(c);
	return 1;
}

static int test_defrules_glob(void)
{
	struct Config *c = config_parse(
		"(defrules"
		"  (float  (app-id \"PCloud*\") :glob true)"
		"  (ignore (title  \"^scratchpad-\") :glob true))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_rules == 2, "n_rules");
	ASSERT(c->rules[0].verb == RULE_VERB_FLOAT, "rule 0 verb");
	ASSERT(c->rules[0].glob == true, "rule 0 glob");
	ASSERT(strcmp(c->rules[0].pattern, "PCloud*") == 0, "rule 0 pat");
	ASSERT(c->rules[1].verb == RULE_VERB_IGNORE, "rule 1 verb");
	ASSERT(c->rules[1].glob == true, "rule 1 glob");
	config_free(c);
	return 1;
}

static int test_err_ignore_with_center(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defrules (ignore (app-id \"x\") :center true))",
		err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, ":center") != NULL, "err = %s", err);
	return 1;
}

static int test_err_defrules_unknown_matcher(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defrules (float (resource \"foo\")))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "matcher") != NULL, "err = %s", err);
	return 1;
}

static int test_err_defrules_bad_keyword(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defrules (float (app-id \"x\") :colour true))",
		err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, ":colour") != NULL, "err = %s", err);
	return 1;
}

static int test_defcfg_defaults(void)
{
	struct Config *c = config_parse("(defkeys (A q close))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->borders.width == 3, "default width %d", c->borders.width);
	ASSERT(c->borders.focused_rgba == 0x21fc0dffu,
	       "default focused 0x%x", c->borders.focused_rgba);
	ASSERT(c->borders.unfocused_rgba == 0x4c566affu,
	       "default unfocused 0x%x", c->borders.unfocused_rgba);
	ASSERT(c->gaps.top == 0 && c->gaps.bottom == 0 && c->gaps.left == 0 &&
	       c->gaps.right == 0 && c->gaps.inner == 0,
	       "default gaps non-zero");
	config_free(c);
	return 1;
}

static int test_defcfg_borders(void)
{
	struct Config *c = config_parse(
		"(defcfg (borders :width 5 :focused \"#aabbcc\" "
		":unfocused \"#11223344\"))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->borders.width == 5, "width %d", c->borders.width);
	ASSERT(c->borders.focused_rgba == 0xaabbccffu,
	       "focused 0x%x", c->borders.focused_rgba);
	ASSERT(c->borders.unfocused_rgba == 0x11223344u,
	       "unfocused 0x%x", c->borders.unfocused_rgba);
	config_free(c);
	return 1;
}

static int test_defcfg_gaps(void)
{
	struct Config *c = config_parse(
		"(defcfg (gaps :top 25 :bottom 1 :left 2 :right 3 :inner 4))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->gaps.top == 25, "top %d", c->gaps.top);
	ASSERT(c->gaps.bottom == 1, "bottom %d", c->gaps.bottom);
	ASSERT(c->gaps.left == 2, "left %d", c->gaps.left);
	ASSERT(c->gaps.right == 3, "right %d", c->gaps.right);
	ASSERT(c->gaps.inner == 4, "inner %d", c->gaps.inner);
	config_free(c);
	return 1;
}

static int test_defcfg_borders_disabled(void)
{
	struct Config *c = config_parse("(defcfg (borders :width 0))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->borders.width == 0, "width %d", c->borders.width);
	config_free(c);
	return 1;
}

static int test_err_defcfg_bad_color(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defcfg (borders :focused \"#xyz\"))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "color") != NULL || strstr(err, "hex") != NULL,
	       "err lacks color hint: %s", err);
	return 1;
}

static int test_err_defcfg_negative(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defcfg (gaps :top -5))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "[0, 1000]") != NULL, "err lacks range hint: %s",
	       err);
	c = config_parse("(defcfg (gaps :inner 1100000000))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL for huge gap");
	return 1;
}

static int test_defcfg_layout_default_tall(void)
{
	struct Config *c = config_parse("(defkeys (A q close))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_TALL, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_bsp(void)
{
	struct Config *c = config_parse("(defcfg (layout bsp))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_BSP, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_deck(void)
{
	struct Config *c = config_parse("(defcfg (layout deck))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_DECK, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_mirror_tall(void)
{
	struct Config *c = config_parse("(defcfg (layout mirror-tall))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_MIRROR_TALL, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_three_columns(void)
{
	struct Config *c =
		config_parse("(defcfg (layout three-columns))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_THREE_COLUMNS, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_grid(void)
{
	struct Config *c = config_parse("(defcfg (layout grid))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_GRID, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_tabbed(void)
{
	struct Config *c = config_parse("(defcfg (layout tabbed))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_TABBED, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_spiral(void)
{
	struct Config *c = config_parse("(defcfg (layout spiral))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_SPIRAL, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_two_pane(void)
{
	struct Config *c = config_parse("(defcfg (layout two-pane))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_TWO_PANE, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layout_resizable_tall(void)
{
	struct Config *c =
		config_parse("(defcfg (layout resizable-tall))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_RESIZABLE_TALL, "kind %d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layouts_multi(void)
{
	struct Config *c = config_parse(
		"(defcfg (layouts tall bsp deck mirror-tall))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 4, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_TALL, "[0]=%d",
	       (int)c->layout_kinds[0]);
	ASSERT(c->layout_kinds[1] == LAYOUT_BSP, "[1]=%d",
	       (int)c->layout_kinds[1]);
	ASSERT(c->layout_kinds[2] == LAYOUT_DECK, "[2]=%d",
	       (int)c->layout_kinds[2]);
	ASSERT(c->layout_kinds[3] == LAYOUT_MIRROR_TALL, "[3]=%d",
	       (int)c->layout_kinds[3]);
	config_free(c);
	return 1;
}

static int test_defcfg_layouts_single(void)
{
	struct Config *c = config_parse("(defcfg (layouts deck))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_layout_kinds == 1, "n=%zu", c->n_layout_kinds);
	ASSERT(c->layout_kinds[0] == LAYOUT_DECK, "[0]=%d",
	       (int)c->layout_kinds[0]);
	config_free(c);
	return 1;
}

static int test_defcfg_layouts_empty_errors(void)
{
	char err[256];
	struct Config *c = config_parse("(defcfg (layouts))", err, sizeof(err));
	ASSERT(c == NULL, "expected parse error on empty (layouts)");
	ASSERT(strstr(err, "one or more") != NULL, "err = %s", err);
	return 1;
}

static int test_defcfg_layouts_unknown_errors(void)
{
	char err[256];
	struct Config *c =
		config_parse("(defcfg (layouts tall bogus))", err, sizeof(err));
	ASSERT(c == NULL, "expected parse error on unknown layout");
	ASSERT(strstr(err, "bogus") != NULL, "err = %s", err);
	return 1;
}

static int test_defcfg_decorations_default(void)
{
	struct Config *c = config_parse("(defkeys (A q close))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->decoration == DECORATION_SSD, "default decoration %d",
	       (int)c->decoration);
	config_free(c);
	return 1;
}

static int test_defcfg_decorations_csd(void)
{
	struct Config *c = config_parse("(defcfg (decorations csd))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->decoration == DECORATION_CSD, "decoration");
	config_free(c);
	return 1;
}

static int test_defcfg_decorations_ssd(void)
{
	struct Config *c = config_parse("(defcfg (decorations ssd))", NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->decoration == DECORATION_SSD, "decoration");
	config_free(c);
	return 1;
}

static int test_err_defcfg_unknown_decorations(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defcfg (decorations fancy))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "fancy") != NULL, "err = %s", err);
	return 1;
}

static int test_err_defcfg_unknown_layout(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defcfg (layout fancy))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "fancy") != NULL, "err = %s", err);
	return 1;
}

static int test_err_defcfg_unknown_block(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defcfg (mysteries :width 1))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "mysteries") != NULL, "err = %s", err);
	return 1;
}

/*
 * examples/config.scm is config_load'd so the test suite fails when a
 * vocabulary change makes it stale. Tests run from the repository root
 * via `make test`, so the relative path resolves to examples/.
 */
static int test_example_config_parses(void)
{
	char err[512];
	struct Config *c = config_load("examples/config.scm", err,
				       sizeof(err));
	ASSERT(c != NULL, "examples/config.scm parse failed: %s", err);
	ASSERT(c->n_layout_kinds == 4 && c->layout_kinds[0] == LAYOUT_TALL,
	       "layouts should be tall bsp three-columns deck");
	ASSERT(c->gaps.inner == 4, "gaps :inner should be 4, got %d",
	       c->gaps.inner);
	ASSERT(c->borders.width == 2, "borders :width should be 2");
	ASSERT(c->n_rules == 6, "expected 6 rules, got %zu", c->n_rules);
	ASSERT(c->n_scratchpads == 2, "expected 2 scratchpads");
	ASSERT(c->n_submaps == 1, "expected 1 submap");
	config_free(c);
	return 1;
}

static int test_defalias_basic(void)
{
	struct Config *c = config_parse(
		"(defalias term (spawn \"alacritty\"))\n"
		"(defkeys (M Return @term) (M t @term))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 2, "n_xkb %zu", c->n_xkb);
	ASSERT(c->xkb[0].action.kind == ACTION_SPAWN, "kind 0");
	ASSERT(c->xkb[0].action.arg != NULL, "arg 0 not null");
	ASSERT(strcmp(c->xkb[0].action.arg, "alacritty") == 0, "arg 0");
	ASSERT(c->xkb[1].action.kind == ACTION_SPAWN, "kind 1");
	ASSERT(strcmp(c->xkb[1].action.arg, "alacritty") == 0, "arg 1");
	/* Each expansion is a fresh deep-copy: changing one must not
	 * alias the other. The strings are different allocations. */
	ASSERT(c->xkb[0].action.arg != c->xkb[1].action.arg,
	       "expansions must not alias");
	config_free(c);
	return 1;
}

static int test_defalias_atom(void)
{
	struct Config *c = config_parse(
		"(defalias k close)\n"
		"(defkeys (M q @k))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_CLOSE, "kind");
	ASSERT(c->xkb[0].action.arg == NULL, "arg");
	config_free(c);
	return 1;
}

static int test_defalias_multi_use(void)
{
	struct Config *c = config_parse(
		"(defalias ws1 (workspace 1))\n"
		"(defalias ws2 (workspace 2))\n"
		"(defkeys (M 1 @ws1) (M 2 @ws2) (M-S 1 @ws1))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_xkb == 3, "n_xkb");
	ASSERT(c->xkb[0].action.kind == ACTION_WORKSPACE, "k0");
	ASSERT(strcmp(c->xkb[0].action.arg, "1") == 0, "a0");
	ASSERT(c->xkb[1].action.kind == ACTION_WORKSPACE, "k1");
	ASSERT(strcmp(c->xkb[1].action.arg, "2") == 0, "a1");
	ASSERT(c->xkb[2].action.kind == ACTION_WORKSPACE, "k2");
	ASSERT(strcmp(c->xkb[2].action.arg, "1") == 0, "a2");
	config_free(c);
	return 1;
}

static int test_err_undefined_alias(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defkeys (M q @nonesuch))\n", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "@nonesuch") != NULL, "err = %s", err);
	return 1;
}

static int test_err_alias_redefined(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defalias k close)\n"
		"(defalias k exit)\n", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "redefined") != NULL, "err = %s", err);
	return 1;
}

static int test_err_alias_bad_form(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defalias)\n", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "defalias") != NULL, "err = %s", err);
	return 1;
}

static int test_defscratchpads_basic(void)
{
	struct Config *c = config_parse(
		"(defscratchpads\n"
		"  (term (spawn \"alacritty --class scratch\")\n"
		"        (app-id \"scratch\")\n"
		"        :geometry (0.1 0.1 0.8 0.8))\n"
		"  (calc (spawn \"qalculate-gtk\")\n"
		"        (app-id \"Qalculate-gtk\")))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_scratchpads == 2, "n_scratchpads = %zu", c->n_scratchpads);
	ASSERT(strcmp(c->scratchpads[0].name, "term") == 0, "name 0 = %s",
	       c->scratchpads[0].name);
	ASSERT(strcmp(c->scratchpads[0].spawn,
		      "alacritty --class scratch") == 0,
	       "spawn 0 = %s", c->scratchpads[0].spawn);
	ASSERT(c->scratchpads[0].match_kind == RULE_MATCH_APP_ID, "match_kind 0");
	ASSERT(strcmp(c->scratchpads[0].match_pattern, "scratch") == 0,
	       "match 0 = %s", c->scratchpads[0].match_pattern);
	ASSERT(c->scratchpads[0].geom.set, "geom 0 should be set");
	ASSERT(c->scratchpads[0].geom.x == 0.1f, "geom.x");
	ASSERT(c->scratchpads[0].geom.w == 0.8f, "geom.w");
	ASSERT(!c->scratchpads[1].geom.set, "geom 1 should be unset");
	config_free(c);
	return 1;
}

static int test_defscratchpads_action(void)
{
	struct Config *c = config_parse(
		"(defkeys (A grave (scratchpad term))\n"
		"         (A-S grave toggle-scratchpad))\n"
		"(defscratchpads (term (spawn \"foot\") (app-id \"t\")))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_NAMED_SCRATCHPAD, "kind 0");
	ASSERT(strcmp(c->xkb[0].action.arg, "term") == 0, "arg 0 = %s",
	       c->xkb[0].action.arg);
	ASSERT(c->xkb[1].action.kind == ACTION_TOGGLE_SCRATCHPAD, "kind 1");
	ASSERT(c->xkb[1].action.arg == NULL, "arg 1");
	config_free(c);
	return 1;
}

static int test_defscratchpads_glob(void)
{
	struct Config *c = config_parse(
		"(defscratchpads\n"
		"  (notes (spawn \"obsidian\")\n"
		"         (title \"[Nn]otes*\") :glob true))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->scratchpads[0].glob, "glob should be true");
	ASSERT(c->scratchpads[0].match_kind == RULE_MATCH_TITLE, "title match");
	config_free(c);
	return 1;
}

static int test_err_defscratchpads_bad_geometry(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defscratchpads\n"
		"  (term (spawn \"x\") (app-id \"y\")\n"
		"        :geometry (1.5 0.1 0.1 0.1)))\n",
		err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "0..1") != NULL, "err = %s", err);
	return 1;
}

static int test_err_defscratchpads_parent_matcher(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defscratchpads\n"
		"  (term (spawn \"x\") (parent)))\n",
		err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "matcher") != NULL, "err = %s", err);
	return 1;
}

static int test_defcfg_smart_borders_and_gaps(void)
{
	struct Config *c = config_parse(
		"(defcfg\n"
		"  (borders :width 3 :smart true)\n"
		"  (gaps :inner 5 :smart true))",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->borders.smart, "borders.smart should be true");
	ASSERT(c->gaps.smart, "gaps.smart should be true");
	ASSERT(c->borders.width == 3, "width preserved");
	ASSERT(c->gaps.inner == 5, "inner preserved");
	config_free(c);
	return 1;
}

static int test_action_resize_delta(void)
{
	struct Config *c = config_parse("(defkeys (A-C h (resize left -30))\n"
					"         (A-C l (resize right +30)))",
					NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_RESIZE_DELTA, "kind 0");
	ASSERT(strcmp(c->xkb[0].action.arg, "left -30") == 0, "arg 0 = %s",
	       c->xkb[0].action.arg);
	ASSERT(c->xkb[1].action.kind == ACTION_RESIZE_DELTA, "kind 1");
	ASSERT(strcmp(c->xkb[1].action.arg, "right +30") == 0, "arg 1 = %s",
	       c->xkb[1].action.arg);
	config_free(c);
	return 1;
}

static int test_err_resize_bad_edge(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defkeys (A h (resize sideways 30)))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "left / right / up / down") != NULL, "err = %s",
	       err);
	return 1;
}

static int test_defsubmap_basic(void)
{
	struct Config *c = config_parse(
		"(defsubmap nav\n"
		"  (g focus-next)\n"
		"  (w focus-prev)\n"
		"  (Return (spawn \"alacritty\")))\n",
		NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->n_submaps == 1, "n_submaps = %zu", c->n_submaps);
	ASSERT(strcmp(c->submaps[0].name, "nav") == 0, "name = %s",
	       c->submaps[0].name);
	ASSERT(c->submaps[0].n_entries == 3, "n_entries = %zu",
	       c->submaps[0].n_entries);
	ASSERT(c->submaps[0].entries[0].action.kind == ACTION_FOCUS_NEXT,
	       "entry 0 kind");
	ASSERT(c->submaps[0].entries[2].action.kind == ACTION_SPAWN,
	       "entry 2 kind");
	ASSERT(strcmp(c->submaps[0].entries[2].action.arg, "alacritty") == 0,
	       "entry 2 arg = %s", c->submaps[0].entries[2].action.arg);
	config_free(c);
	return 1;
}

static int test_defsubmap_action(void)
{
	struct Config *c = config_parse("(defkeys (A s (submap nav)))\n"
					"(defsubmap nav (g close))",
					NULL, 0);
	ASSERT(c != NULL, "parse failed");
	ASSERT(c->xkb[0].action.kind == ACTION_SUBMAP, "kind");
	ASSERT(strcmp(c->xkb[0].action.arg, "nav") == 0, "arg = %s",
	       c->xkb[0].action.arg);
	config_free(c);
	return 1;
}

static int test_err_duplicate_chords(void)
{
	static const char *const srcs[] = {
		"(defkeys (M j focus-next) (M-S x close) (M j focus-prev))",
		"(defkeys (M-S j focus-next) (S-M j close))",
		"(defmouse (M left move-window) (M left resize-window))",
		"(defsubmap nav (g focus-next) (g focus-prev))",
	};
	char err[256];

	for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++) {
		struct Config *c = config_parse(srcs[i], err, sizeof(err));

		ASSERT(c == NULL, "expected NULL for %s", srcs[i]);
		ASSERT(strstr(err, "bound twice") != NULL, "err = %s", err);
	}
	return 1;
}

static int test_keymap_block(void)
{
	char err[256];
	struct Config *c;

	c = config_parse("(defcfg (keymap :layout \"us,dk\" "
			 ":options \"grp:alt_shift_toggle,ctrl:nocaps\"))",
			 err, sizeof(err));
	ASSERT(c != NULL, "err = %s", err);
	ASSERT(c->input.keymap_set, "keymap_set");
	ASSERT(strcmp(c->input.xkb[XKB_NAME_LAYOUT], "us,dk") == 0, "layout");
	ASSERT(c->input.xkb[XKB_NAME_VARIANT] == NULL, "variant");
	config_free(c);

	c = config_parse("(defcfg (keymap :layout \"nosuchlayout\"))", err,
			 sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "does not compile") != NULL, "err = %s", err);

	c = config_parse("(defcfg (keymap :lyout \"us\"))", err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "unknown keymap keyword") != NULL, "err = %s", err);
	return 1;
}

static int test_err_defsubmap_redefined(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defsubmap nav (g focus-next))\n"
		"(defsubmap nav (w focus-prev))\n",
		err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "redefined") != NULL, "err = %s", err);
	return 1;
}

static int test_err_defsubmap_bad_entry(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defsubmap nav (g))\n",
		err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	ASSERT(strstr(err, "submap entry") != NULL, "err = %s", err);
	return 1;
}

static int test_defcfg_smart_default_off(void)
{
	struct Config *c = config_default();
	ASSERT(c != NULL, "config_default returned NULL");
	ASSERT(!c->borders.smart, "borders.smart should default false");
	ASSERT(!c->gaps.smart, "gaps.smart should default false");
	config_free(c);
	return 1;
}

static int test_err_position_info(void)
{
	char err[256];
	struct Config *c = config_parse(
		"(defkeys\n"
		"  (A q close)\n"
		"  (X q close))\n",
		err, sizeof(err));
	ASSERT(c == NULL, "expected NULL");
	/* The bad entry is on line 3 where (X is the offending atom. */
	ASSERT(strstr(err, "line 3") != NULL, "err lacks line 3: %s", err);
	return 1;
}

static int test_err_bad_action_args(void)
{
	static const struct {
		const char *src;
		const char *needle;
	} cases[] = {
		{ "(defkeys (A 1 (workspace foo)))", "integer" },
		{ "(defkeys (A 1 (workspace 0)))", "integer" },
		{ "(defkeys (A 1 (workspace 10)))", "integer" },
		{ "(defkeys (A 1 (move-to-workspace -1)))", "integer" },
		{ "(defkeys (A 1 (focus-output x)))", "integer" },
		{ "(defkeys (A r (resize left abc)))", "integer" },
		{ "(defkeys (A r (resize left 99999999999999999999)))",
		  "integer" },
		{ "(defkeys (A s (submap nope)))", "unknown submap" },
		{ "(defkeys (A s (scratchpad nope)))", "unknown scratchpad" },
		{ "(defsubmap m (g (submap other)))\n"
		  "(defkeys (A s (submap m)))",
		  "unknown submap" },
	};
	char err[256];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct Config *c = config_parse(cases[i].src, err, sizeof(err));

		ASSERT(c == NULL, "expected NULL for %s", cases[i].src);
		ASSERT(strstr(err, cases[i].needle) != NULL, "err = %s", err);
	}
	return 1;
}

static int test_geometry_fraction_forms(void)
{
	static const char *const bad[] = { ".", "1.", "-0.5", "x" };
	char src[256];
	struct Config *c;
	char err[256];

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		snprintf(src, sizeof(src),
			 "(defscratchpads (t (spawn \"foot\") (app-id \"t\")"
			 " :geometry (%s 0 1 1)))",
			 bad[i]);
		c = config_parse(src, err, sizeof(err));
		ASSERT(c == NULL, "expected NULL for %s", bad[i]);
	}
	c = config_parse("(defscratchpads (t (spawn \"foot\") (app-id \"t\")"
			 " :geometry (.5 0 0.25 1)))",
			 err, sizeof(err));
	ASSERT(c != NULL, "err = %s", err);
	ASSERT(c->scratchpads[0].geom.x == 0.5f, "x = %f",
	       (double)c->scratchpads[0].geom.x);
	config_free(c);
	return 1;
}

static int test_geometry_must_fit(void)
{
	static const struct {
		const char *geom;
		bool ok;
	} cases[] = {
		{ "0 0 1 1", true },	       { "0.1 0 0.9 1", true },
		{ "0.25 0.5 0.75 0.5", true }, { "0.5 0 0.6 1", false },
		{ "0 0.3 1 0.8", false },      { "1 1 1 1", false },
	};
	struct Config *c;
	char src[256];
	char err[256];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		snprintf(src, sizeof(src),
			 "(defscratchpads (t (spawn \"foot\") (app-id \"t\")"
			 " :geometry (%s)))",
			 cases[i].geom);
		c = config_parse(src, err, sizeof(err));
		ASSERT((c != NULL) == cases[i].ok, "(%s): %s", cases[i].geom,
		       c != NULL ? "accepted" : err);
		if (!cases[i].ok)
			ASSERT(strstr(err, "must fit") != NULL, "err = %s",
			       err);
		config_free(c);
	}
	return 1;
}

static int test_defcfg_input_blocks(void)
{
	struct Config *c;
	char err[256];

	c = config_parse("(defcfg (repeat :rate 50 :delay 300)\n"
			 "        (cursor :theme \"Adwaita\" :size 32)\n"
			 "        (touchpad :tap true :natural-scroll true\n"
			 "                  :dwt false))",
			 err, sizeof(err));
	ASSERT(c != NULL, "err = %s", err);
	ASSERT(c->input.repeat_set, "repeat_set");
	ASSERT(c->input.repeat_rate == 50 && c->input.repeat_delay == 300,
	       "repeat %d %d", c->input.repeat_rate, c->input.repeat_delay);
	ASSERT(strcmp(c->input.cursor_theme, "Adwaita") == 0, "theme");
	ASSERT(c->input.cursor_size == 32, "size %u", c->input.cursor_size);
	ASSERT(c->input.tap == 1 && c->input.natural_scroll == 1, "tap/ns");
	ASSERT(c->input.dwt == 0, "dwt %d", c->input.dwt);
	ASSERT(c->input.middle_emulation == -1, "middle untouched");
	config_free(c);

	c = config_parse("(defcfg (gaps :top 1))", err, sizeof(err));
	ASSERT(c != NULL, "err = %s", err);
	ASSERT(!c->input.repeat_set && c->input.cursor_theme == NULL &&
		       c->input.tap == -1,
	       "unset input stays untouched");
	config_free(c);

	ASSERT(config_parse("(defcfg (cursor :size 24))", err, sizeof(err)) ==
		       NULL,
	       "cursor without theme");
	ASSERT(config_parse("(defcfg (repeat :rate -1))", err, sizeof(err)) ==
		       NULL,
	       "negative rate");
	ASSERT(config_parse("(defcfg (touchpad :tapp true))", err,
			    sizeof(err)) == NULL,
	       "unknown touchpad key");
	return 1;
}

static int test_action_refs_resolve_forward(void)
{
	char err[256];
	struct Config *c;

	c = config_parse("(defkeys (A s (submap m)) (A t (scratchpad t)))\n"
			 "(defsubmap m (g close))\n"
			 "(defscratchpads (t (spawn \"foot\") (app-id \"t\")))",
			 err, sizeof(err));
	ASSERT(c != NULL, "err = %s", err);
	config_free(c);
	return 1;
}

static const char describe_prelude[] =
	"(defsubmap sm (g close))\n"
	"(defscratchpads (sp (spawn \"x\") (app-id \"x\")))\n";

static const struct Sexpr *describe_kw(const struct Sexpr *form, const char *kw)
{
	for (size_t i = 2; i + 1 < form->u.list.n; i++)
		if (form->u.list.items[i]->kind == SEXPR_ATOM &&
		    strcmp(form->u.list.items[i]->u.atom, kw) == 0)
			return form->u.list.items[i + 1];
	return NULL;
}

static bool describe_parse(const char *form, enum ActionKind *kind)
{
	struct Config *c;
	char src[2048];
	char err[256];

	snprintf(src, sizeof(src), "%s%s", describe_prelude, form);
	c = config_parse(src, err, sizeof(err));
	if (c == NULL)
		return false;
	if (kind != NULL)
		*kind = c->xkb[0].action.kind;
	config_free(c);
	return true;
}

static bool describe_try(const char *binding, enum ActionKind *kind)
{
	char src[256];

	snprintf(src, sizeof(src), "(defkeys (A q %s))", binding);
	return describe_parse(src, kind);
}

static const char *describe_sample(const struct Sexpr *type, char *buf,
				   size_t len, bool bad)
{
	const char *head;

	if (type->kind == SEXPR_ATOM && strcmp(type->u.atom, "string") == 0) {
		snprintf(buf, len, bad ? "x" : "\"x\"");
		return buf;
	}
	if (type->kind == SEXPR_ATOM && strcmp(type->u.atom, "text") == 0) {
		snprintf(buf, len, bad ? "(x)" : "\"\"");
		return buf;
	}
	if (type->kind == SEXPR_ATOM && strcmp(type->u.atom, "bool") == 0) {
		snprintf(buf, len, bad ? "maybe" : "true");
		return buf;
	}
	if (type->kind == SEXPR_ATOM && strcmp(type->u.atom, "color") == 0) {
		snprintf(buf, len, bad ? "\"#12\"" : "\"#123456\"");
		return buf;
	}
	if (type->kind == SEXPR_ATOM) {
		static const struct {
			const char *type;
			const char *good;
			const char *bad;
		} simple[] = {
			{ "atom", "x", "\"x\"" },
			{ "any", "close", NULL },
			{ "fraction", "0.5", "1.5" },
			{ "mods", "M", "Z" },
			{ "keysym", "a", "NoSuchKey" },
			{ "action", "close", "nonsense" },
			{ "matcher", "(app-id \"x\")", "(kind window)" },
		};

		for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]);
		     i++) {
			if (strcmp(type->u.atom, simple[i].type) != 0)
				continue;
			if (bad && simple[i].bad == NULL)
				return NULL;
			snprintf(buf, len, "%s",
				 bad ? simple[i].bad : simple[i].good);
			return buf;
		}
		return NULL;
	}
	if (type->kind != SEXPR_LIST || type->u.list.n < 2)
		return NULL;
	head = type->u.list.items[0]->u.atom;
	if (strcmp(head, "list-of") == 0 || strcmp(head, "rest-of") == 0) {
		bool paren = strcmp(head, "list-of") == 0;
		long n = strtol(type->u.list.items[2]->u.atom, NULL, 10);
		char one[64];
		size_t off;

		off = (size_t)snprintf(buf, len, "%s", paren ? "(" : "");
		for (long i = 0; i < n; i++) {
			if (describe_sample(type->u.list.items[1], one,
					    sizeof(one),
					    bad && i + 1 == n) == NULL)
				return NULL;
			off += (size_t)snprintf(buf + off, len - off, "%s%s",
						i ? " " : "", one);
		}
		snprintf(buf + off, len - off, "%s", paren ? ")" : "");
		return buf;
	}
	if (strcmp(head, "patterns") == 0) {
		const struct Sexpr *pat = type->u.list.items[1];

		if (bad) {
			snprintf(buf, len, "(nosuch)");
			for (size_t i = 1; i < type->u.list.n; i++)
				if (strcmp(type->u.list.items[i]
						   ->u.list.items[0]
						   ->u.atom,
					   "parent") == 0)
					return buf;
			snprintf(buf, len, "(parent)");
			return buf;
		}
		snprintf(buf, len, "(%s%s)", pat->u.list.items[0]->u.atom,
			 pat->u.list.n > 1 ? " \"x\"" : "");
		return buf;
	}
	if (strcmp(head, "ref") == 0) {
		snprintf(buf, len, "%s", bad ? "(nosuch)" : "(layout tall)");
		return buf;
	}
	if (strcmp(head, "integer") == 0) {
		if (bad)
			snprintf(buf, len, "%ld",
				 strtol(type->u.list.items[2]->u.atom, NULL,
					10) +
					 1);
		else
			snprintf(buf, len, "%s", type->u.list.items[1]->u.atom);
		return buf;
	}
	if (strcmp(head, "one-of") == 0) {
		snprintf(buf, len, "%s",
			 bad ? "nowhere" : type->u.list.items[1]->u.atom);
		return buf;
	}
	if (strcmp(head, "name") == 0) {
		if (strcmp(type->u.list.items[1]->u.atom, "defsubmap") == 0)
			snprintf(buf, len, "%s", bad ? "nosuch" : "sm");
		else
			snprintf(buf, len, "%s", bad ? "nosuch" : "sp");
		return buf;
	}
	return NULL;
}

static bool describe_placeholder(const struct Sexpr *s)
{
	if (s->kind != SEXPR_ATOM || s->u.atom[0] == '\0')
		return false;
	for (const char *p = s->u.atom; *p; p++)
		if ((*p < 'A' || *p > 'Z') && *p != '_')
			return false;
	return true;
}

static const struct Sexpr *describe_type_of(const struct Sexpr *types,
					    const char *ph)
{
	for (size_t i = 0; i < types->u.list.n; i++)
		if (strcmp(types->u.list.items[i]->u.list.items[0]->u.atom,
			   ph) == 0)
			return types->u.list.items[i]->u.list.items[1];
	return NULL;
}

static bool describe_render(const struct Sexpr *pat, const struct Sexpr *types,
			    const char *bad_ph, char *buf, size_t len)
{
	const struct Sexpr *type;
	char one[256];
	size_t off;
	bool bad;

	if (describe_placeholder(pat)) {
		type = describe_type_of(types, pat->u.atom);
		if (type == NULL)
			return false;
		bad = bad_ph != NULL && strcmp(bad_ph, pat->u.atom) == 0;
		if (describe_sample(type, one, sizeof(one), bad) == NULL)
			return false;
		snprintf(buf, len, "%s", one);
		return true;
	}
	if (pat->kind == SEXPR_ATOM) {
		snprintf(buf, len, "%s", pat->u.atom);
		return true;
	}
	off = (size_t)snprintf(buf, len, "(");
	for (size_t i = 0; i < pat->u.list.n; i++) {
		if (!describe_render(pat->u.list.items[i], types, bad_ph, one,
				     sizeof(one)))
			return false;
		off += (size_t)snprintf(buf + off, len - off, "%s%s",
					i ? " " : "", one);
	}
	snprintf(buf + off, len - off, ")");
	return true;
}

static bool describe_form_src(const char *name, const struct Sexpr *head,
			      const struct Sexpr *entry_pat,
			      const struct Sexpr *types, const char *bad_ph,
			      const char *extra, char *buf, size_t len)
{
	char part[256];
	size_t off;

	off = (size_t)snprintf(buf, len, "(%s", name);
	if (head != NULL) {
		for (size_t i = 0; i < head->u.list.n; i++) {
			if (!describe_render(head->u.list.items[i], types,
					     bad_ph, part, sizeof(part)))
				return false;
			off += (size_t)snprintf(buf + off, len - off, " %s",
						part);
		}
	}
	if (entry_pat != NULL) {
		if (!describe_render(entry_pat, types, bad_ph, part,
				     sizeof(part)))
			return false;
		if (extra[0] != '\0' && part[strlen(part) - 1] == ')')
			part[strlen(part) - 1] = '\0';
		off += (size_t)snprintf(buf + off, len - off, " %s%s%s", part,
					extra, extra[0] != '\0' ? ")" : "");
	}
	snprintf(buf + off, len - off, ")");
	return true;
}

static bool describe_has_key(const struct Sexpr *keys, const char *k)
{
	if (keys == NULL)
		return false;
	for (size_t i = 0; i < keys->u.list.n; i++)
		if (strcmp(keys->u.list.items[i]->u.list.items[0]->u.atom, k) ==
		    0)
			return true;
	return false;
}

static int describe_check_entry(const char *name, const struct Sexpr *head,
				const struct Sexpr *entries, size_t e,
				const struct Sexpr *types)
{
	const struct Sexpr *entry = entries != NULL ? entries->u.list.items[e] :
						      NULL;
	const struct Sexpr *pat = entry != NULL ? entry->u.list.items[0] : NULL;
	const struct Sexpr *keys = entry != NULL && entry->u.list.n > 2 ?
					   entry->u.list.items[2] :
					   NULL;
	char good[1024];
	char extra[256];
	char src[1024];
	char tmp[128];

	ASSERT(describe_form_src(name, head, pat, types, NULL, "", good,
				 sizeof(good)),
	       "%s: cannot render", name);
	ASSERT(describe_parse(good, NULL), "%s rejected", good);
	for (size_t i = 0; i < types->u.list.n; i++) {
		const char *ph =
			types->u.list.items[i]->u.list.items[0]->u.atom;

		if (!describe_form_src(name, head, pat, types, ph, "", src,
				       sizeof(src)) ||
		    strcmp(src, good) == 0)
			continue;
		ASSERT(!describe_parse(src, NULL), "%s accepted", src);
	}
	if (pat == NULL || pat->kind != SEXPR_LIST)
		return 1;
	if (keys != NULL) {
		describe_form_src(name, head, pat, types, NULL, " :nosuch 1",
				  src, sizeof(src));
		ASSERT(!describe_parse(src, NULL), "%s accepted", src);
		for (size_t k = 0; k < keys->u.list.n; k++) {
			const struct Sexpr *key = keys->u.list.items[k];
			const char *kname = key->u.list.items[0]->u.atom;

			ASSERT(describe_kw(key, ":doc") != NULL,
			       "%s %s has no doc", name, kname);
			ASSERT(describe_sample(key->u.list.items[1], tmp,
					       sizeof(tmp), false) != NULL,
			       "%s %s: unknown type", name, kname);
			snprintf(extra, sizeof(extra), " %s %s", kname, tmp);
			describe_form_src(name, head, pat, types, NULL, extra,
					  src, sizeof(src));
			ASSERT(describe_parse(src, NULL), "%s rejected", src);
			describe_sample(key->u.list.items[1], tmp, sizeof(tmp),
					true);
			snprintf(extra, sizeof(extra), " %s %s", kname, tmp);
			describe_form_src(name, head, pat, types, NULL, extra,
					  src, sizeof(src));
			ASSERT(!describe_parse(src, NULL), "%s accepted", src);
		}
	}
	for (size_t o = 0; entries != NULL && o < entries->u.list.n; o++) {
		const struct Sexpr *other = entries->u.list.items[o];
		const struct Sexpr *okeys;

		if (o == e || other->u.list.n <= 2)
			continue;
		okeys = other->u.list.items[2];
		for (size_t k = 0; k < okeys->u.list.n; k++) {
			const char *kname =
				okeys->u.list.items[k]->u.list.items[0]->u.atom;

			if (describe_has_key(keys, kname))
				continue;
			snprintf(extra, sizeof(extra), " %s true", kname);
			describe_form_src(name, head, pat, types, NULL, extra,
					  src, sizeof(src));
			ASSERT(!describe_parse(src, NULL), "%s accepted", src);
		}
	}
	return 1;
}

static int describe_check_form(const struct Sexpr *form)
{
	const char *name = form->u.list.items[1]->u.atom;
	const struct Sexpr *entries = describe_kw(form, ":entries");
	const struct Sexpr *types = describe_kw(form, ":types");
	const struct Sexpr *head = describe_kw(form, ":head");

	ASSERT(types != NULL, "%s has no :types", name);
	ASSERT(describe_kw(form, ":doc") != NULL, "%s has no doc", name);
	if (entries == NULL)
		return describe_check_entry(name, head, NULL, 0, types);
	for (size_t e = 0; e < entries->u.list.n; e++)
		ASSERT(describe_check_entry(name, head, entries, e, types),
		       "%s entry %zu", name, e);
	return 1;
}

static int describe_check_defcfg(const struct Sexpr *form)
{
	const char *name = form->u.list.items[1]->u.atom;
	const struct Sexpr *keys;
	const struct Sexpr *args;
	const struct Sexpr *type;
	char req[256] = "";
	char src[512];
	char tmp[64];

	keys = describe_kw(form, ":keys");
	args = describe_kw(form, ":args");
	ASSERT((keys == NULL) != (args == NULL), "%s: :keys or :args", name);
	ASSERT(describe_kw(form, ":doc") != NULL, "%s has no doc", name);
	if (args != NULL) {
		type = args->u.list.items[0]->u.list.items[1];
		ASSERT(describe_sample(type, tmp, sizeof(tmp), false) != NULL,
		       "%s: unknown arg type", name);
		snprintf(src, sizeof(src), "(defcfg (%s %s))", name, tmp);
		ASSERT(describe_parse(src, NULL), "%s rejected", src);
		describe_sample(type, tmp, sizeof(tmp), true);
		snprintf(src, sizeof(src), "(defcfg (%s %s))", name, tmp);
		ASSERT(!describe_parse(src, NULL), "%s accepted", src);
		return 1;
	}
	for (size_t k = 0; k < keys->u.list.n; k++) {
		const struct Sexpr *key = keys->u.list.items[k];

		if (describe_kw(key, ":required") == NULL)
			continue;
		describe_sample(key->u.list.items[1], tmp, sizeof(tmp), false);
		snprintf(req + strlen(req), sizeof(req) - strlen(req), " %s %s",
			 key->u.list.items[0]->u.atom, tmp);
	}
	snprintf(src, sizeof(src), "(defcfg (%s%s))", name, req);
	ASSERT(describe_parse(src, NULL), "%s rejected", src);
	if (req[0] != '\0') {
		snprintf(src, sizeof(src), "(defcfg (%s))", name);
		ASSERT(!describe_parse(src, NULL), "%s accepted", src);
	}
	snprintf(src, sizeof(src), "(defcfg (%s%s :nosuch 1))", name, req);
	ASSERT(!describe_parse(src, NULL), "%s accepted", src);
	for (size_t k = 0; k < keys->u.list.n; k++) {
		const struct Sexpr *key = keys->u.list.items[k];
		const char *kname = key->u.list.items[0]->u.atom;

		ASSERT(describe_kw(key, ":doc") != NULL, "%s %s has no doc",
		       name, kname);
		ASSERT(describe_sample(key->u.list.items[1], tmp, sizeof(tmp),
				       false) != NULL,
		       "%s %s: unknown type", name, kname);
		snprintf(src, sizeof(src), "(defcfg (%s%s %s %s))", name, req,
			 kname, tmp);
		ASSERT(describe_parse(src, NULL), "%s rejected", src);
		describe_sample(key->u.list.items[1], tmp, sizeof(tmp), true);
		snprintf(src, sizeof(src), "(defcfg (%s%s %s %s))", name, req,
			 kname, tmp);
		ASSERT(!describe_parse(src, NULL), "%s accepted", src);
	}
	return 1;
}

static int test_describe_matches_parser(void)
{
	bool seen[ACTION_FOCUS_MRU_PREV + 1] = { false };
	const struct Sexpr *schema;
	size_t n_defcfg = 0;
	size_t n_forms = 0;
	struct Sexpr *root;
	char err[256];
	size_t len;
	char *text;
	FILE *mem;

	mem = open_memstream(&text, &len);
	ASSERT(mem != NULL, "open_memstream");
	config_describe(mem);
	fclose(mem);
	root = sexpr_parse(text, err, sizeof(err));
	free(text);
	ASSERT(root != NULL, "describe output does not parse: %s", err);
	schema = root->u.list.items[0];
	ASSERT(schema->kind == SEXPR_LIST && schema->u.list.n > 2 &&
		       strcmp(schema->u.list.items[0]->u.atom,
			      "nomonad-schema") == 0,
	       "bad schema head");
	for (size_t i = 2; i < schema->u.list.n; i++) {
		const struct Sexpr *form = schema->u.list.items[i];
		const struct Sexpr *args;
		const struct Sexpr *doc;
		enum ActionKind kind;
		const char *name;
		char good[256];
		char bad[256];
		char tmp[64];

		ASSERT(form->kind == SEXPR_LIST && form->u.list.n >= 4,
		       "entry %zu is malformed", i);
		if (strcmp(form->u.list.items[0]->u.atom, "type") == 0) {
			ASSERT(describe_kw(form, ":doc") != NULL ||
				       (form->u.list.n > 3 &&
					form->u.list.items[2]->kind ==
						SEXPR_ATOM &&
					strcmp(form->u.list.items[2]->u.atom,
					       ":doc") == 0),
			       "type entry %zu has no doc", i);
			continue;
		}
		if (strcmp(form->u.list.items[0]->u.atom, "form") == 0) {
			ASSERT(describe_check_form(form), "form entry %zu", i);
			n_forms++;
			continue;
		}
		if (strcmp(form->u.list.items[0]->u.atom, "defcfg") == 0) {
			ASSERT(describe_check_defcfg(form), "defcfg entry %zu",
			       i);
			n_defcfg++;
			continue;
		}
		ASSERT(strcmp(form->u.list.items[0]->u.atom, "action") == 0,
		       "entry %zu is not an action", i);
		name = form->u.list.items[1]->u.atom;
		doc = describe_kw(form, ":doc");
		ASSERT(doc != NULL && doc->kind == SEXPR_STRING &&
			       doc->u.string[0] != '\0',
		       "%s has no doc", name);
		args = describe_kw(form, ":args");
		if (args == NULL) {
			ASSERT(describe_try(name, &kind), "%s rejected", name);
			snprintf(bad, sizeof(bad), "(%s)", name);
			ASSERT(!describe_try(bad, NULL), "%s accepted", bad);
		} else {
			snprintf(good, sizeof(good), "(%s", name);
			snprintf(bad, sizeof(bad), "(%s", name);
			for (size_t a = 0; a < args->u.list.n; a++) {
				const struct Sexpr *type =
					args->u.list.items[a]->u.list.items[1];

				ASSERT(describe_sample(type, tmp, sizeof(tmp),
						       false) != NULL,
				       "%s: unknown arg type", name);
				strcat(good, " ");
				strcat(good, tmp);
				describe_sample(type, tmp, sizeof(tmp),
						a + 1 == args->u.list.n);
				strcat(bad, " ");
				strcat(bad, tmp);
			}
			strcat(good, ")");
			strcat(bad, ")");
			ASSERT(describe_try(good, &kind), "%s rejected", good);
			ASSERT(!describe_try(bad, NULL), "%s accepted", bad);
			ASSERT(!describe_try(name, NULL), "bare %s accepted",
			       name);
		}
		ASSERT(kind <= ACTION_FOCUS_MRU_PREV, "%s: kind out of range",
		       name);
		seen[kind] = true;
	}
	sexpr_free(root);
	ASSERT(n_defcfg == 9, "expected 9 defcfg blocks, got %zu", n_defcfg);
	ASSERT(n_forms == 7, "expected 7 forms, got %zu", n_forms);
	for (int k = ACTION_NONE + 1; k <= ACTION_FOCUS_MRU_PREV; k++)
		ASSERT(seen[k], "action kind %d missing from the schema", k);
	return 1;
}

static int test_err_action_arity(void)
{
	char err[256];
	struct Config *c;

	c = config_parse("(defkeys (A q (close)))", err, sizeof(err));
	ASSERT(c == NULL && strstr(err, "takes no argument") != NULL,
	       "err = %s", err);
	c = config_parse("(defkeys (A q workspace))", err, sizeof(err));
	ASSERT(c == NULL && strstr(err, "takes an argument") != NULL,
	       "err = %s", err);
	return 1;
}

int main(void)
{
	RUN(default_config_parses);
	RUN(describe_matches_parser);
	RUN(err_action_arity);
	RUN(mods_no_modifier);
	RUN(mods_single);
	RUN(mods_combined);
	RUN(keysym_resolution);
	RUN(actions_bare);
	RUN(actions_focus_swap);
	RUN(actions_master_and_float);
	RUN(actions_bsp);
	RUN(action_spawn);
	RUN(action_reload_config);
	RUN(action_workspace);
	RUN(action_move_to_workspace);
	RUN(actions_workspace_and_output);
	RUN(buttons);
	RUN(combined_forms);
	RUN(comments);
	RUN(empty_input);
	RUN(err_unknown_form);
	RUN(err_bad_mod);
	RUN(err_unknown_key);
	RUN(err_unknown_action);
	RUN(err_unknown_button);
	RUN(err_spawn_missing_arg);
	RUN(err_spawn_atom_arg);
	RUN(err_workspace_no_arg);
	RUN(err_malformed_entry);
	RUN(defrules_app_id);
	RUN(defrules_title_with_center);
	RUN(defrules_parent_and_kind_dialog);
	RUN(defrules_multiple);
	RUN(defrules_ignore);
	RUN(defrules_glob);
	RUN(err_ignore_with_center);
	RUN(err_defrules_unknown_matcher);
	RUN(err_defrules_bad_keyword);
	RUN(defcfg_defaults);
	RUN(defcfg_borders);
	RUN(defcfg_gaps);
	RUN(defcfg_borders_disabled);
	RUN(err_defcfg_bad_color);
	RUN(err_defcfg_negative);
	RUN(defcfg_layout_default_tall);
	RUN(defcfg_layout_bsp);
	RUN(defcfg_layout_deck);
	RUN(defcfg_layout_mirror_tall);
	RUN(defcfg_layout_three_columns);
	RUN(defcfg_layout_grid);
	RUN(defcfg_layout_tabbed);
	RUN(defcfg_layout_spiral);
	RUN(defcfg_layout_two_pane);
	RUN(defcfg_layout_resizable_tall);
	RUN(defcfg_layouts_multi);
	RUN(defcfg_layouts_single);
	RUN(defcfg_layouts_empty_errors);
	RUN(defcfg_layouts_unknown_errors);
	RUN(defcfg_decorations_default);
	RUN(defcfg_decorations_csd);
	RUN(defcfg_decorations_ssd);
	RUN(err_defcfg_unknown_decorations);
	RUN(err_defcfg_unknown_layout);
	RUN(err_defcfg_unknown_block);
	RUN(example_config_parses);
	RUN(defalias_basic);
	RUN(defalias_atom);
	RUN(defalias_multi_use);
	RUN(err_undefined_alias);
	RUN(err_alias_redefined);
	RUN(err_alias_bad_form);
	RUN(defscratchpads_basic);
	RUN(defscratchpads_action);
	RUN(defscratchpads_glob);
	RUN(err_defscratchpads_bad_geometry);
	RUN(err_defscratchpads_parent_matcher);
	RUN(defcfg_smart_borders_and_gaps);
	RUN(defcfg_smart_default_off);
	RUN(action_resize_delta);
	RUN(err_resize_bad_edge);
	RUN(defsubmap_basic);
	RUN(defsubmap_action);
	RUN(err_defsubmap_redefined);
	RUN(err_duplicate_chords);
	RUN(keymap_block);
	RUN(err_defsubmap_bad_entry);
	RUN(err_position_info);

	RUN(err_bad_action_args);
	RUN(action_refs_resolve_forward);
	RUN(defcfg_input_blocks);
	RUN(geometry_fraction_forms);
	RUN(geometry_must_fit);
	printf("%d/%d passed\n", g_passed, g_run);
	return g_run == g_passed ? 0 : 1;
}
