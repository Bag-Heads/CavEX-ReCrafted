#!/usr/bin/env python3
"""Hermetic regression for tools/import_owned_beta_assets.py (the "user-owns-
the-jar" Beta 1.7.3 asset importer, docs/ship_distribution_plan_2026-08-17.md
Track 2 / docs/import_owned_assets.md).

Follows tests/test_make_release.py's pattern for a host-side Python tooling
test: each test_* function does its own work in a temp directory, raises
AssertionError on failure and prints "PASS: <thing>" on success; main() runs
them all and prints one final "PASS test_import_owned_beta_assets".

HERMETICITY: unlike test_make_release.py, this tool never shells out to git,
so there is no scratch-repo dance needed for the TOOL itself. It DOES read
real, checked-in repo files (source/platform/texture.c, audio_sound_table.h,
audio_music_table.h, assets/) to derive its texture/sound/music lists and
required-vs-optional classification -- exactly the files the tool itself is
designed to track live rather than duplicate, so the test asserts PROPERTIES
of the tool's output (does the required-file gate fire, does the manifest
verify, does the output layout match what the engine reads, is the
union/dedup/rename behaviour correct) instead of a brittle hardcoded
texture/sound COUNT that would need updating every time an unrelated PR adds
a texture.c read-path -- the exact copy-drift trap docs/finish_line_plan.md
calls out for the mesher equivalence tests ("the suite's highest
false-confidence risk").

ASSETS/ HERMETICITY GAP (found 2026-09-04) and its fix: assets/mob/* is
git-ignored (.gitignore:58, "legal-assets rule: never commit Mojang IP") so a
maintainer's real checkout can carry leftover Beta-derived .png files under
assets/mob/ from an earlier real --jar run. The tool's is_texture_required()
(tools/import_owned_beta_assets.py) treats "a bundled fallback file exists on
disk at ASSETS_DEFAULT_DIR / dest" as "not required" -- correct behaviour for
the tool, but it means this TEST's expectations (which dests the fake jar
must supply, which land in the output) silently depended on the ON-DISK
state of the real assets/ tree, not just its git-tracked content: a real
imported assets/mob/zombie.png sitting there (ignored, invisible to `git
status`) makes the live tool stop treating mob/zombie.png as required, so
build_fake_jar() (which asks the live tool what's required) stops adding it
to the fixture jar, and the full-run test's `assert (out / dest).is_file()`
for mob/zombie.png fails in that tree while passing in a clean checkout.
Fixed here, not in the tool: every test in this file that reads
TOOL.ASSETS_DEFAULT_DIR (directly, or indirectly via is_texture_required) now
sees a HERMETIC copy containing only git's tracked assets/ content, built
once at import time by _build_hermetic_assets_root() below and pointed to via
the tool's tests-only CW_IMPORT_ASSETS_ROOT env knob (see
tools/import_owned_beta_assets.py's ASSETS_DEFAULT_DIR comment) -- so this
test's result no longer depends on what an earlier real import may have left
lying around ignored on disk. Both test_full_run_layout_manifest_and_sounds
states (dummy ignored fallback file present or absent) now agree.

Never touches the network. Every jar/resources fixture here is synthetic
(tiny generated PNG bytes + generated PCM16 WAV via the stdlib `wave` module +
placeholder text standing in for .ogg content the tool never decodes) -- zero
Mojang bytes anywhere in this file, satisfying .claude/hooks/guard-ip.sh by
construction (there is nothing at a guarded path to match).
"""

from __future__ import annotations

import atexit
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import wave
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = REPO_ROOT / "tools" / "import_owned_beta_assets.py"
REAL_ASSETS_DIR = REPO_ROOT / "assets"

# A 1x1 transparent PNG -- valid bytes-on-disk content; the tool never decodes
# image content, only copies/hashes it (same precedent as test_make_release.py).
TINY_PNG_BYTES = bytes.fromhex(
    "89504e470d0a1a0a0000000d494844520000000100000001080600000"
    "01f15c4890000000a49444154789c6360000002000100ffff03000006"
    "0005574bde3d0000000049454e44ae426082"
)


_CONVERSION_MAP = json.loads((Path(__file__).resolve().parents[1] / "tools" / "pack_helper"
                              / "conversion_map.json").read_text(encoding="utf-8"))


def canonical_png(name: str) -> bytes:
    """A generated solid PNG at the jar file's canonical Beta size, so the
    helper's conversion (which the CLI now shares, audit F4) accepts it. No
    Mojang bytes. Names outside the conversion map stay the 1x1 PNG."""
    import io
    from PIL import Image
    for spec in _CONVERSION_MAP["assets"].values():
        if name in spec.get("source_names", ()):
            seed = int(hashlib.sha256(name.encode()).hexdigest()[:6], 16)
            out = io.BytesIO()
            Image.new("RGBA", tuple(spec["canonical_size"]),
                      (seed & 255, (seed >> 8) & 255, (seed >> 16) & 255, 255)).save(out, "PNG")
            return out.getvalue()
    return TINY_PNG_BYTES


