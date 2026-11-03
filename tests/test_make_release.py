#!/usr/bin/env python3
"""Host-side regression for tools/make_release.sh, the release packaging
pipeline (Codex review P0: no release pipeline existed at all; CI shipped a
bare stock .dol, meta.xml said 0.0.1-dev dated May, README called
sounds/pistons/repeaters "planned").

HERMETICITY (2026-09-02 fix-up): this test used to run make_release.sh
in-place against the enclosing dev repo's own git state via its default
--repo resolution. That broke under WSL when the worktree's .git file
points at a Windows-path gitdir (`gitdir: C:/Users/.../.git/worktrees/...`)
-- WSL's git cannot resolve that path, so every `git show`/`git rev-parse`
inside make_release.sh failed with "cannot resolve --commit 'HEAD' in
<repo>", even though nothing about the release script itself was broken.
A tooling test must not depend on the enclosing repo's git plumbing being
resolvable from wherever the test happens to run (WSL, a CI checkout, a
worktree on another OS) -- that is a real defect in the TEST, not a
property to special-case around.

The fix: build a throwaway, fully self-contained git repository in a temp
dir for every run (git init + local user.name/email + a copy of
tools/make_release.sh, tools/gen_meta_xml.sh, tools/meta.xml.in, VERSION, a
LICENSE, a minimal config_wii.json, and a tiny assets/ subset -- including
one file that must be excluded -- then one commit), and drive
make_release.sh against THAT repo via the script's own `--repo <path>`
option (added alongside this fix). Every `git show <commit>:<path>` then
resolves inside the scratch repo, regardless of what OS/filesystem/worktree
oddity the enclosing dev repo happens to have. The --dol/--elf/--pass
arguments are plain filesystem paths (never read through git), so those
still live in an ordinary temp directory outside the scratch repo.

This cannot build a real Wii .dol (that needs devkitPPC + the MSYS2-only
libogc KERNEL_HEAP sidecar, docs/wii_libogc_kernel_heap_sidecar.md, which is
not assumed to be present on every machine this test runs on). Instead it
drives make_release.sh against a FAKE .dol/.elf pair and a FAKE PASS.json,
using the script's own --skip-objdump-gate-for-tests escape hatch -- which
is deliberately designed so the real (hardware-certified) path can never
take it: it is refused whenever --pass is supplied, and requires
--allow-uncertified. Both refusal rules are exercised here directly, so
"the real path cannot bypass the gate" is asserted, not just claimed.

What IS exercised against a REAL devkitPPC + sidecar build (manually, during
this pipeline's authoring session, not repeated by this portable test):
the KERNEL_HEAP/TLS/test-marker gates actually PASS on a genuine
sidecar-linked .elf, and the objdump-based KERNEL_HEAP gate correctly
REFUSES a real stock-libogc .elf even when it is not named "*-stock*" (i.e.
the gate inspects the artifact, not just the filename). See the release
packet for that evidence; it is real-toolchain-dependent and out of scope
for a portable ctest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

MINIMAL_CONFIG_WII_JSON = {
    "paths": {"texturepack": "assets", "worlds": "saves"},
    "input": {
        # Small integer WPAD/PAD button codes (source/platform/input.c's
        # WPAD_BUTTON_*/PAD_BUTTON_* tables) -- deliberately NOT 87 (GLFW_KEY_W),
        # so the happy-path test can assert the shipped config is the Wii
        # mapping, never the PC/GLFW keyboard one.
        "player_forward": [0, 200, 910, 930],
        "player_backward": [1, 201, 911, 931],
        "player_left": [2, 202, 912, 932],
        "player_right": [3, 203, 913, 933],
        "player_jump": [5, 210, 404],
        "player_sneak": [211, 408],
        "item_action_left": [100, 204, 410],
        "item_action_right": [101, 205, 409],
        "inventory": [6, 206, 407],
        "open_menu": [10, 214, 411],
    },
    "debug": {"overlay": "off"},
    "lod": {"far_view_distance": 6, "far_storage": "voxel"},
}

# A 1x1 PNG (transparent), valid enough as bytes-on-disk content for a file
# make_release.sh only copies/hashes -- it never decodes image content.
TINY_PNG_BYTES = bytes.fromhex(
    "89504e470d0a1a0a0000000d494844520000000100000001080600000"
    "01f15c4890000000a49444154789c6360000002000100ffff03000006"
    "0005574bde3d0000000049454e44ae426082"
)

# Card tutorial-1021: make_release.sh step 12d3 runs the commit's
# tests/test_tutorial_build.py and then tools/tutorial_world/build_tutorial.py
# --out <dir>. The real pair takes minutes, so the scratch repo commits these
# fast stand-ins (the REAL world is covered by ctest test_tutorial_build +
# test_tutorial_world_real_load); they exercise the step's mechanics: export
# from the commit, test-before-build, fail-closed, staging at both CWDs.
STUB_TUTORIAL_BUILDER = b'''import argparse, os
ap = argparse.ArgumentParser()
ap.add_argument("--out", required=True)
out = ap.parse_args().out
os.makedirs(os.path.join(out, "region"))
open(os.path.join(out, "level.dat"), "wb").write(b"stub level.dat")
open(os.path.join(out, "region", "r.0.0.mcr"), "wb").write(b"stub region")
'''
STUB_TUTORIAL_TEST = b"print('stub tutorial test: OK')\n"
TUTORIAL_WORLD_DIR = "CavEX Tutorial"


def run(cmd: list[str], cwd: Path, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )


def git(repo: Path, *args: str, timeout: int = 60) -> subprocess.CompletedProcess:
    proc = run(["git", *args], repo, timeout=timeout)
    if proc.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed in {repo}:\n{proc.stdout}")
    return proc


def build_scratch_repo(src_repo: Path, tmp: Path, name: str = "scratch_repo",
                       copy_notice: bool = True) -> Path:
    """Build a throwaway, fully self-contained git repo (its own real .git,
    never a worktree) holding exactly what tools/make_release.sh needs to
    package a release: its own copy of the release tooling, VERSION,
    LICENSE, a minimal config_wii.json/icon.png, and a tiny assets/ subset
    with both an included and an excluded file. `src_repo` (the enclosing
    dev repo passed via --repo to THIS test) is used only as a plain file
    source to copy from -- no git command ever runs against it.

    LIC-001 (card LIC-1, 2026-10-10): the source repo's .gitattributes,
    LICENSES/ tree and NOTICE are copied in too, when present, so the
    release attribution checks exercise the REAL license files and the real
    line-ending rules (a CRLF/LF normalization of the vendored, must-stay-
    unedited Isabella II About file would change its bytes). copy_notice=False
    builds a scratch repo WITHOUT a NOTICE, which drives make_release.sh's
    generated-skeleton NOTICE branch."""
    repo = tmp / name
    repo.mkdir()
    git(repo, "init", "--quiet")
    git(repo, "config", "user.name", "CaveWii Test")
    git(repo, "config", "user.email", "test@example.invalid")

    # Copied first so the attributes apply to every file added below.
    src_attrs = src_repo / ".gitattributes"
    if src_attrs.is_file():
        shutil.copyfile(src_attrs, repo / ".gitattributes")
    src_licenses = src_repo / "LICENSES"
    if src_licenses.is_dir():
        shutil.copytree(src_licenses, repo / "LICENSES")
    src_notice = src_repo / "NOTICE"
    if copy_notice and src_notice.is_file():
        shutil.copyfile(src_notice, repo / "NOTICE")
    # ISA-001 (card ISA-1, 2026-10-10): the tracked optional-pack sources
    # (packs/isabella-ii/) are copied in too, when present, so the release
    # zip's texturepacks/isabella-ii/ checks exercise the REAL pack bytes.
    src_packs = src_repo / "packs"
    if src_packs.is_dir():
        shutil.copytree(src_packs, repo / "packs")

    tools_dir = repo / "tools"
    tools_dir.mkdir()
    for name in ("make_release.sh", "gen_meta_xml.sh", "meta.xml.in"):
        src = src_repo / "tools" / name
        if not src.is_file():
            raise AssertionError(f"expected {src} to exist in the source repo")
        shutil.copyfile(src, tools_dir / name)
    (tools_dir / "make_release.sh").chmod(0o755)
    (tools_dir / "gen_meta_xml.sh").chmod(0o755)
    (tools_dir / "tutorial_world").mkdir()
    (tools_dir / "tutorial_world" / "build_tutorial.py").write_bytes(STUB_TUTORIAL_BUILDER)
    (repo / "tests").mkdir()
    (repo / "tests" / "test_tutorial_build.py").write_bytes(STUB_TUTORIAL_TEST)

    (repo / "VERSION").write_text("0.0.0-scratch-test\n", encoding="utf-8")
    (repo / "LICENSE").write_text(
        "This is a placeholder LICENSE for a throwaway test repo, not a real license.\n",
        encoding="utf-8",
    )
    (repo / "icon.png").write_bytes(TINY_PNG_BYTES)
    (repo / "config_wii.json").write_text(
        json.dumps(MINIMAL_CONFIG_WII_JSON, indent=4), encoding="utf-8"
    )

    assets = repo / "assets"
    (assets / "armor").mkdir(parents=True)
    (assets / "gui").mkdir(parents=True)
    (assets / "misc").mkdir(parents=True)
    # Included: nothing in the exclude list matches these paths. controls.png
    # and armor/*.png are included test cases (not excluded) since the
    # 2026-09-02 maintainer decision replaced both with original CC0 art and
    # lifted their exclusion -- exercised here so a regression that silently
    # re-excludes them is caught. misc/{foliagecolor,grasscolor}.png are the
    # same story, one same-day follow-up later: they were briefly excluded
    # too (untraceable provenance, docs/asset_provenance_2026-09-02.md), but
    # source/graphics/biome_color.c loads them UNCONDITIONALLY (unlike
    # misc/water.png below), so excluding them would render every grass/leaf
    # face pure white -- replaced with CC0 art (tools/gen_cc0_colormaps.py)
    # and re-included instead.
    (assets / "gui" / "included_dummy.png").write_bytes(TINY_PNG_BYTES)
    (assets / "controls.png").write_bytes(TINY_PNG_BYTES)
    (assets / "armor" / "chain_1.png").write_bytes(TINY_PNG_BYTES)
    (assets / "misc" / "foliagecolor.png").write_bytes(TINY_PNG_BYTES)
    (assets / "misc" / "grasscolor.png").write_bytes(TINY_PNG_BYTES)
    (assets / "README.md").write_text("scratch test assets, not real credits\n", encoding="utf-8")
    # Excluded (make_release.sh's current exclusion rule -- misc/water.png is
    # the one file that stays excluded: untraceable provenance per the
    # 2026-09-02 audit AND genuinely optional per source/main.c, so exclusion
    # is actually safe here, unlike the two colormaps above):
    (assets / "misc" / "water.png").write_bytes(TINY_PNG_BYTES)

    git(repo, "add", "-A")
    git(repo, "commit", "--quiet", "-m", "scratch repo for test_make_release")
    return repo


def make_release_cmd(scratch_repo: Path, bash: str, extra: list[str]) -> list[str]:
    return [
        bash,
        str(scratch_repo / "tools" / "make_release.sh"),
        "--repo", str(scratch_repo),
        *extra,
    ]


def write_fake_dol_elf(tmp: Path, dol_name: str = "CaveWii_v2.dol") -> tuple[Path, Path]:
    dol = tmp / dol_name
    elf = tmp / (dol_name[: -len(".dol")] + ".elf" if dol_name.endswith(".dol") else dol_name + ".elf")
    # Content is irrelevant for the fake-artifact tests (the objdump gate is
    # skipped); it only needs to exist and be readable for the sha256/md5
    # hashing + `strings` CW_BUILD_ID scan steps.
    dol.write_bytes(b"fake dol content, not a real Wii binary\n")
    elf.write_bytes(b"fake elf content, not a real linked ELF\n")
    return dol, elf


def write_fake_pass(tmp: Path, build_id: str = "abc1234_09.02") -> Path:
    p = tmp / "PASS.json"
    p.write_text(
        json.dumps(
            {
                "status": "PASS",
                "run_id": "run42",
                "profile": "heartbeat_selftest",
                "heartbeats": 50,
                "last_heartbeat": {
                    "event": "heartbeat",
                    "run_id": "run42",
                    "profile": "heartbeat_selftest",
                    "build_id": build_id,
                    "tick": 50,
                },
            }
        ),
        encoding="utf-8",
    )
    return p


def test_output_path_forms(scratch_repo: Path, bash: str, tmp: Path) -> None:
    """Native Windows, slash-separated and relative --out paths reach the
    intended directory, even with spaces and shell metacharacters in names."""
    case_dir = tmp / "output paths"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    for form in ("native", "slashes", "relative"):
        out_dir = case_dir / (form + " packages $literal") / "dist"
        argument = (str(out_dir) if form == "native" else
                    out_dir.as_posix() if form == "slashes" else
                    str(out_dir.relative_to(case_dir)))
        proc = run(make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol), "--elf", str(elf),
            "--allow-uncertified", "--skip-objdump-gate-for-tests",
            "--version", "0.1.0", "--out", argument,
        ]), case_dir)
        assert proc.returncode == 0, proc.stdout
        archives = list(out_dir.glob("*.zip"))
        assert len(archives) == 1, (out_dir, proc.stdout)
        with zipfile.ZipFile(archives[0]) as archive:
            assert archive.testzip() is None
            assert archive.read("apps/CavEX/boot.dol") == dol.read_bytes()
    print("PASS: native/slash/relative output paths with spaces and literal $")


def assert_no_zip_written(out_dir: Path) -> None:
    zips = list(out_dir.glob("*.zip")) if out_dir.exists() else []
    if zips:
        raise AssertionError(f"expected no zip to be written to {out_dir}, found: {zips}")


def test_stock_dol_refused(scratch_repo: Path, bash: str, tmp: Path) -> None:
    case_dir = tmp / "stock"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir, dol_name="CaveWii_v2-stock.dol")
    out_dir = case_dir / "dist"
    proc = run(
        make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol),
            "--elf", str(elf),
            "--allow-uncertified",
            "--skip-objdump-gate-for-tests",
            "--out", str(out_dir),
        ]),
        case_dir,
    )
    if proc.returncode == 0:
        raise AssertionError(f"expected a -stock .dol to be refused; output:\n{proc.stdout}")
    if "stock" not in proc.stdout.lower():
        raise AssertionError(f"expected a stock-related refusal message; got:\n{proc.stdout}")
    assert_no_zip_written(out_dir)
    print("PASS: -stock .dol refused")


def test_non_release_names_refused(scratch_repo: Path, bash: str, tmp: Path) -> None:
    """F7 (G1 adversarial review): the .dol name is an ALLOWLIST of exactly
    CaveWii_v2.dol -- the CI sidecar artifact, the dev build, a sd0-refused
    link and any renamed copy are refused, not just *-stock*."""
    for name in ("CaveWii_v2-ci.dol", "CaveWii_v2-dev.dol",
                 "CaveWii_v2-REFUSED-sd0dma.dol", "boot.dol", "fake.dol",
                 "CaveWii_v2.dol.dol", "xCaveWii_v2.dol"):
        case_dir = tmp / ("name_" + name.replace(".", "_"))
        case_dir.mkdir()
        dol, elf = write_fake_dol_elf(case_dir, dol_name=name)
        out_dir = case_dir / "dist"
        proc = run(
            make_release_cmd(scratch_repo, bash, [
                "--dol", str(dol),
                "--elf", str(elf),
                "--allow-uncertified",
                "--skip-objdump-gate-for-tests",
                "--out", str(out_dir),
            ]),
            case_dir,
        )
        if proc.returncode != 3:
            raise AssertionError(f"expected {name} to be refused (exit 3); rc={proc.returncode} output:\n{proc.stdout}")
        if "only the release artifact" not in proc.stdout:
            raise AssertionError(f"expected the allowlist refusal message for {name}; got:\n{proc.stdout}")
        assert_no_zip_written(out_dir)
    print("PASS: non-release .dol names (-ci, -dev, -REFUSED, renamed) refused by allowlist")


def test_missing_pass_refused_without_allow_uncertified(scratch_repo: Path, bash: str, tmp: Path) -> None:
    case_dir = tmp / "missing_pass"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    out_dir = case_dir / "dist"
    proc = run(
        make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol),
            "--elf", str(elf),
            "--skip-objdump-gate-for-tests",
            "--out", str(out_dir),
        ]),
        case_dir,
    )
    if proc.returncode == 0:
        raise AssertionError(f"expected a missing --pass (no --allow-uncertified) to be refused; output:\n{proc.stdout}")
    assert_no_zip_written(out_dir)
    print("PASS: missing --pass without --allow-uncertified refused")


def test_skip_gate_forbidden_with_pass(scratch_repo: Path, bash: str, tmp: Path) -> None:
    """The core anti-bypass property: --skip-objdump-gate-for-tests must be
    refused whenever --pass is supplied, REGARDLESS of --allow-uncertified.
    This is what makes the test-only escape hatch unusable on the
    hardware-certified path (the only path that ever carries --pass)."""
    case_dir = tmp / "skip_with_pass"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    pass_json = write_fake_pass(case_dir)
    out_dir = case_dir / "dist"
    proc = run(
        make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol),
            "--elf", str(elf),
            "--pass", str(pass_json),
            "--allow-uncertified",
            "--skip-objdump-gate-for-tests",
            "--out", str(out_dir),
        ]),
        case_dir,
    )
    if proc.returncode == 0:
        raise AssertionError(
            "expected --skip-objdump-gate-for-tests + --pass to be refused "
            f"even with --allow-uncertified; output:\n{proc.stdout}"
        )
    assert_no_zip_written(out_dir)
    print("PASS: --skip-objdump-gate-for-tests refused when --pass is given")


def test_skip_gate_requires_allow_uncertified(scratch_repo: Path, bash: str, tmp: Path) -> None:
    case_dir = tmp / "skip_no_allow"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    out_dir = case_dir / "dist"
    proc = run(
        make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol),
            "--elf", str(elf),
            "--skip-objdump-gate-for-tests",
            "--out", str(out_dir),
        ]),
        case_dir,
    )
    if proc.returncode == 0:
        raise AssertionError(
            f"expected --skip-objdump-gate-for-tests without --allow-uncertified to be refused; output:\n{proc.stdout}"
        )
    assert_no_zip_written(out_dir)
    print("PASS: --skip-objdump-gate-for-tests requires --allow-uncertified")


def test_publish_flag_refused(scratch_repo: Path, bash: str, tmp: Path) -> None:
    case_dir = tmp / "publish"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    out_dir = case_dir / "dist"
    proc = run(
        make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol),
            "--elf", str(elf),
            "--allow-uncertified",
            "--skip-objdump-gate-for-tests",
            "--publish",
            "--out", str(out_dir),
        ]),
        case_dir,
    )
    if proc.returncode == 0:
        raise AssertionError(f"expected --publish to always be refused; output:\n{proc.stdout}")
    assert_no_zip_written(out_dir)
    print("PASS: --publish is always refused (local-only tool)")


def test_zip_name_explicit_version(scratch_repo: Path, bash: str, tmp: Path) -> None:
    """Maintainer decision 2026-09-02: with an explicit --version (how a real
    release is actually cut -- e.g. --version 0.1.0, matching VERSION),
    the zip is named EXACTLY 'CavEX-ReCrafted-<version>-<sha>-Wii-UNCERTIFIED.zip'
    (product name 'CavEX: ReCrafted', maintainer 2026-11-03), never 'CaveWii'."""
    case_dir = tmp / "explicit_version"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    out_dir = case_dir / "dist"
    sha_short = git(scratch_repo, "rev-parse", "--short=7", "HEAD").stdout.strip()
    proc = run(
        make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol),
            "--elf", str(elf),
            "--allow-uncertified",
            "--skip-objdump-gate-for-tests",
            "--version", "0.1.0",
            "--out", str(out_dir),
        ]),
        case_dir,
    )
    if proc.returncode != 0:
        raise AssertionError(f"expected the explicit-version happy path to succeed; output:\n{proc.stdout}")
    zips = list(out_dir.glob("*.zip"))
    if len(zips) != 1:
        raise AssertionError(f"expected exactly one zip in {out_dir}, found: {zips}")
    expected = f"CavEX-ReCrafted-0.1.0-{sha_short}-Wii-UNCERTIFIED.zip"
    if zips[0].name != expected:
        raise AssertionError(f"expected zip named {expected!r}, got {zips[0].name!r}")
    print("PASS: zip name is exactly 'CavEX-ReCrafted-<version>-<sha>-Wii-UNCERTIFIED.zip' with an explicit --version")


def test_full_package_layout_and_provenance(scratch_repo: Path, bash: str, tmp: Path) -> None:
    case_dir = tmp / "happy"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    out_dir = case_dir / "dist"
    proc = run(
        make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol),
            "--elf", str(elf),
            "--allow-uncertified",
            "--skip-objdump-gate-for-tests",
            "--out", str(out_dir),
        ]),
        case_dir,
    )
    if proc.returncode != 0:
        raise AssertionError(f"expected the fake-artifact happy path to succeed; output:\n{proc.stdout}")

    zips = list(out_dir.glob("*.zip"))
    if len(zips) != 1:
        raise AssertionError(f"expected exactly one zip in {out_dir}, found: {zips}")
    zip_path = zips[0]
    if "UNCERTIFIED" not in zip_path.name:
        raise AssertionError(f"expected --allow-uncertified to mark the zip name UNCERTIFIED: {zip_path.name}")

    # --- product-name zip naming (maintainer decision 2026-09-02): the
    # release artifact is named "CavEX-ReCrafted-<version>-...-Wii...zip"
    # (maintainer 2026-11-03), never the old "CaveWii" names. (This no-explicit-version
    # run resolves RELEASE_VERSION to "<VERSION file>+<git describe>", so the
    # exact literal "CavEX-ReCrafted-<VERSION>-<sha>" form -- with no "+describe" --
    # is asserted separately below, in test_zip_name_explicit_version, where
    # --version is pinned explicitly the way a real release cut would.)
    if not zip_path.name.startswith("CavEX-ReCrafted-"):
        raise AssertionError(
            f"expected the zip to be named 'CavEX-ReCrafted-<version>-...zip', got: {zip_path.name}"
        )
    if "CaveWii" in zip_path.name:
        raise AssertionError(f"zip name still uses the old 'CaveWii' name: {zip_path.name}")

    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())

        # --- layout ---
        required = [
            "apps/CavEX/boot.dol",
            "apps/CavEX/meta.xml",
            "apps/CavEX/icon.png",
            "apps/CavEX/config_wii.json",
            "config_wii.json",
            "texturepacks/README.txt",
            "LICENSE",
            "NOTICE",
            "README-RELEASE.txt",
            "SHA256SUMS",
            "provenance.json",
        ]
        missing = [r for r in required if r not in names]
        if missing:
            raise AssertionError(f"release zip is missing required entries: {missing}")

        # The zip must never carry a config.json (ci-1101/05): that is the
        # player's own file (MP name, server list, Options), and unzipping an
        # update over an installed card used to overwrite it. Only the
        # config_wii.json fallback template ships (main.c default_config +
        # config_load_user_or_default read it when config.json is missing).
        shipped_user_cfgs = [n for n in names if n in ("config.json", "apps/CavEX/config.json")]
        if shipped_user_cfgs:
            raise AssertionError(f"release zip must not ship the player's config.json: {shipped_user_cfgs}")

        # config_wii.json must carry the WII controller mapping (small integer
        # WPAD/PAD button codes), never the PC/GLFW keyboard codes the
        # repo-root config.json uses for dev/testing (this was a real
        # finding during this pipeline's design: shipping the PC config to
        # a Wii SD card would leave every physical controller unmapped).
        cfg = json.loads(zf.read("apps/CavEX/config_wii.json"))
        if cfg["paths"]["texturepack"] != "assets":
            raise AssertionError(f"expected the bundled default pack path 'assets', got {cfg['paths']['texturepack']!r}")
        fwd = cfg["input"]["player_forward"]
        if 87 in fwd:  # GLFW_KEY_W -- the PC keyboard code, must never appear here
            raise AssertionError(f"release config_wii.json looks like the PC/GLFW keyboard mapping, not Wii: player_forward={fwd}")

        # --- meta.xml: product name "CavEX: ReCrafted" (maintainer 2026-11-02; was "CavEX: Re-Crafted Edition" from
        # decision 2026-10-10; was "CaveWii" from 2026-09-02), version sourced from the scratch repo's VERSION file.
        # Parsed with a regex over the <app>...</app> element, not
        # xml.etree, because tools/meta.xml.in's own header COMMENT contains
        # bare "--" sequences (e.g. "-- both come from") which are invalid
        # inside an XML comment per the XML spec (a strict parser like
        # expat/ElementTree refuses the whole document) -- a pre-existing
        # template issue, out of scope for this asset-provenance change to
        # fix, so this test reads around it instead of failing on it.
        meta_text = zf.read("apps/CavEX/meta.xml").decode("utf-8")
        app_match = re.search(r"<app\b.*</app>", meta_text, re.DOTALL)
        if not app_match:
            raise AssertionError("could not find <app>...</app> in meta.xml")
        app_xml = app_match.group(0)
        meta_root = ET.fromstring(app_xml)
        meta_name = meta_root.findtext("name")
        if meta_name != "CavEX: ReCrafted":
            raise AssertionError(f"expected meta.xml <name>CavEX: ReCrafted</name>, got {meta_name!r}")
        # No --version override on this run, so RELEASE_VERSION resolves to
        # "<VERSION file>+<git describe>" (see make_release.sh step 5) --
        # same "+describe" suffix the zip-name check above already expects.
        meta_version = meta_root.findtext("version")
        if not meta_version or not meta_version.startswith("0.0.0-scratch-test"):
            raise AssertionError(f"expected meta.xml <version> sourced from VERSION file, got {meta_version!r}")

        # Public text: no "Minecraft", no dev notes, coder is BagHeads.
        if meta_root.findtext("coder") != "BagHeads":
            raise AssertionError(f"expected meta.xml <coder>BagHeads</coder>, got {meta_root.findtext('coder')!r}")
        public_text = " ".join((meta_root.findtext(t) or "") for t in ("name", "short_description", "long_description"))
        for bad in ("minecraft", "cavewii_v2", "fire-budget", "dolphin", "tick budget"):
            if bad in public_text.lower():
                raise AssertionError(f"meta.xml public text contains forbidden {bad!r}: {public_text!r}")
        if "Not affiliated with Mojang or Nintendo." not in (meta_root.findtext("long_description") or ""):
            raise AssertionError("meta.xml long_description missing the non-affiliation line")

        # The dev-rig Stage Server app must never ship in the release zip.
        stage_hits = [n for n in names if "stage_server" in n.lower() or "stage-server" in n.lower()]
        if stage_hits:
            raise AssertionError(f"release zip ships the Stage Server app: {stage_hits}")

        # --- provenance.json ---
        prov = json.loads(zf.read("provenance.json"))
        if prov.get("certification_status") != "UNCERTIFIED":
            raise AssertionError(f"expected certification_status UNCERTIFIED, got {prov.get('certification_status')!r}")
        for key in ("schema", "tool", "version", "git", "dol", "elf", "gates", "assets"):
            if key not in prov:
                raise AssertionError(f"provenance.json missing top-level key: {key}")
        if not prov["git"].get("commit"):
            raise AssertionError("provenance.json git.commit is empty")
        if not prov["dol"].get("sha256") or not prov["dol"].get("md5"):
            raise AssertionError("provenance.json dol.sha256/md5 missing")
        for gate in ("kernel_heap_objdump_gate", "tls_gate", "test_marker_gate"):
            if prov["gates"].get(gate) != "SKIPPED_FOR_TESTS":
                raise AssertionError(
                    f"expected gate '{gate}' to read SKIPPED_FOR_TESTS for this "
                    f"--skip-objdump-gate-for-tests run, got {prov['gates'].get(gate)!r}"
                )
        if prov.get("hardware_pass") is not None:
            raise AssertionError("expected hardware_pass to be null when no --pass was given")

        included = prov["assets"]["included"]
        excluded_entries = prov["assets"]["excluded"]
        excluded_paths = {e["path"] for e in excluded_entries}
        if not included:
            raise AssertionError("provenance.json assets.included is empty -- expected the scratch repo's shippable asset(s)")
        # controls.png / armor/*.png are INCLUDED as of the 2026-09-02
        # maintainer decision (replaced with original CC0 art, exclusion
        # lifted) -- regression-check the opposite of what this test used to
        # assert.
        if "assets/controls.png" not in included:
            raise AssertionError(f"expected assets/controls.png to be included (post-CC0-replacement), got included={included}")
        if not any(p.startswith("assets/armor/") for p in included):
            raise AssertionError(f"expected an assets/armor/* path to be included (post-CC0-replacement), got included={included}")
        if "assets/gui/included_dummy.png" not in included:
            raise AssertionError(f"expected assets/gui/included_dummy.png to be included, got included={included}")
        # misc/{foliagecolor,grasscolor}.png are INCLUDED too (same-day
        # follow-up, 2026-09-02): biome_color.c loads them unconditionally,
        # so excluding them would render every grass/leaf face pure white --
        # replaced with CC0 art (tools/gen_cc0_colormaps.py) instead.
        for rel in ("assets/misc/foliagecolor.png", "assets/misc/grasscolor.png"):
            if rel not in included:
                raise AssertionError(f"expected {rel} to be included (post-CC0-replacement), got included={included}")
        # misc/water.png is the one file that stays excluded -- untraceable
        # provenance AND genuinely optional (source/main.c skips the overlay
        # when absent), so exclusion is actually safe there.
        if "assets/misc/water.png" not in excluded_paths:
            raise AssertionError(f"expected assets/misc/water.png to be excluded, got excluded={sorted(excluded_paths)}")

        # Every included path must actually be present in the zip (at both
        # staged locations), and no excluded path may appear ANYWHERE in the
        # zip -- the whole point of the exclusion rule.
        for rel in included:
            # `rel` is already an "assets/..." path (from `git ls-tree`);
            # it must be staged at BOTH the SD-card root and the app
            # subfolder (see make_release.sh's "staging both is harmless"
            # comment, sourced from docs/platform_perf_notes.md).
            if rel not in names:
                raise AssertionError(f"included asset '{rel}' missing from the zip root")
            if f"apps/CavEX/{rel}" not in names:
                raise AssertionError(f"included asset '{rel}' missing from apps/CavEX/")
        for rel in excluded_paths:
            for name in names:
                if name == rel or name == f"apps/CavEX/{rel}":
                    raise AssertionError(f"excluded asset '{rel}' leaked into the zip as '{name}'")
        # Specifically re-assert the current exclusion rule by name, since
        # this is the constraint the whole packaging design hinges on: ONLY
        # misc/water.png is excluded now.
        if any(n.endswith("assets/misc/water.png") for n in names):
            raise AssertionError("assets/misc/water.png leaked into the release zip")
        if not any(n.endswith("assets/misc/foliagecolor.png") for n in names):
            raise AssertionError("assets/misc/foliagecolor.png did not make it into the release zip (should be included)")
        if not any(n.endswith("assets/misc/grasscolor.png") for n in names):
            raise AssertionError("assets/misc/grasscolor.png did not make it into the release zip (should be included)")

        # --- tutorial world (step 12d3): built world at BOTH boot CWDs, ---
        # --- recorded in provenance.json and named in README-RELEASE.txt ---
        for root in ("", "apps/CavEX/"):
            world = f"{root}saves/{TUTORIAL_WORLD_DIR}/"
            if zf.read(world + "level.dat") != b"stub level.dat" or \
                    zf.read(world + "region/r.0.0.mcr") != b"stub region":
                raise AssertionError(f"tutorial world not staged at {world}")
        tut = prov.get("tutorial_world") or {}
        if tut.get("directory") != f"saves/{TUTORIAL_WORLD_DIR}" or \
                sorted(f["path"] for f in tut.get("files", [])) != [
                    f"saves/{TUTORIAL_WORLD_DIR}/level.dat",
                    f"saves/{TUTORIAL_WORLD_DIR}/region/r.0.0.mcr"]:
            raise AssertionError(f"provenance.json tutorial_world wrong: {tut!r}")
        if f"saves/{TUTORIAL_WORLD_DIR}/" not in zf.read("README-RELEASE.txt").decode("utf-8"):
            raise AssertionError("README-RELEASE.txt does not name the tutorial world")

        # --- SHA256SUMS covers every file (except itself) and verifies ---
        sums_text = zf.read("SHA256SUMS").decode("utf-8")
        sum_lines = [l for l in sums_text.splitlines() if l.strip()]
        summed_paths = set()
        for line in sum_lines:
            # GNU coreutils sha256sum format: <64-hex-digest><SP><marker><path>,
            # where marker is ' ' (text mode) or '*' (binary mode) -- exactly
            # one character, not a fixed two-space separator (MSYS2's
            # sha256sum emits the '*' binary-mode form).
            if len(line) < 67 or line[64] != " " or line[65] not in (" ", "*"):
                raise AssertionError(f"malformed SHA256SUMS line: {line!r}")
            digest = line[:64]
            path = line[66:]
            summed_paths.add(path)
            actual = hashlib.sha256(zf.read(path)).hexdigest()
            if actual != digest:
                raise AssertionError(f"SHA256SUMS mismatch for {path}: expected {digest}, got {actual}")
        all_other_files = {n for n in names if n != "SHA256SUMS" and not n.endswith("/")}
        missing_from_sums = all_other_files - summed_paths
        if missing_from_sums:
            raise AssertionError(f"SHA256SUMS does not cover every file: missing {sorted(missing_from_sums)}")
        extra_in_sums = summed_paths - all_other_files
        if extra_in_sums:
            raise AssertionError(f"SHA256SUMS lists files not actually in the zip: {sorted(extra_in_sums)}")

    print("PASS: full package layout, provenance.json, and SHA256SUMS coverage")


def test_earth_and_ash_assets(src_repo: Path, scratch_repo: Path, bash: str, tmp: Path) -> None:
    """Ship all reviewed textures, including water, from the selected commit."""
    manifest = json.loads((src_repo / "assets/earth-and-ash-manifest.json").read_text())
    # The shared Homebrew Channel hand is separately credited GPLv2 art,
    # never part of the Earth & Ash CC0 inventory.
    assert "pointer.png" not in {entry["path"] for entry in manifest}
    inventory = json.loads((src_repo / "tools/release-assets.json").read_text())
    pointers = [entry for entry in inventory["assets"] if "assets/pointer.png" in entry["paths"]]
    assert len(pointers) == 1
    pointer, = pointers
    assert pointer["licence"] == "GPL-2.0-only"
    assert pointer["licence_file"] == "LICENSES/GPL-2.0.txt"
    assert "NOTICE" in pointer["attribution_where"]
    assert hashlib.sha256((src_repo / "assets/pointer.png").read_bytes()).hexdigest() == pointer["sha256"]
    for entry in manifest:
        rel = "assets/" + entry["path"]
        target = scratch_repo / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src_repo / rel, target)
        assert hashlib.sha256(target.read_bytes()).hexdigest() == entry["sha256"]
    for name in ("earth-and-ash-manifest.json", "CC0-1.0.txt"):
        shutil.copyfile(src_repo / "assets" / name, scratch_repo / "assets" / name)
    git(scratch_repo, "add", "assets")
    git(scratch_repo, "commit", "--quiet", "-m", "reviewed CC0 asset fixture")
    # A dirty local overlay must neither change the shipped bytes nor revoke
    # clearance of the reviewed committed water file.
    (scratch_repo / "assets/misc/water.png").write_bytes(b"unreviewed local overlay")
    case_dir = tmp / "earth-and-ash"
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    out_dir = case_dir / "dist"
    proc = run(make_release_cmd(scratch_repo, bash, [
        "--dol", str(dol), "--elf", str(elf), "--allow-uncertified",
        "--skip-objdump-gate-for-tests", "--out", str(out_dir),
    ]), case_dir)
    assert proc.returncode == 0, proc.stdout
    archive, = out_dir.glob("*.zip")
    with zipfile.ZipFile(archive) as zf:
        for entry in manifest:
            for prefix in ("assets/", "apps/CavEX/assets/"):
                assert hashlib.sha256(zf.read(prefix + entry["path"])).hexdigest() == entry["sha256"]
        for name in ("earth-and-ash-manifest.json", "CC0-1.0.txt"):
            # Git normalizes text line endings on Windows; PNG byte hashes
            # above remain exact, while license/manifest text stays equivalent.
            assert zf.read("assets/" + name).decode("utf-8").replace("\r\n", "\n") == (src_repo / "assets" / name).read_text(encoding="utf-8")
        prov = json.loads(zf.read("provenance.json"))
        assert "assets/misc/water.png" not in {e["path"] for e in prov["assets"]["excluded"]}
    print(f"PASS: all {len(manifest)} CC0 textures and license packaged; committed water overrides dirty local file")


# LIC-001 (card LIC-1, maintainer ruling 2026-10-10): the default texture pack
# is Isabella II by Bonemouse, CC BY 3.0 Unported. Its own --LICENSE-- section
# requires attributing Bonemouse, linking the forum thread, and including the
# About file -- UNEDITED, so its bytes are pinned by md5 (1641 bytes, from the
# github.com/minetest-texture-packs/Isabella-II clone @ 8afe939).
ISABELLA_ABOUT_ENTRY = "LICENSES/Isabella-II/About Isabella.txt"
ISABELLA_LICENSE_ENTRY = "LICENSES/Isabella-II/LICENSE.txt"
ISABELLA_ABOUT_MD5 = "7d27990c48652ab0864eb001a7f802ee"
ISABELLA_THREAD_URL = "http://www.minecraftforum.net/topic/242175-Isabella/"
CC_BY_30_URL = "http://creativecommons.org/licenses/by/3.0/"
ISABELLA_ATTRIBUTION_STRINGS = (
    "Bonemouse",
    "Isabella II",
    "CC BY 3.0 Unported",
    CC_BY_30_URL,
    ISABELLA_THREAD_URL,
)


def assert_isabella_attribution_text(label: str, text: str) -> None:
    for needle in ISABELLA_ATTRIBUTION_STRINGS:
        if needle not in text:
            raise AssertionError(f"LIC-001: packaged {label} does not contain {needle!r}")
    if not re.search(r"\bmodified\b", text, re.IGNORECASE):
        raise AssertionError(
            f"LIC-001: packaged {label} does not say the Isabella II textures are modified "
            "(CC BY 3.0 requires indicating changes)"
        )


def assert_isabella_in_zip(zip_path: Path, branch: str) -> None:
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())
        # Exact entry name, including the SPACE in "About Isabella.txt".
        if ISABELLA_ABOUT_ENTRY not in names:
            raise AssertionError("LIC-001: missing Isabella About file")
        about_md5 = hashlib.md5(zf.read(ISABELLA_ABOUT_ENTRY)).hexdigest()
        if about_md5 != ISABELLA_ABOUT_MD5:
            raise AssertionError(
                f"LIC-001: {ISABELLA_ABOUT_ENTRY} is not byte-identical to the unedited original "
                f"(md5 {about_md5}, expected {ISABELLA_ABOUT_MD5})"
            )
        sums_paths = set()
        for line in zf.read("SHA256SUMS").decode("utf-8").splitlines():
            if len(line) >= 67:
                sums_paths.add(line[66:])
        if ISABELLA_ABOUT_ENTRY not in sums_paths:
            raise AssertionError(f"LIC-001: SHA256SUMS does not list {ISABELLA_ABOUT_ENTRY}")

        if ISABELLA_LICENSE_ENTRY not in names:
            raise AssertionError(f"LIC-001: missing {ISABELLA_LICENSE_ENTRY}")
        license_text = zf.read(ISABELLA_LICENSE_ENTRY).decode("utf-8", errors="replace")
        if "Attribution 3.0 Unported" not in license_text:
            raise AssertionError(f"LIC-001: {ISABELLA_LICENSE_ENTRY} does not contain 'Attribution 3.0 Unported'")

        assert_isabella_attribution_text(
            f"NOTICE ({branch})", zf.read("NOTICE").decode("utf-8", errors="replace"))
        assert_isabella_attribution_text(
            "texturepacks/README.txt",
            zf.read("texturepacks/README.txt").decode("utf-8", errors="replace"))


def package_for_isabella(repo: Path, bash: str, case_dir: Path) -> tuple[Path, str]:
    case_dir.mkdir()
    dol, elf = write_fake_dol_elf(case_dir)
    out_dir = case_dir / "dist"
    proc = run(make_release_cmd(repo, bash, [
        "--dol", str(dol), "--elf", str(elf), "--allow-uncertified",
        "--skip-objdump-gate-for-tests", "--out", str(out_dir),
    ]), case_dir)
    if proc.returncode != 0:
        raise AssertionError(f"LIC-001: packaging failed in {repo}; output:\n{proc.stdout}")
    zips = list(out_dir.glob("*.zip"))
    if len(zips) != 1:
        raise AssertionError(f"LIC-001: expected exactly one zip in {out_dir}, found: {zips}")
    return zips[0], proc.stdout


def test_isabella_attribution(src_repo: Path, scratch_repo: Path, bash: str, tmp: Path) -> None:
    """LIC-001: every release zip ships the unedited Isabella II About file and
    licence text, and credits Bonemouse / Isabella II / CC BY 3.0 Unported in
    NOTICE and texturepacks/README.txt -- on BOTH NOTICE branches of
    make_release.sh (repo NOTICE copied in, and the generated skeleton)."""
    if not (src_repo / "NOTICE").is_file():
        raise AssertionError(f"LIC-001: source repo {src_repo} has no NOTICE to exercise the repo-NOTICE branch")
    if not (scratch_repo / "NOTICE").is_file():
        raise AssertionError("LIC-001: scratch repo did not receive the source repo NOTICE")

    # Branch 1: repo NOTICE present at the packaged commit.
    zip_path, out = package_for_isabella(scratch_repo, bash, tmp / "isabella_repo_notice")
    if "NOTICE source: repo NOTICE" not in out:
        raise AssertionError(f"LIC-001: expected the repo-NOTICE branch; output:\n{out}")
    assert_isabella_in_zip(zip_path, "repo NOTICE")

    # Branch 2: no NOTICE at the packaged commit -> generated skeleton.
    skeleton_repo = build_scratch_repo(src_repo, tmp, name="scratch_repo_no_notice", copy_notice=False)
    if (skeleton_repo / "NOTICE").exists():
        raise AssertionError("LIC-001: skeleton scratch repo unexpectedly has a NOTICE")
    zip_path, out = package_for_isabella(skeleton_repo, bash, tmp / "isabella_skeleton_notice")
    if "NOTICE source: generated skeleton" not in out:
        raise AssertionError(f"LIC-001: expected the generated-skeleton NOTICE branch; output:\n{out}")
    assert_isabella_in_zip(zip_path, "generated skeleton")
    print("PASS: Isabella II About/LICENSE shipped unedited and attributed in both NOTICE branches + texturepacks/README.txt")


# ISA-001 (card ISA-1, maintainer 2026-10-10): Isabella II also ships as an
# OPTIONAL, pick-able pack folder texturepacks/isabella-ii/ in the release zip,
# copied from the tracked source packs/isabella-ii/ at the packaged commit. The
# bundled default (assets/, paths.texturepack = "assets") is not touched.
ISA_PACK_PREFIX = "texturepacks/isabella-ii/"
ISA_PACK_SOURCE = "packs/isabella-ii"
ISA_PACK_PNGS = (
    "terrain.png",
    "particles.png",
    "gui/items.png",
    "terrain/sun.png",
    "terrain/moon.png",
    "environment/clouds.png",
    "gui/crafting.png",
    "gui/furnace.png",
    "gui/gui.png",
    "gui/icons.png",
    "gui/inventory.png",
    # 2026-10-11: the pack's own original CC0 pointer and font (not Isabella files).
    "pointer.png",
    "font/default.png",
    # Beta 1.7.3 fire/lava/water/portal motion in Isabella colours (+ its own ice).
    "anim.png",
    # 2026-10-28: Isabella-palette menu backdrop render and player skin (both modified, CC BY 3.0).
    "gui/menu-background.png",
    "mob/char.png",
    # 2026-11-02: Isabella-palette Nether loading scene render (modified, CC BY 3.0; ci-1101/57f).
    "gui/nether-background.png",
)
ISA_PACK_ABOUT = "About Isabella.txt"
ISA_PACK_LICENSE = "LICENSE.txt"
ISA_PACK_README = "README.txt"
ISA_DECOY_NAME = "isa001-untracked-decoy.txt"


def sha256_sums_map(zf: zipfile.ZipFile) -> dict[str, str]:
    sums = {}
    for line in zf.read("SHA256SUMS").decode("utf-8").splitlines():
        if len(line) >= 67 and line[64] == " " and line[65] in (" ", "*"):
            sums[line[66:]] = line[:64]
    return sums


def provenance_path_sha256(node: object, out: dict[str, str]) -> dict[str, str]:
    """Collect every {"path": ..., "sha256": ...} object anywhere in
    provenance.json. The pack entries may live under any key, but each one
    must name its ZIP path (texturepacks/isabella-ii/<rel>) and that file's
    sha256."""
    if isinstance(node, dict):
        path, digest = node.get("path"), node.get("sha256")
        if isinstance(path, str) and isinstance(digest, str):
            out[path] = digest
        for value in node.values():
            provenance_path_sha256(value, out)
    elif isinstance(node, list):
        for value in node:
            provenance_path_sha256(value, out)
    return out


def assert_isabella_pack_in_zip(src_repo: Path, scratch: Path, zip_path: Path, branch: str) -> None:
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())
        files = {n for n in names if not n.endswith("/")}
        pack_files = {n for n in files if n.startswith(ISA_PACK_PREFIX)}
        # Prefix FIRST, before any source-pack file is opened, so RED is this
        # named assertion and never a FileNotFoundError.
        if not pack_files:
            raise AssertionError("ISA-001: missing texturepacks/isabella-ii")

        # Committed-source isolation: the untracked decoy never ships.
        leaked = sorted(n for n in names if n.endswith(ISA_DECOY_NAME))
        if leaked:
            raise AssertionError(
                f"ISA-001: untracked decoy in the scratch repo's {ISA_PACK_SOURCE}/ leaked into the zip ({branch}): {leaked}")

        # Exactly the listed PNGs (11 Isabella + its own pointer, font and anim), no other PNG.
        expected_pngs = {ISA_PACK_PREFIX + p for p in ISA_PACK_PNGS}
        actual_pngs = {n for n in pack_files if n.lower().endswith(".png")}
        if actual_pngs != expected_pngs:
            raise AssertionError(
                f"ISA-001: texturepacks/isabella-ii PNG set mismatch ({branch}): "
                f"missing {sorted(expected_pngs - actual_pngs)}, unexpected {sorted(actual_pngs - expected_pngs)}")
        for rel in ISA_PACK_PNGS:
            src = src_repo / ISA_PACK_SOURCE / rel
            if not src.is_file():
                raise AssertionError(f"ISA-001: source repo is missing {ISA_PACK_SOURCE}/{rel}")
            if zf.read(ISA_PACK_PREFIX + rel) != src.read_bytes():
                raise AssertionError(
                    f"ISA-001: {ISA_PACK_PREFIX}{rel} is not byte-identical to {ISA_PACK_SOURCE}/{rel} ({branch})")

        # The three credit files.
        for rel in (ISA_PACK_ABOUT, ISA_PACK_LICENSE, ISA_PACK_README):
            if ISA_PACK_PREFIX + rel not in names:
                raise AssertionError(f"ISA-001: missing {ISA_PACK_PREFIX}{rel} ({branch})")
        about_md5 = hashlib.md5(zf.read(ISA_PACK_PREFIX + ISA_PACK_ABOUT)).hexdigest()
        if about_md5 != ISABELLA_ABOUT_MD5:
            raise AssertionError(
                f"ISA-001: {ISA_PACK_PREFIX}{ISA_PACK_ABOUT} is not the unedited original "
                f"(md5 {about_md5}, expected {ISABELLA_ABOUT_MD5})")
        license_text = zf.read(ISA_PACK_PREFIX + ISA_PACK_LICENSE).decode("utf-8", errors="replace")
        if "Attribution 3.0 Unported" not in license_text:
            raise AssertionError(
                f"ISA-001: {ISA_PACK_PREFIX}{ISA_PACK_LICENSE} does not contain 'Attribution 3.0 Unported'")
        readme_text = zf.read(ISA_PACK_PREFIX + ISA_PACK_README).decode("utf-8", errors="replace")
        for needle in ISABELLA_ATTRIBUTION_STRINGS:
            if needle not in readme_text:
                raise AssertionError(f"ISA-001: {ISA_PACK_PREFIX}{ISA_PACK_README} does not contain {needle!r}")
        if not re.search(r"\bmodified\b", readme_text, re.IGNORECASE):
            raise AssertionError(
                f"ISA-001: {ISA_PACK_PREFIX}{ISA_PACK_README} does not say the Isabella II files are modified")

        # SHA256SUMS lists every pack file with its correct digest.
        sums = sha256_sums_map(zf)
        for name in sorted(pack_files):
            actual = hashlib.sha256(zf.read(name)).hexdigest()
            if sums.get(name) != actual:
                raise AssertionError(
                    f"ISA-001: SHA256SUMS does not list {name} with sha256 {actual} (got {sums.get(name)!r})")

        # provenance.json lists every pack entry with its sha256.
        prov_map = provenance_path_sha256(json.loads(zf.read("provenance.json")), {})
        for name in sorted(pack_files):
            actual = hashlib.sha256(zf.read(name)).hexdigest()
            if prov_map.get(name) != actual:
                raise AssertionError(
                    f"ISA-001: provenance.json does not list {name} with sha256 {actual} (got {prov_map.get(name)!r})")

        # NOTICE mentions the optional pack (checked on both branches).
        notice = zf.read("NOTICE").decode("utf-8", errors="replace")
        if "texturepacks/isabella-ii" not in notice:
            raise AssertionError(f"ISA-001: NOTICE ({branch}) does not mention the optional texturepacks/isabella-ii pack")

        # The default is untouched: config still selects assets ...
        for cfg_name in ("config_wii.json", "apps/CavEX/config_wii.json"):
            cfg = json.loads(zf.read(cfg_name))
            if cfg["paths"]["texturepack"] != "assets":
                raise AssertionError(
                    f"ISA-001: packaged {cfg_name} selects {cfg['paths']['texturepack']!r}, expected the default 'assets'")

        # ... and every bundled assets/ entry equals the scratch fixture's
        # COMMITTED bytes (the working tree carries a dirty overlay).
        tracked = [p for p in git(scratch, "ls-tree", "-r", "--name-only", "HEAD", "--", "assets").stdout.splitlines() if p]
        prov = json.loads(zf.read("provenance.json"))
        excluded = {e["path"] for e in prov["assets"]["excluded"]}
        shipped = [p for p in tracked if p not in excluded]
        if not shipped:
            raise AssertionError("ISA-001: scratch fixture has no shippable assets/ to compare")
        for rel in shipped:
            committed = subprocess.run(
                ["git", "cat-file", "blob", f"HEAD:{rel}"], cwd=scratch,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            if committed.returncode != 0:
                raise AssertionError(f"ISA-001: could not read committed {rel}: {committed.stderr!r}")
            for prefix in ("", "apps/CavEX/"):
                if prefix + rel not in names:
                    raise AssertionError(f"ISA-001: bundled asset {prefix + rel} missing from the zip ({branch})")
                if zf.read(prefix + rel) != committed.stdout:
                    raise AssertionError(
                        f"ISA-001: bundled asset {prefix + rel} differs from the committed bytes ({branch})")


def test_isabella_ii_optional_pack(src_repo: Path, bash: str, tmp: Path) -> None:
    """ISA-001: the release zip ships texturepacks/isabella-ii/ (14 PNGs
    byte-identical to packs/isabella-ii, unedited About file, licence text,
    README credit) from COMMITTED sources only, lists it in SHA256SUMS and
    provenance.json, mentions it in NOTICE on both branches, and leaves the
    bundled default (assets/ + paths.texturepack) untouched."""
    for repo_name, copy_notice, branch, marker in (
            ("scratch_repo_isa_notice", True, "repo NOTICE", "NOTICE source: repo NOTICE"),
            ("scratch_repo_isa_skeleton", False, "generated skeleton", "NOTICE source: generated skeleton")):
        repo = build_scratch_repo(src_repo, tmp, name=repo_name, copy_notice=copy_notice)
        # Untracked decoy in the pack source + a dirty overlay on a committed
        # asset: neither may reach the zip (packaging reads $COMMIT only).
        decoy = repo / ISA_PACK_SOURCE / ISA_DECOY_NAME
        decoy.parent.mkdir(parents=True, exist_ok=True)
        decoy.write_text("ISA-001 untracked decoy, must never ship\n", encoding="utf-8")
        (repo / "assets" / "gui" / "included_dummy.png").write_bytes(b"ISA-001 dirty uncommitted overlay")
        status = git(repo, "status", "--porcelain", "--untracked-files=all").stdout
        if ISA_DECOY_NAME not in status:
            raise AssertionError(f"ISA-001: decoy is not untracked in {repo}:\n{status}")
        zip_path, out = package_for_isabella(repo, bash, tmp / ("isa_" + repo_name))
        if marker not in out:
            raise AssertionError(f"ISA-001: expected the {branch} NOTICE branch; output:\n{out}")
        assert_isabella_pack_in_zip(src_repo, repo, zip_path, branch)
    print("PASS: optional texturepacks/isabella-ii pack shipped from committed sources on both NOTICE branches; default untouched")


# EAD-001 (maintainer rulings 2026-10-10): the bundled default art is Earth &
# Ash. The eight sheets that used to be Isabella II or fork art are now the
# E&A pack's own files (artwork/cc0-pack/earth-and-ash-complete/, rebuilt from
# its generators with the 38 approved round-2 tiles) and are listed in
# assets/earth-and-ash-manifest.json. Isabella II stays only as the optional
# texturepacks/isabella-ii pack, so no credit text may call it the default.
# anim.png (2026-10-11): Beta 1.7.3 motion in E&A colours; the Beta-coloured bake
# moved to packs/beta-base (pinned by test_anim_pack_art).
EAD_PROMOTED = (
    "terrain.png",
    "gui/items.png",
    "particles.png",
    "terrain/sun.png",
    "terrain/moon.png",
    "environment/clouds.png",
    "gui_2.png",
    "gui/iron_chest.png",
    "item/arrows.png",
    "font/default.png",
    "anim.png",
)
EAD_PACK = "artwork/cc0-pack/earth-and-ash-complete"
EAD_STALE_DEFAULT_CLAIMS = (
    'Default texture pack: "Isabella II"',
    'Default texture pack credit: "Isabella II"',
    "in the bundled default art: assets/terrain.png",
)


def assert_no_isabella_default_claim(label: str, text: str) -> None:
    flat = " ".join(text.split())
    for claim in EAD_STALE_DEFAULT_CLAIMS:
        if claim in flat:
            raise AssertionError(f"EAD-001: {label} still says {claim!r}")


def test_earth_and_ash_default(src_repo: Path, bash: str, tmp: Path) -> None:
    """EAD-001: the eight promoted default sheets are the E&A pack's bytes,
    recorded in the CC0 manifest, never Isabella II's; NOTICE (both branches)
    and texturepacks/README.txt credit Isabella II only as the optional pack."""
    pack = {e["path"]: e for e in json.loads((src_repo / EAD_PACK / "manifest.json").read_text())}
    bundled = {e["path"]: e for e in json.loads((src_repo / "assets/earth-and-ash-manifest.json").read_text())}
    for rel in EAD_PROMOTED:
        if rel not in bundled:
            raise AssertionError(f"EAD-001: assets/earth-and-ash-manifest.json does not list {rel}")
        if bundled[rel]["sha256"] != pack[rel]["sha256"] or bundled[rel]["license"] != "CC0-1.0":
            raise AssertionError(f"EAD-001: manifest entry for {rel} is not the E&A pack's CC0 file")
        actual = hashlib.sha256((src_repo / "assets" / rel).read_bytes()).hexdigest()
        if actual != pack[rel]["sha256"]:
            raise AssertionError(f"EAD-001: assets/{rel} is not the Earth & Ash file (sha256 {actual})")
        isabella = src_repo / ISA_PACK_SOURCE / rel
        if isabella.is_file() and isabella.read_bytes() == (src_repo / "assets" / rel).read_bytes():
            raise AssertionError(f"EAD-001: assets/{rel} is still the Isabella II file")
    readme = (src_repo / "assets/README.md").read_text(encoding="utf-8")
    assert_no_isabella_default_claim("assets/README.md", readme)
    for repo_name, copy_notice, branch in (("scratch_repo_ead_notice", True, "repo NOTICE"),
                                           ("scratch_repo_ead_skeleton", False, "generated skeleton")):
        repo = build_scratch_repo(src_repo, tmp, name=repo_name, copy_notice=copy_notice)
        zip_path, _ = package_for_isabella(repo, bash, tmp / ("ead_" + repo_name))
        with zipfile.ZipFile(zip_path) as zf:
            for name in ("NOTICE", "texturepacks/README.txt", "README-RELEASE.txt"):
                text = zf.read(name).decode("utf-8", errors="replace")
                assert_no_isabella_default_claim(f"{name} ({branch})", text)
                if "Earth & Ash" not in text:
                    raise AssertionError(f"EAD-001: {name} ({branch}) does not name Earth & Ash as the default art")
    print(f"PASS: Earth & Ash is the bundled default ({len(EAD_PROMOTED)} promoted sheets); Isabella II credited only as the optional pack")


# REL-01b (2026-10-17): the certified path, input/proof refusals and the
# shipped fallback config. The KERNEL_HEAP/TLS/test-marker gates are driven
# through a FAKE devkitPPC (--devkitppc): two tiny shell scripts that print
# the objdump/readelf lines the real gates parse, chosen by marker bytes in
# the fake .elf. That runs the script's real gate and PASS-validation code
# without a toolchain; it proves the gate LOGIC, not a real linked binary.
FAKE_OBJDUMP = """#!/bin/sh
if grep -q STOCK_HEAP "$2"; then imm=1; else imm=4; fi
printf '80004000:\\t3c 60 00 0%s \\tlis     r3,%s\\n' "$imm" "$imm"
printf '80004004:\\t48 00 10 01 \\tbl      80005004 <__lwp_wkspace_init>\\n'
"""
FAKE_READELF = """#!/bin/sh
case "$1" in
  -h) echo "ELF Header:" ;;
  -SW) if grep -q TLS_SECTION "$2"; then echo "  [ 7] .tdata PROGBITS 80010000 010000 000010 00 WAT 0 0 4"; fi ;;
  -sW) if grep -q TEST_MARKER "$2"; then echo "    42: 80020000 4 OBJECT GLOBAL DEFAULT 9 cavewii_test_engine_marker"; fi ;;
