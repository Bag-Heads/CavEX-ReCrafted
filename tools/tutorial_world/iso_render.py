#!/usr/bin/env python3
"""
Offline isometric 3D previews of a World (review aid only, not game code).

    python tools/tutorial_world/iso_render.py                 # build tutorial in memory
    python tools/tutorial_world/iso_render.py --save "saves/CaveWii Tutorial"
    python tools/tutorial_world/iso_render.py --box 186,286,214,314 --view se --out x.png
    python tools/tutorial_world/iso_render.py --all --html   # overview + every zone + index.html
    python tools/tutorial_world/iso_render.py --box 204,198,256,212 --elev s --ymin 66 --ymax 86
                                                             # face-on elevation from the south
    python tools/tutorial_world/iso_render.py --fp 200.5,66.62,304.5,149,12
                                                             # first-person view (spawn)

Painter's-algorithm isometric renderer: each visible block is a cached sprite
built from textured, face-shaded quads (top 1.0, left 0.80, right 0.62) and
pasted back to front. Textures come from the SHIPPED default terrain atlas
(assets/terrain.png = Earth & Ash, CC0; see assets/README.md). Mojang/Beta jar
textures are refused. Non-cube blocks (slabs, stairs, torches, fences, signs,
doors, rails, wire, crops, flowers, leaves as cutout, ...) get simple box/quad
models; they are approximations for previews, not the in-game mesher.

Zone boxes are read (via ast, without importing side effects) from
build_tutorial.write_previews so they stay in sync with the 2D previews.
Needs Pillow and numpy.
"""
import argparse
import ast
import base64
import io
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover
    raise SystemExit("iso_render.py needs Pillow: python -m pip install pillow")
try:
    import numpy as np
except ImportError:  # pragma: no cover
    raise SystemExit("iso_render.py needs numpy: python -m pip install numpy")

import worldgen_lib as wl  # noqa: E402

DEFAULT_ATLAS = os.path.join(REPO, "assets", "terrain.png")
DEFAULT_OUT = os.path.join(REPO, "tmp", "jobs", "tutorial-1021", "3d")
SKY_TOP, SKY_BOT = (120, 170, 222), (206, 226, 244)

# ------------------------------------------------------------- directions --
DIRV = {"e": (1, 0, 0), "w": (-1, 0, 0), "s": (0, 0, 1), "n": (0, 0, -1),
        "up": (0, 1, 0), "down": (0, -1, 0)}
# View: (u, v) = (a1*x + b1*z, a2*x + b2*z). Camera sits at +u,+v looking at
# the -u,-v corner; visible faces are top, +u (right) and +v (left).
VIEWS = {"se": (1, 0, 0, 1), "nw": (-1, 0, 0, -1),
         "ne": (0, -1, 1, 0), "sw": (0, 1, -1, 0)}
VIEW_NAMES = {"se": "from south-east", "nw": "from north-west",
              "ne": "from north-east", "sw": "from south-west"}

# ------------------------------------------------------------ block tables --
# Atlas tile (col, row), from source/graphics/texture_atlas.c tex_atlas_reg.
T = {
    "grass_top": (0, 0), "stone": (1, 0), "dirt": (2, 0), "grass_side": (3, 0),
    "planks": (4, 0), "slab_side": (5, 0), "slab_top": (6, 0), "bricks": (7, 0),
    "tnt_side": (8, 0), "tnt_top": (9, 0), "cobweb": (11, 0), "rose": (12, 0),
    "dandelion": (13, 0), "sapling_oak": (15, 0), "cobble": (0, 1),
    "bedrock": (1, 1), "sand": (2, 1), "gravel": (3, 1), "log_oak": (4, 1),
    "log_top": (5, 1), "iron_block": (6, 1), "gold_block": (7, 1),
    "diamond_block": (8, 1), "chest_top": (9, 1), "chest_side": (10, 1),
    "chest_front": (11, 1), "mush_red": (12, 1), "mush_brown": (13, 1),
    "fire": (15, 1), "gold_ore": (0, 2), "iron_ore": (1, 2), "coal_ore": (2, 2),
    "bookshelf": (3, 2), "mossy": (4, 2), "obsidian": (5, 2),
    "grass_overlay": (6, 2), "tallgrass": (7, 2), "bench_top": (11, 2),
    "furnace_front": (12, 2), "furnace_side": (13, 2), "dispenser_front": (14, 2),
    "sponge": (0, 3), "glass": (1, 3), "diamond_ore": (2, 3),
    "redstone_ore": (3, 3), "leaves": (4, 3), "deadbush": (7, 3), "fern": (8, 3),
    "bench_side": (11, 3), "bench_front": (12, 3), "furnace_lit": (13, 3),
    "furnace_top": (14, 3), "sapling_spruce": (15, 3), "wool_white": (0, 4),
    "spawner": (1, 4), "snow": (2, 4), "ice": (3, 4), "cactus_top": (5, 4),
    "cactus_side": (6, 4), "clay": (8, 4), "reed": (9, 4),
    "jukebox_side": (10, 4), "jukebox_top": (11, 4), "sapling_birch": (15, 4),
    "torch": (0, 5), "door_wood_top": (1, 5), "door_iron_top": (2, 5),
    "ladder": (3, 5), "trapdoor": (4, 5), "farmland_wet": (6, 5),
    "farmland_dry": (7, 5), "lever": (0, 6), "door_wood_bot": (1, 6),
    "door_iron_bot": (2, 6), "rtorch_on": (3, 6), "pumpkin_top": (6, 6),
    "netherrack": (7, 6), "soulsand": (8, 6), "glowstone": (9, 6),
    "piston_sticky": (10, 6), "piston_front": (11, 6), "piston_side": (12, 6),
    "piston_back": (13, 6), "piston_inner": (14, 6), "rail_curve": (0, 7),
    "rtorch_off": (3, 7), "log_spruce": (4, 7), "log_birch": (5, 7),
    "pumpkin_side": (6, 7), "pumpkin_face": (7, 7), "jack_face": (8, 7),
    "cake_top": (9, 7), "cake_side": (10, 7), "rail": (0, 8),
    "repeater_off": (3, 8), "leaves_spruce": (4, 8), "bed_foot_top": (6, 8),
    "bed_head_top": (7, 8), "lapis_block": (0, 9), "repeater_on": (3, 9),
    "bed_foot_side": (6, 9), "bed_head_side": (7, 9), "lapis_ore": (0, 10),
    "prail_off": (3, 10), "wire_cross": (4, 10), "wire_line": (5, 10),
    "sandstone_top": (0, 11), "prail_on": (3, 11), "sandstone_side": (0, 12),
    "detector_rail": (3, 12), "water": (13, 12), "sandstone_bot": (0, 13),
    "lava": (13, 14),
}
WOOL = [(0, 4), (2, 13), (2, 12), (2, 11), (2, 10), (2, 9), (2, 8), (2, 7),
        (1, 14), (1, 13), (1, 12), (1, 11), (1, 10), (1, 9), (1, 8), (1, 7)]
GRASS_TINT = (110, 198, 63)      # biome_color.h BIOME_GRASS_FALLBACK
LEAF_TINT = {0: (75, 182, 15), 1: (97, 153, 97), 2: (128, 167, 85)}  # atlas reg_col

SIMPLE = {1: "stone", 3: "dirt", 4: "cobble", 5: "planks", 7: "bedrock",
          12: "sand", 13: "gravel", 14: "gold_ore", 15: "iron_ore",
          16: "coal_ore", 19: "sponge", 21: "lapis_ore", 22: "lapis_block",
          25: "jukebox_side", 41: "gold_block", 42: "iron_block", 45: "bricks",
          48: "mossy", 49: "obsidian", 56: "diamond_ore", 57: "diamond_block",
          73: "redstone_ore", 74: "redstone_ore", 80: "snow", 82: "clay",
          87: "netherrack", 88: "soulsand", 89: "glowstone"}
# Full opaque cubes (cull faces against them).
OPAQUE = set(SIMPLE) | {2, 17, 23, 24, 29, 33, 35, 43, 46, 47, 58, 61, 62, 84,
                        86, 91}
# Full cubes drawn with cutout / translucent textures.
CUTOUT_CUBES = {18, 20, 52, 79}
LIQUID = {8, 9, 10, 11}
CUBE_LIKE = OPAQUE | CUTOUT_CUBES | LIQUID
GROUP = {8: 9, 9: 9, 10: 11, 11: 11, 20: 20, 79: 79}
CROSS = {6, 30, 31, 32, 37, 38, 39, 40, 51, 59, 83}
WIRE_LINKS = {55, 69, 70, 72, 75, 76, 77, 93, 94}

SLAB_TEX = {0: ("slab_top", "slab_side"), 1: ("sandstone_top", "sandstone_side"),
            2: ("planks", "planks"), 3: ("cobble", "cobble")}
FRONT_DIR = {2: "n", 3: "s", 4: "w", 5: "e"}          # furnace / dispenser
PUMPKIN_DIR = {0: "s", 1: "w", 2: "n", 3: "e"}
PISTON_DIR = {0: "down", 1: "up", 2: "n", 3: "s", 4: "w", 5: "e"}
# Torch / button / lever wall meta 1..4: mounted on x-1, x+1, z-1, z+1.
MOUNT = {1: (-1, 0), 2: (1, 0), 3: (0, -1), 4: (0, 1)}
# Ladder / wall sign meta 2..5: mounted on z+1, z-1, x+1, x-1.
WALL_MOUNT = {2: (0, 1), 3: (0, -1), 4: (1, 0), 5: (-1, 0)}
ROT_OF = {"n": 0, "e": 1, "s": 2, "w": 3}


