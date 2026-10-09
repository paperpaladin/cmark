# cmark -- SPDX-License-Identifier: GPL-3.0-or-later
#
# t/run.sh compiles nothing itself; this builds the one binary and the script
# scores it.  -ffile-prefix-map keeps the build reproducible: without it -g
# writes $(CURDIR) into DWARF .debug_line_str.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -ffile-prefix-map=$(CURDIR)=.
ASAN     = -std=c11 -Wall -Wextra -Wpedantic -O1 -g \
	   -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-asan clean

all: cmark

cmark: cmark.c
	$(CC) $(CFLAGS) -o $@ cmark.c

test: all
	CC='$(CC)' CFLAGS='$(CFLAGS)' ./t/run.sh

# We parse untrusted text with unbounded nesting, so the recursion guard and
# every buffer in the inline parser get checked by the sanitizers, not by
# inspection.
test-asan:
	$(CC) $(ASAN) -o cmark cmark.c
	CC='$(CC)' CFLAGS='$(ASAN)' \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		ASAN_OPTIONS=detect_leaks=1 ./t/run.sh
	rm -f cmark

clean:
	rm -f cmark
	rm -rf scratch
