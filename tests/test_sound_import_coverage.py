#!/usr/bin/env python3
"""Sound-coverage safety net for tools/import_owned_beta_assets.py.

Every sound key the engine can play (AUDIO_SOUND_TABLE in
source/platform/audio_sound_table.h -- the engine plays ONLY by enum id from
that table; no string-literal sound keys exist elsewhere in source/) must be
searched by the importer, OR be listed in a documented allowlist with a
reason. The Pack Helper (tools/pack_helper/conversion_map.json) carries no
sound mapping: build_resources.py freezes the importer's own key list, which
is asserted below.

Synthetic data only: a generated resources tree of tiny placeholder files, no
Mojang bytes. Run as a script from anywhere (like the other host tooling
tests); prints "PASS test_sound_import_coverage".
"""

from __future__ import annotations

import importlib.util
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TABLE_H = REPO / "source" / "platform" / "audio_sound_table.h"

# Keys the table carries that Beta 1.7.3 does NOT have in its sound pool.
# Each needs a reason; stale entries (key no longer in the table) fail.
NOT_IN_BETA_ALLOWLIST = {
    "minecart.base": "deliberate non-Beta add-on; no 1.7.3 asset exists; "
                     "filled only by --modern-minecraft (MODERN_SOUND_SOURCE_KEYS)",
    "minecart.inside": "same as minecart.base",
}
# Project-original UI sounds: bundled in assets/sound/menu, never from a jar.
BUNDLED_ONLY_PREFIXES = {
    "menu.": "project-original GPL samples bundled under assets/sound/menu",
}


def load_tool():
    spec = importlib.util.spec_from_file_location(
        "owned_importer_cov", REPO / "tools" / "import_owned_beta_assets.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def table_keys_independent():
    """Independent regex parse of the table (does not use the tool's parser)."""
    text = TABLE_H.read_text(encoding="utf-8")
    start = text.index("#define AUDIO_SOUND_TABLE(X)")
    end = text.index("/* Registry ids, derived FROM the table")
    body = re.sub(r"/\*.*?\*/", "", text[start:end], flags=re.S)
    return re.findall(r'X\(\s*SOUND_[A-Z0-9_]+\s*,\s*"([^"]+)"\s*\)', body)


def test_table_parsers_agree():
    tool = load_tool()
    mine = table_keys_independent()
    theirs, _ = tool.sound_keys_and_max_variants()
    assert mine, "no keys parsed from the sound table"
    assert mine == list(theirs), (
        "importer key list differs from the engine table: only-table={0} "
        "only-tool={1}".format(sorted(set(mine) - set(theirs)),
                               sorted(set(theirs) - set(mine))))
    assert len(set(mine)) == len(mine), "duplicate key in sound table"
    print("PASS: importer key list == engine table ({0} keys)".format(len(mine)))


def test_allowlist_is_not_stale_and_documented():
    keys = set(table_keys_independent())
    tool = load_tool()
    for k, why in NOT_IN_BETA_ALLOWLIST.items():
        assert k in keys, "stale allowlist key {0}".format(k)
        assert why.strip(), "allowlist key {0} has no reason".format(k)
    for p, why in BUNDLED_ONLY_PREFIXES.items():
        assert why.strip()
        assert any(k.startswith(p) for k in keys), "stale prefix " + p
    # Modern add-on source keys must be real table keys and allowlisted.
    for k in tool.MODERN_SOUND_SOURCE_KEYS:
        assert k in keys, "MODERN_SOUND_SOURCE_KEYS has non-table key " + k
        assert k in NOT_IN_BETA_ALLOWLIST, k + " modern-only but undocumented"
    print("PASS: allowlist entries live, documented, modern keys covered")


def test_every_key_is_searched_and_lands_on_engine_path():
    """Plant one variant file per key at the exact Beta pool path, ALL keys at
    once (so prefix-sharing keys like mob.cow/mob.cowhurt or mob.slime/
    mob.slimeattack would cross-contaminate if the variant pattern leaked),
    run the real process_sounds, and require each key to copy exactly its own
    file to the path the engine scans (audio_sound_table_subpath)."""
    tool = load_tool()
    keys = table_keys_independent()
    gaps = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        res, out = td / "res", td / "out"
        for k in keys:
            parts = k.split(".")
            d = res / "sound" / "/".join(parts[:-1]) if len(parts) > 1 else res / "sound"
            d.mkdir(parents=True, exist_ok=True)
            (d / "{0}1.ogg".format(parts[-1])).write_bytes(
                b"synthetic-placeholder:" + k.encode())
        results = tool.process_sounds(res, out, False, None)
        by_key = {e["key"]: e for e in results}
        for k in keys:
            e = by_key.get(k)
            if e is None or e.get("status") != "copied":
                gaps.append((k, e and e.get("status")))
                continue
            vs = [v for v in e["variants"] if v["status"] in ("copied", "transcoded")]
            if len(vs) != 1:
                gaps.append((k, "variants={0}".format(len(vs))))
                continue
            if Path(vs[0]["source"]).read_bytes() != b"synthetic-placeholder:" + k.encode():
                gaps.append((k, "picked another key's file"))
                continue
            rel = "sound/" + k.replace(".", "/")
            dest = vs[0]["dest"]
            if not re.fullmatch(re.escape(rel) + r"\d*\.(ogg|wav)", dest):
                gaps.append((k, "dest {0} != engine scan {1}".format(dest, rel)))
            elif not (out / dest).is_file():
                gaps.append((k, "dest not written"))
    assert not gaps, "importer never searches/places these engine keys: {0}".format(gaps)
    print("PASS: all {0} engine sound keys searched and placed on the engine path".format(len(keys)))


def test_pack_helper_freezes_importer_keys():
    src = (REPO / "tools" / "pack_helper" / "build_resources.py").read_text(encoding="utf-8")
    assert "importer.sound_keys_and_max_variants()" in src and "sound_keys=keys" in src, \
        "pack helper no longer derives sound_keys from the importer"
    cmap = (REPO / "tools" / "pack_helper" / "conversion_map.json").read_text(encoding="utf-8")
    assert "sound" not in cmap.lower(), \
        "conversion_map.json gained a sound mapping: extend this test to cover it"
    print("PASS: Pack Helper sound keys come from the importer; no separate map")


def test_no_string_literal_sound_keys_outside_table():
    """Engine plays by enum id only. A play call with a literal key would
    bypass the table (and this test)."""
    pat = re.compile(r'(audio_play\w*|server_local_emit_sound\w*|play_sound\w*)\s*\(\s*"')
    bad = []
    for p in (REPO / "source").rglob("*.[ch]"):
        text = p.read_text(encoding="utf-8", errors="replace")
        for n, line in enumerate(text.splitlines(), 1):
            if pat.search(line):
                bad.append("{0}:{1}".format(p.relative_to(REPO), n))
    assert not bad, "literal-key sound calls bypass the table: {0}".format(bad)
    print("PASS: no literal-key sound calls outside the table")


def main():
    test_table_parsers_agree()
    test_allowlist_is_not_stale_and_documented()
    test_every_key_is_searched_and_lands_on_engine_path()
    test_pack_helper_freezes_importer_keys()
    test_no_string_literal_sound_keys_outside_table()
    print("PASS test_sound_import_coverage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
