"""Build the helper on its target OS. This never builds or publishes the Wii game.

Reproducibility contract (2026-09-17 acceptance review, finding H2):

* The interpreter is PINNED to the one CI uses. A bundle frozen by a different
  CPython embeds a different runtime, so a "same inputs, same output" claim
  would be false. Mismatch is a hard failure here, not a warning.
* The TRANSITIVE CLOSURE of `requirements-build.txt` is resolved and compared
  against `requirements-build.<platform>.lock`; the build REFUSES to run
  without a matching lock and prints the exact file to commit. (CI uploads the
  resolved list on every run precisely so that file needs no guesswork.)
  Deliberately NOT a snapshot of every installed distribution: pip/setuptools/
  wheel drift with the venv rather than with what is frozen into the bundle,
  so snapshotting them would make every environment but CI's fail the lock
  forever (2026-09-17 DELTA review, MEDIUM). The closure is per-OS, hence the
  platform in the lock's name.
* `EXCLUDED_MODULES` drops what PyInstaller's hooks would otherwise drag in
  although nothing in the helper imports it -- numpy above all, which the PIL
  hook pulls in and which cost 27 of the candidate bundle's 70 MB.
  tests/test_pack_helper_desktop.py import-scans the helper's own sources and
  fails if any excluded name is genuinely imported.
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

try:  # packaging ships with PyInstaller; absent in a bare test environment.
    from packaging.markers import InvalidMarker, Marker
except ImportError:
    Marker = None

    class InvalidMarker(Exception):
        pass

from build_resources import build

# The CI interpreter (.github/workflows/pack-helper.yml `python-version`).
REQUIRED_PYTHON = (3, 12)

# The transitive closure differs per OS (pefile/pywin32-ctypes on Windows,
# macholib on macOS), so each platform locks its own -- one shared file could
# never match all three.
LOCK_NAME = "requirements-build.{0}.lock".format(sys.platform)

# Nothing in tools/pack_helper/ or tools/import_owned_beta_assets.py imports
# any of these; PyInstaller's PIL/Tk hooks pick some of them up opportunistically
# whenever they happen to be installed, which makes the bundle both bigger and
# dependent on what else the build host has.
# Reproducible freeze (PyInstaller's documented recipe).
#
# Measured 2026-09-17: two builds of identical sources, from the same pinned
# venv, produced bundles with identical 1097-entry inventories but two
# differing files -- `_internal/base_library.zip`, whose 155 members had
# identical CRCs and identical mtimes but a different ORDER, and the bootloader
# .exe carrying the same archive. That is PyInstaller iterating hash-ordered
# containers: the module set's iteration order changes with the interpreter's
# per-process hash seed. Pin the seed and the freeze becomes ordered.
#
# SOURCE_DATE_EPOCH is a PINNED CONSTANT, deliberately not the clock and not a
# git timestamp: both would reintroduce the "same inputs, different output"
# property this exists to remove. Bump it on purpose when a release wants a new
# stamp; it is recorded in build_inputs.json either way. 1789603200 is
# 2026-09-17T00:00:00Z, the day this contract landed.
FREEZE_PYTHONHASHSEED = "1"
FREEZE_SOURCE_DATE_EPOCH = "1789603200"

EXCLUDED_MODULES = (
    "numpy",
    "matplotlib",
    "scipy",
    "pandas",
    "IPython",
    "pytest",
    "PyQt5",
    "PyQt6",
    "PySide2",
    "PySide6",
)


def _require_interpreter():
    if sys.version_info[:2] != REQUIRED_PYTHON:
        raise SystemExit(
            "pack_helper: refusing to freeze a bundle with Python {0}.{1}; the "
            "release interpreter is {2}.{3} (see .github/workflows/pack-helper.yml). "
            "Rebuild on the pinned interpreter, or let CI produce the artifact.".format(
                sys.version_info[0], sys.version_info[1], *REQUIRED_PYTHON))


def _normalise(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).strip().lower()


# Installer tooling, not build INPUT: every venv carries its own pip/setuptools/
# wheel, and their versions drift with the venv rather than with what is frozen
# into the bundle. Snapshotting them made every environment but the CI one fail
# the lock forever (2026-09-17 DELTA review, MEDIUM).
TOOLING_DISTRIBUTIONS = frozenset({"pip", "setuptools", "wheel", "pkg-resources",
                                   "distribute", "uv"})


def _requirement_roots(requirements: Path) -> list:
    roots = []
    for line in requirements.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or line.startswith("-"):
            continue
        roots.append(_normalise(re.split(r"[<>=!~\[; ]", line, maxsplit=1)[0]))
    return roots


def resolved_build_inputs(requirements: Path = None, metadata: dict = None) -> dict:
    """The TRANSITIVE CLOSURE of requirements-build.txt, as {name: version}.

    Not "everything installed": that made the lock a snapshot of one machine's
    venv. Walk each pinned root's `Requires-Dist` metadata instead, so the lock
    pins exactly what the bundle is built from and stays indifferent to the
    installer tooling and to unrelated packages in a developer's environment.

    `metadata` is an injection seam for tests: {name: (version, [requires])}.
    """
    if requirements is None:
        requirements = Path(__file__).resolve().parent / "requirements-build.txt"
    if metadata is None:
        metadata = {}
        for dist in importlib.metadata.distributions():
            name = dist.metadata["Name"]
            if not name:
                continue
            metadata[_normalise(name)] = (dist.version, dist.requires or [])

    resolved, pending, seen = {}, list(_requirement_roots(requirements)), set()
    while pending:
        name = pending.pop()
        if name in seen or name in TOOLING_DISTRIBUTIONS:
            continue
        seen.add(name)
        entry = metadata.get(name)
        if entry is None:
            continue  # absent members are reported by the lock comparison
        version, requires = entry
        resolved[name] = version
        for requirement in requires:
            # "pefile >=2022.5.30 ; sys_platform == 'win32'" -- the dependency
            # is only an input where its marker applies on THIS platform.
            requirement, _, marker = requirement.partition(";")
            if marker.strip() and Marker is not None:
                try:
                    if not Marker(marker.strip()).evaluate():
                        continue
                except InvalidMarker:
                    pass  # unparseable marker: keep it rather than drop an input
            dependency = _normalise(re.split(r"[<>=!~\[( ]", requirement.strip(), maxsplit=1)[0])
            if dependency and dependency not in seen:
                pending.append(dependency)
    return resolved


def lock_text(resolved: dict) -> str:
    return "".join("{0}=={1}\n".format(name, resolved[name]) for name in sorted(resolved))


def _require_lock(repo: Path, resolved: dict = None):
    lock_path = repo / "tools/pack_helper" / LOCK_NAME
    if resolved is None:
        resolved = resolved_build_inputs(
            repo / "tools/pack_helper/requirements-build.txt")
    produced = lock_text(resolved)
    if not lock_path.is_file():
        raise SystemExit(
            "pack_helper: {0} is missing, so this build is not reproducible from "
            "pinned inputs. Resolve tools/pack_helper/requirements-build.txt on "
            "Python {1}.{2} and commit exactly this file:\n\n{3}".format(
                lock_path, REQUIRED_PYTHON[0], REQUIRED_PYTHON[1], produced))
    expected = {}
    for line in lock_path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "==" not in line:
            raise SystemExit("pack_helper: {0} line is not an exact pin: {1}".format(
                lock_path, line))
        name, version = line.split("==", 1)
        expected[_normalise(name)] = version.strip()
    missing = sorted(name for name in expected if name not in resolved)
    extra = sorted(name for name in resolved if name not in expected)
    changed = sorted("{0}: locked {1}, installed {2}".format(name, expected[name], resolved[name])
                     for name in expected if name in resolved and resolved[name] != expected[name])
    if missing or extra or changed:
        raise SystemExit(
            "pack_helper: build environment does not match {0}.\n"
            "  missing: {1}\n  unexpected: {2}\n  version drift: {3}\n"
            "Install exactly the locked set, or regenerate the lock deliberately.".format(
                lock_path, missing, extra, changed))
    return resolved


def rehash_resources(resources: Path):
    """Re-stamp resource_hashes.json after build_inputs.json joined the tree.

    selftest._verify_resources demands an EXACT inventory match, so anything
    added after build_resources.build() has to be folded back in.
    """
    manifest_path = resources / "resource_hashes.json"
    manifest = {p.relative_to(resources).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(resources.rglob("*"))
                if p.is_file() and p != manifest_path}
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8",
                             newline="\n")


def freeze_environment(base: dict = None) -> dict:
    """The child environment the FREEZE runs in.

    PYTHONHASHSEED only takes effect at interpreter start-up, so it has to be
    set for the child that actually runs PyInstaller -- setting it in this
    process would do nothing. The freeze is already a subprocess; this is what
    it inherits.
    """
    environment = dict(os.environ if base is None else base)
    environment["PYTHONHASHSEED"] = FREEZE_PYTHONHASHSEED
    environment["SOURCE_DATE_EPOCH"] = FREEZE_SOURCE_DATE_EPOCH
    return environment


def build_inputs_payload(resolved: dict) -> dict:
    """What the bundle records about how it was built."""
    return {
        "python": "{0}.{1}.{2}".format(*sys.version_info[:3]),
        "distributions": resolved,
        "excluded_modules": list(EXCLUDED_MODULES),
        "pythonhashseed": FREEZE_PYTHONHASHSEED,
        "source_date_epoch": FREEZE_SOURCE_DATE_EPOCH,
    }


def package(repo, output, cc):
    repo, output = Path(repo).resolve(), Path(output).resolve()
    _require_interpreter()
    resolved = _require_lock(repo)
    with tempfile.TemporaryDirectory(prefix="cw-helper-build-") as temporary:
        work = Path(temporary)
        resources = work / "resources"
        build(repo, resources, cc)
        # Recorded INSIDE the bundle, so the frozen smoke check's whole-tree
        # hash covers the exact inputs the bundle was frozen from.
        (resources / "build_inputs.json").write_text(
            json.dumps(build_inputs_payload(resolved), indent=2, sort_keys=True),
            encoding="utf-8", newline="\n")
        rehash_resources(resources)
        excludes = []
        for module in EXCLUDED_MODULES:
            excludes += ["--exclude-module", module]
        icon = ["--icon", str(resources / "branding/computer.ico")] if sys.platform == "win32" else []
        subprocess.run([
            sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed", "--onedir",
            "--name", "CavEX Asset Importer", "--paths", str(repo / "tools"),
            "--add-data", str(resources) + os.pathsep + "pack_helper/resources",
            "--add-data", str(repo / "tools/pack_helper/conversion_map.json") + os.pathsep + "pack_helper",
            "--hidden-import", "pack_helper.owned_importer", "--hidden-import", "PIL._tkinter_finder",
            *excludes, *icon,
            "--distpath", str(output), "--workpath", str(work / "build"), "--specpath", str(work),
            str(repo / "tools/pack_helper/launch.py")],
            check=True, env=freeze_environment())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, default=Path("dist/pack-helper"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--emit-lock", type=Path, default=None,
                        help="write the resolved build inputs here and exit; this is the "
                             "text tools/pack_helper/requirements-build.lock must contain")
    args = parser.parse_args()
    if args.emit_lock is not None:
        args.emit_lock.parent.mkdir(parents=True, exist_ok=True)
        args.emit_lock.write_text(lock_text(resolved_build_inputs()),
                                  encoding="utf-8", newline="\n")
        print("wrote resolved build inputs to " + str(args.emit_lock))
    else:
        package(args.repo, args.out, args.cc)
