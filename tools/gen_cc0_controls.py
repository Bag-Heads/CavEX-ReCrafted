#!/usr/bin/env python3
"""tools/gen_cc0_controls.py -- generate assets/controls.png from scratch as
ORIGINAL, non-copyrighted art, dedicated to the public domain (CC0 1.0
Universal). See assets/README.md's "controls.png" entry for the license
statement this generator backs.

WHY: assets/README.md previously credited controls.png to WiiBrew wiki
contributors (Clorox/Flark/Hiker13526/Crayon) under an "unknown" license --
unresolved provenance that tools/make_release.sh handled by EXCLUDING the
file from every release zip. This script replaces it outright with drawn-
from-scratch art (simple outlined button/stick glyphs + text labels, no
WiiBrew pixels read or traced), so the exclusion can be lifted (done
alongside this script, in tools/make_release.sh).

LAYOUT (matches the CODE, not a free-form "diagram" picture): controls.png
is not displayed as a single poster -- source/graphics/gui_util.c's
gutil_control_icon() binds it as an 8-col x 8-row grid of 32x32 glyph
CELLS and blits exactly one cell per on-screen button hint:

    gutil_texquad(x, ..., (symbol_help % 8) * 32, (symbol_help / 8) * 32,
                  32, 32, scale, scale);

real image size is 256x256 (gui_util.c's own atlas-layout comment). Cell
index = symbol_help, whose values come from source/platform/input.c's
input_native_key_symbol()/input_symbol() -- read directly to build the
INDEX -> (label, family) table below, rather than guessing. Replacing the
file with an unrelated full-page diagram would still be "an original
controls diagram" in spirit, but every existing per-button hint in the
game's GUI would start blitting the WRONG 32x32 slice of it (a real
functional regression) -- so this generator draws one simple outlined
glyph + text label PER CELL, at the exact index the code already expects,
which satisfies "labelled buttons, text labels are fine" (docs/
wii_controller_playability.md's control scheme) while keeping every
existing on-screen hint correct.

Usage:
    python3 tools/gen_cc0_controls.py [--out assets/controls.png]

Requires Pillow (PIL). No WiiBrew/Nintendo pixels are read, referenced, or
traced -- every pixel is generated from flat outlines + PIL's built-in
bitmap font for the 1-4 character labels.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.stderr.write(
        "gen_cc0_controls.py: Pillow (PIL) is required -- pip install Pillow\n"
    )
    sys.exit(1)

GRID_COLS, GRID_ROWS = 8, 8
CELL = 32
SIZE = GRID_COLS * CELL  # 256

# Family -> outline/fill colour (original palette, not traced from any
# existing controller-glyph set). Kept high-contrast against the dark
# in-game HUD background gutil_control_icon() draws these over.
FAMILY_COLOR = {
    "wiimote": (235, 235, 235),   # near-white
    "nunchuk": (90, 200, 235),    # cyan-blue
    "classic": (240, 150, 60),    # orange
    "gc": (170, 120, 235),        # purple
    "guitar": (110, 220, 110),    # green
    "generic": (200, 200, 200),   # neutral grey (D-pad / reserved)
}

# index -> (label, family, shape). shape in {"circle", "round", "diamond",
# "stick"}. Derived directly from source/platform/input.c's symbol tables
# (input_native_key_symbol / the `symbols[]` array) -- see the module
# docstring. Indices not present here (16, 20, 21, 25-31, 44-63) are either
# genuinely unused by the code (symbol_help collapses 25-28 to 24 before
# any icon lookup, so 25-28 are never actually blitted) or reserved for
# future controller families; they are left as a faint "reserved" cell
# rather than invented labels.
#
# 42/43 (2026-09-03, task_0d54671b root cause): PLATFORM_PC's
# input_native_key_symbol had returned the SAME hard-coded index (7 --
# this table's Guitar Hero fret cell) for every keyboard key and mouse
# button since the function was first written [fa65056c, 2023-07-09,
# before this atlas existed] -- every PC control hint on every screen
# drew a Guitar Hero fret icon, the "wrong glyph" bug. These two cells
# are the fix's other half: a generic keyboard-key glyph and a generic
# mouse-click glyph, distinct from every controller family (input.c
# picks 42 for key < 1000, 43 for the 1000+ mouse-button range).
CELL_KEY_KEYBOARD = 42
CELL_KEY_MOUSE = 43
CELLS: dict[int, tuple[str, str, str]] = {
    0: ("A", "wiimote", "circle"),
    1: ("B", "wiimote", "round"),  # trigger, underside of the remote
    2: ("1", "wiimote", "circle"),
    3: ("2", "wiimote", "circle"),
    4: ("HOME", "generic", "round"),  # shared Wiimote/Classic Home
    5: ("+", "generic", "round"),  # shared Wiimote/Classic/GH Plus
    6: ("-", "generic", "round"),  # shared Wiimote/Classic/GH Minus
    7: ("FRET", "guitar", "round"),  # Guitar Hero fret/strum, collapsed
    8: ("Z", "nunchuk", "round"),
    9: ("C", "nunchuk", "circle"),
    10: ("A", "classic", "circle"),
    11: ("B", "classic", "circle"),
    12: ("X", "classic", "circle"),
    13: ("Y", "classic", "circle"),
    14: ("ZL", "classic", "round"),
    15: ("ZR", "classic", "round"),
    17: ("STK", "nunchuk", "stick"),
    18: ("LST", "classic", "stick"),
    19: ("RST", "classic", "stick"),
    22: ("L", "classic", "round"),
    23: ("R", "classic", "round"),
    24: ("DP", "generic", "diamond"),
    32: ("A", "gc", "circle"),
    33: ("B", "gc", "circle"),
    34: ("X", "gc", "circle"),
    35: ("Y", "gc", "circle"),
    36: ("Z", "gc", "round"),
    37: ("L", "gc", "round"),
    38: ("R", "gc", "round"),
    39: ("STR", "gc", "round"),
    40: ("STK", "gc", "stick"),
    41: ("CST", "gc", "stick"),
    CELL_KEY_KEYBOARD: ("KEY", "generic", "round"),
    CELL_KEY_MOUSE: ("MSE", "generic", "round"),
}


def _font(size: int) -> "ImageFont.ImageFont":
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        # Older Pillow: load_default() takes no size argument.
        return ImageFont.load_default()


def draw_cell(draw: "ImageDraw.ImageDraw", cx0: int, cy0: int, label: str,
              family: str, shape: str) -> None:
    color = FAMILY_COLOR[family]
    pad = 4
    x0, y0 = cx0 + pad, cy0 + pad
    x1, y1 = cx0 + CELL - pad, cy0 + CELL - pad

    if shape == "circle":
        draw.ellipse([x0, y0, x1, y1], outline=color, width=2)
    elif shape == "round":
        draw.rounded_rectangle([x0, y0, x1, y1], radius=6, outline=color, width=2)
    elif shape == "diamond":
        mx, my = (x0 + x1) // 2, (y0 + y1) // 2
        draw.polygon([(mx, y0), (x1, my), (mx, y1), (x0, my)], outline=color, width=2)
        # Four short arrow ticks pointing out of the diamond's points, so the
        # collapsed generic D-pad glyph reads as "4-way" without needing the
        # label text to also fit inside the narrow diamond interior.
        tick = 3
        draw.line([mx, y0, mx, y0 - tick], fill=color, width=1)
        draw.line([mx, y1, mx, y1 + tick], fill=color, width=1)
        draw.line([x0, my, x0 - tick, my], fill=color, width=1)
        draw.line([x1, my, x1 + tick, my], fill=color, width=1)
    elif shape == "stick":
        draw.ellipse([x0, y0, x1, y1], outline=color, width=2)
        # A small notch at the top (rather than a center dot, which would
        # collide with the centered text label below) to read as "analog
        # stick" without covering the letters.
        mx = (x0 + x1) // 2
        draw.line([mx, y0 + 2, mx, y0 + 6], fill=color, width=2)

    font_size = 11 if len(label) <= 2 else (9 if len(label) <= 4 else 7)
    font = _font(font_size)
    bbox = draw.textbbox((0, 0), label, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tx = cx0 + CELL / 2 - tw / 2 - bbox[0]
    ty = cy0 + CELL / 2 - th / 2 - bbox[1]
    draw.text((tx, ty), label, fill=color, font=font)


def draw_reserved(draw: "ImageDraw.ImageDraw", cx0: int, cy0: int) -> None:
    """A faint corner tick so an unused cell is visibly "empty by design",
    not a rendering failure, without drawing anything a real icon lookup
    could mistake for content."""
    c = (70, 70, 70)
    draw.line([cx0 + 6, cy0 + 6, cx0 + 10, cy0 + 6], fill=c, width=1)
    draw.line([cx0 + 6, cy0 + 6, cx0 + 6, cy0 + 10], fill=c, width=1)


def build_image() -> "Image.Image":
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for row in range(GRID_ROWS):
        for col in range(GRID_COLS):
            idx = row * GRID_COLS + col
            cx0, cy0 = col * CELL, row * CELL
            if idx in CELLS:
                label, family, shape = CELLS[idx]
                draw_cell(draw, cx0, cy0, label, family, shape)
            else:
                draw_reserved(draw, cx0, cy0)
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="assets/controls.png",
                     help="output path (default: assets/controls.png)")
    args = ap.parse_args()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    img = build_image()
    assert img.size == (SIZE, SIZE)
    # GameCube cells 32-41 carry the approved style C artwork (CC0), shared
    # with gui/approved-controls.png; see tools/apply_gc_glyph_style_c.py.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from apply_gc_glyph_style_c import paste_style_c
    paste_style_c(img)
    img.save(out_path)
    print(f"[gen-cc0-controls] wrote {out_path} ({SIZE}x{SIZE} RGBA, "
          f"{len(CELLS)} labelled cells of {GRID_COLS*GRID_ROWS})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
