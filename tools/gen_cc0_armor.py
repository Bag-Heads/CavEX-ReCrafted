#!/usr/bin/env python3
"""tools/gen_cc0_armor.py -- generate the 10 assets/armor/*.png armor-layer
textures from scratch as ORIGINAL, non-copyrighted art, dedicated to the
public domain (CC0 1.0 Universal). See assets/README.md's "armor/" entry
for the license/provenance statement this generator backs.

WHY: docs/m3_legal_pack_plan.md S3 flagged all 10 assets/armor/*.png as
SUSPECT Mojang rips (same source commit, bc90dc6, as the confirmed
creeper/char/pig rips) -- an unresolved provenance question that
tools/make_release.sh has, until now, handled by EXCLUDING the whole
armor/ directory from every release zip (see its ASSET_EXCLUDE_MATCH).
This script replaces the suspect files outright with drawn-from-scratch
art, so the exclusion can be lifted (done alongside this script, in
tools/make_release.sh).

UV LAYOUT (matches the CODE, not the old files): source/graphics/
render_model.c's render_model_box() hard-codes
    int sw = 256 / 64;
    int sh = 256 / 32;
for EVERY box it draws -- head/body/arm/leg alike, armor included (see
render_model_player()'s `armor_texture[][2]` bind-then-draw calls). That is
a literal "this texture is 64x32" assumption, independent of whatever
dimensions the bound texture actually has: it is not "read the PNG's own
width/height and scale accordingly", it is a fixed texel->normalized-UV
factor baked in as a compile-time constant. Confirmed by reading the
function, not assumed. Consequently the two 128x64 files that shipped
before this script (armor/iron_1.png, armor/iron_2.png -- 2x every other
armor file's 64x32) were ALREADY silently mis-mapped: their real content
only occupied the texture's top-left quadrant as far as this UV math is
concerned. This script generates all 10 files at 64x32, fixing that
long-standing dimension bug as a side effect of the legal replacement --
no source-code change was needed or made, since the code's own constants
already assume 64x32 uniformly.

The per-part UV rectangles below are derived directly from
render_model_player()'s render_model_box() calls (ivec2 origin, ivec3 box
== (dx, dy, dz)), which is itself the classic "unfolded cube net" layout
(top, bottom, back, front, right, left) used by every Beta 1.7.3 humanoid
model box -- the same template a Beta skin file (mob/char.png) already
uses for its own head/body/arm/leg regions, just wearing a bigger box:

    head :  origin=( 8, 8), box=(8, 8,  8)  (helmet;      layer 1 only)
    body :  origin=(20,20), box=(8, 4, 12)  (chest+legs;  both layers)
    arm  :  origin=(44,20), box=(4, 4, 12)  (chestplate;  layer 1 only)
    leg  :  origin=( 4,20), box=(4, 4, 12)  (leggings+boots; both layers)

Layer 1 (armor/<material>_1.png) is bound for the helmet, chestplate+arms,
and boots, so it needs all four regions painted. Layer 2
(armor/<material>_2.png) is bound only for leggings (body @ padding 0.5 +
legs @ padding 0.5), so only those two regions are painted there --
matching Beta's own armor_2.png convention of a leggings-only second
sheet. Everywhere else on each 64x32 canvas is left fully transparent
(alpha 0); render_model_box only ever samples inside these rectangles, so
nothing else is ever drawn from these files.

Usage:
    python3 tools/gen_cc0_armor.py [--out-dir assets/armor]

Requires Pillow (PIL). No Mojang pixels are read, referenced, or traced --
every pixel is generated from flat/shaded fills, plus a small procedural
ring pattern for the chainmail's mesh look.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write(
        "gen_cc0_armor.py: Pillow (PIL) is required -- pip install Pillow\n"
    )
    sys.exit(1)

WIDTH, HEIGHT = 64, 32

# Classic Beta humanoid box-net regions: name -> (u, v, dx, dy, dz).
# See the module docstring for where these come from.
REGIONS = {
    "head": (8, 8, 8, 8, 8),
    "body": (20, 20, 8, 4, 12),
    "arm": (44, 20, 4, 4, 12),
    "leg": (4, 20, 4, 4, 12),
}

# Which regions each layer file paints (see docstring: layer 1 = helmet +
# chestplate/arms + boots; layer 2 = leggings' body-overlay + legs only).
LAYER_REGIONS = {
    1: ("head", "body", "arm", "leg"),
    2: ("body", "leg"),
}


def clamp(v: float) -> int:
    return max(0, min(255, int(round(v))))


def shade(rgb: tuple[int, int, int], factor: float) -> tuple[int, int, int]:
    """Scale an RGB triple toward white (factor>1) or black (factor<1)."""
    r, g, b = rgb
    if factor >= 1.0:
        t = factor - 1.0
        return (clamp(r + (255 - r) * t), clamp(g + (255 - g) * t), clamp(b + (255 - b) * t))
    return (clamp(r * factor), clamp(g * factor), clamp(b * factor))


def draw_box_net(draw: "ImageDraw.ImageDraw", u: int, v: int, dx: int, dy: int,
                  dz: int, base: tuple[int, int, int]) -> None:
    """Paint the 6-face unfolded-cube-net for one render_model_box() call,
    matching its exact face placement (see the module docstring's derivation
    from source/graphics/render_model.c) so every drawn pixel lands inside
    what the renderer will actually sample. Faces get distinct flat shades
    (lighter top, darkest back) so the result reads as a simple shaded box,
    not a single flat rectangle."""
    faces = [
        # (x0, y0, x1, y1, shade_factor)
        (u, v - dy, u + dx, v, 1.35),  # top -- lightest
        (u + dx, v - dy, u + 2 * dx, v, 1.15),  # "bottom" cell (next to top)
        (u + dx + dy, v, u + 2 * dx + dy, v + dz, 0.55),  # back -- darkest
        (u, v, u + dx, v + dz, 1.0),  # front -- base color
        (u + dx, v, u + dx + dy, v + dz, 0.75),  # right side
        (u - dy, v, u, v + dz, 0.75),  # left side
    ]
    for x0, y0, x1, y1, factor in faces:
        if x1 <= x0 or y1 <= y0:
            continue
        draw.rectangle([x0, y0, x1 - 1, y1 - 1], fill=shade(base, factor) + (255,))


def add_chainmail_mesh(img: "Image.Image", u: int, v: int, dx: int, dy: int,
                        dz: int, ring_color: tuple[int, int, int]) -> None:
    """Overlay a small 2x2 ring-dot pattern across a box-net's bounding
    rectangle to read as chainmail rather than a flat grey plate. Original
    procedural pattern -- not traced from any real chainmail texture."""
    x0, y0 = u - dy, v - dy
    x1, y1 = u + 2 * dx + dy, v + dz
    if x1 <= x0 or y1 <= y0:
        return
    px = img.load()
    w, h = img.size
    for y in range(max(0, y0), min(h, y1)):
        for x in range(max(0, x0), min(w, x1)):
            if (x + y) % 2 == 0:
                r, g, b, a = px[x, y]
                if a == 0:
                    continue
                dr, dg, db = ring_color
                px[x, y] = (clamp((r + dr) / 2), clamp((g + dg) / 2), clamp((b + db) / 2), a)


# material -> (file stem, base RGB, is_chainmail)
MATERIALS = {
    "cloth": ((150, 98, 48), False),  # leather (Beta's internal name is "cloth")
    "chain": ((150, 150, 158), True),  # chainmail: grey mesh
    "iron": ((205, 205, 210), False),  # light grey
    "gold": ((250, 210, 55), False),  # yellow
    "diamond": ((70, 210, 205), False),  # cyan
}


def gen_one(material: str, base_rgb: tuple[int, int, int], is_chain: bool,
            layer: int) -> "Image.Image":
    img = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for region_name in LAYER_REGIONS[layer]:
        u, v, dx, dy, dz = REGIONS[region_name]
        draw_box_net(draw, u, v, dx, dy, dz, base_rgb)
        if is_chain:
            add_chainmail_mesh(img, u, v, dx, dy, dz, shade(base_rgb, 0.45))
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="assets/armor",
                     help="output directory (default: assets/armor)")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    written = []
    for material, (base_rgb, is_chain) in MATERIALS.items():
        for layer in (1, 2):
            img = gen_one(material, base_rgb, is_chain, layer)
            assert img.size == (WIDTH, HEIGHT)
            out_path = out_dir / f"{material}_{layer}.png"
            img.save(out_path)
            written.append(out_path)

    for p in written:
        print(f"[gen-cc0-armor] wrote {p} (64x32 RGBA)")
    print(f"[gen-cc0-armor] OK: {len(written)} files written to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
