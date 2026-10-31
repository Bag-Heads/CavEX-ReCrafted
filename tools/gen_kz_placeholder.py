#!/usr/bin/env python3
"""
gen_kz_placeholder.py -- generate the ORIGINAL / CC0 bundled default painting
motive atlas (assets/art/kz.png) for CaveWii_v2.

Deferred item #1 (PROJECT_MEMORY 2026-09-01): paintings render invisible with
the bundled default texture pack because no art/kz.png ships in this repo --
Beta's real one is Mojang IP and is never copied here (guard-ip hook fails
closed on that). This script instead PROCEDURALLY draws an original 256x256
RGBA replacement atlas: every one of the 25 EnumArt tiles [740c583 EnumArt,
cross-checked against g_painting_art in source/entity/entity_painting.c,
which is the single source of truth for the offset/size table below -- keep
the two in sync if EnumArt ever changes] gets its OWN simple geometric
motif (flat fills, gradients, and a frame border) at its EXACT pixel
offset/size, so entity_painting_render's existing UV math (art->atlas_u_px/
atlas_v_px .. +size_x_px/+size_y_px) lands on real, distinguishable pixels.
Also paints the generic back/side frame-border strip Beta reuses for the
5 non-front faces [740c583 RenderPainting.func_158_a/func_159_a; the strip
sits at kz.png pixels ~[192,0]-[208,16], a region no EnumArt tile occupies --
verified against the offset table below].

Every pixel here is generated from this script's own color/shape choices.
Zero Mojang bytes; zero tracing of any real kz.png (none is even present in
this repo to trace from -- see texture.c's own note). Dedicated to the
public domain (CC0 1.0 Universal), matching this repo's existing
tools/make_default_pack.py precedent (assets/README.md's "mob/wolf*.png"
and "sound/" entries) for procedurally-authored placeholder art.

Usage:
  python tools/gen_kz_placeholder.py            # writes assets/art/kz.png
  python tools/gen_kz_placeholder.py --out DIR  # write to DIR/art/kz.png instead
"""
import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow (PIL) required: pip install pillow")

ATLAS_SIZE = 256

# Beta EnumArt [740c583], verbatim: (name, sizeX, sizeY, offsetX, offsetY),
# all in pixels. MUST stay byte-identical (same order, same values) to
# g_painting_art in source/entity/entity_painting.c -- that array's own
# comment cites this same decompile table as its source of truth.
ENUM_ART = [
    ("Kebab", 16, 16, 0, 0),
    ("Aztec", 16, 16, 16, 0),
    ("Alban", 16, 16, 32, 0),
    ("Aztec2", 16, 16, 48, 0),
    ("Bomb", 16, 16, 64, 0),
    ("Plant", 16, 16, 80, 0),
    ("Wasteland", 16, 16, 96, 0),
    ("Pool", 32, 16, 0, 32),
    ("Courbet", 32, 16, 32, 32),
    ("Sea", 32, 16, 64, 32),
    ("Sunset", 32, 16, 96, 32),
    ("Creebet", 32, 16, 128, 32),
    ("Wanderer", 16, 32, 0, 64),
    ("Graham", 16, 32, 16, 64),
    ("Match", 32, 32, 0, 128),
    ("Bust", 32, 32, 32, 128),
    ("Stage", 32, 32, 64, 128),
    ("Void", 32, 32, 96, 128),
    ("SkullAndRoses", 32, 32, 128, 128),
    ("Fighters", 64, 32, 0, 96),
    ("Pointer", 64, 64, 0, 192),
    ("Pigscene", 64, 64, 64, 192),
    ("BurningSkull", 64, 64, 128, 192),
    ("Skeleton", 64, 48, 192, 64),
    ("DonkeyKong", 64, 48, 192, 112),
]
assert len(ENUM_ART) == 25

# Generic frame border-strip patch [740c583 RenderPainting's back/side
# faces]: a 16x16 region no EnumArt tile above occupies (every offsetY=0
# tile stops at x=112; every other tile's y range starts >=32).
BACK_STRIP = (192, 0, 16, 16)


def clamp8(v):
    return max(0, min(255, int(round(v))))


def hashshade(x, y, amp):
    """Deterministic, seedless +/-amp dither (tools/make_default_pack.py's
    own helper, reused verbatim) so flat fills read as painted, not flat."""
    h = (x * 73856093) ^ (y * 19349663)
    return ((h >> 4) & 0xFF) / 255.0 * (2 * amp) - amp


def shade(c, d):
    return (clamp8(c[0] + d), clamp8(c[1] + d), clamp8(c[2] + d), 255)


def hsv_to_rgb(h, s, v):
    """h in [0,1), s/v in [0,1]. Plain HSV->RGB, no external deps."""
    i = int(h * 6.0)
    f = h * 6.0 - i
    p = v * (1.0 - s)
    q = v * (1.0 - f * s)
    t = v * (1.0 - (1.0 - f) * s)
    i %= 6
    r, g, b = [
        (v, t, p), (q, v, p), (p, v, t),
        (p, q, v), (t, p, v), (v, p, q),
    ][i]
    return (clamp8(r * 255), clamp8(g * 255), clamp8(b * 255))


