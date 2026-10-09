/* cmark -- a CommonMark parser and HTML renderer in one C11 file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads Markdown on stdin (or from a named file) and writes HTML on stdout.
 * The only claim worth making about a Markdown parser is a conformance score
 * against the spec's own suite, so see README.md and t/run.sh for that number.
 *
 * Exit status: 0 rendered, 1 input nested too deeply, 2 usage or I/O error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TAB_STOP 4
#define CODE_INDENT 4

/* Containers nest by recursion in the renderer and by a linked stack in the
 * parser, so the ceiling is the renderer's.  512 frames of the render
 * function is a few hundred kB of stack; the spec has nothing deeper than a
 * handful. */
#define MAX_DEPTH 512

static void oom(void) {
	fputs("cmark: out of memory\n", stderr);
	exit(2);
}

static void *xcalloc(size_t n) {
	void *p = calloc(1, n ? n : 1);
	if (!p) oom();
	return p;
}

/* ---------------------------------------------------------------- buffers */

typedef struct {
	char *p;
	size_t n, cap;
} Buf;

static void bgrow(Buf *b, size_t need) {
	size_t cap = b->cap ? b->cap : 32;
	if (b->n + need + 1 <= b->cap) return;
	while (cap < b->n + need + 1) {
		if (cap > (size_t)-1 / 2) oom();
		cap *= 2;
	}
	b->p = realloc(b->p, cap);
	if (!b->p) oom();
	b->cap = cap;
}

static void bput(Buf *b, const char *s, size_t n) {
	if (!n) return;
	bgrow(b, n);
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

static void bputc(Buf *b, char c) { bput(b, &c, 1); }
static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }

static void bfree(Buf *b) {
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}

/* ------------------------------------------------------- character classes */

static int is_line_end(int c) { return c == '\n' || c == '\r'; }
static int c_isspace(int c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
	       c == '\r';
}

/* ------------------------------------------------------------------- nodes */

typedef enum {
	N_DOC,
	N_BQUOTE,
	N_LIST,
	N_ITEM,
	N_PARA,
	N_HEADING,
	N_CODE,
	N_HTML_BLOCK,
	N_HRULE,
	N_TEXT,
	N_SOFTBREAK,
	N_LINEBREAK,
	N_CODESPAN,
	N_EMPH,
	N_STRONG,
	N_LINK,
	N_IMAGE,
	N_HTML_INLINE
} Type;

typedef struct Node Node;
struct Node {
	Type t;
	Node *parent, *first, *last, *next, *prev;
	Buf content;		/* blocks: raw lines.  inlines: the literal */
	int open;
	int level, setext;	/* heading */
	int fenced, fence_len, fence_off;
	char fence_char;
	Buf info;		/* code block info string */
	int ordered, start, tight;
	char delim, bullet;	/* list and item */
	int marker_off, padding;
	int html_type;
	int last_line_blank, last_line_checked;
	Buf url, title;		/* link and image */
};

static Node *node_new(Type t) {
	Node *n = xcalloc(sizeof *n);
	n->t = t;
	n->open = 1;
	return n;
}

static void node_free(Node *n) {
	Node *c, *next;
	if (!n) return;
	for (c = n->first; c; c = next) {
		next = c->next;
		node_free(c);
	}
	bfree(&n->content);
	bfree(&n->info);
	bfree(&n->url);
	bfree(&n->title);
	free(n);
}

static void node_append(Node *parent, Node *child) {
	child->parent = parent;
	child->prev = parent->last;
	child->next = NULL;
	if (parent->last) parent->last->next = child;
	else parent->first = child;
	parent->last = child;
}

static void node_unlink(Node *n) {
	if (n->prev) n->prev->next = n->next;
	else if (n->parent) n->parent->first = n->next;
	if (n->next) n->next->prev = n->prev;
	else if (n->parent) n->parent->last = n->prev;
	n->parent = n->prev = n->next = NULL;
}

static int accepts_lines(Type t) {
	return t == N_PARA || t == N_HEADING || t == N_CODE || t == N_HTML_BLOCK;
}

/* ------------------------------------------------------------ parser state */

typedef struct {
	Node *doc;
	Node *cur;		/* deepest open block */
	const char *line;	/* current line, newline-terminated */
	size_t len;
	size_t off, col;	/* cursor: byte offset and display column */
	size_t fns;		/* offset of first non-space on the line */
	size_t fns_col;
	int indent;		/* columns of indentation at fns */
	int blank;
	int partial_tab;
	int depth;		/* open container nesting */
	int too_deep;
} Parser;

/* cmark's S_advance_offset.  `columns` means "advance this many display
 * columns", which can stop halfway through a tab; the leftover is recorded in
 * partial_tab and turned back into spaces by add_line. */
