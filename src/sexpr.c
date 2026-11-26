/* SPDX-License-Identifier: 0BSD */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sexpr.h"

#define SEXPR_MAX_DEPTH 256

struct Parser {
	const char *src;
	size_t len;
	size_t pos;
	int line;
	int col;
	char *errbuf;
	size_t errbuf_len;
	int depth;
};

static void parser_error(struct Parser *p, const char *msg)
{
	if (p->errbuf == NULL || p->errbuf_len == 0)
		return;
	/* Only the first error is reported. */
	if (p->errbuf[0] != '\0')
		return;
	snprintf(p->errbuf, p->errbuf_len, "line %d:%d: %s", p->line, p->col,
		 msg);
}

static int peek(struct Parser *p)
{
	if (p->pos >= p->len)
		return -1;
	return (unsigned char)p->src[p->pos];
}

static int advance(struct Parser *p)
{
	int c = peek(p);
	if (c == -1)
		return -1;
	p->pos++;
	if (c == '\n') {
		p->line++;
		p->col = 1;
	} else {
		p->col++;
	}
	return c;
}

static void skip_ws(struct Parser *p)
{
	for (;;) {
		int c = peek(p);
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			advance(p);
		} else if (c == ';') {
			while (peek(p) != -1 && peek(p) != '\n')
				advance(p);
		} else {
			break;
		}
	}
}

static int is_atom_char(int c)
{
	if (c == -1)
		return 0;
	if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
		return 0;
	if (c == '(' || c == ')' || c == '"' || c == ';')
		return 0;
	return 1;
}

static struct Sexpr *parse_form(struct Parser *p);

static struct Sexpr *make_node(enum SexprKind kind, int line, int col)
{
	struct Sexpr *s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->kind = kind;
	s->line = line;
	s->col = col;
	return s;
}

static struct Sexpr *parse_atom(struct Parser *p)
{
	struct Sexpr *s;
	size_t start;
	size_t n;
	int line;
	int col;

	line = p->line;
	col = p->col;
	start = p->pos;
	while (is_atom_char(peek(p)))
		advance(p);
	n = p->pos - start;

	s = make_node(SEXPR_ATOM, line, col);
	if (s == NULL)
		return NULL;
	s->u.atom = malloc(n + 1);
	if (s->u.atom == NULL) {
		free(s);
		return NULL;
	}
	memcpy(s->u.atom, p->src + start, n);
	s->u.atom[n] = '\0';
	return s;
}

static struct Sexpr *parse_string(struct Parser *p)
{
	struct Sexpr *s;
	char *buf;
	size_t cap;
	size_t len;
	int line;
	int col;

	line = p->line;
	col = p->col;
	advance(p); /* consume opening " */

	buf = NULL;
	cap = 0;
	len = 0;

	for (;;) {
		int c = peek(p);

		if (c == -1) {
			parser_error(p, "unterminated string");
			free(buf);
			return NULL;
		}
		if (c == '"') {
			advance(p);
			break;
		}
		if (c == '\\') {
			int esc;

			advance(p);
			esc = advance(p);
			switch (esc) {
			case 'n':
				c = '\n';
				break;
			case 't':
				c = '\t';
				break;
			case 'r':
				c = '\r';
				break;
			case '"':
				c = '"';
				break;
			case '\\':
				c = '\\';
				break;
			case -1:
				parser_error(p, "unterminated string escape");
				free(buf);
				return NULL;
			default:
				parser_error(p, "unknown string escape");
				free(buf);
				return NULL;
			}
		} else {
			advance(p);
		}
		if (len + 2 > cap) {
			char *grown;

			cap = cap ? cap * 2 : 16;
			grown = realloc(buf, cap);
			if (grown == NULL) {
				free(buf);
				return NULL;
			}
			buf = grown;
		}
		buf[len++] = (char)c;
	}
	if (buf == NULL) {
		buf = malloc(1);
		if (buf == NULL)
			return NULL;
	}
	buf[len] = '\0';

	s = make_node(SEXPR_STRING, line, col);
	if (s == NULL) {
		free(buf);
		return NULL;
	}
	s->u.string = buf;
	return s;
}

static struct Sexpr *parse_list(struct Parser *p)
{
	struct Sexpr **items;
	struct Sexpr *s;
	size_t cap;
	size_t n;
	int line;
	int col;

	line = p->line;
	col = p->col;
	if (p->depth >= SEXPR_MAX_DEPTH) {
		parser_error(p, "lists nested too deeply");
		return NULL;
	}
	advance(p); /* consume ( */
	p->depth++;

	items = NULL;
	n = 0;
	cap = 0;