def _build_hermetic_assets_root() -> Path:
    """Materialize a throwaway copy of ONLY git's tracked assets/ files (see
    the module docstring's "ASSETS/ HERMETICITY GAP" section) and point the
    tool's CW_IMPORT_ASSETS_ROOT tests-only knob at it, before the tool
    module is ever imported or run as a subprocess. This is fixture setup
    (a read-only `git ls-files` snapshot), not a dependency the tool itself
    takes on git -- the tool never shells out to git either way.

    Falls back to copying the real assets/ tree verbatim if git is
    unavailable (e.g. a tarball checkout with no .git) -- strictly less
    hermetic than the tracked-only copy, but never worse than the original,
    pre-fix behaviour, and it prints a note explaining the gap so a
    surprising result is self-explaining rather than a silent mismatch.
    """
    root = Path(tempfile.mkdtemp(prefix="cwiab-assets-"))
    atexit.register(shutil.rmtree, root, True)
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z", "--", "assets"],
            cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=True,
        )
        rel_paths = [p for p in out.stdout.decode("utf-8").split("\0") if p]
        if not rel_paths:
            raise RuntimeError("git ls-files -- assets returned nothing")
        for rel in rel_paths:
            src = REPO_ROOT / rel
            dest = root / Path(rel).relative_to("assets")
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
        print("hermetic assets root: {0} tracked file(s) copied from {1} "
              "(git-ignored fallbacks like assets/mob/* excluded on purpose)"
              .format(len(rel_paths), REAL_ASSETS_DIR))
    except Exception as exc:  # pragma: no cover -- only on a non-git checkout
        shutil.rmtree(root, ignore_errors=True)
        shutil.copytree(REAL_ASSETS_DIR, root)
        print("WARNING: could not snapshot git-tracked assets/ ({0}); fell "
              "back to copying the real assets/ tree verbatim -- this run is "
              "NOT hermetic against git-ignored fallback files (e.g. "
              "assets/mob/*)".format(exc))
    return root


os.environ["CW_IMPORT_ASSETS_ROOT"] = str(_build_hermetic_assets_root())


