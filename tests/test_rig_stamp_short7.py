"""push-0921/C7: the rig's EXPECTED build stamp must be the SAME fixed
7-char short hash the build embeds, always.

The build stamp embedded in every artifact is a FIXED 7-char short hash
(tools/worktree-stamp.sh, tools/make_release.sh both pin
`rev-parse --short=7`). But `git rev-parse --short HEAD` with no `=7` leaves
the length up to git -- 8 chars in this repo today -- so a rig script that
derives its EXPECTED stamp that way silently disagrees with the embedded
one and false-refuses a real run ("build stamp mismatch: expected
a4180220_09.21, got a418022_09.21"), hit on the real rig twice on
2026-09-21.

This sweeps every tools/**/*.ps1, tools/**/*.sh and
.claude/skills/build-wii/scripts/* for `rev-parse --short` and fails,
naming file:line, for any occurrence that is not pinned `--short=7`.

Standalone script, not unittest: exit 0 = every occurrence is pinned (or is
an explicitly allow-listed label-only use); exit 1 = at least one
occurrence is not, with every offending file:line printed.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

_STAMP_SHORT_RE = re.compile(r'rev-parse\s+--short(?:=(\d+))?')

# Explicit allow-list: label-only uses that build a FILENAME/slug and are
# never compared against the embedded build stamp, so an unpinned length
# cannot cause a false stamp-mismatch refusal. Not a silent skip -- each
# entry is commented with why it is safe.
_STAMP_SHORT_ALLOWED = {
    # git_stamp(): proof-upload filename label only (dirty-suffix aside),
    # not a value any rig script gates a run against.
    ('tools/upload_proof.sh', 42),
    # log "tip $(git rev-parse --short HEAD)": a diagnostic log line only --
    # the SAME script's own gate two lines down (capture_chain.sh:46) already
    # compares against `rev-parse --short=7 HEAD`, so this unpinned copy
    # never drives a stamp-mismatch decision.
    ('tools/console_capture/capture_chain.sh', 43),
}


def find_violations():
    violations = []
    roots = [ROOT / 'tools', ROOT / '.claude/skills/build-wii/scripts']
    for root in roots:
        if not root.exists():
            continue
        for suffix in ('*.ps1', '*.sh'):
            for path in sorted(root.rglob(suffix)):
                rel = path.relative_to(ROOT).as_posix()
                text = path.read_text(encoding='utf-8', errors='replace')
                for lineno, line in enumerate(text.splitlines(), start=1):
                    match = _STAMP_SHORT_RE.search(line)
                    if not match:
                        continue
                    if match.group(1) == '7':
                        continue
                    if (rel, lineno) in _STAMP_SHORT_ALLOWED:
                        continue
                    violations.append(f'{rel}:{lineno}: {line.strip()}')
    return violations


def main():
    violations = find_violations()
    if violations:
        print('FAIL rev-parse --short not pinned to =7 '
              '(false build-stamp-mismatch risk):')
        for v in violations:
            print('  ' + v)
        return 1
    print('PASS stamp-short7 sweep: every rig rev-parse --short is pinned =7')
    return 0


if __name__ == '__main__':
    sys.exit(main())
