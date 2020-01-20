/*	$Id$ */
/*
 * Copyright (c) 2020, Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#if HAVE_SYS_QUEUE
# include <sys/queue.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lowdown.h"
#include "extern.h"

struct sym {
	const char	*key;
	const char	*val;
};

/*
 * Order is important: check the longest subset first.
 * (So basically "---" comes before "--".)
 */
static const struct sym syms[] = {
	{ "(c)",	"&#169;" },
	{ "(C)",	"&#169;" },
	{ "(r)",	"&#174;" },
	{ "(R)",	"&#174;" },
	{ "(tm)",	"&#8482;" },
	{ "(TM)",	"&#8482;" },
	{ "(sm)",	"&#8480;" },
	{ "(SM)",	"&#8480;" },
	{ "...",	"&#8230;" },
	{ ". . .",	"&#8230;" },
	{ "---",	"&#8212;" },
	{ "--",		"&#8211;" },
	{ NULL,		NULL }
};

static const struct sym syms2[] = {
	{ "1/4th",	"&frac14;" },
	{ "1/4",	"&frac14;" },
	{ "3/4ths",	"&frac34;" },
	{ "3/4th",	"&frac34;" },
	{ "3/4",	"&frac34;" },
	{ "1/2",	"&frac12;" },
	{ "\"",		"&rdquo;" },
	{ "'",		"&rsquo;" },
	{ NULL,		NULL }
};

struct smarty {
	int			 last_nspace;
	const struct sym	*last_syms2;
	struct lowdown_node	*last_node;
	size_t			 last_pos;
};

enum type {
	TYPE_ROOT, /* root (LOWDOWN_ROOT) */
	TYPE_BLOCK, /* block-level */
	TYPE_SPAN, /* span-level */
	TYPE_OPAQUE, /* skip */
	TYPE_TEXT /* text (LOWDOWN_NORMAL_TEXT) */
};

static const enum type types[LOWDOWN__MAX] = {
	TYPE_ROOT, /* LOWDOWN_ROOT */
	TYPE_OPAQUE, /* LOWDOWN_BLOCKCODE */
	TYPE_BLOCK, /* LOWDOWN_BLOCKQUOTE */
	TYPE_BLOCK, /* LOWDOWN_HEADER */
	TYPE_BLOCK, /* LOWDOWN_HRULE */
	TYPE_BLOCK, /* LOWDOWN_LIST */
	TYPE_BLOCK, /* LOWDOWN_LISTITEM */
	TYPE_BLOCK, /* LOWDOWN_PARAGRAPH */
	TYPE_BLOCK, /* LOWDOWN_TABLE_BLOCK */
	TYPE_BLOCK, /* LOWDOWN_TABLE_HEADER */
	TYPE_BLOCK, /* LOWDOWN_TABLE_BODY */
	TYPE_BLOCK, /* LOWDOWN_TABLE_ROW */
	TYPE_BLOCK, /* LOWDOWN_TABLE_CELL */
	TYPE_BLOCK, /* LOWDOWN_FOOTNOTES_BLOCK */
	TYPE_BLOCK, /* LOWDOWN_FOOTNOTE_DEF */
	TYPE_OPAQUE, /* LOWDOWN_BLOCKHTML */
	TYPE_OPAQUE, /* LOWDOWN_LINK_AUTO */
	TYPE_OPAQUE, /* LOWDOWN_CODESPAN */
	TYPE_SPAN, /* LOWDOWN_DOUBLE_EMPHASIS */
	TYPE_SPAN, /* LOWDOWN_EMPHASIS */
	TYPE_SPAN, /* LOWDOWN_HIGHLIGHT */
	TYPE_SPAN, /* LOWDOWN_IMAGE */
	TYPE_SPAN, /* LOWDOWN_LINEBREAK */
	TYPE_SPAN, /* LOWDOWN_LINK */
	TYPE_SPAN, /* LOWDOWN_TRIPLE_EMPHASIS */
	TYPE_SPAN, /* LOWDOWN_STRIKETHROUGH */
	TYPE_SPAN, /* LOWDOWN_SUPERSCRIPT */
	TYPE_SPAN, /* LOWDOWN_FOOTNOTE_REF */
	TYPE_OPAQUE, /* LOWDOWN_MATH_BLOCK */
	TYPE_OPAQUE, /* LOWDOWN_RAW_HTML */
	TYPE_OPAQUE, /* LOWDOWN_ENTITY */
	TYPE_TEXT, /* LOWDOWN_NORMAL_TEXT */
	TYPE_BLOCK, /* LOWDOWN_DOC_HEADER */
	TYPE_BLOCK, /* LOWDOWN_DOC_FOOTER */
};

