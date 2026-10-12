#!/bin/sh
# t/run.sh -- score ./cmark against the CommonMark spec suite, check the
# things the suite cannot reach, print "N passed, M failed".
# SPDX-License-Identifier: GPL-3.0-or-later
#
# POSIX sh.  The Makefile builds the binary; this only runs it.
set -u

cd "$(dirname "$0")/.." || exit 1

SCRATCH=./scratch
mkdir -p "$SCRATCH"
OUT=$SCRATCH/results.txt
: > "$OUT"

say()  { printf '%s\n' "$*" | tee -a "$OUT"; }
ok()   { say "ok - $*"; }
nok()  { say "not ok - $*"; }

[ -x ./cmark ] || { printf 't/run.sh: ./cmark not built\n' >&2; exit 2; }

# --- CLI contract -----------------------------------------------------------
printf '\n--- CLI\n'
if printf 'hi\n' | ./cmark > "$SCRATCH/cli.out" 2>"$SCRATCH/cli.err" \
	&& [ "$(cat "$SCRATCH/cli.out")" = '<p>hi</p>' ] \
	&& [ ! -s "$SCRATCH/cli.err" ]; then
	ok "stdin to stdout, nothing on stderr"
else
	nok "stdin to stdout"; cat "$SCRATCH/cli.err"
fi
printf 'hi\n' > "$SCRATCH/f.md"
if [ "$(./cmark "$SCRATCH/f.md")" = '<p>hi</p>' ]; then
	ok "named file argument"
else
	nok "named file argument"
fi
# $SCRATCH is never cleared between runs, so "does not exist" has to be made
# true rather than assumed.
rm -f "$SCRATCH/does-not-exist.md"
if ./cmark "$SCRATCH/does-not-exist.md" >/dev/null 2>&1; then
	nok "missing file is an error"
else
	ok "missing file is an error"
fi
# Empty input is a document with no children: no output, exit 0.
if [ -z "$(printf '' | ./cmark)" ] && printf '' | ./cmark >/dev/null 2>&1; then
	ok "empty input gives empty output, exit 0"
else
	nok "empty input gives empty output, exit 0"
fi

# --- the nesting-depth limit ------------------------------------------------
# Deeply nested containers and deeply nested inline emphasis are the two ways
# a Markdown parser dies on untrusted input, and they fail in different
# places: containers recurse in the block parser, emphasis recurses in the
# renderer and in the teardown.  The limit has to be an error return, not a
# stack overflow, so these assert the exit status exactly -- 1 is "too deep",
# 139 or 134 would be a crash and must fail the suite.
printf '\n--- nesting-depth limit\n'
too_deep() { # too_deep <python> <label>
	python3 -c "$1" | ./cmark >/dev/null 2>&1
	st=$?
	if [ "$st" -eq 1 ]; then ok "rejects $2 (exit 1)"
	else nok "rejects $2 (exit $st, wanted 1)"; fi
}
fine() { # fine <python> <label>
	python3 -c "$1" | ./cmark >/dev/null 2>&1
	st=$?
	if [ "$st" -eq 0 ]; then ok "accepts $2 (exit 0)"
	else nok "accepts $2 (exit $st, wanted 0)"; fi
}
fine     'print("> " * 100 + "x")'            '100 nested block quotes'
too_deep 'print("> " * 50000 + "x")'          '50000 nested block quotes'
too_deep 'print(">" * 200000 + "x")'          '200000 block quote markers, no spaces'
fine     'print("- " * 100 + "x")'            '100 nested list items'
too_deep 'print("- " * 50000 + "x")'          '50000 nested list items'
fine     'print("    " * 50000 + "x")'        '200000 columns of indentation (one code block, not nesting)'
# 2n delimiters around a word nest n deep, so these two straddle the limit.
fine     'print("*" * 400 + "x" + "*" * 400)' '200 nested strong emphasis'
too_deep 'print("*" * 2000 + "x" + "*" * 2000)' '1000 nested strong emphasis'
too_deep 'print("![" * 2000 + "x" + "](b)" * 2000)' '2000 nested images'
# Pathological but flat: these must render, not error.  100000 unmatched
# brackets build a 100000-entry bracket stack and no tree; 60000 asterisks
# are one delimiter run that pairs with nothing.
fine     'print("[" * 100000 + "x")'          '100000 unclosed inline brackets'
fine     'print("*" * 60000)'                 '60000 asterisks with no pairing'
fine     'print("`" * 20000)'                 '20000 backticks with no pairing'
fine     'print("a" * 1000000)'               'a 1MB single-line paragraph'

