#!/usr/bin/env python3
"""Composite procedural fire/lava filmstrips into a CaveWii anim.png.

Reads 28-frame RGBA bins produced by fx_gen.c (flame_fx / lava_fx / portal_fx)
and paints them into the anim.png cells the render/scroll sample:
  fire = columns 3 & 10 (TEXTURE_INDEX(3,0)); fire+16 = columns 6 & 13 (Beta's
  second TextureFlamesFX, flame_fx_pair instance 1 -- flame_fx.h FLAME_FX_ANIM_COL1);
  lava = columns 2 & 9 (fancy (2,0));
  portal = columns 0 & 7 (TEXTURE_INDEX(0,0) -- the block_portal cell).
Each animated tile's 28-frame strip = column C rows 0..13 then column C+7 rows 0..13
(world.c matrix_anim: anim = 0..27, Y=anim%14, X-phase=+7 cols). Cells are 18x18
(16px content at +1, 1px clamped/edge-replicated border) -- identical geometry to
make_beta_pack.ps1's ice composite.
"""
import sys
from PIL import Image

SCR = sys.argv[1]
BASE = sys.argv[2]
DESTS = sys.argv[3:]
FRAMES = 28


def load_frames(name):
    d = open(SCR + "/" + name, "rb").read()
    return [Image.frombytes("RGBA", (16, 16), d[i * 1024:(i + 1) * 1024])
            for i in range(FRAMES)]


def paste_cell(canvas, tile, col, row):
    """18x18 cell at (col*18+2,row*18+2): inner 16x16 = tile, 1px clamped border."""
    ox, oy = col * 18 + 2, row * 18 + 2
    t = tile.load()
    px = canvas.load()
    for di in range(18):
        si = min(max(di - 1, 0), 15)
        for dj in range(18):
            sj = min(max(dj - 1, 0), 15)
            px[ox + di, oy + dj] = t[si, sj]


def composite(base_path):
    im = Image.open(base_path).convert("RGBA")
    fire = load_frames("flame_frames.bin")
    fire2 = load_frames("flame2_frames.bin")
    water = load_frames("water_frames.bin")
    water_flow = load_frames("water_flow_frames.bin")
    lava = load_frames("lava_frames.bin")
    portal = load_frames("portal_frames.bin")
    for f in range(FRAMES):
        # frame f: f<14 -> (col C, row f); else -> (col C+7, row f-14)
        if f < 14:
            fcol, f2col, lcol, pcol, wcol, wfcol, r = 3, 6, 2, 0, 1, 5, f
        else:
            fcol, f2col, lcol, pcol, wcol, wfcol, r = 10, 13, 9, 7, 8, 12, f - 14
        paste_cell(im, fire[f], fcol, r)
        paste_cell(im, fire2[f], f2col, r)
        paste_cell(im, lava[f], lcol, r)
        paste_cell(im, portal[f], pcol, r)
        # WATER, both tiles, RESTORED 2026-07-31. The 07-30 revert was a
        # MISDIAGNOSIS: it disabled the STILL bake because the maintainer's
        # photos showed "pale speckle + a glaring seam against the fork flow
        # tile". Measuring the atlases against the decompile settled it -- the
        # baked still tile was byte-correct Beta (TextureWaterFX: R 32..64,
        # G 50..114, B pinned 255) and the FORK FLOW TILE was the broken half
        # (blue channel 75..91, i.e. grey). The seam was the two disagreeing,
        # and the correct half got reverted. Beta colours flow with the exact
        # same mapping as still (TextureWaterFlowFX shares the eight colour
        # lines verbatim), so with both baked from the ports the sides finally
        # match the tops.
        paste_cell(im, water[f], wcol, r)
        paste_cell(im, water_flow[f], wfcol, r)
    return im


result = composite(BASE)
for d in DESTS:
    result.save(d)
    print("wrote", d)