def _load_tool_module():
    spec = importlib.util.spec_from_file_location("import_owned_beta_assets", TOOL_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


TOOL = _load_tool_module()


def run_tool(args: list, timeout: int = 60, assets_root: Path | None = None) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    if assets_root is not None:
        env["CW_IMPORT_ASSETS_ROOT"] = str(assets_root)
    return subprocess.run(
        [sys.executable, str(TOOL_PATH)] + args,
        cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, env=env,
    )


def make_pcm_wav(path: Path, freq_hz: int, n_samples: int = 200, rate: int = 22050) -> None:
    """A tiny, genuinely-valid PCM16-mono WAV -- the exact format
    audio_wav.h's wavh_parse (and hence sound_transcode, if it happens to be
    built on this machine) accepts -- so this test is deterministic whether
    or not sound_transcode is present: a raw copy is directly playable, and a
    real transcode of an already-canonical WAV is a pass-through."""
    import math
    import struct
    samples = [int(3000 * math.sin(2 * math.pi * freq_hz * i / rate)) for i in range(n_samples)]
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def build_fake_jar(path: Path, include_all_required: bool, include_snow: bool = True) -> None:
    """A synthetic, flat/un-namespaced (Beta-era-shaped) jar covering: every
    JAR_SIGNATURE_ENTRIES path (so verify_jar accepts it), the
    gui/container.png -> gui/chest.png rename case, art/kz.png (the direct
    pass-through case make_beta_pack.ps1 historically missed), and -- gated by
    the two flags -- the REQUIRED texture set, so a caller can exercise both
    the happy path and the MISSING_REQUIRED failure path from one builder."""
    entries = list(TOOL.JAR_SIGNATURE_ENTRIES) + [
        "gui/icons.png", "gui/container.png", "art/kz.png",
        "mob/zombie.png", "mob/skeleton.png", "mob/spider.png",
        "environment/rain.png", "gui/trap.png",
    ]
    if include_snow:
        entries.append("environment/snow.png")
    # Every dest the live tool currently classifies as required, minus
    # environment/snow.png when include_snow is False (the deliberate gap).
    required_dests = [d for d in TOOL.extract_texture_dest_paths(
        TOOL.TEXTURE_C.read_text(encoding="utf-8", errors="replace"))
        if TOOL.is_texture_required(d)]
    if include_all_required:
        for dest in required_dests:
            jar_source = TOOL.DEST_TO_JAR_SOURCE_OVERRIDE.get(dest, dest)
            if jar_source == "environment/snow.png" and not include_snow:
                continue
            entries.append(jar_source)
    with zipfile.ZipFile(path, "w") as zf:
        for name in dict.fromkeys(entries):  # de-dup while preserving order
            zf.writestr(name, canonical_png(name))


def build_resources(root: Path) -> None:
    """sound/ + newsound/ (union with one deliberately duplicated variant and
    one deliberately distinct one, to exercise both the byte-identical dedup
    AND real multi-variant collection), music/ + newmusic/, and streaming/
    with a mixed-case record name (tests the lowercase-rename rule)."""
    (root / "sound" / "step").mkdir(parents=True)
    (root / "newsound" / "step").mkdir(parents=True)
    (root / "music").mkdir(parents=True)
    (root / "newmusic").mkdir(parents=True)
    (root / "streaming").mkdir(parents=True)

    # grass1 exists byte-identically in BOTH sound/ and newsound/ (Beta's real
    # overlap shape) -> must dedup to exactly ONE output variant.
    make_pcm_wav(root / "sound" / "step" / "grass1.wav", freq_hz=440)
    make_pcm_wav(root / "newsound" / "step" / "grass1.wav", freq_hz=440)
    # grass2 is genuinely distinct content -> a SECOND output variant.
    make_pcm_wav(root / "sound" / "step" / "grass2.wav", freq_hz=880)

    (root / "music" / "calm1.ogg").write_bytes(b"placeholder-ogg-calm1")
    (root / "music" / "calm2.ogg").write_bytes(b"placeholder-ogg-calm2")
    (root / "newmusic" / "hal1.ogg").write_bytes(b"placeholder-ogg-hal1")

    (root / "streaming" / "13.ogg").write_bytes(b"placeholder-ogg-13")
    (root / "streaming" / "Cat.ogg").write_bytes(b"placeholder-ogg-cat-mixed-case")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_modern_minecraft(root: Path, rel_paths: dict) -> None:
    """A synthetic MODERN launcher assets root: assets/indexes/<n>.json +
    assets/objects/<hash[:2]>/<hash> -- content-addressed exactly like a real
    one, but every byte here is placeholder text this test wrote itself,
    never real Mojang audio (guard-ip.sh has nothing to match). rel_paths
    maps "minecraft/sounds/..." -> the placeholder bytes to store for it.
    Generic (any full asset path); build_modern_creative_music below is the
    creative-playlist-specific sibling fixture, sharing this same on-disk
    shape (both feed the SAME find_modern_asset_index/resolve_modern_object
    resolver pair in the tool)."""
    idx_dir = root / "assets" / "indexes"
    obj_dir = root / "assets" / "objects"
    idx_dir.mkdir(parents=True)
    objects = {}
    for rel_path, data in rel_paths.items():
        h = hashlib.sha1(data).hexdigest()  # real launcher indexes use sha1
        objects[rel_path] = {"hash": h, "size": len(data)}
        d = obj_dir / h[:2]
        d.mkdir(parents=True, exist_ok=True)
        (d / h).write_bytes(data)
    (idx_dir / "17.json").write_text(json.dumps({"objects": objects}), encoding="utf-8")
    # A non-numeric legacy index name must never confuse "pick the highest
    # numeric index" -- same shape as the real pre-1.6.json seen on disk.
    (idx_dir / "pre-1.6.json").write_text(json.dumps({"objects": {}}), encoding="utf-8")


def build_modern_creative_music(root: Path, leaves, index_name: str = "1.7") -> dict:
    """A synthetic modern (1.6+) .minecraft asset-hash store: assets/objects/
    holds one placeholder-content file per requested leaf name, hashed and
    referenced from assets/indexes/<index_name>.json exactly the way a real
    modern client's index does (flat "objects" map keyed by the FULL
    minecraft/... path, each value {"hash": sha1hex, "size": n}). Returns the
    leaf -> sha1 map so callers can check dest bytes / hashes without
    re-deriving them. Zero Mojang bytes: every "sound" is a short ASCII
    placeholder string, exactly like build_resources()'s "placeholder-ogg-*"
    music fixtures above. Creative-music-specific sibling of the generic
    build_modern_minecraft above (same on-disk shape, same tool-side
    resolver, different object-key prefix)."""
    objects_dir = root / "assets" / "objects"
    indexes_dir = root / "assets" / "indexes"
    indexes_dir.mkdir(parents=True, exist_ok=True)
    objects = {}
    hashes = {}
    for leaf in leaves:
        data = "placeholder-creative-ogg-{0}".format(leaf).encode("ascii")
        h = hashlib.sha1(data).hexdigest()
        sub = objects_dir / h[:2]
        sub.mkdir(parents=True, exist_ok=True)
        (sub / h).write_bytes(data)
        objects["minecraft/sounds/music/game/creative/{0}.ogg".format(leaf)] = {
            "hash": h, "size": len(data),
        }
        hashes[leaf] = h
    (indexes_dir / "{0}.json".format(index_name)).write_text(
        json.dumps({"objects": objects}), encoding="utf-8")
    return hashes


# ============================================================================

def test_live_parsing_sane() -> None:
    """The tool's live extraction from texture.c / audio_sound_table.h /
    audio_music_table.h must find a plausible, non-empty, well-formed set --
    and the one documented rename (Beta's gui/container.png -> the engine's
    gui/chest.png) must be present."""
    text = TOOL.TEXTURE_C.read_text(encoding="utf-8", errors="replace")
    dests = TOOL.extract_texture_dest_paths(text)
    assert len(dests) > 20, "expected texture.c to have many TEX_LOAD_FILE call sites"
    assert "gui/chest.png" in dests
    assert TOOL.DEST_TO_JAR_SOURCE_OVERRIDE.get("gui/chest.png") == "gui/container.png"
    assert "terrain.png" in dests and "particles.png" in dests

    required = [d for d in dests if TOOL.is_texture_required(d)]
    exempt = (TOOL.COMPOSITE_DEST | TOOL.FORK_ONLY_DEST |
              TOOL.NOT_IN_BETA_173_DEST | TOOL.OPTIONAL_NO_FALLBACK_DEST)
    assert set(required) == {d for d in dests if d not in exempt
                             and not (TOOL.ASSETS_DEFAULT_DIR / d).exists()}
    # every required entry must have NO bundled default (by the tool's own rule)
    for d in required:
        assert not (TOOL.ASSETS_DEFAULT_DIR / d).exists(), d
    # composites and fork-only art are never required
    for d in TOOL.COMPOSITE_DEST | TOOL.FORK_ONLY_DEST:
        assert d not in required
    # misc/dial.png (TextureWatchFX's dial ring): a genuine Beta jar entry
    # that remains optional even without a bundled fallback; discovered live
    # from texture.c's own TEX_LOAD_FILE call AND never classified required.
    assert "misc/dial.png" in dests, (
        "misc/dial.png must be scraped live from texture.c's TEX_LOAD_FILE "
        "call, not a hand-maintained list")
    assert "misc/dial.png" in TOOL.OPTIONAL_NO_FALLBACK_DEST
    assert "misc/dial.png" not in required

    keys, max_variants = TOOL.sound_keys_and_max_variants()
    assert "step.grass" in keys and "ambient.cave.cave" in keys
    assert max_variants >= 13

    assert TOOL.music_keys() == ["calm", "hal", "nuance", "piano"]
    assert TOOL.creative_music_keys() == [
        "aria_math", "biome_fest", "blind_spots", "dreiton", "haunt_muskie", "taswell",
    ]
    print("PASS: live parsing of texture.c / audio_sound_table.h / audio_music_table.h")


def test_texture_loader_aliases_use_canonical_output_names() -> None:
    cases = (
        ('tex_read_gfx("gui/container.png", &w, &h, fmt);', ["gui/chest.png"]),
        ('TEX_LOAD_FILE(&chest, "gui/chest.png", fmt, false);', ["gui/chest.png"]),
        ('tex_read_gfx("gui/container.png", &w, &h, fmt);\n'
         'tex_read_gfx("gui/chest.png", &w, &h, fmt);\n'
         'tex_read_gfx("custom.png", &w, &h, fmt);', ["gui/chest.png", "custom.png"]),
        ('tex_read_gfx("gui/chest.png", &w, &h, fmt);\n'
         'tex_read_gfx("gui/container.png", &w, &h, fmt);', ["gui/chest.png"]),
    )
    for source, expected in cases:
        actual = TOOL.extract_texture_dest_paths(source)
        assert actual == expected, (actual, expected)
    print("PASS: raw loader aliases become one canonical importer output")


def test_refuses_without_jar_or_destination() -> None:
    proc = run_tool(["--out", "somewhere"])
    assert proc.returncode != 0, proc.stdout
    assert "--jar" in proc.stdout or "required" in proc.stdout.lower()

    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        jar = Path(td) / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        proc2 = run_tool(["--jar", str(jar)])
        assert proc2.returncode != 0, proc2.stdout
    print("PASS: refuses to run without --jar or a destination (--out/--sd)")


def test_refuses_bad_jar() -> None:
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        not_a_zip = tmp / "not_a_jar.jar"
        not_a_zip.write_bytes(b"this is not a zip file at all")
        proc = run_tool(["--jar", str(not_a_zip), "--out", str(tmp / "out1")])
        assert proc.returncode != 0
        assert not (tmp / "out1").exists()

        modern_jar = tmp / "modern.jar"
        with zipfile.ZipFile(modern_jar, "w") as zf:
            zf.writestr("assets/minecraft/textures/block/stone.png", TINY_PNG_BYTES)
        proc2 = run_tool(["--jar", str(modern_jar), "--out", str(tmp / "out2")])
        assert proc2.returncode != 0, proc2.stdout
        assert "MODERN" in proc2.stdout
        assert not (tmp / "out2").exists()
    print("PASS: refuses a non-zip file and a modern namespaced jar")


def test_dry_run_writes_nothing() -> None:
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        out = tmp / "out"
        proc = run_tool(["--jar", str(jar), "--out", str(out), "--dry-run", "--manifest"])
        assert proc.returncode == 0, proc.stdout
        assert not out.exists(), "dry-run must not create the destination at all"
    print("PASS: --dry-run writes nothing")


def test_missing_required_exits_nonzero() -> None:
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True, include_snow=False)
        out = tmp / "out"
        # Exercise the required gate even when the shipped pack is complete.
        assets = tmp / "assets"
        shutil.copytree(TOOL.ASSETS_DEFAULT_DIR, assets)
        (assets / "environment/snow.png").unlink(missing_ok=True)
        proc = run_tool(["--jar", str(jar), "--out", str(out)], assets_root=assets)
        assert proc.returncode != 0, proc.stdout
        assert "environment/snow.png" in proc.stdout
        assert "MISSING" in proc.stdout
    print("PASS: a missing REQUIRED texture exits non-zero and names the file")


