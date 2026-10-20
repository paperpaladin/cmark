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

/* Shrink to exactly the bytes held, plus the NUL byte bput() maintains at
 * p[n].  bgrow() doubles and never shrinks, so a buffer that has finished
 * growing is up to twice the size of its contents, and every byte of that
 * slack is a live allocation ASan is happy to let anybody read.  A read one
 * past the end of a 16-byte document lands 15 bytes inside a 32-byte block
 * and the sanitizer says nothing, which means every clean run over this
 * parser was clean over a larger region than the document.  Call this
 * wherever a buffer stops being written and starts being parsed; `make
 * overread` is the gate that proves it is still called. */
static void bfit(Buf *b) {
	char *q;
	if (!b->p || b->cap == b->n + 1) return;
	q = realloc(b->p, b->n + 1);
	if (!q) oom();
	b->p = q;
	b->cap = b->n + 1;
	b->p[b->n] = 0;
}

static void bfree(Buf *b) {
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}

/* ------------------------------------------------------- character classes */

static int is_space_or_tab(int c) { return c == ' ' || c == '\t'; }
static int c_isalpha(int c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int c_isalnum(int c) {
	return c_isalpha(c) || (c >= '0' && c <= '9');
}
static int c_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* ASCII case-insensitive compare of a fixed length.  strncasecmp is POSIX,
 * not C11, and this file compiles with -std=c11 and no feature macros. */
static int ci_eq(const char *a, const char *b, size_t n) {
	size_t i;
	for (i = 0; i < n; i++)
		if (c_lower((unsigned char)a[i]) != c_lower((unsigned char)b[i]))
			return 0;
	return 1;
}
static int is_line_end(int c) { return c == '\n' || c == '\r'; }
#define c_isspace c_isspace_fwd

/* ------------------------------------------------------------------- UTF-8 */

/* Decodes one codepoint.  Any ill-formed byte is reported as U+FFFD and
 * consumed singly, which keeps the scanners advancing on arbitrary bytes. */
static int utf8_decode(const char *s, size_t n, size_t i, int32_t *cp) {
	unsigned char c = (unsigned char)s[i];
	int len, j;
	int32_t v;
	if (c < 0x80) { *cp = c; return 1; }
	if ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1F; }
	else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; }
	else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; }
	else { *cp = 0xFFFD; return 1; }
	if (i + (size_t)len > n) { *cp = 0xFFFD; return 1; }
	for (j = 1; j < len; j++) {
		if (((unsigned char)s[i + j] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
		v = (v << 6) | ((unsigned char)s[i + j] & 0x3F);
	}
	*cp = v;
	return len;
}

static void utf8_encode(Buf *b, int32_t cp) {
	if (cp < 0 || cp > 0x10FFFF) cp = 0xFFFD;
	if (cp < 0x80) {
		bputc(b, (char)cp);
	} else if (cp < 0x800) {
		bputc(b, (char)(0xC0 | (cp >> 6)));
		bputc(b, (char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		bputc(b, (char)(0xE0 | (cp >> 12)));
		bputc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		bputc(b, (char)(0x80 | (cp & 0x3F)));
	} else {
		bputc(b, (char)(0xF0 | (cp >> 18)));
		bputc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
		bputc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		bputc(b, (char)(0x80 | (cp & 0x3F)));
	}
}

/* Unicode case folding, for link label matching.  681 runs of (first, last,
 * delta) covering the 1426 single-character foldings, plus the 104 codepoints
 * that fold to more than one character (U+00DF -> "ss", U+1E9E -> "ss",
 * U+0390 -> three codepoints, and so on).  Generated from CPython's
 * str.casefold(), Unicode 15.1.0.
 *
 * The multi-character half is not optional: the spec suite matches the label
 * [SS] against the reference [U+1E9E], which only works under full folding. */
static const int32_t CASEFOLD[][3] = {
    {0x41,0x5A,32}, {0xB5,0xB5,775}, {0xC0,0xD6,32}, {0xD8,0xDE,32},
    {0x100,0x100,1}, {0x102,0x102,1}, {0x104,0x104,1}, {0x106,0x106,1},
    {0x108,0x108,1}, {0x10A,0x10A,1}, {0x10C,0x10C,1}, {0x10E,0x10E,1},
    {0x110,0x110,1}, {0x112,0x112,1}, {0x114,0x114,1}, {0x116,0x116,1},
    {0x118,0x118,1}, {0x11A,0x11A,1}, {0x11C,0x11C,1}, {0x11E,0x11E,1},
    {0x120,0x120,1}, {0x122,0x122,1}, {0x124,0x124,1}, {0x126,0x126,1},
    {0x128,0x128,1}, {0x12A,0x12A,1}, {0x12C,0x12C,1}, {0x12E,0x12E,1},
    {0x132,0x132,1}, {0x134,0x134,1}, {0x136,0x136,1}, {0x139,0x139,1},
    {0x13B,0x13B,1}, {0x13D,0x13D,1}, {0x13F,0x13F,1}, {0x141,0x141,1},
    {0x143,0x143,1}, {0x145,0x145,1}, {0x147,0x147,1}, {0x14A,0x14A,1},
    {0x14C,0x14C,1}, {0x14E,0x14E,1}, {0x150,0x150,1}, {0x152,0x152,1},
    {0x154,0x154,1}, {0x156,0x156,1}, {0x158,0x158,1}, {0x15A,0x15A,1},
    {0x15C,0x15C,1}, {0x15E,0x15E,1}, {0x160,0x160,1}, {0x162,0x162,1},
    {0x164,0x164,1}, {0x166,0x166,1}, {0x168,0x168,1}, {0x16A,0x16A,1},
    {0x16C,0x16C,1}, {0x16E,0x16E,1}, {0x170,0x170,1}, {0x172,0x172,1},
    {0x174,0x174,1}, {0x176,0x176,1}, {0x178,0x178,-121}, {0x179,0x179,1},
    {0x17B,0x17B,1}, {0x17D,0x17D,1}, {0x17F,0x17F,-268}, {0x181,0x181,210},
    {0x182,0x182,1}, {0x184,0x184,1}, {0x186,0x186,206}, {0x187,0x187,1},
    {0x189,0x18A,205}, {0x18B,0x18B,1}, {0x18E,0x18E,79}, {0x18F,0x18F,202},
    {0x190,0x190,203}, {0x191,0x191,1}, {0x193,0x193,205}, {0x194,0x194,207},
    {0x196,0x196,211}, {0x197,0x197,209}, {0x198,0x198,1}, {0x19C,0x19C,211},
    {0x19D,0x19D,213}, {0x19F,0x19F,214}, {0x1A0,0x1A0,1}, {0x1A2,0x1A2,1},
    {0x1A4,0x1A4,1}, {0x1A6,0x1A6,218}, {0x1A7,0x1A7,1}, {0x1A9,0x1A9,218},
    {0x1AC,0x1AC,1}, {0x1AE,0x1AE,218}, {0x1AF,0x1AF,1}, {0x1B1,0x1B2,217},
    {0x1B3,0x1B3,1}, {0x1B5,0x1B5,1}, {0x1B7,0x1B7,219}, {0x1B8,0x1B8,1},
    {0x1BC,0x1BC,1}, {0x1C4,0x1C4,2}, {0x1C5,0x1C5,1}, {0x1C7,0x1C7,2},
    {0x1C8,0x1C8,1}, {0x1CA,0x1CA,2}, {0x1CB,0x1CB,1}, {0x1CD,0x1CD,1},
    {0x1CF,0x1CF,1}, {0x1D1,0x1D1,1}, {0x1D3,0x1D3,1}, {0x1D5,0x1D5,1},
    {0x1D7,0x1D7,1}, {0x1D9,0x1D9,1}, {0x1DB,0x1DB,1}, {0x1DE,0x1DE,1},
    {0x1E0,0x1E0,1}, {0x1E2,0x1E2,1}, {0x1E4,0x1E4,1}, {0x1E6,0x1E6,1},
    {0x1E8,0x1E8,1}, {0x1EA,0x1EA,1}, {0x1EC,0x1EC,1}, {0x1EE,0x1EE,1},
    {0x1F1,0x1F1,2}, {0x1F2,0x1F2,1}, {0x1F4,0x1F4,1}, {0x1F6,0x1F6,-97},
    {0x1F7,0x1F7,-56}, {0x1F8,0x1F8,1}, {0x1FA,0x1FA,1}, {0x1FC,0x1FC,1},
    {0x1FE,0x1FE,1}, {0x200,0x200,1}, {0x202,0x202,1}, {0x204,0x204,1},
    {0x206,0x206,1}, {0x208,0x208,1}, {0x20A,0x20A,1}, {0x20C,0x20C,1},
    {0x20E,0x20E,1}, {0x210,0x210,1}, {0x212,0x212,1}, {0x214,0x214,1},
    {0x216,0x216,1}, {0x218,0x218,1}, {0x21A,0x21A,1}, {0x21C,0x21C,1},
    {0x21E,0x21E,1}, {0x220,0x220,-130}, {0x222,0x222,1}, {0x224,0x224,1},
    {0x226,0x226,1}, {0x228,0x228,1}, {0x22A,0x22A,1}, {0x22C,0x22C,1},
    {0x22E,0x22E,1}, {0x230,0x230,1}, {0x232,0x232,1}, {0x23A,0x23A,10795},
    {0x23B,0x23B,1}, {0x23D,0x23D,-163}, {0x23E,0x23E,10792}, {0x241,0x241,1},
    {0x243,0x243,-195}, {0x244,0x244,69}, {0x245,0x245,71}, {0x246,0x246,1},
    {0x248,0x248,1}, {0x24A,0x24A,1}, {0x24C,0x24C,1}, {0x24E,0x24E,1},
    {0x345,0x345,116}, {0x370,0x370,1}, {0x372,0x372,1}, {0x376,0x376,1},
    {0x37F,0x37F,116}, {0x386,0x386,38}, {0x388,0x38A,37}, {0x38C,0x38C,64},
    {0x38E,0x38F,63}, {0x391,0x3A1,32}, {0x3A3,0x3AB,32}, {0x3C2,0x3C2,1},
    {0x3CF,0x3CF,8}, {0x3D0,0x3D0,-30}, {0x3D1,0x3D1,-25}, {0x3D5,0x3D5,-15},
    {0x3D6,0x3D6,-22}, {0x3D8,0x3D8,1}, {0x3DA,0x3DA,1}, {0x3DC,0x3DC,1},
    {0x3DE,0x3DE,1}, {0x3E0,0x3E0,1}, {0x3E2,0x3E2,1}, {0x3E4,0x3E4,1},
    {0x3E6,0x3E6,1}, {0x3E8,0x3E8,1}, {0x3EA,0x3EA,1}, {0x3EC,0x3EC,1},
    {0x3EE,0x3EE,1}, {0x3F0,0x3F0,-54}, {0x3F1,0x3F1,-48}, {0x3F4,0x3F4,-60},
    {0x3F5,0x3F5,-64}, {0x3F7,0x3F7,1}, {0x3F9,0x3F9,-7}, {0x3FA,0x3FA,1},
    {0x3FD,0x3FF,-130}, {0x400,0x40F,80}, {0x410,0x42F,32}, {0x460,0x460,1},
    {0x462,0x462,1}, {0x464,0x464,1}, {0x466,0x466,1}, {0x468,0x468,1},
    {0x46A,0x46A,1}, {0x46C,0x46C,1}, {0x46E,0x46E,1}, {0x470,0x470,1},
    {0x472,0x472,1}, {0x474,0x474,1}, {0x476,0x476,1}, {0x478,0x478,1},
    {0x47A,0x47A,1}, {0x47C,0x47C,1}, {0x47E,0x47E,1}, {0x480,0x480,1},
    {0x48A,0x48A,1}, {0x48C,0x48C,1}, {0x48E,0x48E,1}, {0x490,0x490,1},
    {0x492,0x492,1}, {0x494,0x494,1}, {0x496,0x496,1}, {0x498,0x498,1},
    {0x49A,0x49A,1}, {0x49C,0x49C,1}, {0x49E,0x49E,1}, {0x4A0,0x4A0,1},
    {0x4A2,0x4A2,1}, {0x4A4,0x4A4,1}, {0x4A6,0x4A6,1}, {0x4A8,0x4A8,1},
    {0x4AA,0x4AA,1}, {0x4AC,0x4AC,1}, {0x4AE,0x4AE,1}, {0x4B0,0x4B0,1},
    {0x4B2,0x4B2,1}, {0x4B4,0x4B4,1}, {0x4B6,0x4B6,1}, {0x4B8,0x4B8,1},
    {0x4BA,0x4BA,1}, {0x4BC,0x4BC,1}, {0x4BE,0x4BE,1}, {0x4C0,0x4C0,15},
    {0x4C1,0x4C1,1}, {0x4C3,0x4C3,1}, {0x4C5,0x4C5,1}, {0x4C7,0x4C7,1},
    {0x4C9,0x4C9,1}, {0x4CB,0x4CB,1}, {0x4CD,0x4CD,1}, {0x4D0,0x4D0,1},
    {0x4D2,0x4D2,1}, {0x4D4,0x4D4,1}, {0x4D6,0x4D6,1}, {0x4D8,0x4D8,1},
    {0x4DA,0x4DA,1}, {0x4DC,0x4DC,1}, {0x4DE,0x4DE,1}, {0x4E0,0x4E0,1},
    {0x4E2,0x4E2,1}, {0x4E4,0x4E4,1}, {0x4E6,0x4E6,1}, {0x4E8,0x4E8,1},
    {0x4EA,0x4EA,1}, {0x4EC,0x4EC,1}, {0x4EE,0x4EE,1}, {0x4F0,0x4F0,1},
    {0x4F2,0x4F2,1}, {0x4F4,0x4F4,1}, {0x4F6,0x4F6,1}, {0x4F8,0x4F8,1},
    {0x4FA,0x4FA,1}, {0x4FC,0x4FC,1}, {0x4FE,0x4FE,1}, {0x500,0x500,1},
    {0x502,0x502,1}, {0x504,0x504,1}, {0x506,0x506,1}, {0x508,0x508,1},
    {0x50A,0x50A,1}, {0x50C,0x50C,1}, {0x50E,0x50E,1}, {0x510,0x510,1},
    {0x512,0x512,1}, {0x514,0x514,1}, {0x516,0x516,1}, {0x518,0x518,1},
    {0x51A,0x51A,1}, {0x51C,0x51C,1}, {0x51E,0x51E,1}, {0x520,0x520,1},
    {0x522,0x522,1}, {0x524,0x524,1}, {0x526,0x526,1}, {0x528,0x528,1},
    {0x52A,0x52A,1}, {0x52C,0x52C,1}, {0x52E,0x52E,1}, {0x531,0x556,48},
    {0x10A0,0x10C5,7264}, {0x10C7,0x10C7,7264}, {0x10CD,0x10CD,7264},
    {0x13F8,0x13FD,-8}, {0x1C80,0x1C80,-6222}, {0x1C81,0x1C81,-6221},
    {0x1C82,0x1C82,-6212}, {0x1C83,0x1C84,-6210}, {0x1C85,0x1C85,-6211},
    {0x1C86,0x1C86,-6204}, {0x1C87,0x1C87,-6180}, {0x1C88,0x1C88,35267},
    {0x1C90,0x1CBA,-3008}, {0x1CBD,0x1CBF,-3008}, {0x1E00,0x1E00,1},
    {0x1E02,0x1E02,1}, {0x1E04,0x1E04,1}, {0x1E06,0x1E06,1},
    {0x1E08,0x1E08,1}, {0x1E0A,0x1E0A,1}, {0x1E0C,0x1E0C,1},
    {0x1E0E,0x1E0E,1}, {0x1E10,0x1E10,1}, {0x1E12,0x1E12,1},
    {0x1E14,0x1E14,1}, {0x1E16,0x1E16,1}, {0x1E18,0x1E18,1},
    {0x1E1A,0x1E1A,1}, {0x1E1C,0x1E1C,1}, {0x1E1E,0x1E1E,1},
    {0x1E20,0x1E20,1}, {0x1E22,0x1E22,1}, {0x1E24,0x1E24,1},
    {0x1E26,0x1E26,1}, {0x1E28,0x1E28,1}, {0x1E2A,0x1E2A,1},
    {0x1E2C,0x1E2C,1}, {0x1E2E,0x1E2E,1}, {0x1E30,0x1E30,1},
    {0x1E32,0x1E32,1}, {0x1E34,0x1E34,1}, {0x1E36,0x1E36,1},
    {0x1E38,0x1E38,1}, {0x1E3A,0x1E3A,1}, {0x1E3C,0x1E3C,1},
    {0x1E3E,0x1E3E,1}, {0x1E40,0x1E40,1}, {0x1E42,0x1E42,1},
    {0x1E44,0x1E44,1}, {0x1E46,0x1E46,1}, {0x1E48,0x1E48,1},
    {0x1E4A,0x1E4A,1}, {0x1E4C,0x1E4C,1}, {0x1E4E,0x1E4E,1},
    {0x1E50,0x1E50,1}, {0x1E52,0x1E52,1}, {0x1E54,0x1E54,1},
    {0x1E56,0x1E56,1}, {0x1E58,0x1E58,1}, {0x1E5A,0x1E5A,1},
    {0x1E5C,0x1E5C,1}, {0x1E5E,0x1E5E,1}, {0x1E60,0x1E60,1},
    {0x1E62,0x1E62,1}, {0x1E64,0x1E64,1}, {0x1E66,0x1E66,1},
    {0x1E68,0x1E68,1}, {0x1E6A,0x1E6A,1}, {0x1E6C,0x1E6C,1},
    {0x1E6E,0x1E6E,1}, {0x1E70,0x1E70,1}, {0x1E72,0x1E72,1},
    {0x1E74,0x1E74,1}, {0x1E76,0x1E76,1}, {0x1E78,0x1E78,1},
    {0x1E7A,0x1E7A,1}, {0x1E7C,0x1E7C,1}, {0x1E7E,0x1E7E,1},
    {0x1E80,0x1E80,1}, {0x1E82,0x1E82,1}, {0x1E84,0x1E84,1},
    {0x1E86,0x1E86,1}, {0x1E88,0x1E88,1}, {0x1E8A,0x1E8A,1},
    {0x1E8C,0x1E8C,1}, {0x1E8E,0x1E8E,1}, {0x1E90,0x1E90,1},
    {0x1E92,0x1E92,1}, {0x1E94,0x1E94,1}, {0x1E9B,0x1E9B,-58},
    {0x1EA0,0x1EA0,1}, {0x1EA2,0x1EA2,1}, {0x1EA4,0x1EA4,1},
    {0x1EA6,0x1EA6,1}, {0x1EA8,0x1EA8,1}, {0x1EAA,0x1EAA,1},
    {0x1EAC,0x1EAC,1}, {0x1EAE,0x1EAE,1}, {0x1EB0,0x1EB0,1},
    {0x1EB2,0x1EB2,1}, {0x1EB4,0x1EB4,1}, {0x1EB6,0x1EB6,1},
    {0x1EB8,0x1EB8,1}, {0x1EBA,0x1EBA,1}, {0x1EBC,0x1EBC,1},
    {0x1EBE,0x1EBE,1}, {0x1EC0,0x1EC0,1}, {0x1EC2,0x1EC2,1},
    {0x1EC4,0x1EC4,1}, {0x1EC6,0x1EC6,1}, {0x1EC8,0x1EC8,1},
    {0x1ECA,0x1ECA,1}, {0x1ECC,0x1ECC,1}, {0x1ECE,0x1ECE,1},
    {0x1ED0,0x1ED0,1}, {0x1ED2,0x1ED2,1}, {0x1ED4,0x1ED4,1},
    {0x1ED6,0x1ED6,1}, {0x1ED8,0x1ED8,1}, {0x1EDA,0x1EDA,1},
    {0x1EDC,0x1EDC,1}, {0x1EDE,0x1EDE,1}, {0x1EE0,0x1EE0,1},
    {0x1EE2,0x1EE2,1}, {0x1EE4,0x1EE4,1}, {0x1EE6,0x1EE6,1},
    {0x1EE8,0x1EE8,1}, {0x1EEA,0x1EEA,1}, {0x1EEC,0x1EEC,1},
    {0x1EEE,0x1EEE,1}, {0x1EF0,0x1EF0,1}, {0x1EF2,0x1EF2,1},
    {0x1EF4,0x1EF4,1}, {0x1EF6,0x1EF6,1}, {0x1EF8,0x1EF8,1},
    {0x1EFA,0x1EFA,1}, {0x1EFC,0x1EFC,1}, {0x1EFE,0x1EFE,1},
    {0x1F08,0x1F0F,-8}, {0x1F18,0x1F1D,-8}, {0x1F28,0x1F2F,-8},
    {0x1F38,0x1F3F,-8}, {0x1F48,0x1F4D,-8}, {0x1F59,0x1F59,-8},
    {0x1F5B,0x1F5B,-8}, {0x1F5D,0x1F5D,-8}, {0x1F5F,0x1F5F,-8},
    {0x1F68,0x1F6F,-8}, {0x1FB8,0x1FB9,-8}, {0x1FBA,0x1FBB,-74},
    {0x1FBE,0x1FBE,-7173}, {0x1FC8,0x1FCB,-86}, {0x1FD8,0x1FD9,-8},
    {0x1FDA,0x1FDB,-100}, {0x1FE8,0x1FE9,-8}, {0x1FEA,0x1FEB,-112},
    {0x1FEC,0x1FEC,-7}, {0x1FF8,0x1FF9,-128}, {0x1FFA,0x1FFB,-126},
    {0x2126,0x2126,-7517}, {0x212A,0x212A,-8383}, {0x212B,0x212B,-8262},
    {0x2132,0x2132,28}, {0x2160,0x216F,16}, {0x2183,0x2183,1},
    {0x24B6,0x24CF,26}, {0x2C00,0x2C2F,48}, {0x2C60,0x2C60,1},
    {0x2C62,0x2C62,-10743}, {0x2C63,0x2C63,-3814}, {0x2C64,0x2C64,-10727},
    {0x2C67,0x2C67,1}, {0x2C69,0x2C69,1}, {0x2C6B,0x2C6B,1},
    {0x2C6D,0x2C6D,-10780}, {0x2C6E,0x2C6E,-10749}, {0x2C6F,0x2C6F,-10783},
    {0x2C70,0x2C70,-10782}, {0x2C72,0x2C72,1}, {0x2C75,0x2C75,1},
    {0x2C7E,0x2C7F,-10815}, {0x2C80,0x2C80,1}, {0x2C82,0x2C82,1},
    {0x2C84,0x2C84,1}, {0x2C86,0x2C86,1}, {0x2C88,0x2C88,1},
    {0x2C8A,0x2C8A,1}, {0x2C8C,0x2C8C,1}, {0x2C8E,0x2C8E,1},
    {0x2C90,0x2C90,1}, {0x2C92,0x2C92,1}, {0x2C94,0x2C94,1},
    {0x2C96,0x2C96,1}, {0x2C98,0x2C98,1}, {0x2C9A,0x2C9A,1},
    {0x2C9C,0x2C9C,1}, {0x2C9E,0x2C9E,1}, {0x2CA0,0x2CA0,1},
    {0x2CA2,0x2CA2,1}, {0x2CA4,0x2CA4,1}, {0x2CA6,0x2CA6,1},
    {0x2CA8,0x2CA8,1}, {0x2CAA,0x2CAA,1}, {0x2CAC,0x2CAC,1},
    {0x2CAE,0x2CAE,1}, {0x2CB0,0x2CB0,1}, {0x2CB2,0x2CB2,1},
    {0x2CB4,0x2CB4,1}, {0x2CB6,0x2CB6,1}, {0x2CB8,0x2CB8,1},
    {0x2CBA,0x2CBA,1}, {0x2CBC,0x2CBC,1}, {0x2CBE,0x2CBE,1},
    {0x2CC0,0x2CC0,1}, {0x2CC2,0x2CC2,1}, {0x2CC4,0x2CC4,1},
    {0x2CC6,0x2CC6,1}, {0x2CC8,0x2CC8,1}, {0x2CCA,0x2CCA,1},
    {0x2CCC,0x2CCC,1}, {0x2CCE,0x2CCE,1}, {0x2CD0,0x2CD0,1},
    {0x2CD2,0x2CD2,1}, {0x2CD4,0x2CD4,1}, {0x2CD6,0x2CD6,1},
    {0x2CD8,0x2CD8,1}, {0x2CDA,0x2CDA,1}, {0x2CDC,0x2CDC,1},
    {0x2CDE,0x2CDE,1}, {0x2CE0,0x2CE0,1}, {0x2CE2,0x2CE2,1},
    {0x2CEB,0x2CEB,1}, {0x2CED,0x2CED,1}, {0x2CF2,0x2CF2,1},
    {0xA640,0xA640,1}, {0xA642,0xA642,1}, {0xA644,0xA644,1},
    {0xA646,0xA646,1}, {0xA648,0xA648,1}, {0xA64A,0xA64A,1},
    {0xA64C,0xA64C,1}, {0xA64E,0xA64E,1}, {0xA650,0xA650,1},
    {0xA652,0xA652,1}, {0xA654,0xA654,1}, {0xA656,0xA656,1},
    {0xA658,0xA658,1}, {0xA65A,0xA65A,1}, {0xA65C,0xA65C,1},
    {0xA65E,0xA65E,1}, {0xA660,0xA660,1}, {0xA662,0xA662,1},
    {0xA664,0xA664,1}, {0xA666,0xA666,1}, {0xA668,0xA668,1},
    {0xA66A,0xA66A,1}, {0xA66C,0xA66C,1}, {0xA680,0xA680,1},
    {0xA682,0xA682,1}, {0xA684,0xA684,1}, {0xA686,0xA686,1},
    {0xA688,0xA688,1}, {0xA68A,0xA68A,1}, {0xA68C,0xA68C,1},
    {0xA68E,0xA68E,1}, {0xA690,0xA690,1}, {0xA692,0xA692,1},
    {0xA694,0xA694,1}, {0xA696,0xA696,1}, {0xA698,0xA698,1},
    {0xA69A,0xA69A,1}, {0xA722,0xA722,1}, {0xA724,0xA724,1},
    {0xA726,0xA726,1}, {0xA728,0xA728,1}, {0xA72A,0xA72A,1},
    {0xA72C,0xA72C,1}, {0xA72E,0xA72E,1}, {0xA732,0xA732,1},
    {0xA734,0xA734,1}, {0xA736,0xA736,1}, {0xA738,0xA738,1},
    {0xA73A,0xA73A,1}, {0xA73C,0xA73C,1}, {0xA73E,0xA73E,1},
    {0xA740,0xA740,1}, {0xA742,0xA742,1}, {0xA744,0xA744,1},
    {0xA746,0xA746,1}, {0xA748,0xA748,1}, {0xA74A,0xA74A,1},
    {0xA74C,0xA74C,1}, {0xA74E,0xA74E,1}, {0xA750,0xA750,1},
    {0xA752,0xA752,1}, {0xA754,0xA754,1}, {0xA756,0xA756,1},
    {0xA758,0xA758,1}, {0xA75A,0xA75A,1}, {0xA75C,0xA75C,1},
    {0xA75E,0xA75E,1}, {0xA760,0xA760,1}, {0xA762,0xA762,1},
    {0xA764,0xA764,1}, {0xA766,0xA766,1}, {0xA768,0xA768,1},
    {0xA76A,0xA76A,1}, {0xA76C,0xA76C,1}, {0xA76E,0xA76E,1},
    {0xA779,0xA779,1}, {0xA77B,0xA77B,1}, {0xA77D,0xA77D,-35332},
    {0xA77E,0xA77E,1}, {0xA780,0xA780,1}, {0xA782,0xA782,1},
    {0xA784,0xA784,1}, {0xA786,0xA786,1}, {0xA78B,0xA78B,1},
    {0xA78D,0xA78D,-42280}, {0xA790,0xA790,1}, {0xA792,0xA792,1},
    {0xA796,0xA796,1}, {0xA798,0xA798,1}, {0xA79A,0xA79A,1},
    {0xA79C,0xA79C,1}, {0xA79E,0xA79E,1}, {0xA7A0,0xA7A0,1},
    {0xA7A2,0xA7A2,1}, {0xA7A4,0xA7A4,1}, {0xA7A6,0xA7A6,1},
    {0xA7A8,0xA7A8,1}, {0xA7AA,0xA7AA,-42308}, {0xA7AB,0xA7AB,-42319},
    {0xA7AC,0xA7AC,-42315}, {0xA7AD,0xA7AD,-42305}, {0xA7AE,0xA7AE,-42308},
    {0xA7B0,0xA7B0,-42258}, {0xA7B1,0xA7B1,-42282}, {0xA7B2,0xA7B2,-42261},
    {0xA7B3,0xA7B3,928}, {0xA7B4,0xA7B4,1}, {0xA7B6,0xA7B6,1},
    {0xA7B8,0xA7B8,1}, {0xA7BA,0xA7BA,1}, {0xA7BC,0xA7BC,1},
    {0xA7BE,0xA7BE,1}, {0xA7C0,0xA7C0,1}, {0xA7C2,0xA7C2,1},
    {0xA7C4,0xA7C4,-48}, {0xA7C5,0xA7C5,-42307}, {0xA7C6,0xA7C6,-35384},
    {0xA7C7,0xA7C7,1}, {0xA7C9,0xA7C9,1}, {0xA7D0,0xA7D0,1},
    {0xA7D6,0xA7D6,1}, {0xA7D8,0xA7D8,1}, {0xA7F5,0xA7F5,1},
    {0xAB70,0xABBF,-38864}, {0xFF21,0xFF3A,32}, {0x10400,0x10427,40},
    {0x104B0,0x104D3,40}, {0x10570,0x1057A,39}, {0x1057C,0x1058A,39},
    {0x1058C,0x10592,39}, {0x10594,0x10595,39}, {0x10C80,0x10CB2,64},
    {0x118A0,0x118BF,32}, {0x16E40,0x16E5F,32}, {0x1E900,0x1E921,34}};

#define N_CASEFOLD (sizeof CASEFOLD / sizeof *CASEFOLD)

static const struct {
	int32_t cp;
	const char *s;
} CASEFOLD_MULTI[] = {
    {0xDF,"\163\163"}, {0x130,"\151\314\207"}, {0x149,"\312\274\156"},
    {0x1F0,"\152\314\214"}, {0x390,"\316\271\314\210\314\201"},
    {0x3B0,"\317\205\314\210\314\201"}, {0x587,"\325\245\326\202"},
    {0x1E96,"\150\314\261"}, {0x1E97,"\164\314\210"}, {0x1E98,"\167\314\212"},
    {0x1E99,"\171\314\212"}, {0x1E9A,"\141\312\276"}, {0x1E9E,"\163\163"},
    {0x1F50,"\317\205\314\223"}, {0x1F52,"\317\205\314\223\314\200"},
    {0x1F54,"\317\205\314\223\314\201"}, {0x1F56,"\317\205\314\223\315\202"},
    {0x1F80,"\341\274\200\316\271"}, {0x1F81,"\341\274\201\316\271"},
    {0x1F82,"\341\274\202\316\271"}, {0x1F83,"\341\274\203\316\271"},
    {0x1F84,"\341\274\204\316\271"}, {0x1F85,"\341\274\205\316\271"},
    {0x1F86,"\341\274\206\316\271"}, {0x1F87,"\341\274\207\316\271"},
    {0x1F88,"\341\274\200\316\271"}, {0x1F89,"\341\274\201\316\271"},
    {0x1F8A,"\341\274\202\316\271"}, {0x1F8B,"\341\274\203\316\271"},
    {0x1F8C,"\341\274\204\316\271"}, {0x1F8D,"\341\274\205\316\271"},
    {0x1F8E,"\341\274\206\316\271"}, {0x1F8F,"\341\274\207\316\271"},
    {0x1F90,"\341\274\240\316\271"}, {0x1F91,"\341\274\241\316\271"},
    {0x1F92,"\341\274\242\316\271"}, {0x1F93,"\341\274\243\316\271"},
    {0x1F94,"\341\274\244\316\271"}, {0x1F95,"\341\274\245\316\271"},
    {0x1F96,"\341\274\246\316\271"}, {0x1F97,"\341\274\247\316\271"},
    {0x1F98,"\341\274\240\316\271"}, {0x1F99,"\341\274\241\316\271"},
    {0x1F9A,"\341\274\242\316\271"}, {0x1F9B,"\341\274\243\316\271"},
    {0x1F9C,"\341\274\244\316\271"}, {0x1F9D,"\341\274\245\316\271"},
    {0x1F9E,"\341\274\246\316\271"}, {0x1F9F,"\341\274\247\316\271"},
    {0x1FA0,"\341\275\240\316\271"}, {0x1FA1,"\341\275\241\316\271"},
    {0x1FA2,"\341\275\242\316\271"}, {0x1FA3,"\341\275\243\316\271"},
    {0x1FA4,"\341\275\244\316\271"}, {0x1FA5,"\341\275\245\316\271"},
    {0x1FA6,"\341\275\246\316\271"}, {0x1FA7,"\341\275\247\316\271"},
    {0x1FA8,"\341\275\240\316\271"}, {0x1FA9,"\341\275\241\316\271"},
    {0x1FAA,"\341\275\242\316\271"}, {0x1FAB,"\341\275\243\316\271"},
    {0x1FAC,"\341\275\244\316\271"}, {0x1FAD,"\341\275\245\316\271"},
    {0x1FAE,"\341\275\246\316\271"}, {0x1FAF,"\341\275\247\316\271"},
    {0x1FB2,"\341\275\260\316\271"}, {0x1FB3,"\316\261\316\271"},
    {0x1FB4,"\316\254\316\271"}, {0x1FB6,"\316\261\315\202"},
    {0x1FB7,"\316\261\315\202\316\271"}, {0x1FBC,"\316\261\316\271"},
    {0x1FC2,"\341\275\264\316\271"}, {0x1FC3,"\316\267\316\271"},
    {0x1FC4,"\316\256\316\271"}, {0x1FC6,"\316\267\315\202"},
    {0x1FC7,"\316\267\315\202\316\271"}, {0x1FCC,"\316\267\316\271"},
    {0x1FD2,"\316\271\314\210\314\200"}, {0x1FD3,"\316\271\314\210\314\201"},
    {0x1FD6,"\316\271\315\202"}, {0x1FD7,"\316\271\314\210\315\202"},
    {0x1FE2,"\317\205\314\210\314\200"}, {0x1FE3,"\317\205\314\210\314\201"},
    {0x1FE4,"\317\201\314\223"}, {0x1FE6,"\317\205\315\202"},
    {0x1FE7,"\317\205\314\210\315\202"}, {0x1FF2,"\341\275\274\316\271"},
    {0x1FF3,"\317\211\316\271"}, {0x1FF4,"\317\216\316\271"},
    {0x1FF6,"\317\211\315\202"}, {0x1FF7,"\317\211\315\202\316\271"},
    {0x1FFC,"\317\211\316\271"}, {0xFB00,"\146\146"}, {0xFB01,"\146\151"},
    {0xFB02,"\146\154"}, {0xFB03,"\146\146\151"}, {0xFB04,"\146\146\154"},
    {0xFB05,"\163\164"}, {0xFB06,"\163\164"}, {0xFB13,"\325\264\325\266"},
    {0xFB14,"\325\264\325\245"}, {0xFB15,"\325\264\325\253"},
    {0xFB16,"\325\276\325\266"}, {0xFB17,"\325\264\325\255"}};

#define N_CASEFOLD_MULTI (sizeof CASEFOLD_MULTI / sizeof *CASEFOLD_MULTI)

static void fold_into(Buf *out, int32_t c) {
	size_t lo = 0, hi = N_CASEFOLD_MULTI;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (c < CASEFOLD_MULTI[mid].cp) hi = mid;
		else if (c > CASEFOLD_MULTI[mid].cp) lo = mid + 1;
		else { bputs(out, CASEFOLD_MULTI[mid].s); return; }
	}
	lo = 0;
	hi = N_CASEFOLD;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (c < CASEFOLD[mid][0]) hi = mid;
		else if (c > CASEFOLD[mid][1]) lo = mid + 1;
		else { utf8_encode(out, c + CASEFOLD[mid][2]); return; }
	}
	utf8_encode(out, c);
}

/* --------------------------------------------------------------- entities */

/* The HTML5 named character references, 2125 of them, as "name\0value" pairs
 * sorted by name so the lookup is a binary search.  Generated once with
 *
 *   python3 -c 'import html.entities; ...'   (see PLAN.md)
 *
 * from CPython's html.entities.html5, which is the WHATWG entity table.  The
 * values are UTF-8, written as octal escapes to keep this file ASCII.  Bulk
 * data: there is no algorithm here and no shortcut that would still pass
 * &ngE; and &DifferentialD; while rejecting &MadeUpEntity;. */
static const char *const ENTITIES[] = {
    "AElig\0\303\206", "AMP\0&", "Aacute\0\303\201", "Abreve\0\304\202",
    "Acirc\0\303\202", "Acy\0\320\220", "Afr\0\360\235\224\204",
    "Agrave\0\303\200", "Alpha\0\316\221", "Amacr\0\304\200",
    "And\0\342\251\223", "Aogon\0\304\204", "Aopf\0\360\235\224\270",
    "ApplyFunction\0\342\201\241", "Aring\0\303\205", "Ascr\0\360\235\222\234",
    "Assign\0\342\211\224", "Atilde\0\303\203", "Auml\0\303\204",
    "Backslash\0\342\210\226", "Barv\0\342\253\247", "Barwed\0\342\214\206",
    "Bcy\0\320\221", "Because\0\342\210\265", "Bernoullis\0\342\204\254",
    "Beta\0\316\222", "Bfr\0\360\235\224\205", "Bopf\0\360\235\224\271",
    "Breve\0\313\230", "Bscr\0\342\204\254", "Bumpeq\0\342\211\216",
    "CHcy\0\320\247", "COPY\0\302\251", "Cacute\0\304\206",
    "Cap\0\342\213\222", "CapitalDifferentialD\0\342\205\205",
    "Cayleys\0\342\204\255", "Ccaron\0\304\214", "Ccedil\0\303\207",
    "Ccirc\0\304\210", "Cconint\0\342\210\260", "Cdot\0\304\212",
    "Cedilla\0\302\270", "CenterDot\0\302\267", "Cfr\0\342\204\255",
    "Chi\0\316\247", "CircleDot\0\342\212\231", "CircleMinus\0\342\212\226",
    "CirclePlus\0\342\212\225", "CircleTimes\0\342\212\227",
    "ClockwiseContourIntegral\0\342\210\262",
    "CloseCurlyDoubleQuote\0\342\200\235", "CloseCurlyQuote\0\342\200\231",
    "Colon\0\342\210\267", "Colone\0\342\251\264", "Congruent\0\342\211\241",
    "Conint\0\342\210\257", "ContourIntegral\0\342\210\256",
    "Copf\0\342\204\202", "Coproduct\0\342\210\220",
    "CounterClockwiseContourIntegral\0\342\210\263", "Cross\0\342\250\257",
    "Cscr\0\360\235\222\236", "Cup\0\342\213\223", "CupCap\0\342\211\215",
    "DD\0\342\205\205", "DDotrahd\0\342\244\221", "DJcy\0\320\202",
    "DScy\0\320\205", "DZcy\0\320\217", "Dagger\0\342\200\241",
    "Darr\0\342\206\241", "Dashv\0\342\253\244", "Dcaron\0\304\216",
    "Dcy\0\320\224", "Del\0\342\210\207", "Delta\0\316\224",
    "Dfr\0\360\235\224\207", "DiacriticalAcute\0\302\264",
    "DiacriticalDot\0\313\231", "DiacriticalDoubleAcute\0\313\235",
    "DiacriticalGrave\0`", "DiacriticalTilde\0\313\234",
    "Diamond\0\342\213\204", "DifferentialD\0\342\205\206",
    "Dopf\0\360\235\224\273", "Dot\0\302\250", "DotDot\0\342\203\234",
    "DotEqual\0\342\211\220", "DoubleContourIntegral\0\342\210\257",
    "DoubleDot\0\302\250", "DoubleDownArrow\0\342\207\223",
    "DoubleLeftArrow\0\342\207\220", "DoubleLeftRightArrow\0\342\207\224",
    "DoubleLeftTee\0\342\253\244", "DoubleLongLeftArrow\0\342\237\270",
    "DoubleLongLeftRightArrow\0\342\237\272",
    "DoubleLongRightArrow\0\342\237\271", "DoubleRightArrow\0\342\207\222",
    "DoubleRightTee\0\342\212\250", "DoubleUpArrow\0\342\207\221",
    "DoubleUpDownArrow\0\342\207\225", "DoubleVerticalBar\0\342\210\245",
    "DownArrow\0\342\206\223", "DownArrowBar\0\342\244\223",
    "DownArrowUpArrow\0\342\207\265", "DownBreve\0\314\221",
    "DownLeftRightVector\0\342\245\220", "DownLeftTeeVector\0\342\245\236",
    "DownLeftVector\0\342\206\275", "DownLeftVectorBar\0\342\245\226",
    "DownRightTeeVector\0\342\245\237", "DownRightVector\0\342\207\201",
    "DownRightVectorBar\0\342\245\227", "DownTee\0\342\212\244",
    "DownTeeArrow\0\342\206\247", "Downarrow\0\342\207\223",
    "Dscr\0\360\235\222\237", "Dstrok\0\304\220", "ENG\0\305\212",
    "ETH\0\303\220", "Eacute\0\303\211", "Ecaron\0\304\232", "Ecirc\0\303\212",
    "Ecy\0\320\255", "Edot\0\304\226", "Efr\0\360\235\224\210",
    "Egrave\0\303\210", "Element\0\342\210\210", "Emacr\0\304\222",
    "EmptySmallSquare\0\342\227\273", "EmptyVerySmallSquare\0\342\226\253",
    "Eogon\0\304\230", "Eopf\0\360\235\224\274", "Epsilon\0\316\225",
    "Equal\0\342\251\265", "EqualTilde\0\342\211\202",
    "Equilibrium\0\342\207\214", "Escr\0\342\204\260", "Esim\0\342\251\263",
    "Eta\0\316\227", "Euml\0\303\213", "Exists\0\342\210\203",
    "ExponentialE\0\342\205\207", "Fcy\0\320\244", "Ffr\0\360\235\224\211",
    "FilledSmallSquare\0\342\227\274", "FilledVerySmallSquare\0\342\226\252",
    "Fopf\0\360\235\224\275", "ForAll\0\342\210\200",
    "Fouriertrf\0\342\204\261", "Fscr\0\342\204\261", "GJcy\0\320\203",
    "GT\0>", "Gamma\0\316\223", "Gammad\0\317\234", "Gbreve\0\304\236",
    "Gcedil\0\304\242", "Gcirc\0\304\234", "Gcy\0\320\223", "Gdot\0\304\240",
    "Gfr\0\360\235\224\212", "Gg\0\342\213\231", "Gopf\0\360\235\224\276",
    "GreaterEqual\0\342\211\245", "GreaterEqualLess\0\342\213\233",
    "GreaterFullEqual\0\342\211\247", "GreaterGreater\0\342\252\242",
    "GreaterLess\0\342\211\267", "GreaterSlantEqual\0\342\251\276",
    "GreaterTilde\0\342\211\263", "Gscr\0\360\235\222\242", "Gt\0\342\211\253",
    "HARDcy\0\320\252", "Hacek\0\313\207", "Hat\0^", "Hcirc\0\304\244",
    "Hfr\0\342\204\214", "HilbertSpace\0\342\204\213", "Hopf\0\342\204\215",
    "HorizontalLine\0\342\224\200", "Hscr\0\342\204\213", "Hstrok\0\304\246",
    "HumpDownHump\0\342\211\216", "HumpEqual\0\342\211\217", "IEcy\0\320\225",
    "IJlig\0\304\262", "IOcy\0\320\201", "Iacute\0\303\215", "Icirc\0\303\216",
    "Icy\0\320\230", "Idot\0\304\260", "Ifr\0\342\204\221", "Igrave\0\303\214",
    "Im\0\342\204\221", "Imacr\0\304\252", "ImaginaryI\0\342\205\210",
    "Implies\0\342\207\222", "Int\0\342\210\254", "Integral\0\342\210\253",
    "Intersection\0\342\213\202", "InvisibleComma\0\342\201\243",
    "InvisibleTimes\0\342\201\242", "Iogon\0\304\256",
    "Iopf\0\360\235\225\200", "Iota\0\316\231", "Iscr\0\342\204\220",
    "Itilde\0\304\250", "Iukcy\0\320\206", "Iuml\0\303\217", "Jcirc\0\304\264",
    "Jcy\0\320\231", "Jfr\0\360\235\224\215", "Jopf\0\360\235\225\201",
    "Jscr\0\360\235\222\245", "Jsercy\0\320\210", "Jukcy\0\320\204",
    "KHcy\0\320\245", "KJcy\0\320\214", "Kappa\0\316\232", "Kcedil\0\304\266",
    "Kcy\0\320\232", "Kfr\0\360\235\224\216", "Kopf\0\360\235\225\202",
    "Kscr\0\360\235\222\246", "LJcy\0\320\211", "LT\0<", "Lacute\0\304\271",
    "Lambda\0\316\233", "Lang\0\342\237\252", "Laplacetrf\0\342\204\222",
    "Larr\0\342\206\236", "Lcaron\0\304\275", "Lcedil\0\304\273",
    "Lcy\0\320\233", "LeftAngleBracket\0\342\237\250",
    "LeftArrow\0\342\206\220", "LeftArrowBar\0\342\207\244",
    "LeftArrowRightArrow\0\342\207\206", "LeftCeiling\0\342\214\210",
    "LeftDoubleBracket\0\342\237\246", "LeftDownTeeVector\0\342\245\241",
    "LeftDownVector\0\342\207\203", "LeftDownVectorBar\0\342\245\231",
    "LeftFloor\0\342\214\212", "LeftRightArrow\0\342\206\224",
    "LeftRightVector\0\342\245\216", "LeftTee\0\342\212\243",
    "LeftTeeArrow\0\342\206\244", "LeftTeeVector\0\342\245\232",
    "LeftTriangle\0\342\212\262", "LeftTriangleBar\0\342\247\217",
    "LeftTriangleEqual\0\342\212\264", "LeftUpDownVector\0\342\245\221",
    "LeftUpTeeVector\0\342\245\240", "LeftUpVector\0\342\206\277",
    "LeftUpVectorBar\0\342\245\230", "LeftVector\0\342\206\274",
    "LeftVectorBar\0\342\245\222", "Leftarrow\0\342\207\220",
    "Leftrightarrow\0\342\207\224", "LessEqualGreater\0\342\213\232",
    "LessFullEqual\0\342\211\246", "LessGreater\0\342\211\266",
    "LessLess\0\342\252\241", "LessSlantEqual\0\342\251\275",
    "LessTilde\0\342\211\262", "Lfr\0\360\235\224\217", "Ll\0\342\213\230",
    "Lleftarrow\0\342\207\232", "Lmidot\0\304\277",
    "LongLeftArrow\0\342\237\265", "LongLeftRightArrow\0\342\237\267",
    "LongRightArrow\0\342\237\266", "Longleftarrow\0\342\237\270",
    "Longleftrightarrow\0\342\237\272", "Longrightarrow\0\342\237\271",
    "Lopf\0\360\235\225\203", "LowerLeftArrow\0\342\206\231",
    "LowerRightArrow\0\342\206\230", "Lscr\0\342\204\222", "Lsh\0\342\206\260",
    "Lstrok\0\305\201", "Lt\0\342\211\252", "Map\0\342\244\205",
    "Mcy\0\320\234", "MediumSpace\0\342\201\237", "Mellintrf\0\342\204\263",
    "Mfr\0\360\235\224\220", "MinusPlus\0\342\210\223",
    "Mopf\0\360\235\225\204", "Mscr\0\342\204\263", "Mu\0\316\234",
    "NJcy\0\320\212", "Nacute\0\305\203", "Ncaron\0\305\207",
    "Ncedil\0\305\205", "Ncy\0\320\235", "NegativeMediumSpace\0\342\200\213",
    "NegativeThickSpace\0\342\200\213", "NegativeThinSpace\0\342\200\213",
    "NegativeVeryThinSpace\0\342\200\213",
    "NestedGreaterGreater\0\342\211\253", "NestedLessLess\0\342\211\252",
    "NewLine\0\012", "Nfr\0\360\235\224\221", "NoBreak\0\342\201\240",
    "NonBreakingSpace\0\302\240", "Nopf\0\342\204\225", "Not\0\342\253\254",
    "NotCongruent\0\342\211\242", "NotCupCap\0\342\211\255",
    "NotDoubleVerticalBar\0\342\210\246", "NotElement\0\342\210\211",
    "NotEqual\0\342\211\240", "NotEqualTilde\0\342\211\202\314\270",
    "NotExists\0\342\210\204", "NotGreater\0\342\211\257",
    "NotGreaterEqual\0\342\211\261",
    "NotGreaterFullEqual\0\342\211\247\314\270",
    "NotGreaterGreater\0\342\211\253\314\270", "NotGreaterLess\0\342\211\271",
    "NotGreaterSlantEqual\0\342\251\276\314\270",
    "NotGreaterTilde\0\342\211\265", "NotHumpDownHump\0\342\211\216\314\270",
    "NotHumpEqual\0\342\211\217\314\270", "NotLeftTriangle\0\342\213\252",
    "NotLeftTriangleBar\0\342\247\217\314\270",
    "NotLeftTriangleEqual\0\342\213\254", "NotLess\0\342\211\256",
    "NotLessEqual\0\342\211\260", "NotLessGreater\0\342\211\270",
    "NotLessLess\0\342\211\252\314\270",
    "NotLessSlantEqual\0\342\251\275\314\270", "NotLessTilde\0\342\211\264",
    "NotNestedGreaterGreater\0\342\252\242\314\270",
    "NotNestedLessLess\0\342\252\241\314\270", "NotPrecedes\0\342\212\200",
    "NotPrecedesEqual\0\342\252\257\314\270",
    "NotPrecedesSlantEqual\0\342\213\240", "NotReverseElement\0\342\210\214",
    "NotRightTriangle\0\342\213\253",
    "NotRightTriangleBar\0\342\247\220\314\270",
    "NotRightTriangleEqual\0\342\213\255",
    "NotSquareSubset\0\342\212\217\314\270",
    "NotSquareSubsetEqual\0\342\213\242",
    "NotSquareSuperset\0\342\212\220\314\270",
    "NotSquareSupersetEqual\0\342\213\243",
    "NotSubset\0\342\212\202\342\203\222", "NotSubsetEqual\0\342\212\210",
    "NotSucceeds\0\342\212\201", "NotSucceedsEqual\0\342\252\260\314\270",
    "NotSucceedsSlantEqual\0\342\213\241",
    "NotSucceedsTilde\0\342\211\277\314\270",
    "NotSuperset\0\342\212\203\342\203\222", "NotSupersetEqual\0\342\212\211",
    "NotTilde\0\342\211\201", "NotTildeEqual\0\342\211\204",
    "NotTildeFullEqual\0\342\211\207", "NotTildeTilde\0\342\211\211",
    "NotVerticalBar\0\342\210\244", "Nscr\0\360\235\222\251",
    "Ntilde\0\303\221", "Nu\0\316\235", "OElig\0\305\222", "Oacute\0\303\223",
    "Ocirc\0\303\224", "Ocy\0\320\236", "Odblac\0\305\220",
    "Ofr\0\360\235\224\222", "Ograve\0\303\222", "Omacr\0\305\214",
    "Omega\0\316\251", "Omicron\0\316\237", "Oopf\0\360\235\225\206",
    "OpenCurlyDoubleQuote\0\342\200\234", "OpenCurlyQuote\0\342\200\230",
    "Or\0\342\251\224", "Oscr\0\360\235\222\252", "Oslash\0\303\230",
    "Otilde\0\303\225", "Otimes\0\342\250\267", "Ouml\0\303\226",
    "OverBar\0\342\200\276", "OverBrace\0\342\217\236",
    "OverBracket\0\342\216\264", "OverParenthesis\0\342\217\234",
    "PartialD\0\342\210\202", "Pcy\0\320\237", "Pfr\0\360\235\224\223",
    "Phi\0\316\246", "Pi\0\316\240", "PlusMinus\0\302\261",
    "Poincareplane\0\342\204\214", "Popf\0\342\204\231", "Pr\0\342\252\273",
    "Precedes\0\342\211\272", "PrecedesEqual\0\342\252\257",
    "PrecedesSlantEqual\0\342\211\274", "PrecedesTilde\0\342\211\276",
    "Prime\0\342\200\263", "Product\0\342\210\217", "Proportion\0\342\210\267",
    "Proportional\0\342\210\235", "Pscr\0\360\235\222\253", "Psi\0\316\250",
    "QUOT\0\042", "Qfr\0\360\235\224\224", "Qopf\0\342\204\232",
    "Qscr\0\360\235\222\254", "RBarr\0\342\244\220", "REG\0\302\256",
    "Racute\0\305\224", "Rang\0\342\237\253", "Rarr\0\342\206\240",
    "Rarrtl\0\342\244\226", "Rcaron\0\305\230", "Rcedil\0\305\226",
    "Rcy\0\320\240", "Re\0\342\204\234", "ReverseElement\0\342\210\213",
    "ReverseEquilibrium\0\342\207\213", "ReverseUpEquilibrium\0\342\245\257",
    "Rfr\0\342\204\234", "Rho\0\316\241", "RightAngleBracket\0\342\237\251",
    "RightArrow\0\342\206\222", "RightArrowBar\0\342\207\245",
    "RightArrowLeftArrow\0\342\207\204", "RightCeiling\0\342\214\211",
    "RightDoubleBracket\0\342\237\247", "RightDownTeeVector\0\342\245\235",
    "RightDownVector\0\342\207\202", "RightDownVectorBar\0\342\245\225",
    "RightFloor\0\342\214\213", "RightTee\0\342\212\242",
    "RightTeeArrow\0\342\206\246", "RightTeeVector\0\342\245\233",
    "RightTriangle\0\342\212\263", "RightTriangleBar\0\342\247\220",
    "RightTriangleEqual\0\342\212\265", "RightUpDownVector\0\342\245\217",
    "RightUpTeeVector\0\342\245\234", "RightUpVector\0\342\206\276",
    "RightUpVectorBar\0\342\245\224", "RightVector\0\342\207\200",
    "RightVectorBar\0\342\245\223", "Rightarrow\0\342\207\222",
    "Ropf\0\342\204\235", "RoundImplies\0\342\245\260",
    "Rrightarrow\0\342\207\233", "Rscr\0\342\204\233", "Rsh\0\342\206\261",
    "RuleDelayed\0\342\247\264", "SHCHcy\0\320\251", "SHcy\0\320\250",
    "SOFTcy\0\320\254", "Sacute\0\305\232", "Sc\0\342\252\274",
    "Scaron\0\305\240", "Scedil\0\305\236", "Scirc\0\305\234", "Scy\0\320\241",
    "Sfr\0\360\235\224\226", "ShortDownArrow\0\342\206\223",
    "ShortLeftArrow\0\342\206\220", "ShortRightArrow\0\342\206\222",
    "ShortUpArrow\0\342\206\221", "Sigma\0\316\243",
    "SmallCircle\0\342\210\230", "Sopf\0\360\235\225\212",
    "Sqrt\0\342\210\232", "Square\0\342\226\241",
    "SquareIntersection\0\342\212\223", "SquareSubset\0\342\212\217",
    "SquareSubsetEqual\0\342\212\221", "SquareSuperset\0\342\212\220",
    "SquareSupersetEqual\0\342\212\222", "SquareUnion\0\342\212\224",
    "Sscr\0\360\235\222\256", "Star\0\342\213\206", "Sub\0\342\213\220",
    "Subset\0\342\213\220", "SubsetEqual\0\342\212\206",
    "Succeeds\0\342\211\273", "SucceedsEqual\0\342\252\260",
    "SucceedsSlantEqual\0\342\211\275", "SucceedsTilde\0\342\211\277",
    "SuchThat\0\342\210\213", "Sum\0\342\210\221", "Sup\0\342\213\221",
    "Superset\0\342\212\203", "SupersetEqual\0\342\212\207",
    "Supset\0\342\213\221", "THORN\0\303\236", "TRADE\0\342\204\242",
    "TSHcy\0\320\213", "TScy\0\320\246", "Tab\0\011", "Tau\0\316\244",
    "Tcaron\0\305\244", "Tcedil\0\305\242", "Tcy\0\320\242",
    "Tfr\0\360\235\224\227", "Therefore\0\342\210\264", "Theta\0\316\230",
    "ThickSpace\0\342\201\237\342\200\212", "ThinSpace\0\342\200\211",
    "Tilde\0\342\210\274", "TildeEqual\0\342\211\203",
    "TildeFullEqual\0\342\211\205", "TildeTilde\0\342\211\210",
    "Topf\0\360\235\225\213", "TripleDot\0\342\203\233",
    "Tscr\0\360\235\222\257", "Tstrok\0\305\246", "Uacute\0\303\232",
    "Uarr\0\342\206\237", "Uarrocir\0\342\245\211", "Ubrcy\0\320\216",
    "Ubreve\0\305\254", "Ucirc\0\303\233", "Ucy\0\320\243", "Udblac\0\305\260",
    "Ufr\0\360\235\224\230", "Ugrave\0\303\231", "Umacr\0\305\252",
    "UnderBar\0_", "UnderBrace\0\342\217\237", "UnderBracket\0\342\216\265",
    "UnderParenthesis\0\342\217\235", "Union\0\342\213\203",
    "UnionPlus\0\342\212\216", "Uogon\0\305\262", "Uopf\0\360\235\225\214",
    "UpArrow\0\342\206\221", "UpArrowBar\0\342\244\222",
    "UpArrowDownArrow\0\342\207\205", "UpDownArrow\0\342\206\225",
    "UpEquilibrium\0\342\245\256", "UpTee\0\342\212\245",
    "UpTeeArrow\0\342\206\245", "Uparrow\0\342\207\221",
    "Updownarrow\0\342\207\225", "UpperLeftArrow\0\342\206\226",
    "UpperRightArrow\0\342\206\227", "Upsi\0\317\222", "Upsilon\0\316\245",
    "Uring\0\305\256", "Uscr\0\360\235\222\260", "Utilde\0\305\250",
    "Uuml\0\303\234", "VDash\0\342\212\253", "Vbar\0\342\253\253",
    "Vcy\0\320\222", "Vdash\0\342\212\251", "Vdashl\0\342\253\246",
    "Vee\0\342\213\201", "Verbar\0\342\200\226", "Vert\0\342\200\226",
    "VerticalBar\0\342\210\243", "VerticalLine\0|",
    "VerticalSeparator\0\342\235\230", "VerticalTilde\0\342\211\200",
    "VeryThinSpace\0\342\200\212", "Vfr\0\360\235\224\231",
    "Vopf\0\360\235\225\215", "Vscr\0\360\235\222\261", "Vvdash\0\342\212\252",
    "Wcirc\0\305\264", "Wedge\0\342\213\200", "Wfr\0\360\235\224\232",
    "Wopf\0\360\235\225\216", "Wscr\0\360\235\222\262",
    "Xfr\0\360\235\224\233", "Xi\0\316\236", "Xopf\0\360\235\225\217",
    "Xscr\0\360\235\222\263", "YAcy\0\320\257", "YIcy\0\320\207",
    "YUcy\0\320\256", "Yacute\0\303\235", "Ycirc\0\305\266", "Ycy\0\320\253",
    "Yfr\0\360\235\224\234", "Yopf\0\360\235\225\220",
    "Yscr\0\360\235\222\264", "Yuml\0\305\270", "ZHcy\0\320\226",
    "Zacute\0\305\271", "Zcaron\0\305\275", "Zcy\0\320\227", "Zdot\0\305\273",
    "ZeroWidthSpace\0\342\200\213", "Zeta\0\316\226", "Zfr\0\342\204\250",
    "Zopf\0\342\204\244", "Zscr\0\360\235\222\265", "aacute\0\303\241",
    "abreve\0\304\203", "ac\0\342\210\276", "acE\0\342\210\276\314\263",
    "acd\0\342\210\277", "acirc\0\303\242", "acute\0\302\264", "acy\0\320\260",
    "aelig\0\303\246", "af\0\342\201\241", "afr\0\360\235\224\236",
    "agrave\0\303\240", "alefsym\0\342\204\265", "aleph\0\342\204\265",
    "alpha\0\316\261", "amacr\0\304\201", "amalg\0\342\250\277", "amp\0&",
    "and\0\342\210\247", "andand\0\342\251\225", "andd\0\342\251\234",
    "andslope\0\342\251\230", "andv\0\342\251\232", "ang\0\342\210\240",
    "ange\0\342\246\244", "angle\0\342\210\240", "angmsd\0\342\210\241",
    "angmsdaa\0\342\246\250", "angmsdab\0\342\246\251",
    "angmsdac\0\342\246\252", "angmsdad\0\342\246\253",
    "angmsdae\0\342\246\254", "angmsdaf\0\342\246\255",
    "angmsdag\0\342\246\256", "angmsdah\0\342\246\257", "angrt\0\342\210\237",
    "angrtvb\0\342\212\276", "angrtvbd\0\342\246\235", "angsph\0\342\210\242",
    "angst\0\303\205", "angzarr\0\342\215\274", "aogon\0\304\205",
    "aopf\0\360\235\225\222", "ap\0\342\211\210", "apE\0\342\251\260",
    "apacir\0\342\251\257", "ape\0\342\211\212", "apid\0\342\211\213",
    "apos\0'", "approx\0\342\211\210", "approxeq\0\342\211\212",
    "aring\0\303\245", "ascr\0\360\235\222\266", "ast\0*",
    "asymp\0\342\211\210", "asympeq\0\342\211\215", "atilde\0\303\243",
    "auml\0\303\244", "awconint\0\342\210\263", "awint\0\342\250\221",
    "bNot\0\342\253\255", "backcong\0\342\211\214", "backepsilon\0\317\266",
    "backprime\0\342\200\265", "backsim\0\342\210\275",
    "backsimeq\0\342\213\215", "barvee\0\342\212\275", "barwed\0\342\214\205",
    "barwedge\0\342\214\205", "bbrk\0\342\216\265", "bbrktbrk\0\342\216\266",
    "bcong\0\342\211\214", "bcy\0\320\261", "bdquo\0\342\200\236",
    "becaus\0\342\210\265", "because\0\342\210\265", "bemptyv\0\342\246\260",
    "bepsi\0\317\266", "bernou\0\342\204\254", "beta\0\316\262",
    "beth\0\342\204\266", "between\0\342\211\254", "bfr\0\360\235\224\237",
    "bigcap\0\342\213\202", "bigcirc\0\342\227\257", "bigcup\0\342\213\203",
    "bigodot\0\342\250\200", "bigoplus\0\342\250\201",
    "bigotimes\0\342\250\202", "bigsqcup\0\342\250\206",
    "bigstar\0\342\230\205", "bigtriangledown\0\342\226\275",
    "bigtriangleup\0\342\226\263", "biguplus\0\342\250\204",
    "bigvee\0\342\213\201", "bigwedge\0\342\213\200", "bkarow\0\342\244\215",
    "blacklozenge\0\342\247\253", "blacksquare\0\342\226\252",
    "blacktriangle\0\342\226\264", "blacktriangledown\0\342\226\276",
    "blacktriangleleft\0\342\227\202", "blacktriangleright\0\342\226\270",
    "blank\0\342\220\243", "blk12\0\342\226\222", "blk14\0\342\226\221",
    "blk34\0\342\226\223", "block\0\342\226\210", "bne\0=\342\203\245",
    "bnequiv\0\342\211\241\342\203\245", "bnot\0\342\214\220",
    "bopf\0\360\235\225\223", "bot\0\342\212\245", "bottom\0\342\212\245",
    "bowtie\0\342\213\210", "boxDL\0\342\225\227", "boxDR\0\342\225\224",
    "boxDl\0\342\225\226", "boxDr\0\342\225\223", "boxH\0\342\225\220",
    "boxHD\0\342\225\246", "boxHU\0\342\225\251", "boxHd\0\342\225\244",
    "boxHu\0\342\225\247", "boxUL\0\342\225\235", "boxUR\0\342\225\232",
    "boxUl\0\342\225\234", "boxUr\0\342\225\231", "boxV\0\342\225\221",
    "boxVH\0\342\225\254", "boxVL\0\342\225\243", "boxVR\0\342\225\240",
    "boxVh\0\342\225\253", "boxVl\0\342\225\242", "boxVr\0\342\225\237",
    "boxbox\0\342\247\211", "boxdL\0\342\225\225", "boxdR\0\342\225\222",
    "boxdl\0\342\224\220", "boxdr\0\342\224\214", "boxh\0\342\224\200",
    "boxhD\0\342\225\245", "boxhU\0\342\225\250", "boxhd\0\342\224\254",
    "boxhu\0\342\224\264", "boxminus\0\342\212\237", "boxplus\0\342\212\236",
    "boxtimes\0\342\212\240", "boxuL\0\342\225\233", "boxuR\0\342\225\230",
    "boxul\0\342\224\230", "boxur\0\342\224\224", "boxv\0\342\224\202",
    "boxvH\0\342\225\252", "boxvL\0\342\225\241", "boxvR\0\342\225\236",
    "boxvh\0\342\224\274", "boxvl\0\342\224\244", "boxvr\0\342\224\234",
    "bprime\0\342\200\265", "breve\0\313\230", "brvbar\0\302\246",
    "bscr\0\360\235\222\267", "bsemi\0\342\201\217", "bsim\0\342\210\275",
    "bsime\0\342\213\215", "bsol\0\134", "bsolb\0\342\247\205",
    "bsolhsub\0\342\237\210", "bull\0\342\200\242", "bullet\0\342\200\242",
    "bump\0\342\211\216", "bumpE\0\342\252\256", "bumpe\0\342\211\217",
    "bumpeq\0\342\211\217", "cacute\0\304\207", "cap\0\342\210\251",
    "capand\0\342\251\204", "capbrcup\0\342\251\211", "capcap\0\342\251\213",
    "capcup\0\342\251\207", "capdot\0\342\251\200",
    "caps\0\342\210\251\357\270\200", "caret\0\342\201\201", "caron\0\313\207",
    "ccaps\0\342\251\215", "ccaron\0\304\215", "ccedil\0\303\247",
    "ccirc\0\304\211", "ccups\0\342\251\214", "ccupssm\0\342\251\220",
    "cdot\0\304\213", "cedil\0\302\270", "cemptyv\0\342\246\262",
    "cent\0\302\242", "centerdot\0\302\267", "cfr\0\360\235\224\240",
    "chcy\0\321\207", "check\0\342\234\223", "checkmark\0\342\234\223",
    "chi\0\317\207", "cir\0\342\227\213", "cirE\0\342\247\203",
    "circ\0\313\206", "circeq\0\342\211\227", "circlearrowleft\0\342\206\272",
    "circlearrowright\0\342\206\273", "circledR\0\302\256",
    "circledS\0\342\223\210", "circledast\0\342\212\233",
    "circledcirc\0\342\212\232", "circleddash\0\342\212\235",
    "cire\0\342\211\227", "cirfnint\0\342\250\220", "cirmid\0\342\253\257",
    "cirscir\0\342\247\202", "clubs\0\342\231\243", "clubsuit\0\342\231\243",
    "colon\0:", "colone\0\342\211\224", "coloneq\0\342\211\224", "comma\0,",
    "commat\0@", "comp\0\342\210\201", "compfn\0\342\210\230",
    "complement\0\342\210\201", "complexes\0\342\204\202",
    "cong\0\342\211\205", "congdot\0\342\251\255", "conint\0\342\210\256",
    "copf\0\360\235\225\224", "coprod\0\342\210\220", "copy\0\302\251",
    "copysr\0\342\204\227", "crarr\0\342\206\265", "cross\0\342\234\227",
    "cscr\0\360\235\222\270", "csub\0\342\253\217", "csube\0\342\253\221",
    "csup\0\342\253\220", "csupe\0\342\253\222", "ctdot\0\342\213\257",
    "cudarrl\0\342\244\270", "cudarrr\0\342\244\265", "cuepr\0\342\213\236",
    "cuesc\0\342\213\237", "cularr\0\342\206\266", "cularrp\0\342\244\275",
    "cup\0\342\210\252", "cupbrcap\0\342\251\210", "cupcap\0\342\251\206",
    "cupcup\0\342\251\212", "cupdot\0\342\212\215", "cupor\0\342\251\205",
    "cups\0\342\210\252\357\270\200", "curarr\0\342\206\267",
    "curarrm\0\342\244\274", "curlyeqprec\0\342\213\236",
    "curlyeqsucc\0\342\213\237", "curlyvee\0\342\213\216",
    "curlywedge\0\342\213\217", "curren\0\302\244",
    "curvearrowleft\0\342\206\266", "curvearrowright\0\342\206\267",
    "cuvee\0\342\213\216", "cuwed\0\342\213\217", "cwconint\0\342\210\262",
    "cwint\0\342\210\261", "cylcty\0\342\214\255", "dArr\0\342\207\223",
    "dHar\0\342\245\245", "dagger\0\342\200\240", "daleth\0\342\204\270",
    "darr\0\342\206\223", "dash\0\342\200\220", "dashv\0\342\212\243",
    "dbkarow\0\342\244\217", "dblac\0\313\235", "dcaron\0\304\217",
    "dcy\0\320\264", "dd\0\342\205\206", "ddagger\0\342\200\241",
    "ddarr\0\342\207\212", "ddotseq\0\342\251\267", "deg\0\302\260",
    "delta\0\316\264", "demptyv\0\342\246\261", "dfisht\0\342\245\277",
    "dfr\0\360\235\224\241", "dharl\0\342\207\203", "dharr\0\342\207\202",
    "diam\0\342\213\204", "diamond\0\342\213\204", "diamondsuit\0\342\231\246",
    "diams\0\342\231\246", "die\0\302\250", "digamma\0\317\235",
    "disin\0\342\213\262", "div\0\303\267", "divide\0\303\267",
    "divideontimes\0\342\213\207", "divonx\0\342\213\207", "djcy\0\321\222",
    "dlcorn\0\342\214\236", "dlcrop\0\342\214\215", "dollar\0$",
    "dopf\0\360\235\225\225", "dot\0\313\231", "doteq\0\342\211\220",
    "doteqdot\0\342\211\221", "dotminus\0\342\210\270",
    "dotplus\0\342\210\224", "dotsquare\0\342\212\241",
    "doublebarwedge\0\342\214\206", "downarrow\0\342\206\223",
    "downdownarrows\0\342\207\212", "downharpoonleft\0\342\207\203",
    "downharpoonright\0\342\207\202", "drbkarow\0\342\244\220",
    "drcorn\0\342\214\237", "drcrop\0\342\214\214", "dscr\0\360\235\222\271",
    "dscy\0\321\225", "dsol\0\342\247\266", "dstrok\0\304\221",
    "dtdot\0\342\213\261", "dtri\0\342\226\277", "dtrif\0\342\226\276",
    "duarr\0\342\207\265", "duhar\0\342\245\257", "dwangle\0\342\246\246",
    "dzcy\0\321\237", "dzigrarr\0\342\237\277", "eDDot\0\342\251\267",
    "eDot\0\342\211\221", "eacute\0\303\251", "easter\0\342\251\256",
    "ecaron\0\304\233", "ecir\0\342\211\226", "ecirc\0\303\252",
    "ecolon\0\342\211\225", "ecy\0\321\215", "edot\0\304\227",
    "ee\0\342\205\207", "efDot\0\342\211\222", "efr\0\360\235\224\242",
    "eg\0\342\252\232", "egrave\0\303\250", "egs\0\342\252\226",
    "egsdot\0\342\252\230", "el\0\342\252\231", "elinters\0\342\217\247",
    "ell\0\342\204\223", "els\0\342\252\225", "elsdot\0\342\252\227",
    "emacr\0\304\223", "empty\0\342\210\205", "emptyset\0\342\210\205",
    "emptyv\0\342\210\205", "emsp\0\342\200\203", "emsp13\0\342\200\204",
    "emsp14\0\342\200\205", "eng\0\305\213", "ensp\0\342\200\202",
    "eogon\0\304\231", "eopf\0\360\235\225\226", "epar\0\342\213\225",
    "eparsl\0\342\247\243", "eplus\0\342\251\261", "epsi\0\316\265",
    "epsilon\0\316\265", "epsiv\0\317\265", "eqcirc\0\342\211\226",
    "eqcolon\0\342\211\225", "eqsim\0\342\211\202", "eqslantgtr\0\342\252\226",
    "eqslantless\0\342\252\225", "equals\0=", "equest\0\342\211\237",
    "equiv\0\342\211\241", "equivDD\0\342\251\270", "eqvparsl\0\342\247\245",
    "erDot\0\342\211\223", "erarr\0\342\245\261", "escr\0\342\204\257",
    "esdot\0\342\211\220", "esim\0\342\211\202", "eta\0\316\267",
    "eth\0\303\260", "euml\0\303\253", "euro\0\342\202\254", "excl\0!",
    "exist\0\342\210\203", "expectation\0\342\204\260",
    "exponentiale\0\342\205\207", "fallingdotseq\0\342\211\222",
    "fcy\0\321\204", "female\0\342\231\200", "ffilig\0\357\254\203",
    "fflig\0\357\254\200", "ffllig\0\357\254\204", "ffr\0\360\235\224\243",
    "filig\0\357\254\201", "fjlig\0fj", "flat\0\342\231\255",
    "fllig\0\357\254\202", "fltns\0\342\226\261", "fnof\0\306\222",
    "fopf\0\360\235\225\227", "forall\0\342\210\200", "fork\0\342\213\224",
    "forkv\0\342\253\231", "fpartint\0\342\250\215", "frac12\0\302\275",
    "frac13\0\342\205\223", "frac14\0\302\274", "frac15\0\342\205\225",
    "frac16\0\342\205\231", "frac18\0\342\205\233", "frac23\0\342\205\224",
    "frac25\0\342\205\226", "frac34\0\302\276", "frac35\0\342\205\227",
    "frac38\0\342\205\234", "frac45\0\342\205\230", "frac56\0\342\205\232",
    "frac58\0\342\205\235", "frac78\0\342\205\236", "frasl\0\342\201\204",
    "frown\0\342\214\242", "fscr\0\360\235\222\273", "gE\0\342\211\247",
    "gEl\0\342\252\214", "gacute\0\307\265", "gamma\0\316\263",
    "gammad\0\317\235", "gap\0\342\252\206", "gbreve\0\304\237",
    "gcirc\0\304\235", "gcy\0\320\263", "gdot\0\304\241", "ge\0\342\211\245",
    "gel\0\342\213\233", "geq\0\342\211\245", "geqq\0\342\211\247",
    "geqslant\0\342\251\276", "ges\0\342\251\276", "gescc\0\342\252\251",
    "gesdot\0\342\252\200", "gesdoto\0\342\252\202", "gesdotol\0\342\252\204",
    "gesl\0\342\213\233\357\270\200", "gesles\0\342\252\224",
    "gfr\0\360\235\224\244", "gg\0\342\211\253", "ggg\0\342\213\231",
    "gimel\0\342\204\267", "gjcy\0\321\223", "gl\0\342\211\267",
    "glE\0\342\252\222", "gla\0\342\252\245", "glj\0\342\252\244",
    "gnE\0\342\211\251", "gnap\0\342\252\212", "gnapprox\0\342\252\212",
    "gne\0\342\252\210", "gneq\0\342\252\210", "gneqq\0\342\211\251",
    "gnsim\0\342\213\247", "gopf\0\360\235\225\230", "grave\0`",
    "gscr\0\342\204\212", "gsim\0\342\211\263", "gsime\0\342\252\216",
    "gsiml\0\342\252\220", "gt\0>", "gtcc\0\342\252\247",
    "gtcir\0\342\251\272", "gtdot\0\342\213\227", "gtlPar\0\342\246\225",
    "gtquest\0\342\251\274", "gtrapprox\0\342\252\206", "gtrarr\0\342\245\270",
    "gtrdot\0\342\213\227", "gtreqless\0\342\213\233",
    "gtreqqless\0\342\252\214", "gtrless\0\342\211\267",
    "gtrsim\0\342\211\263", "gvertneqq\0\342\211\251\357\270\200",
    "gvnE\0\342\211\251\357\270\200", "hArr\0\342\207\224",
    "hairsp\0\342\200\212", "half\0\302\275", "hamilt\0\342\204\213",
    "hardcy\0\321\212", "harr\0\342\206\224", "harrcir\0\342\245\210",
    "harrw\0\342\206\255", "hbar\0\342\204\217", "hcirc\0\304\245",
    "hearts\0\342\231\245", "heartsuit\0\342\231\245", "hellip\0\342\200\246",
    "hercon\0\342\212\271", "hfr\0\360\235\224\245", "hksearow\0\342\244\245",
    "hkswarow\0\342\244\246", "hoarr\0\342\207\277", "homtht\0\342\210\273",
    "hookleftarrow\0\342\206\251", "hookrightarrow\0\342\206\252",
    "hopf\0\360\235\225\231", "horbar\0\342\200\225", "hscr\0\360\235\222\275",
    "hslash\0\342\204\217", "hstrok\0\304\247", "hybull\0\342\201\203",
    "hyphen\0\342\200\220", "iacute\0\303\255", "ic\0\342\201\243",
    "icirc\0\303\256", "icy\0\320\270", "iecy\0\320\265", "iexcl\0\302\241",
    "iff\0\342\207\224", "ifr\0\360\235\224\246", "igrave\0\303\254",
    "ii\0\342\205\210", "iiiint\0\342\250\214", "iiint\0\342\210\255",
    "iinfin\0\342\247\234", "iiota\0\342\204\251", "ijlig\0\304\263",
    "imacr\0\304\253", "image\0\342\204\221", "imagline\0\342\204\220",
    "imagpart\0\342\204\221", "imath\0\304\261", "imof\0\342\212\267",
    "imped\0\306\265", "in\0\342\210\210", "incare\0\342\204\205",
    "infin\0\342\210\236", "infintie\0\342\247\235", "inodot\0\304\261",
    "int\0\342\210\253", "intcal\0\342\212\272", "integers\0\342\204\244",
    "intercal\0\342\212\272", "intlarhk\0\342\250\227",
    "intprod\0\342\250\274", "iocy\0\321\221", "iogon\0\304\257",
    "iopf\0\360\235\225\232", "iota\0\316\271", "iprod\0\342\250\274",
    "iquest\0\302\277", "iscr\0\360\235\222\276", "isin\0\342\210\210",
    "isinE\0\342\213\271", "isindot\0\342\213\265", "isins\0\342\213\264",
    "isinsv\0\342\213\263", "isinv\0\342\210\210", "it\0\342\201\242",
    "itilde\0\304\251", "iukcy\0\321\226", "iuml\0\303\257", "jcirc\0\304\265",
    "jcy\0\320\271", "jfr\0\360\235\224\247", "jmath\0\310\267",
    "jopf\0\360\235\225\233", "jscr\0\360\235\222\277", "jsercy\0\321\230",
    "jukcy\0\321\224", "kappa\0\316\272", "kappav\0\317\260",
    "kcedil\0\304\267", "kcy\0\320\272", "kfr\0\360\235\224\250",
    "kgreen\0\304\270", "khcy\0\321\205", "kjcy\0\321\234",
    "kopf\0\360\235\225\234", "kscr\0\360\235\223\200", "lAarr\0\342\207\232",
    "lArr\0\342\207\220", "lAtail\0\342\244\233", "lBarr\0\342\244\216",
    "lE\0\342\211\246", "lEg\0\342\252\213", "lHar\0\342\245\242",
    "lacute\0\304\272", "laemptyv\0\342\246\264", "lagran\0\342\204\222",
    "lambda\0\316\273", "lang\0\342\237\250", "langd\0\342\246\221",
    "langle\0\342\237\250", "lap\0\342\252\205", "laquo\0\302\253",
    "larr\0\342\206\220", "larrb\0\342\207\244", "larrbfs\0\342\244\237",
    "larrfs\0\342\244\235", "larrhk\0\342\206\251", "larrlp\0\342\206\253",
    "larrpl\0\342\244\271", "larrsim\0\342\245\263", "larrtl\0\342\206\242",
    "lat\0\342\252\253", "latail\0\342\244\231", "late\0\342\252\255",
    "lates\0\342\252\255\357\270\200", "lbarr\0\342\244\214",
    "lbbrk\0\342\235\262", "lbrace\0{", "lbrack\0[", "lbrke\0\342\246\213",
    "lbrksld\0\342\246\217", "lbrkslu\0\342\246\215", "lcaron\0\304\276",
    "lcedil\0\304\274", "lceil\0\342\214\210", "lcub\0{", "lcy\0\320\273",
    "ldca\0\342\244\266", "ldquo\0\342\200\234", "ldquor\0\342\200\236",
    "ldrdhar\0\342\245\247", "ldrushar\0\342\245\213", "ldsh\0\342\206\262",
    "le\0\342\211\244", "leftarrow\0\342\206\220",
    "leftarrowtail\0\342\206\242", "leftharpoondown\0\342\206\275",
    "leftharpoonup\0\342\206\274", "leftleftarrows\0\342\207\207",
    "leftrightarrow\0\342\206\224", "leftrightarrows\0\342\207\206",
    "leftrightharpoons\0\342\207\213", "leftrightsquigarrow\0\342\206\255",
    "leftthreetimes\0\342\213\213", "leg\0\342\213\232", "leq\0\342\211\244",
    "leqq\0\342\211\246", "leqslant\0\342\251\275", "les\0\342\251\275",
    "lescc\0\342\252\250", "lesdot\0\342\251\277", "lesdoto\0\342\252\201",
    "lesdotor\0\342\252\203", "lesg\0\342\213\232\357\270\200",
    "lesges\0\342\252\223", "lessapprox\0\342\252\205",
    "lessdot\0\342\213\226", "lesseqgtr\0\342\213\232",
    "lesseqqgtr\0\342\252\213", "lessgtr\0\342\211\266",
    "lesssim\0\342\211\262", "lfisht\0\342\245\274", "lfloor\0\342\214\212",
    "lfr\0\360\235\224\251", "lg\0\342\211\266", "lgE\0\342\252\221",
    "lhard\0\342\206\275", "lharu\0\342\206\274", "lharul\0\342\245\252",
    "lhblk\0\342\226\204", "ljcy\0\321\231", "ll\0\342\211\252",
    "llarr\0\342\207\207", "llcorner\0\342\214\236", "llhard\0\342\245\253",
    "lltri\0\342\227\272", "lmidot\0\305\200", "lmoust\0\342\216\260",
    "lmoustache\0\342\216\260", "lnE\0\342\211\250", "lnap\0\342\252\211",
    "lnapprox\0\342\252\211", "lne\0\342\252\207", "lneq\0\342\252\207",
    "lneqq\0\342\211\250", "lnsim\0\342\213\246", "loang\0\342\237\254",
    "loarr\0\342\207\275", "lobrk\0\342\237\246",
    "longleftarrow\0\342\237\265", "longleftrightarrow\0\342\237\267",
    "longmapsto\0\342\237\274", "longrightarrow\0\342\237\266",
    "looparrowleft\0\342\206\253", "looparrowright\0\342\206\254",
    "lopar\0\342\246\205", "lopf\0\360\235\225\235", "loplus\0\342\250\255",
    "lotimes\0\342\250\264", "lowast\0\342\210\227", "lowbar\0_",
    "loz\0\342\227\212", "lozenge\0\342\227\212", "lozf\0\342\247\253",
    "lpar\0(", "lparlt\0\342\246\223", "lrarr\0\342\207\206",
    "lrcorner\0\342\214\237", "lrhar\0\342\207\213", "lrhard\0\342\245\255",
    "lrm\0\342\200\216", "lrtri\0\342\212\277", "lsaquo\0\342\200\271",
    "lscr\0\360\235\223\201", "lsh\0\342\206\260", "lsim\0\342\211\262",
    "lsime\0\342\252\215", "lsimg\0\342\252\217", "lsqb\0[",
    "lsquo\0\342\200\230", "lsquor\0\342\200\232", "lstrok\0\305\202", "lt\0<",
    "ltcc\0\342\252\246", "ltcir\0\342\251\271", "ltdot\0\342\213\226",
    "lthree\0\342\213\213", "ltimes\0\342\213\211", "ltlarr\0\342\245\266",
    "ltquest\0\342\251\273", "ltrPar\0\342\246\226", "ltri\0\342\227\203",
    "ltrie\0\342\212\264", "ltrif\0\342\227\202", "lurdshar\0\342\245\212",
    "luruhar\0\342\245\246", "lvertneqq\0\342\211\250\357\270\200",
    "lvnE\0\342\211\250\357\270\200", "mDDot\0\342\210\272", "macr\0\302\257",
    "male\0\342\231\202", "malt\0\342\234\240", "maltese\0\342\234\240",
    "map\0\342\206\246", "mapsto\0\342\206\246", "mapstodown\0\342\206\247",
    "mapstoleft\0\342\206\244", "mapstoup\0\342\206\245",
    "marker\0\342\226\256", "mcomma\0\342\250\251", "mcy\0\320\274",
    "mdash\0\342\200\224", "measuredangle\0\342\210\241",
    "mfr\0\360\235\224\252", "mho\0\342\204\247", "micro\0\302\265",
    "mid\0\342\210\243", "midast\0*", "midcir\0\342\253\260",
    "middot\0\302\267", "minus\0\342\210\222", "minusb\0\342\212\237",
    "minusd\0\342\210\270", "minusdu\0\342\250\252", "mlcp\0\342\253\233",
    "mldr\0\342\200\246", "mnplus\0\342\210\223", "models\0\342\212\247",
    "mopf\0\360\235\225\236", "mp\0\342\210\223", "mscr\0\360\235\223\202",
    "mstpos\0\342\210\276", "mu\0\316\274", "multimap\0\342\212\270",
    "mumap\0\342\212\270", "nGg\0\342\213\231\314\270",
    "nGt\0\342\211\253\342\203\222", "nGtv\0\342\211\253\314\270",
    "nLeftarrow\0\342\207\215", "nLeftrightarrow\0\342\207\216",
    "nLl\0\342\213\230\314\270", "nLt\0\342\211\252\342\203\222",
    "nLtv\0\342\211\252\314\270", "nRightarrow\0\342\207\217",
    "nVDash\0\342\212\257", "nVdash\0\342\212\256", "nabla\0\342\210\207",
    "nacute\0\305\204", "nang\0\342\210\240\342\203\222", "nap\0\342\211\211",
    "napE\0\342\251\260\314\270", "napid\0\342\211\213\314\270",
    "napos\0\305\211", "napprox\0\342\211\211", "natur\0\342\231\256",
    "natural\0\342\231\256", "naturals\0\342\204\225", "nbsp\0\302\240",
    "nbump\0\342\211\216\314\270", "nbumpe\0\342\211\217\314\270",
    "ncap\0\342\251\203", "ncaron\0\305\210", "ncedil\0\305\206",
    "ncong\0\342\211\207", "ncongdot\0\342\251\255\314\270",
    "ncup\0\342\251\202", "ncy\0\320\275", "ndash\0\342\200\223",
    "ne\0\342\211\240", "neArr\0\342\207\227", "nearhk\0\342\244\244",
    "nearr\0\342\206\227", "nearrow\0\342\206\227",
    "nedot\0\342\211\220\314\270", "nequiv\0\342\211\242",
    "nesear\0\342\244\250", "nesim\0\342\211\202\314\270",
    "nexist\0\342\210\204", "nexists\0\342\210\204", "nfr\0\360\235\224\253",
    "ngE\0\342\211\247\314\270", "nge\0\342\211\261", "ngeq\0\342\211\261",
    "ngeqq\0\342\211\247\314\270", "ngeqslant\0\342\251\276\314\270",
    "nges\0\342\251\276\314\270", "ngsim\0\342\211\265", "ngt\0\342\211\257",
    "ngtr\0\342\211\257", "nhArr\0\342\207\216", "nharr\0\342\206\256",
    "nhpar\0\342\253\262", "ni\0\342\210\213", "nis\0\342\213\274",
    "nisd\0\342\213\272", "niv\0\342\210\213", "njcy\0\321\232",
    "nlArr\0\342\207\215", "nlE\0\342\211\246\314\270", "nlarr\0\342\206\232",
    "nldr\0\342\200\245", "nle\0\342\211\260", "nleftarrow\0\342\206\232",
    "nleftrightarrow\0\342\206\256", "nleq\0\342\211\260",
    "nleqq\0\342\211\246\314\270", "nleqslant\0\342\251\275\314\270",
    "nles\0\342\251\275\314\270", "nless\0\342\211\256", "nlsim\0\342\211\264",
    "nlt\0\342\211\256", "nltri\0\342\213\252", "nltrie\0\342\213\254",
    "nmid\0\342\210\244", "nopf\0\360\235\225\237", "not\0\302\254",
    "notin\0\342\210\211", "notinE\0\342\213\271\314\270",
    "notindot\0\342\213\265\314\270", "notinva\0\342\210\211",
    "notinvb\0\342\213\267", "notinvc\0\342\213\266", "notni\0\342\210\214",
    "notniva\0\342\210\214", "notnivb\0\342\213\276", "notnivc\0\342\213\275",
    "npar\0\342\210\246", "nparallel\0\342\210\246",
    "nparsl\0\342\253\275\342\203\245", "npart\0\342\210\202\314\270",
    "npolint\0\342\250\224", "npr\0\342\212\200", "nprcue\0\342\213\240",
    "npre\0\342\252\257\314\270", "nprec\0\342\212\200",
    "npreceq\0\342\252\257\314\270", "nrArr\0\342\207\217",
    "nrarr\0\342\206\233", "nrarrc\0\342\244\263\314\270",
    "nrarrw\0\342\206\235\314\270", "nrightarrow\0\342\206\233",
    "nrtri\0\342\213\253", "nrtrie\0\342\213\255", "nsc\0\342\212\201",
    "nsccue\0\342\213\241", "nsce\0\342\252\260\314\270",
    "nscr\0\360\235\223\203", "nshortmid\0\342\210\244",
    "nshortparallel\0\342\210\246", "nsim\0\342\211\201",
    "nsime\0\342\211\204", "nsimeq\0\342\211\204", "nsmid\0\342\210\244",
    "nspar\0\342\210\246", "nsqsube\0\342\213\242", "nsqsupe\0\342\213\243",
    "nsub\0\342\212\204", "nsubE\0\342\253\205\314\270", "nsube\0\342\212\210",
    "nsubset\0\342\212\202\342\203\222", "nsubseteq\0\342\212\210",
    "nsubseteqq\0\342\253\205\314\270", "nsucc\0\342\212\201",
    "nsucceq\0\342\252\260\314\270", "nsup\0\342\212\205",
    "nsupE\0\342\253\206\314\270", "nsupe\0\342\212\211",
    "nsupset\0\342\212\203\342\203\222", "nsupseteq\0\342\212\211",
    "nsupseteqq\0\342\253\206\314\270", "ntgl\0\342\211\271",
    "ntilde\0\303\261", "ntlg\0\342\211\270", "ntriangleleft\0\342\213\252",
    "ntrianglelefteq\0\342\213\254", "ntriangleright\0\342\213\253",
    "ntrianglerighteq\0\342\213\255", "nu\0\316\275", "num\0#",
    "numero\0\342\204\226", "numsp\0\342\200\207", "nvDash\0\342\212\255",
    "nvHarr\0\342\244\204", "nvap\0\342\211\215\342\203\222",
    "nvdash\0\342\212\254", "nvge\0\342\211\245\342\203\222",
    "nvgt\0>\342\203\222", "nvinfin\0\342\247\236", "nvlArr\0\342\244\202",
    "nvle\0\342\211\244\342\203\222", "nvlt\0<\342\203\222",
    "nvltrie\0\342\212\264\342\203\222", "nvrArr\0\342\244\203",
    "nvrtrie\0\342\212\265\342\203\222", "nvsim\0\342\210\274\342\203\222",
    "nwArr\0\342\207\226", "nwarhk\0\342\244\243", "nwarr\0\342\206\226",
    "nwarrow\0\342\206\226", "nwnear\0\342\244\247", "oS\0\342\223\210",
    "oacute\0\303\263", "oast\0\342\212\233", "ocir\0\342\212\232",
    "ocirc\0\303\264", "ocy\0\320\276", "odash\0\342\212\235",
    "odblac\0\305\221", "odiv\0\342\250\270", "odot\0\342\212\231",
    "odsold\0\342\246\274", "oelig\0\305\223", "ofcir\0\342\246\277",
    "ofr\0\360\235\224\254", "ogon\0\313\233", "ograve\0\303\262",
    "ogt\0\342\247\201", "ohbar\0\342\246\265", "ohm\0\316\251",
    "oint\0\342\210\256", "olarr\0\342\206\272", "olcir\0\342\246\276",
    "olcross\0\342\246\273", "oline\0\342\200\276", "olt\0\342\247\200",
    "omacr\0\305\215", "omega\0\317\211", "omicron\0\316\277",
    "omid\0\342\246\266", "ominus\0\342\212\226", "oopf\0\360\235\225\240",
    "opar\0\342\246\267", "operp\0\342\246\271", "oplus\0\342\212\225",
    "or\0\342\210\250", "orarr\0\342\206\273", "ord\0\342\251\235",
    "order\0\342\204\264", "orderof\0\342\204\264", "ordf\0\302\252",
    "ordm\0\302\272", "origof\0\342\212\266", "oror\0\342\251\226",
    "orslope\0\342\251\227", "orv\0\342\251\233", "oscr\0\342\204\264",
    "oslash\0\303\270", "osol\0\342\212\230", "otilde\0\303\265",
    "otimes\0\342\212\227", "otimesas\0\342\250\266", "ouml\0\303\266",
    "ovbar\0\342\214\275", "par\0\342\210\245", "para\0\302\266",
    "parallel\0\342\210\245", "parsim\0\342\253\263", "parsl\0\342\253\275",
    "part\0\342\210\202", "pcy\0\320\277", "percnt\0%", "period\0.",
    "permil\0\342\200\260", "perp\0\342\212\245", "pertenk\0\342\200\261",
    "pfr\0\360\235\224\255", "phi\0\317\206", "phiv\0\317\225",
    "phmmat\0\342\204\263", "phone\0\342\230\216", "pi\0\317\200",
    "pitchfork\0\342\213\224", "piv\0\317\226", "planck\0\342\204\217",
    "planckh\0\342\204\216", "plankv\0\342\204\217", "plus\0+",
    "plusacir\0\342\250\243", "plusb\0\342\212\236", "pluscir\0\342\250\242",
    "plusdo\0\342\210\224", "plusdu\0\342\250\245", "pluse\0\342\251\262",
    "plusmn\0\302\261", "plussim\0\342\250\246", "plustwo\0\342\250\247",
    "pm\0\302\261", "pointint\0\342\250\225", "popf\0\360\235\225\241",
    "pound\0\302\243", "pr\0\342\211\272", "prE\0\342\252\263",
    "prap\0\342\252\267", "prcue\0\342\211\274", "pre\0\342\252\257",
    "prec\0\342\211\272", "precapprox\0\342\252\267",
    "preccurlyeq\0\342\211\274", "preceq\0\342\252\257",
    "precnapprox\0\342\252\271", "precneqq\0\342\252\265",
    "precnsim\0\342\213\250", "precsim\0\342\211\276", "prime\0\342\200\262",
    "primes\0\342\204\231", "prnE\0\342\252\265", "prnap\0\342\252\271",
    "prnsim\0\342\213\250", "prod\0\342\210\217", "profalar\0\342\214\256",
    "profline\0\342\214\222", "profsurf\0\342\214\223", "prop\0\342\210\235",
    "propto\0\342\210\235", "prsim\0\342\211\276", "prurel\0\342\212\260",
    "pscr\0\360\235\223\205", "psi\0\317\210", "puncsp\0\342\200\210",
    "qfr\0\360\235\224\256", "qint\0\342\250\214", "qopf\0\360\235\225\242",
    "qprime\0\342\201\227", "qscr\0\360\235\223\206",
    "quaternions\0\342\204\215", "quatint\0\342\250\226", "quest\0\077",
    "questeq\0\342\211\237", "quot\0\042", "rAarr\0\342\207\233",
    "rArr\0\342\207\222", "rAtail\0\342\244\234", "rBarr\0\342\244\217",
    "rHar\0\342\245\244", "race\0\342\210\275\314\261", "racute\0\305\225",
    "radic\0\342\210\232", "raemptyv\0\342\246\263", "rang\0\342\237\251",
    "rangd\0\342\246\222", "range\0\342\246\245", "rangle\0\342\237\251",
    "raquo\0\302\273", "rarr\0\342\206\222", "rarrap\0\342\245\265",
    "rarrb\0\342\207\245", "rarrbfs\0\342\244\240", "rarrc\0\342\244\263",
    "rarrfs\0\342\244\236", "rarrhk\0\342\206\252", "rarrlp\0\342\206\254",
    "rarrpl\0\342\245\205", "rarrsim\0\342\245\264", "rarrtl\0\342\206\243",
    "rarrw\0\342\206\235", "ratail\0\342\244\232", "ratio\0\342\210\266",
    "rationals\0\342\204\232", "rbarr\0\342\244\215", "rbbrk\0\342\235\263",
    "rbrace\0}", "rbrack\0]", "rbrke\0\342\246\214", "rbrksld\0\342\246\216",
    "rbrkslu\0\342\246\220", "rcaron\0\305\231", "rcedil\0\305\227",
    "rceil\0\342\214\211", "rcub\0}", "rcy\0\321\200", "rdca\0\342\244\267",
    "rdldhar\0\342\245\251", "rdquo\0\342\200\235", "rdquor\0\342\200\235",
    "rdsh\0\342\206\263", "real\0\342\204\234", "realine\0\342\204\233",
    "realpart\0\342\204\234", "reals\0\342\204\235", "rect\0\342\226\255",
    "reg\0\302\256", "rfisht\0\342\245\275", "rfloor\0\342\214\213",
    "rfr\0\360\235\224\257", "rhard\0\342\207\201", "rharu\0\342\207\200",
    "rharul\0\342\245\254", "rho\0\317\201", "rhov\0\317\261",
    "rightarrow\0\342\206\222", "rightarrowtail\0\342\206\243",
    "rightharpoondown\0\342\207\201", "rightharpoonup\0\342\207\200",
    "rightleftarrows\0\342\207\204", "rightleftharpoons\0\342\207\214",
    "rightrightarrows\0\342\207\211", "rightsquigarrow\0\342\206\235",
    "rightthreetimes\0\342\213\214", "ring\0\313\232",
    "risingdotseq\0\342\211\223", "rlarr\0\342\207\204", "rlhar\0\342\207\214",
    "rlm\0\342\200\217", "rmoust\0\342\216\261", "rmoustache\0\342\216\261",
    "rnmid\0\342\253\256", "roang\0\342\237\255", "roarr\0\342\207\276",
    "robrk\0\342\237\247", "ropar\0\342\246\206", "ropf\0\360\235\225\243",
    "roplus\0\342\250\256", "rotimes\0\342\250\265", "rpar\0)",
    "rpargt\0\342\246\224", "rppolint\0\342\250\222", "rrarr\0\342\207\211",
    "rsaquo\0\342\200\272", "rscr\0\360\235\223\207", "rsh\0\342\206\261",
    "rsqb\0]", "rsquo\0\342\200\231", "rsquor\0\342\200\231",
    "rthree\0\342\213\214", "rtimes\0\342\213\212", "rtri\0\342\226\271",
    "rtrie\0\342\212\265", "rtrif\0\342\226\270", "rtriltri\0\342\247\216",
    "ruluhar\0\342\245\250", "rx\0\342\204\236", "sacute\0\305\233",
    "sbquo\0\342\200\232", "sc\0\342\211\273", "scE\0\342\252\264",
    "scap\0\342\252\270", "scaron\0\305\241", "sccue\0\342\211\275",
    "sce\0\342\252\260", "scedil\0\305\237", "scirc\0\305\235",
    "scnE\0\342\252\266", "scnap\0\342\252\272", "scnsim\0\342\213\251",
    "scpolint\0\342\250\223", "scsim\0\342\211\277", "scy\0\321\201",
    "sdot\0\342\213\205", "sdotb\0\342\212\241", "sdote\0\342\251\246",
    "seArr\0\342\207\230", "searhk\0\342\244\245", "searr\0\342\206\230",
    "searrow\0\342\206\230", "sect\0\302\247", "semi\0;",
    "seswar\0\342\244\251", "setminus\0\342\210\226", "setmn\0\342\210\226",
    "sext\0\342\234\266", "sfr\0\360\235\224\260", "sfrown\0\342\214\242",
    "sharp\0\342\231\257", "shchcy\0\321\211", "shcy\0\321\210",
    "shortmid\0\342\210\243", "shortparallel\0\342\210\245", "shy\0\302\255",
    "sigma\0\317\203", "sigmaf\0\317\202", "sigmav\0\317\202",
    "sim\0\342\210\274", "simdot\0\342\251\252", "sime\0\342\211\203",
    "simeq\0\342\211\203", "simg\0\342\252\236", "simgE\0\342\252\240",
    "siml\0\342\252\235", "simlE\0\342\252\237", "simne\0\342\211\206",
    "simplus\0\342\250\244", "simrarr\0\342\245\262", "slarr\0\342\206\220",
    "smallsetminus\0\342\210\226", "smashp\0\342\250\263",
    "smeparsl\0\342\247\244", "smid\0\342\210\243", "smile\0\342\214\243",
    "smt\0\342\252\252", "smte\0\342\252\254",
    "smtes\0\342\252\254\357\270\200", "softcy\0\321\214", "sol\0/",
    "solb\0\342\247\204", "solbar\0\342\214\277", "sopf\0\360\235\225\244",
    "spades\0\342\231\240", "spadesuit\0\342\231\240", "spar\0\342\210\245",
    "sqcap\0\342\212\223", "sqcaps\0\342\212\223\357\270\200",
    "sqcup\0\342\212\224", "sqcups\0\342\212\224\357\270\200",
    "sqsub\0\342\212\217", "sqsube\0\342\212\221", "sqsubset\0\342\212\217",
    "sqsubseteq\0\342\212\221", "sqsup\0\342\212\220", "sqsupe\0\342\212\222",
    "sqsupset\0\342\212\220", "sqsupseteq\0\342\212\222", "squ\0\342\226\241",
    "square\0\342\226\241", "squarf\0\342\226\252", "squf\0\342\226\252",
    "srarr\0\342\206\222", "sscr\0\360\235\223\210", "ssetmn\0\342\210\226",
    "ssmile\0\342\214\243", "sstarf\0\342\213\206", "star\0\342\230\206",
    "starf\0\342\230\205", "straightepsilon\0\317\265",
    "straightphi\0\317\225", "strns\0\302\257", "sub\0\342\212\202",
    "subE\0\342\253\205", "subdot\0\342\252\275", "sube\0\342\212\206",
    "subedot\0\342\253\203", "submult\0\342\253\201", "subnE\0\342\253\213",
    "subne\0\342\212\212", "subplus\0\342\252\277", "subrarr\0\342\245\271",
    "subset\0\342\212\202", "subseteq\0\342\212\206",
    "subseteqq\0\342\253\205", "subsetneq\0\342\212\212",
    "subsetneqq\0\342\253\213", "subsim\0\342\253\207", "subsub\0\342\253\225",
    "subsup\0\342\253\223", "succ\0\342\211\273", "succapprox\0\342\252\270",
    "succcurlyeq\0\342\211\275", "succeq\0\342\252\260",
    "succnapprox\0\342\252\272", "succneqq\0\342\252\266",
    "succnsim\0\342\213\251", "succsim\0\342\211\277", "sum\0\342\210\221",
    "sung\0\342\231\252", "sup\0\342\212\203", "sup1\0\302\271",
    "sup2\0\302\262", "sup3\0\302\263", "supE\0\342\253\206",
    "supdot\0\342\252\276", "supdsub\0\342\253\230", "supe\0\342\212\207",
    "supedot\0\342\253\204", "suphsol\0\342\237\211", "suphsub\0\342\253\227",
    "suplarr\0\342\245\273", "supmult\0\342\253\202", "supnE\0\342\253\214",
    "supne\0\342\212\213", "supplus\0\342\253\200", "supset\0\342\212\203",
    "supseteq\0\342\212\207", "supseteqq\0\342\253\206",
    "supsetneq\0\342\212\213", "supsetneqq\0\342\253\214",
    "supsim\0\342\253\210", "supsub\0\342\253\224", "supsup\0\342\253\226",
    "swArr\0\342\207\231", "swarhk\0\342\244\246", "swarr\0\342\206\231",
    "swarrow\0\342\206\231", "swnwar\0\342\244\252", "szlig\0\303\237",
    "target\0\342\214\226", "tau\0\317\204", "tbrk\0\342\216\264",
    "tcaron\0\305\245", "tcedil\0\305\243", "tcy\0\321\202",
    "tdot\0\342\203\233", "telrec\0\342\214\225", "tfr\0\360\235\224\261",
    "there4\0\342\210\264", "therefore\0\342\210\264", "theta\0\316\270",
    "thetasym\0\317\221", "thetav\0\317\221", "thickapprox\0\342\211\210",
    "thicksim\0\342\210\274", "thinsp\0\342\200\211", "thkap\0\342\211\210",
    "thksim\0\342\210\274", "thorn\0\303\276", "tilde\0\313\234",
    "times\0\303\227", "timesb\0\342\212\240", "timesbar\0\342\250\261",
    "timesd\0\342\250\260", "tint\0\342\210\255", "toea\0\342\244\250",
    "top\0\342\212\244", "topbot\0\342\214\266", "topcir\0\342\253\261",
    "topf\0\360\235\225\245", "topfork\0\342\253\232", "tosa\0\342\244\251",
    "tprime\0\342\200\264", "trade\0\342\204\242", "triangle\0\342\226\265",
    "triangledown\0\342\226\277", "triangleleft\0\342\227\203",
    "trianglelefteq\0\342\212\264", "triangleq\0\342\211\234",
    "triangleright\0\342\226\271", "trianglerighteq\0\342\212\265",
    "tridot\0\342\227\254", "trie\0\342\211\234", "triminus\0\342\250\272",
    "triplus\0\342\250\271", "trisb\0\342\247\215", "tritime\0\342\250\273",
    "trpezium\0\342\217\242", "tscr\0\360\235\223\211", "tscy\0\321\206",
    "tshcy\0\321\233", "tstrok\0\305\247", "twixt\0\342\211\254",
    "twoheadleftarrow\0\342\206\236", "twoheadrightarrow\0\342\206\240",
    "uArr\0\342\207\221", "uHar\0\342\245\243", "uacute\0\303\272",
    "uarr\0\342\206\221", "ubrcy\0\321\236", "ubreve\0\305\255",
    "ucirc\0\303\273", "ucy\0\321\203", "udarr\0\342\207\205",
    "udblac\0\305\261", "udhar\0\342\245\256", "ufisht\0\342\245\276",
    "ufr\0\360\235\224\262", "ugrave\0\303\271", "uharl\0\342\206\277",
    "uharr\0\342\206\276", "uhblk\0\342\226\200", "ulcorn\0\342\214\234",
    "ulcorner\0\342\214\234", "ulcrop\0\342\214\217", "ultri\0\342\227\270",
    "umacr\0\305\253", "uml\0\302\250", "uogon\0\305\263",
    "uopf\0\360\235\225\246", "uparrow\0\342\206\221",
    "updownarrow\0\342\206\225", "upharpoonleft\0\342\206\277",
    "upharpoonright\0\342\206\276", "uplus\0\342\212\216", "upsi\0\317\205",
    "upsih\0\317\222", "upsilon\0\317\205", "upuparrows\0\342\207\210",
    "urcorn\0\342\214\235", "urcorner\0\342\214\235", "urcrop\0\342\214\216",
    "uring\0\305\257", "urtri\0\342\227\271", "uscr\0\360\235\223\212",
    "utdot\0\342\213\260", "utilde\0\305\251", "utri\0\342\226\265",
    "utrif\0\342\226\264", "uuarr\0\342\207\210", "uuml\0\303\274",
    "uwangle\0\342\246\247", "vArr\0\342\207\225", "vBar\0\342\253\250",
    "vBarv\0\342\253\251", "vDash\0\342\212\250", "vangrt\0\342\246\234",
    "varepsilon\0\317\265", "varkappa\0\317\260", "varnothing\0\342\210\205",
    "varphi\0\317\225", "varpi\0\317\226", "varpropto\0\342\210\235",
    "varr\0\342\206\225", "varrho\0\317\261", "varsigma\0\317\202",
    "varsubsetneq\0\342\212\212\357\270\200",
    "varsubsetneqq\0\342\253\213\357\270\200",
    "varsupsetneq\0\342\212\213\357\270\200",
    "varsupsetneqq\0\342\253\214\357\270\200", "vartheta\0\317\221",
    "vartriangleleft\0\342\212\262", "vartriangleright\0\342\212\263",
    "vcy\0\320\262", "vdash\0\342\212\242", "vee\0\342\210\250",
    "veebar\0\342\212\273", "veeeq\0\342\211\232", "vellip\0\342\213\256",
    "verbar\0|", "vert\0|", "vfr\0\360\235\224\263", "vltri\0\342\212\262",
    "vnsub\0\342\212\202\342\203\222", "vnsup\0\342\212\203\342\203\222",
    "vopf\0\360\235\225\247", "vprop\0\342\210\235", "vrtri\0\342\212\263",
    "vscr\0\360\235\223\213", "vsubnE\0\342\253\213\357\270\200",
    "vsubne\0\342\212\212\357\270\200", "vsupnE\0\342\253\214\357\270\200",
    "vsupne\0\342\212\213\357\270\200", "vzigzag\0\342\246\232",
    "wcirc\0\305\265", "wedbar\0\342\251\237", "wedge\0\342\210\247",
    "wedgeq\0\342\211\231", "weierp\0\342\204\230", "wfr\0\360\235\224\264",
    "wopf\0\360\235\225\250", "wp\0\342\204\230", "wr\0\342\211\200",
    "wreath\0\342\211\200", "wscr\0\360\235\223\214", "xcap\0\342\213\202",
    "xcirc\0\342\227\257", "xcup\0\342\213\203", "xdtri\0\342\226\275",
    "xfr\0\360\235\224\265", "xhArr\0\342\237\272", "xharr\0\342\237\267",
    "xi\0\316\276", "xlArr\0\342\237\270", "xlarr\0\342\237\265",
    "xmap\0\342\237\274", "xnis\0\342\213\273", "xodot\0\342\250\200",
    "xopf\0\360\235\225\251", "xoplus\0\342\250\201", "xotime\0\342\250\202",
    "xrArr\0\342\237\271", "xrarr\0\342\237\266", "xscr\0\360\235\223\215",
    "xsqcup\0\342\250\206", "xuplus\0\342\250\204", "xutri\0\342\226\263",
    "xvee\0\342\213\201", "xwedge\0\342\213\200", "yacute\0\303\275",
    "yacy\0\321\217", "ycirc\0\305\267", "ycy\0\321\213", "yen\0\302\245",
    "yfr\0\360\235\224\266", "yicy\0\321\227", "yopf\0\360\235\225\252",
    "yscr\0\360\235\223\216", "yucy\0\321\216", "yuml\0\303\277",
    "zacute\0\305\272", "zcaron\0\305\276", "zcy\0\320\267", "zdot\0\305\274",
    "zeetrf\0\342\204\250", "zeta\0\316\266", "zfr\0\360\235\224\267",
    "zhcy\0\320\266", "zigrarr\0\342\207\235", "zopf\0\360\235\225\253",
    "zscr\0\360\235\223\217", "zwj\0\342\200\215", "zwnj\0\342\200\214",};

#define N_ENTITIES (sizeof ENTITIES / sizeof *ENTITIES)
#define MAX_ENTITY_NAME 32

static const char *entity_lookup(const char *name, size_t len) {
	size_t lo = 0, hi = N_ENTITIES;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const char *e = ENTITIES[mid];
		size_t elen = strlen(e);
		int cmp;
		size_t m = elen < len ? elen : len;
		cmp = memcmp(e, name, m);
		if (cmp == 0) cmp = elen < len ? -1 : (elen > len ? 1 : 0);
		if (cmp == 0) return e + elen + 1;
		if (cmp < 0) lo = mid + 1;
		else hi = mid;
	}
	return NULL;
}