static void advance(Parser *p, size_t count, int columns) {
	while (count > 0 && p->off < p->len) {
		char c = p->line[p->off];
		if (c == '\t') {
			size_t to_tab = TAB_STOP - (p->col % TAB_STOP);
			if (columns) {
				p->partial_tab = to_tab > count;
				if (to_tab > count) to_tab = count;
				p->col += to_tab;
				p->off += p->partial_tab ? 0 : 1;
				count -= to_tab;
			} else {
				p->partial_tab = 0;
				p->col += to_tab;
				p->off += 1;
				count -= 1;
			}
		} else {
			p->partial_tab = 0;
			p->off += 1;
			p->col += 1;
			count -= 1;
		}
	}
}

static void find_first_nonspace(Parser *p) {
	size_t i = p->off, c = p->col;
	while (i < p->len) {
		char ch = p->line[i];
		if (ch == ' ') { i++; c++; }
		else if (ch == '\t') { i++; c += TAB_STOP - (c % TAB_STOP); }
		else break;
	}
	p->blank = i >= p->len || is_line_end(p->line[i]);
	p->fns = i;
	p->fns_col = c;
	p->indent = (int)(c - p->col);
}

static void add_line(Parser *p, Node *n) {
	if (p->partial_tab) {
		size_t to_tab = TAB_STOP - (p->col % TAB_STOP), i;
		p->off += 1;
		for (i = 0; i < to_tab; i++) bputc(&n->content, ' ');
	}
	if (p->off < p->len) bput(&n->content, p->line + p->off, p->len - p->off);
}

static Node *add_child(Parser *p, Node *parent, Type t) {
	Node *child;
	if (p->depth >= MAX_DEPTH) {
		p->too_deep = 1;
		return parent;
	}
	child = node_new(t);
	node_append(parent, child);
	p->depth++;
	return child;
}

/* ------------------------------------------------------- inline parsing */

static void rtrim_buf(Buf *b) {
	while (b->n > 0 && c_isspace((unsigned char)b->p[b->n - 1])) b->n--;
	if (b->p) b->p[b->n] = 0;
}

/* shortcut: placeholder inline pass -- one text node, no emphasis, no links.
 * Replaced by the real inline lexer; see PLAN.md. */
static void parse_inlines(Node *parent) {
	Node *t;
	rtrim_buf(&parent->content);
	if (parent->content.n == 0) return;
	t = node_new(N_TEXT);
	bput(&t->content, parent->content.p, parent->content.n);
	node_append(parent, t);
}

/* --------------------------------------------------------------- finalize */

static Node *finalize(Parser *p, Node *b) {
	Node *parent = b->parent;
	(void)p;
	b->open = 0;
	switch (b->t) {
	case N_PARA:
		if (b->content.n == 0) {
			node_unlink(b);
			node_free(b);
			return parent;
		}
		break;
	default:
		break;
	}
	return parent;
}

static void finalize_document(Parser *p) {
	while (p->cur) p->cur = finalize(p, p->cur);
}

/* Inline content is parsed after the whole block structure is known, because
 * a link reference definition may be defined after its first use. */
static void process_inlines(Node *n) {
	Node *c;
	if (n->t == N_PARA || n->t == N_HEADING) parse_inlines(n);
	for (c = n->first; c; c = c->next) process_inlines(c);
}

/* ------------------------------------------------------- block structure */

/* Walk the chain of currently open containers from the document down and ask
 * each whether this line continues it.  Returns the deepest one that matched
 * and sets *all_matched. */
static Node *check_open_blocks(Parser *p, int *all_matched) {
	Node *cont = p->doc;
	*all_matched = 0;
	for (;;) {
		Node *next = cont->last;
		if (!next || !next->open) break;
		if (!(next->t == N_BQUOTE || next->t == N_ITEM || next->t == N_LIST ||
		      next->t == N_PARA || next->t == N_HEADING || next->t == N_CODE ||
		      next->t == N_HTML_BLOCK))
			break;
		cont = next;
		switch (cont->t) {
		case N_PARA:
			find_first_nonspace(p);
			if (p->blank) return cont->parent;
			break;
		case N_HEADING:
			return cont->parent;
		default:
			break;
		}
	}
	*all_matched = 1;
	return cont;
}

/* shortcut: only the paragraph fallback in add_text_to_container exists yet;
 * every block start is added here commit by commit.  See PLAN.md. */
static void open_new_blocks(Parser *p, Node **cont, int all_matched) {
	(void)p;
	(void)cont;
	(void)all_matched;
}

static void add_text_to_container(Parser *p, Node *cont, Node *last_matched) {
	Node *tmp;
	find_first_nonspace(p);

	if (p->blank && cont->last) cont->last->last_line_blank = 1;
	cont->last_line_blank = p->blank && cont->t != N_BQUOTE &&
	                        cont->t != N_HEADING && cont->t != N_HRULE &&
	                        !(cont->t == N_CODE && cont->fenced);
	for (tmp = cont; tmp->parent; tmp = tmp->parent)
		tmp->parent->last_line_blank = 0;

	if (p->cur != last_matched && cont == last_matched && !p->blank &&
	    p->cur->t == N_PARA) {
		add_line(p, p->cur);	/* lazy continuation of a paragraph */
		return;
	}
	while (p->cur != last_matched) p->cur = finalize(p, p->cur);

	if (cont->t == N_CODE || cont->t == N_HTML_BLOCK) {
		add_line(p, cont);
	} else if (p->blank) {
		/* nothing to add */
	} else if (accepts_lines(cont->t)) {
		advance(p, p->fns - p->off, 0);
		add_line(p, cont);
	} else {
		cont = add_child(p, cont, N_PARA);
		advance(p, p->fns - p->off, 0);
		add_line(p, cont);
	}
	p->cur = cont;
}

