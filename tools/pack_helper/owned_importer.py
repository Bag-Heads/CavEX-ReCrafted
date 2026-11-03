#!/usr/bin/env python3
"""import_owned_beta_assets.py -- lay the USER'S OWN legally-owned Minecraft
Beta 1.7.3 textures and sounds out exactly where CaveWii_v2 reads them.

LEGAL MODEL (see docs/ship_distribution_plan_2026-08-17.md "Track 2"): the
shipped release contains ZERO Mojang-derived bytes -- only this project's own
CC0/CC-BY default assets under assets/. This tool is the other half of that
hybrid: it moves the PLAYER's own Beta 1.7.3 client jar + resources tree,
already sitting on their own machine, onto their own SD card / output
directory, in the EXACT layout the engine's asset loaders read. It never
downloads anything, never bundles its output into anything shareable, and
never runs unless given both a real jar and a real destination.

WHERE THIS DIFFERS FROM tools/make_beta_pack.ps1: that script assumes the
user (or a human) already extracted the jar into a folder; this tool reads
the jar directly (a zip) and needs no PowerShell/.NET. It also derives its
texture list LIVE from source/platform/texture.c and its sound/music key
lists LIVE from the engine's own X-macro tables (audio_sound_table.h /
audio_music_table.h) instead of a hand-maintained, independently-drifting
copy -- the same "generated from the shared table" discipline those headers
already use between the engine and tools/sound_transcode.c. It does NOT
reproduce make_beta_pack.ps1's gui_2.png/anim.png pixel COMPOSITES (see
"NOT DONE" below); for those, run that script afterwards, or use its
bundled-default fallback (this tool never touches assets/ as a copy source
either way -- the engine's own per-file/per-key runtime fallback to assets/
already handles anything this importer's pack omits).

MAPPING TABLE -- source (Beta jar / resources) -> dest (in the pack), and WHY
================================================================================
TEXTURES: for every read-path the engine's texture.c actually loads (scraped
live from its TEX_LOAD_FILE / tex_atlas_block / tex_atlas_particles calls),
the Beta jar source path is IDENTICAL to the dest path, with exactly one
documented exception (Beta names the chest GUI sheet differently -- the same
exception tools/make_beta_pack.ps1 already carries):

    gui/container.png (Beta jar)  ->  gui/chest.png (dest)   [texture.c :189]

Two dest paths are COMPOSITED (no 1:1 Beta source exists) and are explicitly
OUT OF SCOPE here (see NOT DONE): gui_2.png (Beta's gui/gui.png + gui/icons.png
overlaid onto the fork's HUD layout) and anim.png (the fork's animated
water/lava/fire/portal filmstrips with the Beta terrain.png ice tile overlaid).
Both already have a bundled CC0 fallback, so they are never "required".

Two dest paths are FORK-ONLY engine art with no Minecraft-jar source at all
(controls.png, pointer.png) -- always fall back to the bundled default.

Everything else this tool finds under the Beta jar's expected path is
required/optional per REQUIRED TEXTURES below.

SOUNDS (one-shot pool, sound/...): the key list + the "sound"+"newsound"
union-with-no-precedence rule + the digit-strip variant scan are Beta's own
SoundPool contract [decompile 740c583, cited in audio_sound_table.h and
tools/make_beta_pack.ps1's S1 banner] -- reproduced here by parsing
AUDIO_SOUND_TABLE(X) directly out of source/platform/audio_sound_table.h so
this tool's key list can never drift from the engine's. Nearly every key
ships a bundled CC0 default (assets/README.md, assets/sound/), so it is never
"required" -- omitting it just means the default plays. Exceptions: a handful
of keys are DELIBERATE non-Beta add-ons with no bundled default at all
(wolf/thunder/rain, and since 2026-09-03 minecart.base/minecart.inside too --
see docs/deviations.md) -- those stay silent, matching Beta, until their own
source supplies real audio. minecart.base/inside have NO Beta source to begin
with (not even a real Beta jar has one); see "MODERN (post-Beta) asset
add-ons" below and --modern-minecraft for how those two specifically get
filled in.

MODERN (post-Beta) asset add-ons (--modern-minecraft, optional, independent
of --jar): a small, explicitly-scoped exception to "this tool is Beta-only" --
some engine features are deliberate non-Beta additions that can only be
sourced from a modern, licensed .minecraft install (MODERN_SOUND_SOURCE_KEYS
below the RECORD_STEMS table). Reads a modern launcher's assets/indexes/*.json
+ assets/objects/ content-addressed store (read-only; never a jar), resolves
each add-on's key to its real Mojang sample if the user's own install has it
downloaded, and writes it into the SAME output pack next to the Beta-sourced
sound/ files. Omitting --modern-minecraft leaves those add-ons on their
silent-by-default Beta behaviour -- exactly like omitting --resources leaves
a Beta-sourced key on its bundled default.

MUSIC (music/...) and RECORDS (records/...): copied VERBATIM (.ogg, never
transcoded) -- audio_music.c pull-decodes the compressed file a chunk at a
time via stb_vorbis_open_filename on a worker thread, so writing pre-decoded
PCM here would be exactly backwards (see FINDING below). Neither has a
bundled CC0 default (docs/deviations.md's legal model: never bundle/commit
audio) and BOTH are explicitly allowed to be silently absent by the engine's
own design (audio_music.c's registry-scan comment) -- so neither is ever
"required" either.

CREATIVE MUSIC (music/creative/...) -- maintainer ruling 2026-09-03
(docs/creative_mode.md): a CaveWii_v2 creative world plays the Java
1.7.2+ creative-mode playlist (C418 "Volume Beta": Aria Math, Biome Fest,
Blind Spots, Dreiton, Haunt Muskie, Taswell) instead of Beta 1.7.3's list --
a documented, sanctioned addition on top of the faithful base (this project's
creative mode is itself a net-new, maintainer-approved exception; see
CLAUDE.md). These six tracks were never part of Beta 1.7.3 and therefore
never lived in an old-style `resources/` tree at all -- they only exist in a
MODERN (1.6+) Minecraft installation's asset-hash store. `--modern-minecraft
<dir>` points this tool at that installation's root (the directory that
directly contains `assets/indexes/` and `assets/objects/`); it reads the
newest `assets/indexes/<version>.json` that actually references the six
`minecraft/sounds/music/game/creative/<name>.ogg` objects (or an exact
version via `--modern-minecraft-index <name>`, name without the `.json`),
resolves each by its content hash to `assets/objects/<hash[:2]>/<hash>`,
verifies that file's sha1 against the hash the index itself claims, and
copies it VERBATIM -- the SAME "never transcoded, streamed a chunk at a
time" handling as the Beta `music/` tracks above, for the identical reason
(FINDING below) -- to `music/creative/<name>.ogg`, the exact subpath
audio_music_table.h's `AUDIO_MUSIC_CREATIVE_TABLE` scans. The six basenames
are derived LIVE from that same table (never a hand-typed, independently-
drifting copy). Like Beta music/records, none of the six is ever
"required": a modern install with no creative-mode listening history (the
assets cache only holds objects the game actually streamed at least once)
just leaves the run reporting "not found", and CaveWii_v2 falls back to the
Beta 1.7.3 list for that player, exactly as designed.

REQUIRED TEXTURES (no CC0 fallback under assets/, AND genuinely part of a
real Beta 1.7.3 client jar -- exit code is non-zero if any of these 13 are
missing from the supplied jar): gui/trap.png (dispenser GUI bg), mob/zombie.png,
mob/skeleton.png, mob/spider.png, mob/spider_eyes.png, mob/cow.png,
mob/chicken.png, mob/sheep.png, mob/sheep_fur.png, mob/squid.png,
mob/slime.png, environment/rain.png, environment/snow.png.

NOT required, even though assets/ has no bundled default for them, because
they are NOT part of any real Beta 1.7.3 client jar -- this project ports
them forward from later Beta snapshots as documented, sanctioned deviations
(PROJECT_MEMORY 'cavex-amahpour-delta-2026-07-06', docs/hell_worldgen /
nether-phase0-complete): mob/wolf*.png, mob/pigzombie.png, mob/ghast*.png
(wolves + the Nether/Halloween-Update mobs, all post-1.7.3), misc/mapbg.png
(maps shipped in Beta 1.8, after 1.7.3). A genuine b1.7.3.jar will correctly
report these "missing in source" every time -- that is expected, not a defect,
and must never fail the run.

ALSO not required despite no bundled default, but for a DIFFERENT reason --
this one genuinely IS part of a real Beta 1.7.3 client jar, but the ENGINE
gates its absence cleanly (2026-09-03, lane/parity-compass-clock):
misc/dial.png (TextureWatchFX's dial ring). CaveWii's clock-needle animation
checks whether this file loaded before ever touching it; without it, the
clock item just keeps its pre-existing static icon, exactly as it always
has. A player who owns a real b1.7.3.jar gets it imported like any other
texture (it is in OPTIONAL_NO_FALLBACK_DEST, not REQUIRED_TEXTURES); one who
doesn't loses nothing that worked before this feature existed.

FINDING -- why sounds are transcoded but music/records are not
================================================================================
audio_registry.c's one-shot loader CAN technically decode ogg directly at
load time (it tries "<name>.wav" then "<name>.ogg", and stb_vorbis is linked
into that TU unconditionally, Wii included) -- so a raw copy of Beta's .ogg
step/mob/random sounds would, in isolation, PLAY. But docs/finish_line_plan.md
S0 records a NON-NEGOTIABLE hardening rule: "NO runtime ogg decode on the
Broadway core" -- decode is meant to happen ONCE, at pack-BUILD time, on the
user's PC (tools/sound_transcode.c's own banner: "keeps stb_vorbis off the
Wii's load path for user packs entirely ... a built pack contains only
.wav"). tools/make_beta_pack.ps1 already enforces this by shelling out to the
compiled `sound_transcode` host tool. This importer follows the SAME
established discipline rather than either (a) silently ignoring the hardening
rule for the sake of a simpler pure-Python copy, or (b) reimplementing an OGG
decoder in stdlib Python, or (c) adding a new ffmpeg dependency the project
does not otherwise need: it locates and shells out to the EXISTING,
already-tested `sound_transcode` binary (auto-probed under repo-root
build*/ directories, exactly like make_beta_pack.ps1 does; override with
--transcoder). No format conversion is therefore "invented" here -- zero new
decode logic, byte-identical to the existing pipeline's output. If
sound_transcode is not built, the tool falls back to a RAW COPY of the
source .wav/.ogg (still fully playable -- the engine's own decoder handles
it) and prints an explicit warning that this is a compliance gap against the
S0 rule, naming the fix (build sound_transcode and re-run). Pass
--no-sound-transcode to force the raw-copy path deterministically (used by
this tool's own test).

NOT DONE (explicitly, not silently):
  - gui_2.png / anim.png pixel COMPOSITING (make_beta_pack.ps1 already does
    this with System.Drawing; re-deriving the same per-pixel algorithm in
    stdlib Python here would duplicate, not improve, tested logic). Both
    fall back to the bundled default and are never required.
  - No jar/resources SHAPE auto-detection beyond "is it a zip with the right
    entries" (the ship_distribution_plan.md v1 design's three real-world
    shapes -- era jar+resources, modern launcher versions/+assets/virtual,
    bare jar -- are a --resources UX nicety for a future revision, not a
    correctness requirement: --resources is accepted as any directory
    containing sound/newsound/music/newmusic/streaming subdirectories,
    whatever launcher shape put it there).

Never touches the network. Never writes outside --out/--sd. Refuses to run
without a --jar and a destination.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path
from typing import Optional

TOOL_VERSION = "1.0.0"
FROZEN_MAPPING = None

# This module lives in tools/pack_helper/ since 2026-10-30 so the Pack Helper
# package is self-contained (a release export of tools/pack_helper alone used
# to fail with "importer is not installed"); tools/import_owned_beta_assets.py
# is a thin command-line shim. Source-table paths are only read in a checkout.
REPO_ROOT = Path(__file__).resolve().parents[2]

# ASSETS_DEFAULT_DIR is where is_texture_required() looks for a bundled CC0
# fallback -- normally exactly the repo's own assets/ tree. TESTS-ONLY KNOB:
# CW_IMPORT_ASSETS_ROOT overrides it so tests/test_import_owned_beta_assets.py
# can point this tool at a hermetic, tracked-files-only copy of assets/
# instead of the real one on disk. This matters because /assets/mob/* is
# git-ignored (legal-assets rule, .gitignore:58) so a maintainer's real
# checkout can carry leftover Beta-derived .png files from an earlier real
# --jar run; those files are real and correct there, but they are NOT part
# of the repo's checked-in state, so a test asserting "required" vs.
# "fallback_default" classification must not see them. Unset (the default),
# behaviour is unchanged: ASSETS_DEFAULT_DIR is exactly REPO_ROOT / "assets",
# same as before this knob existed.
ASSETS_DEFAULT_DIR = Path(os.environ.get("CW_IMPORT_ASSETS_ROOT", str(REPO_ROOT / "assets")))
TEXTURE_C = REPO_ROOT / "source" / "platform" / "texture.c"
AUDIO_SOUND_TABLE_H = REPO_ROOT / "source" / "platform" / "audio_sound_table.h"
AUDIO_MUSIC_TABLE_H = REPO_ROOT / "source" / "platform" / "audio_music_table.h"


def load_frozen_mapping(path: Path) -> dict:
    """Load the release helper's source-derived importer map.

    This is the packaged-runtime seam: callers can use the existing importer
    rules without shipping or locating a CaveWii source checkout.
    """
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ValueError("invalid frozen mapping {0}: {1}".format(path, exc))
    if value.get("schema_version") != 1:
        raise ValueError("unsupported frozen mapping schema")
    required = ("texture_paths", "sound_keys", "sound_max_variants",
                "music_keys", "creative_music_keys")
    if any(key not in value for key in required):
        raise ValueError("frozen mapping is missing required fields")
    return value

# sd:/texturepacks/beta-1.7.3 -- the exact folder name tools/provision_wii_sd.ps1
# auto-selects and config.json's paths.texturepack points at on the live card.
SD_PACK_SUBDIR = Path("texturepacks") / "beta-1.7.3"

# The project's documented oracle record for a genuine b1.7.3.jar
# (docs/ship_distribution_plan_2026-08-17.md, Track 2 step 4). INFORMATIONAL
# ONLY -- a legitimately-owned jar can be a different (re-packed / different
# launcher-vendored) build and still be perfectly usable, so this NEVER gates
# the run; it is printed for the user's own peace of mind.
KNOWN_B173_JAR_MD5 = "eae3353fdaa7e10a59b4cb5b45bfa10d"

# Flat, un-namespaced signature entries that must mostly be present for a zip
# to plausibly be a Beta-era Minecraft CLIENT jar. A modern (1.6+) jar keeps
# textures under assets/minecraft/textures/... instead, so this ALSO catches
# someone pointing the tool at a modern jar by mistake.
JAR_SIGNATURE_ENTRIES = [
    "terrain.png", "gui/gui.png", "font/default.png", "item/cart.png",
    "mob/creeper.png", "gui/items.png",
]
MODERN_JAR_MARKER_PREFIX = "assets/minecraft/"

# ---- dest paths the engine will NEVER source from any Minecraft jar --------
FORK_ONLY_DEST = {"controls.png", "pointer.png"}

# ---- composited dest paths (make_beta_pack.ps1's job, not this tool's) -----
COMPOSITE_DEST = {"gui_2.png", "anim.png"}

# ---- Beta-pack base (packs/beta-base), copied verbatim ----------------------
# Since 2026-10-11 the bundled default (assets/) is Earth & Ash, so a Beta pack
# must not fall back to it for these two: pointer.png is the Homebrew Channel
# hand (GPLv2) and anim.png carries Beta's own procedural FX colours (the ice
# overlay from the jar terrain is still make_beta_pack.ps1's / the helper's job).
BETA_BASE_DIR = REPO_ROOT / "packs" / "beta-base"
BETA_BASE_DEST = {"pointer.png", "anim.png"}

# ---- dest -> jar SOURCE, where the two differ (see module docstring) -------
DEST_TO_JAR_SOURCE_OVERRIDE = {
    "gui/chest.png": "gui/container.png",
}

# ---- genuinely engine content, but NOT part of any real b1.7.3 client jar --
NOT_IN_BETA_173_DEST = {
    "mob/wolf.png", "mob/wolf_tame.png", "mob/wolf_angry.png",
    "mob/pigzombie.png", "mob/ghast.png", "mob/ghast_fire.png",
    "misc/mapbg.png",
}

# ---- genuinely part of a real b1.7.3 client jar, no bundled CC0 fallback,
# but the ENGINE ITSELF already gates the feature cleanly when the file is
# absent -- unlike REQUIRED TEXTURES above, never worth a hard failure here.
# (lane/parity-compass-clock, 2026-09-03): misc/dial.png is TextureWatchFX's
# rotated dial-ring texture [740c583]; source/platform/texture.c's
# item_needle_fx_init() checks texture_dial.data != NULL before arming the
# clock-needle FX at all, so a missing dial.png just keeps the pre-existing
# static clock icon (items_object.h's documented asset-gap note) -- the same
# "degrade gracefully, never crash" contract texture_painting's art/kz.png
# has, except kz.png gets a generated CC0 placeholder and dial.png does not
# (a placeholder dial ring would have to fabricate Beta's actual rotation
# artwork, which is a worse idea than just not drawing one).
OPTIONAL_NO_FALLBACK_DEST = {
    "misc/dial.png",
}

# Beta's two jukebox discs [item_types.h ITEM_RECORD_13=2256 / _CAT=2257;
# audio_music.c's audio_music_record_file_stem -- "cat" is lowercase on disk
# even though the in-game item name is "Cat"].
RECORD_STEMS = ("13", "cat")

# ============================================================================
# MODERN (post-Beta) asset add-ons -- ONE shared index/object resolver
# ============================================================================
# This importer is otherwise strictly Beta-era (verify_jar rejects a modern
# jar on sight). Two engine features are DELIBERATE non-Beta add-ons that only
# make sense sourced from a modern, licensed .minecraft install: the minecart
# rolling/riding sound (docs/deviations.md "Minecart rolling sound") and the
# creative-mode music playlist (docs/creative_mode.md). --modern-minecraft
# (build_arg_parser) points at such an install's root (the directory that
# directly contains "assets/indexes/" and "assets/objects/"); optional
# --modern-minecraft-index pins an exact index version instead of the
# auto-discovery find_modern_asset_index below does.
#
# Both add-ons share find_modern_asset_index (locate the right
# assets/indexes/<version>.json) and resolve_modern_object (one entry in it
# -> a verified local object file) rather than each re-deriving the
# index/objects lookup -- that shared pair is the actual growth point this
# section exists for. A future modern-era add-on should reuse both and add
# only its own key table/prefix constant plus a thin process_modern_...
# wrapper.
#
# sound key ("minecart.base") -> modern asset index path (verified against a
# real local index: %APPDATA%\.minecraft\assets\indexes\{24,27,29,32}.json,
# 2026-09-03 -- all four agree on both hash and path).
MODERN_SOUND_SOURCE_KEYS = {
    "minecart.base": "minecraft/sounds/minecart/base.ogg",
    "minecart.inside": "minecraft/sounds/minecart/inside.ogg",
}

# A modern (1.6+) client's per-object asset-hash-store key prefix for the
# Java 1.7.2+ creative-mode playlist (maintainer ruling 2026-09-03,
# docs/creative_mode.md). The six leaf basenames are derived LIVE from
# audio_music_table.h's AUDIO_MUSIC_CREATIVE_TABLE (creative_music_keys()
# below), never hand-typed here.
CREATIVE_MUSIC_OBJECT_PREFIX = "minecraft/sounds/music/game/creative/"


# ============================================================================
# live extraction from the engine's own source (never a hand-copied list)
# ============================================================================

def extract_texture_dest_paths(texture_c_text: str) -> list:
    """Every pack-relative dest path the engine's texture loader reads,
    in the order texture.c requests them. Scraped directly from the real
    TEX_LOAD_FILE / tex_atlas_block / tex_atlas_particles call sites, so this
    list cannot drift from what the engine actually loads. A load that is
    open-coded around tex_read_gfx("<dest>") (fire-anim-0924: anim.png is
    read, repaired for the fire+16 strip, then handed to tex_gfx_load) is
    the same pack read and is scraped too. Raw loader input aliases are
    normalized to canonical output destinations before deduplication."""
    patterns = [
        r'TEX_LOAD_FILE\(&\w+,\s*"([^"]+)"',
        r'tex_read_gfx\("([^"]+)"',
        r'tex_atlas_block\("([^"]+)"',
        r'tex_atlas_particles\("([^"]+)"',
    ]
    seen = set()
    ordered = []
    source_to_dest = {source: dest for dest, source in DEST_TO_JAR_SOURCE_OVERRIDE.items()}
    for pat in patterns:
        for m in re.finditer(pat, texture_c_text):
            p = m.group(1)
            p = source_to_dest.get(p, p)
            if p not in seen:
                seen.add(p)
                ordered.append(p)
    return ordered


def extract_x_macro_table(header_text: str, macro_name: str):
    """Extract [(ID, "key"), ...] from a `#define <macro_name>(X) ...` block
    of backslash-continued lines, stopping at the table's real last row (the
    first line that does NOT end in a backslash) -- exactly how a valid C
    preprocessor macro must be shaped, so this is not a heuristic."""
    lines = header_text.splitlines()
    start = None
    needle = "#define {0}(X)".format(macro_name)
    for i, line in enumerate(lines):
        if line.strip().startswith(needle):
            start = i
            break
    if start is None:
        raise RuntimeError("{0}(X) not found".format(macro_name))
    block_lines = []
    i = start + 1
    while i < len(lines):
        block_lines.append(lines[i])
        continues = lines[i].rstrip().endswith("\\")
        i += 1
        if not continues:
            break
    block_text = "\n".join(block_lines)
    return re.findall(r'X\(\s*([A-Za-z0-9_]+)\s*,\s*"([^"]+)"\s*\)', block_text)


def extract_define_int(text: str, name: str) -> int:
    m = re.search(r'#define\s+' + re.escape(name) + r'\s+(\d+)', text)
    if not m:
        raise RuntimeError("{0} not found".format(name))
    return int(m.group(1))


def sound_keys_and_max_variants():
    if FROZEN_MAPPING is not None:
        return (list(FROZEN_MAPPING["sound_keys"]),
                int(FROZEN_MAPPING["sound_max_variants"]))
    text = AUDIO_SOUND_TABLE_H.read_text(encoding="utf-8")
    keys = [name for _id, name in extract_x_macro_table(text, "AUDIO_SOUND_TABLE")]
    max_variants = extract_define_int(text, "AUDIO_MAX_VARIANTS")
    return keys, max_variants


def music_keys():
    if FROZEN_MAPPING is not None:
        return list(FROZEN_MAPPING["music_keys"])
    text = AUDIO_MUSIC_TABLE_H.read_text(encoding="utf-8")
    return [name for _id, name in extract_x_macro_table(text, "AUDIO_MUSIC_TABLE")]


def creative_music_keys():
    """Leaf basenames of the creative-mode playlist ("aria_math", not
    "creative/aria_math") -- audio_music_table.h's AUDIO_MUSIC_CREATIVE_TABLE
    rows are rooted one level under music/ for the pack-subpath helper; the
    modern asset index and this tool's dest layout only need the leaf."""
    if FROZEN_MAPPING is not None:
        return list(FROZEN_MAPPING["creative_music_keys"])
    text = AUDIO_MUSIC_TABLE_H.read_text(encoding="utf-8")
    rows = extract_x_macro_table(text, "AUDIO_MUSIC_CREATIVE_TABLE")
    return [name.rsplit("/", 1)[-1] for _id, name in rows]