# ------------------------------------------------------------------ atlas --
class Atlas:
    def __init__(self, path=DEFAULT_ATLAS):
        p = os.path.abspath(path).replace("\\", "/").lower()
        if "beta-1.7.3" in p or "minecraft" in p or p.endswith(".jar"):
            raise SystemExit("refusing Mojang/Beta textures: %s" % path)
        im = Image.open(path).convert("RGBA")
        self.tile_px = im.width // 16
        self.im = im
        self.cache = {}
        lt = self.raw(T["wire_line"])
        xs, ys = set(), set()
        for yy in range(lt.height):
            for xx in range(lt.width):
                if lt.getpixel((xx, yy))[3]:
                    xs.add(xx)
                    ys.add(yy)
        # rotation that makes the line texture run north-south
        self.wire_ns_rot = 1 if (len(xs) > len(ys)) else 0

    def raw(self, tile):
        c, r = tile
        t = self.tile_px
        return self.im.crop((c * t, r * t, c * t + t, r * t + t))

    def tex(self, tile, tint=None, shade=1.0, rot=0, overlay=None):
        key = (tile, tint, round(shade, 3), rot % 4, overlay)
        im = self.cache.get(key)
        if im is not None:
            return im
        im = self.raw(tile)
        if overlay is not None:
            ov = self.raw(overlay[0])
            im = Image.alpha_composite(im, _mul(ov, overlay[1], 1.0))
        im = _mul(im, tint, shade)
        if rot % 4:
            im = im.rotate(-90 * (rot % 4))
        self.cache[key] = im
        return im


def _mul(im, tint, shade):
    if tint is None and shade == 1.0:
        return im
    tr, tg, tb = tint if tint else (255, 255, 255)
    r, g, b, a = im.split()
    r = r.point(lambda v: int(v * tr / 255.0 * shade))
    g = g.point(lambda v: int(v * tg / 255.0 * shade))
    b = b.point(lambda v: int(v * tb / 255.0 * shade))
    return Image.merge("RGBA", (r, g, b, a))


# ------------------------------------------------------------------ models --
class F:
    """One face texture spec: tile, tint, rotation (0 = texture top -> 'up' axis)."""
    __slots__ = ("tile", "tint", "rot", "overlay")

    def __init__(self, tile, tint=None, rot=0, overlay=None):
        self.tile = T[tile] if isinstance(tile, str) else tile
        self.tint, self.rot, self.overlay = tint, rot, overlay


def faces_all(f):
    return {d: f for d in DIRV}


def faces_tsb(top, side, bottom=None):
    d = {k: side for k in ("n", "s", "e", "w")}
    d["up"] = top
    d["down"] = bottom or top
    return d


def box(x0, y0, z0, x1, y1, z1, faces, cull=False):
    return ("box", (x0, y0, z0, x1, y1, z1), faces, cull)


def quad(p0, ps, pt, f, shade=1.0, sub=(0, 0, 1, 1)):
    return ("quad", (p0, ps, pt), f, shade, sub)


def cross(f, h=1.0):
    return [quad((0, h, 0), (1, h, 1), (0, 0, 0), f, 0.92),
            quad((1, h, 0), (0, h, 1), (1, 0, 0), f, 0.92)]


def flat(f, y=1 / 32.0, heights=None, sub=(0, 0, 1, 1)):
    h = heights or (y, y, y)  # at (0,0), (1,0), (0,1)
    return quad((0, h[0], 0), (1, h[1], 0), (0, h[2], 1), f, 1.0, sub)


def toward(d, lo, hi, a=0.0, b=1.0, ylo=0.0, yhi=1.0):
    """Box whose extent along horizontal/vertical facing d is [lo,hi] (0 = back,
    1 = the face in direction d); the other axes span [a,b] (and y [ylo,yhi])."""
    dx, dy, dz = DIRV[d]
    def rng(comp, other):
        if comp == 1:
            return lo, hi
        if comp == -1:
            return 1 - hi, 1 - lo
        return other
    x0, x1 = rng(dx, (a, b))
    z0, z1 = rng(dz, (a, b))
    y0, y1 = rng(dy, (ylo, yhi) if dy == 0 else (a, b))
    return x0, y0, z0, x1, y1, z1


