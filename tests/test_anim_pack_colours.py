#!/usr/bin/env python3
"""EAD-007: every pack's anim.png = Beta 1.7.3 motion in that pack's own colours.

Independent of tools/gen_anim_fx: the reference is the Beta-coloured sheet
packs/beta-base/anim.png (itself pinned texel-for-texel to the ports by
tests/test_anim_pack_art.c). For the Earth & Ash default (assets/anim.png, equal to
the E&A pack's own sheet) and the Isabella II pack, every texel of every animated
cell (portal, water, lava, fire, flowing water, fire+16; 28 frames) must have
  * exactly the Beta sheet's ALPHA (same motion, same Beta alpha), and
  * an RGB equal to the pack's ramp entry for the colour input c that the Beta
    texel encodes. c is recovered by inverting Beta's own colour formula on one
    channel; integer truncation makes that an interval, so a +-1 index tolerance
    is allowed around it.
Ice cells keep each pack's own ice: E&A's from full_effects.py, Isabella's
composited from its own terrain.png ice tile.
Run: python tests/test_anim_pack_colours.py
"""
import math
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
BETA = REPO / "packs/beta-base/anim.png"
PACKS = {
    "earth-and-ash": REPO / "assets/anim.png",
    "isabella-ii": REPO / "packs/isabella-ii/anim.png",
}
EA_PACK_SHEET = REPO / "artwork/cc0-pack/earth-and-ash-complete/anim.png"
COLUMNS = {0: "portal", 1: "water", 2: "lava", 3: "flame", 5: "water_flow", 6: "flame"}


def interval(effect, rgba):
    """Colour-input interval [lo, hi] that Beta's formula maps to this texel."""
    r, g, _b, _a = rgba
    if effect == "lava":                       # R = (int)(c*100 + 155)
        return (r - 155) / 100, (r - 154) / 100
    if effect == "flame":                      # R = (int)(c*155 + 100)
        return (r - 100) / 155, (r - 99) / 155
    if effect in ("water", "water_flow"):      # G = (int)(50 + c*64), c = v^2
        return (g - 50) / 64, (g - 49) / 64
    if effect == "portal":                     # R = (int)(i*i*200 + 55); R < 55 = byte wrap, i > 1
        if r < 55:
            return 1.0, 1.0
        return math.sqrt((r - 55) / 200), math.sqrt(max(0.0, (r - 54) / 200))
    raise ValueError(effect)


def index(c):
    return int(math.floor(min(1.0, max(0.0, c)) * 255 + 0.5))


def cells(col):
    for frame in range(28):
        c = col + (7 if frame >= 14 else 0)
        yield c * 18 + 3, (frame % 14) * 18 + 3


def check_pack(name, sheet_path, beta):
    sheet = Image.open(sheet_path).convert("RGBA")
    assert sheet.size == beta.size == (256, 256), (name, sheet.size)
    ramps = {}
    for effect in ("lava", "flame", "water", "water_flow", "portal"):
        raw = (REPO / "artwork/fx-ramps" / name / (effect + ".ramp")).read_bytes()
        assert len(raw) == 1024, (name, effect)
        ramps[effect] = [tuple(raw[i * 4:i * 4 + 3]) for i in range(256)]
    sp, bp = sheet.load(), beta.load()
    checked = 0
    for col, effect in COLUMNS.items():
        for ox, oy in cells(col):
            for y in range(16):
                for x in range(16):
                    got, ref = sp[ox + x, oy + y], bp[ox + x, oy + y]
                    assert got[3] == ref[3], (name, effect, ox, oy, x, y, "alpha", got, ref)
                    lo, hi = interval(effect, ref)
                    allowed = {ramps[effect][k] for k in range(max(0, index(lo) - 1),
                                                               min(255, index(hi) + 1) + 1)}
                    assert got[:3] in allowed, (name, effect, ox, oy, x, y, got, ref, lo, hi)
                    checked += 1
    return sheet, checked


def ice_cells(sheet):
    return [sheet.crop((ox, oy, ox + 16, oy + 16)).tobytes() for ox, oy in cells(4)]


def main():
    beta = Image.open(BETA).convert("RGBA")
    ea, n_ea = check_pack("earth-and-ash", PACKS["earth-and-ash"], beta)
    assert PACKS["earth-and-ash"].read_bytes() == EA_PACK_SHEET.read_bytes(), \
        "assets/anim.png must be the Earth & Ash pack's own anim.png"
    isa, n_isa = check_pack("isabella-ii", PACKS["isabella-ii"], beta)
    # Each pack's own ice, not Beta's and not each other's.
    ice = Image.open(REPO / "packs/isabella-ii/terrain.png").convert("RGBA").crop((48, 64, 64, 80)).tobytes()
    assert all(c == ice for c in ice_cells(isa)), "Isabella ice must be its own terrain ice tile"
    assert ice_cells(ea) != ice_cells(beta) and ice_cells(ea) != ice_cells(isa)
    # The colours really changed (a no-op ramp would pass the checks above vacuously).
    for sheet in (ea, isa):
        assert sheet.crop((2 * 18 + 3, 3, 2 * 18 + 19, 19)).tobytes() != beta.crop((2 * 18 + 3, 3, 2 * 18 + 19, 19)).tobytes()
    print(f"PASS test_anim_pack_colours: {n_ea} E&A + {n_isa} Isabella animated texels = Beta motion/alpha in pack ramps; own ice")
    return 0


if __name__ == "__main__":
    sys.exit(main())