def put(px, x, y, w, h, color, ax, ay):
    """Set one pixel at atlas-absolute (ax+x, ay+y) if inside the tile."""
    if 0 <= x < w and 0 <= y < h:
        px[ax + x, ay + y] = color


# --- per-tile motifs ----------------------------------------------------
# Each draws INTO the tile-local (w x h) box; the caller offsets by (ax, ay).
# Picking motif = index % len(MOTIFS) keeps every one of the 25 tiles visibly
# distinct from its neighbours even where sizes repeat.

def motif_diagonal_stripes(px, w, h, ax, ay, base, accent):
    for y in range(h):
        for x in range(w):
            stripe = ((x + y) // 3) % 2
            c = accent if stripe else base
            put(px, x, y, w, h, shade(c, hashshade(x, y, 10)), ax, ay)


def motif_concentric_rings(px, w, h, ax, ay, base, accent):
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    r_max = max(cx, cy) + 1
    for y in range(h):
        for x in range(w):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            ring = int((d / r_max) * 5) % 2
            c = accent if ring else base
            put(px, x, y, w, h, c, ax, ay)


def motif_diamond(px, w, h, ax, ay, base, accent):
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    for y in range(h):
        for x in range(w):
            d = abs(x - cx) / (w / 2.0) + abs(y - cy) / (h / 2.0)
            c = accent if d < 0.6 else base
            put(px, x, y, w, h, shade(c, hashshade(x, y, 8)), ax, ay)


def motif_checker(px, w, h, ax, ay, base, accent):
    cell = max(2, min(w, h) // 4)
    for y in range(h):
        for x in range(w):
            c = accent if ((x // cell) + (y // cell)) % 2 else base
            put(px, x, y, w, h, c, ax, ay)


def motif_vertical_gradient(px, w, h, ax, ay, base, accent):
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(clamp8(base[i] * (1 - t) + accent[i] * t) for i in range(3)) + (255,)
        for x in range(w):
            put(px, x, y, w, h, shade(c, hashshade(x, y, 6)), ax, ay)


def motif_cross(px, w, h, ax, ay, base, accent):
    cx, cy = w // 2, h // 2
    bar_w = max(1, min(w, h) // 4)
    for y in range(h):
        for x in range(w):
            on_cross = abs(x - cx) < bar_w or abs(y - cy) < bar_w
            c = accent if on_cross else base
            put(px, x, y, w, h, c, ax, ay)


MOTIFS = [
    motif_diagonal_stripes,
    motif_concentric_rings,
    motif_diamond,
    motif_checker,
    motif_vertical_gradient,
    motif_cross,
]


def draw_frame_border(px, w, h, ax, ay, border_color):
    """A 1px darker border so the motive reads as a framed painting, matching
    Beta's own RenderPainting look of a thin frame edge around the motive."""
    for x in range(w):
        put(px, x, 0, w, h, border_color, ax, ay)
        put(px, x, h - 1, w, h, border_color, ax, ay)
    for y in range(h):
        put(px, 0, y, w, h, border_color, ax, ay)
        put(px, w - 1, y, w, h, border_color, ax, ay)


def draw_tile(px, index, name, w, h, ax, ay):
    hue = (index * 0.13) % 1.0
    base = hsv_to_rgb(hue, 0.55, 0.72) + (255,)
    accent = hsv_to_rgb((hue + 0.5) % 1.0, 0.6, 0.85) + (255,)
    border = shade(base, -70)

    motif = MOTIFS[index % len(MOTIFS)]
    motif(px, w, h, ax, ay, base, accent)
    if w >= 4 and h >= 4:
        draw_frame_border(px, w, h, ax, ay, border)


def draw_back_strip(px):
    """Generic border-strip patch for the back/side faces -- a plain wood-
    frame tone with the same darker-edge treatment as every front tile."""
    x0, y0, w, h = BACK_STRIP
    wood = (140, 100, 62, 255)
    for y in range(h):
        for x in range(w):
            put(px, x, y, w, h, shade(wood, hashshade(x, y, 10)), x0, y0)
    draw_frame_border(px, w, h, x0, y0, shade(wood, -60))


def build_atlas():
    im = Image.new("RGBA", (ATLAS_SIZE, ATLAS_SIZE), (0, 0, 0, 0))
    px = im.load()
    for i, (name, sx, sy, ox, oy) in enumerate(ENUM_ART):
        assert ox + sx <= ATLAS_SIZE and oy + sy <= ATLAS_SIZE, name
        draw_tile(px, i, name, sx, sy, ox, oy)
    draw_back_strip(px)
    return im


def save(im, out):
    path = os.path.join(out, "art", "kz.png")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
    print(f"[gen_kz_placeholder] wrote {path}  ({im.width}x{im.height})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "..", "assets"),
        help="assets root to write art/kz.png under (default: ../assets)")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    print(f"[gen_kz_placeholder] writing CC0 painting atlas -> {out}")
    im = build_atlas()
    save(im, out)
    print("[gen_kz_placeholder] done. (CC0 / public domain -- zero Mojang bytes)")


if __name__ == "__main__":
    main()
