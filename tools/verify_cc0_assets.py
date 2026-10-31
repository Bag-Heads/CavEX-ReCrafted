#!/usr/bin/env python3
"""tools/verify_cc0_assets.py -- load-and-shape verification for the
generated assets/armor/*.png (tools/gen_cc0_armor.py) and assets/controls.png
(tools/gen_cc0_controls.py). Not wired into CTest (no existing armor/
controls-loading PC test exists to extend -- tests/ has none, per a `grep -ri
armor tests/` sweep -- and this project's CMakeLists test-count gate plus the
Python3-availability gating around test_make_release.py made a full CTest
registration a bigger, riskier change than this task warrants for a load/
shape check). Run directly:

    python3 tools/verify_cc0_assets.py [--armor-dir assets/armor]
                                        [--controls assets/controls.png]

Exit 0 and prints "PASS" on success; exit 1 with an AssertionError otherwise.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("verify_cc0_assets.py: Pillow (PIL) is required -- pip install Pillow\n")
    sys.exit(1)

# Must match source/platform/texture.c's TEX_LOAD_FILE calls for armor
# (assets/armor/<material>_<layer>.png) exactly.
ARMOR_FILES = [
    f"{material}_{layer}.png"
    for material in ("chain", "cloth", "gold", "iron", "diamond")
    for layer in (1, 2)
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--armor-dir", default="assets/armor")
    ap.add_argument("--controls", default="assets/controls.png")
    args = ap.parse_args()

    armor_dir = Path(args.armor_dir)
    checked = 0

    for name in ARMOR_FILES:
        path = armor_dir / name
        if not path.is_file():
            raise AssertionError(f"missing armor file: {path}")
        img = Image.open(path)
        img.load()  # force full decode, not just header parse
        if img.size != (64, 32):
            raise AssertionError(f"{path}: expected 64x32, got {img.size}")
        if img.mode != "RGBA":
            raise AssertionError(f"{path}: expected RGBA, got {img.mode}")
        checked += 1
        print(f"PASS: {path} is 64x32 RGBA")

    controls_path = Path(args.controls)
    if not controls_path.is_file():
        raise AssertionError(f"missing controls file: {controls_path}")
    cimg = Image.open(controls_path)
    cimg.load()
    if cimg.size != (256, 256):
        raise AssertionError(f"{controls_path}: expected 256x256, got {cimg.size}")
    if cimg.mode != "RGBA":
        raise AssertionError(f"{controls_path}: expected RGBA, got {cimg.mode}")
    print(f"PASS: {controls_path} is 256x256 RGBA")
    checked += 1

    print(f"PASS verify_cc0_assets ({checked} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
