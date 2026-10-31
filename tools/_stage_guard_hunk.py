#!/usr/bin/env python3
"""Filter a unified `git diff` to a subset of its hunks, preserving the file
header. Used to split entangled changes in a shared file (e.g. another session's
far-ring WIP vs. my window-click guard in source/network/server_local.c) so each
logical change can be staged and committed on its own.

Usage:
  git diff -- <file> | _stage_guard_hunk.py keep <marker>   # keep hunks containing marker
  git diff -- <file> | _stage_guard_hunk.py drop <marker>   # keep hunks NOT containing marker
  git diff -- <file> | _stage_guard_hunk.py <marker>        # legacy: keep, expect exactly 1
then: git apply --cached <patch>
"""
import sys


def main():
    if len(sys.argv) == 3:
        mode, marker = sys.argv[1], sys.argv[2]
        expect_one = False
    else:
        mode, marker, expect_one = "keep", sys.argv[1], True

    lines = sys.stdin.readlines()

    header = []
    i = 0
    while i < len(lines) and not lines[i].startswith("@@"):
        header.append(lines[i])
        i += 1

    hunks = []
    cur = None
    for line in lines[i:]:
        if line.startswith("@@"):
            if cur is not None:
                hunks.append(cur)
            cur = [line]
        elif cur is not None:
            cur.append(line)
    if cur is not None:
        hunks.append(cur)

    def has(h):
        return any(marker in l for l in h)

    kept = [h for h in hunks if (has(h) if mode == "keep" else not has(h))]
    sys.stderr.write("kept %d/%d hunks (mode=%s marker=%r)\n"
                     % (len(kept), len(hunks), mode, marker))
    if expect_one and len(kept) != 1:
        sys.stderr.write("expected exactly 1 hunk, got %d\n" % len(kept))
        sys.exit(2)
    if not kept:
        sys.stderr.write("no hunks matched -- empty patch\n")
        sys.exit(3)

    sys.stdout.write("".join(header))
    for h in kept:
        sys.stdout.write("".join(h))


if __name__ == "__main__":
    main()
