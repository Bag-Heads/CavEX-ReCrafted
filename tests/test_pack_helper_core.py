import errno
import io
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from PIL import Image

from tools.pack_helper import core
from tools.pack_helper import owned_importer

# Never let a synthetic jar pick up this computer's own .minecraft through the
# launcher-location fallback; the tests that need it patch minecraft_roots.
os.environ[core.NO_LAUNCHER_SEARCH_ENV] = "1"


def png(size, color=(30, 80, 140, 255)):
    output = io.BytesIO()
    Image.new("RGBA", size, color).save(output, "PNG")
    return output.getvalue()


def legacy_zip(path, extra=None):
    files = {
        "terrain.png": png((256, 256)),
        "gui/trap.png": png((256, 256), (20, 30, 40, 255)),
        "pack.txt": b"Test pack\n",
    }
    files.update(extra or {})
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)


def importer_map(path):
    path.write_text(json.dumps({
        "schema_version": 1,
        "texture_paths": ["terrain.png", "gui/trap.png"],
        "sound_keys": ["step.grass"],
        "sound_max_variants": 8,
        "music_keys": ["calm"],
        "creative_music_keys": [],
        "records": ["13", "cat"],
        "required_textures": ["gui/trap.png"],
        "release_version": "test",
    }), encoding="utf-8")
    return path


class InspectTests(unittest.TestCase):
    def test_pc_destination_accepts_current_and_legacy_program_names(self):
        for name in ("CavEX-ReCrafted.exe", "CavEX-ReCrafted",
                     "cavewii_v2.exe", "cavewii_v2"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                (root / name).write_bytes(b"synthetic PC paths.audiopack\x00")
                self.assertEqual(core.validate_destination(root), root.resolve())

    def test_legacy_archive_is_structured(self):
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "classic.zip"
            legacy_zip(source)
            result = core.inspect(source)
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["format"], "legacy-java")
            self.assertEqual(result["totals"]["images"], 2)

    def test_rejects_traversal_absolute_backslash_and_case_collision(self):
        bad_names = ("../escape.png", "/absolute.png", "C:/drive.png")
        with tempfile.TemporaryDirectory() as td:
            for index, name in enumerate(bad_names):
                source = Path(td) / (str(index) + ".zip")
                legacy_zip(source, {name: png((1, 1))})
                result = core.inspect(source)
                self.assertFalse(result["ok"], (name, result))
            # zipfile normalizes backslashes while writing on Windows. Patch
            # both central/local names byte-for-byte to model an untrusted ZIP
            # authored elsewhere.
            slash = Path(td) / "slash.zip"
            legacy_zip(slash, {"a/b.png": png((1, 1))})
            slash.write_bytes(slash.read_bytes().replace(b"a/b.png", b"a\\b.png"))
            self.assertFalse(core.inspect(slash)["ok"])
            collision = Path(td) / "case.zip"
            legacy_zip(collision, {"GUI/TRAP.PNG": png((256, 256))})
            result = core.inspect(collision)
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "case_collision")
            prefix = Path(td) / "prefix.zip"
            legacy_zip(prefix, {"blocked": b"file", "blocked/nested.png": png((1, 1))})
            self.assertEqual(core.inspect(prefix)["errors"][0]["code"], "file_directory_collision")
            reserved = Path(td) / "reserved.zip"
            legacy_zip(reserved, {"CON.png": png((1, 1))})
            self.assertEqual(core.inspect(reserved)["errors"][0]["code"], "windows_ambiguous_path")

    def test_rejects_symlink_and_modern_pack(self):
        with tempfile.TemporaryDirectory() as td:
            link_zip = Path(td) / "link.zip"
            info = zipfile.ZipInfo("linked.png")
            info.create_system = 3
            info.external_attr = (0o120777 << 16)
            with zipfile.ZipFile(link_zip, "w") as archive:
                archive.writestr(info, b"terrain.png")
                archive.writestr("terrain.png", png((256, 256)))
            self.assertEqual(core.inspect(link_zip)["errors"][0]["code"], "symlink")

            modern = Path(td) / "modern.zip"
            legacy_zip(modern, {"pack.mcmeta": b"{}"})
            self.assertEqual(core.inspect(modern)["errors"][0]["code"], "modern_format")

    def test_rejects_corrupt_and_bounded_archive(self):
        with tempfile.TemporaryDirectory() as td:
            corrupt = Path(td) / "bad.zip"
            corrupt.write_bytes(b"not a zip")
            self.assertEqual(core.inspect(corrupt)["errors"][0]["code"], "invalid_archive")
            source = Path(td) / "large.zip"
            legacy_zip(source, {"huge.png": b"x" * 1000})
            result = core.inspect(source, limits={"file_bytes": 512})
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "file_too_large")


