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
if python3 t/spec.py ./cmark t/commonmark-spec-0.31.2.json.gz "$SCRATCH" \
	| tee -a "$OUT"; then
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
else
	nok "t/spec.py ran"
fi

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")
printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
