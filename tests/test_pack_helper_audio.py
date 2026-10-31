"""Structural guard for the audio seam's TEST-ONLY symbols.

What this file used to do -- assert that substrings like "audio_registry_reload()"
appear in screen_texturepacks.c -- was blind: the greps pass just as happily with
the guard inverted, the call dead, or the whole branch unreachable (2026-09-17
DELTA review, test gap (b)). Every behavioural claim it pretended to cover is now
executed for real by tests/test_pack_helper_audio.c, which drives the production
texpacks_apply_now through screen_texturepacks_test_apply.

What remains here is the one claim no runtime test in this suite can make, because
it is about what is NOT in the shipping binary: each `*_test_*` seam these two
files expose must sit inside a CAVEWII_TESTING region, so a production build never
exports it. That is checked by parsing the conditional regions, not by looking for
a string -- inverting or deleting a guard fails this.
"""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GUARDED = {
    "source/platform/audio_music.c": {
        "audio_music_test_effective_pack",
        "audio_music_test_set_observer",
    },
    "source/game/gui/screen_texturepacks.c": {
        "screen_texturepacks_test_arm",
        "screen_texturepacks_test_apply",
        "screen_texturepacks_test_apply_audio",
        "screen_texturepacks_test_last_result",
        "screen_texturepacks_test_set_observer",
    },
    "source/platform/audio_registry.c": {
        "audio_registry_test_reload_count",
        "audio_registry_test_reload_count_reset",
        "audio_registry_test_set_observer",
    },
}

def definition_offsets(text: str, name: str) -> list[int]:
    """Offsets where `name` is DEFINED (its argument list is followed by `{`).

    Matching the brace rather than a regex over the return type keeps
    multi-line signatures and `const char*` returns working, and skips the
    prototypes, which end in `;`.
    """
    found = []
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        depth, index = 1, match.end()
        while index < len(text) and depth:
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
            index += 1
        while index < len(text) and text[index] in " \t\r\n":
            index += 1
        if index < len(text) and text[index] == "{":
            found.append(match.start())
    return found



def testing_only_spans(text: str) -> list[tuple[int, int]]:
    """Character spans covered by an `#ifdef CAVEWII_TESTING` ... `#endif`.

    Nesting is tracked so an inner #if inside the region cannot close it early.
    """
    spans, start, depth = [], None, 0
    for line in re.finditer(r"^[ \t]*#\s*(\w+)[^\n]*$", text, re.M):
        directive = line.group(1)
        if start is None:
            if directive == "ifdef" and "CAVEWII_TESTING" in line.group(0):
                start, depth = line.start(), 0
            continue
        if directive in ("if", "ifdef", "ifndef"):
            depth += 1
        elif directive == "endif":
            if depth == 0:
                spans.append((start, line.end()))
                start = None
            else:
                depth -= 1
    assert start is None, "unterminated #ifdef CAVEWII_TESTING"
    return spans


def main() -> int:
    failures = []
    for relative, names in GUARDED.items():
        text = (ROOT / relative).read_text()
        spans = testing_only_spans(text)
        for name in sorted(names):
            offsets = definition_offsets(text, name)
            if not offsets:
                failures.append("{0}: {1} is not defined".format(relative, name))
                continue
            for offset in offsets:
                if not any(lo <= offset < hi for lo, hi in spans):
                    failures.append(
                        "{0}: {1} is defined OUTSIDE #ifdef CAVEWII_TESTING -- it "
                        "would be exported from the shipping binary".format(
                            relative, name))
        # A file that lost its guard entirely must not pass by having no spans.
        if not spans:
            failures.append(relative + ": no #ifdef CAVEWII_TESTING region at all")

    if failures:
        for line in failures:
            print("FAIL " + line)
        return 1
    print("PASS every audio/packs-screen test seam is CAVEWII_TESTING-only; the "
          "behavioural claims are executed by test_pack_helper_audio.c against "
          "the production texpacks_apply_now")
    return 0


if __name__ == "__main__":
    sys.exit(main())
