"""In-game Beta importer end-to-end: the pack the Wii writes equals the PC
Pack Helper's for the same jar.

A fixture b1.7.3-style jar is GENERATED here (seeded noise PNGs at every
canonical size, a .class file, an unused image -- never Mojang files), in two
zip flavours: ordinary stored/deflated entries and streamed entries with data
descriptors. For each, tools/pack_helper core.prepare() builds the helper's
pack and tests/beta_import_probe runs the production importer
(source/platform/beta_import*.c) inside a fake SD root. Required:
  * same file set (manifest aside);
  * jar textures and pointer.png byte-identical; gui_2.png and anim.png
    identical after decode (encoder differs: Pillow vs lodepng);
  * the importer's .cavewii-pack.json passes the helper's own
    _verify_prepared (so the helper can later replace an in-game import);
  * the 55-texture table equals the helper's client_jar_textures;
  * async API: one start at a time, selector called once with
    "texturepacks/Beta-1.7.3" after the swap; cancel leaves nothing;
  * refusals (missing texture, modern jar, HD texture, corrupt entry, not a
    client jar) install nothing and keep the previous pack.

Usage: python -B tests/test_beta_import.py --probe <beta_import_probe>
"""

from __future__ import annotations

import argparse
import io
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image  # noqa: E402

from tools.pack_helper import core  # noqa: E402

PACK = "Beta-1.7.3"           # folder (no space)
PACK_DISPLAY = "Beta 1.7.3"   # manifest name the list shows
FAILURES: list[str] = []


def check(cond: bool, what: str) -> None:
    if not cond:
        FAILURES.append(what)
        print("FAIL:", what)


def conversion_map() -> dict:
    return json.loads((ROOT / "tools/pack_helper/conversion_map.json").read_text())