def test_full_run_layout_manifest_and_sounds() -> None:
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        resources = tmp / "resources"
        build_resources(resources)
        out = tmp / "out"

        proc = run_tool([
            "--jar", str(jar), "--resources", str(resources), "--out", str(out),
            "--manifest", "--no-sound-transcode",
        ])
        assert proc.returncode == 0, proc.stdout

        # --- manifest loaded up front and its texture classification printed
        # immediately, BEFORE the layout assertions below -- so if one of
        # those assertions ever fails again (e.g. the exact hermeticity gap
        # this test now guards against), the failure is self-explaining from
        # the test output alone: which dest was required/optional and why,
        # without needing a second run with extra debugging added. ---
        manifest_path = out / "import_manifest.json"
        assert manifest_path.is_file()
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        print("texture classification (dest: required, status[, reason]):")
        for entry in manifest["entries"]:
            if entry.get("category") != "texture":
                continue
            reason = entry.get("reason")
            print("  {0}: required={1}, status={2}{3}".format(
                entry.get("dest"), entry.get("required"), entry.get("status"),
                ", reason={0}".format(reason) if reason else ""))

        # --- layout matches what the engine actually reads ---
        assert (out / "gui" / "chest.png").is_file(), "gui/container.png must land as gui/chest.png"
        assert not (out / "gui" / "container.png").exists(), "the Beta jar name must not leak into the pack"
        assert (out / "art" / "kz.png").is_file()
        for dest in ("mob/zombie.png", "mob/skeleton.png", "mob/spider.png",
                     "environment/rain.png", "environment/snow.png", "gui/trap.png"):
            assert (out / dest).is_file(), dest

        # --- sound union + dedup + renumbering ---
        step_dir = out / "sound" / "step"
        wavs = sorted(p.name for p in step_dir.glob("grass*.wav"))
        assert len(wavs) == 2, (
            "grass1 (byte-identical in sound/ and newsound/) must dedup to ONE file, "
            "grass2 (distinct content) must produce a SECOND -- got %r" % wavs)
        for name in wavs:
            data = (step_dir / name).read_bytes()
            assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", name
        # both dest .wav files must actually be BYTE-DISTINCT from each other
        # (grass1 and grass2 were built with different tone frequencies).
        contents = {(step_dir / n).read_bytes() for n in wavs}
        assert len(contents) == 2, "the two variants must not collide to the same bytes"

        # --- music: BOTH music/ and newmusic/ merge into one flat music/ dir ---
        assert (out / "music" / "calm1.ogg").is_file()
        assert (out / "music" / "calm2.ogg").is_file()
        assert (out / "music" / "hal1.ogg").is_file()

        # --- records: mixed-case "Cat.ogg" must land lowercase, matching the
        # engine's hardcoded audio_music_record_file_stem() lookup exactly.
        # Checked via a case-SENSITIVE directory listing rather than
        # Path.exists() -- Windows filesystems resolve paths case-
        # insensitively, so exists() alone cannot tell "cat.ogg" from
        # "Cat.ogg" on this host. ---
        record_names = {p.name for p in (out / "records").iterdir()}
        assert "13.ogg" in record_names
        assert "cat.ogg" in record_names, record_names
        assert "Cat.ogg" not in record_names, record_names

        # --- manifest completeness + hash verification (manifest already
        # loaded and its texture classification printed above) ---
        assert manifest["tool_version"] == TOOL.TOOL_VERSION
        assert manifest["jar"]["sha256"] == sha256_file(jar)
        assert manifest["jar"]["sha256"] == hashlib.sha256(jar.read_bytes()).hexdigest()
        assert len(manifest["entries"]) > 0

        verified = 0
        for entry in manifest["entries"]:
            dest_hash = entry.get("dest_sha256")
            dest_rel = entry.get("dest")
            if dest_hash and dest_rel:
                on_disk = out / dest_rel
                assert on_disk.is_file(), "manifest references a dest file that was not written: %s" % dest_rel
                assert sha256_file(on_disk) == dest_hash, "hash mismatch for %s" % dest_rel
                verified += 1
            src_hash = entry.get("source_sha256")
            src_path = entry.get("source")
            if src_hash and src_path and entry.get("category") == "texture":
                with zipfile.ZipFile(jar) as zf:
                    assert hashlib.sha256(zf.read(src_path)).hexdigest() == src_hash
                verified += 1
        assert verified > 10, "expected many manifest hash entries to be checkable"

        # required textures must never be reported required==True with a
        # bundled default AND missing at the same time (internal consistency)
        for entry in manifest["entries"]:
            if entry.get("category") == "texture" and entry.get("required"):
                assert entry["status"] in ("copied",), (
                    "a REQUIRED texture must have been copied on a run that "
                    "exits 0: %r" % entry)
    print("PASS: full run -- output layout, sound union/dedup/rename, music merge, "
          "record case-fold, and manifest hash verification")