def model(bid, meta, conn):
    """List of primitives for one block (world-local coords 0..1)."""
    if bid in SIMPLE:
        return [box(0, 0, 0, 1, 1, 1, faces_all(F(SIMPLE[bid])), True)]
    if bid == 2:
        side = F("grass_side", None, 0, (T["grass_overlay"], GRASS_TINT))
        return [box(0, 0, 0, 1, 1, 1,
                    faces_tsb(F("grass_top", GRASS_TINT), side, F("dirt")), True)]
    if bid == 17:
        side = {0: "log_oak", 1: "log_spruce", 2: "log_birch"}.get(meta & 3, "log_oak")
        return [box(0, 0, 0, 1, 1, 1, faces_tsb(F("log_top"), F(side)), True)]
    if bid == 18:
        tile = "leaves_spruce" if (meta & 3) == 1 else "leaves"
        return [box(0, 0, 0, 1, 1, 1, faces_all(F(tile, LEAF_TINT.get(meta & 3))), True)]
    if bid in (20, 79, 52):
        tile = {20: "glass", 79: "ice", 52: "spawner"}[bid]
        return [box(0, 0, 0, 1, 1, 1, faces_all(F(tile)), True)]
    if bid == 24:
        return [box(0, 0, 0, 1, 1, 1, faces_tsb(F("sandstone_top"),
                    F("sandstone_side"), F("sandstone_bot")), True)]
    if bid == 35:
        return [box(0, 0, 0, 1, 1, 1, faces_all(F(WOOL[meta & 15])), True)]
    if bid == 43 or bid == 44:
        top, side = SLAB_TEX.get(meta & 7, SLAB_TEX[0])
        return [box(0, 0, 0, 1, 1.0 if bid == 43 else 0.5, 1,
                    faces_tsb(F(top), F(side)), bid == 43)]
    if bid == 46:
        return [box(0, 0, 0, 1, 1, 1, faces_tsb(F("tnt_top"), F("tnt_side")), True)]
    if bid == 47:
        return [box(0, 0, 0, 1, 1, 1, faces_tsb(F("planks"), F("bookshelf")), True)]
    if bid == 58:
        f = faces_tsb(F("bench_top"), F("bench_side"), F("planks"))
        f["s"] = f["n"] = F("bench_front")
        return [box(0, 0, 0, 1, 1, 1, f, True)]
    if bid in (61, 62, 23):
        front = {61: "furnace_front", 62: "furnace_lit", 23: "dispenser_front"}[bid]
        f = faces_tsb(F("furnace_top"), F("furnace_side"))
        f[FRONT_DIR.get(meta & 7, "s")] = F(front)
        return [box(0, 0, 0, 1, 1, 1, f, True)]
    if bid == 84:
        return [box(0, 0, 0, 1, 1, 1, faces_tsb(F("jukebox_top"), F("jukebox_side")), True)]
    if bid in (86, 91):
        f = faces_tsb(F("pumpkin_top"), F("pumpkin_side"))
        f[PUMPKIN_DIR[meta & 3]] = F("pumpkin_face" if bid == 86 else "jack_face")
        return [box(0, 0, 0, 1, 1, 1, f, True)]
    if bid in (29, 33):
        d = PISTON_DIR.get(meta & 7, "up")
        ext = bool(meta & 8)
        side = F("piston_side")
        f = {k: side for k in DIRV}
        f[d] = F("piston_inner" if ext else
                 ("piston_sticky" if bid == 29 else "piston_front"))
        f[_opp(d)] = F("piston_back")
        f = _orient_sides(f, d)
        return [box(*toward(d, 0, 0.75 if ext else 1.0), f, not ext)]
    if bid == 34:
        d = PISTON_DIR.get(meta & 7, "up")
        f = _orient_sides({k: F("piston_side") for k in DIRV}, d)
        f[d] = F("piston_sticky" if meta & 8 else "piston_front")
        arm = _orient_sides({k: F("piston_side") for k in DIRV}, d)
        return [box(*toward(d, -0.25, 0.75, 0.375, 0.625, 0.375, 0.625), arm),
                box(*toward(d, 0.75, 1.0), f)]
    if bid in LIQUID:
        water = bid in (8, 9)
        f = faces_all(F("water" if water else "lava"))
        top_exposed = conn & 1
        h = 1.0
        if top_exposed:
            lvl = meta & 7
            h = 1.0 - (lvl + 1) / 9.0 if lvl < 8 else 1.0
        return [box(0, 0, 0, 1, h, 1, f, True)]
    if bid in (53, 67):
        f = faces_all(F("planks" if bid == 53 else "cobble"))
        hi = {0: "e", 1: "w", 2: "s", 3: "n"}.get(meta & 3, "e")
        return [box(0, 0, 0, 1, 0.5, 1, f),
                box(*toward(hi, 0.5, 1.0, 0, 1, 0.5, 1.0), f)]
    if bid in (50, 75, 76):
        tile = {50: "torch", 75: "rtorch_off", 76: "rtorch_on"}[bid]
        m = meta & 7
        dx, dz = MOUNT.get(m, (0, 0))
        ox, oz, oy = dx * 0.32, dz * 0.32, (0.2 if m in MOUNT else 0.0)
        a, b = 7 / 16.0, 9 / 16.0
        side = F(tile)
        f = {k: side for k in DIRV}
        f["up"] = F(tile)
        prims = [("box", (a + ox, oy, a + oz, b + ox, oy + 10 / 16.0, b + oz), f, False,
                  {"side_sub": (7 / 16.0, 6 / 16.0, 9 / 16.0, 1.0),
                   "top_sub": (7 / 16.0, 6 / 16.0, 9 / 16.0, 8 / 16.0)})]
        return prims
    if bid == 85:
        f = faces_all(F("planks"))
        p = [box(6 / 16.0, 0, 6 / 16.0, 10 / 16.0, 1, 10 / 16.0, f)]
        arms = {"e": 1, "w": 2, "s": 4, "n": 8}
        for d, bit in arms.items():
            if conn & bit:
                for (y0, y1) in ((6 / 16.0, 9 / 16.0), (12 / 16.0, 15 / 16.0)):
                    p.append(box(*toward(d, 10 / 16.0, 1.0, 7 / 16.0, 9 / 16.0, y0, y1), f))
        return p
    if bid == 63:
        f = faces_all(F("planks"))
        along_x = ((meta + 2) // 4) % 2 == 0
        p = [box(7 / 16.0, 0, 7 / 16.0, 9 / 16.0, 9 / 16.0, 9 / 16.0, faces_all(F("log_oak")))]
        if along_x:
            p.append(box(0, 9 / 16.0, 7 / 16.0, 1, 1, 9 / 16.0, f))
        else:
            p.append(box(7 / 16.0, 9 / 16.0, 0, 9 / 16.0, 1, 1, f))
        return p
    if bid in (68, 65):
        dx, dz = WALL_MOUNT.get(meta & 7, (0, 1))
        d = {(1, 0): "e", (-1, 0): "w", (0, 1): "s", (0, -1): "n"}[(dx, dz)]
        if bid == 68:
            return [box(*toward(d, 14 / 16.0, 1.0, 0, 1, 4.5 / 16.0, 12.5 / 16.0),
                        faces_all(F("planks")))]
        return [box(*toward(d, 15 / 16.0, 1.0), faces_all(F("ladder")))]
    if bid in (64, 71):
        state = ((meta & 3) + (1 if meta & 4 else 0)) % 4
        d = {0: "w", 1: "n", 2: "e", 3: "s"}[state]
        top = bool(meta & 8)
        tile = ("door_wood_" if bid == 64 else "door_iron_") + ("top" if top else "bot")
        return [box(*toward(d, 13 / 16.0, 1.0), faces_all(F(tile)))]
    if bid == 96:
        f = faces_all(F("trapdoor"))
        if meta & 4:
            d = {0: "s", 1: "n", 2: "e", 3: "w"}[meta & 3]
            return [box(*toward(d, 13 / 16.0, 1.0), f)]
        return [box(0, 0, 0, 1, 3 / 16.0, 1, f)]
    if bid in (66, 27, 28):
        shape = meta if bid == 66 else (meta & 7)
        if bid == 66:
            tile = "rail_curve" if shape >= 6 else "rail"
        elif bid == 27:
            tile = "prail_on" if meta & 8 else "prail_off"
        else:
            tile = "detector_rail"
        y = 1 / 32.0
        if shape >= 6:
            rot = {6: 0, 7: 1, 8: 2, 9: 3}.get(shape, 0)
            return [flat(F(tile, None, rot), y)]
        rot = 1 if shape in (1, 2, 3) else 0
        hts = {2: (y, 1 + y, y), 3: (1 + y, y, 1 + y),
               4: (1 + y, 1 + y, y), 5: (y, y, 1 + y)}.get(shape)
        return [flat(F(tile, None, rot), y, hts)]
    if bid == 55:
        f_ = meta / 15.0
        r = f_ * 0.6 + 0.4 if meta else 0.3
        g = max(0.0, f_ * f_ * 0.7 - 0.5)
        b = max(0.0, f_ * f_ * 0.6 - 0.7)
        tint = (int(r * 255), int(g * 255), int(b * 255))
        ew = conn & 3
        ns = conn & 12
        if (ew and not ns) or (ns and not ew):
            rot = WIRE_ROT[0] if ns else WIRE_ROT[0] + 1
            return [flat(F("wire_line", tint, rot), 1 / 48.0)]
        if not conn:
            return [flat(F("wire_cross", tint), 1 / 48.0, sub=(5 / 16.0, 5 / 16.0,
                                                                 11 / 16.0, 11 / 16.0))]
        s0 = 0 if conn & 2 else 5 / 16.0
        s1 = 1 if conn & 1 else 11 / 16.0
        t0 = 0 if conn & 8 else 5 / 16.0
        t1 = 1 if conn & 4 else 11 / 16.0
        y = 1 / 48.0
        return [quad((s0, y, t0), (s1, y, t0), (s0, y, t1), F("wire_cross", tint),
                     1.0, (s0, t0, s1, t1))]
    if bid in (93, 94):
        out = {0: "n", 1: "e", 2: "s", 3: "w"}[meta & 3]
        top = F("repeater_on" if bid == 94 else "repeater_off", None, ROT_OF[out])
        f = faces_all(F("slab_top"))
        f["up"] = top
        return [box(0, 0, 0, 1, 2 / 16.0, 1, f)]
    if bid in (70, 72):
        f = faces_all(F("stone" if bid == 70 else "planks"))
        return [box(1 / 16.0, 0, 1 / 16.0, 15 / 16.0, 1 / 16.0, 15 / 16.0, f)]
    if bid == 77:
        dx, dz = MOUNT.get(meta & 7, (0, 1))
        d = {(1, 0): "e", (-1, 0): "w", (0, 1): "s", (0, -1): "n"}[(dx, dz)]
        return [box(*toward(d, 14 / 16.0, 1.0, 5 / 16.0, 11 / 16.0, 6 / 16.0, 10 / 16.0),
                    faces_all(F("stone")))]
    if bid == 69:
        m = meta & 7
        base = faces_all(F("cobble"))
        stick = faces_all(F("lever"))
        if m in MOUNT:
            dx, dz = MOUNT[m]
            d = {(1, 0): "e", (-1, 0): "w", (0, 1): "s", (0, -1): "n"}[(dx, dz)]
            return [box(*toward(d, 13 / 16.0, 1.0, 5 / 16.0, 11 / 16.0, 3 / 16.0, 13 / 16.0), base),
                    box(*toward(d, 7 / 16.0, 13 / 16.0, 7 / 16.0, 9 / 16.0, 9 / 16.0, 11 / 16.0), stick)]
        return [box(5 / 16.0, 0, 4 / 16.0, 11 / 16.0, 3 / 16.0, 12 / 16.0, base),
                box(7 / 16.0, 3 / 16.0, 7 / 16.0, 9 / 16.0, 11 / 16.0, 9 / 16.0, stick)]
    if bid == 26:
        d = {0: "s", 1: "w", 2: "n", 3: "e"}[meta & 3]
        head = bool(meta & 8)
        top = F("bed_head_top" if head else "bed_foot_top", None, ROT_OF[d])
        side = F("bed_head_side" if head else "bed_foot_side")
        f = faces_tsb(top, side, F("planks"))
        return [box(0, 0, 0, 1, 9 / 16.0, 1, f)]
    if bid == 54:
        f = faces_tsb(F("chest_top"), F("chest_side"))
        f["s"] = F("chest_front")
        return [box(1 / 16.0, 0, 1 / 16.0, 15 / 16.0, 14 / 16.0, 15 / 16.0, f)]
    if bid == 92:
        f = faces_tsb(F("cake_top"), F("cake_side"))
        return [box(1 / 16.0, 0, 1 / 16.0, 15 / 16.0, 0.5, 15 / 16.0, f)]
    if bid == 81:
        f = faces_tsb(F("cactus_top"), F("cactus_side"))
        return [box(1 / 16.0, 0, 1 / 16.0, 15 / 16.0, 1, 15 / 16.0, f)]
    if bid == 60:
        f = faces_tsb(F("farmland_wet" if meta else "farmland_dry"), F("dirt"))
        return [box(0, 0, 0, 1, 15 / 16.0, 1, f)]
    if bid == 78:
        h = ((meta & 7) + 1) * 2 / 16.0
        return [box(0, 0, 0, 1, h, 1, faces_all(F("snow")))]
    if bid in CROSS:
        if bid == 6:
            tile = {0: "sapling_oak", 1: "sapling_spruce", 2: "sapling_birch"}.get(meta & 3, "sapling_oak")
            return cross(F(tile))
        if bid == 31:
            if meta == 0:
                return cross(F("deadbush"))
            return cross(F("fern" if meta == 2 else "tallgrass", GRASS_TINT))
        if bid == 59:
            return cross(F((8 + min(7, meta), 5)))
        tile = {30: "cobweb", 32: "deadbush", 37: "dandelion", 38: "rose",
                39: "mush_brown", 40: "mush_red", 51: "fire", 83: "reed"}[bid]
        return cross(F(tile))
    # unknown: magenta marker cube so it is visible in review
    return [("box", (0.2, 0, 0.2, 0.8, 0.6, 0.8), None, False)]


WIRE_ROT = [0]


def _opp(d):
    return {"e": "w", "w": "e", "n": "s", "s": "n", "up": "down", "down": "up"}[d]


def _orient_sides(f, d):
    """Rotate side textures so their 'top' points along facing d (pistons)."""
    out = dict(f)
    for k in ("n", "s", "e", "w", "up", "down"):
        if k in (d, _opp(d)):
            continue
        spec = f[k]
        out[k] = F(spec.tile, spec.tint, _rot_toward(k, d), spec.overlay)
    return out


# Each face's (s axis, up axis) in world vectors; used to orient textures.
FACE_AXES = {
    "up": ((1, 0, 0), (0, 0, -1)), "down": ((1, 0, 0), (0, 0, 1)),
    "s": ((1, 0, 0), (0, 1, 0)), "n": ((-1, 0, 0), (0, 1, 0)),
    "e": ((0, 0, -1), (0, 1, 0)), "w": ((0, 0, 1), (0, 1, 0)),
}


def _rot_toward(face, d):
    s, up = FACE_AXES[face]
    v = DIRV[d]
    if v == up:
        return 0
    if v == s:
        return 1
    if v == tuple(-c for c in up):
        return 2
    return 3


# ---------------------------------------------------------------- renderer --
class Renderer:
    def __init__(self, atlas, a, view="se"):
        self.atlas = atlas
        self.a = a
        self.view = view
        self.a1, self.b1, self.a2, self.b2 = VIEWS[view]
        self.m = 2
        self.size = 2 * a + 2 * self.m
        self.cache = {}
        WIRE_ROT[0] = atlas.wire_ns_rot
        # world face names that are the view's +u (right) and +v (left) faces
        self.right = self._face_for(1, 0)
        self.left = self._face_for(0, 1)

    def _face_for(self, du, dv):
        for k in ("e", "w", "s", "n"):
            x, _, z = DIRV[k]
            if (self.a1 * x + self.b1 * z, self.a2 * x + self.b2 * z) == (du, dv):
                return k
        raise AssertionError

    # world-local point -> sprite-local screen point
    def proj(self, p):
        x, y, z = p
        u = self.a1 * (x - 0.5) + self.b1 * (z - 0.5) + 0.5
        v = self.a2 * (x - 0.5) + self.b2 * (z - 0.5) + 0.5
        a = self.a
        return ((u - v) * a + a + self.m, (u + v) * a / 2.0 + (1 - y) * a + self.m,
                u + v + y)

    def face_shade(self, face):
        if face == "up":
            return 1.0
        if face == self.left:
            return 0.80
        if face == self.right:
            return 0.62
        return None  # not visible

    def sprite(self, bid, meta, mask, conn):
        key = (bid, meta, mask, conn)
        sp = self.cache.get(key)
        if sp is None:
            sp = self.cache[key] = self._build(bid, meta, mask, conn)
        return sp

    def _build(self, bid, meta, mask, conn):
        canvas = Image.new("RGBA", (self.size, self.size), (0, 0, 0, 0))
        prims = model(bid, meta, conn)
        items = []
        for pr in prims:
            if pr[0] == "box":
                x0, y0, z0, x1, y1, z1 = pr[1]
                c = self.proj(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2))[2]
                items.append((c, pr))
            else:
                p0, ps, pt = pr[1]
                cx = (ps[0] + pt[0]) / 2
                cy = (ps[1] + pt[1]) / 2
                cz = (ps[2] + pt[2]) / 2
                items.append((self.proj((cx, cy, cz))[2], pr))
        items.sort(key=lambda t: t[0])
        for _, pr in items:
            if pr[0] == "box":
                self._draw_box(canvas, pr, mask)
            else:
                _, (p0, ps, pt), f, shade, sub = pr
                tex = self.atlas.tex(f.tile, f.tint, shade, f.rot)
                self._quad(canvas, tex, p0, ps, pt, sub)
        bb = canvas.getbbox()
        if bb is None:
            return None
        return (canvas.crop(bb), bb[0], bb[1])

    def _draw_box(self, canvas, pr, mask):
        (x0, y0, z0, x1, y1, z1) = pr[1]
        faces, cull = pr[2], pr[3]
        extra = pr[4] if len(pr) > 4 else {}
        if faces is None:  # unknown block marker
            faces = faces_all(F((0, 0), (255, 0, 255)))
        for face in ("up", self.left, self.right):
            shade = self.face_shade(face)
            if cull:
                bit = {"up": 1, self.right: 2, self.left: 4}[face]
                if not (mask & bit):
                    continue
            spec = faces.get(face)
            if spec is None:
                continue
            tex = self.atlas.tex(spec.tile, spec.tint, shade, spec.rot, spec.overlay)
            if face == "up":
                p0, ps, pt = (x0, y1, z0), (x1, y1, z0), (x0, y1, z1)
                sub = extra.get("top_sub", (x0, z0, x1, z1))
            elif face == "s":
                p0, ps, pt = (x0, y1, z1), (x1, y1, z1), (x0, y0, z1)
                sub = (x0, 1 - y1, x1, 1 - y0)
            elif face == "n":
                p0, ps, pt = (x1, y1, z0), (x0, y1, z0), (x1, y0, z0)
                sub = (1 - x1, 1 - y1, 1 - x0, 1 - y0)
            elif face == "e":
                p0, ps, pt = (x1, y1, z1), (x1, y1, z0), (x1, y0, z1)
                sub = (1 - z1, 1 - y1, 1 - z0, 1 - y0)
            else:  # w
                p0, ps, pt = (x0, y1, z0), (x0, y1, z1), (x0, y0, z0)
                sub = (z0, 1 - y1, z1, 1 - y0)
            if face != "up" and "side_sub" in extra:
                sub = extra["side_sub"]
            self._quad(canvas, tex, p0, ps, pt, sub)

    def _quad(self, canvas, tex, p0, ps, pt, sub):
        q0 = self.proj(p0)[:2]
        qs = self.proj(ps)[:2]
        qt = self.proj(pt)[:2]
        ex, ey = qs[0] - q0[0], qs[1] - q0[1]
        fx, fy = qt[0] - q0[0], qt[1] - q0[1]
        det = ex * fy - fx * ey
        if abs(det) < 1e-6 * self.a * self.a:
            return
        # grow the quad ~0.5 px so neighbouring faces leave no seams
        le = math.hypot(ex, ey) or 1.0
        lf = math.hypot(fx, fy) or 1.0
        ke, kf = 0.5 / le, 0.5 / lf
        ox = q0[0] - ex * ke - fx * kf
        oy = q0[1] - ey * ke - fy * kf
        ex, ey, fx, fy = ex * (1 + 2 * ke), ey * (1 + 2 * ke), fx * (1 + 2 * kf), fy * (1 + 2 * kf)
        det = ex * fy - fx * ey
        xs = [ox, ox + ex, ox + fx, ox + ex + fx]
        ys = [oy, oy + ey, oy + fy, oy + ey + fy]
        bx0, by0 = int(math.floor(min(xs))), int(math.floor(min(ys)))
        bx1, by1 = int(math.ceil(max(xs))), int(math.ceil(max(ys)))
        w, h = bx1 - bx0, by1 - by0
        if w <= 0 or h <= 0:
            return
        n = tex.width
        s0, t0, s1, t1 = sub
        # output (X, Y) -> canvas point (bx0 + X, by0 + Y) -> (alpha, beta)
        # alpha = ( fy*(Px-ox) - fx*(Py-oy)) / det ; beta = (-ey*(Px-ox) + ex*(Py-oy)) / det
        A = fy / det
        B = -fx / det
        C = (fy * (bx0 - ox) - fx * (by0 - oy)) / det
        D = -ey / det
        E = ex / det
        G = (-ey * (bx0 - ox) + ex * (by0 - oy)) / det
        sw, th = (s1 - s0) * n, (t1 - t0) * n
        coeffs = (A * sw, B * sw, C * sw + s0 * n, D * th, E * th, G * th + t0 * n)
        part = tex.transform((w, h), Image.AFFINE, coeffs, resample=Image.NEAREST,
                             fillcolor=(0, 0, 0, 0))
        canvas.alpha_composite(part, (bx0, by0)) if bx0 >= 0 and by0 >= 0 else \
            _composite_clip(canvas, part, bx0, by0)

    # ------------------------------------------------------------ scene --
    def render(self, world, box_xz, ymin, ymax, hide=()):
        x0, z0, x1, z1 = box_xz
        B, M, MASK, CONN = scene_arrays(world, x0, z0, x1, z1, ymin, ymax, self.view,
                                        hide)
        ny, nv, nu = B.shape
        a = self.a
        draw = np.nonzero(B)
        ys, vs, us = draw
        keys = us + vs + ys
        order = np.lexsort((ys, keys))
        # canvas extents
        W = (nu + nv) * a + 4 * a
        H = int((nu + nv) * a / 2 + (ny + 2) * a + 4 * a)
        offx = nv * a + 2 * a
        offy = (ny + 1) * a + 2 * a
        img = Image.new("RGB", (W, H), (0, 0, 0))
        _sky(img)
        bbox = [W, H, 0, 0]
        cnt = 0
        for i in order:
            y, v, u = int(ys[i]), int(vs[i]), int(us[i])
            sp = self.sprite(int(B[y, v, u]), int(M[y, v, u]), int(MASK[y, v, u]),
                             int(CONN[y, v, u]))
            if sp is None:
                continue
            im, dx, dy = sp
            # global screen of the cell's view-min corner (lu=lv=0, ly=0)
            gx = (u - v) * a + offx
            gy = (u + v) * a / 2.0 - y * a + offy
            px = int(round(gx - a - self.m + dx))
            py = int(round(gy - a - self.m + dy))
            img.paste(im, (px, py), im)
            cnt += 1
            if px < bbox[0]: bbox[0] = px
            if py < bbox[1]: bbox[1] = py
            if px + im.width > bbox[2]: bbox[2] = px + im.width
            if py + im.height > bbox[3]: bbox[3] = py + im.height
        pad = 2 * a
        crop = (max(0, bbox[0] - pad), max(0, bbox[1] - pad),
                min(W, bbox[2] + pad), min(H, bbox[3] + pad))
        return img.crop(crop), cnt


