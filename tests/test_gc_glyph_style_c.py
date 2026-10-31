#!/usr/bin/env python3
"""test_gc_glyph_style_c -- the approved GameCube glyphs (style C, 12px fixes)
are what both controller atlases draw for cells 32-41.

  1. The CC0 artwork files match the manifest hashes.
  2. gui/approved-controls.png and controls.png (its fallback in
     gutil_glyph_texture) hold exactly that artwork in cells 32-41.
  3. tools/gen_cc0_controls.py regenerates controls.png byte-for-byte, so a
     regeneration can never silently drop the new cells.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from PIL import Image  # noqa: E402

import apply_gc_glyph_style_c as gc  # noqa: E402


def main():
    cells = gc.load_style_c()  # raises on a hash mismatch
    assert sorted(cells) == list(range(32, 42))
    for rel in gc.ATLASES:
        atlas = Image.open(ROOT / rel).convert("RGBA")
        assert atlas.size == (256, 256), rel
        for cell, img in cells.items():
            got = atlas.crop(gc.cell_box(cell)).tobytes()
            assert got == img.tobytes(), f"{rel}: cell {cell} is not style C"
        print(f"PASS {rel}: cells 32-41 = style C")

    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "controls.png"
        subprocess.run([sys.executable, str(ROOT / "tools/gen_cc0_controls.py"),
                        "--out", str(out)], check=True, stdout=subprocess.DEVNULL)
        assert out.read_bytes() == (ROOT / "assets/controls.png").read_bytes(), \
            "gen_cc0_controls.py no longer reproduces assets/controls.png"
    print("PASS gen_cc0_controls.py reproduces assets/controls.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
