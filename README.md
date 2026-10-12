# cmark

A CommonMark 0.31.2 parser and HTML renderer in one C11 file.  No
dependencies beyond libc.  3689 lines, of which 1143 are the three generated
Unicode and entity tables, so about 2550 lines of parser.

```sh
make
echo '**hi** `there`' | ./cmark
make test        # the score, pinned per section
make test-asan   # the same, under -fsanitize=address,undefined
```

## The score, which is the only interesting claim

Anybody can write a Markdown parser and declare it conformant.  The claim is
unverifiable without a number from a corpus the author did not write, and for
CommonMark there is no excuse for not having one, because **the specification
ships its own conformance suite**: `spec.json` is 652 numbered (markdown,
html) pairs, one per example in the spec text, each tagged with the section it
illustrates.  It is not a reading of the spec and not a port of somebody's
tests.  It is the spec.

```
SPEC 652/652 examples (CommonMark 0.31.2)
```

| passed | section | passed | section |
| --- | --- | --- | --- |
| 11/11 | Tabs | 48/48 | List items |
| 13/13 | Backslash escapes | 26/26 | Lists |
| 17/17 | Entity and numeric character references | 1/1 | Inlines |
| 1/1 | Precedence | 22/22 | Code spans |
| 19/19 | Thematic breaks | 132/132 | Emphasis and strong emphasis |
| 18/18 | ATX headings | 90/90 | Links |
| 27/27 | Setext headings | 22/22 | Images |
| 12/12 | Indented code blocks | 19/19 | Autolinks |
| 29/29 | Fenced code blocks | 20/20 | Raw HTML |
| 44/44 | HTML blocks | 15/15 | Hard line breaks |
| 27/27 | Link reference definitions | 2/2 | Soft line breaks |
| 8/8 | Paragraphs | 3/3 | Textual content |
| 1/1 | Blank lines | | |
| 25/25 | Block quotes | **652/652** | **TOTAL** |

**No section fails.**  That sentence is worth less than it looks, and the next
two sections of this README are about why, so read them before believing the
table.

The per-section numbers are pinned in `t/sections.expected` and diffed on
every run.  A total on its own hides the shape of a regression: a change that
fixes four examples in Links and breaks two in Lists leaves the total up by
two and reads as progress.  With the pin it is a test failure.

### Byte for byte, and nothing normalised

Comparison is `got == want` on raw stdout bytes.  Not trailing newlines, not
entity spelling, not attribute order, not whitespace.  This matters more than
the score, because the easiest way to get a high number out of a Markdown
test harness is to normalise both sides until they agree.

The evidence that the harness is real: change `</p>\n` to `</p>\n\n` in the
renderer -- one byte, in the one place a human would never notice -- and the
score drops from 652/652 to 149/652 and `make test` fails on the pinned diff.
503 examples catch that byte.

**The only transformations applied to input** are the two the spec mandates
plus one that is unobservable:

- CRLF and lone CR become LF.  The spec treats all three as line endings; the
  output always uses LF, which is what the expected HTML in the suite uses.
  If your input has CRLF line endings your output will not.