# ------------------------------------------------------- front elevation --
# Orthographic face-on view: the viewer stands on one side of the box and
# looks straight across it (no top faces, no perspective), e.g. "s" = from
# the south looking north, which is how the CAVEX wordmark is seen from the
# spawn side. Far layers are faded toward the sky colour (aerial
# perspective) so overlapping planes stay readable.
ELEV_NAMES = {"s": "from the south (looking north)",
              "n": "from the north (looking south)",
              "e": "from the east (looking west)",
              "w": "from the west (looking east)"}


def _elev_local(facing, p):
    """Cell-local point -> (u across the screen, y, nearness)."""
    x, y, z = p
    if facing == "s":
        return x, y, z
    if facing == "n":
        return 1 - x, y, 1 - z
    if facing == "e":
        return 1 - z, y, x
    return z, y, 1 - x                      # "w"


def _elev_arrays(arr, facing):
    """[y, z, x] world array -> [y, depth (0 = nearest), u] view array."""
    if facing == "s":
        return arr[:, ::-1, :]
    if facing == "n":
        return arr[:, :, ::-1]
    if facing == "e":
        return arr.transpose(0, 2, 1)[:, ::-1, ::-1]
    if facing == "w":
        return arr.transpose(0, 2, 1)
    raise ValueError(facing)


