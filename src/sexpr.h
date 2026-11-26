/* SPDX-License-Identifier: 0BSD */
/*
 * S-expression parser.
 *
 * Reads a buffer of source text into a tree of Sexpr nodes. Lists are
 * (...), strings are "...", everything else (identifiers, numbers,
 * #colors, :keywords, M-S mod combos, @aliases) becomes an atom. The
 * consumer in config.c does the semantic interpretation.
 *
 * The module is standalone: it depends on libc only. The test binary
 * links sexpr.o without dragging in wayland.
 */
#ifndef NOMONAD_SEXPR_H
#define NOMONAD_SEXPR_H

#include <stddef.h>

enum SexprKind {
	SEXPR_LIST,
	SEXPR_ATOM,
	SEXPR_STRING,
};

struct Sexpr {
	enum SexprKind kind;
	int line;
	int col;
	union {
		struct {
			struct Sexpr **items; /* array of n pointers */
			size_t n;
		} list;
		char *atom;   /* NUL-terminated, owned */
		char *string; /* NUL-terminated, owned (escapes processed) */
	} u;
};

/*
 * Parse `src` into a synthetic top-level list whose items are the
 * forms found at the top level. Returns NULL on error and writes a
 * "line:col: message" diagnostic into errbuf if errbuf_len > 0.
 *
 * The returned tree is owned by the caller and must be freed with
 * sexpr_free().
 */
struct Sexpr *sexpr_parse(const char *src, char *errbuf, size_t errbuf_len);
void sexpr_free(struct Sexpr *s);

/*
 * Deep-copy a Sexpr tree. Returns NULL on allocation failure or if
 * `s` is NULL. Used by the alias-substitution pass in config.c so
 * the same alias body can be expanded into multiple call sites
 * without aliasing memory between them.
 */
struct Sexpr *sexpr_dup(const struct Sexpr *s);

#endif /* NOMONAD_SEXPR_H */