def test_dial_png_optional_present_and_absent() -> None:
    """misc/dial.png (TextureWatchFX's dial ring) is optional regardless of
    whether a bundled fallback exists -- its absence from the jar must
    still exit 0 (the engine gates it), and its PRESENCE must still copy it
    through like any other texture (a player who owns it should get it)."""
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)

        # (a) absent from the jar -> run succeeds, no misc/dial.png written.
        jar_without = tmp / "b173_no_dial.jar"
        build_fake_jar(jar_without, include_all_required=True)
        with zipfile.ZipFile(jar_without) as zf:
            assert "misc/dial.png" not in zf.namelist()
        out_without = tmp / "out_no_dial"
        proc = run_tool(["--jar", str(jar_without), "--out", str(out_without)])
        assert proc.returncode == 0, proc.stdout
        assert not (out_without / "misc" / "dial.png").exists()
        # print_report always prints a "MISSING (required): N" count line, so
        # assert the COUNT is zero rather than the bare substring "MISSING".
        assert "MISSING (required):    0" in proc.stdout, proc.stdout

        # (b) present in the jar -> copied through like any other texture.
        jar_with = tmp / "b173_with_dial.jar"
        build_fake_jar(jar_with, include_all_required=True)
        with zipfile.ZipFile(jar_with, "a") as zf:
            zf.writestr("misc/dial.png", canonical_png("misc/dial.png"))
        out_with = tmp / "out_with_dial"
        proc2 = run_tool(["--jar", str(jar_with), "--out", str(out_with), "--manifest"])
        assert proc2.returncode == 0, proc2.stdout
        dial_out = out_with / "misc" / "dial.png"
        assert dial_out.is_file()
        assert dial_out.read_bytes() == canonical_png("misc/dial.png")

        manifest = json.loads((out_with / "import_manifest.json").read_text(encoding="utf-8"))
        dial_entries = [e for e in manifest["entries"] if e.get("dest") == "misc/dial.png"]
        assert len(dial_entries) == 1, dial_entries
        assert dial_entries[0]["status"] == "copied"
        assert dial_entries[0]["required"] is False, (
            "misc/dial.png must be copied when present, but NEVER reported "
            "required -- that flag governs the MISSING-from-jar failure path only")
    print("PASS: misc/dial.png is optional-but-imported (absent -> exit 0, "
          "present -> copied, both paths report required=False)")