def is_texture_required(dest: str) -> bool:
    if (dest in FORK_ONLY_DEST or dest in COMPOSITE_DEST or dest in NOT_IN_BETA_173_DEST
            or dest in OPTIONAL_NO_FALLBACK_DEST):
        return False
    return not (ASSETS_DEFAULT_DIR / dest).exists()


# ============================================================================
# hashing / misc helpers
# ============================================================================

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def find_sound_transcode() -> Optional[Path]:
    """Probe repo-root build*/ directories for a built sound_transcode(.exe),
    newest first -- the same discovery tools/make_beta_pack.ps1 already uses."""
    names = ("sound_transcode.exe", "sound_transcode")
    build_dirs = [d for d in REPO_ROOT.glob("build*") if d.is_dir()]
    # Fast path: CMake puts the executable at each build dir's TOP LEVEL
    # (build-verify/sound_transcode.exe, build-ci-linux/sound_transcode), so
    # check there first; the recursive walk costs ~25 s per call over WSL
    # drvfs (/mnt/c) on a tree with populated build dirs. Only when no build
    # dir has a top-level copy does the original recursive search run.
    found = [d / name for d in build_dirs for name in names
             if (d / name).is_file()]
    if not found:
        for d in build_dirs:
            for name in names:
                for p in d.rglob(name):
                    if p.is_file():
                        found.append(p)
    if not found:
        return None
    found.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return found[0]