/*
 * Given the sequence in "n" starting at "start" and ending at "end",
 * split "n" around the sequence and replace it with "entity".
 * This behaves properly if the leading or trailing sequence is
 * zero-length.
 * It may modify the subtree rooted at the parent of "n".
 */
static void
smarty_entity(struct lowdown_node *n, size_t *maxn,
	size_t start, size_t end, const char *entity)
{
	struct lowdown_node	*nn, *nent;

	assert(n->type == LOWDOWN_NORMAL_TEXT);

	/* Allocate the subsequent entity. */

	nent = xcalloc(1, sizeof(struct lowdown_node));
	nent->id = (*maxn)++;
	nent->type = LOWDOWN_ENTITY;
	nent->parent = n->parent;
	TAILQ_INIT(&nent->children);
	nent->rndr_entity.text.data = xstrdup(entity);
	nent->rndr_entity.text.size = strlen(entity);
	TAILQ_INSERT_AFTER(&n->parent->children, n, nent, entries);

	/* Allocate the remaining bits, if applicable. */

	if (n->rndr_normal_text.text.size - end > 0) {
		nn = xcalloc(1, sizeof(struct lowdown_node));
		nn->id = (*maxn)++;
		nn->type = LOWDOWN_NORMAL_TEXT;
		nn->parent = n->parent;
		TAILQ_INIT(&nn->children);
		nn->rndr_entity.text.data = xstrdup
			(n->rndr_normal_text.text.data + end);
		nn->rndr_entity.text.size = 
			n->rndr_normal_text.text.size - end;
		TAILQ_INSERT_AFTER(&n->parent->children, 
			nent, nn, entries);
	}

	n->rndr_normal_text.text.size = start;
}

static int
smarty_purge(struct smarty *s, size_t *maxn,
	const struct lowdown_node *n)
{
	struct lowdown_node	*sv = s->last_node;

	if (s->last_node == NULL)
		return 0;

	smarty_entity(s->last_node, maxn, 
		s->last_pos, 
		s->last_pos + strlen(s->last_syms2->key), 
		s->last_syms2->val);
	memset(s, 0, sizeof(struct smarty));
	s->last_nspace = 1;
	return sv == n;
}