def noise_png(w: int, h: int, seed: int, *, terrain=False) -> bytes:
    rng = random.Random(seed)
    img = Image.new("RGBA", (w, h))
    img.putdata([(rng.randrange(256), rng.randrange(256), rng.randrange(256),
                  rng.choice((0, 255, 255, rng.randrange(256))))
                 for _ in range(w * h)])
    if terrain:
        # The desktop scene map requires solid Nether terrain to be opaque.
        # Keep random alpha everywhere else (including mushroom cutouts), so
        # the original alpha-conversion coverage remains in this fixture.
        for cx, cy in ((7, 6), (8, 6), (9, 6)):
            box = (cx * w // 16, cy * h // 16, (cx + 1) * w // 16, (cy + 1) * h // 16)
            tile = img.crop(box)
            tile.putalpha(255)
            img.paste(tile, box)
    out = io.BytesIO()
    img.save(out, format="PNG")
    return out.getvalue()


def jar_entries(seed: int, *, drop=None, hd=None, modern=False,
                no_class=False) -> list[tuple[str, bytes]]:
    cmap = conversion_map()["assets"]
    entries = []
    if not no_class:
        entries.append(("net/minecraft/client/Minecraft.class", b"\xca\xfe\xba\xbe" + bytes(64)))
    for i, dest in enumerate(core.client_jar_textures({"assets": cmap})):
        if dest == drop:
            continue
        w, h = cmap[dest]["canonical_size"]
        if dest == hd:
            w, h = w * 2, h * 2
        name = "gui/container.png" if dest == "gui/chest.png" else dest
        entries.append((name, noise_png(w, h, seed * 1000 + i, terrain=dest == "terrain.png")))
    entries.append(("title/mclogo.png", noise_png(32, 32, seed)))  # unused image
    if drop != "title/splashes.txt":
        entries.append(("title/splashes.txt", b"Synthetic splash!\r\nAnother \xff splash\n"))
    if modern:
        entries.append(("assets/minecraft/textures/block/stone.png", noise_png(16, 16, 1)))
    return entries


class _Unseekable(io.RawIOBase):
    def __init__(self, sink):
        self.sink = sink

    def writable(self):
        return True

    def write(self, b):
        self.sink.write(b)
        return len(b)


def write_jar(path: Path, entries, *, descriptors: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as raw:
        target = _Unseekable(raw) if descriptors else raw
        with zipfile.ZipFile(target, "w") as zf:
            for i, (name, data) in enumerate(entries):
                method = zipfile.ZIP_STORED if i % 3 == 0 else zipfile.ZIP_DEFLATED
                zf.writestr(zipfile.ZipInfo(name, (2011, 7, 8, 0, 0, 0)), data,
                            compress_type=method)
    if descriptors:
        with zipfile.ZipFile(path) as zf:
            check(all(i.flag_bits & 8 for i in zf.infolist()),
                  "fixture: streamed jar has data descriptors")


def make_root(tmp: Path, name: str) -> Path:
    root = tmp / name
    (root / "assets" / "beta-base").mkdir(parents=True)
    (root / "beta").mkdir()
    for rel in ("gui_2.png", "pointer.png", "beta-base/anim.png"):
        shutil.copyfile(ROOT / "assets" / rel, root / "assets" / rel)
    return root


def run(probe: str, mode: str, root: Path) -> tuple[int, str]:
    proc = subprocess.run([probe, mode, str(root)], capture_output=True, text=True,
                          timeout=300)
    return proc.returncode, proc.stdout + proc.stderr


def decoded(path: Path) -> bytes:
    with Image.open(path) as img:
        return img.convert("RGBA").tobytes()


def files_of(folder: Path) -> set[str]:
    return {p.relative_to(folder).as_posix() for p in folder.rglob("*") if p.is_file()}


SOUND_TOOL: str = ""  # --sound-tool: test_beta_import_sound (coder B), --tool form
SOUND_FIXTURES = ROOT / "tests" / "fixtures" / "beta_import_sound"


def sound_transcoder(tmp: Path) -> Path:
    """A transcoder the Pack Helper can exec: the in-game sound phase's own
    tool form (tools/sound_transcode's main() path, same compiler as the
    engine), so helper-vs-engine sound bytes are compared without a stale
    prebuilt sound_transcode binary in the way (see test_beta_import_sound.py)."""
    if os.name == "nt":
        w = tmp / "bi_tool.cmd"
        w.write_text('@"%s" --tool %%1 %%2\r\n' % SOUND_TOOL)
    else:
        w = tmp / "bi_tool.sh"
        w.write_text('#!/bin/sh\nexec "%s" --tool "$1" "$2"\n' % SOUND_TOOL)
        w.chmod(0o755)
    return w


def write_resources(res: Path) -> None:
    """A tiny .minecraft/resources: ffmpeg-made CC0 tones under Beta's
    sound/ + newsound/ layout (never Mojang files)."""
    for sub, name, fx in (("sound/step", "grass1.ogg", "tone_mono_44k.ogg"),
                          ("sound/step", "grass2.ogg", "tone_stereo_22k.ogg"),
                          ("newsound/step", "grass1.ogg", "tone_mono_44k.ogg"),  # dedup
                          ("newsound/mob/zombiepig", "zpig1.ogg", "tone_stereo_48k.ogg"),
                          ("sound/random", "click.wav", "tone_mono_11k.wav"),
                          ("music", "calm1.ogg", "tone_stereo_22k.ogg"),
                          ("newmusic", "calm1.ogg", "tone_mono_44k.ogg"),  # same name: skipped
                          ("newmusic", "hal1.ogg", "tone_mono_44k.ogg"),
                          ("streaming", "13.ogg", "tone_stereo_48k.ogg"),
                          ("streaming", "mall.ogg", "tone_mono_44k.ogg")):  # not a Beta record
        (res / sub).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SOUND_FIXTURES / fx, res / sub / name)


def compare_with_helper(jar: Path, ours: Path, tmp: Path, label: str,
                        resources: Path | None = None) -> None:
    kw = {}
    if resources is not None:
        kw = dict(resources_path=str(resources), transcoder=str(sound_transcoder(tmp)))
    prepared = core.prepare(str(jar), str(tmp / ("helper-" + label)), **kw)
    check(prepared["ok"], f"{label}: helper prepare ok ({prepared.get('errors')})")
    if not prepared["ok"]:
        return
    helper = Path(prepared["path"])
    # The desktop importer now supplies a precomposed Nether illustration for
    # Wii. The in-game importer intentionally cannot run this offline composer.
    scene_files = {"gui/nether-background.png", "gui/nether-background.md"}
    check(scene_files <= files_of(helper), f"{label}: desktop Nether scene present")
    hset = files_of(helper) - {core.MANIFEST_NAME} - scene_files
    oset = files_of(ours) - {core.MANIFEST_NAME}
    check(hset == oset, f"{label}: same file set (helper-only {sorted(hset - oset)}, "
                        f"ours-only {sorted(oset - hset)})")
    for rel in sorted(hset & oset):
        a, b = helper / rel, ours / rel
        if rel in ("gui_2.png", "anim.png"):
            check(decoded(a) == decoded(b), f"{label}: {rel} decoded RGBA equals helper")
        else:
            check(a.read_bytes() == b.read_bytes(), f"{label}: {rel} byte-identical")
    try:
        core._verify_prepared(ours, core._read_manifest(ours))
    except Exception as exc:  # noqa: BLE001
        check(False, f"{label}: helper _verify_prepared accepts our manifest ({exc})")
    manifest = json.loads((ours / core.MANIFEST_NAME).read_text())
    check(manifest["name"] == PACK_DISPLAY and manifest["schema_version"] == core.SCHEMA_VERSION,
          f"{label}: manifest name/schema")


def test_table(probe: str) -> None:
    out = subprocess.run([probe, "table"], capture_output=True, text=True).stdout.split("\n")
    rows = [line.split(" ") for line in out if line.strip()]
    cmap = conversion_map()
    want = core.client_jar_textures(cmap)
    check([r[0] for r in rows] == want, "table: 55 dests equal helper client_jar_textures")
    for r in rows:
        spec = cmap["assets"][r[0]]
        names = [n for n in r[1:3] if n != "-"]
        check(names == spec["source_names"], f"table: {r[0]} source names")
        check([int(r[3]), int(r[4])] == spec["canonical_size"], f"table: {r[0]} size")


def test_import(probe: str, tmp: Path) -> None:
    for label, descriptors in (("plain", False), ("descriptor", True),
                               ("no-splashes", False)):
        root = make_root(tmp, label)
        jar = root / "beta" / "b1.7.3.jar"
        write_jar(jar, jar_entries(7 if descriptors else 3,
                                 drop="title/splashes.txt" if label == "no-splashes" else None),
                  descriptors=descriptors)
        code, out = run(probe, "sync", root)
        check(code == 0, f"{label}: sync import DONE ({out.strip()})")
        pack = root / "texturepacks" / PACK
        splash = pack / "title/splashes.txt"
        if label == "no-splashes":
            check(not splash.exists(), "missing optional splashes: no file, import succeeds")
        else:
            with zipfile.ZipFile(jar) as archive:
                check(splash.is_file() and splash.read_bytes() == archive.read("title/splashes.txt"),
                      f"{label}: C importer preserves jar splash bytes")
        compare_with_helper(jar, pack, tmp, label)
        check(not (root / "texturepacks" / ".beta-import-tmp").exists()
              and not (root / "texturepacks" / ".beta-import-old").exists(),
              f"{label}: no leftovers")

    # Async API + re-import replaces in place + selection hand-off + any *.jar.
    # Earlier imports on the card: an older helper's lowercase "beta-1.7.3"
    # (replaced whole, never merged) and a helper "Beta 1.7.3" (retired after
    # the new pack is selected, deleted by the next import's recovery).
    root = make_root(tmp, "async")
    lower = root / "texturepacks" / "beta-1.7.3"
    lower.mkdir(parents=True)
    (lower / "stale-only-in-old.png").write_bytes(b"x")
    (lower / core.MANIFEST_NAME).write_text("{}")
    legacy = root / "texturepacks" / "Beta 1.7.3"
    legacy.mkdir()
    (legacy / core.MANIFEST_NAME).write_text("{}")
    write_jar(root / "beta" / "My Beta Copy.jar", jar_entries(11), descriptors=False)
    code, out = run(probe, "async", root)
    check(code == 0 and "SELECTED texturepacks/Beta-1.7.3" in out,
          f"async: done + selector called once ({out.strip()})")
    check("For sounds, copy" in out, "async: no resources -> message says how to add sounds")
    first = (root / "texturepacks" / PACK / "terrain.png").read_bytes()
    check(not (root / "texturepacks" / PACK / "stale-only-in-old.png").exists(),
          "lowercase beta-1.7.3 replaced, not merged into")
    check(not legacy.exists(), "legacy 'Beta 1.7.3' retired after selection")
    visible = sorted(p.name for p in (root / "texturepacks").iterdir()
                     if not p.name.startswith("."))
    check(visible == [PACK], f"one visible Beta folder, spelled Beta-1.7.3 ({visible})")
    write_jar(root / "beta" / "My Beta Copy.jar", jar_entries(12), descriptors=False)
    (root / "beta" / "resources" / "sound").mkdir(parents=True)
    code, out = run(probe, "async", root)
    check(code == 0, f"async re-import done ({out.strip()})")
    check("No sounds found" in out and "SOUNDS 0 0" in out,
          f"async: empty resources -> 'No sounds found', none counted ({out.strip()})")
    packs = sorted(p.name for p in (root / "texturepacks").iterdir())
    check(packs == [PACK], f"re-import replaced in place, no extra folders ({packs})")
    check((root / "texturepacks" / PACK / "terrain.png").read_bytes() != first,
          "re-import wrote the new jar's art")

    # Sounds (coder B): resources with Beta-layout tones -> the pack carries
    # the helper's exact sound files too (strict transcode, dedup, naming).
    if SOUND_TOOL:
        root = make_root(tmp, "sounds")
        jar = root / "beta" / "b1.7.3.jar"
        write_jar(jar, jar_entries(5), descriptors=False)
        write_resources(root / "beta" / "resources")
        code, out = run(probe, "async", root)
        check(code == 0 and "textures and sounds imported" in out and "SOUNDS 1 7" in out,
              f"sounds: done, 4 sound + 2 music + 1 record files counted ({out.strip()})")
        pack = root / "texturepacks" / PACK
        wavs = sorted(p.relative_to(pack).as_posix() for p in pack.rglob("*.wav"))
        check(wavs == ["sound/mob/zombiepig/zpig1.wav", "sound/random/click1.wav",
                       "sound/step/grass1.wav", "sound/step/grass2.wav"],
              f"sounds: helper layout (dedup of newsound/grass1, bare click -> click1) ({wavs})")
        check(not list((pack / "sound").rglob("*.ogg")), "sounds: nothing raw-copied")
        oggs = sorted(p.relative_to(pack).as_posix() for p in pack.rglob("*.ogg"))
        check(oggs == ["music/calm1.ogg", "music/hal1.ogg", "records/13.ogg"],
              f"music/records verbatim, helper names ({oggs})")
        check((pack / "music" / "calm1.ogg").read_bytes()
              == (SOUND_FIXTURES / "tone_stereo_22k.ogg").read_bytes(), "music/ wins over newmusic/")
        compare_with_helper(jar, pack, tmp, "sounds", resources=root / "beta" / "resources")

    # Cancel: previous pack untouched, no staging.
    before = (root / "texturepacks" / PACK / "terrain.png").read_bytes()
    code, out = run(probe, "cancel", root)
    check(code == 0, f"cancel -> CANCELLED ({out.strip()})")
    check((root / "texturepacks" / PACK / "terrain.png").read_bytes() == before,
          "cancel: previous pack untouched")
    check(not (root / "texturepacks" / ".beta-import-tmp").exists(), "cancel: staging removed")


def test_refusals(probe: str, tmp: Path) -> None:
    cases = {
        "missing": (dict(drop="mob/pig.png"), "missing"),
        "modern": (dict(modern=True), "(it's a newer version)"),
        "hd": (dict(hd="terrain.png"), "Pack Helper"),
        "noclass": (dict(no_class=True), "not the Beta 1.7.3 jar"),
        "corrupt": ({}, "damaged"),
    }
    for label, (kwargs, needle) in cases.items():
        root = make_root(tmp, "refuse-" + label)
        prev = root / "texturepacks" / PACK
        prev.mkdir(parents=True)
        (prev / "terrain.png").write_bytes(b"previous")
        (prev / core.MANIFEST_NAME).write_text("{}")
        jar = root / "beta" / "b1.7.3.jar"
        write_jar(jar, jar_entries(5, **kwargs), descriptors=False)
        if label == "corrupt":
            data = bytearray(jar.read_bytes())
            with zipfile.ZipFile(jar) as zf:
                info = zf.getinfo("terrain.png")
            start = info.header_offset + 30 + len(info.filename) + 40
            data[start] ^= 0xFF
            jar.write_bytes(bytes(data))
        code, out = run(probe, "sync", root)
        check(code != 0 and needle in out, f"refuse {label}: FAILED with '{needle}' ({out.strip()})")
        check((prev / "terrain.png").read_bytes() == b"previous",
              f"refuse {label}: previous pack untouched")
        check(sorted(p.name for p in (root / "texturepacks").iterdir()) == [PACK],
              f"refuse {label}: nothing else left in texturepacks")

    root = make_root(tmp, "nojar")
    code, out = run(probe, "async", root)
    check(code != 0 and "not available" in out, "no jar: beta_import_available false")


def main() -> int:
    # The helper otherwise auto-finds a real .minecraft on this PC and adds its
    # sounds; this test compares textures from the generated jar only.
    import os
    os.environ[core.NO_LAUNCHER_SEARCH_ENV] = "1"
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--sound-tool", default="",
                    help="test_beta_import_sound binary (its --tool form is the helper's transcoder)")
    args = ap.parse_args()
    global SOUND_TOOL
    SOUND_TOOL = args.sound_tool
    with tempfile.TemporaryDirectory(prefix="beta-import-e2e-") as tmp_s:
        tmp = Path(tmp_s)
        test_table(args.probe)
        test_import(args.probe, tmp)
        test_refusals(args.probe, tmp)
    if FAILURES:
        print(f"test_beta_import.py: {len(FAILURES)} failure(s)")
        return 1
    print("test_beta_import.py: PASS (helper-equal pack, plain + data-descriptor jars, "
          "async/selector/cancel/re-import, refusals)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