def transcode_or_copy(src_path: Path, data: bytes, transcoder: Optional[Path]):
    """Returns (transcoded: bool, out_bytes: bytes, out_ext: str, note: str)."""
    if transcoder is not None:
        with tempfile.TemporaryDirectory(prefix="cwsnd-") as td:
            out_tmp = Path(td) / "out.wav"
            try:
                proc = subprocess.run(
                    [str(transcoder), str(src_path), str(out_tmp)],
                    capture_output=True, timeout=30,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            except Exception as exc:  # pragma: no cover - defensive
                return (False, data, src_path.suffix.lower(),
                        "transcoder invocation failed ({0}); raw copy fallback".format(exc))
            if proc.returncode == 0 and out_tmp.is_file():
                return (True, out_tmp.read_bytes(), ".wav",
                         "transcoded via sound_transcode (S0 hardening: no runtime ogg "
                         "decode on the Broadway core)")
            return (False, data, src_path.suffix.lower(),
                    "sound_transcode exited {0}; raw copy fallback".format(proc.returncode))
    return (False, data, src_path.suffix.lower(),
            "no sound_transcode found -- raw copy (engine CAN decode .wav/.ogg directly, "
            "but the S0 hardening rule prefers pre-decoded WAV; build sound_transcode and "
            "re-run for full compliance)")


class DestinationError(ValueError):
    pass


_PROTECTED_SUBTREES = (".git", "source", "tools", "tests", "docs", "assets", "third_party")


def guard_destination(dest_root: Path) -> Path:
    """Refuse to write anywhere the repo itself lives -- the write target is
    always --out or <--sd>/texturepacks/beta-1.7.3, never repo source. A
    dest under a gitignored leaf like repo-root texturepacks/ or tmp/ is the
    normal local-dev path and stays allowed."""
    resolved = dest_root.resolve()
    repo_resolved = REPO_ROOT.resolve()
    if resolved == repo_resolved:
        raise DestinationError("refusing to write directly into the repo root: {0}".format(resolved))
    for name in _PROTECTED_SUBTREES:
        protected = (REPO_ROOT / name).resolve()
        if resolved == protected or protected in resolved.parents:
            raise DestinationError(
                "refusing to write inside the repo's {0}/ subtree: {1}".format(name, resolved))
    return resolved


# ============================================================================
# jar verification
# ============================================================================

def verify_jar(jar_path: Path):
    """Returns (ok, reasons, ZipFile|None)."""
    if not zipfile.is_zipfile(jar_path):
        return False, ["not a valid zip/jar file"], None
    zf = zipfile.ZipFile(jar_path)
    names = set(zf.namelist())
    reasons = []
    if any(n.startswith(MODERN_JAR_MARKER_PREFIX) for n in names):
        reasons.append(
            "looks like a MODERN (namespaced) Minecraft jar ({0}... present) -- "
            "point this tool at a Beta-era client jar instead".format(MODERN_JAR_MARKER_PREFIX))
    missing_sig = [p for p in JAR_SIGNATURE_ENTRIES if p not in names]
    if len(missing_sig) > len(JAR_SIGNATURE_ENTRIES) // 2:
        reasons.append("missing most Beta-era signature entries: {0}".format(missing_sig))
    return (len(reasons) == 0), reasons, zf


# ============================================================================
# per-category processing
# ============================================================================

def _pack_helper_core():
    from . import core
    return core


def convert_textures_like_the_helper(jar_path: Path):
    """ONE conversion path (2026-10-30 jar-import audit F4): the textures this
    CLI writes are the Pack Helper's own prepare() output for the same jar --
    the same layout conversion, grass/foliage colour maps, gui_2.png HUD
    composite, anim.png ice composite and Beta pointer -- so the CLI and the
    helper give byte-identical textures. Before this the CLI raw-copied jar
    PNGs and skipped the composites (audit F3).

    Returns (dest -> bytes, None) or (None, the helper's error message)."""
    core = _pack_helper_core()
    with tempfile.TemporaryDirectory(prefix="cw-import-textures-") as td:
        result = core.prepare(jar_path, Path(td))
        if not result["ok"]:
            return None, result["errors"][0]["message"]
        prepared = Path(result["path"])
        converted = {}
        for row in result["manifest"]["files"]:
            if row["path"].endswith(".png"):
                converted[row["path"]] = (prepared / row["path"]).read_bytes()
        return converted, None


def process_textures(zf: zipfile.ZipFile, dest_root: Path, dry_run: bool,
                     converted: Optional[dict] = None) -> list:
    """*converted* (dest -> bytes) is convert_textures_like_the_helper()'s
    output; every texture written comes from it when given."""
    converted = converted or {}
    if FROZEN_MAPPING is not None:
        dest_paths = list(FROZEN_MAPPING["texture_paths"])
    else:
        texture_c_text = TEXTURE_C.read_text(encoding="utf-8", errors="replace")
        dest_paths = extract_texture_dest_paths(texture_c_text)
    jar_names = set(zf.namelist())
    results = []
    for dest in dest_paths:
        entry = {"category": "texture", "dest": dest, "required": False}
        if dest in BETA_BASE_DEST and not (BETA_BASE_DIR / dest).is_file():
            entry["required"] = True
            entry["status"] = "MISSING_REQUIRED"
            entry["reason"] = "packs/beta-base/" + dest + " missing: the pack would inherit Earth & Ash art"
            results.append(entry)
            continue
        if dest in BETA_BASE_DEST:
            data = converted.get(dest) or (BETA_BASE_DIR / dest).read_bytes()
            entry["status"] = "beta_base"
            # Not a jar entry: "source"/"source_sha256" are reserved for jar paths.
            entry["base_source"] = "packs/beta-base/" + dest
            entry["base_sha256"] = sha256_bytes(data)
            if not dry_run:
                out_path = dest_root / dest
                out_path.parent.mkdir(parents=True, exist_ok=True)
                out_path.write_bytes(data)
                entry["dest_sha256"] = sha256_bytes(data)
            results.append(entry)
            continue
        if dest in COMPOSITE_DEST and dest in converted:
            data = converted[dest]
            entry["status"] = "composited"
            if not dry_run:
                out_path = dest_root / dest
                out_path.parent.mkdir(parents=True, exist_ok=True)
                out_path.write_bytes(data)
                entry["dest_sha256"] = sha256_bytes(data)
            results.append(entry)
            continue
        if dest in COMPOSITE_DEST:
            entry["status"] = "composite_not_implemented"
            entry["note"] = ("composited by tools/make_beta_pack.ps1 (pixel overlay), "
                              "not this importer; falls back to the bundled default")
            results.append(entry)
            continue
        if dest in FORK_ONLY_DEST:
            entry["status"] = "fork_only"
            entry["note"] = "engine/fork-original art; no Minecraft jar supplies this path"
            results.append(entry)
            continue

        jar_source = DEST_TO_JAR_SOURCE_OVERRIDE.get(dest, dest)
        required = is_texture_required(dest)
        entry["source"] = jar_source
        entry["required"] = required

        if jar_source in jar_names:
            data = zf.read(jar_source)
            entry["source_sha256"] = sha256_bytes(data)
            data = converted.get(dest, data)
            entry["size"] = len(data)
            entry["status"] = "copied"
            if not dry_run:
                out_path = dest_root / dest
                out_path.parent.mkdir(parents=True, exist_ok=True)
                out_path.write_bytes(data)
                entry["dest_sha256"] = sha256_bytes(data)
        else:
            if required:
                entry["status"] = "MISSING_REQUIRED"
            else:
                entry["status"] = "fallback_default"
                entry["reason"] = ("not_in_beta_1_7_3" if dest in NOT_IN_BETA_173_DEST
                                    else "optional_gated_feature" if dest in OPTIONAL_NO_FALLBACK_DEST
                                    else "bundled_default")
        results.append(entry)
    # Text is optional and copied verbatim; it is not a texture-map entry.
    splash = "title/splashes.txt"
    if splash in jar_names:
        data = zf.read(splash)
        entry = {"category": "text", "dest": splash, "source": splash,
                 "required": False, "status": "copied", "size": len(data),
                 "source_sha256": sha256_bytes(data)}
        if not dry_run:
            out_path = dest_root / splash
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(data)
            entry["dest_sha256"] = entry["source_sha256"]
        results.append(entry)
    return results


_VARIANT_FILE_RE_CACHE = {}


def _variant_pattern(base: str) -> "re.Pattern":
    pat = _VARIANT_FILE_RE_CACHE.get(base)
    if pat is None:
        pat = re.compile(r'^' + re.escape(base) + r'\d*\.(?:wav|ogg)$', re.IGNORECASE)
        _VARIANT_FILE_RE_CACHE[base] = pat
    return pat


def _resource_file_key(path: Path):
    # Match the in-game importer's case-insensitive filename order on every
    # host. Path ordering/globbing otherwise changes with the host filesystem.
    return path.name.lower(), path.name


def process_sounds(resources_dir: Optional[Path], dest_root: Path, dry_run: bool,
                    transcoder: Optional[Path], strict_transcode: bool = False) -> list:
    keys, max_variants = sound_keys_and_max_variants()
    results = []
    for key in keys:
        entry = {"category": "sound", "key": key, "required": False}
        rel_parts = key.split(".")
        base = rel_parts[-1]
        subdir = "/".join(rel_parts[:-1])  # "" for a bare (dot-free) key

        if resources_dir is None:
            entry["status"] = "no_resources"
            entry["reason"] = "bundled_default"
            results.append(entry)
            continue

        pat = _variant_pattern(base)
        candidates = []
        for root_name in ("sound", "newsound"):  # Beta's union, no precedence
            d = resources_dir / root_name / subdir if subdir else resources_dir / root_name
            if d.is_dir():
                for f in sorted(d.iterdir(), key=_resource_file_key):
                    if f.is_file() and pat.match(f.name):
                        candidates.append(f)

        seen_hash = set()
        deduped = []
        for f in candidates:
            data = f.read_bytes()
            h = sha256_bytes(data)
            if h not in seen_hash:  # byte-identical union dedup (sound==newsound overlap)
                seen_hash.add(h)
                deduped.append((f, data, h))

        if not deduped:
            entry["status"] = "fallback_default"
            entry["reason"] = "bundled_default"
            results.append(entry)
            continue

        out_dir = dest_root / "sound" / subdir if subdir else dest_root / "sound"
        variants = []
        written = 0
        for n, (f, data, h) in enumerate(deduped, start=1):
            if n <= max_variants - 1:
                out_stem = "{0}{1}".format(base, n)
            elif n == max_variants:
                out_stem = base
            else:
                variants.append({"source": str(f), "status": "skipped_variant_overflow"})
                continue
            transcoded, out_bytes, out_ext, note = transcode_or_copy(f, data, transcoder)
            if strict_transcode and not transcoded:
                variants.append({
                    "source": str(f), "source_sha256": h,
                    "status": "skipped_transcode_failed", "note": note,
                })
                continue
            out_path = out_dir / "{0}{1}".format(out_stem, out_ext)
            vr = {
                "source": str(f), "source_sha256": h,
                "dest": str(out_path.relative_to(dest_root)).replace("\\", "/"),
                "status": "transcoded" if transcoded else "copied",
                "note": note,
            }
            if not dry_run:
                out_dir.mkdir(parents=True, exist_ok=True)
                out_path.write_bytes(out_bytes)
                vr["dest_sha256"] = sha256_bytes(out_bytes)
            variants.append(vr)
            written += 1

        entry["status"] = "copied" if written else "fallback_default"
        entry["variant_count"] = written
        entry["variants"] = variants
        results.append(entry)
    return results


def process_music(resources_dir: Optional[Path], dest_root: Path, dry_run: bool) -> list:
    results = []
    if resources_dir is None:
        for key in music_keys():
            results.append({"category": "music", "key": key, "status": "no_resources",
                             "required": False})
        return results

    out_dir = dest_root / "music"
    seen_names = set()
    any_found = False
    for src_dirname in ("music", "newmusic"):  # BOTH feed the one soundPoolMusic pool
        d = resources_dir / src_dirname
        if not d.is_dir():
            continue
        for f in sorted(d.iterdir(), key=_resource_file_key):
            if not f.is_file() or f.suffix.lower() != ".ogg":
                continue
            any_found = True
            data = f.read_bytes()
            entry = {
                "category": "music", "source": str(f), "dest": "music/{0}".format(f.name),
                "required": False, "source_sha256": sha256_bytes(data), "size": len(data),
            }
            if f.name in seen_names:
                entry["status"] = "skipped_duplicate_name"
            else:
                seen_names.add(f.name)
                entry["status"] = "copied"
                if not dry_run:
                    out_dir.mkdir(parents=True, exist_ok=True)
                    (out_dir / f.name).write_bytes(data)
                    entry["dest_sha256"] = entry["source_sha256"]
            results.append(entry)

    if not any_found:
        results.append({
            "category": "music", "status": "none_found", "required": False,
            "note": ("no music/*.ogg or newmusic/*.ogg found -- ships silently musicless "
                     "(no CC0 default exists, by design -- audio_music.c's registry scan)"),
        })
    return results


def process_records(resources_dir: Optional[Path], dest_root: Path, dry_run: bool) -> list:
    results = []
    if resources_dir is None:
        for stem in RECORD_STEMS:
            results.append({"category": "record", "stem": stem, "status": "no_resources",
                             "required": False})
        return results

    d = resources_dir / "streaming"
    out_dir = dest_root / "records"
    found_stems = set()
    if d.is_dir():
        for f in sorted(d.iterdir(), key=_resource_file_key):
            if not f.is_file() or f.suffix.lower() != ".ogg":
                continue
            stem_lower = f.stem.lower()
            if stem_lower not in RECORD_STEMS or stem_lower in found_stems:
                continue
            data = f.read_bytes()
            out_name = "{0}.ogg".format(stem_lower)
            entry = {
                "category": "record", "source": str(f), "dest": "records/{0}".format(out_name),
                "required": False, "source_sha256": sha256_bytes(data), "size": len(data),
                "status": "copied",
            }
            if not dry_run:
                out_dir.mkdir(parents=True, exist_ok=True)
                (out_dir / out_name).write_bytes(data)
                entry["dest_sha256"] = entry["source_sha256"]
            results.append(entry)
            found_stems.add(stem_lower)

    for stem in RECORD_STEMS:
        if stem not in found_stems:
            results.append({
                "category": "record", "stem": stem, "status": "fallback_default",
                "required": False, "reason": "no_default_by_design",
                "note": "jukebox still accepts/ejects the record, it just plays nothing",
            })
    return results


def find_modern_asset_index(modern_root: Path, index_version: Optional[str],
                             wanted_object_keys: set):
    """Returns (index_path, index_dict_or_None). If index_version is given,
    reads exactly assets/indexes/<index_version>.json (or fails soft with
    None). Otherwise scans every assets/indexes/*.json newest-mtime-first and
    returns the first whose "objects" map references at least one of
    wanted_object_keys -- a real .minecraft can carry many historical
    version indexes, and a given add-on's tracks/sounds may only be cached
    under whichever version the player last played with sound on. If none
    reference any of them, falls back to the single newest index (so the
    caller can still report a clean "not found" rather than "no index
    exists" for what may be a perfectly normal modern install). Shared by
    every modern-era add-on below (minecart sound, creative music) so none
    of them re-derives this lookup."""
    indexes_dir = modern_root / "assets" / "indexes"
    if not indexes_dir.is_dir():
        return None, None

    def _load(p: Path):
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            return None

    if index_version:
        p = indexes_dir / "{0}.json".format(index_version)
        return (p, _load(p)) if p.is_file() else (p, None)

    candidates = sorted(indexes_dir.glob("*.json"),
                         key=lambda p: p.stat().st_mtime, reverse=True)
    for p in candidates:
        data = _load(p)
        if data and wanted_object_keys & set(data.get("objects", {}).keys()):
            return p, data
    if candidates:
        return candidates[0], _load(candidates[0])
    return None, None


def resolve_modern_object(modern_root: Path, objects: dict, rel_path: str,
                           verify_hash: bool = True):
    """One assets/indexes "objects" entry (rel_path, e.g.
    "minecraft/sounds/minecart/base.ogg") -> (object_path, sha1_hex,
    data_bytes) if the index lists it, the object file is actually present
    on disk (a real launcher install can have an index entry for an asset it
    has not downloaded), and -- when verify_hash -- the file's own content
    hashes to what the index claims (an asset cache can in principle hold a
    corrupt/truncated object). Returns (None, None, None) on any failure;
    never raises. Shared by every modern-era add-on below so none of them
    re-derives this lookup+verification."""
    entry = objects.get(rel_path) if objects else None
    if not entry or "hash" not in entry:
        return None, None, None
    h = entry["hash"]
    obj_path = modern_root / "assets" / "objects" / h[:2] / h
    if not obj_path.is_file():
        return None, None, None
    data = obj_path.read_bytes()
    if verify_hash and hashlib.sha1(data).hexdigest() != h:
        return None, None, None
    return obj_path, h, data


def process_modern_sounds(modern_root: Optional[Path], dest_root: Path,
                           dry_run: bool, transcoder: Optional[Path]) -> list:
    """Mirrors process_sounds' shape (same result-entry fields, same
    transcode-or-copy path) but reads from a modern assets index via the
    shared find_modern_asset_index/resolve_modern_object pair instead of a
    Beta resources tree, and writes exactly ONE bare file per key (these are
    singular real samples, not a Beta-style multi-variant pool)."""
    results = []
    if modern_root is None:
        for key in MODERN_SOUND_SOURCE_KEYS:
            results.append({"category": "modern_sound", "key": key,
                             "status": "no_modern_source", "required": False,
                             "reason": "bundled_default"})
        return results

    wanted_keys = set(MODERN_SOUND_SOURCE_KEYS.values())
    index_path, index_data = find_modern_asset_index(modern_root, None, wanted_keys)
    if index_data is None:
        note = ("no assets/indexes/*.json found under --modern-minecraft {0}"
                .format(modern_root) if index_path is None else
                "failed to parse {0}".format(index_path))
        print("[import_owned_beta_assets] warning: {0} -- skipping modern "
              "sound import.".format(note), file=sys.stderr)
        for key in MODERN_SOUND_SOURCE_KEYS:
            results.append({"category": "modern_sound", "key": key,
                             "status": "no_modern_source", "required": False,
                             "reason": "bundled_default", "note": note})
        return results

    objects = index_data.get("objects", {})
    for key, rel_path in MODERN_SOUND_SOURCE_KEYS.items():
        entry = {"category": "modern_sound", "key": key, "source": rel_path,
                  "required": False, "index": index_path.name}
        obj_path, obj_hash, data = resolve_modern_object(modern_root, objects,
                                                           rel_path)
        if obj_path is None:
            entry["status"] = "fallback_default"
            entry["reason"] = "bundled_default"
            results.append(entry)
            continue

        entry["index_sha1"] = obj_hash
        entry["source_sha256"] = sha256_bytes(data)
        entry["size"] = len(data)
        subdir = "/".join(key.split(".")[:-1])
        base = key.split(".")[-1]
        out_dir = dest_root / "sound" / subdir
        # A modern assets/objects/<hash[:2]>/<hash> file has NO extension --
        # unlike a Beta resources file, its name carries no format hint at
        # all. transcode_or_copy derives its raw-copy extension (and a real
        # sound_transcode subprocess may derive its input format) from the
        # SOURCE path's suffix, so hand it a same-bytes temp copy named with
        # the extension the asset index itself already told us (rel_path's),
        # rather than the extension-less object path.
        src_suffix = Path(rel_path).suffix or ".ogg"
        with tempfile.TemporaryDirectory(prefix="cwmodsnd-") as td:
            named_src = Path(td) / ("src" + src_suffix)
            named_src.write_bytes(data)
            transcoded, out_bytes, out_ext, note = transcode_or_copy(
                named_src, data, transcoder)
        out_path = out_dir / "{0}{1}".format(base, out_ext)
        entry["dest"] = str(out_path.relative_to(dest_root)).replace("\\", "/")
        entry["status"] = "transcoded" if transcoded else "copied"
        entry["note"] = note
        if not dry_run:
            out_dir.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(out_bytes)
            entry["dest_sha256"] = sha256_bytes(out_bytes)
        results.append(entry)
    return results


def process_creative_music(modern_root: Optional[Path],
                            index_version: Optional[str], dest_root: Path,
                            dry_run: bool) -> list:
    leaves = creative_music_keys()
    results = []
    if modern_root is None:
        for leaf in leaves:
            results.append({"category": "creative_music", "key": leaf,
                             "status": "no_resources", "required": False})
        return results

    wanted_keys = {"{0}{1}.ogg".format(CREATIVE_MUSIC_OBJECT_PREFIX, leaf)
                   for leaf in leaves}
    index_path, index_data = find_modern_asset_index(modern_root, index_version,
                                                       wanted_keys)
    if index_data is None:
        note = ("no assets/indexes/*.json found under --modern-minecraft {0}"
                .format(modern_root) if index_path is None else
                "failed to parse {0}".format(index_path))
        for leaf in leaves:
            results.append({"category": "creative_music", "key": leaf,
                             "status": "no_index_found", "required": False,
                             "note": note})
        return results

    objects = index_data.get("objects", {})
    out_dir = dest_root / "music" / "creative"
    any_found = False
    for leaf in leaves:
        object_key = "{0}{1}.ogg".format(CREATIVE_MUSIC_OBJECT_PREFIX, leaf)
        entry = {"category": "creative_music", "key": leaf,
                  "dest": "music/creative/{0}.ogg".format(leaf),
                  "required": False, "index": index_path.name}
        obj_path, obj_hash, data = resolve_modern_object(modern_root, objects,
                                                           object_key)
        if obj_path is None:
            # resolve_modern_object collapses "not in the index at all",
            # "in the index but missing on disk" and "on disk but corrupt"
            # to one None -- re-derive which for the note (same information
            # the pre-consolidation inline checks gave, byte-for-byte).
            raw = objects.get(object_key)
            if not raw or "hash" not in raw:
                entry["status"] = "not_in_index"
                entry["reason"] = "no_default_by_design"
                entry["note"] = ("object key not present in {0}: {1} (the "
                                  "modern client's asset cache only holds "
                                  "objects it actually streamed at least "
                                  "once)".format(index_path.name, object_key))
            else:
                h = raw["hash"]
                src_path = modern_root / "assets" / "objects" / h[:2] / h
                entry["index_sha1"] = h
                if not src_path.is_file():
                    entry["status"] = "object_file_missing"
                    entry["note"] = ("the index references {0} but the object "
                                      "file is not on disk (incomplete asset "
                                      "cache -- play a creative world once "
                                      "with sound on in the modern client to "
                                      "populate it, then re-run)".format(h))
                else:
                    entry["status"] = "hash_mismatch"
                    entry["note"] = ("object file content does not match the "
                                      "index hash -- skipped")
            results.append(entry)
            continue

        any_found = True
        entry["index_sha1"] = obj_hash
        entry["source_sha1"] = obj_hash
        entry["status"] = "copied"
        entry["source_sha256"] = sha256_bytes(data)
        entry["size"] = len(data)
        if not dry_run:
            out_dir.mkdir(parents=True, exist_ok=True)
            out_path = out_dir / "{0}.ogg".format(leaf)
            out_path.write_bytes(data)
            entry["dest_sha256"] = entry["source_sha256"]
        results.append(entry)

    if not any_found:
        results.append({
            "category": "creative_music", "status": "none_found", "required": False,
            "note": ("no minecraft/sounds/music/game/creative/*.ogg objects found "
                     "in the supplied modern index -- creative worlds correctly "
                     "fall back to the Beta 1.7.3 list until these are imported"),
        })
    return results

# ============================================================================
# report + manifest
# ============================================================================

def _count(results, status):
    return sum(1 for e in results if e.get("status") == status)


def print_report(texture_results, sound_results, music_results, record_results,
                  modern_sound_results=(), creative_music_results=()) -> None:
    print("")
    print("=== texture report ({0} pack-relative read-paths) ===".format(len(texture_results)))
    print("  copied:                {0}".format(_count(texture_results, "copied")))
    print("  fallback to default:   {0}".format(_count(texture_results, "fallback_default")))
    print("  fork-only (skipped):   {0}".format(_count(texture_results, "fork_only")))
    print("  composited:            {0}".format(_count(texture_results, "composited")))
    print("  composite (skipped):   {0}".format(_count(texture_results, "composite_not_implemented")))
    missing = [e for e in texture_results if e["status"] == "MISSING_REQUIRED"]
    print("  MISSING (required):    {0}".format(len(missing)))
    for e in missing:
        if "source" in e:
            print("    - {0}  (expected in jar at: {1})".format(e["dest"], e["source"]))
        else:  # beta-base entries are not jar files (EAD review)
            print("    - {0}  ({1})".format(e["dest"], e.get("reason", "missing")))

    total_keys = len(sound_results)
    overlaid = [e for e in sound_results if e["status"] == "copied"]
    total_variants = sum(e.get("variant_count", 0) for e in sound_results)
    print("")
    print("=== sound report (one-shot pool, {0} keys) ===".format(total_keys))
    print("  keys overlaid from your resources: {0}".format(len(overlaid)))
    print("  keys on the bundled default:       {0}".format(total_keys - len(overlaid)))
    print("  total variant files written:       {0}".format(total_variants))

    print("")
    print("=== music report ===")
    m_copied = [e for e in music_results if e.get("status") == "copied"]
    print("  tracks copied: {0}".format(len(m_copied)))

    print("")
    print("=== records report ===")
    r_copied = [e for e in record_results if e.get("status") == "copied"]
    names = ", ".join(e["dest"] for e in r_copied) if r_copied else "none"
    print("  records copied: {0} ({1})".format(len(r_copied), names))

    if modern_sound_results:
        print("")
        print("=== modern sound add-ons report ({0} keys) ===".format(
            len(modern_sound_results)))
        ms_copied = [e for e in modern_sound_results
                     if e.get("status") in ("copied", "transcoded")]
        if ms_copied:
            for e in ms_copied:
                print("  imported: {0} <- {1} ({2})".format(
                    e["dest"], e["source"], e["status"]))
        else:
            print("  none imported -- pass --modern-minecraft <dir> to import "
                  "the real minecart rolling/riding sound; these add-ons stay "
                  "silent (Beta-faithful) without it")

    if creative_music_results:
        print("")
        print("=== creative music report (Java 1.7.2+ playlist, 6 tracks) ===")
        cm_copied = [e for e in creative_music_results if e.get("status") == "copied"]
        cm_names = ", ".join(e["key"] for e in cm_copied) if cm_copied else "none"
        print("  tracks copied: {0} ({1})".format(len(cm_copied), cm_names))
        if creative_music_results[0].get("status") == "no_resources":
            print("  (no --modern-minecraft supplied -- creative worlds fall back "
                  "to the Beta 1.7.3 list)")


def build_manifest(args, jar_sha256, jar_md5, md5_match, dest_root, all_results) -> dict:
    return {
        "tool": "tools/import_owned_beta_assets.py",
        "tool_version": TOOL_VERSION,
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "jar": {
            "path": str(args.jar.resolve()),
            "sha256": jar_sha256,
            "md5": jar_md5,
            "matches_documented_b173_md5": md5_match,
        },
        "resources": {
            "path": str(args.resources.resolve()) if args.resources else None,
        },
        "modern_minecraft": {
            "path": str(args.modern_minecraft.resolve()) if args.modern_minecraft else None,
            "index_version": args.modern_minecraft_index,
        },
        "destination_root": str(dest_root),
        "sd_mode": bool(args.sd),
        "dry_run": bool(args.dry_run),
        "entries": all_results,
    }


# ============================================================================
# CLI
# ============================================================================

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="import_owned_beta_assets.py",
        description="Import textures/sounds from YOUR OWN Minecraft Beta 1.7.3 "
                     "client jar + resources into CaveWii_v2's texture-pack layout.")
    p.add_argument("--jar", required=True, type=Path,
                    help="path to your own b1.7.3 client jar (a zip)")
    p.add_argument("--resources", type=Path, default=None,
                    help="path to your .minecraft resources dir (sound/newsound/"
                         "music/newmusic/streaming); omit for textures-only")
    p.add_argument("--modern-minecraft", type=Path, default=None,
                    help="OPTIONAL: path to a MODERN (1.6+), licensed "
                         ".minecraft install root (the directory directly "
                         "containing assets/indexes/ and assets/objects/); "
                         "independent of --jar/--resources. Feeds TWO "
                         "post-Beta add-ons that have no Beta source at all: "
                         "the minecart rolling/riding sound "
                         "(MODERN_SOUND_SOURCE_KEYS; docs/deviations.md) and "
                         "the six-track Java 1.7.2+ creative-mode playlist "
                         "(docs/creative_mode.md). Omit to leave both on "
                         "their silent/Beta-fallback default behaviour.")
    p.add_argument("--modern-minecraft-index", default=None,
                    help="pin an exact assets/indexes/<name>.json version "
                         "(name without .json) instead of auto-discovering "
                         "the newest index that references a given add-on's "
                         "tracks/sounds; rarely needed. Currently only the "
                         "creative-music add-on's lookup accepts a pin -- "
                         "minecart sound always auto-discovers.")
    dest_group = p.add_mutually_exclusive_group(required=True)
    dest_group.add_argument("--out", type=Path,
                             help="write the pack directly into this directory")
    dest_group.add_argument("--sd", type=Path,
                             help="an SD card drive/mount; writes into "
                                  "<sd>/texturepacks/beta-1.7.3/")
    p.add_argument("--dry-run", action="store_true",
                    help="report what would happen; write nothing")
    p.add_argument("--manifest", action="store_true",
                    help="write <dest>/import_manifest.json (sha256 of every "
                         "input+output, jar hash, tool version, timestamp)")
    p.add_argument("--expect-jar-sha", default=None,
                    help="optional: refuse unless the jar's sha256 matches this "
                         "(never required)")
    p.add_argument("--transcoder", type=Path, default=None,
                    help="explicit path to a built sound_transcode(.exe); "
                         "auto-probed under build*/ if omitted")
    p.add_argument("--no-sound-transcode", action="store_true",
                    help="never invoke sound_transcode; always raw-copy sound "
                         "one-shots (deterministic; used by this tool's own test)")
    p.add_argument("--mapping", type=Path, default=None,
                    help="frozen schema-v1 importer mapping; avoids reading "
                         "CaveWii source tables at runtime")
    return p