static void
smarty_hbuf(struct lowdown_node *n, size_t *maxn, hbuf *b, struct smarty *s)
{
	size_t	 i = 0, j, sz;

	assert(n->type == LOWDOWN_NORMAL_TEXT);

	for (i = 0; i < b->size; i++) {
		/*
		 * Begin by seeing if the last character is waiting to
		 * see if we're on a word boundary.
		 * If we split on the current node, then return to get
		 * the new node later.
		 */

		if (s->last_node != NULL &&
		    (ispunct((unsigned char)b->data[i]) ||
		     isspace((unsigned char)b->data[i]))) {
			if (smarty_purge(s, maxn, n))
				return;
		} else if (s->last_node != NULL) {
			memset(s, 0, sizeof(struct smarty));
			s->last_nspace = 1;
		}

		/*
		 * Begin by seeing if the given character is going to
		 * start a sequence that we need to interpret.
		 * Do this *before* we check to see if we're a word
		 * boundary, as some word boundaries (e.g., 
		 */
		
		switch (b->data[i]) {
		case '.':
		case '(':
		case '-':
			/*
			 * Look up all instances of "standalone" symbols
			 * that don't need a before or after context.
			 */
			for (j = 0; syms[j].key != NULL; j++) {
				sz = strlen(syms[j].key);
				if (i + sz - 1 >= b->size)
					continue;
				if (memcmp(syms[j].key, &b->data[i], sz))
					continue;
				smarty_entity(n, maxn, i, 
					i + sz, syms[j].val);
				return;
			}
			break;
		case '"':
			if (s->last_nspace) {
				s->last_syms2 = &syms2[1];
				s->last_node = n;
				s->last_pos = i;
				i += strlen(s->last_syms2->key) - 1;
				continue;
			}
			smarty_entity(n, maxn, i, i + 1, "&ldquo;");
			return;
		case '\'':
			if (s->last_nspace) {
				s->last_syms2 = &syms2[2];
				s->last_node = n;
				s->last_pos = i;
				i += strlen(s->last_syms2->key) - 1;
				continue;
			}
			smarty_entity(n, maxn, i, i + 1, "&lsquo;");
			return;
		case '1':
		case '3':
			if (s->last_nspace)
				break;
			for (j = 0; syms2[j].key != NULL; j++) {
				sz = strlen(syms2[j].key);
				if (i + sz - 1 >= b->size)
					continue;
				if (memcmp(syms2[j].key, &b->data[i], sz))
					continue;
				s->last_syms2 = &syms2[j];
				s->last_node = n;
				s->last_pos = i;
				i += strlen(s->last_syms2->key) - 1;
				continue;
			}
			break;
		default:
			break;
		}

		/* Does the current char count as a word break? */

		s->last_nspace = 
			!(isspace((unsigned char)b->data[i]) || 
			  ispunct((unsigned char)b->data[i]));
	}
}

static void
smarty_span(struct lowdown_node *root, size_t *maxn, struct smarty *s)
{
	struct lowdown_node	*n;

	TAILQ_FOREACH(n, &root->children, entries)  {
		switch (types[n->type]) {
		case TYPE_TEXT:
			assert(n->type == LOWDOWN_NORMAL_TEXT);
			smarty_hbuf(n, maxn, 
				&n->rndr_normal_text.text, s);
			break;
		case TYPE_SPAN:
			smarty_span(n, maxn, s);
			break;
		case TYPE_OPAQUE:
			s->last_nspace = 1;
			break;
		default:
			smarty_purge(s, maxn, NULL);
			break;
		}
	}
}

static void
smarty_block(struct lowdown_node *root, size_t *maxn)
{
	struct smarty		 s;
	struct lowdown_node	*n;

	memset(&s, 0, sizeof(struct smarty));
	TAILQ_FOREACH(n, &root->children, entries) {
		switch (types[n->type]) {
		case TYPE_ROOT:
		case TYPE_BLOCK:
			smarty_purge(&s, maxn, NULL);
			s.last_nspace = 0;
			smarty_block(n, maxn);
			break;
		case TYPE_TEXT:
			assert(n->type == LOWDOWN_NORMAL_TEXT);
			smarty_hbuf(n, maxn, 
				&n->rndr_normal_text.text, &s);
			break;
		case TYPE_SPAN:
			smarty_span(n, maxn, &s);
			break;
		case TYPE_OPAQUE:
			s.last_nspace = 1;
			break;
		default:
			break;
		}
	}

	smarty_purge(&s, maxn, NULL);
}

void
smarty(struct lowdown_node *n, size_t maxn)
{

	if (n == NULL)
		return;
	if (types[n->type] == TYPE_ROOT ||
	    types[n->type] == TYPE_BLOCK)
		smarty_block(n, &maxn);
}