/* `&name;`, `&#DDDDDDD;` or `&#xHHHHHH;`.  Returns the number of bytes
 * consumed and appends the decoded text to `out`, or 0 if this is not a
 * character reference and the `&` is literal. */
static size_t decode_entity(Buf *out, const char *s, size_t n, size_t i) {
	size_t j = i + 1, start;
	if (j >= n) return 0;
	if (s[j] == '#') {
		int32_t cp = 0;
		int digits = 0;
		j++;
		if (j < n && (s[j] == 'x' || s[j] == 'X')) {
			j++;
			for (; j < n && digits < 7; j++, digits++) {
				int c = (unsigned char)s[j];
				if (c >= '0' && c <= '9') cp = cp * 16 + (c - '0');
				else if (c >= 'a' && c <= 'f') cp = cp * 16 + (c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') cp = cp * 16 + (c - 'A' + 10);
				else break;
			}
			if (digits < 1 || digits > 6) return 0;
		} else {
			for (; j < n && digits < 8; j++, digits++) {
				int c = (unsigned char)s[j];
				if (c < '0' || c > '9') break;
				cp = cp * 10 + (c - '0');
			}
			if (digits < 1 || digits > 7) return 0;
		}
		if (j >= n || s[j] != ';') return 0;
		/* U+0000 is not representable and a value out of range is not a
		 * codepoint: both become the replacement character. */
		if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
		utf8_encode(out, cp);
		return j + 1 - i;
	}
	start = j;
	while (j < n && c_isalnum((unsigned char)s[j]) &&
	       j - start < MAX_ENTITY_NAME)
		j++;
	if (j >= n || s[j] != ';' || j == start) return 0;
	{
		const char *val = entity_lookup(s + start, j - start);
		if (!val) return 0;
		bputs(out, val);
	}
	return j + 1 - i;
}

/* Unicode punctuation, as CommonMark 0.31 defines it: general categories Pc,
 * Pd, Pe, Pf, Pi, Po, Ps, Sc, Sk, Sm and So.  (0.31 widened this to include
 * the symbol categories, so a parser written against 0.30 gets `$` and `+`
 * wrong here.)  339 ranges, generated from CPython's unicodedata, Unicode
 * 15.1.0 -- the spec is written against 15.0 and the difference does not
 * touch any example in the suite.  Binary search. */
static const int32_t UNI_PUNCT[][2] = {
    {0x21,0x2F}, {0x3A,0x40}, {0x5B,0x60}, {0x7B,0x7E}, {0xA1,0xA9},
    {0xAB,0xAC}, {0xAE,0xB1}, {0xB4,0xB4}, {0xB6,0xB8}, {0xBB,0xBB},
    {0xBF,0xBF}, {0xD7,0xD7}, {0xF7,0xF7}, {0x2C2,0x2C5}, {0x2D2,0x2DF},
    {0x2E5,0x2EB}, {0x2ED,0x2ED}, {0x2EF,0x2FF}, {0x375,0x375}, {0x37E,0x37E},
    {0x384,0x385}, {0x387,0x387}, {0x3F6,0x3F6}, {0x482,0x482}, {0x55A,0x55F},
    {0x589,0x58A}, {0x58D,0x58F}, {0x5BE,0x5BE}, {0x5C0,0x5C0}, {0x5C3,0x5C3},
    {0x5C6,0x5C6}, {0x5F3,0x5F4}, {0x606,0x60F}, {0x61B,0x61B}, {0x61D,0x61F},
    {0x66A,0x66D}, {0x6D4,0x6D4}, {0x6DE,0x6DE}, {0x6E9,0x6E9}, {0x6FD,0x6FE},
    {0x700,0x70D}, {0x7F6,0x7F9}, {0x7FE,0x7FF}, {0x830,0x83E}, {0x85E,0x85E},
    {0x888,0x888}, {0x964,0x965}, {0x970,0x970}, {0x9F2,0x9F3}, {0x9FA,0x9FB},
    {0x9FD,0x9FD}, {0xA76,0xA76}, {0xAF0,0xAF1}, {0xB70,0xB70}, {0xBF3,0xBFA},
    {0xC77,0xC77}, {0xC7F,0xC7F}, {0xC84,0xC84}, {0xD4F,0xD4F}, {0xD79,0xD79},
    {0xDF4,0xDF4}, {0xE3F,0xE3F}, {0xE4F,0xE4F}, {0xE5A,0xE5B}, {0xF01,0xF17},
    {0xF1A,0xF1F}, {0xF34,0xF34}, {0xF36,0xF36}, {0xF38,0xF38}, {0xF3A,0xF3D},
    {0xF85,0xF85}, {0xFBE,0xFC5}, {0xFC7,0xFCC}, {0xFCE,0xFDA},
    {0x104A,0x104F}, {0x109E,0x109F}, {0x10FB,0x10FB}, {0x1360,0x1368},
    {0x1390,0x1399}, {0x1400,0x1400}, {0x166D,0x166E}, {0x169B,0x169C},
    {0x16EB,0x16ED}, {0x1735,0x1736}, {0x17D4,0x17D6}, {0x17D8,0x17DB},
    {0x1800,0x180A}, {0x1940,0x1940}, {0x1944,0x1945}, {0x19DE,0x19FF},
    {0x1A1E,0x1A1F}, {0x1AA0,0x1AA6}, {0x1AA8,0x1AAD}, {0x1B5A,0x1B6A},
    {0x1B74,0x1B7E}, {0x1BFC,0x1BFF}, {0x1C3B,0x1C3F}, {0x1C7E,0x1C7F},
    {0x1CC0,0x1CC7}, {0x1CD3,0x1CD3}, {0x1FBD,0x1FBD}, {0x1FBF,0x1FC1},
    {0x1FCD,0x1FCF}, {0x1FDD,0x1FDF}, {0x1FED,0x1FEF}, {0x1FFD,0x1FFE},
    {0x2010,0x2027}, {0x2030,0x205E}, {0x207A,0x207E}, {0x208A,0x208E},
    {0x20A0,0x20C0}, {0x2100,0x2101}, {0x2103,0x2106}, {0x2108,0x2109},
    {0x2114,0x2114}, {0x2116,0x2118}, {0x211E,0x2123}, {0x2125,0x2125},
    {0x2127,0x2127}, {0x2129,0x2129}, {0x212E,0x212E}, {0x213A,0x213B},
    {0x2140,0x2144}, {0x214A,0x214D}, {0x214F,0x214F}, {0x218A,0x218B},
    {0x2190,0x2426}, {0x2440,0x244A}, {0x249C,0x24E9}, {0x2500,0x2775},
    {0x2794,0x2B73}, {0x2B76,0x2B95}, {0x2B97,0x2BFF}, {0x2CE5,0x2CEA},
    {0x2CF9,0x2CFC}, {0x2CFE,0x2CFF}, {0x2D70,0x2D70}, {0x2E00,0x2E2E},
    {0x2E30,0x2E5D}, {0x2E80,0x2E99}, {0x2E9B,0x2EF3}, {0x2F00,0x2FD5},
    {0x2FF0,0x2FFF}, {0x3001,0x3004}, {0x3008,0x3020}, {0x3030,0x3030},
    {0x3036,0x3037}, {0x303D,0x303F}, {0x309B,0x309C}, {0x30A0,0x30A0},
    {0x30FB,0x30FB}, {0x3190,0x3191}, {0x3196,0x319F}, {0x31C0,0x31E3},
    {0x31EF,0x31EF}, {0x3200,0x321E}, {0x322A,0x3247}, {0x3250,0x3250},
    {0x3260,0x327F}, {0x328A,0x32B0}, {0x32C0,0x33FF}, {0x4DC0,0x4DFF},
    {0xA490,0xA4C6}, {0xA4FE,0xA4FF}, {0xA60D,0xA60F}, {0xA673,0xA673},
    {0xA67E,0xA67E}, {0xA6F2,0xA6F7}, {0xA700,0xA716}, {0xA720,0xA721},
    {0xA789,0xA78A}, {0xA828,0xA82B}, {0xA836,0xA839}, {0xA874,0xA877},
    {0xA8CE,0xA8CF}, {0xA8F8,0xA8FA}, {0xA8FC,0xA8FC}, {0xA92E,0xA92F},
    {0xA95F,0xA95F}, {0xA9C1,0xA9CD}, {0xA9DE,0xA9DF}, {0xAA5C,0xAA5F},
    {0xAA77,0xAA79}, {0xAADE,0xAADF}, {0xAAF0,0xAAF1}, {0xAB5B,0xAB5B},
    {0xAB6A,0xAB6B}, {0xABEB,0xABEB}, {0xFB29,0xFB29}, {0xFBB2,0xFBC2},
    {0xFD3E,0xFD4F}, {0xFDCF,0xFDCF}, {0xFDFC,0xFDFF}, {0xFE10,0xFE19},
    {0xFE30,0xFE52}, {0xFE54,0xFE66}, {0xFE68,0xFE6B}, {0xFF01,0xFF0F},
    {0xFF1A,0xFF20}, {0xFF3B,0xFF40}, {0xFF5B,0xFF65}, {0xFFE0,0xFFE6},
    {0xFFE8,0xFFEE}, {0xFFFC,0xFFFD}, {0x10100,0x10102}, {0x10137,0x1013F},
    {0x10179,0x10189}, {0x1018C,0x1018E}, {0x10190,0x1019C},
    {0x101A0,0x101A0}, {0x101D0,0x101FC}, {0x1039F,0x1039F},
    {0x103D0,0x103D0}, {0x1056F,0x1056F}, {0x10857,0x10857},
    {0x10877,0x10878}, {0x1091F,0x1091F}, {0x1093F,0x1093F},
    {0x10A50,0x10A58}, {0x10A7F,0x10A7F}, {0x10AC8,0x10AC8},
    {0x10AF0,0x10AF6}, {0x10B39,0x10B3F}, {0x10B99,0x10B9C},
    {0x10EAD,0x10EAD}, {0x10F55,0x10F59}, {0x10F86,0x10F89},
    {0x11047,0x1104D}, {0x110BB,0x110BC}, {0x110BE,0x110C1},
    {0x11140,0x11143}, {0x11174,0x11175}, {0x111C5,0x111C8},
    {0x111CD,0x111CD}, {0x111DB,0x111DB}, {0x111DD,0x111DF},
    {0x11238,0x1123D}, {0x112A9,0x112A9}, {0x1144B,0x1144F},
    {0x1145A,0x1145B}, {0x1145D,0x1145D}, {0x114C6,0x114C6},
    {0x115C1,0x115D7}, {0x11641,0x11643}, {0x11660,0x1166C},
    {0x116B9,0x116B9}, {0x1173C,0x1173F}, {0x1183B,0x1183B},
    {0x11944,0x11946}, {0x119E2,0x119E2}, {0x11A3F,0x11A46},
    {0x11A9A,0x11A9C}, {0x11A9E,0x11AA2}, {0x11B00,0x11B09},
    {0x11C41,0x11C45}, {0x11C70,0x11C71}, {0x11EF7,0x11EF8},
    {0x11F43,0x11F4F}, {0x11FD5,0x11FF1}, {0x11FFF,0x11FFF},
    {0x12470,0x12474}, {0x12FF1,0x12FF2}, {0x16A6E,0x16A6F},
    {0x16AF5,0x16AF5}, {0x16B37,0x16B3F}, {0x16B44,0x16B45},
    {0x16E97,0x16E9A}, {0x16FE2,0x16FE2}, {0x1BC9C,0x1BC9C},
    {0x1BC9F,0x1BC9F}, {0x1CF50,0x1CFC3}, {0x1D000,0x1D0F5},
    {0x1D100,0x1D126}, {0x1D129,0x1D164}, {0x1D16A,0x1D16C},
    {0x1D183,0x1D184}, {0x1D18C,0x1D1A9}, {0x1D1AE,0x1D1EA},
    {0x1D200,0x1D241}, {0x1D245,0x1D245}, {0x1D300,0x1D356},
    {0x1D6C1,0x1D6C1}, {0x1D6DB,0x1D6DB}, {0x1D6FB,0x1D6FB},
    {0x1D715,0x1D715}, {0x1D735,0x1D735}, {0x1D74F,0x1D74F},
    {0x1D76F,0x1D76F}, {0x1D789,0x1D789}, {0x1D7A9,0x1D7A9},
    {0x1D7C3,0x1D7C3}, {0x1D800,0x1D9FF}, {0x1DA37,0x1DA3A},
    {0x1DA6D,0x1DA74}, {0x1DA76,0x1DA83}, {0x1DA85,0x1DA8B},
    {0x1E14F,0x1E14F}, {0x1E2FF,0x1E2FF}, {0x1E95E,0x1E95F},
    {0x1ECAC,0x1ECAC}, {0x1ECB0,0x1ECB0}, {0x1ED2E,0x1ED2E},
    {0x1EEF0,0x1EEF1}, {0x1F000,0x1F02B}, {0x1F030,0x1F093},
    {0x1F0A0,0x1F0AE}, {0x1F0B1,0x1F0BF}, {0x1F0C1,0x1F0CF},
    {0x1F0D1,0x1F0F5}, {0x1F10D,0x1F1AD}, {0x1F1E6,0x1F202},
    {0x1F210,0x1F23B}, {0x1F240,0x1F248}, {0x1F250,0x1F251},
    {0x1F260,0x1F265}, {0x1F300,0x1F6D7}, {0x1F6DC,0x1F6EC},
    {0x1F6F0,0x1F6FC}, {0x1F700,0x1F776}, {0x1F77B,0x1F7D9},
    {0x1F7E0,0x1F7EB}, {0x1F7F0,0x1F7F0}, {0x1F800,0x1F80B},
    {0x1F810,0x1F847}, {0x1F850,0x1F859}, {0x1F860,0x1F887},
    {0x1F890,0x1F8AD}, {0x1F8B0,0x1F8B1}, {0x1F900,0x1FA53},
    {0x1FA60,0x1FA6D}, {0x1FA70,0x1FA7C}, {0x1FA80,0x1FA88},
    {0x1FA90,0x1FABD}, {0x1FABF,0x1FAC5}, {0x1FACE,0x1FADB},
    {0x1FAE0,0x1FAE8}, {0x1FAF0,0x1FAF8}, {0x1FB00,0x1FB92}, {0x1FB94,0x1FBCA}};

#define N_UNI_PUNCT (sizeof UNI_PUNCT / sizeof *UNI_PUNCT)

static int uni_punct(int32_t c) {
	size_t lo = 0, hi = N_UNI_PUNCT;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (c < UNI_PUNCT[mid][0]) hi = mid;
		else if (c > UNI_PUNCT[mid][1]) lo = mid + 1;
		else return 1;
	}
	return 0;
}

