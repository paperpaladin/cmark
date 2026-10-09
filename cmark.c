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

static int c_isspace_fwd(int c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
	       c == '\r';
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

static void btrim(Buf *b) {
	size_t i = 0;
	while (i < b->n && c_isspace_fwd((unsigned char)b->p[i])) i++;
	if (i) {
		memmove(b->p, b->p + i, b->n - i);
		b->n -= i;
	}
	while (b->n > 0 && c_isspace_fwd((unsigned char)b->p[b->n - 1])) b->n--;
	if (b->p) b->p[b->n] = 0;
}

static void bfree(Buf *b) {
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}

/* ------------------------------------------------------- character classes */

static int is_space_or_tab(int c) { return c == ' ' || c == '\t'; }
static int is_line_end(int c) { return c == '\n' || c == '\r'; }
#define c_isspace c_isspace_fwd

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
	int start_line;
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

static int is_block(Type t) { return t <= N_HRULE; }

/* Which blocks may hold which.  This is what makes `add_child` able to close
 * an open paragraph on its own: it walks up until it finds a parent that can
 * take the new block. */
static int can_contain(Type parent, Type child) {
	if (!is_block(child) || child == N_DOC) return 0;
	switch (parent) {
	case N_DOC:
	case N_BQUOTE:
	case N_ITEM:
		return child != N_ITEM;
	case N_LIST:
		return child == N_ITEM;
	default:
		return 0;
	}
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
	int lineno;
	int too_deep;
} Parser;

static Node *finalize(Parser *p, Node *b);

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

static int node_depth(Node *n) {
	int d = 0;
	for (; n; n = n->parent)
		if (++d > MAX_DEPTH) break;
	return d;
}

static Node *add_child(Parser *p, Node *parent, Type t) {
	Node *child;
	while (!can_contain(parent->t, t)) parent = finalize(p, parent);
	if (node_depth(parent) >= MAX_DEPTH) {
		p->too_deep = 1;
		return parent;
	}
	child = node_new(t);
	child->start_line = p->lineno;
	node_append(parent, child);
	return child;
}

/* ---------------------------------------------------------------- scanners */

/* `([*] | [_] | [-]) ([ \t]* same)* [\r\n]` with at least three of the
 * character.  Returns 1 on a match; a thematic break is the whole line. */
static int scan_hrule(Parser *p, size_t i) {
	char c = p->line[i];
	int count = 0;
	if (c != '*' && c != '-' && c != '_') return 0;
	for (; i < p->len; i++) {
		char ch = p->line[i];
		if (ch == c) count++;
		else if (is_space_or_tab(ch)) continue;
		else if (is_line_end(ch)) break;
		else return 0;
	}
	return count >= 3;
}

/* `[#]{1,6} ([ \t]+ | [\r\n])`.  Returns the length matched, including the
 * spaces that separate the marker from the content, so the caller can skip
 * straight to the heading text. */
static size_t scan_atx_start(Parser *p, size_t i) {
	size_t start = i;
	int hashes = 0;
	while (i < p->len && p->line[i] == '#' && hashes < 7) { hashes++; i++; }
	if (hashes < 1 || hashes > 6) return 0;
	if (i < p->len && is_line_end(p->line[i])) return i + 1 - start;
	if (i >= p->len || !is_space_or_tab(p->line[i])) return 0;
	while (i < p->len && is_space_or_tab(p->line[i])) i++;
	return i - start;
}

/* `[`]{3,}` or `[~]{3,}`.  Returns the fence length, or 0.  A backtick fence
 * may not have a backtick anywhere in its info string; a tilde fence may. */
static int scan_open_fence(Parser *p, size_t i) {
	char c = p->line[i];
	int count = 0;
	size_t j;
	if (c != '`' && c != '~') return 0;
	while (i < p->len && p->line[i] == c) { count++; i++; }
	if (count < 3) return 0;
	if (c == '`')
		for (j = i; j < p->len && !is_line_end(p->line[j]); j++)
			if (p->line[j] == '`') return 0;
	return count;
}

/* `(fence_char){3,} [ \t]* [\r\n]`.  Returns the fence length, or 0.  The
 * caller compares it with the opening length. */
static int scan_close_fence(Parser *p, size_t i, char c) {
	int count = 0;
	while (i < p->len && p->line[i] == c) { count++; i++; }
	if (count < 3) return 0;
	while (i < p->len && is_space_or_tab(p->line[i])) i++;
	if (i >= p->len || !is_line_end(p->line[i])) return 0;
	return count;
}

/* A bullet or ordered list marker.  `interrupts` is true when the marker
 * would start a list inside an open paragraph, which the spec restricts: the
 * item must be non-empty, and an ordered marker must start at 1. */
typedef struct {
	int ordered, start, padding, marker_off;
	char bullet, delim;
} ListData;

static size_t parse_list_marker(Parser *p, size_t pos, int interrupts,
                                ListData *d) {
	size_t startpos = pos, i;
	int c = pos < p->len ? (unsigned char)p->line[pos] : 0;

	memset(d, 0, sizeof *d);
	if (c == '*' || c == '-' || c == '+') {
		pos++;
		if (pos >= p->len || !c_isspace((unsigned char)p->line[pos])) return 0;
		if (interrupts) {
			for (i = pos; i < p->len && is_space_or_tab(p->line[i]); i++)
				;
			if (i >= p->len || is_line_end(p->line[i])) return 0;
		}
		d->ordered = 0;
		d->bullet = (char)c;
	} else if (c >= '0' && c <= '9') {
		int start = 0, digits = 0;
		do {
			start = 10 * start + (p->line[pos] - '0');
			pos++;
			digits++;
		} while (digits < 9 && pos < p->len && p->line[pos] >= '0' &&
		         p->line[pos] <= '9');
		if (interrupts && start != 1) return 0;
		c = pos < p->len ? (unsigned char)p->line[pos] : 0;
		if (c != '.' && c != ')') return 0;
		pos++;
		if (pos >= p->len || !c_isspace((unsigned char)p->line[pos])) return 0;
		if (interrupts) {
			for (i = pos; i < p->len && is_space_or_tab(p->line[i]); i++)
				;
			if (i >= p->len || is_line_end(p->line[i])) return 0;
		}
		d->ordered = 1;
		d->start = start;
		d->delim = (char)c;
	} else {
		return 0;
	}
	return pos - startpos;
}

/* Two markers belong to the same list if they are the same kind: same bullet
 * character, or same ordered delimiter.  `1.` and `1)` are two lists. */
static int lists_match(Node *list, ListData *d) {
	return list->ordered == d->ordered && list->delim == d->delim &&
	       list->bullet == d->bullet;
}

static void apply_list(Node *n, ListData *d) {
	n->ordered = d->ordered;
	n->start = d->start;
	n->bullet = d->bullet;
	n->delim = d->delim;
	n->padding = d->padding;
	n->marker_off = d->marker_off;
}

/* `[=]+ [ \t]* [\r\n]` is an h1 underline, `[-]+ ...` an h2.  Returns the
 * level, or 0.  A single `-` is both a valid h2 underline and a valid list
 * marker, which is why the caller's branch order matters. */
static int scan_setext(Parser *p, size_t i) {
	char c = p->line[i];
	if (c != '=' && c != '-') return 0;
	while (i < p->len && p->line[i] == c) i++;
	while (i < p->len && is_space_or_tab(p->line[i])) i++;
	if (i >= p->len || !is_line_end(p->line[i])) return 0;
	return c == '=' ? 1 : 2;
}

/* The ATX closing sequence: `## foo ##` is an h2 holding "foo".  A line of
 * nothing but hashes after the marker is content, not a closing sequence,
 * because the sequence has to be preceded by a space. */
static void chop_hashes(Parser *p) {
	size_t e;
	while (p->len > p->off && c_isspace((unsigned char)p->line[p->len - 1]))
		p->len--;
	e = p->len;
	while (e > p->off && p->line[e - 1] == '#') e--;
	if (e != p->len && e > p->off &&
	    is_space_or_tab((unsigned char)p->line[e - 1])) {
		p->len = e;
		while (p->len > p->off && c_isspace((unsigned char)p->line[p->len - 1]))
			p->len--;
	}
}

/* A backslash before an ASCII punctuation character is an escape.  Used on
 * code fence info strings, link destinations and link titles, where the
 * escapes are resolved during parsing rather than during inline rendering.
 * shortcut: entity references in those three places are resolved by the same
 * helper once the entity table exists. */
static int ascii_punct(int c) {
	return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
	       (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static void unescape_backslashes(Buf *b) {
	size_t r, w = 0;
	for (r = 0; r < b->n; r++) {
		if (b->p[r] == '\\' && r + 1 < b->n &&
		    ascii_punct((unsigned char)b->p[r + 1]))
			r++;
		b->p[w++] = b->p[r];
	}
	b->n = w;
	if (b->p) b->p[b->n] = 0;
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

/* Trailing blank lines are not part of an indented code block: the block ends
 * at the last non-blank line. */
static void remove_trailing_blank_lines(Buf *b) {
	size_t i;
	for (i = b->n; i > 0; i--) {
		char c = b->p[i - 1];
		if (c != ' ' && c != '\t' && !is_line_end(c)) break;
	}
	if (i == 0) { b->n = 0; if (b->p) b->p[0] = 0; return; }
	for (; i < b->n; i++)
		if (is_line_end(b->p[i])) break;
	b->n = i;
	b->p[b->n] = 0;
}

/* A list is loose if any of its items is followed by a blank line, or if any
 * item's own children are separated by one.  This is a property of the whole
 * list and can only be decided once it is closed. */
static int ends_with_blank_line(Node *n) {
	if (n->last_line_checked) return n->last_line_blank;
	n->last_line_checked = 1;
	if ((n->t == N_LIST || n->t == N_ITEM) && n->last)
		return ends_with_blank_line(n->last);
	return n->last_line_blank;
}

static void set_list_tight(Node *list) {
	Node *item, *sub;
	list->tight = 1;
	for (item = list->first; item; item = item->next) {
		if (item->last_line_blank && item->next) {
			list->tight = 0;
			break;
		}
		for (sub = item->first; sub; sub = sub->next) {
			if (ends_with_blank_line(sub) && (item->next || sub->next)) {
				list->tight = 0;
				break;
			}
		}
		if (!list->tight) break;
	}
}

static Node *finalize(Parser *p, Node *b) {
	Node *parent = b->parent;
	(void)p;
	b->open = 0;
	switch (b->t) {
	case N_CODE:
		if (!b->fenced) {
			remove_trailing_blank_lines(&b->content);
			bputc(&b->content, '\n');
		} else {
			/* The first line of the content is the info string. */
			size_t i = 0;
			while (i < b->content.n && !is_line_end(b->content.p[i])) i++;
			bput(&b->info, b->content.p, i);
			btrim(&b->info);
			unescape_backslashes(&b->info);
			if (i < b->content.n && b->content.p[i] == '\r') i++;
			if (i < b->content.n && b->content.p[i] == '\n') i++;
			memmove(b->content.p, b->content.p + i, b->content.n - i);
			b->content.n -= i;
			b->content.p[b->content.n] = 0;
		}
		break;
	case N_LIST:
		set_list_tight(b);
		break;
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
	while (cont->last && cont->last->open) {
		cont = cont->last;
		find_first_nonspace(p);
		switch (cont->t) {
		case N_BQUOTE:
			if (p->indent <= 3 && p->fns < p->len && p->line[p->fns] == '>') {
				advance(p, p->fns + 1 - p->off, 0);
				if (p->off < p->len && is_space_or_tab(p->line[p->off]))
					advance(p, 1, 1);
			} else {
				return cont->parent;
			}
			break;
		case N_ITEM:
			if (p->indent >= cont->marker_off + cont->padding)
				advance(p, (size_t)(cont->marker_off + cont->padding), 1);
			else if (p->blank && cont->first)
				advance(p, p->fns - p->off, 0);
			else
				return cont->parent;
			break;
		case N_CODE:
			if (!cont->fenced) {
				if (p->indent >= CODE_INDENT) advance(p, CODE_INDENT, 1);
				else if (p->blank) advance(p, p->fns - p->off, 0);
				else return cont->parent;
			} else {
				int m = 0, k;
				if (p->indent <= 3 &&
				    p->fns < p->len && p->line[p->fns] == cont->fence_char)
					m = scan_close_fence(p, p->fns, cont->fence_char);
				if (m >= cont->fence_len) {
					/* The closing fence consumes the whole line. */
					advance(p, (size_t)m, 0);
					p->cur = finalize(p, cont);
					return NULL;
				}
				for (k = cont->fence_off;
				     k > 0 && p->off < p->len &&
				     is_space_or_tab(p->line[p->off]); k--)
					advance(p, 1, 1);
			}
			break;
		case N_PARA:
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

/* shortcut: block starts are added here commit by commit; see PLAN.md for
 * which are still missing. */
static void open_new_blocks(Parser *p, Node **cont, int all_matched) {
	size_t matched;
	int lev;
	ListData ld;
	int maybe_lazy = p->cur->t == N_PARA;
	while ((*cont)->t != N_CODE && (*cont)->t != N_HTML_BLOCK) {
		int indented;
		find_first_nonspace(p);
		indented = p->indent >= CODE_INDENT;

		if (!indented && p->fns < p->len && p->line[p->fns] == '>') {
			advance(p, p->fns + 1 - p->off, 0);
			if (p->off < p->len && is_space_or_tab(p->line[p->off]))
				advance(p, 1, 1);
			*cont = add_child(p, *cont, N_BQUOTE);
		} else if (!indented && (matched = scan_atx_start(p, p->fns)) != 0) {
			size_t h = p->fns;
			int level = 0;
			advance(p, p->fns + matched - p->off, 0);
			*cont = add_child(p, *cont, N_HEADING);
			while (h < p->len && p->line[h] == '#') { level++; h++; }
			(*cont)->level = level;
		} else if (!indented && (matched = (size_t)scan_open_fence(p, p->fns))
		           != 0) {
			char fc = p->line[p->fns];
			int off = (int)(p->fns - p->off);
			advance(p, p->fns + matched - p->off, 0);
			*cont = add_child(p, *cont, N_CODE);
			(*cont)->fenced = 1;
			(*cont)->fence_char = fc;
			(*cont)->fence_len = (int)matched;
			(*cont)->fence_off = off;
		} else if (!indented && (*cont)->t == N_PARA && all_matched &&
		           (lev = scan_setext(p, p->fns)) != 0) {
			(*cont)->t = N_HEADING;
			(*cont)->level = lev;
			(*cont)->setext = 1;
			advance(p, p->len - 1 - p->off, 0);
		} else if (!indented && !((*cont)->t == N_PARA && !all_matched) &&
		           scan_hrule(p, p->fns)) {
			*cont = add_child(p, *cont, N_HRULE);
			advance(p, p->len - 1 - p->off, 0);
		} else if ((!indented || (*cont)->t == N_LIST) && p->indent < 4 &&
		           (matched = parse_list_marker(p, p->fns,
		                                        (*cont)->t == N_PARA, &ld))
		           != 0) {
			size_t save_off, save_col;
			int save_tab, pad;
			advance(p, p->fns + matched - p->off, 0);
			save_tab = p->partial_tab;
			save_off = p->off;
			save_col = p->col;
			while (p->col - save_col <= 5 && p->off < p->len &&
			       is_space_or_tab(p->line[p->off]))
				advance(p, 1, 1);
			pad = (int)(p->col - save_col);
			/* A marker followed by five or more spaces, or by nothing at
			 * all, takes one space of padding and the rest is content. */
			if (pad >= 5 || pad < 1 ||
			    (p->off < p->len && is_line_end(p->line[p->off]))) {
				ld.padding = (int)matched + 1;
				p->off = save_off;
				p->col = save_col;
				p->partial_tab = save_tab;
				if (p->off < p->len && is_space_or_tab(p->line[p->off]))
					advance(p, 1, 1);
			} else {
				ld.padding = (int)matched + pad;
			}
			ld.marker_off = p->indent;
			if ((*cont)->t != N_LIST || !lists_match(*cont, &ld)) {
				*cont = add_child(p, *cont, N_LIST);
				apply_list(*cont, &ld);
			}
			if (p->too_deep) return;
			*cont = add_child(p, *cont, N_ITEM);
			apply_list(*cont, &ld);
		} else if (indented && !maybe_lazy && !p->blank) {
			advance(p, CODE_INDENT, 1);
			*cont = add_child(p, *cont, N_CODE);
		} else {
			break;
		}
		if (p->too_deep) return;
		if (accepts_lines((*cont)->t)) break;
		maybe_lazy = 0;
	}
}

static void add_text_to_container(Parser *p, Node *cont, Node *last_matched) {
	Node *tmp;
	find_first_nonspace(p);

	if (p->blank && cont->last) cont->last->last_line_blank = 1;
	cont->last_line_blank =
	    p->blank && cont->t != N_BQUOTE && cont->t != N_HEADING &&
	    cont->t != N_HRULE && !(cont->t == N_CODE && cont->fenced) &&
	    !(cont->t == N_ITEM && !cont->first && cont->start_line == p->lineno);
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
		if (cont->t == N_HEADING && !cont->setext) chop_hashes(p);
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
	case N_PARA: {
		/* A paragraph directly inside an item of a tight list has no <p>.
		 * "Directly" matters: a paragraph in a block quote in a tight item
		 * keeps its tags, because the grandparent is the quote. */
		Node *gp = n->parent ? n->parent->parent : NULL;
		int tight = gp && gp->t == N_LIST && gp->tight;
		if (!tight) { cr(out); bputs(out, "<p>"); }
		render_children(out, n, depth);
		if (!tight) bputs(out, "</p>\n");
		break;
	}
	case N_HEADING: {
		char tag[8];
		cr(out);
		snprintf(tag, sizeof tag, "<h%d>", n->level);
		bputs(out, tag);
		render_children(out, n, depth);
		snprintf(tag, sizeof tag, "</h%d>\n", n->level);
		bputs(out, tag);
		break;
	}
	case N_LIST:
		cr(out);
		if (!n->ordered) bputs(out, "<ul>\n");
		else if (n->start == 1) bputs(out, "<ol>\n");
		else {
			char tag[32];
			snprintf(tag, sizeof tag, "<ol start=\"%d\">\n", n->start);
			bputs(out, tag);
		}
		render_children(out, n, depth);
		bputs(out, n->ordered ? "</ol>\n" : "</ul>\n");
		break;
	case N_ITEM:
		cr(out);
		bputs(out, "<li>");
		render_children(out, n, depth);
		bputs(out, "</li>\n");
		break;
	case N_BQUOTE:
		cr(out);
		bputs(out, "<blockquote>\n");
		render_children(out, n, depth);
		cr(out);
		bputs(out, "</blockquote>\n");
		break;
	case N_HRULE:
		cr(out);
		bputs(out, "<hr />\n");
		break;
	case N_CODE:
		cr(out);
		if (n->info.n == 0) {
			bputs(out, "<pre><code>");
		} else {
			size_t k = 0;
			while (k < n->info.n && !c_isspace((unsigned char)n->info.p[k]))
				k++;
			bputs(out, "<pre><code class=\"language-");
			escape_html(out, n->info.p, k);
			bputs(out, "\">");
		}
		escape_html(out, n->content.p, n->content.n);
		bputs(out, "</code></pre>\n");
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
			p.lineno++;
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