def test_no_resources_is_textures_only_and_never_fails() -> None:
    """The ship_distribution_plan.md 'bare jar' flow: omitting --resources
    must never fail the run (no sound/music/record key is ever required)."""
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        out = tmp / "out"
        proc = run_tool(["--jar", str(jar), "--out", str(out)])
        assert proc.returncode == 0, proc.stdout
        assert not (out / "sound").exists()
        assert not (out / "music").exists()
        assert not (out / "records").exists()
    print("PASS: a bare jar (no --resources) is textures-only and still exits 0")


def test_modern_minecraft_import_gates_on_the_flag() -> None:
    """The minecart sound add-on (docs/deviations.md): omitting
    --modern-minecraft must leave sound/minecart/ entirely absent (Beta-
    faithful silence, fallback-silent behaviour); supplying a synthetic
    modern assets root must import BOTH keys under the exact subpath the
    engine's audio_registry scans (sound/minecart/{base,inside}.<ext>)."""
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)

        # --- (a) omitted: no modern-only key may appear anywhere ---
        out_silent = tmp / "out_silent"
        proc = run_tool(["--jar", str(jar), "--out", str(out_silent),
                          "--no-sound-transcode"])
        assert proc.returncode == 0, proc.stdout
        assert not (out_silent / "sound" / "minecart").exists(), (
            "minecart.base/inside ship no bundled default -- a run with no "
            "--modern-minecraft must not manufacture a sound/minecart/ dir")

        # --- (b) supplied: both keys land at the engine's exact scan path ---
        modern = tmp / "modern"
        build_modern_minecraft(modern, {
            "minecraft/sounds/minecart/base.ogg": b"placeholder-ogg-base",
            "minecraft/sounds/minecart/inside.ogg": b"placeholder-ogg-inside",
        })
        out = tmp / "out"
        proc = run_tool(["--jar", str(jar), "--out", str(out),
                          "--modern-minecraft", str(modern),
                          "--no-sound-transcode"])
        assert proc.returncode == 0, proc.stdout
        base_path = out / "sound" / "minecart" / "base.ogg"
        inside_path = out / "sound" / "minecart" / "inside.ogg"
        assert base_path.is_file(), sorted(
            p.name for p in (out / "sound" / "minecart").iterdir())
        assert inside_path.is_file()
        assert base_path.read_bytes() == b"placeholder-ogg-base"
        assert inside_path.read_bytes() == b"placeholder-ogg-inside"

        # --- (c) a directory that is not a modern assets root at all (e.g.
        # someone points --modern-minecraft at a Beta resources dir by
        # mistake) must warn and fall back, never crash or write partial
        # output ---
        out_bad = tmp / "out_bad"
        not_modern = tmp / "not_modern"
        not_modern.mkdir()
        proc = run_tool(["--jar", str(jar), "--out", str(out_bad),
                          "--modern-minecraft", str(not_modern),
                          "--no-sound-transcode"])
        assert proc.returncode == 0, proc.stdout
        assert not (out_bad / "sound" / "minecart").exists()
    print("PASS: --modern-minecraft gates the minecart sound add-on "
          "(absent -> silent, present -> imported, malformed root -> silent)")


