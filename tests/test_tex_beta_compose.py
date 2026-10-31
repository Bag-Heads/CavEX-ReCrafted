"""Raw-jar texture packs: the loader's compositions equal the Pack Helper's.

A player who just unzips b1.7.3.jar into texturepacks/<name>/ gets the jar's
own layout. source/platform/tex_beta_compose.c builds the four sheets the
helper would have made (gui/chest.png from gui/container.png, gui_2.png,
anim.png; pointer.png is bundled-only). This test GENERATES a fixture jar
layout (seeded noise, never real Mojang files), runs the helper's own
tools/pack_helper/conversion.py functions on it exactly as core.prepare does
(convert_image at the default crisp look, then compose_gui2 / compose_anim on
the bundled bases), runs the production C code via tex_beta_compose_probe on
the same folder, and requires the decoded RGBA to be byte-identical -- at 1x,
2x and 4x. It also pins the gate: helper-built packs and Isabella II are NOT
raw jars, so they load exactly as before.

Usage: python -B tests/test_tex_beta_compose.py --probe <tex_beta_compose_probe>
"""

from __future__ import annotations

import argparse
import io
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image  # noqa: E402

from tools.pack_helper import conversion  # noqa: E402

GUI2_BASE = ROOT / "assets" / "gui_2.png"            # helper: bundled assets/gui_2.png
HELPER_ANIM_BASE = ROOT / "packs" / "beta-base" / "anim.png"   # helper: beta-base/anim.png
RUNTIME_ANIM_BASE = ROOT / "assets" / "beta-base" / "anim.png"  # what the game reads


def noise(size: int, seed: int, mode: str = "RGBA") -> bytes:
    rng = random.Random(seed)
    if mode == "P":
        image = Image.new("P", (size, size))
        image.putpalette([rng.randrange(256) for _ in range(768)])
        image.putdata([rng.randrange(256) for _ in range(size * size)])
        out = io.BytesIO()
        # A transparent palette entry, like Beta's paletted GUI sheets.
        image.save(out, format="PNG", transparency=0)
        return out.getvalue()
    image = Image.new("RGBA", (size, size))
    image.putdata([(rng.randrange(256), rng.randrange(256), rng.randrange(256),
                    rng.choice((0, 0, 255, 255, rng.randrange(256))))
                   for _ in range(size * size)])
    out = io.BytesIO()
    image.save(out, format="PNG")
    return out.getvalue()


def write_jar_layout(folder: Path, scale: int, seed: int) -> dict[str, bytes]:
    size = 256 * scale
    files = {
        "terrain.png": noise(size, seed + 1),
        "gui/gui.png": noise(size, seed + 2),
        "gui/icons.png": noise(size, seed + 3, "P"),
        "gui/container.png": noise(size, seed + 4),
    }
    for rel, data in files.items():
        path = folder / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    # The rest of a jar unzip: code and metadata the loader must ignore.
    (folder / "META-INF").mkdir()
    (folder / "META-INF" / "MANIFEST.MF").write_text("Manifest-Version: 1.0\n")
    (folder / "fixture.class").write_bytes(b"\xca\xfe\xba\xbe fixture")
    return files


def rgba(png: bytes) -> bytes:
    with Image.open(io.BytesIO(png)) as image:
        assert image.size == (256, 256), image.size
        return image.convert("RGBA").tobytes()


def helper_outputs(files: dict[str, bytes]) -> tuple[bytes, bytes, bytes]:
    """core.prepare's texture steps for these files, default look."""
    gui = conversion.convert_image(files["gui/gui.png"], "gui/gui.png", "crisp")
    icons = conversion.convert_image(files["gui/icons.png"], "gui/icons.png", "crisp")
    terrain = conversion.convert_image(files["terrain.png"], "terrain.png", "crisp")
    chest = conversion.convert_image(files["gui/container.png"], "gui/container.png", "crisp")
    gui2 = conversion.compose_gui2(GUI2_BASE.read_bytes(), gui, icons)
    anim = conversion.compose_anim(HELPER_ANIM_BASE.read_bytes(), terrain)
    return gui2, anim, chest


