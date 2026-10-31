"""Release-time resources only. Customers never need Git, a compiler or sources."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


# The asset-exclusion rule, in one machine-readable place. It MUST stay
# identical to tools/make_release.sh's ASSET_EXCLUDE_MATCH: the two used to be
# the same rule written out twice, free to drift (2026-09-17 review, M3).
# tests/test_pack_helper_desktop.py parses BOTH and fails on any difference.
#
# Semantics, same as the shell: a listed path is EXCLUDED unless the content at
# the selected commit hashes to the one digest cleared for it -- so an older ref
# or a local overlay cannot inherit the clearance by filename alone.
ASSET_EXCLUDE_MATCH = {
    # 2026-09-11: admit only the reviewed Earth & Ash CC0 replacement.
    "assets/misc/water.png":
        "7a1a30372469a5db1567f7451365dcbde1abb71b0d85aea7cec10043bd34677f",
}

# Legal files the bundle must carry, mirroring make_release.sh 12f
# (tools/make_release.sh:693-707): packaging is REFUSED without them, and they
# are never synthesized here -- the helper has no license text of its own.
# LICENSES/GPL-2.0.txt: the bundled beta-base/pointer.png is the GPLv2 Homebrew
# Channel pointer (NOTICE cites this file), so the bundle ships its text (EAD review).
REQUIRED_LEGAL_FILES = ("LICENSE", "NOTICE", "LICENSES/GPL-2.0.txt")
BRANDING_FILES = ("logo.png", "computer.png", "computer.ico")


def copy_committed_branding(repo, destination):
    """Use the approved committed art, never an untracked/local replacement."""
    branding = Path(destination) / "branding"
    branding.mkdir(parents=True, exist_ok=True)
    for name in BRANDING_FILES:
        relative = "tools/pack_helper/art/" + name
        try:
            data = subprocess.check_output(["git", "-C", str(repo), "show", "HEAD:" + relative])
        except subprocess.CalledProcessError as exc:
            raise SystemExit("pack_helper: approved branding missing at HEAD: " + relative) from exc
        (branding / name).write_bytes(data)


def asset_is_excluded(name: str, data: bytes) -> bool:
    cleared = ASSET_EXCLUDE_MATCH.get(name)
    return cleared is not None and hashlib.sha256(data).hexdigest() != cleared


def build(repo, destination, cc="cc"):
    repo, destination = Path(repo).resolve(), Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=False)
    assets = destination / "assets"
    assets.mkdir()
    # Same tracked-content and water-overlay provenance rule as make_release.sh.
    tracked = subprocess.check_output(
        ["git", "-C", str(repo), "ls-tree", "-r", "--name-only", "HEAD", "--", "assets"],
        text=True).splitlines()
    for name in tracked:
        data = subprocess.check_output(["git", "-C", str(repo), "show", "HEAD:" + name])
        if asset_is_excluded(name, data):
            continue
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    # Beta-pack base (packs/beta-base): Beta's own animated-texture colours and
    # the Homebrew pointer, which Beta-format packs take instead of the Earth &
    # Ash default in assets/ (2026-10-11). Tracked content only, like assets/.
    beta_base = destination / "beta-base"
    for name in ("anim.png", "pointer.png"):
        try:
            data = subprocess.check_output(
                ["git", "-C", str(repo), "show", "HEAD:packs/beta-base/" + name])
        except subprocess.CalledProcessError as exc:
            raise SystemExit("pack_helper: packs/beta-base/{0} not found at HEAD -- refusing "
                             "to bundle a helper that would give Beta packs Earth & Ash "
                             "colours or pointer".format(name)) from exc
        beta_base.mkdir(exist_ok=True)
        (beta_base / name).write_bytes(data)
    # 12f mirror: a helper bundle that ships the project's assets ships the
    # project's license and attributions with them, or it is not built at all.
    for legal in REQUIRED_LEGAL_FILES:
        try:
            data = subprocess.check_output(
                ["git", "-C", str(repo), "show", "HEAD:" + legal])
        except subprocess.CalledProcessError as exc:
            raise SystemExit(
                "pack_helper: {0} not found at HEAD -- refusing to package the "
                "helper bundle without it (tools/make_release.sh:693-707 applies "
                "the same rule to the game release).".format(legal)) from exc
        (destination / legal).parent.mkdir(parents=True, exist_ok=True)
        (destination / legal).write_bytes(data)
    # The game's shipped default settings: when a config.json is missing on
    # the card the helper starts it from these, never from a file holding only
    # the pack path (2026-10-30 jar-import audit F9). Committed content only.
    defaults = destination / "defaults"
    defaults.mkdir()
    for name in ("config_wii.json", "config_pc.json"):
        try:
            data = subprocess.check_output(["git", "-C", str(repo), "show", "HEAD:" + name])
        except subprocess.CalledProcessError as exc:
            raise SystemExit("pack_helper: {0} not found at HEAD -- refusing to bundle a "
                             "helper that cannot start a missing config.json".format(name)) from exc
        (defaults / name).write_bytes(data)
    copy_committed_branding(repo, destination)
    spec = importlib.util.spec_from_file_location("owned_importer", repo / "tools/pack_helper/owned_importer.py")
    importer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(importer)
    textures = importer.extract_texture_dest_paths((repo / "source/platform/texture.c").read_text())
    keys, variants = importer.sound_keys_and_max_variants()
    mapping = dict(schema_version=1, release_version=(repo / "VERSION").read_text().strip(),
                   texture_paths=textures, sound_keys=keys, sound_max_variants=variants,
                   music_keys=importer.music_keys(), creative_music_keys=importer.creative_music_keys(),
                   records=list(importer.RECORD_STEMS),
                   dest_to_source_overrides=importer.DEST_TO_JAR_SOURCE_OVERRIDE,
                   required_textures=[p for p in textures if not (assets / p).is_file()
                                      and p not in importer.FORK_ONLY_DEST | importer.COMPOSITE_DEST
                                      | importer.OPTIONAL_NO_FALLBACK_DEST | importer.NOT_IN_BETA_173_DEST])
    # newline="\n" on every bundle writer: a CRLF from a Windows build would
    # make the hashed tree differ across OSes for identical content.
    (destination / "mapping.json").write_text(json.dumps(mapping, indent=2),
                                              encoding="utf-8", newline="\n")
    shutil.copyfile(repo / "tools/pack_helper/conversion_map.json", destination / "conversion_map.json")
    suffix = ".exe" if os.name == "nt" else ""
    native = destination / ("sound_transcode" + suffix)
    args = [cc, "-O2", "-std=c99", "-I", str(repo / "source"), "-I", str(repo / "third_party"),
            "-I", str(repo / "tools"), str(repo / "tools/sound_transcode.c"), "-lm", "-o", str(native)]
    if os.name == "nt":
        # -static: no MinGW runtime DLLs beside the frozen app.
        # --no-insert-timestamp: measured 2026-09-17, two builds of identical
        # sources 30 s apart differed in exactly TWO bytes, the PE
        # TimeDateStamp at file offset 136 (0x6aac1115 vs 0x6aac1133) and its
        # debug-directory copy. A wall clock is not an input; drop it so the
        # native tool -- and resource_hashes.json, which hashes it -- are
        # byte-identical across rebuilds.
        args.insert(1, "-static")
        args.insert(2, "-Wl,--no-insert-timestamp")
    subprocess.run(args, check=True)
    subprocess.run([str(native), "--manifest"], check=True, stdout=subprocess.DEVNULL)
    # Bake the faithful procedural effects once, while compilers are available.
    with tempfile.TemporaryDirectory(prefix="cw-fx-") as tmp:
        exe = Path(tmp) / ("fx_gen" + suffix)
        sources = ["tools/gen_anim_fx/fx_gen.c", "source/graphics/lava_fx.c",
                   "source/graphics/flame_fx.c", "source/graphics/portal_fx.c",
                   "source/graphics/water_fx.c", "source/graphics/water_flow_fx.c",
                   "source/worldgen/beta_math.c", "source/worldgen/java_random.c"]
        subprocess.run([cc, "-O2", "-I", str(repo / "source"), "-I", str(repo / "source/graphics"),
                        *[str(repo / p) for p in sources], "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], cwd=tmp, check=True)
        subprocess.run([sys.executable, str(repo / "tools/gen_anim_fx/composite_anim_fx.py"),
                        tmp, str(beta_base / "anim.png"), str(beta_base / "anim.png")], check=True)
    manifest = {p.relative_to(destination).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(destination.rglob("*")) if p.is_file()}
    (destination / "resource_hashes.json").write_text(json.dumps(manifest, indent=2),
                                                      encoding="utf-8", newline="\n")
    return mapping


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    build(args.repo, args.out, args.cc)
