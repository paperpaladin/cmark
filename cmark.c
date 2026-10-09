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

/* shortcut: case folding for link labels covers ASCII, Latin-1 Supplement,
 * Latin Extended-A, Greek and Cyrillic -- the alphabets the spec suite uses.
 * Full Unicode case folding needs the CaseFolding.txt table (and the
 * multi-character foldings: SS -> ss, 1E9E -> 00DF), which is a few thousand
 * entries of generated data for a handful of real labels.  Upgrade path: the
 * same generated-table trick the entity list uses. */
static int32_t fold_cp(int32_t c) {
	if (c >= 'A' && c <= 'Z') return c + 32;
	if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 0x20;
	if (c == 0x0178) return 0x00FF;
	if (c >= 0x0100 && c <= 0x0177) return (c % 2 == 0) ? c + 1 : c;
	if (c >= 0x0179 && c <= 0x017E) return (c % 2 == 1) ? c + 1 : c;
	if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return c + 0x20;
	if (c == 0x0386) return 0x03AC;
	if (c >= 0x0388 && c <= 0x038A) return c + 0x25;
	if (c == 0x038C) return 0x03CC;
	if (c == 0x038E) return 0x03CD;
	if (c == 0x038F) return 0x03CE;
	if (c >= 0x0400 && c <= 0x040F) return c + 0x50;
	if (c >= 0x0410 && c <= 0x042F) return c + 0x20;
	return c;
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
			utf8_encode(out, fold_cp(cp));
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

/* The subject of inline parsing: one block's accumulated text, a cursor, and
 * the stacks that emphasis and links need. */
typedef struct {
	const char *s;
	size_t n, pos;
	RefMap *refs;
} Subject;

/* The characters that can start something other than plain text.  Everything
 * else is swallowed in runs by handle_text. */
static int is_special(int c) {
	switch (c) {
	case '\n': case '\r': case '\\': case '&': case '`': case '<':
		return 1;
	default:
		return 0;
	}
}

static Node *make_literal(Type t, const char *s, size_t n) {
	Node *node = node_new(t);
	bput(&node->content, s, n);
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

/* shortcut: emphasis, links, images, autolinks and raw
 * HTML are added to this switch commit by commit.  See PLAN.md. */
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
	s.s = parent->content.p;
	s.n = parent->content.n;
	s.refs = refs;
	while (s.n > 0 && c_isspace((unsigned char)s.s[s.n - 1])) s.n--;
	while (parse_inline(&s, parent))
		;
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
	case N_PARA: {
		/* Leading link reference definitions are stripped out here, not in
		 * the inline pass, because a definition may be referenced by text
		 * that appears earlier in the document. */
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
		if (buf_is_blank(&b->content)) {
			node_unlink(b);
			node_free(b);
			return parent;
		}
		break;
	}
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
		} else if (!indented &&
		           ((lev = scan_html_block_start(p->line, p->len, p->fns)) ||
		            ((*cont)->t != N_PARA &&
		             (lev = scan_html_block_start_7(p->line, p->len,
		                                            p->fns))))) {
			*cont = add_child(p, *cont, N_HTML_BLOCK);
			(*cont)->html_type = lev;
			/* The offset is deliberately not advanced: the tag is content. */
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
		process_inlines(p.doc, &p.refs);
		render(out, p.doc, 0);
	}
	node_free(p.doc);
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