def probe(exe: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([exe, *args], capture_output=True, text=True)


def probe_rgba(exe: str, mode: str, pack: Path, base: Path, work: Path) -> bytes:
    out = work / f"{mode}.rgba"
    proc = probe(exe, mode, str(pack), str(base), str(out))
    assert proc.returncode == 0, f"{mode}: exit {proc.returncode}\n{proc.stderr}"
    return out.read_bytes()


def first_diff(a: bytes, b: bytes) -> str:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            px = i // 4
            return f"first differing byte {i} (pixel x={px % 256} y={px // 256})"
    return f"length {len(a)} vs {len(b)}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    exe = parser.parse_args().probe

    # The runtime base is the helper's base, byte for byte.
    assert RUNTIME_ANIM_BASE.read_bytes() == HELPER_ANIM_BASE.read_bytes(), \
        "assets/beta-base/anim.png must be a byte copy of packs/beta-base/anim.png"

    with tempfile.TemporaryDirectory(prefix="cw-rawjar-") as tmp:
        tmp_path = Path(tmp)
        for scale in (1, 2, 4):
            pack = tmp_path / f"b1.7.3-x{scale}"
            files = write_jar_layout(pack, scale, seed=1730 + scale)
            want_gui2, want_anim, want_chest = helper_outputs(files)
            work = tmp_path / f"out{scale}"
            work.mkdir()

            got = probe_rgba(exe, "gui2", pack, ROOT / "assets", work)
            want = rgba(want_gui2)
            assert got == want, f"gui_2 x{scale}: {first_diff(got, want)}"
            got = probe_rgba(exe, "anim", pack, RUNTIME_ANIM_BASE, work)
            want = rgba(want_anim)
            assert got == want, f"anim x{scale}: {first_diff(got, want)}"
            # The chest: the loader reads gui/container.png as-is; the helper
            # at 1x passes the same bytes through (HD: both keep the sheet's
            # own size -- the helper reduces it, the loader samples it with
            # the same normalized UVs, as for any HD gui/chest.png).
            if scale == 1:
                assert want_chest == files["gui/container.png"]
            proc = probe(exe, "rawjar", str(pack))
            assert proc.returncode == 0 and proc.stdout.strip() == "1", proc
            print(f"PASS: x{scale} raw jar -> gui_2.png and anim.png byte-identical to the Pack Helper")

        # A sheet the helper refuses (not a whole multiple) is refused here
        # too: NULL -> the bundled sheet, never a guessed resample.
        odd = tmp_path / "odd"
        files = write_jar_layout(odd, 1, seed=99)
        Image.new("RGBA", (300, 300)).save(odd / "gui" / "gui.png")
        try:
            conversion.convert_image((odd / "gui" / "gui.png").read_bytes(), "gui/gui.png")
        except conversion.ConversionError:
            pass
        else:
            raise AssertionError("helper accepted a 300x300 gui.png")
        proc = probe(exe, "gui2", str(odd), str(ROOT / "assets"), str(tmp_path / "odd.rgba"))
        assert proc.returncode == 2, (proc.returncode, proc.stderr)
        print("PASS: an unsupported sheet size keeps the bundled gui_2.png, like the helper's refusal")

        # The gate: only the jar layout takes the new path.
        helper_pack = tmp_path / "helper-built"
        (helper_pack / "gui").mkdir(parents=True)
        (helper_pack / "gui" / "chest.png").write_bytes(noise(256, 5))
        both = tmp_path / "both"
        write_jar_layout(both, 1, seed=7)
        (both / "gui" / "chest.png").write_bytes(noise(256, 8))
        for pack in (helper_pack, both, ROOT / "packs" / "isabella-ii", ROOT / "assets",
                     tmp_path / "missing"):
            proc = probe(exe, "rawjar", str(pack))
            assert proc.returncode == 0 and proc.stdout.strip() == "0", (pack, proc)
        print("PASS: helper-built packs, Isabella II and the default are not raw jars (unchanged load)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
