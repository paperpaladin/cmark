# cmark -- SPDX-License-Identifier: GPL-3.0-or-later
#
# t/run.sh compiles nothing itself; this builds the one binary and the script
# scores it.  -ffile-prefix-map keeps the build reproducible: without it -g
# writes $(CURDIR) into DWARF .debug_line_str.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -ffile-prefix-map=$(CURDIR)=.
ASAN     = -std=c11 -Wall -Wextra -Wpedantic -O1 -g \
	   -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-asan overread clean

all: cmark

cmark: cmark.c
	$(CC) $(CFLAGS) -o $@ cmark.c

test: all
	CC='$(CC)' CFLAGS='$(CFLAGS)' ./t/run.sh

# We parse untrusted text with unbounded nesting, so the recursion guard and
# every buffer in the inline parser get checked by the sanitizers, not by
# inspection.
# exitcode=86 is load-bearing, not decoration.
#
# A sanitizer abort exits 1 by default, and t/run.sh treats exit 1 as a
# CORRECT REFUSAL: `rejects $2 (exit 1)` for the five depth cases, and
# `exit 0 or 1` for the urandom, NUL, non-ASCII and ill-formed-UTF-8 cases.
# Those are precisely the inputs this target exists to run under a
# sanitizer, and stderr is discarded there, so a heap error in any of them
# was scored as the behaviour being tested and the report was never seen.
#
# 86 is outside every status the suite expects, so a sanitizer abort becomes
# a failure instead of a pass. Copied from inflate, which found the
# collision the hard way; derx and gitobj carry it too.
test-asan: overread
	$(CC) $(ASAN) -o cmark cmark.c
	CC='$(CC)' CFLAGS='$(ASAN)' \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:exitcode=86 \
		ASAN_OPTIONS=detect_leaks=1:exitcode=86 ./t/run.sh
	rm -f cmark

# A buffer larger than its data turns the sanitizer off.  bgrow() doubles and
# never shrinks, so main()'s read buffer used to be up to twice the size of
# the document, and a read one byte past the end of the input landed inside a
# live allocation where ASan is happy to let anybody read.  Every clean
# sanitizer run this parser ever had was clean over a bigger region than the
# document it was given.
#
# This target is the interlock.  It compiles a deliberate read of
# src[srclen + 1] (src[srclen] is the NUL bput() maintains, so + 1 is the
# first byte that is out of bounds) and REQUIRES ASan to report it.  Measured
# with the same probe, same compiler, same flags:
#
#   before the bfit() call in main():  exit 0, nothing on stderr, "<p>hello
#                                      <strong>world</strong></p>" on stdout
#   after it:                          heap-buffer-overflow, READ of size 1,
#                                      "0 bytes after 17-byte region"
#
# So removing bfit() turns this red instead of quietly turning the sanitizer
# off, which is the failure mode the gate exists for.
overread:
	@mkdir -p scratch
	$(CC) $(ASAN) -DCMARK_PLANT_OVERREAD -o scratch/cmark-overread cmark.c
	@printf 'hello **world**\n' | \
		ASAN_OPTIONS=detect_leaks=1:exitcode=86 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:exitcode=86 \
		./scratch/cmark-overread > scratch/overread.out \
		2> scratch/overread.err; \
	st=$$?; \
	if [ "$$st" -eq 86 ] && \
	   grep -q 'heap-buffer-overflow' scratch/overread.err; then \
		printf 'ok - overread: ASan reports the planted read past the input (exit %s)\n' "$$st"; \
		grep -m1 'SUMMARY: AddressSanitizer' scratch/overread.err; \
	else \
		printf 'not ok - overread: the planted read of src[srclen + 1] was NOT reported (exit %s)\n' "$$st"; \
		printf '  the input buffer is larger than the input, so ASan cannot see past it;\n'; \
		printf '  restore bfit(&in) after the read loop in main() -- see bfit() in cmark.c\n'; \
		printf '  stdout was: '; cat scratch/overread.out; \
		printf '  stderr was %s bytes\n' "$$(wc -c < scratch/overread.err)"; \
		head -20 scratch/overread.err; \
		rm -f scratch/cmark-overread; \
		exit 1; \
	fi
	@rm -f scratch/cmark-overread

clean:
	rm -f cmark
	rm -rf scratch
