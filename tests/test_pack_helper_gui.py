import sys
from pathlib import Path
import tempfile
import threading
import time
import tkinter as tk
from tkinter import ttk
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pack_helper.gui import PackHelper


class GuiTests(unittest.TestCase):
    def setUp(self):
        try:
            self.root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest("Display unavailable: " + str(exc))
        self.root.withdraw()
        with patch("pack_helper.gui.discover_packs", return_value=[]):
            self.app = PackHelper(self.root)

    def tearDown(self):
        if hasattr(self, "app"):
            self.root.after_cancel(self.app.poll_id)
            self.app.workspace.cleanup()
            self.root.destroy()

    def test_busy_disables_input_until_completion(self):
        gate = threading.Event()
        completed = []
        self.app.run(lambda: gate.wait(2), completed.append)
        self.assertTrue(self.app.busy)
        self.assertTrue(self.app.disabled_widgets)
        for widget in self.app.disabled_widgets:
            self.assertIn("disabled", widget.state())
        gate.set()
        deadline = time.monotonic() + 3
        while self.app.busy and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertFalse(self.app.busy)
        self.assertEqual(completed, [True])

    def test_advanced_is_optional_and_preserves_choices(self):
        self.assertEqual(self.app.advanced.winfo_manager(), "")
        self.app.advanced_open.set(True)
        self.app.toggle_advanced()
        self.assertEqual(self.app.advanced.winfo_manager(), "grid")
        self.app.look.set("smooth")
        self.app.select.set(False)
        self.app.advanced_open.set(False)
        self.app.toggle_advanced()
        self.app.choose_page()
        self.assertEqual(self.app.look.get(), "smooth")
        self.assertFalse(self.app.select.get())
        self.assertEqual(self.app.advanced.winfo_manager(), "")

    def test_one_action_prepares_once_then_installs_supplied_assets(self):
        self.assertEqual(self.app.look.get(), "crisp")
        self.assertTrue(self.app.select.get())
        for texture, audio, has_audio in (("owned.jar", "", False),
                                          ("owned.jar", "resources", True),
                                          ("", "resources", True)):
            with self.subTest(texture=texture, audio=audio):
                self.app.source.set(texture)
                self.app.audio.set(audio)
                self.app.sd.set("game")
                prepared = {"ok": True, "files": [{"category": "audio" if has_audio else "texture",
                                                    "status": "transcoded"},
                                                   {"category": "texture", "status": "unsupported", "path": "custom.png"},
                                                   {"category": "texture", "status": "fallback", "path": "optional.png"}],
                            "warnings": ["A skipped file"]}
                with patch("pack_helper.gui.core.validate_destination") as preflight, \
                        patch("pack_helper.gui.core.prepare", return_value=prepared) as prepare, \
                        patch("pack_helper.gui.core.install", return_value={"ok": True, "warnings": []}) as install, \
                        patch.object(self.app, "installed") as done, \
                        patch.object(self.app, "run", side_effect=lambda operation, callback: callback(operation())):
                    self.app.import_assets()
                preflight.assert_called_once_with("game", True)
                prepare.assert_called_once()
                self.assertEqual(prepare.call_args.args[0], texture or audio)
                self.assertEqual(prepare.call_args.kwargs["resources_path"], audio if texture and audio else None)
                self.assertEqual(prepare.call_args.kwargs["look"], "crisp")
                install.assert_called_once()
                self.assertEqual(install.call_args.kwargs["audio_select"], has_audio)
                self.assertTrue(install.call_args.kwargs["select"])
                self.assertIn("A skipped file", done.call_args.args[0]["warnings"])
                self.assertEqual(sum(done.call_args.args[0]["imported"].values()), 1)
                self.assertTrue(any("custom.png" in warning for warning in done.call_args.args[0]["warnings"]))

    def test_failed_preparation_does_not_install(self):
        self.app.source.set("bad.zip")
        self.app.sd.set("game")
        failure = {"ok": False, "errors": [{"message": "Unsupported layout"}]}
        with patch("pack_helper.gui.core.validate_destination"), \
                patch("pack_helper.gui.core.prepare", return_value=failure), \
                patch("pack_helper.gui.core.install") as install, \
                patch.object(self.app, "installed") as done, \
                patch.object(self.app, "run", side_effect=lambda operation, callback: callback(operation())):
            self.app.import_assets()
        install.assert_not_called()
        done.assert_called_once_with(failure)

    def test_missing_destination_stops_before_preparation(self):
        self.app.source.set("owned.jar")
        with patch("pack_helper.gui.messagebox.showinfo") as notice, \
                patch("pack_helper.gui.core.prepare") as prepare:
            self.app.import_assets()
        notice.assert_called_once()
        prepare.assert_not_called()

    def test_partial_install_failure_reports_copied_location(self):
        with patch("pack_helper.gui.messagebox.showerror") as notice:
            self.app.failure({"path": "game/texturepacks/new", "errors": [{"message": "Config changed"}],
                              "warnings": ["Automatic selection did not complete"]})
        self.assertIn("game/texturepacks/new", notice.call_args.args[1])
        self.assertIn("activation did not finish", self.app.status.get())

    def find_button(self, text):
        def descendants(parent):
            for widget in parent.winfo_children():
                yield widget
                yield from descendants(widget)
        return next((widget for widget in descendants(self.root)
                     if isinstance(widget, ttk.Button) and widget.cget("text") == text), None)

    def test_success_folder_action_binds_installed_path_with_or_without_activation(self):
        for selected in (True, False):
            with self.subTest(selected=selected), tempfile.TemporaryDirectory() as temporary:
                installed = Path(temporary) / "actual imported pack"
                installed.mkdir()
                result = {"ok": True, "path": str(installed), "selected": selected,
                          "imported": {"texture": 1, "audio": 0}}
                self.app.sd.set("a different destination")
                self.app.installed(result)
                result["path"] = "changed after rendering"
                action = self.find_button("Open imported folder")
                self.assertIsNotNone(action)
                self.assertIsNotNone(self.find_button("Import again"))
                with patch.object(self.app, "open_imported_folder") as opened:
                    action.invoke()
                opened.assert_called_once_with(str(installed))
                if not selected:
                    self.assertTrue(any("Imported without activation" in child.cget("text")
                                        for child in self.app.body.winfo_children()
                                        if isinstance(child, ttk.Label)))

    def test_missing_imported_folder_reports_path_without_changing_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "removed pack"
            self.app.installed({"ok": True, "path": str(path), "selected": False,
                                "imported": {"texture": 1, "audio": 0}})
            status = self.app.status.get()
            with patch("pack_helper.gui.messagebox.showwarning") as notice, \
                    patch("pack_helper.gui.os.startfile", create=True) as windows, \
                    patch("pack_helper.gui.subprocess.Popen") as other:
                self.find_button("Open imported folder").invoke()
            notice.assert_called_once()
            self.assertIn(str(path), notice.call_args.args[1])
            windows.assert_not_called()
            other.assert_not_called()
            self.assertEqual(self.app.status.get(), status)
            self.assertIsNotNone(self.find_button("Import again"))

    def test_folder_dispatch_uses_argument_safe_absolute_path_on_each_platform(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "pack with spaces & [brackets]; $name"
            folder.mkdir()
            for platform, command in (("win32", None), ("darwin", "open"), ("linux", "xdg-open")):
                with self.subTest(platform=platform), \
                        patch("pack_helper.gui.sys.platform", platform), \
                        patch("pack_helper.gui.os.startfile", create=True) as windows, \
                        patch("pack_helper.gui.subprocess.Popen") as other, \
                        patch("pack_helper.gui.messagebox.showwarning") as notice:
                    self.app.open_imported_folder(folder)
                    if command:
                        other.assert_called_once_with([command, str(folder.resolve())])
                        windows.assert_not_called()
                    else:
                        windows.assert_called_once_with(str(folder.resolve()), "explore")
                        other.assert_not_called()
                    notice.assert_not_called()

    def test_folder_launch_failure_keeps_success_and_reports_manual_location(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.app.installed({"ok": True, "path": temporary, "selected": True,
                                "imported": {"texture": 1, "audio": 0}})
            status = self.app.status.get()
            with patch("pack_helper.gui.sys.platform", "linux"), \
                    patch("pack_helper.gui.subprocess.Popen", side_effect=OSError("launcher unavailable")), \
                    patch("pack_helper.gui.messagebox.showwarning") as notice:
                self.find_button("Open imported folder").invoke()
            self.assertIn(temporary, notice.call_args.args[1])
            self.assertIn("import is complete", notice.call_args.args[1])
            self.assertEqual(self.app.status.get(), status)
            self.assertIsNotNone(self.find_button("Open imported folder"))

    def test_partial_failure_does_not_offer_success_folder_action(self):
        with patch("pack_helper.gui.messagebox.showerror"):
            self.app.installed({"ok": False, "path": "copied but not activated",
                                "errors": [{"message": "Activation failed"}]})
        self.assertIsNone(self.find_button("Open imported folder"))

    def test_theme_defaults_light_and_switches_to_dark(self):
        self.assertEqual(self.app.theme.get(), "Light")
        light = dict(self.app.palette)
        with patch("pack_helper.gui.appearance.apply_theme",
                   wraps=__import__("pack_helper.appearance", fromlist=["x"]).apply_theme) as themed:
            self.app.theme.set("Dark")
            self.app.apply_theme()
        self.assertEqual(themed.call_args.args[1], "dark")
        self.assertNotEqual(self.app.palette["bg"], light["bg"])
        header, _ = self.app.header_widgets[0]
        self.assertEqual(header.cget("bg"), self.app.palette["header_bg"])

    def test_import_action_gated_and_summary_follows_inputs(self):
        button = self.app.import_button
        self.assertIn("disabled", button.state())
        self.assertEqual(self.app.asset_summary.get(), "No assets selected")
        self.app.audio.set("resources")
        self.assertIn("disabled", button.state())
        self.app.sd.set("E:\\")
        self.assertNotIn("disabled", button.state())
        self.assertEqual(self.app.asset_summary.get(), "Sounds and music")
        self.assertEqual(self.app.source_name.get(), "Audio-only selection")
        self.app.source.set("C:/packs/minecraft-b1.7.3.jar")
        self.assertEqual(self.app.asset_summary.get(), "Textures + sounds and music")
        self.assertIn("minecraft-b1.7.3.jar", self.app.source_name.get())
        self.app.select.set(False)
        self.assertEqual(self.app.activation_summary.get(), "Keep current settings")
        self.app.look.set("smooth")
        self.assertEqual(self.app.look_summary.get(), "Smooth")
        self.find_button("Clear").invoke()
        self.assertEqual(self.app.source.get(), "")

    def test_busy_locks_theme_and_import_then_restores(self):
        self.app.source.set("owned.jar")
        self.app.sd.set("game")
        gate = threading.Event()
        self.app.run(lambda: gate.wait(2) and {"ok": False, "errors": []}, lambda result: None)
        self.assertIn("disabled", self.app.theme_choice.state())
        self.assertIn("disabled", self.app.import_button.state())
        self.app.update_summary()
        self.assertIn("disabled", self.app.import_button.state())
        gate.set()
        deadline = time.monotonic() + 3
        while self.app.busy and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertNotIn("disabled", self.app.theme_choice.state())
        self.assertIn("readonly", self.app.theme_choice.state())
        self.assertNotIn("disabled", self.app.import_button.state())

    def test_worker_progress_is_applied_on_main_thread_poll(self):
        self.app.report_progress({"phase": "convert", "current": 3, "total": 9})
        self.assertNotIn("3/9", self.app.status.get())
        self.root.after_cancel(self.app.poll_id)
        self.app.poll()
        self.assertIn("Converting textures… 3/9", self.app.status.get())
        self.assertEqual(float(self.app.progress.cget("value")), 3.0)

    def test_failure_keeps_inputs_and_form_and_is_not_success(self):
        self.app.source.set("bad.zip")
        self.app.audio.set("resources")
        self.app.sd.set("game")
        with patch("pack_helper.gui.messagebox.showerror"):
            self.app.installed({"ok": False, "errors": [{"message": "CavEX was not found"}]})
        self.assertEqual(self.app.phase, "error")
        self.assertEqual((self.app.source.get(), self.app.audio.get(), self.app.sd.get()),
                         ("bad.zip", "resources", "game"))
        self.assertEqual(self.app.form.winfo_manager(), "grid")
        self.assertEqual(self.app.import_button.cget("text"), "Import assets")

    def test_import_again_returns_to_form_with_choices_and_scrollable_notes(self):
        self.app.source.set("owned.jar")
        self.app.sd.set("game")
        with tempfile.TemporaryDirectory() as temporary:
            self.app.installed({"ok": True, "path": temporary, "selected": True,
                                "imported": {"texture": 4, "audio": 2},
                                "warnings": ["note {0}".format(i) for i in range(40)]})
            self.assertEqual(self.app.form.winfo_manager(), "")
            self.assertIn("note 39", self.app.notes.get("1.0", "end"))
            self.assertTrue(any(isinstance(child, ttk.Label) and child.cget("text") == "4 textures · 2 audio files"
                                for child in self.app.body.winfo_children()))
            self.find_button("Import again").invoke()
        self.assertEqual(self.app.form.winfo_manager(), "grid")
        self.assertIsNone(self.find_button("Open imported folder"))
        self.assertEqual(self.app.source.get(), "owned.jar")
        self.assertEqual(self.app.import_button.cget("text"), "Import assets")


if __name__ == "__main__":
    unittest.main()
