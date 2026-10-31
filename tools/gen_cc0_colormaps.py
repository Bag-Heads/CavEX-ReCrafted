#!/usr/bin/env python3
"""tools/gen_cc0_colormaps.py -- generate assets/misc/{grasscolor,foliagecolor}.png
from scratch as ORIGINAL, non-copyrighted art, dedicated to the public domain
(CC0 1.0 Universal). See assets/README.md's entries for both files for the
license statement this generator backs.

WHY: the 2026-09-02 Isabella-II provenance audit (docs/asset_provenance_2026-09-02.md)
found both files 0% pixel-identical to an archived copy of the Isabella II
pack despite matching 256x256 dimensions, and neither has any other
documented CC0/CC-BY/GPLv2 credit entry -- i.e. their real origin was
untraceable. A first pass EXCLUDED them from the release zip
(tools/make_release.sh), but that is not actually safe: unlike
misc/water.png (an optional overlay explicitly `.data`-guarded and skipped
when absent, source/main.c's own comment says so), grasscolor.png/
foliagecolor.png ARE consumed on every render -- source/graphics/
biome_color.c's biome_color_init() loads them unconditionally at tex_init(),
and their absence makes sample() return 0xFFFFFF (pure white, no tint) for
EVERY grass/leaf face, which is a visibly broken render, not a graceful
"missing overlay". They must ship SOMETHING plausible, so they are
regenerated here instead of merely excluded, exactly like armor/*.png and
controls.png were in the same lane.

INDEX SEMANTICS (matches the CODE, not the old files): source/graphics/
biome_color.h documents `colour = map[cj << 8 | ci]`, and
source/worldgen/worldgen.c's worldgen_climate() is the authoritative source
of what (ci, cj) actually mean -- read directly, not assumed:

    double temp = wg->temp[i];           // per-column Beta temperature, 0..1
    double humi = wg->humi[i] * temp;    // Beta's own humidity*temperature product
    ci = (uint8_t)((1.0 - temp) * 255.0);   // x: HOT -> 0, COLD -> 255
    cj = (uint8_t)((1.0 - humi) * 255.0);   // y: WET(+hot) -> 0, DRY(or cold) -> 255

So x=0/y=0 (top-left) is the hottest, most humid corner (jungle-like); the
bottom row (y=255) is reached by any cold column regardless of humidity
(temp=0 forces humi*temp=0 -> cj=255), and in general cj >= ci always
(humi<=1 => humi*temp <= temp), so only the lower-left triangle (row y >=
column x) is ever actually sampled by the game -- the upper-right triangle
is dead space, filled here with the same gradient formula for a coherent
look rather than being left as a discontinuity.

Colours are an original 2D bilinear blend (own palette, not sampled or
traced from any real Minecraft/Isabella asset) across four corners defined
in terms of a "warmth" axis (w = 1 - ci/255, so w=1 is hottest) and a
"wetness" axis (m = 1 - cj/255, so m=1 is wettest+hottest, per the cj
formula above) -- i.e. exactly the temperature/humidity axes
source/graphics/biome_color.h describes, just expressed as 0..1 floats
instead of raw byte indices:

    grasscolor.png:   warm+dry -> olive/yellow-green
                       warm+wet -> vivid jungle green
                       cold+dry -> pale grayish-green
                       cold+wet -> deep saturated green
    foliagecolor.png: same 4 corners, shifted slightly darker/bluer
                       (Beta's own foliage ramp is a darker, cooler
                       relative of the grass ramp; matched here as a
                       deliberate, documented design choice, not a copy).

Usage:
    python3 tools/gen_cc0_colormaps.py [--out-dir assets/misc]

Requires Pillow (PIL). No Mojang/Isabella pixels are read, referenced, or
traced -- every pixel is computed from this file's own corner-colour
constants and a bilinear blend function.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("gen_cc0_colormaps.py: Pillow (PIL) is required -- pip install Pillow\n")
    sys.exit(1)

SIZE = 256


def clamp(v: float) -> int:
    return max(0, min(255, int(round(v))))


def bilerp(c00, c10, c01, c11, w: float, m: float) -> tuple[int, int, int]:
    """c00 = warm+dry (w=1,m=0), c10 = cold+dry (w=0,m=0),
    c01 = warm+wet (w=1,m=1), c11 = cold+wet (w=0,m=1).
    (Named by which corner they sit at in the w/m unit square.)"""
    top = tuple(a * w + b * (1 - w) for a, b in zip(c00, c10))
    bot = tuple(a * w + b * (1 - w) for a, b in zip(c01, c11))
    return tuple(clamp(t * (1 - m) + b * m) for t, b in zip(top, bot))


# Original corner palettes (own numbers, no sampling from any real asset).
GRASS_CORNERS = {
    "warm_dry": (180, 183, 74),   # olive/yellow-green
    "cold_dry": (132, 145, 112),  # pale grayish-green
    "warm_wet": (66, 158, 58),    # vivid jungle green
    "cold_wet": (41, 112, 71),    # deep saturated green
}
FOLIAGE_CORNERS = {
    "warm_dry": (112, 128, 48),   # darker olive
    "cold_dry": (86, 104, 96),    # muted blue-green
    "warm_wet": (42, 112, 66),    # deep jungle green
    "cold_wet": (26, 72, 82),     # dark blue-green
}


def make_colormap(corners: dict) -> "Image.Image":
    img = Image.new("RGB", (SIZE, SIZE))
    px = img.load()
    for cj in range(SIZE):       # y index (row)
        m = 1.0 - cj / 255.0     # wetness axis (1 - cj/255)
        for ci in range(SIZE):   # x index (column)
            w = 1.0 - ci / 255.0  # warmth axis (1 - ci/255)
            px[ci, cj] = bilerp(
                corners["warm_dry"], corners["cold_dry"],
                corners["warm_wet"], corners["cold_wet"],
                w, m,
            )
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="assets/misc",
                     help="output directory (default: assets/misc)")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    grass = make_colormap(GRASS_CORNERS)
    assert grass.size == (SIZE, SIZE)
    grass_path = out_dir / "grasscolor.png"
    grass.save(grass_path)
    print(f"[gen-cc0-colormaps] wrote {grass_path} ({SIZE}x{SIZE} RGB)")

    foliage = make_colormap(FOLIAGE_CORNERS)
    assert foliage.size == (SIZE, SIZE)
    foliage_path = out_dir / "foliagecolor.png"
    foliage.save(foliage_path)
    print(f"[gen-cc0-colormaps] wrote {foliage_path} ({SIZE}x{SIZE} RGB)")

    print("[gen-cc0-colormaps] OK: 2 files written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
