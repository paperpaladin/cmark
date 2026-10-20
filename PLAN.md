# PLAN

## Contract

`cmark` is one C11 file that parses CommonMark 0.31.2 and writes HTML.

- stdin or one named file in, HTML on stdout.  Exit 0 rendered, 1 input nested
  too deeply, 2 usage or I/O error.
- Block structure: paragraphs, ATX and setext headings, block quotes, bullet
  and ordered lists, indented and fenced code blocks, thematic breaks, HTML
  blocks, link reference definitions.
- Inline structure: emphasis and strong emphasis, code spans, links, images,
  autolinks, raw HTML, hard and soft line breaks, backslash escapes, entity
  and numeric character references.
- Not in the contract: any extension (tables, strikethrough, task lists,
  footnotes, autolink literals), smart punctuation, XML/LaTeX/man output, an
  AST API for callers, source position tracking, incremental reparsing, a
  safe/unsafe mode distinction (raw HTML is always passed through, like
  `cmark --unsafe`), or 652/652.

## Hard invariant

**Every example the parser claims to pass is compared byte for byte against
the HTML in the spec's own `spec.json`, with nothing normalised, and the
per-section score is pinned so it cannot drop silently.**

Both halves matter.  Byte-for-byte means no whitespace trimming, no entity
re-spelling, no "close enough": the spec fixes exactly where the newlines go
and exactly which characters are escaped, and a harness that normalises either
side is measuring itself.  Pinned means `t/sections.expected` records
passed/total for all 26 sections plus the total, and `make test` diffs it, so
a change that fixes four examples and breaks two is a test failure, not a
quietly flat number.

The second invariant is memory safety on untrusted input. A nesting-depth
limit that returns an error instead of overflowing the stack, proved under
`-fsanitize=address,undefined`.

A Markdown parser claiming correctness without a number from a corpus it did
not write is making an unverifiable claim.  The number is the deliverable, and
so is the honest list of sections it fails.

## Oracle

Ranked by how little we control them.

1. The CommonMark specification's own conformance suite.  `spec.json` from
   <https://spec.commonmark.org/0.31.2/spec.json>: 652 numbered (markdown,
   html) pairs, every one extracted from the spec text itself, tagged with the
   section it illustrates.  It *is* the definition of correct: not a reading
   of the spec, not a port of someone's tests, the spec.  Vendored gzipped as
   `t/commonmark-spec-0.31.2.json.gz` (18kB, 140kB uncompressed) with its
   licence in `t/commonmark-spec-LICENSE`, so `make test` needs no network.
   The data is left byte-exact: it deliberately contains smart quotes,
   ellipses, ill-formed UTF-8 and tabs as *test input*.
2. The sanitizers.  `-fsanitize=address,undefined` over the whole suite
   plus 400kB of `/dev/urandom`, an embedded NUL, ill-formed UTF-8, and the
   deep-nesting cases.  Not a correctness oracle; the only authority on memory
   safety that is not our own opinion.
3. Ourselves, and said so.  The CLI contract and the depth limit are our
   own tests of our own choices.  They are labelled as such in the README.

## Non-goals

- Any Markdown dialect other than CommonMark 0.31.2.  No tables, no
  strikethrough, no task lists, no footnotes, no autolink literals, no smart
  punctuation, no attributes.
- A library API.  There is no header, no `cmark_parse`/`cmark_render` pair
  exposed to callers, no AST walker, no source position tracking.  One
  function, `run()`, takes bytes and returns HTML.
- A safe mode.  Raw HTML in the input is passed through, as the
  spec's expected output requires and what `cmark --unsafe` does.  A
  `--safe` mode that replaced it with a comment would be a second renderer.
- Performance beyond "the suite runs in under a second".  The parser is O(n)
  per line in the normal case. Two places are knowingly worse and carry
  `shortcut:` comments (backtick scanning, reference lookup).
- Claiming that 652/652 means "correct".  It means every example in the
  spec is reproduced byte for byte.  The suite is not exhaustive, and the
  README lists what is known to be outside it.

Two things that *were* non-goals and stopped being so, because the suite
would not allow the shortcut: full Unicode case folding for link labels (the
spec matches `[SS]` against `[U+1E9E]`, which needs the multi-character
foldings) and the real Unicode punctuation categories for emphasis flanking
(0.31 includes the symbol categories, so an ASCII approximation fails in both
directions).  Both are now generated tables, not hand-written
approximations.

## The generated tables

Three tables in `cmark.c` are generated data, never hand-written.  None of them
has an algorithm hiding in it, and all three are reproducible from CPython's
standard library, which is where the authority lives, not in this
repo.

