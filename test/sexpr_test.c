/* SPDX-License-Identifier: 0BSD */
/*
 * Tests for sexpr.c. Standalone: links sexpr.o and libc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int test_empty_input(void)
{
	struct Sexpr *r = sexpr_parse("", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->kind == SEXPR_LIST, "root is not a list");
	ASSERT(r->u.list.n == 0, "expected 0 items, got %zu", r->u.list.n);
	sexpr_free(r);
	return 1;
}

static int test_whitespace_only(void)
{
	struct Sexpr *r = sexpr_parse("  \t\n\r  ", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 0, "expected 0 items, got %zu", r->u.list.n);
	sexpr_free(r);
	return 1;
}

static int test_comments_only(void)
{
	struct Sexpr *r = sexpr_parse(";; first\n; second\n", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 0, "expected 0 items, got %zu", r->u.list.n);
	sexpr_free(r);
	return 1;
}

static int test_single_atom(void)
{
	struct Sexpr *r = sexpr_parse("foo", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 1, "expected 1 item, got %zu", r->u.list.n);
	struct Sexpr *a = r->u.list.items[0];
	ASSERT(a->kind == SEXPR_ATOM, "expected atom");
	ASSERT(strcmp(a->u.atom, "foo") == 0, "atom = %s", a->u.atom);
	sexpr_free(r);
	return 1;
}

static int test_multiple_atoms(void)
{
	struct Sexpr *r = sexpr_parse("foo bar baz", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 3, "expected 3 items");
	ASSERT(strcmp(r->u.list.items[0]->u.atom, "foo") == 0, "first");
	ASSERT(strcmp(r->u.list.items[1]->u.atom, "bar") == 0, "second");
	ASSERT(strcmp(r->u.list.items[2]->u.atom, "baz") == 0, "third");
	sexpr_free(r);
	return 1;
}

static int test_empty_list(void)
{
	struct Sexpr *r = sexpr_parse("()", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 1, "expected 1 form");
	struct Sexpr *l = r->u.list.items[0];
	ASSERT(l->kind == SEXPR_LIST, "form is not a list");
	ASSERT(l->u.list.n == 0, "list should be empty, got %zu", l->u.list.n);
	sexpr_free(r);
	return 1;
}

static int test_nested_list(void)
{
	struct Sexpr *r = sexpr_parse("(a (b c) d)", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	struct Sexpr *outer = r->u.list.items[0];
	ASSERT(outer->kind == SEXPR_LIST, "outer not list");
	ASSERT(outer->u.list.n == 3, "outer should have 3 items");
	ASSERT(outer->u.list.items[0]->kind == SEXPR_ATOM, "0 atom");
	ASSERT(outer->u.list.items[1]->kind == SEXPR_LIST, "1 list");
	ASSERT(outer->u.list.items[1]->u.list.n == 2, "inner has 2");
	ASSERT(outer->u.list.items[2]->kind == SEXPR_ATOM, "2 atom");
	ASSERT(strcmp(outer->u.list.items[2]->u.atom, "d") == 0, "last is d");
	sexpr_free(r);
	return 1;
}

static int test_simple_string(void)
{
	struct Sexpr *r = sexpr_parse("\"hello world\"", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 1, "1 form");
	struct Sexpr *s = r->u.list.items[0];
	ASSERT(s->kind == SEXPR_STRING, "expected string");
	ASSERT(strcmp(s->u.string, "hello world") == 0, "string = %s",
	       s->u.string);
	sexpr_free(r);
	return 1;
}

static int test_string_escapes(void)
{
	struct Sexpr *r = sexpr_parse("\"a\\nb\\tc\\\"d\\\\e\"", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	struct Sexpr *s = r->u.list.items[0];
	ASSERT(s->kind == SEXPR_STRING, "expected string");
	ASSERT(strcmp(s->u.string, "a\nb\tc\"d\\e") == 0, "got %s",
	       s->u.string);
	sexpr_free(r);
	return 1;
}

static int test_empty_string(void)
{
	struct Sexpr *r = sexpr_parse("\"\"", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	struct Sexpr *s = r->u.list.items[0];
	ASSERT(s->kind == SEXPR_STRING, "expected string");
	ASSERT(s->u.string[0] == '\0', "string not empty");
	sexpr_free(r);
	return 1;
}

static int test_atom_chars(void)
{
	const char *src = "M-S #21fc0d :center @term true 0.55 9 XF86AudioMute";
	struct Sexpr *r = sexpr_parse(src, NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 8, "expected 8 atoms, got %zu", r->u.list.n);
	const char *want[] = { "M-S", "#21fc0d", ":center", "@term",
			       "true", "0.55", "9", "XF86AudioMute" };
	for (size_t i = 0; i < 8; i++) {
		struct Sexpr *a = r->u.list.items[i];
		ASSERT(a->kind == SEXPR_ATOM, "item %zu not atom", i);
		ASSERT(strcmp(a->u.atom, want[i]) == 0, "item %zu = %s",
		       i, a->u.atom);
	}
	sexpr_free(r);
	return 1;
}

static int test_comments_inline(void)
{
	struct Sexpr *r = sexpr_parse("(a ; comment\n b) ; trailing\n c",
				      NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	ASSERT(r->u.list.n == 2, "expected 2 forms, got %zu", r->u.list.n);
	struct Sexpr *list = r->u.list.items[0];
	ASSERT(list->kind == SEXPR_LIST, "first not list");
	ASSERT(list->u.list.n == 2, "list should have 2 items");
	ASSERT(strcmp(list->u.list.items[0]->u.atom, "a") == 0, "a");
	ASSERT(strcmp(list->u.list.items[1]->u.atom, "b") == 0, "b");
	ASSERT(strcmp(r->u.list.items[1]->u.atom, "c") == 0, "c");
	sexpr_free(r);
	return 1;
}

static int test_position_tracking(void)
{
	struct Sexpr *r = sexpr_parse("  (foo)\n   bar", NULL, 0);
	ASSERT(r != NULL, "parse returned NULL");
	struct Sexpr *list = r->u.list.items[0];
	ASSERT(list->line == 1 && list->col == 3, "list at %d:%d",
	       list->line, list->col);
	struct Sexpr *foo = list->u.list.items[0];
	ASSERT(foo->line == 1 && foo->col == 4, "foo at %d:%d",
	       foo->line, foo->col);
	struct Sexpr *bar = r->u.list.items[1];
	ASSERT(bar->line == 2 && bar->col == 4, "bar at %d:%d",
	       bar->line, bar->col);
	sexpr_free(r);
	return 1;
}

static int test_xmonad_mock(void)
{
	const char *src =
		"(defcfg\n"
		"  mod        super\n"
		"  border     (width 3 focused #21fc0d unfocused #4c566a)\n"
		"  gaps       (top 25 bottom 1 left 1 right 1 inner 4)\n"
		"  focus-follows-mouse true\n"
		"  startup    \"$HOME/.config/nomonad/autostart.sh\")\n"
		"\n"
		"(defrules\n"
		"  (float (app-id \"Pavucontrol\"))\n"
		"  (float (app-id \"mpv\") :center true))\n"
		"\n"
		"(defkeys\n"
		"  (M       Return    @term)\n"
		"  (M-S     r         reload-config)\n"
		"  (()      XF86AudioMute  (spawn \"wpctl set-mute toggle\")))\n";
	char err[256];
	struct Sexpr *r = sexpr_parse(src, err, sizeof(err));
	ASSERT(r != NULL, "parse failed: %s", err);
	ASSERT(r->u.list.n == 3, "expected 3 top-level forms, got %zu",
	       r->u.list.n);
	for (size_t i = 0; i < 3; i++)
		ASSERT(r->u.list.items[i]->kind == SEXPR_LIST,
		       "form %zu not a list", i);
	sexpr_free(r);
	return 1;
}

static int test_error_unclosed_paren(void)
{
	char err[256];
	struct Sexpr *r = sexpr_parse("(a b", err, sizeof(err));
	ASSERT(r == NULL, "expected NULL on unclosed paren");
	ASSERT(strstr(err, "unclosed") != NULL, "err = %s", err);
	return 1;
}

static int test_error_unterminated_string(void)
{
	char err[256];
	struct Sexpr *r = sexpr_parse("\"hello", err, sizeof(err));
	ASSERT(r == NULL, "expected NULL");
	ASSERT(strstr(err, "unterminated") != NULL, "err = %s", err);
	return 1;
}

static int test_error_unexpected_close(void)
{
	char err[256];
	struct Sexpr *r = sexpr_parse("a)", err, sizeof(err));
	ASSERT(r == NULL, "expected NULL");
	ASSERT(strstr(err, "')'") != NULL, "err = %s", err);
	return 1;
}

static int test_error_unknown_escape(void)
{
	char err[256];
	struct Sexpr *r = sexpr_parse("\"a\\qb\"", err, sizeof(err));
	ASSERT(r == NULL, "expected NULL on unknown escape");
	ASSERT(strstr(err, "escape") != NULL, "err = %s", err);
	return 1;
}

static int test_error_too_deep(void)
{
	size_t depth = 100000;
	struct Sexpr *r;
	char err[256];
	char *src;

	src = malloc(depth * 2 + 1);
	ASSERT(src != NULL, "malloc");
	memset(src, '(', depth);
	memset(src + depth, ')', depth);
	src[depth * 2] = '\0';
	r = sexpr_parse(src, err, sizeof(err));
	ASSERT(r == NULL, "expected NULL on deep nesting");
	ASSERT(strstr(err, "too deeply") != NULL, "err = %s", err);
	src[depth] = '\0';
	r = sexpr_parse(src, err, sizeof(err));
	ASSERT(r == NULL, "expected NULL on deep unclosed nesting");
	free(src);
	return 1;
}

int main(void)
{
	RUN(empty_input);
	RUN(whitespace_only);
	RUN(comments_only);
	RUN(single_atom);
	RUN(multiple_atoms);
	RUN(empty_list);
	RUN(nested_list);
	RUN(simple_string);
	RUN(string_escapes);
	RUN(empty_string);
	RUN(atom_chars);
	RUN(comments_inline);
	RUN(position_tracking);
	RUN(xmonad_mock);
	RUN(error_unclosed_paren);
	RUN(error_unterminated_string);
	RUN(error_unexpected_close);
	RUN(error_too_deep);
	RUN(error_unknown_escape);

	printf("%d/%d passed\n", g_passed, g_run);
	return g_run == g_passed ? 0 : 1;
}