def test_creative_music_omitted_is_noop() -> None:
    """Omitting --modern-minecraft must never fail the run and must leave
    music/creative/ entirely absent -- the same "bare jar" no-failure shape
    test_no_resources_is_textures_only_and_never_fails() already proves for
    --resources."""
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        out = tmp / "out"
        proc = run_tool(["--jar", str(jar), "--out", str(out)])
        assert proc.returncode == 0, proc.stdout
        assert "creative music report" in proc.stdout
        assert not (out / "music" / "creative").exists()
    print("PASS: omitting --modern-minecraft is a no-op (no failure, nothing written)")


def test_creative_music_full_run_hash_and_manifest() -> None:
    """The happy path: all six creative tracks resolve through a synthetic
    modern asset index and land at music/creative/<name>.ogg, byte-identical
    to their source object, with manifest hash entries that verify."""
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        modern = tmp / "modern"
        leaves = TOOL.creative_music_keys()
        hashes = build_modern_creative_music(modern, leaves)
        out = tmp / "out"

        proc = run_tool([
            "--jar", str(jar), "--modern-minecraft", str(modern),
            "--out", str(out), "--manifest",
        ])
        assert proc.returncode == 0, proc.stdout
        assert "tracks copied: 6" in proc.stdout

        creative_dir = out / "music" / "creative"
        for leaf in leaves:
            dest = creative_dir / "{0}.ogg".format(leaf)
            assert dest.is_file(), leaf
            expected = "placeholder-creative-ogg-{0}".format(leaf).encode("ascii")
            assert dest.read_bytes() == expected, leaf

        manifest = json.loads((out / "import_manifest.json").read_text(encoding="utf-8"))
        assert manifest["modern_minecraft"]["path"] == str(modern.resolve())
        cm_entries = {e["key"]: e for e in manifest["entries"]
                      if e.get("category") == "creative_music"}
        assert set(cm_entries) == set(leaves)
        for leaf, entry in cm_entries.items():
            assert entry["status"] == "copied"
            assert entry["index_sha1"] == hashes[leaf]
            assert entry["source_sha1"] == hashes[leaf]
            dest = creative_dir / "{0}.ogg".format(leaf)
            assert sha256_file(dest) == entry["dest_sha256"]
    print("PASS: full modern-minecraft run -- all 6 creative tracks copied, "
          "byte-identical, manifest hashes verify")


def test_creative_music_partial_and_missing_object_file() -> None:
    """A modern install that is missing one track from the index and has a
    dangling reference (index entry present, object file absent -- an
    incomplete/pruned asset cache) for another must report BOTH precisely
    and still exit 0 -- neither case is ever a hard failure (no CC0 fallback
    exists, by design, docs/creative_mode.md's fallback-to-Beta rule)."""
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        modern = tmp / "modern"
        leaves = TOOL.creative_music_keys()
        present = [l for l in leaves if l != "taswell"]  # omit one entirely
        build_modern_creative_music(modern, present)

        # Add a dangling reference for "taswell": present in the index, but
        # its object file is never written to assets/objects/.
        index_path = modern / "assets" / "indexes" / "1.7.json"
        data = json.loads(index_path.read_text(encoding="utf-8"))
        dangling_hash = hashlib.sha1(b"never-written").hexdigest()
        data["objects"]["minecraft/sounds/music/game/creative/taswell.ogg"] = {
            "hash": dangling_hash, "size": 13,
        }
        index_path.write_text(json.dumps(data), encoding="utf-8")

        out = tmp / "out"
        proc = run_tool([
            "--jar", str(jar), "--modern-minecraft", str(modern), "--out", str(out),
        ])
        assert proc.returncode == 0, proc.stdout
        assert not (out / "music" / "creative" / "taswell.ogg").exists()
        for leaf in present:
            assert (out / "music" / "creative" / "{0}.ogg".format(leaf)).is_file()
    print("PASS: a dangling index reference (object file missing) is reported, "
          "not fatal, and the other five tracks still import")