The 2125 HTML5 named character references.  Here is the one-liner that produced them:

```sh
python3 -c 'import html.entities
for k, v in sorted(html.entities.html5.items()):
    if k.endswith(";"):
        print(k[:-1], v.encode("utf-8").hex())'
```

CPython ships the WHATWG table, the one the CommonMark spec points
at.  Keys without a trailing semicolon are dropped: CommonMark requires the
semicolon, so `&amp` without one is literal text.  The values are written as
octal escapes so the source file stays ASCII.

The 339 Unicode punctuation ranges, for emphasis flanking:

```sh
python3 -c 'import unicodedata
cats = {"Pc","Pd","Pe","Pf","Pi","Po","Ps","Sc","Sk","Sm","So"}
print([c for c in range(0x110000) if unicodedata.category(chr(c)) in cats])'
```

The 681 case-folding runs and 104 multi-character foldings, for link
label matching:

```sh
python3 -c 'print([(c, chr(c).casefold()) for c in range(0x110000)
                   if chr(c).casefold() != chr(c)])'
```

Both are Unicode 15.1.0 as shipped with this CPython. The spec is written
against 15.0 and the difference touches no example in the suite.

## Commit sequence

- [x] line reader, paragraphs, HTML escaping, the spec runner, the pin
  - why: the harness comes first.  104/652 falls out of "every paragraph is a
    paragraph", and that number is the baseline every later commit moves.
- [x] thematic breaks and ATX headings
  - why: the two simplest leaf blocks, and they need `open_new_blocks` to
    exist, which everything else then hangs off.
- [x] setext headings
  - why: the first block type that mutates an already-open block, which is
    what forces `all_matched` into the signature.
- [x] indented code blocks
  - why: needs the column-accurate tab handling in `advance`, which nothing
    before this exercises.
- [x] fenced code blocks
  - why: the first block with a close condition of its own and an info string
    that needs entity and backslash unescaping.
- [x] block quotes
  - why: the first real container, and the first place lazy continuation can
    be observed.
- [x] lists, bullet and ordered
  - why: the hardest container.  Item continuation, padding, `lists_match`,
    and tight versus loose, a property of the whole list decided at
    finalize time.
- [x] HTML blocks, all seven start and end conditions
  - why: 44 examples, and type 7's "cannot interrupt a paragraph" rule is the
    only reason `open_new_blocks` needs to know the container type.
- [x] link reference definitions
  - why: block-level because they are stripped from paragraph starts at
    finalize, but their payload is inline syntax, so this is the seam.
- [x] inline framework: text runs, soft and hard breaks, backslash escapes
  - why: replaces the placeholder inline pass.  Hard breaks first because they
    are the only inline that depends on trailing whitespace, which the
    paragraph accumulator must not have eaten.
- [x] entity and numeric character references
  - why: needs the HTML5 named entity table, which is bulk data and belongs in
    its own commit so the diff of real code stays readable.
- [x] code spans
  - why: highest precedence inline, so everything after it must respect it.
- [x] autolinks and raw HTML
  - why: both start at `<`; one dispatch point, two grammars.
- [x] links and images
  - why: needs the bracket stack and the reference map from two commits back.
- [x] emphasis
  - why: last, because the delimiter stack has to be able to see link
    boundaries.  Left- and right-flanking plus the rule of 3.
- [x] the last four failures: ATX closing sequences, info string entities,
  setext after reference definitions, full Unicode case folding
  - why: 648 -> 652.  Each one was a single condition in the wrong place, and
    all four were found by reading the failure dump, not the spec.
- [x] the depth limit and `make test-asan`
  - why: the limit is cheap but the test for it is the point, and it can only
    be written once containers and inline brackets both recurse.
- [x] README with the per-section table and the shortfalls named
  - why: last, so the numbers in it are the numbers the suite prints.
- [x] `bfit()` and `make overread`: stop the doubling buffers from switching
  the sanitizer off
  - why: `bgrow()` doubles and never shrinks, so `run()` was handed up to
    twice the document's bytes and ASan could not see a read past the end of
    the input.  Proved first, then fixed: a planted read of
    `src[srclen + 1]` under `-fsanitize=address,undefined` produced exit 0
    and an empty stderr, and after one `realloc` down to `n + 1` the same
    probe produced `heap-buffer-overflow ... 0 bytes after 17-byte region`.
    `make overread` requires the report and fails without it, verified by
    deleting `bfit(&in)`.  The same shape was blind in the inline parser's
    subject and in every node `escape_html()` renders, which makes an
    existing README claim about `escape_html` false as written; both are
    fitted now and the README records that it was wrong.
