#!/usr/bin/env python3
"""Score ./cmark against the CommonMark spec's own conformance suite.

SPDX-License-Identifier: GPL-3.0-or-later

The suite is `spec.json` from the CommonMark specification itself: roughly 650
numbered (markdown, html) pairs, one per example in the spec text, each tagged
with the section it came from.  It is vendored gzipped under t/ with its
licence beside it, so this needs no network.

Comparison is byte for byte on stdout.  Nothing is normalised: not trailing
newlines, not entity spelling, not whitespace.  A test that trims its inputs
to agree is not a test.

Usage: t/spec.py ./cmark t/commonmark-spec-0.31.2.json.gz scratch
Writes  <scratch>/sections.txt   pinned per-section score, diffed by t/run.sh
        <scratch>/failures.txt   every failing example, with both HTML strings
Exit status is 0 always; t/run.sh decides pass/fail from the diff.
"""
import gzip
import json
import subprocess
import sys


def main():
    exe, corpus, scratch = sys.argv[1], sys.argv[2], sys.argv[3]
    with gzip.open(corpus, "rb") as fh:
        cases = json.loads(fh.read().decode("utf-8"))

    order, tally = [], {}
    failures = []
    crashes = 0
    for case in cases:
        sec = case["section"]
        if sec not in tally:
            order.append(sec)
            tally[sec] = [0, 0]
        tally[sec][1] += 1
        md = case["markdown"].encode("utf-8")
        want = case["html"].encode("utf-8")
        try:
            proc = subprocess.run([exe], input=md, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, timeout=20)
            got, rc = proc.stdout, proc.returncode
        except subprocess.TimeoutExpired:
            got, rc = b"", -99
        if rc != 0:
            crashes += 1
        if got == want and rc == 0:
            tally[sec][0] += 1
        else:
            failures.append((case["example"], sec, rc, case["markdown"],
                             case["html"], got.decode("utf-8", "replace")))

    passed = sum(v[0] for v in tally.values())
    total = sum(v[1] for v in tally.values())
    with open(scratch + "/sections.txt", "w") as fh:
        for sec in order:
            got, tot = tally[sec]
            fh.write("%3d/%-3d  %s\n" % (got, tot, sec))
        fh.write("%3d/%-3d  TOTAL\n" % (passed, total))
    with open(scratch + "/failures.txt", "w") as fh:
        for ex, sec, rc, md, want, got in failures:
            fh.write("=== example %d (%s)%s\n--- markdown\n%s--- want\n%s"
                     "--- got\n%s" % (ex, sec, "" if rc == 0 else
                                      " EXIT %d" % rc, md, want, got))
            if not got.endswith("\n"):
                fh.write("\n")
    print("SPEC %d/%d examples (CommonMark 0.31.2)" % (passed, total))
    if crashes:
        print("SPEC %d examples exited nonzero" % crashes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