esac
exit 0
"""
REL_BUILD_ID = "abc1234_10.17"


def write_fake_devkitppc(tmp: Path) -> Path:
    root = tmp / "fake devkitPPC"
    (root / "bin").mkdir(parents=True)
    for name, body in (("powerpc-eabi-objdump", FAKE_OBJDUMP), ("powerpc-eabi-readelf", FAKE_READELF)):
        path = root / "bin" / name
        path.write_bytes(body.encode("ascii"))
        path.chmod(0o755)
    return root


def write_release_pass(case_dir: Path, **changes: object) -> Path:
    data = {
        "status": "PASS", "run_id": "rel01b_run", "profile": "far_walk_6m",
        "release_performance_pass": True,
        "last_heartbeat": {"build_id": REL_BUILD_ID, "far_storage": 0, "far_view_distance": 6},
    }
    data.update(changes)
    path = case_dir / "PASS.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


def assert_sums_cover_zip(zf: zipfile.ZipFile, label: str) -> None:
    files = {n for n in zf.namelist() if not n.endswith("/")}
    sums = sha256_sums_map(zf)
    if set(sums) != files - {"SHA256SUMS"}:
        raise AssertionError(f"{label}: SHA256SUMS coverage differs: missing "
                             f"{sorted(files - {'SHA256SUMS'} - set(sums))}, extra {sorted(set(sums) - files)}")
    for name, digest in sums.items():
        if hashlib.sha256(zf.read(name)).hexdigest() != digest:
            raise AssertionError(f"{label}: SHA256SUMS digest wrong for {name}")


def test_certified_path_and_proof_refusals(scratch_repo: Path, bash: str, tmp: Path) -> None:
    """A matching release PASS packages a PERFORMANCE-VERIFIED zip through the
    real gates; a stale/mismatched, stability-only or failed PASS, a stock,
    TLS or test-instrumented .elf, a dev-stamped .dol and missing files are
    all refused with no zip written."""
    toolchain = write_fake_devkitppc(tmp)
    cases = (
        # name, dol stamp, elf marker, PASS changes (None = no --pass file), rc, message
        ("certified", REL_BUILD_ID, "", {}, 0, "PASS.json OK"),
        ("stale_pass", REL_BUILD_ID, "", {"last_heartbeat": {
            "build_id": "abc1233_10.16", "far_storage": 0, "far_view_distance": 6}}, 4, "does not certify THIS .dol"),
        ("stability_only", REL_BUILD_ID, "", {"release_performance_pass": False}, 3, "stability-only PASS"),
        ("failed_run", REL_BUILD_ID, "", {"status": "FAIL"}, 3, "status is not PASS"),
        ("wrong_profile", REL_BUILD_ID, "", {"profile": "heartbeat_selftest"}, 3, "six-minute walk"),
        ("stock_heap", REL_BUILD_ID, "STOCK_HEAP", {}, 4, "KERNEL_HEAP gate"),
        ("tls", REL_BUILD_ID, "TLS_SECTION", {}, 4, "TLS gate"),
        ("test_marker", REL_BUILD_ID, "TEST_MARKER", {}, 4, "test-marker gate"),
        ("dev_stamp", REL_BUILD_ID + "-dev", "", {"last_heartbeat": {
            "build_id": REL_BUILD_ID + "-dev", "far_storage": 0, "far_view_distance": 6}}, 3, "dev build stamp"),
        ("missing_pass_file", REL_BUILD_ID, "", None, 3, "--pass file not found"),
        ("missing_elf", REL_BUILD_ID, "NO_ELF", {}, 3, ".elf not found"),
        ("missing_dol", None, "", {}, 3, ".dol not found"),
    )
    for name, stamp, marker, changes, want_rc, message in cases:
        case_dir = tmp / ("rel01b_" + name)
        case_dir.mkdir()
        dol, elf = case_dir / "CaveWii_v2.dol", case_dir / "CaveWii_v2.elf"
        if stamp is not None:
            dol.write_bytes(b"\x00\x00fake dol\x00\x00" + stamp.encode("ascii") + b"\x00\x00")
        if marker != "NO_ELF":
            elf.write_bytes(b"fake elf " + marker.encode("ascii") + b"\n")
        pass_json = write_release_pass(case_dir, **changes) if changes is not None else case_dir / "absent.json"
        out_dir = case_dir / "dist"
        proc = run(make_release_cmd(scratch_repo, bash, [
            "--dol", str(dol), "--elf", str(elf), "--pass", str(pass_json),
            "--devkitppc", str(toolchain), "--version", "0.1.0", "--out", str(out_dir),
        ]), case_dir)
        if proc.returncode != want_rc or message not in proc.stdout:
            raise AssertionError(f"REL-01b {name}: expected rc={want_rc} with {message!r}; "
                                 f"rc={proc.returncode} output:\n{proc.stdout}")
        if want_rc != 0:
            assert_no_zip_written(out_dir)
            continue
        archive, = out_dir.glob("*.zip")
        if not archive.name.endswith("-Wii.zip") or "UNCERTIFIED" in archive.name:
            raise AssertionError(f"REL-01b: certified zip misnamed: {archive.name}")
        with zipfile.ZipFile(archive) as zf:
            prov = json.loads(zf.read("provenance.json"))
            if prov["certification_status"] != "PERFORMANCE_VERIFIED":
                raise AssertionError(f"REL-01b: certification_status {prov['certification_status']!r}")
            if set(prov["gates"].values()) != {"PASSED"}:
                raise AssertionError(f"REL-01b: certified gates not all PASSED: {prov['gates']}")
            if prov["hardware_pass"] != {"path_basename": "PASS.json", "run_id": "rel01b_run",
                                         "profile": "far_walk_6m", "build_id": REL_BUILD_ID}:
                raise AssertionError(f"REL-01b: hardware_pass record wrong: {prov['hardware_pass']}")
            if prov["dol"]["embedded_build_ids"] != [REL_BUILD_ID]:
                raise AssertionError(f"REL-01b: DOL stamp not recorded: {prov['dol']}")
            if zf.read("apps/CavEX/boot.dol") != dol.read_bytes():
                raise AssertionError("REL-01b: boot.dol is not the certified .dol")
            assert_sums_cover_zip(zf, "certified zip")
    print(f"PASS: certified package via the real gates + {len(cases) - 1} proof/input refusals")


def commit_variant(repo: Path, base: str, path: str, content: bytes | None) -> str:
    """Commit `path` removed (content None) or rewritten on top of `base`,
    return that commit, and put the scratch repo back on `base`."""
    if content is None:
        git(repo, "rm", "--quiet", "--cached", "--", path)
    else:
        (repo / path).write_bytes(content)
        git(repo, "add", "--", path)
    git(repo, "commit", "--quiet", "-m", f"REL-01b variant: {path}")
    sha = git(repo, "rev-parse", "HEAD").stdout.strip()
    git(repo, "reset", "--quiet", "--hard", base)
    return sha


def test_missing_and_unusable_inputs_refused(src_repo: Path, bash: str, tmp: Path) -> None:
    repo = build_scratch_repo(src_repo, tmp, name="scratch_repo_rel01b_inputs")
    base = git(repo, "rev-parse", "HEAD").stdout.strip()
    capture = dict(MINIMAL_CONFIG_WII_JSON, debug={"overlay": "off", "autopilot": "walk"})
    variants = [(rel, None, "required release input missing") for rel in (
        "LICENSE", "icon.png", "config_wii.json", "LICENSES/Isabella-II/About Isabella.txt",
        "LICENSES/Isabella-II/LICENSE.txt", "packs/isabella-ii/README.txt")]
    variants += [(rel, None, "the release must ship the tutorial world") for rel in (
        "tools/tutorial_world/build_tutorial.py", "tests/test_tutorial_build.py")]
    variants += [
        ("config_wii.json", b'{"input": {', "not valid JSON"),
        ("config_wii.json", b"[1, 2]", "root is not a JSON object"),
        ("config_wii.json", json.dumps(dict(MINIMAL_CONFIG_WII_JSON, input={})).encode(), "no input map"),
        ("config_wii.json", json.dumps(capture).encode(), "debug.autopilot"),
        ("config_wii.json", json.dumps(dict(MINIMAL_CONFIG_WII_JSON, debug={"overlay": "on"})).encode(),
         "forces the debug overlay on"),
        # The template is the ONLY shipped config (no derived config.json), so
        # a dev pack path in it would be the release default: refused.
        ("config_wii.json", json.dumps(dict(MINIMAL_CONFIG_WII_JSON,
         paths={"texturepack": "texturepacks/beta-1.7.3", "worlds": "saves"})).encode(),
         "paths.texturepack must be the bundled 'assets' pack"),
    ]
    dol_dir = tmp / "rel01b_inputs_dol"
    dol_dir.mkdir()
    dol, elf = write_fake_dol_elf(dol_dir)
    for index, (rel, content, message) in enumerate(variants):
        sha = commit_variant(repo, base, rel, content)
        out_dir = tmp / f"rel01b_inputs_out_{index}"
        proc = run(make_release_cmd(repo, bash, [
            "--dol", str(dol), "--elf", str(elf), "--allow-uncertified",
            "--skip-objdump-gate-for-tests", "--commit", sha, "--out", str(out_dir),
        ]), tmp)
        if proc.returncode != 3 or message not in proc.stdout:
            raise AssertionError(f"REL-01b: {rel} variant {index} expected rc=3 with {message!r}; "
                                 f"rc={proc.returncode} output:\n{proc.stdout}")
        assert_no_zip_written(out_dir)
    print(f"PASS: {len(variants)} missing/unusable release inputs refused before packaging")


def test_tutorial_world_failures_refused(src_repo: Path, bash: str, tmp: Path) -> None:
    """Card tutorial-1021: a failing tutorial acceptance test, a failing
    builder, or a builder that writes no world each stop the release (rc 6,
    no zip). The failing test must also stop the builder from running."""
    repo = build_scratch_repo(src_repo, tmp, name="scratch_repo_tutorial_fail")
    base = git(repo, "rev-parse", "HEAD").stdout.strip()
    marker = tmp / "tutorial_builder_ran"
    builder = "tools/tutorial_world/build_tutorial.py"
    variants = [
        # failing test; the builder leaves a marker if it is (wrongly) run anyway
        ({"tests/test_tutorial_build.py": b"raise SystemExit(1)\n",
          builder: STUB_TUTORIAL_BUILDER + f"open({str(marker)!r}, 'w').close()\n".encode()},
         "tests/test_tutorial_build.py failed"),
        ({builder: b"raise SystemExit(2)\n"}, f"{builder} failed"),
        ({builder: b"pass\n"}, "wrote no level.dat"),
        # incomplete world: level.dat but no region file
        ({builder: b"import argparse, os\nap = argparse.ArgumentParser()\nap.add_argument('--out', required=True)\nout = ap.parse_args().out\nos.makedirs(os.path.join(out, 'region'))\nopen(os.path.join(out, 'level.dat'), 'wb').write(b'x')\n"},
         "wrote no level.dat"),
    ]
    dol_dir = tmp / "tutorial_fail_dol"
    dol_dir.mkdir()
    dol, elf = write_fake_dol_elf(dol_dir)
    for index, (files, message) in enumerate(variants):
        for rel, content in files.items():
            (repo / rel).write_bytes(content)
            git(repo, "add", "--", rel)
        git(repo, "commit", "--quiet", "-m", f"tutorial failure variant {index}")
        sha = git(repo, "rev-parse", "HEAD").stdout.strip()
        git(repo, "reset", "--quiet", "--hard", base)
        out_dir = tmp / f"tutorial_fail_out_{index}"
        proc = run(make_release_cmd(repo, bash, [
            "--dol", str(dol), "--elf", str(elf), "--allow-uncertified",
            "--skip-objdump-gate-for-tests", "--commit", sha, "--out", str(out_dir),
        ]), tmp)
        if proc.returncode != 6 or message not in proc.stdout:
            raise AssertionError(f"tutorial variant {index} expected rc=6 with {message!r}; "
                                 f"rc={proc.returncode} output:\n{proc.stdout}")
        assert_no_zip_written(out_dir)
    if marker.exists():
        raise AssertionError("the tutorial builder ran although its acceptance test failed")
    print(f"PASS: {len(variants)} tutorial-world test/build failures stop the release")


def test_usable_fallback_defaults(src_repo: Path, bash: str, tmp: Path) -> None:
    """The REAL shipped config_wii.json is packaged, byte-for-byte, as the
    ONLY config at BOTH boot CWDs (SD root and apps/CavEX/): no config.json
    is shipped (it is the player's file, ci-1101/05), the template already
    names the release paths, and the default pack directory it names is in
    the zip at both."""
    repo = build_scratch_repo(src_repo, tmp, name="scratch_repo_rel01b_defaults")
    shutil.copyfile(src_repo / "config_wii.json", repo / "config_wii.json")
    git(repo, "add", "config_wii.json")
    git(repo, "commit", "--quiet", "-m", "real shipped config_wii.json")
    committed = subprocess.run(["git", "cat-file", "blob", "HEAD:config_wii.json"], cwd=repo,
                               stdout=subprocess.PIPE, check=True, timeout=60).stdout
    zip_path, _ = package_for_isabella(repo, bash, tmp / "rel01b_defaults")
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())
        template = json.loads(committed)
        for root in ("", "apps/CavEX/"):
            if zf.read(root + "config_wii.json") != committed:
                raise AssertionError(f"REL-01b: {root}config_wii.json is not the committed fallback template")
            if root + "config.json" in names:
                raise AssertionError(f"REL-01b: {root}config.json shipped; an update would wipe the player's MP name/servers")
            if template["paths"]["texturepack"] != "assets" or template["paths"]["worlds"] != "saves":
                raise AssertionError("REL-01b: the shipped template does not name the release paths (assets, saves)")
            pack_dir = root + template["paths"]["texturepack"] + "/"
            if not any(n.startswith(pack_dir) for n in names):
                raise AssertionError(f"REL-01b: default pack {pack_dir} named by {root}config_wii.json is not in the zip")
        assert_sums_cover_zip(zf, "defaults zip")
    print("PASS: real config_wii.json ships as the only config (fallback template) at both boot CWDs; no config.json")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="the enclosing dev repo, used only as a file source to build the scratch repo from")
    ap.add_argument("--bash", required=True, help="path to a bash executable (git-bash/MSYS2 on Windows, /bin/bash on Linux/WSL)")
    args = ap.parse_args()

    src_repo = Path(args.repo).resolve()
    bash = args.bash

    import tempfile

    with tempfile.TemporaryDirectory(prefix="cavewii-make-release-test-") as td:
        tmp = Path(td)
        scratch_repo = build_scratch_repo(src_repo, tmp)

        # Exercise the real embedded proof validator independently of the
        # devkitPPC object gate. A stability PASS and a mismatched config must
        # never get a release-performance label, even with a matching build ID.
        script = (src_repo / "tools/make_release.sh").read_text()
        gate = script.split('PASS_FIELDS="$("$PY" - ', 1)[1]
        gate = gate.split("<<'PYEOF'\n", 1)[1].split('\nPYEOF', 1)[0]
        proof = json.loads(write_fake_pass(tmp).read_text())
        proof["profile"] = "keepup_walk_6m"
        proof["last_heartbeat"].update(far_view_distance=6, far_storage=0)
        for accepted, changes in ((False, {}),
                                  (True, {"release_performance_pass": True}),
                                  (False, {"last_heartbeat": {
                                      **proof["last_heartbeat"], "far_storage": 1}})):
            proof.update(changes)
            path = tmp / "gate-pass.json"
            path.write_text(json.dumps(proof))
            result = run([sys.executable, "-c", gate, str(path),
                          str(scratch_repo), "HEAD"], tmp)
            assert (result.returncode == 0) == accepted, result.stdout

        shipped = json.loads((src_repo / "config_wii.json").read_text())
        assert shipped["lod"]["far_storage"] == "voxel"
        assert shipped["lod"]["far_view_distance"] == 6
        # 2026-09-19, maintainer in person ("I agree, cavex layout") ratified by
        # Astra ruling 3 D1: the shipped Wii Remote scheme is the original CavEX
        # one -- D-pad MOVES (code 0, not the emulated stick 900), the Nunchuk
        # stick is the CAMERA so pointer look ships OFF, 1 is SNEAK (code 6, not
        # the Nunchuk C 101 that now places), and nothing on the Remote throws an
        # item in gameplay (the held stack is dropped from the inventory screen).
        #
        # 2026-09-19 (Astra ergonomics study 4 and 6, maintainer-accepted):
        # NO Wii controller has a gameplay drop any more -- Classic B and
        # GameCube B lost theirs for the same reason the Remote never had one,
        # so player_drop is empty on all three and dropping is an inventory
        # visit. The shipped file also states a sneak-toggle key per family.
        assert shipped["controls"]["wiimote_ir_look"] == 0
        assert 0 in shipped["input"]["player_forward"]
        assert 6 in shipped["input"]["player_sneak"]
        assert shipped["input"]["player_drop"] == []
        for key in ("wiimote_sneak_toggle", "classic_sneak_toggle",
                    "gamecube_sneak_toggle"):
            assert shipped["controls"][key] == 1
        # The F3 chords left the release configuration (ergonomics 2): an
        # EXPLICIT empty array, never an omitted key.
        assert shipped["input"]["debug_overlay"] == []

        test_stock_dol_refused(scratch_repo, bash, tmp)
        test_non_release_names_refused(scratch_repo, bash, tmp)
        test_missing_pass_refused_without_allow_uncertified(scratch_repo, bash, tmp)
        test_skip_gate_forbidden_with_pass(scratch_repo, bash, tmp)
        test_skip_gate_requires_allow_uncertified(scratch_repo, bash, tmp)
        test_publish_flag_refused(scratch_repo, bash, tmp)
        test_zip_name_explicit_version(scratch_repo, bash, tmp)
        test_full_package_layout_and_provenance(scratch_repo, bash, tmp)
        test_output_path_forms(scratch_repo, bash, tmp)
        test_earth_and_ash_assets(src_repo, scratch_repo, bash, tmp)
        test_isabella_attribution(src_repo, scratch_repo, bash, tmp)
        # ISA-001: runs last, after every existing check has passed.
        test_isabella_ii_optional_pack(src_repo, bash, tmp)
        test_earth_and_ash_default(src_repo, bash, tmp)
        test_certified_path_and_proof_refusals(scratch_repo, bash, tmp)
        test_missing_and_unusable_inputs_refused(src_repo, bash, tmp)
        test_tutorial_world_failures_refused(src_repo, bash, tmp)
        test_usable_fallback_defaults(src_repo, bash, tmp)

    print("PASS test_make_release")
    return 0


if __name__ == "__main__":
    sys.exit(main())