	for (;;) {
		struct Sexpr *child;
		int c;

		skip_ws(p);
		c = peek(p);
		if (c == -1) {
			parser_error(p, "unclosed list");
			goto fail;
		}
		if (c == ')') {
			advance(p);
			break;
		}
		child = parse_form(p);
		if (child == NULL)
			goto fail;
		if (n + 1 > cap) {
			struct Sexpr **grown;

			cap = cap ? cap * 2 : 4;
			grown = realloc(items, cap * sizeof(*items));
			if (grown == NULL) {
				sexpr_free(child);
				goto fail;
			}
			items = grown;
		}
		items[n++] = child;
	}

	s = make_node(SEXPR_LIST, line, col);
	if (s == NULL)
		goto fail;
	s->u.list.items = items;
	s->u.list.n = n;
	p->depth--;
	return s;

fail:
	p->depth--;
	for (size_t i = 0; i < n; i++)
		sexpr_free(items[i]);
	free(items);
	return NULL;
}

static struct Sexpr *parse_form(struct Parser *p)
{
	int c;

	skip_ws(p);
	c = peek(p);
	if (c == -1) {
		parser_error(p, "unexpected end of input");
		return NULL;
	}
	if (c == '(')
		return parse_list(p);
	if (c == '"')
		return parse_string(p);
	if (c == ')') {
		parser_error(p, "unexpected ')'");
		return NULL;
	}
	return parse_atom(p);
}

struct Sexpr *sexpr_parse(const char *src, char *errbuf, size_t errbuf_len)
{
	struct Parser p = {
		.src = src,
		.len = strlen(src),
		.pos = 0,
		.line = 1,
		.col = 1,
		.errbuf = errbuf,
		.errbuf_len = errbuf_len,
	};
	struct Sexpr **items;
	struct Sexpr *root;
	size_t cap;
	size_t n;

	if (errbuf != NULL && errbuf_len > 0)
		errbuf[0] = '\0';

	items = NULL;
	n = 0;
	cap = 0;

	for (;;) {
		struct Sexpr *form;

		skip_ws(&p);
		if (p.pos >= p.len)
			break;
		form = parse_form(&p);
		if (form == NULL)
			goto fail;
		if (n + 1 > cap) {
			struct Sexpr **grown;

			cap = cap ? cap * 2 : 4;
			grown = realloc(items, cap * sizeof(*items));
			if (grown == NULL) {
				sexpr_free(form);
				goto fail;
			}
			items = grown;
		}
		items[n++] = form;
	}

	root = make_node(SEXPR_LIST, 1, 1);
	if (root == NULL)
		goto fail;
	root->u.list.items = items;
	root->u.list.n = n;
	return root;

fail:
	for (size_t i = 0; i < n; i++)
		sexpr_free(items[i]);
	free(items);
	return NULL;
}

void sexpr_free(struct Sexpr *s)
{
	if (s == NULL)
		return;
	switch (s->kind) {
	case SEXPR_LIST:
		for (size_t i = 0; i < s->u.list.n; i++)
			sexpr_free(s->u.list.items[i]);
		free(s->u.list.items);
		break;
	case SEXPR_ATOM:
		free(s->u.atom);
		break;
	case SEXPR_STRING:
		free(s->u.string);
		break;
	}
	free(s);
}

struct Sexpr *sexpr_dup(const struct Sexpr *s)
{
	if (s == NULL)
		return NULL;
	struct Sexpr *out = calloc(1, sizeof(*out));
	if (out == NULL)
		return NULL;
	out->kind = s->kind;
	out->line = s->line;
	out->col = s->col;
	switch (s->kind) {
	case SEXPR_LIST:
		out->u.list.n = s->u.list.n;
		if (s->u.list.n > 0) {
			out->u.list.items =
				calloc(s->u.list.n, sizeof(*out->u.list.items));
			if (out->u.list.items == NULL) {
				free(out);
				return NULL;
			}
			for (size_t i = 0; i < s->u.list.n; i++) {
				out->u.list.items[i] =
					sexpr_dup(s->u.list.items[i]);
				if (out->u.list.items[i] == NULL) {
					out->u.list.n = i;
					sexpr_free(out);
					return NULL;
				}
			}
		}
		break;
	case SEXPR_ATOM:
		if (s->u.atom != NULL) {
			out->u.atom = strdup(s->u.atom);
			if (out->u.atom == NULL) {
				free(out);
				return NULL;
			}
		}
		break;
	case SEXPR_STRING:
		if (s->u.string != NULL) {
			out->u.string = strdup(s->u.string);
			if (out->u.string == NULL) {
				free(out);
				return NULL;
			}
		}
		break;
	}
	return out;
}