class ElevRenderer:
    def __init__(self, atlas, a, facing="s", fog=0.012, fog_max=0.6):
        self.atlas, self.a, self.facing = atlas, a, facing
        self.fog, self.fog_max = fog, fog_max
        self.cache = {}
        WIRE_ROT[0] = atlas.wire_ns_rot

    def proj(self, p):
        u, y, n = _elev_local(self.facing, p)
        return u * self.a, (1 - y) * self.a, n

    def _quad(self, canvas, tex, p0, ps, pt, sub):
        Renderer._quad(self, canvas, tex, p0, ps, pt, sub)

    def sprite(self, bid, meta, conn):
        key = (bid, meta, conn)
        sp = self.cache.get(key)
        if sp is None:
            sp = self.cache[key] = self._build(bid, meta, conn)
        return sp

    def _build(self, bid, meta, conn):
        a = self.a
        canvas = Image.new("RGBA", (a + 2, a + 2), (0, 0, 0, 0))
        self.m = 1
        items = []
        for pr in model(bid, meta, conn):
            if pr[0] == "box":
                x0, y0, z0, x1, y1, z1 = pr[1]
                faces = pr[2] or faces_all(F((0, 0), (255, 0, 255)))
                spec = faces.get(self.facing)
                if spec is None:
                    continue
                f = self.facing
                if f == "s":
                    q = ((x0, y1, z1), (x1, y1, z1), (x0, y0, z1),
                         (x0, 1 - y1, x1, 1 - y0))
                elif f == "n":
                    q = ((x1, y1, z0), (x0, y1, z0), (x1, y0, z0),
                         (1 - x1, 1 - y1, 1 - x0, 1 - y0))
                elif f == "e":
                    q = ((x1, y1, z1), (x1, y1, z0), (x1, y0, z1),
                         (1 - z1, 1 - y1, 1 - z0, 1 - y0))
                else:
                    q = ((x0, y1, z0), (x0, y1, z1), (x0, y0, z0),
                         (z0, 1 - y1, z1, 1 - y0))
                extra = pr[4] if len(pr) > 4 else {}
                sub = extra.get("side_sub", q[3])
                tex = self.atlas.tex(spec.tile, spec.tint, 1.0, spec.rot,
                                     spec.overlay)
                n = self.proj(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2))[2]
                items.append((n, tex, q[0], q[1], q[2], sub))
            else:
                _, (p0, ps, pt), f, shade, sub = pr
                tex = self.atlas.tex(f.tile, f.tint, shade, f.rot)
                c = ((ps[0] + pt[0]) / 2, (ps[1] + pt[1]) / 2, (ps[2] + pt[2]) / 2)
                items.append((self.proj(c)[2], tex, p0, ps, pt, sub))
        items.sort(key=lambda t: t[0])
        for (_, tex, p0, ps, pt, sub) in items:
            self._quad(canvas, tex, p0, ps, pt, sub)
        if canvas.getbbox() is None:
            return None
        return canvas

    def faded(self, bid, meta, conn, d):
        bucket = min(int(d * self.fog / 0.04), int(self.fog_max / 0.04))
        key = (bid, meta, conn, bucket)
        sp = self.cache.get(key)
        if sp is None:
            base = self.sprite(bid, meta, conn)
            if base is None:
                sp = None
            elif bucket == 0:
                sp = base
            else:
                f = bucket * 0.04
                arr = np.asarray(base).astype(np.float32)
                sky = np.array(SKY_BOT, np.float32)
                arr[..., :3] = arr[..., :3] * (1 - f) + sky * f
                sp = Image.fromarray(arr.astype(np.uint8), "RGBA")
            self.cache[key] = sp
        return sp

    def render(self, world, box_xz, ymin, ymax, hide=()):
        x0, z0, x1, z1 = box_xz
        Bw, Mw = world_box(world, x0, z0, x1, z1, ymin, ymax)
        if hide:
            Bw[np.isin(Bw, list(hide))] = 0
        Cw = _conn(Bw)
        sl = (slice(1, -1), slice(1, -1), slice(1, -1))
        B = np.ascontiguousarray(_elev_arrays(Bw[sl], self.facing))
        M = np.ascontiguousarray(_elev_arrays(Mw[sl], self.facing))
        C = np.ascontiguousarray(_elev_arrays(Cw[sl], self.facing))
        ny, nd, nu = B.shape
        near = np.zeros_like(B)
        near[:, 1:, :] = B[:, :-1, :]
        hidden = CUBE[B] & OPQ[near] & ~((GRP[B] != 0) & (GRP[B] == GRP[near]))
        hidden |= CUBE[B] & (GRP[B] != 0) & (GRP[B] == GRP[near])
        draw = (B != 0) & ~hidden
        a = self.a
        img = Image.new("RGB", (nu * a + 2, ny * a + 2), (0, 0, 0))
        _sky(img)
        cnt = 0
        for d in range(nd - 1, -1, -1):
            ys, us = np.nonzero(draw[:, d, :])
            for y, u in zip(ys.tolist(), us.tolist()):
                sp = self.faded(int(B[y, d, u]), int(M[y, d, u]),
                                int(C[y, d, u]), d)
                if sp is None:
                    continue
                img.paste(sp, (u * a, (ny - 1 - y) * a), sp)
                cnt += 1
        return img, cnt


