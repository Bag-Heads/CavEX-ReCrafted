"""Real Tk worker/queue imports using freshly built resources and scratch inputs.

No engine calls are mocked. Discovery and modal error presentation are isolated
so these tests neither inspect personal asset locations nor block unattended runs.
"""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time
import tkinter as tk
from tkinter import ttk
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from pack_helper import core
from pack_helper.gui import PackHelper
from pack_helper.selftest import _verify_resources, _write_synthetic_source, _write_wav


def tree_hashes(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


class RealGuiImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resources = REPO / "tools/pack_helper/resources"
        if not (resources / "resource_hashes.json").is_file():
            raise unittest.SkipTest("Fresh helper resources absent; run build_resources.py before integration tests")
        _verify_resources(resources)

    def setUp(self):
        try:
            self.root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest("Tk display unavailable: " + str(exc))
        self.root.withdraw()
        with patch("pack_helper.gui.discover_packs", return_value=[]):
            self.app = PackHelper(self.root)
        self.scratch = tempfile.TemporaryDirectory(prefix="cavex-gui-integration-")
        self.addCleanup(self.scratch.cleanup)
        self.base = Path(self.scratch.name)
        self.texture = self.base / "owned synthetic textures"
        self.texture.mkdir()
        _write_synthetic_source(self.texture)
        self.audio = self.base / "owned synthetic resources"
        _write_wav(self.audio / "sound/step/grass1.wav")
        self.game = self.base / "scratch game destination"
        self.game.mkdir()
        # Capability fixture only: never executable and never launched.
        (self.game / "CavEX-ReCrafted.exe").write_bytes(b"SYNTHETIC CAPABILITY FIXTURE paths.audiopack\x00")
        self.original_config = {"paths": {"texturepack": "texturepacks/previous-textures",
                                           "audiopack": "texturepacks/previous-audio"},
                                "unrelated": {"controller": "preserve me", "volume": 37}}
        self.config_bytes = (json.dumps(self.original_config, indent=2) + "\n").encode()
        (self.game / "config.json").write_bytes(self.config_bytes)
        (self.game / "config.json.bak").write_bytes(b"user's unrelated backup")
        (self.game / "saves").mkdir()
        (self.game / "saves/world.dat").write_bytes(b"synthetic save sentinel")
        previous = self.game / "texturepacks/previous-pack"
        previous.mkdir(parents=True)
        (previous / "sentinel.txt").write_bytes(b"previous pack is preserved")
        self.sources_before = (tree_hashes(self.texture), tree_hashes(self.audio))
        self.app.sd.set(str(self.game))

    def tearDown(self):
        if hasattr(self, "app"):
            self.assertFalse(self.app.busy, "worker must finish before test teardown")
            self.root.after_cancel(self.app.poll_id)
            self.app.workspace.cleanup()
            self.root.destroy()

    def buttons(self):
        def walk(parent):
            for widget in parent.winfo_children():
                yield widget
                yield from walk(widget)
        return {widget.cget("text"): widget for widget in walk(self.app.root)
                if isinstance(widget, ttk.Button)}

    def run_import(self):
        self.app.import_assets()
        self.assertTrue(self.app.busy)
        self.assertTrue(self.app.disabled_widgets)
        self.assertTrue(all("disabled" in widget.state() for widget in self.app.disabled_widgets))
        deadline = time.monotonic() + 30
        while self.app.busy and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertFalse(self.app.busy, "real import worker exceeded 30-second bound")
        self.root.update_idletasks()
        self.assertEqual(self.app.disabled_widgets, [])

    def assert_success(self, textures, audio, activate=True):
        self.assertIn("Open imported folder", self.buttons())
        self.assertTrue(any(name in self.buttons() for name in ("Import again", "Import more assets")))
        installed = [path for path in (self.game / "texturepacks").iterdir()
                     if path.name != "previous-pack"]
        self.assertEqual(len(installed), 1)
        output = installed[0]
        manifest = json.loads((output / core.MANIFEST_NAME).read_text(encoding="utf-8"))
        for row in manifest["files"]:
            data = (output / row["path"]).read_bytes()
            self.assertEqual(len(data), row["size"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), row["sha256"])
        paths = {row["path"] for row in manifest["files"]}
        self.assertEqual("terrain.png" in paths, textures)
        self.assertEqual("sound/step/grass1.wav" in paths, audio)
        if audio:
            import wave
            with wave.open(str(output / "sound/step/grass1.wav"), "rb") as stream:
                self.assertEqual((stream.getnchannels(), stream.getsampwidth(), stream.getframerate()), (1, 2, 11025))
        if activate:
            selected = json.loads((self.game / "config.json").read_text(encoding="utf-8"))
            value = "texturepacks/" + output.name
            self.assertEqual(selected["unrelated"], self.original_config["unrelated"])
            self.assertEqual(selected["paths"]["texturepack"], value if textures else self.original_config["paths"]["texturepack"])
            self.assertEqual(selected["paths"]["audiopack"], value if audio else self.original_config["paths"]["audiopack"])
            self.assertEqual((self.game / "config.json.pack-helper.bak").read_bytes(), self.config_bytes)
        else:
            self.assertEqual((self.game / "config.json").read_bytes(), self.config_bytes)
            self.assertFalse((self.game / "config.json.pack-helper.bak").exists())
        self.assertEqual(tree_hashes(self.texture), self.sources_before[0])
        self.assertEqual(tree_hashes(self.audio), self.sources_before[1])
        self.assertEqual((self.game / "config.json.bak").read_bytes(), b"user's unrelated backup")
        self.assertEqual((self.game / "saves/world.dat").read_bytes(), b"synthetic save sentinel")
        self.assertEqual((self.game / "texturepacks/previous-pack/sentinel.txt").read_bytes(), b"previous pack is preserved")

    def test_real_texture_only_import(self):
        self.app.source.set(str(self.texture))
        self.run_import()
        self.assert_success(True, False)

    def test_real_audio_only_import(self):
        self.app.audio.set(str(self.audio))
        self.run_import()
        self.assert_success(False, True)

    def test_real_combined_import(self):
        self.app.source.set(str(self.texture))
        self.app.audio.set(str(self.audio))
        self.run_import()
        self.assert_success(True, True)

    def test_real_import_without_activation(self):
        self.app.source.set(str(self.texture))
        self.app.audio.set(str(self.audio))
        self.app.select.set(False)
        self.run_import()
        self.assert_success(True, True, activate=False)

    def test_real_validation_failure_preserves_inputs_then_retry_succeeds(self):
        executable = self.game / "CavEX-ReCrafted.exe"
        fixture = executable.read_bytes()
        executable.unlink()
        self.app.source.set(str(self.texture))
        self.app.look.set("smooth")
        self.app.advanced_open.set(True)
        self.app.toggle_advanced()
        with patch("pack_helper.gui.messagebox.showerror") as notice:
            self.run_import()
        notice.assert_called_once()
        self.assertIn("not found", notice.call_args.args[1])
        self.assertIn("could not finish", self.app.status.get())
        self.assertEqual(self.app.source.get(), str(self.texture))
        self.assertEqual(self.app.sd.get(), str(self.game))
        self.assertEqual(self.app.look.get(), "smooth")
        self.assertTrue(self.app.advanced_open.get())
        self.assertNotIn("Open imported folder", self.buttons())
        self.assertEqual((self.game / "config.json").read_bytes(), self.config_bytes)
        executable.write_bytes(fixture)
        self.run_import()
        self.assert_success(True, False)

    def test_real_preparation_failure_preserves_source_then_retry_succeeds(self):
        source = self.base / "owned synthetic texture pack.zip"
        invalid_zip = b"synthetic invalid ZIP archive"
        source.write_bytes(invalid_zip)
        self.app.source.set(str(source))
        self.app.look.set("smooth")
        with patch("pack_helper.gui.messagebox.showerror") as notice:
            self.run_import()
        notice.assert_called_once()
        self.assertIn("zip", notice.call_args.args[1].lower())
        self.assertIn("could not finish", self.app.status.get())
        self.assertEqual(source.read_bytes(), invalid_zip)
        self.assertEqual(self.app.source.get(), str(source))
        self.assertEqual(self.app.sd.get(), str(self.game))
        self.assertEqual(self.app.look.get(), "smooth")
        self.assertEqual((self.game / "config.json").read_bytes(), self.config_bytes)
        self.assertNotIn("Open imported folder", self.buttons())
        self.assertEqual([path.name for path in (self.game / "texturepacks").iterdir()], ["previous-pack"])
        with zipfile.ZipFile(source, "w") as archive:
            for path in self.texture.rglob("*"):
                if path.is_file():
                    archive.write(path, path.relative_to(self.texture).as_posix())
        repaired_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        self.run_import()
        self.assert_success(True, False)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), repaired_hash)


if __name__ == "__main__":
    unittest.main()