static void process_line(Parser *p, const char *line, size_t len) {
	Node *cont, *last_matched;
	int all_matched;

	p->line = line;
	p->len = len;
	p->off = p->col = 0;
	p->fns = 0;
	p->fns_col = 0;
	p->indent = 0;
	p->blank = 0;
	p->partial_tab = 0;

	cont = check_open_blocks(p, &all_matched);
	if (!cont) return;
	last_matched = cont;
	open_new_blocks(p, &cont, all_matched);
	add_text_to_container(p, cont, last_matched);
}

/* ------------------------------------------------------------- rendering */

static void escape_html(Buf *out, const char *s, size_t n) {
	size_t i, start = 0;
	for (i = 0; i < n; i++) {
		const char *rep = NULL;
		switch (s[i]) {
		case '&': rep = "&amp;"; break;
		case '<': rep = "&lt;"; break;
		case '>': rep = "&gt;"; break;
		case '"': rep = "&quot;"; break;
		default: continue;
		}
		bput(out, s + start, i - start);
		bputs(out, rep);
		start = i + 1;
	}
	bput(out, s + start, n - start);
}

static void cr(Buf *out) {
	if (out->n && out->p[out->n - 1] != '\n') bputc(out, '\n');
}

static void render(Buf *out, Node *n, int depth);

static void render_children(Buf *out, Node *n, int depth) {
	Node *c;
	for (c = n->first; c; c = c->next) render(out, c, depth + 1);
}

static void render(Buf *out, Node *n, int depth) {
	if (depth > MAX_DEPTH) return;
	switch (n->t) {
	case N_DOC:
		render_children(out, n, depth);
		break;
	case N_PARA:
		cr(out);
		bputs(out, "<p>");
		render_children(out, n, depth);
		bputs(out, "</p>\n");
		break;
	case N_TEXT:
		escape_html(out, n->content.p, n->content.n);
		break;
	default:
		render_children(out, n, depth);
		break;
	}
}

/* ------------------------------------------------------------------ driver */

/* Normalise line endings and strip the insecure NUL character, as the spec
 * requires ("For security reasons, the Unicode character U+0000 must be
 * replaced with the REPLACEMENT CHARACTER").  This is the only
 * transformation applied to the input. */
static void normalize(Buf *out, const char *in, size_t n) {
	size_t i;
	for (i = 0; i < n; i++) {
		char c = in[i];
		if (c == '\r') {
			bputc(out, '\n');
			if (i + 1 < n && in[i + 1] == '\n') i++;
		} else if (c == 0) {
			bputs(out, "\xef\xbf\xbd");
		} else {
			bputc(out, c);
		}
	}
	if (out->n && out->p[out->n - 1] != '\n') bputc(out, '\n');
}

static int run(const char *src, size_t srclen, Buf *out) {
	Parser p;
	Buf text = {0};
	size_t i, start;
	int rc = 0;

	memset(&p, 0, sizeof p);
	normalize(&text, src, srclen);
	p.doc = node_new(N_DOC);
	p.cur = p.doc;

	for (start = 0, i = 0; i < text.n; i++) {
		if (text.p[i] == '\n') {
			process_line(&p, text.p + start, i - start + 1);
			start = i + 1;
			if (p.too_deep) break;
		}
	}
	if (p.too_deep) rc = 1;
	else {
		finalize_document(&p);
		process_inlines(p.doc);
		render(out, p.doc, 0);
	}
	node_free(p.doc);
	bfree(&text);
	return rc;
}

int main(int argc, char **argv) {
	Buf in = {0}, out = {0};
	FILE *f = stdin;
	char chunk[65536];
	size_t got;
	int rc;

	if (argc > 2) {
		fputs("usage: cmark [FILE]\n", stderr);
		return 2;
	}
	if (argc == 2 && strcmp(argv[1], "-") != 0) {
		f = fopen(argv[1], "rb");
		if (!f) {
			fprintf(stderr, "cmark: %s: cannot open\n", argv[1]);
			return 2;
		}
	}
	while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) bput(&in, chunk, got);
	if (ferror(f)) {
		fputs("cmark: read error\n", stderr);
		return 2;
	}
	if (f != stdin) fclose(f);

	rc = run(in.p ? in.p : "", in.n, &out);
	if (rc == 1) fputs("cmark: input nested too deeply\n", stderr);
	else if (out.n && fwrite(out.p, 1, out.n, stdout) != out.n) {
		fputs("cmark: write error\n", stderr);
		rc = 2;
	}
	bfree(&in);
	bfree(&out);
	return rc;
}