/* Unicode whitespace: the Zs category plus tab, newline, form feed and
 * carriage return. */
static int uni_space(int32_t c) {
	return c == 9 || c == 10 || c == 12 || c == 13 || c == 32 || c == 0xA0 ||
	       c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F ||
	       c == 0x205F || c == 0x3000;
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

static void free_one(Node *n) {
	bfree(&n->content);
	bfree(&n->info);
	bfree(&n->url);
	bfree(&n->title);
	free(n);
}

/* Iterative, not recursive.  A document can legitimately build an inline tree
 * a few hundred deep and a hostile one can build it half a million deep, and
 * a recursive free is a stack overflow in the teardown path -- the one place
 * where there is nothing left to check a depth against.  Each step either
 * detaches the first child and descends, or frees a leaf and climbs the
 * parent pointer, so it is O(nodes) with O(1) stack. */
static void node_free(Node *root) {
	Node *cur = root;
	if (!root) return;
	root->parent = NULL;
	while (cur) {
		if (cur->first) {
			Node *child = cur->first;
			cur->first = child->next;
			if (child->next) child->next->prev = NULL;
			else cur->last = NULL;
			child->next = child->prev = NULL;
			cur = child;
		} else {
			Node *up = cur->parent;
			free_one(cur);
			cur = up;
		}
	}
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

static void node_insert_after(Node *at, Node *n) {
	n->parent = at->parent;
	n->prev = at;
	n->next = at->next;
	if (at->next) at->next->prev = n;
	else if (at->parent) at->parent->last = n;
	at->next = n;
}

static void node_insert_before(Node *at, Node *n) {
	n->parent = at->parent;
	n->next = at;
	n->prev = at->prev;
	if (at->prev) at->prev->next = n;
	else if (at->parent) at->parent->first = n;
	at->prev = n;
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

/* --------------------------------------------------------- reference map */

/* Link reference definitions.  Labels are normalised: outer whitespace
 * trimmed, interior whitespace runs collapsed to one space, case folded.
 * The first definition of a label wins.
 * shortcut: linear lookup.  Fine for documents; a sorted array or a hash
 * would matter only for a document with thousands of definitions. */
typedef struct {
	Buf label, url, title;
} Ref;

typedef struct {
	Ref *v;
	size_t n, cap;
} RefMap;

static void normalize_label(Buf *out, const char *s, size_t n) {
	size_t i = 0;
	int pending_ws = 0, seen = 0;
	while (n > 0 && c_isspace((unsigned char)s[n - 1])) n--;
	while (i < n && c_isspace((unsigned char)s[i])) i++;
	while (i < n) {
		int32_t cp;
		int len = utf8_decode(s, n, i, &cp);
		if (c_isspace(cp)) {
			pending_ws = 1;
		} else {
			if (pending_ws && seen) bputc(out, ' ');
			pending_ws = 0;
			seen = 1;
			fold_into(out, cp);
		}
		i += (size_t)len;
	}
}

static Ref *ref_find(RefMap *m, const char *s, size_t n) {
	Buf key = {0};
	size_t i;
	Ref *hit = NULL;
	normalize_label(&key, s, n);
	for (i = 0; i < m->n; i++)
		if (m->v[i].label.n == key.n &&
		    memcmp(m->v[i].label.p, key.p, key.n) == 0) {
			hit = &m->v[i];
			break;
		}
	bfree(&key);
	return hit;
}

static void ref_add(RefMap *m, const char *lab, size_t lablen, Buf *url,
                    Buf *title) {
	Ref *r;
	if (ref_find(m, lab, lablen)) {	/* first definition wins */
		bfree(url);
		bfree(title);
		return;
	}
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 8;
		m->v = realloc(m->v, m->cap * sizeof *m->v);
		if (!m->v) oom();
	}
	r = &m->v[m->n++];
	memset(r, 0, sizeof *r);
	normalize_label(&r->label, lab, lablen);
	r->url = *url;
	r->title = *title;
}

static void refmap_free(RefMap *m) {
	size_t i;
	for (i = 0; i < m->n; i++) {
		bfree(&m->v[i].label);
		bfree(&m->v[i].url);
		bfree(&m->v[i].title);
	}
	free(m->v);
	m->v = NULL;
	m->n = m->cap = 0;
}

/* ------------------------------------------------------------ parser state */

typedef struct {
	Node *doc;
	Node *cur;		/* deepest open block */
	Node *dead;		/* unlinked blocks, freed with the document */
	RefMap refs;
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


/* ------------------------------------------------- HTML tag grammar (spec) */

/* Spaces and tabs, plus at most one line ending.  Attributes may be split
 * across one newline, which is why the inline raw-HTML scanner can see a tag
 * spanning two source lines. */
static size_t skip_sp_nl(const char *s, size_t n, size_t i) {
	int nl = 0;
	while (i < n) {
		if (s[i] == ' ' || s[i] == '\t') i++;
		else if (!nl && s[i] == '\n') { nl = 1; i++; }
		else if (!nl && s[i] == '\r') {
			nl = 1; i++;
			if (i < n && s[i] == '\n') i++;
		} else break;
	}
	return i;
}

static int attr_name_char(int c) {
	return c_isalnum(c) || c == '_' || c == '.' || c == ':' || c == '-';
}

/* One attribute: required whitespace, a name, and an optional value which may
 * be unquoted, single-quoted or double-quoted.  Returns the offset just past
 * it, or 0. */
static size_t scan_attribute(const char *s, size_t n, size_t i) {
	size_t j = skip_sp_nl(s, n, i), k, m;
	if (j == i || j >= n) return 0;
	if (!(c_isalpha((unsigned char)s[j]) || s[j] == '_' || s[j] == ':'))
		return 0;
	for (k = j + 1; k < n && attr_name_char((unsigned char)s[k]); k++)
		;
	m = skip_sp_nl(s, n, k);
	if (m >= n || s[m] != '=') return k;
	m = skip_sp_nl(s, n, m + 1);
	if (m >= n) return 0;
	if (s[m] == '\'' || s[m] == '"') {
		char q = s[m];
		for (k = m + 1; k < n && s[k] != q; k++)
			;
		if (k >= n) return 0;
		return k + 1;
	}
	for (k = m; k < n; k++) {
		char c = s[k];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' ||
		    c == '\'' || c == '=' || c == '<' || c == '>' || c == '`')
			break;
	}
	return k == m ? 0 : k;
}

/* `<` tagname attributes* `/`? `>`.  Reports the tag name so callers can
 * exclude pre/script/style/textarea.  Returns the length, or 0. */
static size_t scan_open_tag(const char *s, size_t n, size_t i, size_t *nstart,
                            size_t *nlen) {
	size_t j = i + 1, ns;
	if (i >= n || s[i] != '<') return 0;
	if (j >= n || !c_isalpha((unsigned char)s[j])) return 0;
	for (ns = j; j < n && (c_isalnum((unsigned char)s[j]) || s[j] == '-'); j++)
		;
	if (nstart) *nstart = ns;
	if (nlen) *nlen = j - ns;
	for (;;) {
		size_t k = scan_attribute(s, n, j);
		if (!k) break;
		j = k;
	}
	j = skip_sp_nl(s, n, j);
	if (j < n && s[j] == '/') j++;
	if (j < n && s[j] == '>') return j + 1 - i;
	return 0;
}

static size_t scan_closing_tag(const char *s, size_t n, size_t i) {
	size_t j = i + 2;
	if (i + 1 >= n || s[i] != '<' || s[i + 1] != '/') return 0;
	if (j >= n || !c_isalpha((unsigned char)s[j])) return 0;
	while (j < n && (c_isalnum((unsigned char)s[j]) || s[j] == '-')) j++;
	j = skip_sp_nl(s, n, j);
	if (j < n && s[j] == '>') return j + 1 - i;
	return 0;
}

/* `<!-->`, `<!--->`, or `<!--` anything `-->` (CommonMark 0.31 relaxed this:
 * the interior may now contain `--`). */
static size_t scan_comment(const char *s, size_t n, size_t i) {
	size_t j = i + 4;
	if (i + 4 > n || memcmp(s + i, "<!--", 4) != 0) return 0;
	if (j < n && s[j] == '>') return j + 1 - i;
	if (j + 1 < n && s[j] == '-' && s[j + 1] == '>') return j + 2 - i;
	for (; j + 2 < n; j++)
		if (s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')
			return j + 3 - i;
	return 0;
}

static size_t scan_pi(const char *s, size_t n, size_t i) {
	size_t j;
	if (i + 2 > n || s[i] != '<' || s[i + 1] != '?') return 0;
	for (j = i + 2; j + 1 < n; j++)
		if (s[j] == '?' && s[j + 1] == '>') return j + 2 - i;
	return 0;
}

static size_t scan_declaration(const char *s, size_t n, size_t i) {
	size_t j;
	if (i + 3 > n || s[i] != '<' || s[i + 1] != '!' ||
	    !c_isalpha((unsigned char)s[i + 2]))
		return 0;
	for (j = i + 3; j < n; j++)
		if (s[j] == '>') return j + 1 - i;
	return 0;
}

static size_t scan_cdata(const char *s, size_t n, size_t i) {
	size_t j;
	if (i + 9 > n || memcmp(s + i, "<![CDATA[", 9) != 0) return 0;
	for (j = i + 9; j + 2 < n; j++)
		if (s[j] == ']' && s[j + 1] == ']' && s[j + 2] == '>')
			return j + 3 - i;
	return 0;
}

/* Any of the six.  Used for inline raw HTML and for HTML block type 7. */
static size_t scan_html_tag(const char *s, size_t n, size_t i) {
	size_t m;
	if ((m = scan_open_tag(s, n, i, NULL, NULL)) != 0) return m;
	if ((m = scan_closing_tag(s, n, i)) != 0) return m;
	if ((m = scan_comment(s, n, i)) != 0) return m;
	if ((m = scan_pi(s, n, i)) != 0) return m;
	if ((m = scan_declaration(s, n, i)) != 0) return m;
	return scan_cdata(s, n, i);
}

/* The seven HTML block kinds, by start condition.  Types 1-5 end on a line
 * containing a specific string; types 6 and 7 end on a blank line. */
static const char *const HTML6[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body",
    "caption", "center", "col", "colgroup", "dd", "details", "dialog", "dir",
    "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
    "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header",
    "hr", "html", "iframe", "legend", "li", "link", "main", "menu",
    "menuitem", "nav", "noframes", "ol", "optgroup", "option", "p", "param",
    "search", "section", "summary", "table", "tbody", "td", "tfoot", "th",
    "thead", "title", "tr", "track", "ul", NULL};

static const char *const HTML1[] = {"pre", "script", "style", "textarea",
                                    NULL};

static int html_type1_name(const char *s, size_t n, size_t i) {
	int k;
	for (k = 0; HTML1[k]; k++) {
		size_t L = strlen(HTML1[k]);
		if (i + L <= n && ci_eq(s + i, HTML1[k], L)) return 1;
	}
	return 0;
}

static int scan_html_block_start(const char *s, size_t n, size_t i) {
	size_t j;
	int k;
	if (i >= n || s[i] != '<') return 0;
	if (i + 4 <= n && memcmp(s + i, "<!--", 4) == 0) return 2;
	if (i + 2 <= n && s[i + 1] == '?') return 3;
	if (i + 9 <= n && memcmp(s + i, "<![CDATA[", 9) == 0) return 5;
	if (i + 3 <= n && s[i + 1] == '!' && c_isalpha((unsigned char)s[i + 2]))
		return 4;
	for (k = 0; HTML1[k]; k++) {
		size_t L = strlen(HTML1[k]);
		j = i + 1;
		if (j + L > n || !ci_eq(s + j, HTML1[k], L)) continue;
		j += L;
		if (j >= n || s[j] == ' ' || s[j] == '\t' || s[j] == '>' ||
		    is_line_end(s[j]))
			return 1;
	}
	j = i + 1;
	if (j < n && s[j] == '/') j++;
	for (k = 0; HTML6[k]; k++) {
		size_t L = strlen(HTML6[k]), e;
		if (j + L > n || !ci_eq(s + j, HTML6[k], L)) continue;
		e = j + L;
		if (e >= n || s[e] == ' ' || s[e] == '\t' || s[e] == '>' ||
		    is_line_end(s[e]) ||
		    (s[e] == '/' && e + 1 < n && s[e + 1] == '>'))
			return 6;
	}
	return 0;
}

/* Type 7: a complete open or closing tag, any name but the type 1 four,
 * alone on the line but for trailing spaces and tabs. */
static int scan_html_block_start_7(const char *s, size_t n, size_t i) {
	size_t ns = 0, nl = 0, m = scan_open_tag(s, n, i, &ns, &nl);
	if (m) {
		if (nl && html_type1_name(s, n, ns) &&
		    (nl == 3 || nl == 5 || nl == 6 || nl == 8))
			return 0;
	} else {
		m = scan_closing_tag(s, n, i);
		if (!m) return 0;
	}
	i += m;
	while (i < n && is_space_or_tab(s[i])) i++;
	return (i >= n || is_line_end(s[i])) ? 7 : 0;
}

static int contains(const char *s, size_t n, size_t i, const char *pat) {
	size_t L = strlen(pat);
	for (; i + L <= n; i++)
		if (memcmp(s + i, pat, L) == 0) return 1;
	return 0;
}

static int html_block_end(int type, const char *s, size_t n, size_t i) {
	int k;
	switch (type) {
	case 1:
		for (k = 0; HTML1[k]; k++) {
			char pat[16];
			size_t L, j;
			snprintf(pat, sizeof pat, "</%s>", HTML1[k]);
			L = strlen(pat);
			for (j = i; j + L <= n; j++)
				if (ci_eq(s + j, pat, L)) return 1;
		}
		return 0;
	case 2: return contains(s, n, i, "-->");
	case 3: return contains(s, n, i, "?>");
	case 4: return contains(s, n, i, ">");
	case 5: return contains(s, n, i, "]]>");
	default: return 0;
	}
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

/* The ATX closing sequence: `## foo ##` is an h2 holding "foo".  The scan
 * runs over the whole line rather than over the content after the marker,
 * which is what makes `### ###` an empty h3: the trailing hashes are
 * preceded by a space, so they are a closing sequence, and the line is then
 * shorter than the offset the marker left us at -- add_line adds nothing. */
static void chop_hashes(Parser *p) {
	size_t e;
	while (p->len > 0 && c_isspace((unsigned char)p->line[p->len - 1]))
		p->len--;
	e = p->len;
	while (e > 0 && p->line[e - 1] == '#') e--;
	if (e != p->len && e > 0 &&
	    is_space_or_tab((unsigned char)p->line[e - 1])) {
		p->len = e;
		while (p->len > 0 && c_isspace((unsigned char)p->line[p->len - 1]))
			p->len--;
	}
}

/* A backslash before an ASCII punctuation character is an escape.  Used on
 * code fence info strings, link destinations and link titles, where the
 * escapes are resolved during parsing rather than during inline rendering.
 * Entity references in those three places go through unescape_buf alongside
 * these. */
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

/* ------------------------------------------------- link syntax (shared) */

/* `[` ... `]` with backslash escapes and no nested unescaped brackets.  The
 * spec caps the label at 999 characters.  Returns the length including both
 * brackets, or 0. */
#define MAX_LABEL 999

static size_t scan_link_label(const char *s, size_t n, size_t i, size_t *ls,
                              size_t *ll) {
	size_t j = i + 1;
	size_t len = 0;
	if (i >= n || s[i] != '[') return 0;
	while (j < n) {
		char c = s[j];
		if (c == '\\') {
			j++;
			len++;
			if (j < n && ascii_punct((unsigned char)s[j])) { j++; len++; }
		} else if (c == '[') {
			return 0;
		} else if (c == ']') {
			break;
		} else {
			j++;
			len++;
		}
		if (len > MAX_LABEL) return 0;
	}
	if (j >= n || s[j] != ']') return 0;
	*ls = i + 1;
	*ll = j - (i + 1);
	return j + 1 - i;
}

/* A link destination: either `<...>` or a bare run with balanced parentheses
 * and no ASCII space or control character.  Returns the consumed length (which
 * may be 0, for `()` and `<>`), or -1 for no match. */
static long scan_link_dest(const char *s, size_t n, size_t i, size_t *ds,
                           size_t *dl) {
	size_t j;
	int depth = 0;
	if (i < n && s[i] == '<') {
		for (j = i + 1; j < n; j++) {
			if (s[j] == '>') {
				*ds = i + 1;
				*dl = j - (i + 1);
				return (long)(j + 1 - i);
			}
			if (s[j] == '\\' && j + 1 < n &&
			    ascii_punct((unsigned char)s[j + 1])) {
				j++;
				continue;
			}
			if (s[j] == '\n' || s[j] == '<') return -1;
		}
		return -1;
	}
	for (j = i; j < n; j++) {
		unsigned char c = (unsigned char)s[j];
		if (c == '\\' && j + 1 < n && ascii_punct((unsigned char)s[j + 1])) {
			j++;
			continue;
		}
		if (c == '(') {
			if (++depth > 32) return -1;
			continue;
		}
		if (c == ')') {
			if (depth == 0) break;
			depth--;
			continue;
		}
		if (c <= 32 || c == 127) {
			if (j == i) return -1;
			break;
		}
	}
	if (j >= n || depth != 0) return -1;
	*ds = i;
	*dl = j - i;
	return (long)(j - i);
}

/* A link title in double quotes, single quotes or parentheses.  The quotes
 * are included in the returned span; clean_title strips them. */
static long scan_link_title(const char *s, size_t n, size_t i, size_t *ts,
                            size_t *tl) {
	char open, close;
	size_t j;
	if (i >= n) return -1;
	open = s[i];
	if (open == '"') close = '"';
	else if (open == '\'') close = '\'';
	else if (open == '(') close = ')';
	else return -1;
	for (j = i + 1; j < n; j++) {
		if (s[j] == '\\' && j + 1 < n && ascii_punct((unsigned char)s[j + 1])) {
			j++;
			continue;
		}
		if (open == '(' && s[j] == '(') return -1;
		if (s[j] == close) {
			*ts = i;
			*tl = j + 1 - i;
			return (long)(j + 1 - i);
		}
	}
	return -1;
}

/* `<scheme:anything-but-space-or-angle>` with a 2 to 32 character scheme.
 * Returns the length including both angle brackets, or 0. */
static size_t scan_autolink_uri(const char *s, size_t n, size_t i) {
	size_t j = i + 1, k;
	if (i >= n || s[i] != '<') return 0;
	if (j >= n || !c_isalpha((unsigned char)s[j])) return 0;
	for (k = j + 1; k < n && k - j < 32; k++) {
		char c = s[k];
		if (c_isalnum((unsigned char)c) || c == '.' || c == '+' || c == '-')
			continue;
		break;
	}
	if (k - j < 2 || k >= n || s[k] != ':') return 0;
	for (k++; k < n; k++) {
		unsigned char c = (unsigned char)s[k];
		if (c == '>') return k + 1 - i;
		if (c <= 0x20 || c == '<') return 0;
	}
	return 0;
}

static int email_local(int c) {
	return c_isalnum(c) || strchr(".!#$%&'*+/=?^_`{|}~-", c) != NULL;
}

/* `<local@label(.label)*>`, each label 1 to 63 characters of letters, digits
 * and hyphens, not starting or ending with a hyphen. */
static size_t scan_autolink_email(const char *s, size_t n, size_t i) {
	size_t j = i + 1, start;
	if (i >= n || s[i] != '<') return 0;
	for (start = j; j < n && email_local((unsigned char)s[j]); j++)
		;
	if (j == start || j >= n || s[j] != '@') return 0;
	for (;;) {
		size_t lab;
		j++;
		if (j >= n || !c_isalnum((unsigned char)s[j])) return 0;
		for (lab = j; j < n && (c_isalnum((unsigned char)s[j]) || s[j] == '-');
		     j++)
			;
		if (j - lab > 63 || s[j - 1] == '-') return 0;
		if (j < n && s[j] == '.') continue;
		break;
	}
	if (j < n && s[j] == '>') return j + 1 - i;
	return 0;
}

/* Spaces and tabs, then at most one line ending, then spaces and tabs. */
static size_t spnl(const char *s, size_t n, size_t i) {
	while (i < n && is_space_or_tab(s[i])) i++;
	if (i < n && s[i] == '\r') { i++; if (i < n && s[i] == '\n') i++; }
	else if (i < n && s[i] == '\n') i++;
	else return i;
	while (i < n && is_space_or_tab(s[i])) i++;
	return i;
}

/* Destinations and titles have their entities and backslash escapes resolved
 * at parse time, in that order, because `\&amp;` is a literal ampersand. */
static void unescape_buf(Buf *b) {
	Buf t = {0};
	size_t i = 0;
	bfit(b);	/* decode_entity() scans ahead in it; see bfit() */
	while (i < b->n) {
		if (b->p[i] == '&') {
			size_t m = decode_entity(&t, b->p, b->n, i);
			if (m) { i += m; continue; }
		}
		bputc(&t, b->p[i]);
		i++;
	}
	unescape_backslashes(&t);
	bfree(b);
	*b = t;
}

static void decode_entities_into(Buf *out, const char *s, size_t n) {
	size_t i = 0;
	while (i < n) {
		if (s[i] == '&') {
			size_t m = decode_entity(out, s, n, i);
			if (m) { i += m; continue; }
		}
		bputc(out, s[i]);
		i++;
	}
}

/* An autolink destination keeps its backslashes -- there are no escapes
 * inside angle brackets -- but entities are decoded, and an email autolink
 * gains a mailto: scheme. */
static void clean_autolink(Buf *out, const char *s, size_t n, int email) {
	Buf t = {0};
	bput(&t, s, n);
	btrim(&t);
	bfit(&t);	/* decode_entity() scans ahead in it; see bfit() */
	if (email) bputs(out, "mailto:");
	decode_entities_into(out, t.p, t.n);
	bfree(&t);
}

static void clean_url(Buf *out, const char *s, size_t n) {
	bput(out, s, n);
	btrim(out);
	unescape_buf(out);
}

static void clean_title(Buf *out, const char *s, size_t n) {
	if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
	               (s[0] == '\'' && s[n - 1] == '\'') ||
	               (s[0] == '(' && s[n - 1] == ')')))
		bput(out, s + 1, n - 2);
	else
		bput(out, s, n);
	unescape_buf(out);
}

/* `[label]: destination "title"` at the start of a paragraph's content.
 * Returns the number of bytes consumed, or 0.  A definition may be followed
 * by another, which is why finalize loops. */
static size_t parse_reference(RefMap *refs, const char *s, size_t n) {
	size_t i, ls = 0, ll = 0, ds = 0, dl = 0, ts = 0, tl = 0, before;
	size_t m;
	long r;
	Buf url = {0}, title = {0}, key = {0};
	int have_title = 0;

	m = scan_link_label(s, n, 0, &ls, &ll);
	if (!m) return 0;
	normalize_label(&key, s + ls, ll);
	if (key.n == 0) { bfree(&key); return 0; }
	bfree(&key);
	i = m;
	if (i >= n || s[i] != ':') return 0;
	i++;
	i = spnl(s, n, i);
	r = scan_link_dest(s, n, i, &ds, &dl);
	if (r < 0) return 0;
	i += (size_t)r;

	before = i;
	i = spnl(s, n, i);
	if (i != before) {
		r = scan_link_title(s, n, i, &ts, &tl);
		if (r > 0) { i += (size_t)r; have_title = 1; }
		else i = before;
	}
	while (i < n && is_space_or_tab(s[i])) i++;
	if (!(i >= n || is_line_end(s[i]))) {
		if (!have_title) return 0;
		/* A trailing title that does not end the line is not a title; try
		 * again with the destination alone. */
		i = before;
		have_title = 0;
		while (i < n && is_space_or_tab(s[i])) i++;
		if (!(i >= n || is_line_end(s[i]))) return 0;
	}
	if (i < n && s[i] == '\r') i++;
	if (i < n && s[i] == '\n') i++;

	clean_url(&url, s + ds, dl);
	if (have_title) clean_title(&title, s + ts, tl);
	ref_add(refs, s + ls, ll, &url, &title);
	return i;
}

/* ------------------------------------------------------- inline parsing */

/* One run of `*` or `_`, with the flanking verdict computed when it was
 * scanned.  The stack is doubly linked because process_emphasis walks it
 * forwards looking for closers and backwards looking for openers. */
typedef struct Delim Delim;
struct Delim {
	Delim *prev, *next;
	Node *inl_text;
	size_t length;
	char delim_char;
	int can_open, can_close;
};

/* An unresolved `[` or `![`.  The stack exists because a link's extent is
 * only known when its `]` is found, and the text in between has already been
 * parsed into nodes by then. */
typedef struct Bracket Bracket;
struct Bracket {
	Bracket *prev;
	Node *inl_text;		/* the literal "[" node, replaced on a match */
	size_t position;	/* just after the bracket */
	Delim *prev_delim;	/* delimiter stack top when this was pushed */
	int image, active, bracket_after;
};

typedef struct {
	const char *s;
	size_t n, pos;
	RefMap *refs;
	Bracket *last_bracket;
	Delim *last_delim;
} Subject;	/* one block's text, a cursor, and the two stacks */

static void push_delimiter(Subject *s, char c, int can_open, int can_close,
                           Node *inl) {
	Delim *d = xcalloc(sizeof *d);
	d->delim_char = c;
	d->can_open = can_open;
	d->can_close = can_close;
	d->inl_text = inl;
	d->length = inl->content.n;
	d->prev = s->last_delim;
	if (d->prev) d->prev->next = d;
	s->last_delim = d;
}

static void remove_delimiter(Subject *s, Delim *d) {
	if (d->next) d->next->prev = d->prev;
	else s->last_delim = d->prev;
	if (d->prev) d->prev->next = d->next;
	free(d);
}

static void push_bracket(Subject *s, int image, Node *inl_text) {
	Bracket *b = xcalloc(sizeof *b);
	if (s->last_bracket) s->last_bracket->bracket_after = 1;
	b->image = image;
	b->active = 1;
	b->inl_text = inl_text;
	b->position = s->pos;
	b->prev = s->last_bracket;
	b->prev_delim = s->last_delim;
	s->last_bracket = b;
}

static void pop_bracket(Subject *s) {
	Bracket *b = s->last_bracket;
	if (!b) return;
	s->last_bracket = b->prev;
	free(b);
}

/* The characters that can start something other than plain text.  Everything
 * else is swallowed in runs by handle_text. */
static int is_special(int c) {
	switch (c) {
	case '\n': case '\r': case '\\': case '&': case '`': case '<':
	case '[': case ']': case '!': case '*': case '_':
		return 1;
	default:
		return 0;
	}
}

static Node *make_literal(Type t, const char *s, size_t n) {
	Node *node = node_new(t);
	bput(&node->content, s, n);
	/* A literal never grows again; the renderer hands it to escape_html()
	 * with a separate length.  See bfit(): without this a two-byte "![" sat
	 * in a 32-byte allocation and a read past its end was invisible. */
	bfit(&node->content);
	return node;
}

static Node *handle_text(Subject *s) {
	size_t start = s->pos, end;
	while (s->pos < s->n && !is_special((unsigned char)s->s[s->pos])) s->pos++;
	end = s->pos;
	/* Trailing spaces before a line ending belong to the break, not to the
	 * text: they are what makes it a hard break. */
	if (s->pos < s->n && is_line_end(s->s[s->pos]))
		while (end > start && c_isspace((unsigned char)s->s[end - 1])) end--;
	return make_literal(N_TEXT, s->s + start, end - start);
}

static void skip_line_end(Subject *s) {
	if (s->pos < s->n && s->s[s->pos] == '\r') s->pos++;
	if (s->pos < s->n && s->s[s->pos] == '\n') s->pos++;
}

/* Two or more spaces then a line ending is a hard break; one or none is a
 * soft break. */
static Node *handle_newline(Subject *s) {
	size_t nlpos = s->pos;
	skip_line_end(s);
	while (s->pos < s->n && is_space_or_tab(s->s[s->pos])) s->pos++;
	if (nlpos > 1 && s->s[nlpos - 1] == ' ' && s->s[nlpos - 2] == ' ')
		return node_new(N_LINEBREAK);
	return node_new(N_SOFTBREAK);
}

/* A backslash escapes any ASCII punctuation character, is a hard break before
 * a line ending, and is a literal backslash anywhere else. */
static Node *handle_backslash(Subject *s) {
	s->pos++;
	if (s->pos < s->n && ascii_punct((unsigned char)s->s[s->pos])) {
		s->pos++;
		return make_literal(N_TEXT, s->s + s->pos - 1, 1);
	}
	if (s->pos < s->n && is_line_end(s->s[s->pos])) {
		skip_line_end(s);
		while (s->pos < s->n && is_space_or_tab(s->s[s->pos])) s->pos++;
		return node_new(N_LINEBREAK);
	}
	return make_literal(N_TEXT, "\\", 1);
}

/* Line endings inside a code span become spaces, and then one space is
 * removed from each end if the content begins and ends with a space but is
 * not all spaces.  Both halves are spec 0.29+ behaviour and both are in the
 * suite: `` ` `` is a code span holding one space, which is not stripped. */
static void normalize_code(Buf *b) {
	size_t r, w = 0;
	int nonspace = 0;
	for (r = 0; r < b->n; r++) {
		if (b->p[r] == '\r') {
			if (r + 1 >= b->n || b->p[r + 1] != '\n') b->p[w++] = ' ';
		} else if (b->p[r] == '\n') {
			b->p[w++] = ' ';
		} else {
			b->p[w++] = b->p[r];
		}
	}
	for (r = 0; r < w; r++)
		if (b->p[r] != ' ') nonspace = 1;
	if (nonspace && w >= 2 && b->p[0] == ' ' && b->p[w - 1] == ' ') {
		memmove(b->p, b->p + 1, w - 2);
		w -= 2;
	}
	b->n = w;
	if (b->p) b->p[b->n] = 0;
}

/* A code span is delimited by backtick runs of equal length.  An opening run
 * with no matching run is literal text.
 * shortcut: the search for the closing run is linear from the opening one, so
 * a line of n backticks is O(n^2).  cmark caches backtick positions; the
 * upgrade is the same table if a document ever makes it matter. */
static Node *handle_backticks(Subject *s) {
	size_t start = s->pos, openlen, cstart, j;
	while (s->pos < s->n && s->s[s->pos] == '`') s->pos++;
	openlen = s->pos - start;
	cstart = s->pos;
	for (j = cstart; j < s->n;) {
		size_t k;
		if (s->s[j] != '`') { j++; continue; }
		for (k = j; k < s->n && s->s[k] == '`'; k++)
			;
		if (k - j == openlen) {
			Buf b = {0};
			Node *n;
			bput(&b, s->s + cstart, j - cstart);
			normalize_code(&b);
			s->pos = k;
			n = make_literal(N_CODESPAN, b.p, b.n);
			bfree(&b);
			return n;
		}
		j = k;
	}
	s->pos = cstart;
	return make_literal(N_TEXT, s->s + start, openlen);
}

/* An ampersand that does not begin a valid character reference is literal. */
static Node *handle_entity(Subject *s) {
	Buf out = {0};
	Node *n;
	size_t m = decode_entity(&out, s->s, s->n, s->pos);
	if (!m) {
		bfree(&out);
		s->pos++;
		return make_literal(N_TEXT, "&", 1);
	}
	s->pos += m;
	n = make_literal(N_TEXT, out.p, out.n);
	bfree(&out);
	return n;
}

/* Everything that can start with `<`: a URI autolink, an email autolink, a
 * raw HTML tag, or a literal less-than sign. */
static Node *handle_pointy_brace(Subject *s) {
	size_t m;
	Node *n;
	if ((m = scan_autolink_uri(s->s, s->n, s->pos)) != 0 ||
	    (m = scan_autolink_email(s->s, s->n, s->pos)) != 0) {
		int email = s->s[s->pos] == '<' &&
		            scan_autolink_uri(s->s, s->n, s->pos) == 0;
		Node *txt = node_new(N_TEXT);
		n = node_new(N_LINK);
		clean_autolink(&n->url, s->s + s->pos + 1, m - 2, email);
		decode_entities_into(&txt->content, s->s + s->pos + 1, m - 2);
		bfit(&txt->content);	/* see bfit() */
		node_append(n, txt);
		s->pos += m;
		return n;
	}
	if ((m = scan_html_tag(s->s, s->n, s->pos)) != 0) {
		n = make_literal(N_HTML_INLINE, s->s + s->pos, m);
		s->pos += m;
		return n;
	}
	s->pos++;
	return make_literal(N_TEXT, "<", 1);
}

/* The flanking rules, which are the nastiest paragraph in the spec.  A run is
 * left-flanking if it is not followed by Unicode whitespace and either not
 * followed by punctuation or preceded by whitespace or punctuation;
 * right-flanking is the mirror image.  `_` is then further restricted so that
 * intraword underscores do not open or close emphasis. */
static size_t scan_delims(Subject *s, char c, int *can_open, int *can_close) {
	size_t numdelims = 0;
	int32_t before = 10, after = 10;
	int left, right;

	if (s->pos > 0) {
		size_t bp = s->pos - 1;
		while (bp > 0 && ((unsigned char)s->s[bp] >> 6) == 2) bp--;
		utf8_decode(s->s, s->n, bp, &before);
	}
	while (s->pos < s->n && s->s[s->pos] == c) {
		numdelims++;
		s->pos++;
	}
	if (s->pos < s->n) utf8_decode(s->s, s->n, s->pos, &after);

	left = numdelims > 0 && !uni_space(after) &&
	       !(uni_punct(after) && !uni_space(before) && !uni_punct(before));
	right = numdelims > 0 && !uni_space(before) &&
	        !(uni_punct(before) && !uni_space(after) && !uni_punct(after));
	if (c == '_') {
		*can_open = left && (!right || uni_punct(before));
		*can_close = right && (!left || uni_punct(after));
	} else {
		*can_open = left;
		*can_close = right;
	}
	return numdelims;
}

static Node *handle_delim(Subject *s, char c) {
	int can_open, can_close;
	size_t start = s->pos;
	size_t numdelims = scan_delims(s, c, &can_open, &can_close);
	Node *inl = make_literal(N_TEXT, s->s + start, numdelims);
	if (can_open || can_close)
		push_delimiter(s, c, can_open, can_close, inl);
	return inl;
}

/* Wraps everything between a matched opener and closer in <em> or <strong>,
 * consuming two delimiter characters if both runs have two to spare and one
 * otherwise.  A run with characters left over stays on the stack, which is
 * how `***foo***` becomes <em><strong>: the first pass takes two from each
 * end, the second takes the remaining one. */
static Delim *insert_emph(Subject *s, Delim *opener, Delim *closer) {
	Node *opener_inl = opener->inl_text, *closer_inl = closer->inl_text;
	size_t open_chars = opener_inl->content.n;
	size_t close_chars = closer_inl->content.n;
	size_t use = (close_chars >= 2 && open_chars >= 2) ? 2 : 1;
	Node *tmp, *tmpnext, *emph;
	Delim *d, *prev;

	open_chars -= use;
	close_chars -= use;
	opener_inl->content.n = open_chars;
	opener_inl->content.p[open_chars] = 0;
	closer_inl->content.n = close_chars;
	closer_inl->content.p[close_chars] = 0;

	for (d = closer->prev; d && d != opener; d = prev) {
		prev = d->prev;
		remove_delimiter(s, d);
	}

	emph = node_new(use == 1 ? N_EMPH : N_STRONG);
	for (tmp = opener_inl->next; tmp && tmp != closer_inl; tmp = tmpnext) {
		tmpnext = tmp->next;
		node_unlink(tmp);
		node_append(emph, tmp);
	}
	node_insert_after(opener_inl, emph);

	if (open_chars == 0) {
		node_unlink(opener_inl);
		node_free(opener_inl);
		remove_delimiter(s, opener);
	}
	if (close_chars == 0) {
		Delim *next = closer->next;
		node_unlink(closer_inl);
		node_free(closer_inl);
		remove_delimiter(s, closer);
		return next;
	}
	return closer;
}

/* The spec's own algorithm, including the "rule of 3": a closer that could
 * also open (or an opener that could also close) cannot match when the two
 * run lengths sum to a multiple of three unless the closer's own length
 * already is one.  openers_bottom remembers, per delimiter character and per
 * closer length mod 3, how far back a failed search got, so the whole thing
 * stays linear instead of quadratic. */
static void process_emphasis(Subject *s, Delim *stack_bottom) {
	Delim *closer = s->last_delim, *opener, *old_closer;
	Delim *openers_bottom[3][128];
	int opener_found, odd_match;

	memset(openers_bottom, 0, sizeof openers_bottom);
	while (closer && closer->prev != stack_bottom) closer = closer->prev;

	while (closer) {
		if (!closer->can_close) {
			closer = closer->next;
			continue;
		}
		opener = closer->prev;
		opener_found = 0;
		odd_match = 0;
		while (opener && opener != stack_bottom &&
		       opener != openers_bottom[closer->length % 3]
		                              [(unsigned char)closer->delim_char]) {
			if (opener->can_open && opener->delim_char == closer->delim_char) {
				odd_match = (closer->can_open || opener->can_close) &&
				            closer->length % 3 != 0 &&
				            (opener->length + closer->length) % 3 == 0;
				if (!odd_match) {
					opener_found = 1;
					break;
				}
			}
			opener = opener->prev;
		}
		old_closer = closer;
		if (opener_found) closer = insert_emph(s, opener, closer);
		else closer = closer->next;
		if (!opener_found) {
			openers_bottom[old_closer->length % 3]
			              [(unsigned char)old_closer->delim_char] =
			    old_closer->prev;
			if (!old_closer->can_open) remove_delimiter(s, old_closer);
		}
	}
	while (s->last_delim && s->last_delim != stack_bottom)
		remove_delimiter(s, s->last_delim);
}

/* `]` closes the nearest active `[` or `![`, if what follows makes a link:
 * an inline `(dest "title")`, a `[label]` in the reference map, a collapsed
 * `[]`, or nothing at all (shortcut reference).  Otherwise the `]` is
 * literal text and the opener is discarded. */
static Node *handle_close_bracket(Subject *s) {
	size_t initial;
	Bracket *opener;
	Node *inl, *tmp, *tmpnext;
	Buf url = {0}, title = {0};
	int is_image, matched = 0;

	s->pos++;
	initial = s->pos;
	opener = s->last_bracket;
	if (!opener) return make_literal(N_TEXT, "]", 1);
	if (!opener->active) {
		pop_bracket(s);
		return make_literal(N_TEXT, "]", 1);
	}
	is_image = opener->image;

	/* An inline link: ( destination title? ) */
	if (s->pos < s->n && s->s[s->pos] == '(') {
		size_t ds = 0, dl = 0, ts = 0, tl = 0;
		size_t sps = s->pos + 1, endurl, starttitle, endtitle, endall;
		long r;
		while (sps < s->n && c_isspace((unsigned char)s->s[sps])) sps++;
		r = scan_link_dest(s->s, s->n, sps, &ds, &dl);
		if (r >= 0) {
			endurl = sps + (size_t)r;
			starttitle = endurl;
			while (starttitle < s->n &&
			       c_isspace((unsigned char)s->s[starttitle]))
				starttitle++;
			endtitle = starttitle;
			if (starttitle != endurl) {
				long t = scan_link_title(s->s, s->n, starttitle, &ts, &tl);
				if (t > 0) endtitle = starttitle + (size_t)t;
			}
			endall = endtitle;
			while (endall < s->n && c_isspace((unsigned char)s->s[endall]))
				endall++;
			if (endall < s->n && s->s[endall] == ')') {
				s->pos = endall + 1;
				clean_url(&url, s->s + ds, dl);
				if (endtitle != starttitle)
					clean_title(&title, s->s + ts, tl);
				matched = 1;
			}
		}
	}

	/* A reference link: [label], [] or nothing. */
	if (!matched) {
		size_t ls = 0, ll = 0, m;
		int found_label;
		Ref *ref = NULL;
		m = scan_link_label(s->s, s->n, s->pos, &ls, &ll);
		found_label = m != 0;
		if (found_label) s->pos += m;
		if ((!found_label || ll == 0) && !opener->bracket_after) {
			ls = opener->position;
			ll = initial - 1 - opener->position;
			found_label = 1;
		}
		if (found_label) ref = ref_find(s->refs, s->s + ls, ll);
		if (ref) {
			bput(&url, ref->url.p, ref->url.n);
			bput(&title, ref->title.p, ref->title.n);
			matched = 1;
		}
	}

	if (!matched) {
		bfree(&url);
		bfree(&title);
		pop_bracket(s);
		s->pos = initial;
		return make_literal(N_TEXT, "]", 1);
	}

	inl = node_new(is_image ? N_IMAGE : N_LINK);
	inl->url = url;
	inl->title = title;
	node_insert_before(opener->inl_text, inl);
	for (tmp = opener->inl_text->next; tmp; tmp = tmpnext) {
		tmpnext = tmp->next;
		node_unlink(tmp);
		node_append(inl, tmp);
	}
	node_unlink(opener->inl_text);
	node_free(opener->inl_text);
	process_emphasis(s, opener->prev_delim);
	pop_bracket(s);

	/* Links do not nest: once one is made, every earlier link opener is
	 * deactivated.  Image openers are left alone, because a link may sit
	 * inside an image's alt text. */
	if (!is_image) {
		Bracket *b = s->last_bracket;
		while (b) {
			if (!b->image) {
				if (!b->active) break;
				b->active = 0;
			}
			b = b->prev;
		}
	}
	return NULL;
}

/* One inline per call.  The switch is in no particular order; precedence is
 * enforced by each handler consuming its own extent, and by process_emphasis
 * running last over whatever delimiters are left. */
static int parse_inline(Subject *s, Node *parent) {
	Node *n = NULL;
	int c;
	if (s->pos >= s->n) return 0;
	c = (unsigned char)s->s[s->pos];
	switch (c) {
	case '\r':
	case '\n':
		n = handle_newline(s);
		break;
	case '\\':
		n = handle_backslash(s);
		break;
	case '&':
		n = handle_entity(s);
		break;
	case '`':
		n = handle_backticks(s);
		break;
	case '<':
		n = handle_pointy_brace(s);
		break;
	case '[':
		s->pos++;
		n = make_literal(N_TEXT, "[", 1);
		node_append(parent, n);
		push_bracket(s, 0, n);
		return 1;
	case ']':
		n = handle_close_bracket(s);
		break;
	case '*':
	case '_':
		n = handle_delim(s, (char)c);
		break;
	case '!':
		s->pos++;
		if (s->pos < s->n && s->s[s->pos] == '[') {
			s->pos++;
			n = make_literal(N_TEXT, "![", 2);
			node_append(parent, n);
			push_bracket(s, 1, n);
			return 1;
		}
		n = make_literal(N_TEXT, "!", 1);
		break;
	default:
		n = handle_text(s);
		break;
	}
	if (n) node_append(parent, n);
	return 1;
}

static void parse_inlines(Node *parent, RefMap *refs) {
	Subject s;
	memset(&s, 0, sizeof s);
	/* The inline parser is the hungriest reader in the program -- backtick
	 * runs, delimiter flanking and bracket matching all scan ahead -- so
	 * this is the buffer most worth making exactly as big as its data. */
	bfit(&parent->content);
	s.s = parent->content.p;
	s.n = parent->content.n;
	s.refs = refs;
	while (s.n > 0 && c_isspace((unsigned char)s.s[s.n - 1])) s.n--;
	while (parse_inline(&s, parent))
		;
	process_emphasis(&s, NULL);
	while (s.last_bracket) pop_bracket(&s);
	while (s.last_delim) remove_delimiter(&s, s.last_delim);
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

static int buf_is_blank(Buf *b) {
	size_t i;
	for (i = 0; i < b->n; i++)
		if (!c_isspace((unsigned char)b->p[i])) return 0;
	return 1;
}

/* Strips leading link reference definitions off a paragraph's content and
 * reports whether anything but whitespace is left.  Called from two places:
 * paragraph finalize, and the setext branch, because

 *   [foo]: /url
 *   ===
 *
 * is not a heading -- there is no paragraph left to underline -- while
 *
 *   [foo]: /url
 *   bar
 *   ===
 *
 * is an h1 holding "bar". */
static int strip_references(Parser *p, Node *b) {
	size_t off = 0, m;
	while (off < b->content.n && b->content.p[off] == '[' &&
	       (m = parse_reference(&p->refs, b->content.p + off,
	                            b->content.n - off)) != 0)
		off += m;
	if (off) {
		memmove(b->content.p, b->content.p + off, b->content.n - off);
		b->content.n -= off;
		b->content.p[b->content.n] = 0;
	}
	return buf_is_blank(&b->content);
}

static Node *finalize(Parser *p, Node *b) {
	Node *parent = b->parent;
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
			unescape_buf(&b->info);
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
		/* A paragraph that was nothing but reference definitions leaves the
		 * tree.  It is parked rather than freed: process_line still holds
		 * pointers to it, and comparing against a freed pointer is the kind
		 * of thing that works until it does not. */
		if (strip_references(p, b)) {
			node_unlink(b);
			node_append(p->dead, b);
			return parent;
		}
		break;
	default:
		break;
	}
	/* The last write to this block's text has happened: it is closed, and
	 * only an open block gets lines appended.  From here it is read with a
	 * separate length by the inline parser and by escape_html(), so size it
	 * to its data.  See bfit(). */
	bfit(&b->content);
	return parent;
}

static void finalize_document(Parser *p) {
	while (p->cur) p->cur = finalize(p, p->cur);
}

/* The inline parser builds <em>, <strong>, <a> and <img> nodes by nesting,
 * and nothing in it bounds that nesting: 2n asterisks around a word produce n
 * nested <strong>, and 2n `![` produce n nested images.  The renderer
 * recurses, so this is checked once over the finished tree and the whole
 * document is refused rather than silently truncated.  The recursion here is
 * itself bounded by MAX_DEPTH, which is the same bound the renderer needs. */
static int tree_too_deep(Node *n, int d) {
	Node *c;
	if (d > MAX_DEPTH) return 1;
	for (c = n->first; c; c = c->next)
		if (tree_too_deep(c, d + 1)) return 1;
	return 0;
}

/* Inline content is parsed after the whole block structure is known, because
 * a link reference definition may be defined after its first use. */
static void process_inlines(Node *n, RefMap *refs) {
	Node *c;
	if (n->t == N_PARA || n->t == N_HEADING) parse_inlines(n, refs);
	for (c = n->first; c; c = c->next) process_inlines(c, refs);
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
		case N_HTML_BLOCK:
			/* Types 1-5 swallow blank lines; 6 and 7 end at one. */
			if (cont->html_type >= 6 && p->blank) return cont->parent;
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

/* Every block start, in the one order that works.  Setext before thematic
 * break so `---` under a paragraph is an h2; thematic break before list
 * markers so `* * *` is a break; list markers before indented code so a
 * marker at column 4 inside an open list still starts an item. */
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
		} else if (!indented &&
		           ((lev = scan_html_block_start(p->line, p->len, p->fns)) ||
		            ((*cont)->t != N_PARA &&
		             (lev = scan_html_block_start_7(p->line, p->len,
		                                            p->fns))))) {
			*cont = add_child(p, *cont, N_HTML_BLOCK);
			(*cont)->html_type = lev;
			/* The offset is deliberately not advanced: the tag is content. */
		} else if (!indented && (*cont)->t == N_PARA && all_matched &&
		           (lev = scan_setext(p, p->fns)) != 0 &&
		           !strip_references(p, *cont)) {
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

	if (cont->t == N_CODE) {
		add_line(p, cont);
	} else if (cont->t == N_HTML_BLOCK) {
		add_line(p, cont);
		if (html_block_end(cont->html_type, p->line, p->len, p->fns))
			p->cur = cont = finalize(p, cont);
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

/* Percent-encoding for URLs, matching the reference implementation's set:
 * alphanumerics and the reserved characters that are meant to work as URL
 * components pass through unchanged, on the assumption that a reserved
 * character inside a URL is there to do its reserved job.  `&` and `'` are
 * the two exceptions -- they are left in the URL but need HTML entity
 * escaping to produce valid markup -- and everything else, including every
 * byte of a multi-byte UTF-8 sequence, becomes %XX. */
static int href_safe(unsigned char c) {
	return c != 0 && (c_isalnum(c) ||
	                  strchr("-_.+!*(),%#@?=;:/$~", (char)c) != NULL);
}

static void escape_href(Buf *out, const char *s, size_t n) {
	static const char hex[] = "0123456789ABCDEF";
	size_t i;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (href_safe(c)) { bputc(out, (char)c); continue; }
		switch (c) {
		case '&': bputs(out, "&amp;"); break;
		case '\'': bputs(out, "&#x27;"); break;
		case ' ': bputs(out, "%20"); break;
		default:
			bputc(out, '%');
			bputc(out, hex[(c >> 4) & 0xF]);
			bputc(out, hex[c & 0xF]);
		}
	}
}

static void cr(Buf *out) {
	if (out->n && out->p[out->n - 1] != '\n') bputc(out, '\n');
}

static void render(Buf *out, Node *n, int depth);

/* An image's alt text is the plain text of its children: tags dropped, code
 * kept as its literal, any break collapsed to a space. */
static void render_plain(Buf *out, Node *n, int depth) {
	Node *c;
	if (depth > MAX_DEPTH) return;
	for (c = n->first; c; c = c->next) {
		switch (c->t) {
		case N_TEXT:
		case N_CODESPAN:
		case N_HTML_INLINE:
			escape_html(out, c->content.p, c->content.n);
			break;
		case N_SOFTBREAK:
		case N_LINEBREAK:
			bputc(out, ' ');
			break;
		default:
			render_plain(out, c, depth + 1);
			break;
		}
	}
}

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
	case N_HTML_BLOCK:
		cr(out);
		bput(out, n->content.p, n->content.n);
		cr(out);
		break;
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
	case N_CODESPAN:
		bputs(out, "<code>");
		escape_html(out, n->content.p, n->content.n);
		bputs(out, "</code>");
		break;
	case N_HTML_INLINE:
		bput(out, n->content.p, n->content.n);
		break;
	case N_LINK:
		bputs(out, "<a href=\"");
		escape_href(out, n->url.p, n->url.n);
		bputc(out, '"');
		if (n->title.n) {
			bputs(out, " title=\"");
			escape_html(out, n->title.p, n->title.n);
			bputc(out, '"');
		}
		bputc(out, '>');
		render_children(out, n, depth);
		bputs(out, "</a>");
		break;
	case N_EMPH:
		bputs(out, "<em>");
		render_children(out, n, depth);
		bputs(out, "</em>");
		break;
	case N_STRONG:
		bputs(out, "<strong>");
		render_children(out, n, depth);
		bputs(out, "</strong>");
		break;
	case N_IMAGE:
		bputs(out, "<img src=\"");
		escape_href(out, n->url.p, n->url.n);
		bputs(out, "\" alt=\"");
		render_plain(out, n, depth);
		bputc(out, '"');
		if (n->title.n) {
			bputs(out, " title=\"");
			escape_html(out, n->title.p, n->title.n);
			bputc(out, '"');
		}
		bputs(out, " />");
		break;
	case N_SOFTBREAK:
		bputc(out, '\n');
		break;
	case N_LINEBREAK:
		bputs(out, "<br />\n");
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

#ifdef CMARK_PLANT_OVERREAD
	/* A deliberate one-past-the-end read of the input, for `make overread`.
	 * bput() keeps a NUL at p[n], so src[srclen] is still in bounds and
	 * src[srclen + 1] is the first byte that is not.  ASan can only see it
	 * if main() handed us a buffer of exactly the right size: a doubling
	 * buffer leaves up to srclen bytes of live allocation past the end for
	 * this read to land harmlessly inside, which is the whole point of the
	 * target.  volatile so -O1 cannot drop the load. */
	{
		static volatile char sink;
		sink = src[srclen + 1];
		(void)sink;
	}
#endif

	memset(&p, 0, sizeof p);
	normalize(&text, src, srclen);
	bfit(&text);	/* nothing appends to it after this; the parser only reads */
	p.doc = node_new(N_DOC);
	p.dead = node_new(N_DOC);
	p.cur = p.doc;

	for (start = 0, i = 0; i < text.n; i++) {
		if (text.p[i] == '\n') {
			p.lineno++;
			process_line(&p, text.p + start, i - start + 1);
			start = i + 1;
			if (p.too_deep) break;
		}
	}
	if (p.too_deep) {
		rc = 1;
	} else {
		finalize_document(&p);
		process_inlines(p.doc, &p.refs);
		if (tree_too_deep(p.doc, 0)) rc = 1;
		else render(out, p.doc, 0);
	}
	node_free(p.doc);
	node_free(p.dead);
	refmap_free(&p.refs);
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
	bfit(&in);	/* see bfit(): a doubled read buffer blinds the sanitizer */
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