# --- hostile input ----------------------------------------------------------
# Not an oracle, a smoke test: any byte sequence must render or error, never
# crash.  Markdown has no invalid input, so exit 0 or 1 are both fine.
printf '\n--- hostile input\n'
head -c 400000 /dev/urandom > "$SCRATCH/random.bin"
./cmark "$SCRATCH/random.bin" > /dev/null 2>&1
st=$?
if [ "$st" -eq 0 ] || [ "$st" -eq 1 ]; then ok "400kB of /dev/urandom: exit $st"
else nok "400kB of /dev/urandom: exit $st"; fi
printf 'a\000b\n' > "$SCRATCH/nul.md"
./cmark "$SCRATCH/nul.md" > /dev/null 2>&1
st=$?
if [ "$st" -eq 0 ]; then ok "embedded NUL byte: exit 0"
else nok "embedded NUL byte: exit $st"; fi
printf '# \357\277\276\n*\303\251*\n' > "$SCRATCH/u.md"
if ./cmark "$SCRATCH/u.md" >/dev/null 2>&1; then ok "non-ASCII UTF-8 input"
else nok "non-ASCII UTF-8 input"; fi
printf '# \377\376 \300\257\n' > "$SCRATCH/ill.md"
if ./cmark "$SCRATCH/ill.md" >/dev/null 2>&1; then ok "ill-formed UTF-8 input"
else nok "ill-formed UTF-8 input"; fi

# --- the CommonMark spec suite ---------------------------------------------
# spec.json from the CommonMark specification, 652 (markdown, html) pairs.
# Byte-for-byte comparison, nothing normalised.  The per-section score is
# pinned in t/sections.expected and diffed, so a regression in any one section
# is a test failure even if the total happens to stay level.
printf '\n--- CommonMark 0.31.2 spec suite (t/commonmark-spec-0.31.2.json.gz)\n'
# `if python3 ... | tee; then` tested tee's exit status, not spec.py's, and tee
# all but always succeeds. So a spec.py that died took the true branch, and
# since $SCRATCH is a fixed directory that is never cleared, the pin below
# diffed the *previous* run's sections.txt and passed. A spec.py crashing on
# import scored "ok - per-section score matches t/sections.expected" with the
# whole 652-example spec suite not run. Take the status from spec.py itself,
# and delete the file first so a stale one cannot stand in for a fresh one.
rm -f "$SCRATCH/sections.txt"
python3 t/spec.py ./cmark t/commonmark-spec-0.31.2.json.gz "$SCRATCH" \
	> "$SCRATCH/spec.out" 2>&1
specrc=$?
cat "$SCRATCH/spec.out" | tee -a "$OUT"
if [ "$specrc" -eq 0 ] && [ -f "$SCRATCH/sections.txt" ]; then
	cat "$SCRATCH/sections.txt"
	if [ ! -f t/sections.expected ]; then
		nok "t/sections.expected missing (cp scratch/sections.txt t/)"
	elif diff -u t/sections.expected "$SCRATCH/sections.txt" \
		> "$SCRATCH/sections.diff" 2>&1; then
		ok "per-section score matches t/sections.expected"
	else
		nok "per-section score moved"
		cat "$SCRATCH/sections.diff"
	fi
elif [ "$specrc" -ne 0 ]; then
	nok "t/spec.py ran" "exited $specrc"
else
	nok "t/spec.py ran" "wrote no $SCRATCH/sections.txt"
fi

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")
printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
