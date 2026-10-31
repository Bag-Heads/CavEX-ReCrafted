import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch
import zipfile
from PIL import Image

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
# package_app.py and build_resources.py are run as scripts from inside their own
# directory ("python tools/pack_helper/package_app.py"), so they import each
# other as TOP-LEVEL modules. Import them the same way the build does.
sys.path.append(str(REPO / "tools/pack_helper"))
from pack_helper.preview import preview_sheet
from pack_helper.discovery import discover_packs
from pack_helper.assemble_release import assemble
from pack_helper.selftest import _verify_resources
from pack_helper import selftest as selftest_module
from pack_helper import appearance
import ast
import re
import build_resources
import package_app
import smoke_packaged


def _file_info(name, mode=0o100644, *, stored=False):
    info = zipfile.ZipInfo(name, date_time=(2025, 8, 9, 10, 12, 14))
    info.create_system = 3
    info.external_attr = mode << 16
    info.internal_attr = 1
    info.comment = ("metadata:" + name).encode()
    info.extra = b"\x01\x00\x01\x00x"
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    return info


def _make_game_zip(path, extra=None):
    game = {
        "apps/CavEX/boot.dol": b"synthetic game paths.audiopack\x00",
        "provenance.json": b'{"certification_status":"UNCERTIFIED"}',
    }
    game.update(extra or {})
    sums = "".join(hashlib.sha256(value).hexdigest() + "  " + name + "\n"
                   for name, value in game.items()).encode()
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(_file_info("apps/", stat.S_IFDIR | 0o755), b"")
        archive.writestr(_file_info("apps/CavEX/", stat.S_IFDIR | 0o755), b"")
        for index, (name, value) in enumerate(game.items()):
            archive.writestr(_file_info(name, stat.S_IFREG | (0o755 if index == 0 else 0o644),
                                        stored=index == 0), value)
        archive.writestr(_file_info("SHA256SUMS"), sums)
    return game, sums


def _read_sums(archive):
    rows = {}
    for row in archive.read("SHA256SUMS").decode().splitlines():
        digest, name = row.split("  ", 1)
        rows[name] = digest
    return rows


def _make_resource_tree(root, omit=()):
    values = {
        "mapping.json": json.dumps({"sound_keys": ["step.grass"],
                                    "records": ["13", "cat"]}).encode(),
        "conversion_map.json": b"{}",
        "assets/gui_2.png": b"gui",
        "beta-base/anim.png": b"anim",
        "beta-base/pointer.png": b"pointer",
        "defaults/config_wii.json": b"{}",
        "defaults/config_pc.json": b"{}",
        "branding/logo.png": b"logo",
        "branding/computer.png": b"computer",
        "branding/computer.ico": b"icon",
        "LICENSE": b"GPLv3 text",
        "NOTICE": b"third-party attributions",
        "LICENSES/GPL-2.0.txt": b"GPLv2 text",
        "sound_transcode.exe" if os.name == "nt" else "sound_transcode": b"native",
    }
    for relative, value in values.items():
        if relative in omit:
            continue
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)
    hashes = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in root.rglob("*") if path.is_file()}
    (root / "resource_hashes.json").write_text(json.dumps(hashes), encoding="utf-8")


def _mock_link(helper, name, resolved):
    link = MagicMock()
    link.as_posix.return_value = (helper / name).as_posix()
    link.relative_to.return_value = Path(name)
    link.is_symlink.return_value = True
    if isinstance(resolved, BaseException):
        link.resolve.side_effect = resolved
    else:
        link.resolve.return_value = resolved
    return link


