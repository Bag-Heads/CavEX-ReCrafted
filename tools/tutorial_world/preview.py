#!/usr/bin/env python3
"""
Phone-friendly PNG previews of a World or a save directory.

    python tools/tutorial_world/preview.py "saves/Tutorial Demo" \
        --out tmp/jobs/tutorial-1021/demo_top.png [--layers 64,65] [--scale 8]

Top-down: the highest non-air block of every column, one flat colour per
block id, shaded by height (higher = lighter) with faint chunk borders.
--layers Y,...: one extra PNG per Y (<out>_y<Y>.png) showing that slice
(air drawn dark, so walls/rooms read as a floor plan).
Uses Pillow when installed, otherwise a built-in stdlib PNG writer.
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import worldgen_lib as wl  # noqa: E402

# Flat colours per block id (approximate Beta palette, for review only).
COLOURS = {
    1: (125, 125, 125), 2: (95, 159, 53), 3: (134, 96, 67),
    4: (110, 110, 110), 5: (157, 128, 79), 6: (60, 140, 40),
    7: (40, 40, 40), 8: (48, 82, 205), 9: (48, 82, 205), 10: (230, 90, 10),
    11: (230, 90, 10), 12: (219, 211, 160), 13: (136, 126, 126),
    14: (180, 160, 90), 15: (160, 140, 120), 16: (80, 80, 80),
    17: (102, 81, 51), 18: (55, 120, 35), 20: (200, 230, 240),
    21: (40, 70, 150), 22: (30, 60, 160), 23: (100, 100, 100),
    24: (218, 210, 158), 25: (100, 70, 50), 26: (175, 30, 30),
    27: (200, 170, 60), 28: (150, 120, 100), 29: (150, 160, 90),
    31: (80, 150, 50), 33: (160, 140, 100), 34: (170, 150, 110),
    35: (230, 230, 230), 37: (240, 230, 40), 38: (220, 30, 30),
    39: (150, 110, 80), 40: (200, 40, 40), 41: (250, 210, 60),
    42: (220, 220, 220), 43: (165, 165, 165), 44: (175, 175, 175),
    45: (150, 75, 60), 46: (200, 60, 40), 47: (120, 90, 60),
    48: (90, 120, 90), 49: (20, 18, 30), 50: (255, 220, 80), 51: (255, 120, 0),
    52: (30, 50, 70), 53: (170, 140, 90), 54: (160, 110, 40),
    55: (190, 20, 20), 56: (100, 200, 200), 57: (110, 220, 220),
    58: (140, 100, 60), 59: (180, 170, 50), 60: (110, 70, 40),
    61: (90, 90, 90), 62: (220, 140, 60), 63: (190, 160, 100),
    64: (130, 95, 55), 65: (150, 120, 70), 66: (150, 130, 110),
    67: (115, 115, 115), 68: (190, 160, 100), 69: (120, 100, 70),
    70: (130, 130, 130), 71: (200, 200, 200), 72: (160, 130, 80),
    73: (130, 80, 80), 75: (120, 40, 30), 76: (255, 60, 40),
    77: (120, 120, 120), 78: (245, 250, 250), 79: (150, 180, 250),
    80: (240, 250, 250), 81: (20, 120, 30), 82: (160, 165, 180),
    83: (140, 190, 90), 84: (110, 75, 55), 85: (140, 115, 70),
    86: (220, 130, 20), 87: (110, 50, 50), 88: (85, 65, 50),
    89: (250, 220, 120), 91: (240, 170, 30), 92: (240, 240, 230),
    93: (150, 120, 120), 94: (210, 70, 60), 96: (130, 100, 60),
}
WOOL = [(233, 236, 236), (240, 118, 19), (189, 68, 179), (58, 175, 217),
        (248, 198, 39), (112, 185, 25), (237, 141, 172), (62, 68, 71),
        (142, 142, 134), (21, 137, 145), (121, 42, 172), (53, 57, 157),
        (114, 71, 40), (84, 109, 27), (161, 39, 34), (20, 21, 25)]
UNKNOWN = (255, 0, 255)
AIR_SLICE = (24, 24, 32)
VOID = (8, 8, 12)


def colour(bid, meta):
    if bid == 35:
        return WOOL[meta & 15]
    return COLOURS.get(bid, UNKNOWN)


def _bounds(world):
    xs = [c[0] for c in world.chunks]
    zs = [c[1] for c in world.chunks]
    return min(xs) * 16, min(zs) * 16, (max(xs) + 1) * 16, (max(zs) + 1) * 16


def _shade(rgb, f):
    return tuple(max(0, min(255, int(v * f))) for v in rgb)


def render(world, layer=None, scale=8, grid=True):
    """Return (width, height, rows) where rows is a list of bytearray RGB."""
    x0, z0, x1, z1 = _bounds(world)
    w, h = (x1 - x0) * scale, (z1 - z0) * scale
    rows = [bytearray(w * 3) for _ in range(h)]
    for z in range(z0, z1):
        for x in range(x0, x1):
            c = world.chunks.get((x >> 4, z >> 4))
            if c is None:
                rgb = VOID
            elif layer is None:
                rgb = VOID
                base = ((x & 15) << 11) | ((z & 15) << 7)
                for y in range(wl.WORLD_HEIGHT - 1, -1, -1):
                    b = c.blocks[base + y]
                    if b:
                        rgb = _shade(colour(b, c.meta[base + y]),
                                     0.55 + 0.45 * (y / 127.0) ** 0.5 * 1.3)
                        break
            else:
                i = wl.idx(x & 15, layer, z & 15)
                b = c.blocks[i]
                rgb = colour(b, c.meta[i]) if b else AIR_SLICE
            px = bytes(rgb) * scale
            for dy in range(scale):
                row = rows[(z - z0) * scale + dy]
                row[(x - x0) * scale * 3:(x - x0 + 1) * scale * 3] = px
    if grid and scale >= 4:
        for zz in range(0, h):
            for xx in range(0, w, 16 * scale):
                rows[zz][xx * 3:xx * 3 + 3] = b"\x00\x00\x00"
        for zz in range(0, h, 16 * scale):
            rows[zz][:] = bytes(w * 3)
    return w, h, rows


def write_png(path, w, h, rows):
    try:
        from PIL import Image
        Image.frombytes("RGB", (w, h), b"".join(bytes(r) for r in rows)).save(path)
        return
    except ImportError:
        pass
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def preview(world, out, layers=(), scale=8):
    """Write the top-down PNG plus one PNG per requested Y. Returns paths."""
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    write_png(out, *render(world, None, scale))
    paths = [out]
    stem, ext = os.path.splitext(out)
    for y in layers:
        p = "%s_y%d%s" % (stem, y, ext or ".png")
        write_png(p, *render(world, y, scale))
        paths.append(p)
    return paths


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("save_dir")
    ap.add_argument("--out", required=True)
    ap.add_argument("--layers", default="")
    ap.add_argument("--scale", type=int, default=8)
    a = ap.parse_args(argv)
    layers = [int(v) for v in a.layers.split(",") if v.strip()]
    for p in preview(wl.load_world(a.save_dir), a.out, layers, a.scale):
        print(p)


if __name__ == "__main__":
    main()