def main(argv=None) -> int:
    global FROZEN_MAPPING
    args = build_arg_parser().parse_args(argv)

    FROZEN_MAPPING = None
    if args.mapping is not None:
        try:
            FROZEN_MAPPING = load_frozen_mapping(args.mapping)
        except ValueError as exc:
            print("error: {0}".format(exc), file=sys.stderr)
            return 2

    print("[import_owned_beta_assets] LEGAL: these assets stay on YOUR machine; "
          "nothing produced by this tool is for redistribution. No network access "
          "is ever performed.")

    if not args.jar.is_file():
        print("error: --jar not found: {0}".format(args.jar), file=sys.stderr)
        return 2
    if args.resources is not None and not args.resources.is_dir():
        print("error: --resources is not a directory: {0}".format(args.resources),
              file=sys.stderr)
        return 2
    if args.resources is not None:
        # Same rule as the Pack Helper: a picked .minecraft (or version
        # folder) means its resources/ folder (2026-10-30 audit).
        from . import discovery
        found = discovery.find_resources_folder(args.resources)
        if found is None:
            print("warning: no Beta 1.7.3 sounds or music were found in {0}; pick the "
                  "\"resources\" folder inside .minecraft (start Beta 1.7.3 once in the "
                  "launcher if it is missing). Importing textures only.".format(args.resources),
                  file=sys.stderr)
            args.resources = None
        elif Path(found) != args.resources:
            print("[import_owned_beta_assets] sounds: using {0}".format(found))
            args.resources = Path(found)
    if args.modern_minecraft is not None and not args.modern_minecraft.is_dir():
        print("error: --modern-minecraft is not a directory: {0}".format(
            args.modern_minecraft), file=sys.stderr)
        return 2

    raw_dest = (args.sd / SD_PACK_SUBDIR) if args.sd else args.out
    try:
        dest_root = guard_destination(raw_dest)
    except DestinationError as exc:
        print("error: {0}".format(exc), file=sys.stderr)
        return 2

    jar_bytes = args.jar.read_bytes()
    jar_sha256 = sha256_bytes(jar_bytes)
    jar_md5 = hashlib.md5(jar_bytes).hexdigest()

    ok, reasons, zf = verify_jar(args.jar)
    if not ok:
        print("error: --jar does not look like a Minecraft Beta 1.7.3 client jar:",
              file=sys.stderr)
        for r in reasons:
            print("  - {0}".format(r), file=sys.stderr)
        return 3
    if args.expect_jar_sha and args.expect_jar_sha.lower() != jar_sha256:
        print("error: --expect-jar-sha mismatch (jar sha256 is {0})".format(jar_sha256),
              file=sys.stderr)
        return 3

    md5_match = (jar_md5 == KNOWN_B173_JAR_MD5)
    print("[import_owned_beta_assets] jar sha256={0}".format(jar_sha256))
    print("[import_owned_beta_assets] jar md5={0} ({1} the documented b1.7.3.jar md5 "
          "in docs/ship_distribution_plan_2026-08-17.md -- informational only)".format(
              jar_md5, "matches" if md5_match else "does not match"))

    transcoder = None
    if not args.no_sound_transcode:
        if args.transcoder and args.transcoder.is_file():
            transcoder = args.transcoder
        else:
            transcoder = find_sound_transcode()
    if transcoder:
        print("[import_owned_beta_assets] sound_transcode found: {0}".format(transcoder))
    elif args.no_sound_transcode:
        print("[import_owned_beta_assets] --no-sound-transcode: raw-copying all one-shot sounds")
    else:
        print("[import_owned_beta_assets] sound_transcode NOT found under build*/ -- one-shot "
              "sound/ files will be a RAW COPY (playable, but a compliance gap against the S0 "
              "'no runtime ogg decode on the Broadway core' hardening rule; build "
              "`sound_transcode` and re-run for full compliance)")

    if args.dry_run:
        print("[import_owned_beta_assets] --dry-run: no files will be written")
    if not args.dry_run:
        dest_root.mkdir(parents=True, exist_ok=True)

    converted, texture_error = convert_textures_like_the_helper(args.jar)
    if texture_error:
        print("error: {0}".format(texture_error), file=sys.stderr)
        return 1
    texture_results = process_textures(zf, dest_root, args.dry_run, converted)
    # Whatever else the helper produced (the grass/foliage colour maps are
    # read by a different loader than texture.c's table) lands too, so the
    # pack is the helper's pack file for file.
    listed = {e["dest"] for e in texture_results}
    for dest in sorted(set(converted) - listed):
        data = converted[dest]
        entry = {"category": "texture", "dest": dest, "required": False,
                 "status": "copied", "size": len(data)}
        if not args.dry_run:
            out_path = dest_root / dest
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(data)
            entry["dest_sha256"] = sha256_bytes(data)
        texture_results.append(entry)
    sound_results = process_sounds(args.resources, dest_root, args.dry_run, transcoder)
    music_results = process_music(args.resources, dest_root, args.dry_run)
    record_results = process_records(args.resources, dest_root, args.dry_run)
    modern_sound_results = process_modern_sounds(args.modern_minecraft, dest_root,
                                                  args.dry_run, transcoder)
    creative_music_results = process_creative_music(
        args.modern_minecraft, args.modern_minecraft_index, dest_root, args.dry_run)
    all_results = (texture_results + sound_results + music_results + record_results
                   + modern_sound_results + creative_music_results)

    print_report(texture_results, sound_results, music_results, record_results,
                 modern_sound_results, creative_music_results)

    if args.manifest:
        if args.dry_run:
            print("\n[import_owned_beta_assets] --dry-run: manifest not written")
        else:
            manifest = build_manifest(args, jar_sha256, jar_md5, md5_match, dest_root, all_results)
            manifest_path = dest_root / "import_manifest.json"
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True),
                                      encoding="utf-8")
            print("\n[import_owned_beta_assets] manifest written: {0}".format(manifest_path))

    missing = [e for e in texture_results if e["status"] == "MISSING_REQUIRED"]
    if missing:
        print("\n[import_owned_beta_assets] FAILED: {0} required file(s) missing from the "
              "supplied jar (no CC0 fallback exists for these):".format(len(missing)))
        for e in missing:
            print("  - {0}".format(e["dest"]))
        return 1

    print("\n[import_owned_beta_assets] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