class DesktopTests(unittest.TestCase):
    def test_discovery_does_not_recurse_or_modify(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "versions/b1.7.3").mkdir(parents=True)
            jar = root / "versions/b1.7.3/b1.7.3.jar"
            jar.write_bytes(b"candidate only")
            with patch("pack_helper.discovery.minecraft_roots", return_value=[root]):
                self.assertEqual(discover_packs(), [str(jar)])
            self.assertEqual(jar.read_bytes(), b"candidate only")

    def test_preview_light_dark_and_item_from_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "gui").mkdir()
            Image.new("RGBA", (256, 256), (0, 0, 0, 0)).save(root / "terrain.png")
            items = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
            items.paste((231, 51, 71, 255), (48, 64, 64, 80))
            items.save(root / "gui/items.png")
            preview = preview_sheet(root)
            self.assertEqual(preview.size, (500, 215))
            self.assertEqual(preview.getpixel((20, 40)), (248, 247, 242))
            self.assertEqual(preview.getpixel((20, 140)), (36, 41, 37))
            self.assertEqual(preview.getpixel((420, 40)), (231, 51, 71))

    def test_release_preserves_game_metadata_provenance_and_checksums(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game, original_sums = _make_game_zip(root / "game.zip")
            original_digest = hashlib.sha256((root / "game.zip").read_bytes()).hexdigest()
            with zipfile.ZipFile(root / "game.zip") as source:
                metadata = {item.filename: item for item in source.infolist()}
            helper = root / "helper"
            helper.mkdir()
            _make_resource_tree(helper / "_internal/pack_helper/resources")
            app = helper / "app.exe"
            app.write_bytes(b"synthetic helper")
            app.chmod(app.stat().st_mode | stat.S_IXUSR)
            output = root / "release.zip"
            assemble(root / "game.zip", helper, output, "Windows")
            with zipfile.ZipFile(output) as archive:
                for key, value in game.items():
                    self.assertEqual(archive.read(key), value)
                    before, after = metadata[key], archive.getinfo(key)
                    self.assertEqual(after.date_time, before.date_time)
                    self.assertEqual(after.create_system, before.create_system)
                    self.assertEqual(after.external_attr, before.external_attr)
                    self.assertEqual(after.internal_attr, before.internal_attr)
                    self.assertEqual(after.comment, before.comment)
                    self.assertEqual(after.extra, before.extra)
                    self.assertEqual(after.compress_type, before.compress_type)
                rows = _read_sums(archive)
                for name, digest in rows.items():
                    self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)
                regular = {
                    item.filename for item in archive.infolist()
                    if not item.is_dir()
                    and not stat.S_ISLNK(item.external_attr >> 16)
                    and item.filename != "SHA256SUMS"
                }
                self.assertEqual(set(rows), regular)
                self.assertEqual(archive.read("Pack Helper/app.exe"), b"synthetic helper")
                self.assertTrue(archive.getinfo("Pack Helper/app.exe").external_attr >> 16 & stat.S_IXUSR)
                release = json.loads(archive.read("pack-helper-release.json"))
                self.assertEqual(release["game_release_sha256"], original_digest)
                self.assertEqual(release["game_checksum_inventory_sha256"],
                                 hashlib.sha256(original_sums).hexdigest())
                self.assertEqual(release["game_provenance_sha256"],
                                 hashlib.sha256(game["provenance.json"]).hexdigest())
            with self.assertRaises(FileExistsError):
                assemble(root / "game.zip", helper, output, "Windows")

    def test_release_refuses_a_helper_without_its_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_game_zip(root / "game.zip")
            helper = root / "helper"
            helper.mkdir()
            _make_resource_tree(helper / "_internal/pack_helper/resources",
                                omit=("assets/gui_2.png",))
            (helper / "app.exe").write_bytes(b"helper")
            with self.assertRaisesRegex(ValueError, "missing pack_helper/resources.*gui_2.png"):
                assemble(root / "game.zip", helper, root / "release.zip", "Windows")

    def test_release_rejects_case_insensitive_destination_collision(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_game_zip(root / "game.zip", {"Pack Helper/App.exe": b"old"})
            helper = root / "helper"
            helper.mkdir()
            _make_resource_tree(helper / "_internal/pack_helper/resources")
            (helper / "app.exe").write_bytes(b"new")
            with self.assertRaisesRegex(ValueError, "Case-insensitive"):
                assemble(root / "game.zip", helper, root / "release.zip", "Linux")

    def test_release_symlink_manifest_and_regular_file_checksums(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_game_zip(root / "game.zip")
            helper = root / "helper"
            helper.mkdir()
            _make_resource_tree(helper / "_internal/pack_helper/resources")
            (helper / "real.bin").write_bytes(b"target bytes")
            try:
                os.symlink("real.bin", helper / "alias.bin")
            except OSError:
                link = _mock_link(helper, "alias.bin", (helper / "real.bin").resolve())
                with patch.object(Path, "rglob", return_value=[link, helper / "real.bin"]), \
                        patch("pack_helper.assemble_release.os.readlink", return_value="real.bin"):
                    assemble(root / "game.zip", helper, root / "release.zip", "macOS")
            else:
                assemble(root / "game.zip", helper, root / "release.zip", "macOS")
            with zipfile.ZipFile(root / "release.zip") as archive:
                alias = archive.getinfo("Pack Helper/alias.bin")
                self.assertTrue(stat.S_ISLNK(alias.external_attr >> 16))
                self.assertEqual(archive.read(alias), b"real.bin")
                rows = _read_sums(archive)
                self.assertNotIn(alias.filename, rows)
                self.assertIn("Pack Helper/real.bin", rows)
                manifest = json.loads(archive.read("pack-helper-symlinks.json"))
                self.assertIn({"path": alias.filename, "target": "real.bin"}, manifest["links"])
                names = set(archive.namelist())
                for link in manifest["links"]:
                    resolved = (PurePosixPath(link["path"]).parent / link["target"])
                    parts = []
                    for part in resolved.parts:
                        if part == "..":
                            parts.pop()
                        elif part != ".":
                            parts.append(part)
                    self.assertIn(PurePosixPath(*parts).as_posix(), names)
                for name, digest in rows.items():
                    self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)

    def test_release_rejects_broken_and_escaping_symlinks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_game_zip(root / "game.zip")
            helper = root / "helper"
            helper.mkdir()
            _make_resource_tree(helper / "_internal/pack_helper/resources")
            try:
                os.symlink("missing.bin", helper / "broken")
            except OSError:
                broken = _mock_link(helper, "broken", FileNotFoundError("missing"))
                with patch.object(Path, "rglob", return_value=[broken]), \
                        patch("pack_helper.assemble_release.os.readlink",
                              return_value="missing.bin"), \
                        self.assertRaisesRegex(ValueError, "missing"):
                    assemble(root / "game.zip", helper, root / "broken.zip", "Linux")
            else:
                with self.assertRaisesRegex(ValueError, "missing"):
                    assemble(root / "game.zip", helper, root / "broken.zip", "Linux")
                (helper / "broken").unlink()
            outside = root / "outside.bin"
            outside.write_bytes(b"outside")
            try:
                os.symlink("../outside.bin", helper / "escape")
            except OSError:
                escape = _mock_link(helper, "escape", outside.resolve())
                with patch.object(Path, "rglob", return_value=[escape]), \
                        patch("pack_helper.assemble_release.os.readlink",
                              return_value="../outside.bin"), \
                        self.assertRaisesRegex(ValueError, "escapes"):
                    assemble(root / "game.zip", helper, root / "escape.zip", "Linux")
            else:
                with self.assertRaisesRegex(ValueError, "escapes"):
                    assemble(root / "game.zip", helper, root / "escape.zip", "Linux")

    def test_resource_inventory_is_exact_and_requires_runtime_bases(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_resource_tree(root)
            native = _verify_resources(root)
            self.assertEqual(native.name, "sound_transcode.exe" if os.name == "nt"
                             else "sound_transcode")
            (root / "unexpected.bin").write_bytes(b"extra")
            with self.assertRaisesRegex(ValueError, "inventory differs"):
                _verify_resources(root)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_resource_tree(root, omit={"beta-base/anim.png"})
            with self.assertRaisesRegex(ValueError, "Required bundled resource"):
                _verify_resources(root)


def _shell_asset_exclude_match(text):
    """Parse tools/make_release.sh's ASSET_EXCLUDE_MATCH into {path: cleared sha}."""
    body = re.search(r"^ASSET_EXCLUDE_MATCH\(\)\s*\{\n(.*?)^\}$", text, re.S | re.M)
    assert body, "ASSET_EXCLUDE_MATCH function not found in make_release.sh"
    arms = re.findall(r"^\t\t([^)\n]+)\)\n(.*?);;\s*$", body.group(1), re.S | re.M)
    assert arms, "no case arms parsed from ASSET_EXCLUDE_MATCH"
    parsed = {}
    for pattern, arm in arms:
        pattern = pattern.strip()
        if pattern == "*":  # the default "not excluded" arm
            continue
        digests = re.findall(r'!=\s*"([0-9a-f]{64})"', arm)
        assert len(digests) == 1, (pattern, digests)
        parsed[pattern] = digests[0]
    return parsed


def _imported_top_level(paths):
    names = set()
    for path in paths:
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
            if isinstance(node, ast.Import):
                names.update(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
                names.add(node.module.split(".")[0])
    return names


class PackagingContractTests(unittest.TestCase):
    """The 2026-09-17 acceptance-review findings that live in the build scripts."""

    def test_asset_exclusion_rule_cannot_drift_from_make_release(self):
        # M3: the same rule used to be written out twice.
        shell = _shell_asset_exclude_match(
            (REPO / "tools/make_release.sh").read_text(encoding="utf-8"))
        self.assertEqual(build_resources.ASSET_EXCLUDE_MATCH, shell)
        # ...and the Python side really applies it: a listed path is excluded
        # unless its content hashes to the one cleared digest; anything else is
        # shipped untouched.
        listed = next(iter(shell))
        self.assertTrue(build_resources.asset_is_excluded(listed, b"some other bytes"))
        self.assertFalse(build_resources.asset_is_excluded("assets/anything/else.png", b"x"))
        with patch.dict(build_resources.ASSET_EXCLUDE_MATCH,
                        {"assets/fake.png": hashlib.sha256(b"cleared").hexdigest()},
                        clear=False):
            self.assertFalse(build_resources.asset_is_excluded("assets/fake.png", b"cleared"))
            self.assertTrue(build_resources.asset_is_excluded("assets/fake.png", b"other"))

    def test_excluded_modules_are_never_actually_imported(self):
        # H2: numpy (27 of the candidate bundle's 70 MB) arrived through
        # PyInstaller's PIL hook, not through the helper's own code.
        sources = sorted((REPO / "tools/pack_helper").glob("*.py"))
        sources.append(REPO / "tools/import_owned_beta_assets.py")
        imported = _imported_top_level(sources)
        self.assertIn("numpy", package_app.EXCLUDED_MODULES)
        self.assertEqual(sorted(set(package_app.EXCLUDED_MODULES) & imported), [])
        # Guard the other direction: what the helper DOES import stays in.
        self.assertIn("PIL", imported)
        self.assertNotIn("PIL", package_app.EXCLUDED_MODULES)

    def test_interpreter_is_pinned_to_the_ci_one(self):
        workflow = (REPO / ".github/workflows/pack-helper.yml").read_text(encoding="utf-8")
        pinned = re.search(r"python-version:\s*'([0-9]+)\.([0-9]+)'", workflow)
        self.assertIsNotNone(pinned)
        self.assertEqual(package_app.REQUIRED_PYTHON,
                         (int(pinned.group(1)), int(pinned.group(2))))
        if sys.version_info[:2] != package_app.REQUIRED_PYTHON:
            with self.assertRaises(SystemExit):
                package_app._require_interpreter()
        else:
            package_app._require_interpreter()

    # A synthetic environment: the two pinned roots, their real transitive
    # members, some installer tooling and an unrelated package the developer
    # happens to have. {name: (version, [Requires-Dist entries])}
    SYNTHETIC = {
        "pillow": ("12.3.0", []),
        "pyinstaller": ("6.22.3", ["setuptools>=42.0.0", "altgraph", "packaging>=22.0",
                                   "pyinstaller-hooks-contrib>=2025.8"]),
        "altgraph": ("0.17.4", []),
        "packaging": ("25.0", []),
        "pyinstaller-hooks-contrib": ("2025.8", []),
        "setuptools": ("80.9.0", []),
        "pip": ("25.2", []),
        "wheel": ("0.45.1", []),
        "requests": ("2.32.3", []),  # unrelated, present in the developer venv
    }

    def _synthetic_repo(self, tmp):
        repo = Path(tmp)
        lock_dir = repo / "tools/pack_helper"
        lock_dir.mkdir(parents=True)
        (lock_dir / "requirements-build.txt").write_text(
            "# comment\nPillow==12.3.0\nPyInstaller==6.22.3\n", encoding="utf-8")
        return repo, lock_dir / package_app.LOCK_NAME

    def test_lock_covers_the_closure_not_the_whole_venv(self):
        # M4: snapshotting every installed dist meant only CI could ever build.
        with tempfile.TemporaryDirectory() as tmp:
            repo, _ = self._synthetic_repo(tmp)
            closure = package_app.resolved_build_inputs(
                repo / "tools/pack_helper/requirements-build.txt",
                metadata=dict(self.SYNTHETIC))
            self.assertEqual(sorted(closure), [
                "altgraph", "packaging", "pillow", "pyinstaller",
                "pyinstaller-hooks-contrib"])
            for tooling in ("pip", "setuptools", "wheel", "requests"):
                self.assertNotIn(tooling, closure)

    def test_build_refuses_without_a_matching_lock(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo, lock = self._synthetic_repo(tmp)
            requirements = repo / "tools/pack_helper/requirements-build.txt"
            closure = package_app.resolved_build_inputs(
                requirements, metadata=dict(self.SYNTHETIC))

            # No lock at all -> refuse, and say exactly what to commit.
            with self.assertRaises(SystemExit) as missing:
                package_app._require_lock(repo, closure)
            self.assertIn("not reproducible from pinned inputs", str(missing.exception))

            # The exact closure -> accepted.
            lock.write_text(package_app.lock_text(closure), encoding="utf-8")
            self.assertEqual(package_app._require_lock(repo, closure), closure)

            # Installer tooling drifting does NOT invalidate the lock...
            drifted_tooling = dict(self.SYNTHETIC)
            drifted_tooling["pip"] = ("99.9", [])
            drifted_tooling["setuptools"] = ("1.0", [])
            self.assertEqual(
                package_app._require_lock(repo, package_app.resolved_build_inputs(
                    requirements, metadata=drifted_tooling)),
                closure)

            # ...but a closure MEMBER drifting does.
            drifted_member = dict(self.SYNTHETIC)
            drifted_member["altgraph"] = ("0.17.5", [])
            with self.assertRaises(SystemExit) as drift:
                package_app._require_lock(repo, package_app.resolved_build_inputs(
                    requirements, metadata=drifted_member))
            self.assertIn("altgraph", str(drift.exception))

            # A partial lock fails as missing; an empty one fails too.
            partial = {k: v for k, v in closure.items() if k != "packaging"}
            lock.write_text(package_app.lock_text(partial), encoding="utf-8")
            with self.assertRaises(SystemExit) as extra:
                package_app._require_lock(repo, closure)
            self.assertIn("packaging", str(extra.exception))
            lock.write_text("\n# nothing pinned\n", encoding="utf-8")
            with self.assertRaises(SystemExit) as empty:
                package_app._require_lock(repo, closure)
            self.assertIn("unexpected", str(empty.exception))

    def test_lock_name_is_per_platform(self):
        # The closure differs per OS, so one shared lock could never match all.
        self.assertTrue(package_app.LOCK_NAME.endswith(".lock"))
        self.assertIn(sys.platform, package_app.LOCK_NAME)

    def test_smoke_hashes_the_whole_tree_not_just_resources(self):
        # H2: the app's self-test only ever covered resources/.
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "dist"
            (bundle / "CaveWii Pack Helper/_internal/pack_helper/resources").mkdir(parents=True)
            runtime = bundle / "CaveWii Pack Helper/_internal/python312.dll"
            runtime.write_bytes(b"frozen runtime")
            resource = bundle / "CaveWii Pack Helper/_internal/pack_helper/resources/mapping.json"
            resource.write_bytes(b"{}")
            first = smoke_packaged.tree_digest(bundle)
            self.assertEqual(first["entries"], 2)
            self.assertIn("CaveWii Pack Helper/_internal/python312.dll", first["tree"])
            runtime.write_bytes(b"a DIFFERENT frozen runtime")
            second = smoke_packaged.tree_digest(bundle)
            self.assertNotEqual(first["sha256"], second["sha256"])

    def test_tree_digest_records_the_exec_bit(self):
        # LOW: a launcher that lost +x is broken in a way no content hash sees.
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "dist"
            bundle.mkdir()
            launcher = bundle / "CaveWii Pack Helper"
            launcher.write_bytes(b"#!/bin/sh\n")
            digest = smoke_packaged.tree_digest(bundle)
            entry = digest["tree"]["CaveWii Pack Helper"]
            self.assertEqual(digest["records_exec_bit"], os.name != "nt")
            if os.name == "nt":
                self.assertTrue(entry.startswith("file:x0:"))  # documented: always 0
            else:
                self.assertTrue(entry.startswith("file:x0:"))
                before = digest["sha256"]
                launcher.chmod(launcher.stat().st_mode | stat.S_IXUSR)
                after = smoke_packaged.tree_digest(bundle)
                self.assertTrue(after["tree"]["CaveWii Pack Helper"].startswith("file:x1:"))
                self.assertNotEqual(before, after["sha256"])

    def test_bundle_text_writers_pin_unix_newlines(self):
        # LOW: CRLF from a Windows writer would make the hashed tree differ
        # across OSes for byte-identical content. Scan by matching parentheses,
        # not by regex: a nested json.dumps(...) call slipped past the regex.
        for relative in ("tools/pack_helper/package_app.py",
                         "tools/pack_helper/build_resources.py"):
            text = (REPO / relative).read_text(encoding="utf-8")
            calls = 0
            for match in re.finditer(r"\.write_text\(", text):
                depth, index = 1, match.end()
                while index < len(text) and depth:
                    if text[index] == "(":
                        depth += 1
                    elif text[index] == ")":
                        depth -= 1
                    index += 1
                call = text[match.start():index]
                calls += 1
                self.assertIn('newline="\\n"', call,
                              relative + ": " + " ".join(call.split())[:110])
            self.assertGreater(calls, 0, relative)

    def test_freeze_runs_with_a_fixed_hash_seed_and_source_date(self):
        # PyInstaller's reproducible-build recipe. PYTHONHASHSEED only applies
        # at interpreter start-up, so it has to reach the CHILD that freezes.
        environment = package_app.freeze_environment({"PATH": "/existing"})
        self.assertEqual(environment["PYTHONHASHSEED"], package_app.FREEZE_PYTHONHASHSEED)
        self.assertEqual(environment["SOURCE_DATE_EPOCH"],
                         package_app.FREEZE_SOURCE_DATE_EPOCH)
        self.assertEqual(environment["PATH"], "/existing")  # inherits the rest
        # Pinned constants, never the clock or a git timestamp.
        self.assertTrue(package_app.FREEZE_SOURCE_DATE_EPOCH.isdigit())
        self.assertTrue(package_app.FREEZE_PYTHONHASHSEED.isdigit())
        # ...and both are recorded in what the bundle carries.
        payload = package_app.build_inputs_payload({"pillow": "12.3.0"})
        self.assertEqual(payload["pythonhashseed"], package_app.FREEZE_PYTHONHASHSEED)
        self.assertEqual(payload["source_date_epoch"],
                         package_app.FREEZE_SOURCE_DATE_EPOCH)

    def test_package_passes_that_environment_to_pyinstaller(self):
        recorded = {}

        def fake_build(repo, destination, cc="cc"):
            Path(destination).mkdir(parents=True)
            (Path(destination) / "resource_hashes.json").write_text("{}", encoding="utf-8")

        def fake_run(argv, **kwargs):
            recorded["argv"] = argv
            recorded["env"] = kwargs.get("env")
            return None

        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(package_app, "build", fake_build), \
                    patch.object(package_app, "_require_interpreter", lambda: None), \
                    patch.object(package_app, "_require_lock", lambda repo: {"pillow": "12.3.0"}), \
                    patch.object(package_app.subprocess, "run", fake_run):
                package_app.package(REPO, Path(tmp) / "out", "cc")
        self.assertIn("PyInstaller", recorded["argv"])
        if sys.platform == "win32":
            icon_index = recorded["argv"].index("--icon") + 1
            self.assertEqual(Path(recorded["argv"][icon_index]).parts[-2:], ("branding", "computer.ico"))
        self.assertEqual(recorded["env"]["PYTHONHASHSEED"],
                         package_app.FREEZE_PYTHONHASHSEED)
        self.assertEqual(recorded["env"]["SOURCE_DATE_EPOCH"],
                         package_app.FREEZE_SOURCE_DATE_EPOCH)

    def test_bundle_must_carry_license_and_notice(self):
        # L9, mirroring tools/make_release.sh:693-707.
        self.assertEqual(build_resources.REQUIRED_LEGAL_FILES,
                         ("LICENSE", "NOTICE", "LICENSES/GPL-2.0.txt"))
        for legal in build_resources.REQUIRED_LEGAL_FILES:
            self.assertIn(legal, selftest_module.REQUIRED_RESOURCES)
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                _make_resource_tree(root, omit={legal})
                with self.assertRaisesRegex(ValueError, "Required bundled resource"):
                    _verify_resources(root)

    def test_branding_is_committed_content_and_required_in_inventory(self):
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp)
            approved = {"HEAD:tools/pack_helper/art/" + name: ("committed:" + name).encode()
                        for name in build_resources.BRANDING_FILES}
            with patch.object(build_resources.subprocess, "check_output",
                              side_effect=lambda args: approved[args[-1]]) as read:
                build_resources.copy_committed_branding(REPO, destination)
            self.assertEqual(read.call_count, 3)
            for name in build_resources.BRANDING_FILES:
                self.assertEqual((destination / "branding" / name).read_bytes(),
                                 approved["HEAD:tools/pack_helper/art/" + name])
        for name in build_resources.BRANDING_FILES:
            relative = "branding/" + name
            self.assertIn(relative, selftest_module.REQUIRED_RESOURCES)
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                _make_resource_tree(root, omit={relative})
                with self.assertRaisesRegex(ValueError, "Required bundled resource"):
                    _verify_resources(root)

    def test_branding_lookup_distinguishes_source_and_frozen_runtime(self):
        self.assertEqual(set(appearance.THEMES), {"light", "dark"})
        self.assertEqual(appearance.THEMES["light"]["bg"], "#e9edf1")
        self.assertEqual(appearance.THEMES["dark"]["bg"], "#292929")
        with patch.object(appearance.sys, "frozen", False, create=True):
            self.assertEqual(appearance._branding_directory(), REPO / "tools/pack_helper/art")
        with patch.object(appearance.sys, "frozen", True, create=True):
            self.assertEqual(appearance._branding_directory(), REPO / "tools/pack_helper/resources/branding")


if __name__ == "__main__":
    unittest.main()