- U+0000 becomes U+FFFD, because the spec says so ("For security reasons, the
  Unicode character `U+0000` must be replaced with the REPLACEMENT
  CHARACTER").
- A final newline is appended if the input lacks one.  This changes nothing
  observable; it lets the block parser treat every line identically.

Nothing else is touched.  Ill-formed UTF-8 is passed through unchanged (and
reported as U+FFFD *to the flanking classifier* without being rewritten in
the output), tabs are expanded to tab stops only where the spec expands them,
and no input is rejected as invalid, because CommonMark has no invalid input.

## What does not work

A README that only lists successes is not finished.  652/652 means "every
example in the specification is reproduced byte for byte".  It does not mean
"correct", and here is the gap.

### The suite is not exhaustive, and here is what it does not reach

- **No source positions.**  Nothing in this parser records where a node came
  from.  `spec.json` cannot notice, because it only compares HTML.  If you
  wanted error messages pointing at line and column, this is a rewrite of the
  node structure, not an addition.
- **No AST, no API, no header.**  `run()` takes bytes and returns HTML.
  There is no `cmark_parse`, no tree walker, no node accessors, no way to
  render anything but HTML.  The suite cannot tell, because the suite only
  runs the binary.
- **Raw HTML is always passed through.**  This is what the expected output
  requires, and it is what `cmark --unsafe` does.  There is no safe mode, so
  **do not point this at untrusted Markdown and serve the output**:
  `<script>alert(1)</script>` in the input is `<script>alert(1)</script>` in
  the output.  The reference implementation defaults to replacing raw HTML
  with `<!-- raw HTML omitted -->` and scores worse on its own suite as a
  result; we score better by being less safe, and the suite will never say so.
- **No extensions.**  No tables, strikethrough, task lists, footnotes,
  autolink literals, header attributes, or smart punctuation.  Those are
  GFM/cmark-gfm territory and none of them are in CommonMark, so none of them
  are here.
- **Performance is unmeasured.**  The suite's 652 documents are tiny.  There
  is no benchmark in this repo and no claim about throughput.

### Deliberate deviations from the specification

- **A nesting-depth limit of 512.**  The spec sets no limit.  A document with
  more than 512 levels of nested containers, or whose inline tree nests more
  than 512 deep (2n asterisks around a word nest n deep, 2n `![` nest n
  deep), is **refused entirely** with exit status 1 and a message on stderr,
  rather than rendered.  That is a real deviation and a visible one: a valid
  CommonMark document exists that this program will not render.  The
  alternative is a stack overflow on untrusted input, which is worse.
- **Nine digits maximum in an ordered list marker.**  The spec says "up to 9
  digits", so this one is conformant, but it means `1000000000. x` is a
  paragraph, not a list.
- **32 levels of nested parentheses in a bare link destination.**  The spec
  says only "balanced".  This matches the reference implementation's limit and
  nothing in the suite goes past 3.
- **Unicode 15.1.0 tables.**  The spec is written against 15.0.  The
  punctuation categories and case foldings come from whatever CPython on this
  machine ships, which was 15.1.0.  No example in the suite is affected, and
  the difference would show up only in emphasis flanking around characters
  added or recategorised between the two versions.

### One place where I read the spec text rather than copying cmark

A code span's content has its line endings replaced by spaces, and then one
space is stripped from each end "if the resulting string both begins and ends
with a space character, but does not consist entirely of space characters".
I evaluate "consists entirely of space characters" on the content *after* the
newline substitution, which is what "the resulting string" says.  The
reference implementation's `S_normalize_code` evaluates it against a buffer it
is rewriting in place, so for an input whose content is a bare newline it is
reading the already-substituted byte, and for one that collapsed a CRLF it is
reading the original.  The two agree on every example in the suite and on
every input I could construct; the difference is recorded here rather than
left as a silent choice.

### Known ceilings, marked in the source

`grep -n 'shortcut:' cmark.c` lists these.  Each is a deliberate
simplification with a named upgrade path, not an oversight.

| where | ceiling | upgrade |
| --- | --- | --- |
| `handle_backticks` | the search for a closing backtick run is linear from the opening one, so a line of *n* backticks is O(n^2) | cache backtick positions, as cmark does |
| `ref_find` | linear scan of the reference map, so a document with *n* definitions and *n* uses is O(n^2) | sorted array or hash |

20000 unpaired backticks on one line are in the test suite and finish in
milliseconds, so neither has been worth fixing.

## How it works

Two passes, which is forced by one rule: **a link reference definition may
appear after the text that uses it**, so references cannot be resolved while
blocks are still being read.

**Pass one, line by line, builds the block tree.**  For each line:
`check_open_blocks` walks the chain of currently open containers from the
document down and asks each whether this line continues it, consuming its
prefix as it goes; `open_new_blocks` then looks for new block starts at the
cursor; `add_text_to_container` appends what is left to the deepest block that
accepts lines, or creates a paragraph for it.  Closing a block runs
`finalize`, which is where a code block gets its info string, a list decides
whether it is tight, and a paragraph gives up its leading reference
definitions.

The cursor is a byte offset *and* a display column, because indentation is
measured in columns and a tab can be consumed halfway.  That half-consumed tab
is recorded and written back out as spaces, which is the only reason the Tabs
section passes.

**Pass two walks the finished tree and parses inlines.**  A `Subject` holds
one block's accumulated text plus two stacks: brackets, for links and images
whose extent is only known when the `]` arrives, and delimiters, for emphasis.
`process_emphasis` is the spec's own algorithm, left- and right-flanking plus
the rule of 3 plus the `openers_bottom` memo that keeps it linear.

**The renderer** is a recursive walk with one trick, `cr()`, which emits a
newline only if the output does not already end with one.  That is how the
expected HTML's exact newline placement falls out without special cases.

### The parts that are genuinely hard

Four things in CommonMark resist being written from a reading of the spec.
All four are passing, and all four took a transcription of the published
algorithm rather than an interpretation of the prose:

**Emphasis** is the worst, 132 examples deep, and the reason is that the
answer is not derivable from what looks right.  A delimiter run's flanking is
decided by the Unicode category of the characters on either side; `_` gets
extra restrictions so intraword underscores do nothing; and the "rule of 3"
exists purely so that `*foo**bar**baz*` is one `<em>` containing a `<strong>`
rather than three spans.  Nesting then falls out of leaving leftover
characters on the stack: `***foo***` consumes two delimiters from each end
into a `<strong>`, and the survivors pair into an `<em>` around it.

**List item continuation and lazy continuation** are two different rules that
look like one.  An item continues while the line is indented at least
`marker_offset + padding` columns, where padding is *not* the marker length: a
marker followed by five or more spaces, or by nothing, takes exactly one space
of padding and the rest is content, which is how `-     foo` becomes an item
containing an indented code block.  Lazy continuation is separate and lives in
`add_text_to_container`: if the deepest open block is a paragraph and every
container above it matched, the line joins that paragraph even though its
prefix is missing.  Those two clauses between them decide whether `> a` / `b`
is one paragraph or two blocks.

**Link reference definitions** force the two-pass shape, and they have one
edge the prose buries: a paragraph consisting *only* of definitions has
nothing left to underline, so `[foo]: /url` followed by `===` is a paragraph
containing `===`, not a heading -- while `[foo]: /url` / `bar` / `===` is an
h1 containing `bar`.  Getting that right meant moving reference stripping out
of `finalize` into a helper the setext branch calls first.

**HTML blocks** are seven start conditions and five end conditions that do not
generalise, so they are transcribed one by one.  The traps: types 1 to 5
swallow blank lines while 6 and 7 are terminated by one; type 7 cannot
interrupt a paragraph (otherwise a wrapped paragraph whose second line starts
with a tag turns into raw HTML); and the 62-name list for type 6 contains both
`p` and `param`, so a name match that fails its terminator check has to keep
scanning the list rather than fail.

**Tight versus loose lists** is a property of the whole list, not of any line,
decided at `finalize`: loose if any item is followed by a blank line or any
item's own children are separated by one.  The renderer then drops `<p>` tags
inside a tight list -- but only for a paragraph whose *grandparent* is the
list, so a paragraph inside a block quote inside a tight item keeps its tags.

## Oracles

Ranked by how little we control them.

1. **The CommonMark specification's own conformance suite**, from
   <https://spec.commonmark.org/0.31.2/spec.json>.  652 (markdown, html)
   pairs.  Vendored gzipped as `t/commonmark-spec-0.31.2.json.gz` (18kB, 140kB
   uncompressed) with its licence in `t/commonmark-spec-LICENSE`, so
   `make test` needs no network.  See *Provenance* below.
2. **The sanitizers.**  `make test-asan` runs all 652 examples plus the depth
   and hostile-input sections under `-fsanitize=address,undefined` with
   `detect_leaks=1`, `halt_on_error=1` and **`exitcode=86`**.  Not a
   correctness oracle, and the only authority on memory safety that is not
   our own opinion.  Verified live: a deliberate heap overread in
   `escape_html` is reported by ASan with a line number, and deleting the
   `free` in `pop_bracket` is reported by LeakSanitizer as 48 bytes.

   `exitcode=86` is load-bearing and was missing. A sanitizer abort exits 1
   by default, and `t/run.sh` treats exit 1 as a **correct refusal** --
   `rejects ... (exit 1)` for the five depth cases, and `exit 0 or 1` for the
   urandom, NUL, non-ASCII and ill-formed-UTF-8 cases. Those are exactly the
   inputs this target exists to run under a sanitizer, stderr is discarded
   there, so a heap error in any of them was scored as the behaviour being
   tested and the report was never seen.

   Measured, with a heap overread planted on the depth path:

   ```
   without exitcode=86:  22 passed, 0 failed
   with    exitcode=86:  19 passed, 3 failed
   ```

   `inflate` found this collision the hard way and `derx` and `gitobj` carry
   the fix; `cmark` was the one that did not.
3. **CPython's `unicodedata`, `html.entities` and `str.casefold`**, which
   generated the three tables.  Not an oracle for behaviour, but the authority
   for the data lives there rather than in this repo, and the one-liners that
   regenerate it are in `PLAN.md`.
4. **Ourselves, and labelled as such.**  The CLI contract, the depth limit and
   the hostile-input smoke tests are our own tests of our own choices.  They
   are the four non-spec sections of `t/run.sh` output.

## Provenance and licences

`t/commonmark-spec-0.31.2.json.gz` is `spec.json` for CommonMark 0.31.2,
fetched from <https://spec.commonmark.org/0.31.2/spec.json>, gzipped and
otherwise byte-exact.  The specification is Copyright (C) 2014-16 John
MacFarlane and released under **CC-BY-SA 4.0**; the full notice shipped with
the spec repository is in `t/commonmark-spec-LICENSE`.

The data is deliberately **not** ASCII-swept.  It contains smart quotes,
ellipsis characters, ill-formed UTF-8 byte sequences and significant tabs
*as test input* -- those characters are the test.  An ASCII sweep over it
would delete the tests it was trying to tidy.

`cmark.c` embeds three tables generated from CPython's standard library (the
WHATWG named character references, the Unicode punctuation categories, and the
Unicode case foldings).  The generating one-liners are recorded in `PLAN.md`
so the tables are reproducible rather than mysterious.

This program is free software under the **GPL-3.0-or-later**; see `LICENSE`.
It shares no code with the reference `cmark` implementation, but it follows
that implementation's parsing *algorithm* closely, because the algorithm is
published in the specification's appendix and in cmark's source, and inventing
a different one would have been a worse way to spend the time than getting
this one right.

## Files

```
cmark.c                           the parser, the renderer, and three tables
Makefile                          all / test / test-asan / clean
t/run.sh                          the harness: CLI, depth, hostile input, spec
t/spec.py                         the spec runner; writes the per-section score
t/sections.expected               the pin, diffed every run
t/commonmark-spec-0.31.2.json.gz  the vendored suite
t/commonmark-spec-LICENSE         its licence
PLAN.md                           contract, invariant, oracles, commit sequence
scratch/                          per-run reports, gitignored
```

After a run, `scratch/failures.txt` holds every failing example with its
markdown, the expected HTML and the actual HTML, and `scratch/sections.txt`
holds the score that gets diffed against the pin.  Both are empty of
surprises at the moment, which is the point of keeping them.
