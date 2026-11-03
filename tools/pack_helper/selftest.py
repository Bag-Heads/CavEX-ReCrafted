"""A packaged-app smoke check; does not touch a real SD card or owned pack."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import tkinter as tk
import wave
from PIL import Image
from . import core


REQUIRED_RESOURCES = {
    "mapping.json",
    "conversion_map.json",
    "assets/gui_2.png",
    "beta-base/anim.png",
    "beta-base/pointer.png",
    "branding/logo.png",
    "branding/computer.png",
    "branding/computer.ico",
    # L9: a bundle that ships the project's assets ships its license and
    # attributions (build_resources.REQUIRED_LEGAL_FILES puts them here).
    "LICENSE",
    "NOTICE",
    "LICENSES/GPL-2.0.txt",
}


def _verify_resources(resources):
    resources = Path(resources)
    manifest_path = resources / "resource_hashes.json"
    hashes = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(hashes, dict) or not all(
            isinstance(path, str) and isinstance(digest, str) and len(digest) == 64
            for path, digest in hashes.items()):
        raise ValueError("Bundled resource hash inventory is invalid")
    entries = list(resources.rglob("*"))
    if any(path.is_symlink() for path in entries):
        raise ValueError("Bundled resources may not contain symbolic links")
    actual = {path.relative_to(resources).as_posix() for path in entries if path.is_file()}
    expected = set(hashes) | {"resource_hashes.json"}
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ValueError("Bundled resource inventory differs (missing={0}, extra={1})".format(
            missing, extra))
    required = set(REQUIRED_RESOURCES)
    native_name = "sound_transcode.exe" if "sound_transcode.exe" in hashes else "sound_transcode"
    required.add(native_name)
    absent = sorted(required - set(hashes))
    if absent:
        raise ValueError("Required bundled resource is missing: " + absent[0])
    for relative, digest in hashes.items():
        path = resources / Path(*relative.split("/"))
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError("Bundled resource differs: " + relative)
    mapping = json.loads((resources / "mapping.json").read_text(encoding="utf-8"))
    if "step.grass" not in mapping.get("sound_keys", ()):
        raise ValueError("Bundled mapping lacks the step.grass sound key")
    if not {"13", "cat"}.issubset(set(mapping.get("records", ()))):
        raise ValueError("Bundled mapping lacks the Beta record names")
    return resources / native_name


def _write_wav(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = [((index % 16) - 8) * 256 for index in range(128)]
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(11025)
        stream.writeframes(struct.pack("<{0}h".format(len(samples)), *samples))


def _write_synthetic_source(folder):
    (folder / "gui").mkdir(parents=True)
    Image.new("RGBA", (512, 512), (173, 116, 42, 255)).save(folder / "terrain.png")
    Image.new("RGBA", (512, 512), (45, 67, 89, 255)).save(folder / "gui/gui.png")
    Image.new("RGBA", (512, 512), (210, 220, 230, 255)).save(folder / "gui/icons.png")
    Image.new("RGBA", (512, 512), (120, 80, 40, 255)).save(folder / "gui/items.png")


def run(report):
    result = {"ok": False, "checks": [], "errors": []}
    try:
        resources = Path(__file__).parent / "resources"
        native = _verify_resources(resources)
        result["checks"].append("exact bundled resource inventory and hashes")
        subprocess.run([str(native), "--manifest"], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, check=True)
        result["checks"].append("native audio converter")
        root = tk.Tk()
        root.withdraw()
        from .gui import PackHelper
        app = PackHelper(root)
        root.update_idletasks()
        app.close()
        result["checks"].append("Tk interface and Pillow imports")
        with tempfile.TemporaryDirectory(prefix="cw-helper-selftest-") as tmp:
            tmp = Path(tmp)
            folder = tmp / "source"
            audio = tmp / "resources"
            folder.mkdir()
            _write_synthetic_source(folder)
            _write_wav(audio / "sound/step/grass1.wav")
            prepared = core.prepare(folder, tmp / "prepared", resources_path=audio)
            if not prepared["ok"]:
                raise ValueError(str(prepared["errors"]))
            output = Path(prepared["path"])
            expected = {"terrain.png", "gui/gui.png", "gui/icons.png", "gui/items.png",
                        "gui_2.png", "anim.png", "sound/step/grass1.wav",
                        # The packaged smoke must prove the optional composer
                        # works, rather than silently taking its warning path.
                        "gui/nether-background.png", "gui/nether-background.md"}
            written = {row["path"] for row in prepared["manifest"]["files"]}
            if not expected.issubset(written):
                raise ValueError("Synthetic prepare omitted: " + ", ".join(sorted(expected - written)))
            for relative in ("terrain.png", "gui_2.png", "anim.png"):
                with Image.open(output / relative) as image:
                    if image.size != (256, 256):
                        raise ValueError("Wrong prepared sheet size: " + relative)
            with Image.open(output / "gui/nether-background.png") as image:
                if image.size != (1024, 576):
                    raise ValueError("Wrong Nether loading scene size")
            with wave.open(str(output / "sound/step/grass1.wav"), "rb") as stream:
                if (stream.getnchannels(), stream.getsampwidth(), stream.getframerate()) != (1, 2, 11025):
                    raise ValueError("Native converter emitted an unexpected WAV format")
            result["checks"].append(
                "prepare without source checkout (terrain, GUI, animation, Nether scene, native WAV)")
            result["checks"].append(
                "record names verified in mapping (media playback not exercised)")
        result["ok"] = True
    except Exception as exc:
        result["errors"].append(str(exc))
    Path(report).write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0 if result["ok"] else 1