class PrepareTests(unittest.TestCase):
    def test_nether_scene_uses_imported_atlases_and_is_manifested(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            legacy_zip(source)
            result = core.prepare(source, root / "prepared")
            self.assertTrue(result["ok"], result)
            pack = Path(result["path"])
            expected = core.nether_scene.compose((pack / "terrain.png").read_bytes(),
                                                 (pack / "anim.png").read_bytes())
            for rel, data in zip(core.nether_scene.FILES, expected):
                self.assertEqual((pack / rel).read_bytes(), data)
                self.assertIn(rel, {row["path"] for row in result["manifest"]["files"]})
            with Image.open(pack / core.nether_scene.FILES[0]) as image:
                self.assertEqual(image.size, (1024, 576))

    def test_nether_composer_failure_skips_only_scene(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            legacy_zip(source)
            good = core.prepare(source, root / "good")
            with mock.patch.object(core.nether_scene, "compose", side_effect=RuntimeError("injected failure")):
                failed = core.prepare(source, root / "failed")
            self.assertTrue(good["ok"], good)
            self.assertTrue(failed["ok"], failed)
            self.assertTrue(any("Nether loading scene skipped: injected failure" in w
                                for w in failed["warnings"]))
            excluded = set(core.nether_scene.FILES) | {core.MANIFEST_NAME}
            for path in Path(good["path"]).rglob("*"):
                rel = path.relative_to(good["path"]).as_posix()
                if path.is_file() and rel not in excluded:
                    self.assertEqual(path.read_bytes(), (Path(failed["path"]) / rel).read_bytes())
            self.assertTrue(all(not (Path(failed["path"]) / p).exists()
                                for p in core.nether_scene.FILES))

    def test_nether_scene_partial_write_is_removed(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            legacy_zip(source)
            original = core._write_file

            def fail_note(staging, rel, data):
                if rel == core.nether_scene.FILES[1]:
                    original(staging, rel, b"partial")
                    raise OSError("injected scene write failure")
                return original(staging, rel, data)

            with mock.patch.object(core, "_write_file", side_effect=fail_note):
                result = core.prepare(source, root / "prepared")
            self.assertTrue(result["ok"], result)
            self.assertTrue(any("injected scene write failure" in w for w in result["warnings"]))
            self.assertTrue(all(not (Path(result["path"]) / p).exists()
                                for p in core.nether_scene.FILES))

    def test_cancel_before_nether_scene_still_cancels_import(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            legacy_zip(source)

            def cancel(event):
                if event.get("path") == "Nether loading scene":
                    raise InterruptedError("cancelled by user")

            result = core.prepare(source, root / "prepared", progress=cancel)
            self.assertFalse(result["ok"], result)
            self.assertEqual(list((root / "prepared").iterdir()), [])

    def test_nether_map_hash_is_pinned_and_corruption_is_rejected(self):
        import hashlib
        scene = core.nether_scene
        # Deliberate generated-data revision: update only with paired-render proof.
        self.assertEqual(hashlib.sha256(scene.map_path().read_bytes()).hexdigest(),
                         "fc66c89946b9ffe38a330793d8688670dddea556b3a92ae36a183b603a2fbc11")
        scene._read_map.cache_clear()
        try:
            with tempfile.TemporaryDirectory() as td:
                corrupt = Path(td) / "map.bin"
                corrupt.write_bytes(b"not the approved synthetic map")
                with mock.patch.object(scene, "map_path", return_value=corrupt):
                    with self.assertRaisesRegex(ValueError, "checksum"):
                        scene._read_map()
        finally:
            scene._read_map.cache_clear()

    def test_rejected_nested_destination_leaves_source_untouched(self):
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "owned"
            source.mkdir()
            (source / "terrain.png").write_bytes(png((256, 256)))
            result = core.prepare(source, source / "nested-output")
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "destination_inside_source")
            self.assertEqual([p.name for p in source.iterdir()], ["terrain.png"])

    def test_prepare_is_separate_and_preserves_original(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            legacy_zip(source)
            before = source.read_bytes()
            result = core.prepare(source, root / "prepared")
            self.assertTrue(result["ok"], result)
            prepared = Path(result["path"])
            self.assertTrue((prepared / "terrain.png").is_file())
            self.assertTrue((prepared / "gui/trap.png").is_file())
            self.assertEqual(source.read_bytes(), before)
            self.assertNotEqual(prepared.parent, source.parent)
            statuses = {row["path"]: row["status"] for row in result["files"]}
            self.assertEqual(statuses["terrain.png"], "converted")
            self.assertEqual(statuses["particles.png"], "fallback")

    def test_interruption_removes_private_staging(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            destination = root / "prepared"
            legacy_zip(source)

            def stop(_event):
                raise KeyboardInterrupt("cancelled")

            # KeyboardInterrupt is a deliberate cancellation, and callers own
            # its UI handling; cleanup must still occur.
            with self.assertRaises(KeyboardInterrupt):
                core.prepare(source, destination, progress=stop)
            self.assertEqual(list(destination.iterdir()), [])

    def test_full_disk_failure_is_structured_and_cleans_staging(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            destination = root / "prepared"
            legacy_zip(source)
            with mock.patch.object(core, "_write_file", side_effect=OSError(errno.ENOSPC, "full")):
                result = core.prepare(source, destination)
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "io_error")
            self.assertEqual(list(destination.iterdir()), [])

    def test_unconvertible_supplied_image_stops_the_import_by_name(self):
        """A pack file that cannot be converted is never swapped for CavEX art
        behind an "OK" (2026-10-30 audit F5/F8): the import stops, names the
        file and the reason, and publishes nothing."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "owned.zip"
            # 48x192 is neither the 64x256 canonical nor a whole multiple.
            legacy_zip(source, {"environment/rain.png": png((48, 192)),
                                "music/calm1.ogg": b"owned music"})
            mapping = importer_map(root / "mapping.json")
            result = core.prepare(source, root / "prepared", import_mapping_path=mapping)
            self.assertFalse(result["ok"], result)
            error = result["errors"][0]
            self.assertEqual(error["code"], "unsupported_dimensions")
            self.assertEqual(error["path"], "environment/rain.png")
            self.assertIn("environment/rain.png cannot be used", error["message"])
            self.assertIn("Nothing was installed", error["message"])
            self.assertEqual([p for p in (root / "prepared").iterdir()], [])

    def test_mapicons_copies_from_a_beta_jar_and_falls_back_when_absent(self):
        """A Beta-jar layout ships misc/mapicons.png at its root: it is imported
        unchanged (32x32 is canonical). A pack without it takes the bundled CC0
        sheet: a fallback row, nothing written, and the prepare still succeeds."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            icons = png((32, 32), (200, 190, 180, 255))
            jar = root / "b1.7.3.jar"
            legacy_zip(jar, {"misc/mapicons.png": icons})
            result = core.prepare(jar, root / "with",
                                  import_mapping_path=importer_map(root / "m1.json"))
            self.assertTrue(result["ok"], result)
            prepared = Path(result["path"])
            self.assertEqual((prepared / "misc/mapicons.png").read_bytes(), icons)
            rows = [row for row in result["files"] if row["path"] == "misc/mapicons.png"]
            self.assertEqual([row["status"] for row in rows], ["converted"])
            self.assertIn("misc/mapicons.png",
                          [row["path"] for row in result["manifest"]["files"]])

            bare = root / "bare.zip"
            legacy_zip(bare)
            result = core.prepare(bare, root / "without",
                                  import_mapping_path=importer_map(root / "m2.json"))
            self.assertTrue(result["ok"], result)
            prepared = Path(result["path"])
            self.assertFalse((prepared / "misc/mapicons.png").exists())
            rows = [row for row in result["files"] if row["path"] == "misc/mapicons.png"]
            self.assertEqual(len(rows), 1, result["files"])
            self.assertEqual(rows[0]["status"], "fallback")
            self.assertEqual(rows[0]["fallback"], "bundled")
            self.assertNotIn("misc/mapicons.png",
                             [row["path"] for row in result["manifest"]["files"]])
            # The fallback really exists: the bundled CC0 sheet, canonical size.
            bundled = Path(core.__file__).resolve().parents[2] / "assets/misc/mapicons.png"
            with Image.open(bundled) as image:
                self.assertEqual(image.size, (32, 32))

    def test_required_entry_and_required_texture_still_abort(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            conversion_map = json.loads(
                (Path(core.__file__).with_name("conversion_map.json"))
                .read_text(encoding="utf-8"))
            required_map = root / "required.json"
            conversion_map["assets"]["terrain.png"]["required"] = True
            required_map.write_text(json.dumps(conversion_map), encoding="utf-8")
            no_fallback_map = root / "no-fallback.json"
            conversion_map["assets"]["terrain.png"]["required"] = False
            del conversion_map["assets"]["terrain.png"]["fallback"]
            no_fallback_map.write_text(json.dumps(conversion_map), encoding="utf-8")

            source = root / "owned.zip"
            legacy_zip(source, {"terrain.png": png((100, 100))})
            for index, mapping in enumerate((required_map, no_fallback_map)):
                result = core.prepare(source, root / ("out-" + str(index)),
                                      mapping_path=mapping)
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["errors"][0]["code"], "unsupported_dimensions")
                self.assertEqual(result["errors"][0]["path"], "terrain.png")

            # A texture the packaged importer mapping lists as required must be
            # in the pack: gui/trap.png is optional in the conversion map, but
            # importer_map declares it required, so a pack without it aborts.
            importer_required = root / "trap.zip"
            with zipfile.ZipFile(importer_required, "w") as archive:
                archive.writestr("terrain.png", png((256, 256)))
            result = core.prepare(importer_required, root / "out-trap",
                                  import_mapping_path=importer_map(root / "mapping.json"))
            self.assertFalse(result["ok"], result)
            self.assertEqual(result["errors"][0]["code"], "missing_required")
            self.assertEqual(result["errors"][0]["path"], "gui/trap.png")

    def test_embedded_zip_and_folder_music_use_owned_importer(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            mapping = importer_map(root / "mapping.json")
            archive = root / "audio.zip"
            legacy_zip(archive, {"music/calm1.ogg": b"owned music",
                                 "streaming/Cat.ogg": b"owned record"})
            zip_result = core.prepare(archive, root / "zip-out", import_mapping_path=mapping)
            self.assertTrue(zip_result["ok"], zip_result)
            self.assertEqual((Path(zip_result["path"]) / "music/calm1.ogg").read_bytes(), b"owned music")
            self.assertEqual((Path(zip_result["path"]) / "records/cat.ogg").read_bytes(), b"owned record")

            folder = root / "folder-pack"
            (folder / "gui").mkdir(parents=True)
            (folder / "music").mkdir()
            (folder / "terrain.png").write_bytes(png((256, 256)))
            (folder / "gui/trap.png").write_bytes(png((256, 256)))
            (folder / "music/folder.ogg").write_bytes(b"folder music")
            folder_result = core.prepare(folder, root / "folder-out", import_mapping_path=mapping)
            self.assertTrue(folder_result["ok"], folder_result)
            self.assertEqual((Path(folder_result["path"]) / "music/folder.ogg").read_bytes(), b"folder music")


class InstallTests(unittest.TestCase):
    def test_audio_only_folder_and_wrapped_zip_preserve_texture_selection(self):
        for wrapped in (False, True):
            for pc in (False, True):
                with self.subTest(wrapped=wrapped, pc=pc), tempfile.TemporaryDirectory() as td:
                    root = Path(td)
                    mapping = importer_map(root / "mapping.json")
                    if wrapped:
                        source = root / "sounds.zip"
                        with zipfile.ZipFile(source, "w") as archive:
                            archive.writestr("MySounds/music/calm1.ogg", b"owned music")
                            archive.writestr("MySounds/pack.png", png((16, 16)))
                    else:
                        source = root / "resources"
                        (source / "music").mkdir(parents=True)
                        (source / "music/calm1.ogg").write_bytes(b"owned music")
                    prepared = core.prepare(source, root / "prepared", import_mapping_path=mapping)
                    self.assertTrue(prepared["ok"], prepared)
                    self.assertEqual([row["path"] for row in prepared["manifest"]["files"]], ["music/calm1.ogg"])
                    if pc:
                        destination = root / "pc"
                        destination.mkdir()
                        (destination / "CavEX-ReCrafted.exe").write_bytes(b"synthetic PC paths.audiopack\x00")
                    else:
                        destination = self.sd(root)
                    config = {"paths": {"texturepack": "texturepacks/my-look"}, "video": {"fullscreen": True}}
                    (destination / "config.json").write_text(json.dumps(config))
                    result = core.install(prepared, destination, audio_select=True)
                    self.assertTrue(result["ok"], result)
                    updated = json.loads((destination / "config.json").read_text())
                    self.assertEqual(updated["paths"]["texturepack"], "texturepacks/my-look")
                    self.assertEqual(updated["video"], config["video"])
                    self.assertEqual(updated["paths"]["audiopack"], "texturepacks/" + result["pack_name"])
                    self.assertEqual((Path(result["path"]) / "music/calm1.ogg").read_bytes(), b"owned music")

    def test_combined_import_activates_both_and_no_select_preserves_config(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "both.zip"
            legacy_zip(source, {"music/calm1.ogg": b"owned music"})
            prepared = core.prepare(source, root / "prepared", import_mapping_path=importer_map(root / "mapping.json"))
            self.assertTrue(prepared["ok"], prepared)
            sd = self.sd(root)
            result = core.install(prepared, sd, audio_select=True)
            self.assertTrue(result["ok"], result)
            raw = (sd / "config.json").read_bytes()
            paths = json.loads(raw)["paths"]
            self.assertEqual(paths["audiopack"], paths["texturepack"])
            second = core.install(prepared, sd, select=False, audio_select=True)
            self.assertTrue(second["ok"], second)
            self.assertFalse(second["selected"])
            self.assertEqual((sd / "config.json").read_bytes(), raw)

    def test_unsupported_audio_does_not_publish_empty_pack(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "unknown"
            source.mkdir()
            (source / "random.wav").write_bytes(b"unknown layout")
            result = core.prepare(source, root / "prepared", import_mapping_path=importer_map(root / "mapping.json"))
            self.assertFalse(result["ok"], result)
            self.assertEqual(result["errors"][0]["code"], "no_supported_assets")
            self.assertEqual(list((root / "prepared").iterdir()), [])

    def test_preflight_rejects_non_game_folder_without_writes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            with self.assertRaises(core.PackError) as error:
                core.validate_destination(root)
            self.assertEqual(error.exception.code, "game_not_installed")
            self.assertEqual(list(root.iterdir()), [])

    def prepare(self, root):
        source = root / "owned.zip"
        legacy_zip(source)
        result = core.prepare(source, root / "prepared")
        self.assertTrue(result["ok"], result)
        return result

    def sd(self, root):
        sd = root / "sd"
        boot = sd / "apps/CavEX/boot.dol"
        boot.parent.mkdir(parents=True)
        boot.write_bytes(b"synthetic dol paths.audiopack\x00")
        return sd

    def test_older_game_cannot_silently_change_audio(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            (sd / "apps/CavEX/boot.dol").write_bytes(b"old game")
            result = core.install(prepared, sd)
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "game_update_required")
            self.assertFalse((sd / "config.json").exists())
            self.assertTrue(core.install(prepared, sd, select=False)["ok"])

    def test_no_boot_returns_instructions_and_changes_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = root / "empty-sd"
            sd.mkdir()
            result = core.install(prepared, sd)
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "game_not_installed")
            self.assertTrue(result["instructions"])
            self.assertFalse((sd / "texturepacks").exists())

    def test_install_unique_hash_verified_and_config_preserved(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            original = {"paths": {"texturepack": "texturepacks/old"},
                        "unrelated": {"keep": [1, 2, 3]}}
            (sd / "config.json").write_text(json.dumps(original), encoding="utf-8")
            first = core.install(prepared, sd)
            second = core.install(prepared, sd, audio_select=True)
            self.assertTrue(first["ok"], first)
            self.assertTrue(second["ok"], second)
            # Same pack imported twice: ONE folder, replaced (2026-10-30 audit).
            self.assertEqual(first["path"], second["path"])
            self.assertTrue(second.get("replaced"))
            self.assertEqual(sorted(p.name for p in (sd / "texturepacks").iterdir()),
                             [second["pack_name"]])
            self.assertEqual(list(sd.glob(".cavewii-*")), [])
            config = json.loads((sd / "config.json").read_text(encoding="utf-8"))
            self.assertEqual(config["unrelated"], original["unrelated"])
            self.assertEqual(config["paths"]["texturepack"], "texturepacks/" + second["pack_name"])
            self.assertEqual(config["paths"]["audiopack"], "texturepacks/old")
            self.assertTrue(any("no imported audio" in warning for warning in second["warnings"]))
            backup = json.loads((sd / "config.json.pack-helper.bak").read_text(encoding="utf-8"))
            self.assertEqual(backup["paths"]["audiopack"], "texturepacks/old")

    def test_never_overwrites_a_backup_it_did_not_write(self):
        # M5 (2026-09-17 review): config.json.bak is a generic name anything
        # may own. The helper writes only its own, and leaves that one alone.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            (sd / "config.json").write_text(
                json.dumps({"paths": {"texturepack": "texturepacks/old"}}), encoding="utf-8")
            foreign = b'{"someone": "elses backup"}'
            (sd / "config.json.bak").write_bytes(foreign)
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            self.assertEqual((sd / "config.json.bak").read_bytes(), foreign)
            helper_backup = sd / "config.json.pack-helper.bak"
            self.assertEqual(result["config"]["backup"], str(helper_backup))
            self.assertEqual(
                json.loads(helper_backup.read_text(encoding="utf-8"))["paths"]["texturepack"],
                "texturepacks/old")
            # A second install replaces only the helper's own copy.
            core.install(prepared, sd)
            self.assertEqual((sd / "config.json.bak").read_bytes(), foreign)

    def test_prefix_collision_checks_every_pair_not_just_neighbours(self):
        # L7: "a", "a-x", "a/b" sorts with "a-x" between the colliding pair,
        # so an adjacent-only scan missed it.
        rows = [{"path": "a"}, {"path": "a-x"}, {"path": "a/b"}]
        with self.assertRaises(core.PackError) as caught:
            core._check_prefix_collisions(rows)
        self.assertEqual(caught.exception.code, "file_directory_collision")
        # Deep, non-adjacent, and case-folded forms collide too.
        for rows in ([{"path": "x/y"}, {"path": "x-1"}, {"path": "x/y/z"}],
                     [{"path": "A"}, {"path": "a/b"}]):
            with self.assertRaises(core.PackError):
                core._check_prefix_collisions(rows)
        # A shared prefix that is not a whole path component is not a collision.
        core._check_prefix_collisions([{"path": "a"}, {"path": "ab"}])
        core._check_prefix_collisions([{"path": "sound/step/a.wav"},
                                       {"path": "sound/step/ab.wav"}])

    def test_copy_failure_never_exposes_incomplete_pack(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            with mock.patch.object(core.shutil, "copyfile", side_effect=OSError(errno.ENOSPC, "full")):
                result = core.install(prepared, sd)
            self.assertFalse(result["ok"])
            packs = sd / "texturepacks"
            self.assertEqual(list(packs.iterdir()), [])
            self.assertFalse(any(p.name.startswith(".cavewii-install-") for p in sd.iterdir()))

    def test_changed_config_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            config_path = sd / "config.json"
            config_path.write_text('{"paths":{"texturepack":"assets"}}', encoding="utf-8")
            real_load = core._load_config

            def racing_load(path):
                value = real_load(path)
                path.write_text('{"paths":{"texturepack":"other"}}', encoding="utf-8")
                return value

            with mock.patch.object(core, "_load_config", side_effect=racing_load):
                result = core.install(prepared, sd)
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "config_changed")
            self.assertEqual(json.loads(config_path.read_text())["paths"]["texturepack"], "other")
            self.assertTrue(Path(result["path"]).is_dir())

    def test_invalid_config_recovers_from_backup(self):
        # EXPECTATION CHANGED on purpose (2026-09-17 DELTA review, MEDIUM, and
        # authorised by brief02 item 3): the old expectation locked in the
        # defect -- the corrupt original was destroyed with no copy, and the
        # recovery was silent. It must now be kept, and surfaced.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            config_path = sd / "config.json"
            config_path.write_text("{broken", encoding="utf-8")
            backup = {"paths": {"texturepack": "texturepacks/safe"}, "keep": 9}
            (sd / "config.json.bak").write_text(json.dumps(backup), encoding="utf-8")
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            self.assertTrue(result["config"]["recovered_from_backup"])
            updated = json.loads(config_path.read_text(encoding="utf-8"))
            self.assertEqual(updated["keep"], 9)
            self.assertEqual(updated["paths"]["audiopack"], "texturepacks/safe")
            self.assertEqual(json.loads((sd / "config.json.bak").read_text()), backup)
            # The unreadable original survives, byte-for-byte...
            corrupt = sd / "config.json.pack-helper.corrupt"
            self.assertEqual(corrupt.read_bytes(), b"{broken")
            self.assertEqual(result["config"]["corrupt_copy"], str(corrupt))
            # ...and the player is told, rather than losing settings silently.
            self.assertTrue(any("rebuilt from an earlier backup" in w
                                for w in result["warnings"]), result["warnings"])
            self.assertTrue(any(str(corrupt) in w for w in result["warnings"]))

    def test_foreign_backup_is_not_adopted_as_a_config(self):
        # A .bak that is merely "a JSON object" is not a CaveWii config; adopting
        # one would replace the player's settings with something unrelated.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            config_path = sd / "config.json"
            config_path.write_text("{broken", encoding="utf-8")
            (sd / "config.json.bak").write_text(
                json.dumps({"totally": "unrelated"}), encoding="utf-8")
            result = core.install(prepared, sd)
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "config_invalid")
            # The pack still installed, and the corrupt original is preserved.
            self.assertTrue(Path(result["path"]).is_dir())
            self.assertEqual((sd / "config.json.pack-helper.corrupt").read_bytes(),
                             b"{broken")

    def test_helper_backup_is_preferred_over_the_generic_one(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self.prepare(root)
            sd = self.sd(root)
            (sd / "config.json").write_text("{broken", encoding="utf-8")
            (sd / "config.json.bak").write_text(
                json.dumps({"paths": {"texturepack": "texturepacks/stale"}}),
                encoding="utf-8")
            (sd / "config.json.pack-helper.bak").write_text(
                json.dumps({"paths": {"texturepack": "texturepacks/mine"}}),
                encoding="utf-8")
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["config"]["audiopack"], "texturepacks/mine")


def client_jar(path, drop=(), replace=None):
    """A synthetic Beta-shaped CLIENT jar: one obfuscated .class plus every
    texture a real b1.7.3.jar ships, each a generated solid PNG at its
    canonical size. No Mojang bytes."""
    conversion_map = json.loads(Path(core.__file__).with_name("conversion_map.json")
                                .read_text(encoding="utf-8"))
    replace = replace or {}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("a.class", b"\xca\xfe\xba\xbe")
        archive.writestr("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\n")
        if "title/splashes.txt" not in drop:
            archive.writestr("title/splashes.txt", b"Synthetic splash!\r\nAnother \xff splash\n")
        for index, dest in enumerate(core.client_jar_textures(conversion_map)):
            source = conversion_map["assets"][dest]["source_names"][0]
            if dest in drop or source in drop:
                continue
            size = tuple(conversion_map["assets"][dest]["canonical_size"])
            data = replace.get(source) or png(size, (index * 3 % 256, 90, 160, 255))
            archive.writestr(source, data)
        archive.writestr("title/mclogo.png", png((256, 64)))  # never read by the game


class JarImportAuditTests(unittest.TestCase):
    """2026-10-30 jar-import audit: F1/F9 config location, F2/F5/F8
    validation, F3 colour maps + HUD composite, clear messages."""

    def test_install_preserves_optional_jar_splashes(self):
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                jar = root / "b1.7.3.jar"
                client_jar(jar, drop=("title/splashes.txt",) if missing else ())
                result = core.prepare(jar, root / "prepared")
                self.assertTrue(result["ok"], result)
                installed = core.install(result["path"], self.sd(root), select=False)
                self.assertTrue(installed["ok"], installed)
                splash = Path(installed["path"]) / "title/splashes.txt"
                if missing:
                    self.assertFalse(splash.exists())
                else:
                    with zipfile.ZipFile(jar) as archive:
                        self.assertEqual(splash.read_bytes(), archive.read("title/splashes.txt"))

    def test_owned_importer_preserves_optional_jar_splashes(self):
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                jar = root / "b1.7.3.jar"
                client_jar(jar, drop=("title/splashes.txt",) if missing else ())
                with zipfile.ZipFile(jar) as archive, mock.patch.object(
                        owned_importer, "FROZEN_MAPPING", {"texture_paths": []}):
                    owned_importer.process_textures(archive, root / "pack", False)
                    splash = root / "pack/title/splashes.txt"
                    if missing:
                        self.assertFalse(splash.exists())
                    else:
                        self.assertEqual(splash.read_bytes(), archive.read("title/splashes.txt"))

    def sd(self, root, configs=True):
        sd = root / "sd"
        app = sd / "apps/CavEX"
        app.mkdir(parents=True)
        (app / "boot.dol").write_bytes(b"synthetic dol paths.audiopack\x00")
        default = {"paths": {"texturepack": "assets", "worlds": "saves"},
                   "input": {"wiimote": {"jump": 3}}, "video": {"vsync": True}}
        for folder in (sd, app):
            (folder / "config_wii.json").write_text(json.dumps(default), encoding="utf-8")
            if configs:
                mine = dict(default, mine={"where": folder.name})
                (folder / "config.json").write_text(json.dumps(mine), encoding="utf-8")
        return sd

    def test_extracted_incomplete_client_jar_is_refused_by_name(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            jar = root / "missing-zombie.jar"
            client_jar(jar, drop=("mob/zombie.png",))
            extracted = root / "extracted"
            with zipfile.ZipFile(jar) as archive:
                archive.extractall(extracted)
            result = core.prepare(extracted, root / "prepared")
            self.assertFalse(result["ok"], result)
            self.assertEqual(result["errors"][0]["code"], "incomplete_jar")
            self.assertEqual(result["errors"][0]["path"], "mob/zombie.png")

    def test_full_client_jar_keeps_colour_maps_hud_and_anim(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            jar = root / "b1.7.3.jar"
            client_jar(jar)
            result = core.prepare(jar, root / "prepared")
            self.assertTrue(result["ok"], result)
            prepared = Path(result["path"])
            for name in ("misc/grasscolor.png", "misc/foliagecolor.png", "gui_2.png",
                         "anim.png", "pointer.png", "terrain.png", "gui/chest.png"):
                self.assertTrue((prepared / name).is_file(), name)
            statuses = {row["path"]: row["status"] for row in result["files"]}
            self.assertEqual(statuses["gui_2.png"], "composited")
            self.assertEqual(statuses["title/mclogo.png"], "unused")
            self.assertFalse(any("unsupported" in w for w in result["warnings"]), result["warnings"])

    def test_same_jar_gives_identical_bytes_every_time(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            jar = root / "b1.7.3.jar"
            client_jar(jar)
            first = core.prepare(jar, root / "one")
            second = core.prepare(jar, root / "two")
            self.assertTrue(first["ok"] and second["ok"])
            self.assertEqual(first["hashes"], second["hashes"])

    def test_incomplete_client_jar_is_refused_by_name(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            jar = root / "b1.7.3.jar"
            client_jar(jar, drop=("mob/zombie.png",))
            result = core.prepare(jar, root / "prepared")
            self.assertFalse(result["ok"], result)
            self.assertEqual(result["errors"][0]["code"], "incomplete_jar")
            self.assertIn("mob/zombie.png", result["errors"][0]["message"])
            self.assertEqual(list((root / "prepared").iterdir()), [])

    def test_corrupt_odd_and_256x_textures_fail_clearly(self):
        cases = {
            "corrupt": (b"\x89PNG\r\n\x1a\nGARBAGE" * 10, "invalid_png", "damaged"),
            "odd": (png((300, 300)), "unsupported_dimensions", "300x300"),
            "x16": (png((4096, 4096)), "unsupported_dimensions", "256x"),
        }
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for name, (data, code, words) in cases.items():
                with self.subTest(name=name):
                    jar = root / (name + ".jar")
                    client_jar(jar, replace={"terrain.png": data})
                    result = core.prepare(jar, root / ("out-" + name))
                    self.assertFalse(result["ok"], result)
                    self.assertEqual(result["errors"][0]["code"], code)
                    self.assertEqual(result["errors"][0]["path"], "terrain.png")
                    self.assertIn(words, result["errors"][0]["message"])

    def test_hd_pack_at_4x_still_imports(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            conversion_map = json.loads(Path(core.__file__).with_name("conversion_map.json")
                                        .read_text(encoding="utf-8"))
            source = root / "hd.zip"
            with zipfile.ZipFile(source, "w") as archive:
                for dest in core.client_jar_textures(conversion_map):
                    w, h = conversion_map["assets"][dest]["canonical_size"]
                    archive.writestr(conversion_map["assets"][dest]["source_names"][0],
                                     png((w * 4, h * 4)))
            result = core.prepare(source, root / "prepared")
            self.assertTrue(result["ok"], result)
            with Image.open(Path(result["path"]) / "misc/grasscolor.png") as image:
                self.assertEqual(image.size, (256, 256))

    def test_modern_jar_gets_the_plain_message_before_any_limit(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            jar = root / "1.21.jar"
            with zipfile.ZipFile(jar, "w") as archive:
                archive.writestr("assets/minecraft/textures/block/stone.png", png((16, 16)))
                for index in range(40):
                    archive.writestr("x/{0}.class".format(index), b"x")
            result = core.prepare(jar, root / "prepared", limits={"files": 10})
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "modern_format")
            self.assertIn("not Minecraft Beta 1.7.3", result["errors"][0]["message"])
            self.assertIn("b1.7.3.jar", result["errors"][0]["message"])

    def _prepared(self, root):
        jar = root / "b1.7.3.jar"
        client_jar(jar)
        result = core.prepare(jar, root / "prepared")
        self.assertTrue(result["ok"], result)
        return result

    def test_install_selects_in_the_hbc_app_config_and_the_root_config(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self._prepared(root)
            sd = self.sd(root)
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            pack = "texturepacks/" + result["pack_name"]
            for folder in (sd / "apps/CavEX", sd):
                config = json.loads((folder / "config.json").read_text(encoding="utf-8"))
                self.assertEqual(config["paths"]["texturepack"], pack, folder)
                # Merged, never replaced: the player's own keys survive.
                self.assertEqual(config["mine"], {"where": folder.name})
                self.assertEqual(config["input"], {"wiimote": {"jump": 3}})
            self.assertEqual(len(result["configs"]), 2)

    def test_missing_config_starts_from_the_shipped_default(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self._prepared(root)
            sd = self.sd(root, configs=False)
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            for folder in (sd / "apps/CavEX", sd):
                config = json.loads((folder / "config.json").read_text(encoding="utf-8"))
                self.assertEqual(config["paths"]["texturepack"], "texturepacks/" + result["pack_name"])
                self.assertEqual(config["paths"]["worlds"], "saves")
                self.assertEqual(config["input"], {"wiimote": {"jump": 3}})

    def test_missing_config_without_any_default_uses_the_bundled_one(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self._prepared(root)
            sd = self.sd(root, configs=False)
            for leftover in sd.rglob("config_wii.json"):
                leftover.unlink()
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            config = json.loads((sd / "apps/CavEX/config.json").read_text(encoding="utf-8"))
            shipped = json.loads((Path(core.__file__).resolve().parents[2] / "config_wii.json")
                                 .read_text(encoding="utf-8-sig"))
            self.assertGreater(len(config), 1)
            self.assertEqual(set(config), set(shipped) | {"paths"})

    def test_read_only_config_changes_nothing_and_says_so_plainly(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self._prepared(root)
            sd = self.sd(root)
            locked = sd / "config.json"
            app_before = (sd / "apps/CavEX/config.json").read_bytes()
            os.chmod(locked, 0o444)
            try:
                result = core.install(prepared, sd)
            finally:
                os.chmod(locked, 0o666)
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "config_read_only")
            self.assertIn("untick Read-only", result["errors"][0]["message"])
            self.assertNotIn("WinError", result["errors"][0]["message"])
            # Planned first: the writable app config was not changed either.
            self.assertEqual((sd / "apps/CavEX/config.json").read_bytes(), app_before)

    def test_player_folders_with_other_names_are_never_touched(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prepared = self._prepared(root)
            sd = self.sd(root)
            mine = sd / "texturepacks" / "My Beta look"
            mine.mkdir(parents=True)
            (mine / "terrain.png").write_bytes(b"player's own")
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            self.assertEqual((mine / "terrain.png").read_bytes(), b"player's own")


def pcm_wav(path, freq):
    """A tiny valid PCM16 mono WAV (generated, never Mojang audio)."""
    import math
    import struct
    import wave
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(22050)
        w.writeframes(struct.pack("<200h", *[int(3000 * math.sin(i * freq / 3000.0)) for i in range(200)]))


def beta_resources(root):
    """A Beta-shaped resources folder: one sound key, one music track, one record."""
    pcm_wav(root / "sound/step/grass1.wav", 440)
    (root / "music").mkdir(parents=True, exist_ok=True)
    (root / "music/calm1.ogg").write_bytes(b"placeholder-ogg-calm1")
    (root / "streaming").mkdir(parents=True, exist_ok=True)
    (root / "streaming/13.ogg").write_bytes(b"placeholder-ogg-13")
    return root


def _transcoder():
    repo = Path(core.__file__).resolve().parents[2]
    for candidate in (repo / "tools/pack_helper/resources/sound_transcode.exe",
                      repo / "tools/pack_helper/resources/sound_transcode",
                      repo / "build-verify/sound_transcode.exe",
                      repo / "build-verify/sound_transcode"):
        if candidate.is_file():
            return candidate
    return None


class SoundImportAuditTests(unittest.TestCase):
    """2026-10-30 jar-import audit, sounds: find the resources folder for a
    picked .minecraft (or the launcher's asset store), say plainly when there
    are no sounds while textures still import, never pin audio on the
    command line (F11)."""

    def _jar(self, root):
        jar = root / "b1.7.3.jar"
        client_jar(jar)
        return jar

    def _music(self, result):
        return sorted(row["path"] for row in result["files"]
                      if row.get("category") == "audio" and not row["path"].startswith("sound/"))

    def test_audio_only_launcher_metadata_in_folder_and_zip(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            resources = beta_resources(root / "resources")
            (resources / "pack.mcmeta").write_text('{"pack":{"pack_format":1}}')
            for name in ("icons/icon_16x16.png", "pe/humble.png"):
                target = resources / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(png((16, 16)))
            archive = root / "resources.zip"
            with zipfile.ZipFile(archive, "w") as output:
                for path in resources.rglob("*"):
                    if path.is_file():
                        output.write(path, path.relative_to(resources).as_posix())
            for index, source in enumerate((resources, archive)):
                with self.subTest(source=source.name):
                    result = core.prepare(source, root / str(index), transcoder=_transcoder(),
                                          import_mapping_path=importer_map(root / "mapping.json"))
                    self.assertTrue(result["ok"], result)
                    self.assertEqual(self._music(result), ["music/calm1.ogg", "records/13.ogg"])
                    self.assertFalse(any(row["path"].endswith(".png")
                                         for row in result["manifest"]["files"]))
                    self.assertTrue(all(row["status"] == "unused" for row in result["files"]
                                        if row.get("category") == "texture"))

    def test_metadata_exception_needs_legacy_audio_without_modern_or_client_art(self):
        additions = ({}, {"arbitrary.ogg": b"audio"},
                     {"sound/step/grass1.wav": b"audio", "terrain.png": png((256, 256))},
                     {"sound/step/grass1.wav": b"audio", "x.class": b"code"},
                     {"sound/step/grass1.wav": b"audio", "models/block/stone.json": b"{}"},
                     {"sound/step/grass1.wav": b"audio", "textures/entity/pig.png": png((16, 16))},
                     {"sound/step/grass1.wav": b"audio", "assets/minecraft/sounds.json": b"{}"})
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for index, extra in enumerate(additions):
                folder = root / str(index)
                folder.mkdir()
                entries = {"pack.mcmeta": b"{}", **extra}
                archive = root / (str(index) + ".zip")
                with zipfile.ZipFile(archive, "w") as output:
                    for name, data in entries.items():
                        path = folder / name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(data)
                        output.writestr(name, data)
                for source in (folder, archive):
                    with self.subTest(index=index, source=source.name):
                        result = core.inspect(source)
                        self.assertFalse(result["ok"], result)
                        self.assertEqual(result["errors"][0]["code"], "modern_format")

    def test_picking_dot_minecraft_finds_its_resources_folder(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dot = root / ".minecraft"
            beta_resources(dot / "resources")
            (dot / "launcher_log.txt").write_bytes(b"x" * 1024)
            (dot / "versions/b1.7.3").mkdir(parents=True)
            result = core.prepare(self._jar(root), root / "prepared", resources_path=dot,
                                  import_mapping_path=importer_map(root / "mapping.json"))
            self.assertTrue(result["ok"], result)
            self.assertEqual(self._music(result), ["music/calm1.ogg", "records/13.ogg"])

    def test_sounds_found_in_the_launchers_asset_store(self):
        import hashlib as _hashlib
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dot = root / ".minecraft"
            objects = {}
            for rel, data in (("music/calm1.ogg", b"placeholder-ogg-calm1"),
                              ("streaming/13.ogg", b"placeholder-ogg-13"),
                              ("icons/icon_16x16.png", b"not audio")):
                digest = _hashlib.sha1(data).hexdigest()
                target = dot / "assets/objects" / digest[:2] / digest
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                objects[rel] = {"hash": digest, "size": len(data)}
            (dot / "assets/indexes").mkdir(parents=True)
            (dot / "assets/indexes/pre-1.6.json").write_text(
                json.dumps({"map_to_resources": True, "objects": objects}), encoding="utf-8")
            result = core.prepare(self._jar(root), root / "prepared", resources_path=dot,
                                  import_mapping_path=importer_map(root / "mapping.json"))
            self.assertTrue(result["ok"], result)
            self.assertEqual(self._music(result), ["music/calm1.ogg", "records/13.ogg"])
            self.assertTrue(any("asset store" in w for w in result["warnings"]), result["warnings"])

    def test_no_sounds_says_so_and_textures_still_import(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            empty = root / "My Sounds"
            empty.mkdir()
            result = core.prepare(self._jar(root), root / "prepared", resources_path=empty,
                                  import_mapping_path=importer_map(root / "mapping.json"))
            self.assertTrue(result["ok"], result)
            self.assertTrue((Path(result["path"]) / "terrain.png").is_file())
            self.assertTrue(any("No Beta 1.7.3 sounds or music were found" in w
                                and "textures were imported" in w for w in result["warnings"]),
                            result["warnings"])

    def test_audio_only_launcher_folder_without_sounds_is_a_plain_error(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dot = root / ".minecraft"
            (dot / "versions").mkdir(parents=True)
            (dot / "launcher_log.txt").write_bytes(b"log")
            result = core.prepare(dot, root / "prepared")
            self.assertFalse(result["ok"])
            self.assertEqual(result["errors"][0]["code"], "no_sounds_found")
            self.assertIn("resources", result["errors"][0]["message"])

    def test_command_line_install_switches_imported_audio_on(self):
        import sys as _sys
        from tools.pack_helper import __main__ as cli
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "both.zip"
            legacy_zip(source, {"music/calm1.ogg": b"owned music"})
            prepared = core.prepare(source, root / "prepared",
                                    import_mapping_path=importer_map(root / "mapping.json"))
            self.assertTrue(prepared["ok"], prepared)
            sd = JarImportAuditTests.sd(self, root)
            out = root / "out.jsonl"
            argv = ["pack_helper", "--json-output", str(out), "install", prepared["path"], str(sd)]
            with mock.patch.object(_sys, "argv", argv):
                self.assertEqual(cli.main(), 0)
            pack = "texturepacks/" + prepared["name"]
            for folder in (sd / "apps/CavEX", sd):
                paths = json.loads((folder / "config.json").read_text(encoding="utf-8"))["paths"]
                self.assertEqual(paths["audiopack"], pack, folder)
                self.assertEqual(paths["texturepack"], pack, folder)

    def test_helper_and_cli_give_identical_audio_bytes(self):
        import subprocess as _subprocess
        import sys as _sys
        transcoder = _transcoder()
        repo = Path(core.__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            jar = self._jar(root)
            resources = beta_resources(root / "resources")
            # The live importer map, the same derivation build_resources bakes
            # into the bundle, so this runs without a built bundle too.
            from tools import import_owned_beta_assets as importer
            keys, variants = importer.sound_keys_and_max_variants()
            mapping = root / "live-mapping.json"
            mapping.write_text(json.dumps({
                "schema_version": 1, "release_version": "test",
                "texture_paths": importer.extract_texture_dest_paths(
                    importer.TEXTURE_C.read_text(encoding="utf-8", errors="replace")),
                "sound_keys": keys, "sound_max_variants": variants,
                "music_keys": importer.music_keys(),
                "creative_music_keys": importer.creative_music_keys(),
                "records": list(importer.RECORD_STEMS),
                "dest_to_source_overrides": importer.DEST_TO_JAR_SOURCE_OVERRIDE,
                "required_textures": []}), encoding="utf-8")
            helper = core.prepare(jar, root / "prepared", resources_path=resources,
                                  transcoder=transcoder, import_mapping_path=mapping)
            self.assertTrue(helper["ok"], helper)
            out = root / "cli"
            args = [_sys.executable, str(repo / "tools/import_owned_beta_assets.py"),
                    "--jar", str(jar), "--resources", str(resources), "--out", str(out)]
            args += ["--transcoder", str(transcoder)] if transcoder else ["--no-sound-transcode"]
            args += ["--mapping", str(mapping)]
            proc = _subprocess.run(args, cwd=repo, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            prepared = Path(helper["path"])
            audio = [row["path"] for row in helper["manifest"]["files"]
                     if row["path"].startswith(("sound/", "music/", "records/"))]
            if transcoder is None:
                audio = [rel for rel in audio if not rel.startswith("sound/")]
            self.assertIn("music/calm1.ogg", audio)
            for rel in audio + [row["path"] for row in helper["manifest"]["files"]
                                if row["path"].endswith(".png")]:
                self.assertEqual((out / rel).read_bytes(), (prepared / rel).read_bytes(), rel)


def real_launcher_layout(root):
    """Paths-only mirror of the maintainer's real install (2026-10-30 Wii run):
    .minecraft/versions/b1.7.3/b1.7.3.jar, .minecraft/resources/{sound,newsound,
    music,newmusic,streaming,sound3,...}, assets/indexes/pre-1.6.json, a big
    launcher_log.txt. Generated bytes only."""
    dot = root / ".minecraft"
    jar = dot / "versions/b1.7.3/b1.7.3.jar"
    jar.parent.mkdir(parents=True)
    client_jar(jar)
    (dot / "versions/b1.7.3/b1.7.3.json").write_text('{"assets": "pre-1.6"}', encoding="utf-8")
    res = dot / "resources"
    pcm_wav(res / "newsound/step/grass1.wav", 440)
    for rel in ("music/calm1.ogg", "newmusic/hal1.ogg", "streaming/13.ogg"):
        (res / rel).parent.mkdir(parents=True, exist_ok=True)
        (res / rel).write_bytes(b"placeholder-" + rel.encode())
    for extra in ("sound3", "icons", "pe"):
        (res / extra).mkdir()
    (res / "READ_ME_I_AM_VERY_IMPORTANT").write_text("x", encoding="utf-8")
    (dot / "assets/indexes").mkdir(parents=True)
    (dot / "assets/objects").mkdir(parents=True)
    (dot / "assets/indexes/pre-1.6.json").write_text('{"map_to_resources": true, "objects": {}}',
                                                     encoding="utf-8")
    (dot / "launcher_log.txt").write_bytes(b"x" * 4096)
    return dot, jar


class AutoFindAndNameTests(unittest.TestCase):
    """2026-10-30 real-Wii run: no sounds without --resources; pack shown as
    "minecraft" / "b1.7.3-<hash>"; helper export needed a file outside it."""

    def _audio(self, result):
        return sorted(row["path"] for row in result["files"] if row.get("category") == "audio"
                      and not row["path"].startswith("sound/"))

    def test_jar_in_the_launcher_layout_finds_its_sounds_without_resources(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dot, jar = real_launcher_layout(root)
            result = core.prepare(jar, root / "prepared",
                                  import_mapping_path=importer_map(root / "mapping.json"))
            self.assertTrue(result["ok"], result)
            self.assertEqual(self._audio(result),
                             ["music/calm1.ogg", "music/hal1.ogg", "records/13.ogg"])
            self.assertTrue(any(str(dot / "resources") in w for w in result["warnings"]),
                            result["warnings"])

    def test_copied_jar_falls_back_to_the_launcher_location(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dot, jar = real_launcher_layout(root)
            copied = root / "Desktop" / "minecraft.jar"
            copied.parent.mkdir()
            copied.write_bytes(jar.read_bytes())
            with mock.patch.dict(os.environ, {core.NO_LAUNCHER_SEARCH_ENV: ""}), \
                    mock.patch("tools.pack_helper.discovery.minecraft_roots", return_value=[dot]):
                result = core.prepare(copied, root / "prepared",
                                      import_mapping_path=importer_map(root / "mapping.json"))
            self.assertTrue(result["ok"], result)
            self.assertIn("music/calm1.ogg", self._audio(result))
            # minecraft.jar is still "Beta 1.7.3", never "minecraft".
            self.assertEqual(result["name"], core.BETA_PACK_NAME)

    def test_beta_jar_pack_is_beta_1_7_3_and_replaces_every_earlier_import(self):
        """Folder "Beta-1.7.3" (no space, same as the in-game importer), shown
        as "Beta 1.7.3" via the manifest's display_name. Earlier Beta imports
        ("Beta 1.7.3", "minecraft", "b1.7.3-<hash>", the command line's
        lowercase "beta-1.7.3") are REPLACED, never merged; a player's other
        folders and helper leftovers under other names are not."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            jar = root / "minecraft.jar"
            client_jar(jar)
            prepared = core.prepare(jar, root / "prepared")
            self.assertTrue(prepared["ok"], prepared)
            # The in-game list shows the manifest "name" (beta_import.c).
            self.assertEqual(prepared["manifest"]["name"], "Beta 1.7.3")
            sd = JarImportAuditTests.sd(self, root)
            packs = sd / "texturepacks"
            for old in ("Beta 1.7.3", "minecraft", "b1.7.3-f3268c66ea", "beta-1.7.3"):
                (packs / old).mkdir(parents=True)
                (packs / old / "stale.png").write_bytes(b"old")
                (packs / old / core.MANIFEST_NAME).write_text("{}", encoding="utf-8")
            leftover = packs / "b1.7.3-copy-0123456789"
            leftover.mkdir()
            (leftover / core.MANIFEST_NAME).write_text("{}", encoding="utf-8")
            result = core.install(prepared, sd)
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["pack_name"], "Beta-1.7.3")
            self.assertEqual(sorted(p.name for p in packs.iterdir()),
                             ["Beta-1.7.3", "b1.7.3-copy-0123456789"])
            self.assertFalse((packs / "Beta-1.7.3" / "stale.png").exists())  # replaced, not merged
            manifest = json.loads((packs / "Beta-1.7.3" / core.MANIFEST_NAME).read_text(encoding="utf-8"))
            self.assertEqual(manifest["name"], "Beta 1.7.3")
            for folder in (sd / "apps/CavEX", sd):
                config = json.loads((folder / "config.json").read_text(encoding="utf-8"))
                self.assertEqual(config["paths"]["texturepack"], "texturepacks/Beta-1.7.3")
            self.assertEqual(result["old_imports"], ["b1.7.3-copy-0123456789"])
            again = core.install(prepared, sd, remove_old=True)
            self.assertTrue(again["ok"], again)
            self.assertEqual(again["removed_old"], ["b1.7.3-copy-0123456789"])
            self.assertEqual([p.name for p in packs.iterdir()], ["Beta-1.7.3"])
            # A same-folder pack the helper did not write is kept, renamed.
            (packs / "Beta-1.7.3" / core.MANIFEST_NAME).unlink()
            (packs / "minecraft").mkdir()
            (packs / "minecraft" / "terrain.png").write_bytes(b"player's raw unzip")
            third = core.install(prepared, sd)
            self.assertTrue(third["ok"], third)
            self.assertEqual(third["kept_as"], "Beta-1.7.3-old")
            self.assertEqual(sorted(p.name for p in packs.iterdir()),
                             ["Beta-1.7.3", "Beta-1.7.3-old", "minecraft"])
            no_game = root / "empty-card"
            no_game.mkdir()
            missing = core.install(prepared, no_game)
            self.assertEqual(missing["errors"][0]["code"], "game_not_installed")
            self.assertIn("Put CavEX on the SD card first", missing["errors"][0]["message"])

    def test_exported_pack_helper_alone_imports_sounds(self):
        """The package must not need tools/import_owned_beta_assets.py."""
        import shutil as _shutil
        import subprocess as _subprocess
        import sys as _sys
        repo = Path(core.__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            export = root / "export"
            _shutil.copytree(repo / "tools/pack_helper", export / "pack_helper",
                             ignore=_shutil.ignore_patterns("resources", "__pycache__"))
            res = export / "pack_helper/resources"
            for rel, size in (("beta-base/anim.png", (256, 256)), ("beta-base/pointer.png", (32, 32)),
                              ("assets/gui_2.png", (256, 256))):
                (res / rel).parent.mkdir(parents=True, exist_ok=True)
                (res / rel).write_bytes(png(size))
            importer_map(res / "mapping.json")
            dot, jar = real_launcher_layout(root)
            code = ("import json,sys; from pack_helper import core; "
                    "r=core.prepare(sys.argv[1], sys.argv[2]); "
                    "print(json.dumps([r['ok'], r['errors'], sorted(f['path'] for f in r['files'] "
                    "if f.get('category')=='audio')]))")
            proc = _subprocess.run([_sys.executable, "-c", code, str(jar), str(root / "out")],
                                   cwd=export, capture_output=True, text=True,
                                   env=dict(os.environ, PYTHONPATH=str(export)))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            ok, errors, audio = json.loads(proc.stdout.strip().splitlines()[-1])
            self.assertTrue(ok, errors)
            self.assertIn("music/calm1.ogg", audio)


if __name__ == "__main__":
    unittest.main()
