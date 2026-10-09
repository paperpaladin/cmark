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

The second invariant is memory safety on untrusted input: a nesting-depth
limit that returns an error instead of overflowing the stack, proved under
`-fsanitize=address,undefined`.

A Markdown parser claiming correctness without a number from a corpus it did
not write is making an unverifiable claim.  The number is the deliverable, and
so is the honest list of sections it fails.

## Oracle

Ranked by how little we control them.

1. **The CommonMark specification's own conformance suite.**  `spec.json` from
   <https://spec.commonmark.org/0.31.2/spec.json>: 652 numbered (markdown,
   html) pairs, every one extracted from the spec text itself, tagged with the
   section it illustrates.  It *is* the definition of correct -- not a reading
   of the spec, not a port of someone's tests, the spec.  Vendored gzipped as
   `t/commonmark-spec-0.31.2.json.gz` (18kB, 140kB uncompressed) with its
   licence in `t/commonmark-spec-LICENSE`, so `make test` needs no network.
   The data is left byte-exact: it deliberately contains smart quotes,
   ellipses, ill-formed UTF-8 and tabs as *test input*.
2. **The sanitizers.**  `-fsanitize=address,undefined` over the whole suite
   plus 400kB of `/dev/urandom`, an embedded NUL, ill-formed UTF-8, and the
   deep-nesting cases.  Not a correctness oracle; the only authority on memory
   safety that is not our own opinion.
3. **Ourselves, and said so.**  The CLI contract and the depth limit are our
   own tests of our own choices.  They are labelled as such in the README.

## Non-goals

- 652/652.  That is a multi-year target and claiming it would be a lie.  The
  aim is the block-level and basic-inline sections clean and an accurate
  report of the rest.
- Any Markdown dialect other than CommonMark 0.31.2.
- Full Unicode case folding for link labels, or the full Unicode punctuation
  and whitespace tables for emphasis flanking.  Both are approximated to the
  ranges the suite exercises, marked `shortcut:` in the source and stated in
  the README.
- Performance.  The parser is O(n) per line in the normal case and makes no
  attempt to bound the pathological ones beyond the depth limit.

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
- [ ] indented code blocks
  - why: needs the column-accurate tab handling in `advance`, which nothing
    before this exercises.
- [ ] fenced code blocks
  - why: the first block with a close condition of its own and an info string
    that needs entity and backslash unescaping.
- [ ] block quotes
  - why: the first real container, and the first place lazy continuation can
    be observed.
- [ ] lists, bullet and ordered
  - why: the hardest container.  Item continuation, padding, `lists_match`,
    and tight versus loose, which is a property of the whole list decided at
    finalize time.
- [ ] HTML blocks, all seven start and end conditions
  - why: 44 examples, and type 7's "cannot interrupt a paragraph" rule is the
    only reason `open_new_blocks` needs to know the container type.
- [ ] link reference definitions
  - why: block-level because they are stripped from paragraph starts at
    finalize, but their payload is inline syntax, so this is the seam.
- [ ] inline framework: text runs, soft and hard breaks, backslash escapes
  - why: replaces the placeholder inline pass.  Hard breaks first because they
    are the only inline that depends on trailing whitespace, which the
    paragraph accumulator must not have eaten.
- [ ] entity and numeric character references
  - why: needs the HTML5 named entity table, which is bulk data and belongs in
    its own commit so the diff of real code stays readable.
- [ ] code spans
  - why: highest precedence inline, so everything after it must respect it.
- [ ] autolinks and raw HTML
  - why: both start at `<`; one dispatch point, two grammars.
- [ ] links and images
  - why: needs the bracket stack and the reference map from two commits back.
- [ ] emphasis
  - why: last, because the delimiter stack has to be able to see link
    boundaries.  Left- and right-flanking plus the rule of 3.
- [ ] tabs
  - why: a cross-cutting section rather than a feature; it only becomes
    testable once every block type exists to be indented by a tab.
- [ ] the depth limit and `make test-asan`
  - why: the limit is cheap but the test for it is the point, and it can only
    be written once containers and inline brackets both recurse.
- [ ] README with the per-section table and the shortfalls named
  - why: last, so the numbers in it are the numbers the suite prints.