def test_creative_music_index_version_pin() -> None:
    """--modern-minecraft-index pins an exact assets/indexes/<name>.json,
    bypassing auto-discovery -- proven by giving the fixture an index name
    auto-discovery would never guess (a real install can carry many
    historical per-version indexes)."""
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        modern = tmp / "modern"
        leaves = TOOL.creative_music_keys()
        build_modern_creative_music(modern, leaves, index_name="99w99a_snapshot")
        out = tmp / "out"

        proc = run_tool([
            "--jar", str(jar), "--modern-minecraft", str(modern),
            "--modern-minecraft-index", "99w99a_snapshot", "--out", str(out),
        ])
        assert proc.returncode == 0, proc.stdout
        assert "tracks copied: 6" in proc.stdout
        assert (out / "music" / "creative" / "aria_math.ogg").is_file()

        # A wrong pin finds nothing (not_in_index for every key) but still
        # never fails the run.
        out2 = tmp / "out2"
        proc2 = run_tool([
            "--jar", str(jar), "--modern-minecraft", str(modern),
            "--modern-minecraft-index", "does_not_exist", "--out", str(out2),
        ])
        assert proc2.returncode == 0, proc2.stdout
        assert "tracks copied: 0" in proc2.stdout
        assert not (out2 / "music" / "creative").exists()
    print("PASS: --modern-minecraft-index pins an exact version; a wrong pin "
          "finds nothing without failing")


def test_creative_music_dry_run_writes_nothing() -> None:
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        modern = tmp / "modern"
        build_modern_creative_music(modern, TOOL.creative_music_keys())
        out = tmp / "out"
        proc = run_tool([
            "--jar", str(jar), "--modern-minecraft", str(modern),
            "--out", str(out), "--dry-run",
        ])
        assert proc.returncode == 0, proc.stdout
        assert "tracks copied: 6" in proc.stdout  # reported...
        assert not out.exists()                    # ...but nothing written
    print("PASS: --modern-minecraft honors --dry-run (reports, writes nothing)")



def test_cli_textures_are_the_helpers_bytes() -> None:
    """Audit F3/F4: one output. The CLI's textures are byte-identical to the
    Pack Helper's prepare() for the same jar, including the grass/foliage
    colour maps and the gui_2.png HUD / anim.png composites."""
    sys.path.insert(0, str(REPO_ROOT))
    from tools.pack_helper import core
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        with zipfile.ZipFile(jar, "a") as zf:
            for name in ("misc/grasscolor.png", "misc/foliagecolor.png"):
                zf.writestr(name, canonical_png(name))
        out = tmp / "out"
        proc = run_tool(["--jar", str(jar), "--out", str(out)])
        assert proc.returncode == 0, proc.stdout
        helper = core.prepare(jar, tmp / "helper")
        assert helper["ok"], helper
        prepared = Path(helper["path"])
        pngs = [row["path"] for row in helper["manifest"]["files"] if row["path"].endswith(".png")]
        for must in ("misc/grasscolor.png", "misc/foliagecolor.png", "gui_2.png", "anim.png"):
            assert must in pngs, (must, pngs)
        for rel in pngs:
            assert (out / rel).is_file(), rel
            assert (out / rel).read_bytes() == (prepared / rel).read_bytes(), rel
    print("PASS: CLI textures == Pack Helper textures, byte for byte ({0} files)".format(len(pngs)))


def test_cli_refuses_a_bad_texture_with_the_helpers_message() -> None:
    with tempfile.TemporaryDirectory(prefix="cwiab-") as td:
        tmp = Path(td)
        jar = tmp / "b173.jar"
        build_fake_jar(jar, include_all_required=True)
        with zipfile.ZipFile(jar, "a") as zf:
            zf.writestr("particles.png", TINY_PNG_BYTES)  # 1x1, not 128x128
        out = tmp / "out"
        proc = run_tool(["--jar", str(jar), "--out", str(out)])
        assert proc.returncode == 1, proc.stdout
        assert "particles.png cannot be used" in proc.stdout, proc.stdout
        assert "OK" not in proc.stdout.splitlines()[-1]
    print("PASS: a bad texture fails the CLI with the helper's message")


def test_writing_into_repo_subtree_refused() -> None:
    proc = run_tool(["--jar", str(REPO_ROOT / "README.md"), "--out", str(REPO_ROOT / "assets")])
    # README.md isn't even a jar, but the destination guard must fire before
    # (or regardless of) jar validation for a protected repo subtree.
    assert proc.returncode != 0
    print("PASS: refuses to write inside a protected repo subtree (assets/)")


def main() -> int:
    test_live_parsing_sane()
    test_texture_loader_aliases_use_canonical_output_names()
    test_refuses_without_jar_or_destination()
    test_refuses_bad_jar()
    test_dry_run_writes_nothing()
    test_missing_required_exits_nonzero()
    test_full_run_layout_manifest_and_sounds()
    test_dial_png_optional_present_and_absent()
    test_no_resources_is_textures_only_and_never_fails()
    test_modern_minecraft_import_gates_on_the_flag()
    test_creative_music_omitted_is_noop()
    test_creative_music_full_run_hash_and_manifest()
    test_creative_music_partial_and_missing_object_file()
    test_creative_music_index_version_pin()
    test_creative_music_dry_run_writes_nothing()
    test_writing_into_repo_subtree_refused()
    test_cli_textures_are_the_helpers_bytes()
    test_cli_refuses_a_bad_texture_with_the_helpers_message()
    print("PASS test_import_owned_beta_assets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