def render_elev(world, atlas, box_xz, facing="s", ymin=60, ymax=100,
                target_w=1400, title=None, max_a=32, fog=0.012):
    x0, z0, x1, z1 = box_xz
    span = (x1 - x0 + 1) if facing in ("s", "n") else (z1 - z0 + 1)
    a = max(4, min(max_a, int(2 * target_w / span)))
    r = ElevRenderer(atlas, a, facing, fog=fog)
    t = time.time()
    img, n = r.render(world, box_xz, ymin, ymax)
    img = finish(img, target_w)
    if title:
        d = ImageDraw.Draw(img, "RGBA")
        fs = max(16, img.width // 45)
        font = _font(fs)
        tw = d.textlength(title, font=font)
        d.rounded_rectangle((10, 10, 10 + tw + fs, 10 + fs * 1.6),
                            radius=fs // 3, fill=(0, 0, 0, 140))
        d.text((10 + fs // 2, 10 + fs * 0.25), title, font=font,
               fill=(255, 255, 255))
    return img, {"cells": n, "a": a, "secs": round(time.time() - t, 1)}


def _composite_clip(canvas, part, bx, by):
    cx0, cy0 = max(0, -bx), max(0, -by)
    part = part.crop((cx0, cy0, part.width, part.height))
    canvas.alpha_composite(part, (max(0, bx), max(0, by)))


def _sky(img):
    W, H = img.size
    grad = Image.new("RGB", (1, 256))
    for i in range(256):
        f = i / 255.0
        grad.putpixel((0, i), tuple(int(SKY_TOP[k] + (SKY_BOT[k] - SKY_TOP[k]) * f)
                                    for k in range(3)))
    img.paste(grad.resize((W, H)), (0, 0))


# ------------------------------------------------------------ world arrays --
def world_box(world, x0, z0, x1, z1, ymin, ymax):
    """blocks/meta as [y, z, x] numpy arrays with a 1-cell air pad on every side."""
    nx, nz, ny = x1 - x0 + 1, z1 - z0 + 1, ymax - ymin + 1
    Bw = np.zeros((ny + 2, nz + 2, nx + 2), np.uint8)
    Mw = np.zeros_like(Bw)
    for cx in range(x0 >> 4, (x1 >> 4) + 1):
        for cz in range(z0 >> 4, (z1 >> 4) + 1):
            c = world.chunks.get((cx, cz))
            if c is None:
                continue
            cb = np.frombuffer(bytes(c.blocks), np.uint8).reshape(16, 16, 128)  # x,z,y
            cm = np.frombuffer(bytes(c.meta), np.uint8).reshape(16, 16, 128)
            gx0, gz0 = cx * 16, cz * 16
            lx0, lx1 = max(x0, gx0) - gx0, min(x1, gx0 + 15) - gx0
            lz0, lz1 = max(z0, gz0) - gz0, min(z1, gz0 + 15) - gz0
            sb = cb[lx0:lx1 + 1, lz0:lz1 + 1, ymin:ymax + 1].transpose(2, 1, 0)
            sm = cm[lx0:lx1 + 1, lz0:lz1 + 1, ymin:ymax + 1].transpose(2, 1, 0)
            ox, oz = gx0 + lx0 - x0 + 1, gz0 + lz0 - z0 + 1
            Bw[1:ny + 1, oz:oz + sb.shape[1], ox:ox + sb.shape[2]] = sb
            Mw[1:ny + 1, oz:oz + sm.shape[1], ox:ox + sm.shape[2]] = sm
    return Bw, Mw


def _lut(s, dtype=bool):
    t = np.zeros(256, dtype)
    for k in s:
        t[k] = 1
    return t


OPQ = _lut(OPAQUE)
CUBE = _lut(CUBE_LIKE)
GRP = np.zeros(256, np.uint8)
for _k, _g in GROUP.items():
    GRP[_k] = _g


def _conn(Bw):
    """World-space connection bits: fences (e1 w2 s4 n8) and wire."""
    C = np.zeros(Bw.shape, np.uint8)
    for bid in (85, 55):
        idx = np.argwhere(Bw == bid)
        for (y, z, x) in idx:
            bits = 0
            for d, bit in (((1, 0), 1), ((-1, 0), 2), ((0, 1), 4), ((0, -1), 8)):
                nz, nx = z + d[1], x + d[0]
                if not (0 <= nz < Bw.shape[1] and 0 <= nx < Bw.shape[2]):
                    continue
                nb = Bw[y, nz, nx]
                if bid == 85:
                    if nb == 85:
                        bits |= bit
                    continue
                if nb in WIRE_LINKS:
                    bits |= bit
                elif (y + 1 < Bw.shape[0] and Bw[y + 1, nz, nx] == 55
                      and not OPQ[Bw[y + 1, z, x]]):
                    bits |= bit
                elif (y - 1 >= 0 and Bw[y - 1, nz, nx] == 55 and not OPQ[nb]):
                    bits |= bit
            C[y, z, x] = bits
    return C


def _to_view(arr, view):
    """[y, z, x] world array -> [y, v, u] view array."""
    if view == "se":
        return arr
    if view == "nw":
        return arr[:, ::-1, ::-1]
    if view == "ne":   # u = -z, v = x
        return arr.transpose(0, 2, 1)[:, :, ::-1]
    if view == "sw":   # u = z, v = -x
        return arr.transpose(0, 2, 1)[:, ::-1, :]
    raise ValueError(view)


def scene_arrays(world, x0, z0, x1, z1, ymin, ymax, view, hide=()):
    Bw, Mw = world_box(world, x0, z0, x1, z1, ymin, ymax)
    if hide:  # x-ray: treat these ids as air
        Bw[np.isin(Bw, list(hide))] = 0
    Cw = _conn(Bw)
    B = np.ascontiguousarray(_to_view(Bw, view))
    M = np.ascontiguousarray(_to_view(Mw, view))
    C = np.ascontiguousarray(_to_view(Cw, view))

    def exposed(nb):
        same = (GRP[nb] == GRP[B]) & (GRP[B] != 0)
        return ~OPQ[nb] & ~same

    mask = np.zeros(B.shape, np.uint8)
    up = np.zeros_like(B)
    up[:-1] = B[1:]
    pu = np.zeros_like(B)
    pu[:, :, :-1] = B[:, :, 1:]
    pv = np.zeros_like(B)
    pv[:, :-1, :] = B[:, 1:, :]
    mask |= exposed(up).astype(np.uint8)
    mask |= exposed(pu).astype(np.uint8) << 1
    mask |= exposed(pv).astype(np.uint8) << 2
    liquid = (B >= 8) & (B <= 11)
    # liquid top bit doubles as "lower surface" flag inside the model
    C = np.where(liquid, mask & 1, C).astype(np.uint8)
    keep = (B != 0) & (~CUBE[B] | (mask != 0))
    B = np.where(keep, B, 0).astype(np.uint8)
    # drop the pad
    sl = (slice(1, -1), slice(1, -1), slice(1, -1))
    return B[sl], M[sl], mask[sl], C[sl]


# -------------------------------------------------------------- annotation --
def _font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def annotate(img, title, view):
    d = ImageDraw.Draw(img, "RGBA")
    fs = max(16, img.width // 45)
    font = _font(fs)
    if title:
        tw = d.textlength(title, font=font)
        d.rounded_rectangle((10, 10, 10 + tw + 2 * fs // 2, 10 + fs * 1.6),
                            radius=fs // 3, fill=(0, 0, 0, 140))
        d.text((10 + fs // 2, 10 + fs * 0.25), title, font=font, fill=(255, 255, 255))
    # compass: screen direction of world north (-z)
    a1, b1, a2, b2 = VIEWS[view]
    u, v = -b1, -b2
    sx, sy = (u - v), (u + v) / 2.0
    ln = math.hypot(sx, sy) or 1
    sx, sy = sx / ln, sy / ln
    r = fs * 1.9
    cx, cy = img.width - r - 14, img.height - r - 14
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(0, 0, 0, 130))
    # needle: red half points north, white half south; "N" sits on the red tip
    tip = (cx + sx * r * 0.45, cy + sy * r * 0.45)
    tail = (cx - sx * r * 0.85, cy - sy * r * 0.85)
    left = (cx - sy * r * 0.22, cy + sx * r * 0.22)
    right = (cx + sy * r * 0.22, cy - sx * r * 0.22)
    d.polygon([tail, left, right], fill=(235, 235, 235))
    d.polygon([tip, left, right], fill=(230, 60, 50))
    tx, ty = cx + sx * r * 0.7, cy + sy * r * 0.7
    d.text((tx, ty), "N", font=font, fill=(255, 255, 255), anchor="mm")
    return img


def finish(img, max_w):
    if img.width > max_w:
        h = int(img.height * max_w / img.width)
        img = img.resize((max_w, h), Image.LANCZOS)
    return img


def render_view(world, atlas, box_xz, view="se", ymin=58, ymax=127,
                target_w=1400, title=None, max_a=48, supersample=2, hide=()):
    x0, z0, x1, z1 = box_xz
    span = (x1 - x0 + 1) + (z1 - z0 + 1)
    a = max(3, min(max_a, int(supersample * target_w / span)))
    r = Renderer(atlas, a, view)
    t = time.time()
    img, n = r.render(world, box_xz, ymin, ymax, hide)
    img = finish(img, target_w)
    annotate(img, title, view)
    return img, {"blocks": n, "a": a, "secs": round(time.time() - t, 1),
                 "sprites": len(r.cache)}


# ------------------------------------------------------- first person view --
# Approximate eye-level perspective (ray-cast voxels, no lighting), for
# "what does the player see from here" questions (spawn view, a gate view).
# Cube-like blocks are drawn with their face textures; slabs and stairs are
# drawn as full cubes; small models (torches, signs, rails, fences, plants,
# glass) are skipped. Distance haze ends at `fog` (the Wii's far plane is
# about 96 blocks).
FP_SKIP = {0, 6, 20, 26, 27, 28, 30, 31, 32, 37, 38, 39, 40, 50, 51, 55, 59,
           63, 64, 65, 66, 68, 69, 70, 71, 72, 75, 76, 77, 83, 85, 93, 94, 96}


def _fp_face_tex(atlas, cache, bid, meta, d):
    key = (bid, meta, d)
    t = cache.get(key)
    if t is not None:
        return t
    tex = None
    for prim in model(bid, meta, 1):
        if prim and prim[0] == "box":
            f = prim[2].get(d) or prim[2].get("up")
            if f is not None:
                tex = atlas.tex(f.tile, f.tint, 1.0, 0, f.overlay)
            break
    if tex is None:
        tex = Image.new("RGBA", (atlas.tile_px, atlas.tile_px), (255, 0, 255, 255))
    t = np.asarray(tex.convert("RGBA"), np.float32)
    cache[key] = t
    return t


def render_fp(world, atlas, eye, yaw, pitch, width=1200, height=675,
              fov=70.0, maxd=120, fog=(56.0, 104.0), ss=1.5, title=None):
    """Perspective view from `eye` (x, y, z: the eye position), Beta yaw
    (0 = facing +z, 90 = -x, 180 = -z) and pitch in degrees (positive =
    looking up); `fov` is the vertical field of view."""
    W, H = int(width * ss), int(height * ss)
    ex, ey, ez = eye
    x0, x1 = int(ex - maxd), int(ex + maxd)
    z0, z1 = int(ez - maxd), int(ez + maxd)
    y0, y1 = 0, 127
    Bw, Mw = world_box(world, x0, z0, x1, z1, y0, y1)   # [y, z, x], padded
    solid = np.ones(256, bool)
    solid[list(FP_SKIP)] = False
    ya, pa = math.radians(yaw), math.radians(pitch)
    fwd = np.array([-math.sin(ya) * math.cos(pa), math.sin(pa),
                    math.cos(ya) * math.cos(pa)])
    right = np.cross(fwd, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    th = math.tan(math.radians(fov) / 2)
    tw = th * W / H
    u = (np.arange(W) + 0.5) / W * 2 - 1
    v = 1 - (np.arange(H) + 0.5) / H * 2
    uu, vv = np.meshgrid(u * tw, v * th)
    d = (fwd[None, None, :] + uu[..., None] * right[None, None, :]
         + vv[..., None] * up[None, None, :]).reshape(-1, 3)
    d /= np.linalg.norm(d, axis=1)[:, None]
    n = d.shape[0]
    # local voxel coords: array index = world - origin + 1 (pad)
    org = np.array([x0 - 1, y0 - 1, z0 - 1], np.float64)
    p = np.array([ex, ey, ez]) - org
    cell = np.floor(np.broadcast_to(p, (n, 3))).astype(np.int64)
    step = np.where(d >= 0, 1, -1).astype(np.int64)
    with np.errstate(divide="ignore", invalid="ignore"):
        inv = np.where(d != 0, 1.0 / d, np.inf)
    nxt = cell + (step > 0)
    tmax = np.abs((nxt - p) * inv)
    tmax[~np.isfinite(tmax)] = np.inf
    tdel = np.abs(inv)
    hit_b = np.zeros(n, np.uint8)
    hit_m = np.zeros(n, np.uint8)
    hit_ax = np.full(n, -1, np.int8)
    hit_t = np.full(n, np.inf)
    act = np.arange(n)
    ax_last = np.full(n, -1, np.int8)
    t_now = np.zeros(n)
    shp = np.array([Bw.shape[2], Bw.shape[0], Bw.shape[1]])   # x, y, z
    for _ in range(int(maxd * 3)):
        if act.size == 0:
            break
        c = cell[act]
        inside = np.all((c >= 0) & (c < shp), axis=1)
        b = np.zeros(act.size, np.uint8)
        ci = c[inside]
        b[inside] = Bw[ci[:, 1], ci[:, 2], ci[:, 0]]
        hit = solid[b] & (t_now[act] > 0) & inside
        if hit.any():
            h = act[hit]
            hit_b[h] = b[hit]
            hc = c[hit]
            hit_m[h] = Mw[hc[:, 1], hc[:, 2], hc[:, 0]]
            hit_ax[h] = ax_last[h]
            hit_t[h] = t_now[h]
        keep = ~hit & (t_now[act] < maxd) & (inside | (t_now[act] == 0))
        act = act[keep]
        tm = tmax[act]
        ax = np.argmin(tm, axis=1)
        r = np.arange(act.size)
        t_now[act] = tm[r, ax]
        cell[act, ax] += step[act, ax]
        tmax[act, ax] += tdel[act, ax]
        ax_last[act] = ax
    # shade
    img = np.zeros((n, 3), np.float32)
    f = np.linspace(0, 1, H)[:, None]
    sky = (np.array(SKY_TOP)[None, :] * (1 - f) + np.array(SKY_BOT)[None, :] * f)
    sky = np.repeat(sky[:, None, :], W, axis=1).reshape(-1, 3).astype(np.float32)
    img[:] = sky
    got = np.where(hit_ax >= 0)[0]
    if got.size:
        t = hit_t[got]
        hp = p[None, :] + d[got] * t[:, None]
        ax = hit_ax[got]
        sgn = step[got, ax]
        names = {(0, 1): "w", (0, -1): "e", (1, 1): "down", (1, -1): "up",
                 (2, 1): "n", (2, -1): "s"}
        fx, fy, fz = hp[:, 0] % 1, hp[:, 1] % 1, hp[:, 2] % 1
        tu = np.where(ax == 0, np.where(sgn > 0, fz, 1 - fz),
                      np.where(ax == 2, np.where(sgn > 0, 1 - fx, fx), fx))
        tv = np.where(ax == 1, fz, 1 - fy)
        tp = atlas.tile_px
        iu = np.clip((tu * tp).astype(int), 0, tp - 1)
        iv = np.clip((tv * tp).astype(int), 0, tp - 1)
        col = np.zeros((got.size, 3), np.float32)
        cache = {}
        keys = np.stack([hit_b[got].astype(np.int64), hit_m[got].astype(np.int64),
                         ax.astype(np.int64), sgn.astype(np.int64)], 1)
        uk, inv_k = np.unique(keys, axis=0, return_inverse=True)
        inv_k = inv_k.reshape(-1)
        for k, (bb, mm, aa, ss_) in enumerate(uk):
            sel = inv_k == k
            tex = _fp_face_tex(atlas, cache, int(bb), int(mm),
                               names[(int(aa), int(ss_))])
            px = tex[iv[sel], iu[sel]]
            a = px[:, 3:4] / 255.0
            base = tex[..., :3].reshape(-1, 3)[tex[..., 3].reshape(-1) > 0]
            fill = base.mean(axis=0) * 0.7 if base.size else np.zeros(3)
            col[sel] = px[:, :3] * a + fill[None, :] * (1 - a)
        shade = np.where(ax == 1, np.where(sgn < 0, 1.0, 0.5),
                         np.where(ax == 0, 0.8, 0.64)).astype(np.float32)
        col *= shade[:, None]
        fo = np.clip((t - fog[0]) / (fog[1] - fog[0]), 0, 1)[:, None]
        img[got] = col * (1 - fo) + img[got] * fo
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8).reshape(H, W, 3))
    if ss != 1:
        out = out.resize((width, height), Image.LANCZOS)
    if title:
        d2 = ImageDraw.Draw(out, "RGBA")
        fs = max(16, out.width // 45)
        font = _font(fs)
        tw2 = d2.textlength(title, font=font)
        d2.rounded_rectangle((10, 10, 10 + tw2 + fs, 10 + fs * 1.6),
                             radius=fs // 3, fill=(0, 0, 0, 140))
        d2.text((10 + fs // 2, 10 + fs * 0.25), title, font=font,
                fill=(255, 255, 255))
    return out


# ------------------------------------------------------------- zone boxes --
def zone_boxes():
    """The 15 zone boxes from build_tutorial.write_previews (parsed, not run)."""
    src = open(os.path.join(HERE, "build_tutorial.py"), encoding="utf-8").read()
    for node in ast.walk(ast.parse(src)):
        if isinstance(node, ast.FunctionDef) and node.name == "write_previews":
            for st in ast.walk(node):
                if (isinstance(st, ast.Assign) and len(st.targets) == 1
                        and getattr(st.targets[0], "id", None) == "zones"):
                    z = ast.literal_eval(st.value)
                    return {k: v[0] for k, v in z.items()}
    raise SystemExit("zone table not found in build_tutorial.write_previews")


CAPTIONS = {
    "beach": "Stop 1, Beach landing: look, walk, jump. The player spawns here.",
    "woods": "Stop 2, Woods: punch a tree for logs.",
    "crafting": "Stop 3, Crafting shed: planks, sticks, crafting table.",
    "tools": "Stop 4, Tool bench: pickaxe, axe, shovel, sword.",
    "mine": "Stop 5, Mine: a quarry cut 10 deep below the CAVEX crest; the face "
            "shows the ore bands; a spiral mine goes down to the true ore depths "
            "(gallery at y 9).",
    "torch_tunnel": "Stop 6, Torch tunnel: a dark cobble corridor with a slab floor. "
                    "Craft torches and light the way.",
    "smelting": "Stop 7, Smelting yard: furnace, coal, iron; smelt stone for a button.",
    "farm": "Stop 8, Farm: hoe, seeds, water, wheat stages, sugar cane, pumpkins.",
    "cottage": "Stop 9, Cottage: shelter, a door, and light inside.",
    "bed_house": "Stop 10, Bed house and hub gate: craft a bed, sleep, press a button.",
    "hub": "Hub plaza on the castle hill: the trail ends here; the three rooms "
           "open off it; a four-way signpost shows the way.",
    "redstone_hall": "Redstone hall (north room): torch-repeater clock driving a door, "
                     "and a repeater ripple.",
    "cart_yard": "Minecart yard (west, open air): a rail loop with a station bell.",
    "monster_lab": "Monster lab (south room): dark side vs lit side.",
    "bridge_islet": "Piston bridge across the causeway, then the Redstone islet "
                    "(north-east).",
}
# extra cutaways: name -> list of (label, box, ymin, ymax, hidden ids)
MINE_BOX = (222, 199, 240, 216)
CUTAWAYS = {   # V3: the mine sits 3 blocks lower than in V2
    "mine": [("x-ray: stone, dirt, gravel, plants hidden (whole spiral + ores)",
              MINE_BOX, 5, 75, (1, 2, 3, 13, 31, 37, 38)),
             ("slice y 31-38 (iron band on the spiral wall)", MINE_BOX, 31, 38, ()),
             ("slice y 3-11 (bottom gallery, floor y 9)", MINE_BOX, 3, 11, ())],
}
BUILDING_IGNORE = {0, 1, 2, 3, 8, 9, 12, 13, 15, 16, 17, 18, 31, 37, 38, 50,
                   63, 68, 85}


def has_roof(world, box_xz, y=69):
    x0, z0, x1, z1 = box_xz
    n = 0
    for x in range(x0, x1 + 1):
        for z in range(z0, z1 + 1):
            for yy in range(y, y + 6):
                if world.get(x, yy, z)[0] not in BUILDING_IGNORE:
                    n += 1
    return n >= 12


# ------------------------------------------------------------------- html --
def jpeg_b64(img, q):
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=q, optimize=True, progressive=True)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def write_html(path, sections, q=80):
    css = """
:root{--bg:#f4f1ea;--fg:#1d1d1f;--muted:#5d5a55;--card:#fff;--line:#ddd6c8;--acc:#2f6d3a}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#15171a;--fg:#ececec;--muted:#a3a3a3;--card:#1f2226;--line:#30343a;--acc:#7cc489}}
:root[data-theme=dark]{--bg:#15171a;--fg:#ececec;--muted:#a3a3a3;--card:#1f2226;--line:#30343a;--acc:#7cc489}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{padding:18px 16px 8px;max-width:1100px;margin:auto}h1{font-size:1.35rem;margin:0 0 4px}
.sub{color:var(--muted);font-size:.9rem}nav{display:flex;flex-wrap:wrap;gap:6px;padding:8px 16px;max-width:1100px;margin:auto}
nav a{font-size:.82rem;padding:4px 10px;border:1px solid var(--line);border-radius:999px;color:var(--fg);text-decoration:none;background:var(--card)}
main{max-width:1100px;margin:auto;padding:0 16px 40px}section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin:14px 0}
h2{font-size:1.1rem;margin:2px 0 4px}p{margin:0 0 10px;color:var(--muted)}figure{margin:0 0 12px}figure img{width:100%;height:auto;display:block;border-radius:8px}
figcaption{font-size:.8rem;color:var(--muted);margin-top:4px}button{position:fixed;right:12px;top:12px;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:999px;padding:6px 12px;font-size:.85rem}
"""
    out = ["<!doctype html><html lang=en><head><meta charset=utf-8>",
           "<meta name=viewport content='width=device-width,initial-scale=1'>",
           "<title>Tutorial Island 3D</title><style>%s</style></head><body>" % css,
           "<button onclick=\"var r=document.documentElement;r.dataset.theme="
           "(r.dataset.theme==='dark'||(!r.dataset.theme&&matchMedia('(prefers-color-scheme:dark)').matches))"
           "?'light':'dark'\">theme</button>",
           "<header><h1>Tutorial Island: 3D previews</h1><div class=sub>Offline isometric "
           "render of the generated world (tools/tutorial_world/iso_render.py), "
           "default Earth &amp; Ash textures. Review aid only: it is not an in-game "
           "capture (no lighting, simplified non-cube models).</div></header><nav>"]
    for s in sections:
        out.append("<a href='#%s'>%s</a>" % (s["id"], s["title"]))
    out.append("</nav><main>")
    for s in sections:
        out.append("<section id='%s'><h2>%s</h2><p>%s</p>" % (s["id"], s["title"], s["caption"]))
        for (img, cap) in s["images"]:
            out.append("<figure><img loading=lazy alt='%s' src='data:image/jpeg;base64,%s'>"
                       "<figcaption>%s</figcaption></figure>" % (cap, jpeg_b64(img, q), cap))
        out.append("</section>")
    out.append("</main></body></html>")
    data = "".join(out)
    with open(path, "w", encoding="utf-8") as f:
        f.write(data)
    return len(data.encode("utf-8"))


# ------------------------------------------------------------------- main --
def load(save):
    if save:
        return wl.load_world(save)
    import build_tutorial
    return build_tutorial.build().w


def run_all(world, atlas, out_dir, html=True, views=("se", "nw"),
            budget=14.5e6):
    os.makedirs(out_dir, exist_ok=True)
    sections = []
    ov = []
    for v in views:
        img, st = render_view(world, atlas, (160, 180, 368, 330), v, ymin=54,
                              target_w=1600, title="Tutorial Island, " + VIEW_NAMES[v],
                              max_a=10)
        p = os.path.join(out_dir, "overview_%s.png" % v)
        img.save(p)
        print(p, img.size, st)
        ov.append((img, "Whole island " + VIEW_NAMES[v] + " (high angle)"))
    sections.append({"id": "overview", "title": "Overview", "images": ov,
                     "caption": "The whole island: beach landing in the south-west, "
                                "the trail loops north and east to the hub; mine hill "
                                "north-west, Redstone islet north-east."})
    for name, bx in zone_boxes().items():
        bx = (bx[0] - 2, bx[1] - 2, bx[2] + 2, bx[3] + 2)
        imgs = []
        pretty = name.replace("_", " ").title()
        for v in views:
            img, st = render_view(world, atlas, bx, v, ymin=58, target_w=1300,
                                  title=pretty + ", " + VIEW_NAMES[v])
            p = os.path.join(out_dir, "zone_%s_%s.png" % (name, v))
            img.save(p)
            print(p, img.size, st)
            imgs.append((img, pretty + " " + VIEW_NAMES[v]))
        if has_roof(world, bx):
            img, st = render_view(world, atlas, bx, "se", ymin=58, ymax=68,
                                  target_w=1300, title=pretty + ", roof off (y <= 68)")
            p = os.path.join(out_dir, "zone_%s_roofoff.png" % name)
            img.save(p)
            print(p, img.size, st)
            imgs.append((img, pretty + ": roof off, everything above y 68 removed"))
        for (label, cbx, y0, y1, hide) in CUTAWAYS.get(name, []):
            img, st = render_view(world, atlas, cbx, "se", ymin=y0, ymax=y1,
                                  target_w=1300, title=pretty + ", " + label.split(" (")[0],
                                  hide=hide)
            p = os.path.join(out_dir, "zone_%s_y%d.png" % (name, y0))
            img.save(p)
            print(p, img.size, st)
            imgs.append((img, pretty + ": " + label))
        sections.append({"id": name, "title": pretty, "images": imgs,
                         "caption": CAPTIONS.get(name, "")})
    if html:
        path = os.path.join(out_dir, "index.html")
        for q in (80, 72, 64, 56):
            size = write_html(path, sections, q)
            if size <= budget:
                break
        print("html", path, size, "bytes")
        return path, size
    return None, 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--save", help="save directory (default: build the tutorial in memory)")
    ap.add_argument("--atlas", default=DEFAULT_ATLAS)
    ap.add_argument("--box", help="x0,z0,x1,z1 (one image)")
    ap.add_argument("--ymin", type=int, default=58)
    ap.add_argument("--ymax", type=int, default=127)
    ap.add_argument("--view", default="se", choices=sorted(VIEWS))
    ap.add_argument("--elev", choices=sorted(ELEV_NAMES), default=None,
                    help="with --box: orthographic front elevation seen from "
                         "this side (s = from the south looking north)")
    ap.add_argument("--width", type=int, default=1400)
    ap.add_argument("--out", default=None, help="PNG path (--box) or directory (--all)")
    ap.add_argument("--hide", default="", help="comma ids drawn as air (x-ray)")
    ap.add_argument("--fp", default=None, help="first-person view: "
                    "x,y,z,yaw,pitch (eye position, Beta yaw, pitch up)")
    ap.add_argument("--all", action="store_true", help="overview + every zone")
    ap.add_argument("--html", action="store_true", help="with --all: write index.html")
    a = ap.parse_args(argv)
    atlas = Atlas(a.atlas)
    world = load(a.save)
    if a.fp:
        x, y, z, yaw, pitch = (float(v) for v in a.fp.split(","))
        img = render_fp(world, atlas, (x, y, z), yaw, pitch, a.width,
                        int(a.width * 9 / 16))
        out = a.out or os.path.join(DEFAULT_OUT, "fp.png")
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        img.save(out)
        print(out, img.size)
        return
    if a.box:
        bx = tuple(int(v) for v in a.box.split(","))
        hide = tuple(int(v) for v in a.hide.split(",") if v.strip())
        if a.elev:
            img, st = render_elev(world, atlas, bx, a.elev, a.ymin, a.ymax,
                                  a.width)
            out = a.out or os.path.join(DEFAULT_OUT, "elev_%s.png" % a.elev)
            os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
            img.save(out)
            print(out, img.size, st)
            return
        img, st = render_view(world, atlas, bx, a.view, a.ymin, a.ymax, a.width,
                              title=None, hide=hide)
        out = a.out or os.path.join(DEFAULT_OUT, "box_%s.png" % a.view)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        img.save(out)
        print(out, img.size, st)
        return
    run_all(world, atlas, a.out or DEFAULT_OUT, html=a.html or a.all)


if __name__ == "__main__":
    main()
