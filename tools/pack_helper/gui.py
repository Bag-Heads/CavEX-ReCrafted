"""Choose assets and a CavEX location, then import in one operation.

Layout follows the approved R6 design (docs/pack_helper_design). Light (Slate)
and Dark (Obsidian) come from appearance.py; the choice lasts for this session
only and is not saved.
"""
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import appearance, core
from .discovery import discover_packs

THEME_NAMES = (("Light", "light"), ("Dark", "dark"))
FONT = "Segoe UI" if sys.platform == "win32" else "TkDefaultFont"


class PackHelper:
    def __init__(self, root):
        self.root = root
        root.title("CavEX Asset Importer")
        width = min(920, root.winfo_screenwidth() - 40)
        height = min(680, root.winfo_screenheight() - 80)
        root.geometry("{0}x{1}".format(width, height))
        root.minsize(min(820, width), min(620, height))
        self.events = queue.Queue()
        self.workspace = tempfile.TemporaryDirectory(prefix="cavewii-pack-")
        self.busy = False
        self.phase = "form"
        self.disabled_widgets = []
        self.controls = []
        self.palette = {}
        self.notes = None
        self.source = tk.StringVar()
        self.audio = tk.StringVar()
        self.look = tk.StringVar(value="crisp")
        self.sd = tk.StringVar()
        self.select = tk.BooleanVar(value=True)
        self.advanced_open = tk.BooleanVar(value=False)
        self.theme = tk.StringVar(value="Light")
        self.status = tk.StringVar()
        self.source_name = tk.StringVar()
        self.asset_summary = tk.StringVar()
        self.activation_summary = tk.StringVar()
        self.look_summary = tk.StringVar()
        self.launch_note = tk.StringVar()
        self.logo, self.icon = appearance.load_branding(root)
        self.build()
        self.apply_theme()
        for variable in (self.source, self.audio, self.sd, self.select, self.look):
            variable.trace_add("write", self.update_summary)
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.poll_id = root.after(80, self.poll)
        self.choose_page()

    def register(self, widget):
        self.controls.append(widget)
        return widget

    def build(self):
        root = self.root
        root.columnconfigure(0, weight=1)
        root.rowconfigure(2, weight=1)
        header = tk.Frame(root, padx=22, pady=8)
        header.grid(row=0, column=0, sticky="ew")
        logo = tk.Label(header, image=self.logo)
        logo.pack(side="left")
        words = tk.Frame(header)
        words.pack(side="left", padx=(28, 0), fill="both", expand=True)
        kicker = tk.Label(words, text="ASSET IMPORTER", font=(FONT, 9, "bold"), anchor="w")
        kicker.pack(fill="x", pady=(8, 2))
        title = tk.Label(words, text="Asset Importer", font=(FONT, 19, "bold"), anchor="w")
        title.pack(fill="x")
        tagline = tk.Label(words, text="Your Beta textures, sounds and music. Sized for CavEX.",
                           font=(FONT, 10), anchor="w")
        tagline.pack(fill="x", pady=(4, 0))
        self.header_widgets = [(header, None), (logo, None), (words, None),
                               (kicker, "header_kicker"), (title, "header_ink"),
                               (tagline, "header_muted")]

        bar = ttk.Frame(root, style="Bar.TFrame", padding=(22, 7))
        bar.grid(row=1, column=0, sticky="ew")
        ttk.Label(bar, text="Import your own Beta assets. Conversion is automatic.",
                  style="Bar.TLabel").pack(side="left")
        self.theme_choice = ttk.Combobox(bar, textvariable=self.theme, state="readonly", width=7,
                                         values=[label for label, _ in THEME_NAMES])
        self.theme_choice.pack(side="right")
        self.theme_choice.bind("<<ComboboxSelected>>", lambda event: self.apply_theme())
        ttk.Label(bar, text="Appearance", style="Bar.TLabel").pack(side="right", padx=(0, 8))

        self.body = ttk.Frame(root, padding=(22, 16, 22, 0))
        self.body.grid(row=2, column=0, sticky="nsew")
        self.body.columnconfigure(0, weight=1)
        self.body.rowconfigure(0, weight=1)
        self.form = ttk.Frame(self.body)
        self.form.columnconfigure(0, weight=3)
        self.form.columnconfigure(1, weight=2)
        self.form.rowconfigure(0, weight=1)
        self.build_sources(self.form)
        self.build_destination(self.form)

        footer = ttk.Frame(root, padding=(22, 12))
        footer.grid(row=3, column=0, sticky="ew")
        footer.columnconfigure(0, weight=1)
        ttk.Label(footer, text="Original files, saves and bundled assets are kept.",
                  style="Muted.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(footer, textvariable=self.launch_note, style="Muted.TLabel").grid(row=1, column=0, sticky="w")
        self.import_button = ttk.Button(footer, text="Import assets", style="Accent.TButton",
                                        command=self.primary_action)
        self.import_button.grid(row=0, column=1, rowspan=2, sticky="e", padx=(16, 0))

        status = ttk.Frame(root, style="Bar.TFrame", padding=(22, 9))
        status.grid(row=4, column=0, sticky="ew")
        status.columnconfigure(0, weight=1)
        self.status_label = ttk.Label(status, textvariable=self.status, style="Bar.TLabel")
        self.status_label.grid(row=0, column=0, sticky="ew")
        status.bind("<Configure>", lambda event: self.status_label.configure(
            wraplength=max(200, event.width - 48)))
        self.progress = ttk.Progressbar(status, mode="indeterminate")
        self.progress.grid(row=1, column=0, sticky="ew", pady=(6, 0))
        self.progress.grid_remove()

    def path_row(self, parent, variable, row, choices):
        line = ttk.Frame(parent)
        line.grid(row=row, column=0, sticky="ew", pady=(6, 0))
        line.columnconfigure(0, weight=1)
        self.register(ttk.Entry(line, textvariable=variable)).grid(row=0, column=0, sticky="ew")
        buttons = ttk.Frame(line)
        buttons.grid(row=1, column=0, sticky="w", pady=(6, 0))
        for text, command in choices:
            self.register(ttk.Button(buttons, text=text, command=command)).pack(side="left", padx=(0, 6))
        self.register(ttk.Button(buttons, text="Clear", command=lambda: variable.set(""))).pack(side="left")

    def build_sources(self, form):
        left = ttk.Frame(form, padding=(0, 0, 24, 0))
        left.grid(row=0, column=0, sticky="nsew")
        left.columnconfigure(0, weight=1)
        ttk.Label(left, text="Choose textures, sounds, or both", style="Section.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 10))
        ttk.Label(left, text="Texture pack or Beta client  ·  optional", style="Strong.TLabel").grid(
            row=1, column=0, sticky="w")
        self.path_row(left, self.source, 2, [("File…", self.browse_file), ("Folder…", self.browse_folder)])
        ttk.Label(left, text="ZIP, JAR or folder · Modern Java packs are not supported.",
                  style="Muted.TLabel").grid(row=3, column=0, sticky="w", pady=(4, 8))
        local = ttk.Frame(left)
        local.grid(row=4, column=0, sticky="ew")
        ttk.Label(local, text="Found locally", style="Muted.TLabel").pack(side="left", padx=(0, 10))
        candidates = discover_packs()
        if candidates:
            self.found = self.register(ttk.Combobox(local, state="readonly", values=candidates))
            self.found.pack(side="left", fill="x", expand=True)
            self.found.bind("<<ComboboxSelected>>", lambda event: self.source.set(self.found.get()))
        else:
            self.found = None
            ttk.Label(local, text="No Beta clients or packs found. Use File… or Folder….",
                      style="Muted.TLabel").pack(side="left")
        selected = ttk.Frame(left, style="Selected.TFrame", padding=(10, 7))
        selected.grid(row=5, column=0, sticky="ew", pady=(10, 16))
        source_label = ttk.Label(selected, textvariable=self.source_name, style="Selected.TLabel")
        source_label.pack(anchor="w", fill="x")
        selected.bind("<Configure>", lambda event: source_label.configure(wraplength=max(150, event.width - 24)))
        ttk.Label(left, text="Sounds and music  ·  optional", style="Strong.TLabel").grid(row=6, column=0, sticky="w")
        self.path_row(left, self.audio, 7, [("Folder…", self.browse_audio)])
        ttk.Label(left, text="Choose .minecraft (its resources folder is found). Audio-only works too.",
                  style="Muted.TLabel").grid(row=8, column=0, sticky="w", pady=(4, 0))

    def build_destination(self, form):
        right = ttk.Frame(form, style="Panel.TFrame", padding=16)
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        ttk.Label(right, text="Your CavEX location", style="PanelTitle.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 8))
        ttk.Label(right, text="SD card root or PC game folder", style="Panel.TLabel").grid(row=1, column=0, sticky="w")
        self.register(ttk.Entry(right, textvariable=self.sd)).grid(row=2, column=0, sticky="ew", pady=(6, 6))
        self.register(ttk.Button(right, text="Choose location…", command=self.choose_card)).grid(
            row=3, column=0, sticky="w")
        ttk.Label(right, text="SD: choose the drive itself (E:\\).\nPC: game and config.json folder.",
                  style="PanelMuted.TLabel").grid(row=4, column=0, sticky="w", pady=(6, 12))
        ttk.Separator(right).grid(row=5, column=0, sticky="ew", pady=(0, 10))
        for row, (label, variable) in enumerate((("Selected", self.asset_summary),
                                                  ("Next launch", self.activation_summary),
                                                  ("Large textures", self.look_summary)), 6):
            line = ttk.Frame(right, style="Panel.TFrame")
            line.grid(row=row, column=0, sticky="ew", pady=(0, 6))
            line.columnconfigure(1, weight=1)
            ttk.Label(line, text=label, style="PanelMuted.TLabel", width=12).grid(
                row=0, column=0, sticky="nw", padx=(0, 8))
            ttk.Label(line, textvariable=variable, style="PanelStrong.TLabel", wraplength=190).grid(
                row=0, column=1, sticky="w")
        self.register(ttk.Checkbutton(right, text="Import options", variable=self.advanced_open,
                                      command=self.toggle_advanced, style="Panel.TCheckbutton")).grid(
            row=9, column=0, sticky="w", pady=(4, 0))
        self.advanced = ttk.Frame(right, style="Panel.TFrame", padding=(0, 6, 0, 0))
        self.advanced.grid(row=10, column=0, sticky="ew")
        self.register(ttk.Checkbutton(self.advanced, text="Use assets next time I play", variable=self.select,
                                      style="Panel.TCheckbutton")).pack(anchor="w")
        ttk.Label(self.advanced, text="Reduce large textures:", style="PanelMuted.TLabel").pack(anchor="w", pady=(6, 4))
        radios = ttk.Frame(self.advanced, style="Panel.TFrame")
        radios.pack(anchor="w")
        for text, value in (("Crisp pixels", "crisp"), ("Smooth", "smooth")):
            self.register(ttk.Radiobutton(radios, text=text, value=value, variable=self.look,
                                          style="Panel.TRadiobutton")).pack(side="left", padx=(0, 8))
        self.advanced.grid_remove()

    def apply_theme(self):
        name = dict(THEME_NAMES).get(self.theme.get(), "light")
        palette = self.palette = appearance.apply_theme(self.root, name)
        for widget, foreground in self.header_widgets:
            widget.configure(bg=palette["header_bg"])
            if foreground:
                widget.configure(fg=palette[foreground])
        self.root.option_add("*TCombobox*Listbox.background", palette["field"])
        self.root.option_add("*TCombobox*Listbox.foreground", palette["ink"])
        if self.notes is not None and self.notes.winfo_exists():
            self.notes.configure(background=palette["field"], foreground=palette["ink"],
                                 insertbackground=palette["ink"])
        self.paint_status()

    def paint_status(self):
        if self.palette:
            self.status_label.configure(foreground=self.palette["error"] if self.phase == "error"
                                        else self.palette["ink"])

    def set_status(self, text, phase=None):
        if phase is not None:
            self.phase = phase
        self.status.set(text)
        self.paint_status()

    def update_summary(self, *_):
        source, audio = self.source.get().strip(), self.audio.get().strip()
        self.asset_summary.set("Textures + sounds and music" if source and audio else "Textures" if source
                               else "Sounds and music" if audio else "No assets selected")
        self.source_name.set("Selected texture source: " + Path(source).name if source
                             else "Audio-only selection" if audio else "Choose textures, sounds, or both.")
        self.activation_summary.set("Use imported assets" if self.select.get() else "Keep current settings")
        self.look_summary.set("Crisp pixels" if self.look.get() == "crisp" else "Smooth")
        self.launch_note.set("Imported assets apply on your next launch." if self.select.get()
                             else "Current pack settings will stay unchanged.")
        ready = bool((source or audio) and self.sd.get().strip())
        enabled = not self.busy and (self.phase == "success" or ready)
        self.import_button.state(["!disabled"] if enabled else ["disabled"])

    def clear_result(self):
        for child in self.body.winfo_children():
            if child is not self.form:
                child.destroy()
        self.notes = None

    def choose_page(self):
        self.clear_result()
        self.body.rowconfigure(0, weight=1)
        self.body.rowconfigure(7, weight=0)
        self.form.grid(row=0, column=0, sticky="nsew")
        self.import_button.configure(text="Import assets")
        self.set_status("Choose your local assets and where CavEX is installed.", "form")
        self.update_summary()

    def primary_action(self):
        if self.phase == "success":
            self.choose_page()
        else:
            self.import_assets()

    def toggle_advanced(self):
        if self.advanced_open.get():
            self.advanced.grid()
        else:
            self.advanced.grid_remove()

    def browse_file(self):
        result = filedialog.askopenfilename(filetypes=[("Minecraft / texture packs", "*.zip *.jar"), ("All files", "*")])
        if result:
            self.source.set(result)

    def browse_folder(self):
        result = filedialog.askdirectory(title="Choose your texture pack folder")
        if result:
            self.source.set(result)

    def browse_audio(self):
        result = filedialog.askdirectory(title="Choose your Minecraft resources folder")
        if result:
            self.audio.set(result)

    def choose_card(self):
        result = filedialog.askdirectory(title="Choose the SD card root or CavEX PC folder")
        if result:
            self.sd.set(result)

    def run(self, operation, done):
        if self.busy:
            return
        self.busy = True
        self.disabled_widgets = []
        for widget in self.controls + [self.theme_choice, self.import_button]:
            if "disabled" not in widget.state():
                widget.state(["disabled"])
                self.disabled_widgets.append(widget)
        self.progress.configure(mode="indeterminate", value=0)
        self.progress.grid()
        self.progress.start(35)
        def worker():
            try:
                self.events.put((done, operation()))
            except Exception as exc:
                self.events.put((self.failure, {"errors": [core._error(exc)]}))
        threading.Thread(target=worker, daemon=True).start()

    def report_progress(self, event):
        # Called on the worker thread: only queue it; poll() touches Tk.
        self.events.put((None, event))

    def show_progress(self, event):
        phase = event["phase"]
        if phase == "audio":
            self.status.set("Converting sounds and music…")
            if str(self.progress.cget("mode")) != "indeterminate":
                self.progress.configure(mode="indeterminate", value=0)
                self.progress.start(35)
        elif phase in ("convert", "install"):
            action = "Converting textures" if phase == "convert" else "Copying and checking assets"
            self.status.set("{0}… {1}/{2}".format(action, event["current"], event["total"]))
            self.progress.stop()
            self.progress.configure(mode="determinate", maximum=max(1, event["total"]), value=event["current"])

    def poll(self):
        while True:
            try:
                callback, result = self.events.get_nowait()
            except queue.Empty:
                break
            if callback is None:
                self.show_progress(result)
                continue
            self.busy = False
            self.progress.stop()
            self.progress.grid_remove()
            for widget in self.disabled_widgets:
                if widget.winfo_exists():
                    widget.state(["!disabled"])
            self.disabled_widgets = []
            callback(result)
            self.update_summary()
        self.poll_id = self.root.after(80, self.poll)

    def failure(self, result):
        partial = result.get("path")
        self.set_status("Assets were copied, but activation did not finish. Your choices are kept; see the details."
                        if partial else "Import could not finish. Your choices are kept; fix the problem and try again.",
                        "error")
        lines = list(result.get("instructions", []))
        for error in result.get("errors", []):
            message = error.get("message", str(error))
            if error.get("path"):
                message += "\n" + str(error["path"])
            if message not in lines:
                lines.append(message)
        if partial:
            lines.append("Copied assets: " + str(partial))
        lines.extend(result.get("warnings", []))
        messagebox.showerror("Asset import", "\n\n".join(lines))

    def import_assets(self):
        if self.busy:
            return
        source, audio, destination = self.source.get().strip(), self.audio.get().strip(), self.sd.get().strip()
        if not source and not audio:
            messagebox.showinfo("Choose assets", "Choose a texture pack, a sounds folder, or both.")
            return
        if not destination:
            messagebox.showinfo("Choose CavEX", "Choose your SD card's top folder or CavEX PC folder.")
            return
        look, select = self.look.get(), self.select.get()
        self.set_status("Checking CavEX and preparing your assets…", "importing")
        def operation():
            core.validate_destination(destination, select)
            prepared = core.prepare(source or audio, self.workspace.name, look=look,
                                    resources_path=audio if source and audio else None,
                                    progress=self.report_progress)
            if not prepared["ok"]:
                return prepared
            has_audio = any(row.get("category") == "audio" for row in prepared["files"])
            result = core.install(prepared, destination, select=select, audio_select=has_audio,
                                  progress=self.report_progress)
            result["warnings"] = prepared.get("warnings", []) + result.get("warnings", [])
            skipped = [row["path"] for row in prepared["files"]
                       if row.get("status") in ("unsupported", "reserved")]
            if skipped:
                result["warnings"].append("Files not imported (unsupported or supplied by CavEX):\n" + "\n".join(skipped))
            result["imported"] = {
                category: sum(row.get("category") == category and
                              row.get("status") in ("converted", "composited", "transcoded")
                              for row in prepared["files"])
                for category in ("texture", "audio")
            }
            return result
        self.run(operation, self.installed)

    def installed(self, result):
        if not result["ok"]:
            self.failure(result)
            return
        old = result.get("old_imports") or []
        if old and result.get("path") and messagebox.askyesno(
                "Older imports",
                "Earlier imports of Beta 1.7.3 are still on the card:\n\n{0}\n\n"
                "Remove them? Your new import is kept.".format("\n".join(old))):
            try:
                core.remove_old_imports(Path(result["path"]).parent.parent, old)
                result["warnings"] = [w for w in result.get("warnings", [])
                                      if not w.startswith("Older imports of Beta 1.7.3")]
            except (core.PackError, OSError) as exc:
                result.setdefault("warnings", []).append("Older imports were not removed: {0}".format(exc))
        self.clear_result()
        self.form.grid_remove()
        page = self.body
        ttk.Label(page, text="Assets imported", style="Result.TLabel").grid(row=1, column=0, sticky="w", pady=(4, 0))
        text = ("Ready for your next CavEX launch." if result.get("selected")
                else "Imported without activation. Choose the assets in CavEX's pack settings.")
        ttk.Label(page, text=text, wraplength=760).grid(row=2, column=0, sticky="w", pady=(12, 6))
        counts = result["imported"]
        ttk.Label(page, text="{texture} textures · {audio} audio files".format(**counts)).grid(
            row=3, column=0, sticky="w")
        ttk.Label(page, text=str(result["path"]), style="Strong.TLabel", wraplength=820).grid(
            row=4, column=0, sticky="w", pady=(8, 12))
        ttk.Button(page, text="Open imported folder",
                   command=lambda path=result["path"]: self.open_imported_folder(path)).grid(
            row=5, column=0, sticky="w")
        warnings = result.get("warnings", [])
        self.body.rowconfigure(0, weight=0)
        self.body.rowconfigure(7, weight=1 if warnings else 0)
        if warnings:
            ttk.Label(page, text="Import notes", style="Strong.TLabel").grid(row=6, column=0, sticky="w", pady=(16, 6))
            report = ttk.Frame(page)
            report.grid(row=7, column=0, sticky="nsew", pady=(0, 4))
            report.columnconfigure(0, weight=1)
            report.rowconfigure(0, weight=1)
            self.notes = tk.Text(report, height=6, wrap="word", font=(FONT, 9), relief="flat",
                                 borderwidth=1, highlightthickness=1)
            scrollbar = ttk.Scrollbar(report, command=self.notes.yview)
            self.notes.configure(yscrollcommand=scrollbar.set)
            self.notes.grid(row=0, column=0, sticky="nsew")
            scrollbar.grid(row=0, column=1, sticky="ns")
            self.notes.insert("1.0", "\n\n".join(warnings))
            self.notes.configure(state="disabled")
            self.apply_theme()
        self.import_button.configure(text="Import again")
        self.set_status("Import checked. Your saves and original files are preserved.", "success")

    def open_imported_folder(self, path):
        folder = Path(path)
        try:
            if not folder.is_dir():
                messagebox.showwarning("Imported folder unavailable",
                                       "The imported folder is no longer at this location. "
                                       "Check whether it was moved or the drive was disconnected.\n\n" + str(folder))
                return
            target = str(folder.resolve())
            if sys.platform == "win32":
                os.startfile(target, "explore")
            else:
                command = "open" if sys.platform == "darwin" else "xdg-open"
                subprocess.Popen([command, target])
        except (OSError, subprocess.SubprocessError):
            messagebox.showwarning("Could not open imported folder",
                                   "The import is complete, but the folder could not be opened. "
                                   "Open this location in your file manager:\n\n" + str(folder))

    def close(self):
        if self.busy:
            messagebox.showinfo("Please wait", "The importer is finishing a file operation. Wait for it to finish before closing.")
            return
        self.root.after_cancel(self.poll_id)
        self.workspace.cleanup()
        self.root.destroy()


def main():
    root = tk.Tk()
    PackHelper(root)
    root.mainloop()
