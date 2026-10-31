#!/usr/bin/env python3
"""Generate assets/gui/chest.png with BETA 1.7.3 GEOMETRY and CaveWii's own pixels.

WHY. Beta's gui/container.png is a SIX-row DOUBLE-chest sheet: the chest block occupies
source v=0..125 and a self-contained 96px player panel occupies v=126..221, and GuiChest
draws the window as two rects that SKIP the unused chest rows for a small chest
(GuiChest.java:32-33). The bundled fork asset was authored to the FORK's slot coordinates
instead -- three chest rows at v=16/34/52, player rows at 84/102/120, hotbar 142, and
nothing at all below v=165 -- so Beta's draw path would sample ~56 rows of transparency
from it. source/platform/texture.c:132-133 asserts the bundled assets are "laid out to
match" Beta so a real pack fully overrides them; the chest was the one sheet where that
invariant was never honoured.

This regenerates it to Beta's GEOMETRY -- cell pitch, row/column origins, panel band --
while drawing CaveWii's own art (the palette is sampled from the fork's previous sheet, so
the window still looks like this game). ZERO Mojang bytes: nothing here is copied from a
Minecraft asset, which keeps the M3(B) legal-pack constraint intact.

Geometry (all verified against the real Beta sheet by decoding it, not by assumption):
  window            176 x 222 inside a 256x256 sheet
  chest cell grid   origin (7, 17), pitch 18, 9 columns x 6 rows
                    -> dark bevel rows v=17,35,53,71,89,107; columns u=7,25,...,151
  player panel      band v=126..221; main rows origin (7,139) pitch 18 x3
                    -> bevels v=139,157,175 ; hotbar row origin (7,197)
  A cell is an 18x18 footprint: 1px dark border on the top/left edges shared with its
  neighbours, and a 16x16 lighter interior at +1,+1.

Usage: python tools/gen_chest_gui.py [out.png]
"""
import sys
from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else "assets/gui/chest.png"

SHEET = 256
W, H = 176, 222

# Palette sampled from the fork's previous assets/gui/chest.png so the window's look is
# unchanged -- only its geometry moves.
BG = (128, 128, 128, 255)        # panel background
GRID = (97, 97, 97, 255)         # 1px cell bevel (the recessed border)
CELL = (139, 139, 139, 255)      # cell interior, one step lighter than the panel
HI = (200, 200, 200, 255)        # bottom/right cell bevel -- the "sunken" highlight
EDGE = (43, 33, 20, 255)         # outer frame, the fork's dark brown
EDGE_HI = (168, 168, 168, 255)   # inner highlight just inside the frame

im = Image.new("RGBA", (SHEET, SHEET), (0, 0, 0, 0))
px = im.load()


def rect(x0, y0, x1, y1, colour):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px[x, y] = colour


def cell_grid(origin_x, origin_y, cols, rows):
    """A cols x rows block of 18px cells, in Beta's exact sunken-cell structure.

    Decoded from the real sheet at u=88, v=14..36 rather than assumed:
        v=17      dark border   (1px)
        v=18..33  interior     (16px)
        v=34      light bevel   (1px)
        v=35      the NEXT cell's dark border
    i.e. an 18px footprint = 1 dark + 16 interior + 1 light, with cells stacking directly
    so the dark borders land on exactly 17+18r and the columns on 7+18c. An earlier draft
    filled the block and punched out the interiors, which left a TWO-pixel border between
    rows and put row 1's bevel at v=34/35 instead of 35 -- caught by check [6].
    """
    for r in range(rows):
        for c in range(cols):
            x0 = origin_x + c * 18
            y0 = origin_y + r * 18
            rect(x0, y0, x0 + 17, y0, GRID)              # top edge
            rect(x0, y0, x0, y0 + 17, GRID)              # left edge
            rect(x0 + 1, y0 + 1, x0 + 16, y0 + 16, CELL) # 16x16 interior
            rect(x0 + 1, y0 + 17, x0 + 17, y0 + 17, HI)  # bottom bevel
            rect(x0 + 17, y0 + 1, x0 + 17, y0 + 17, HI)  # right bevel


# --- window body ---------------------------------------------------------------------
rect(0, 0, W - 1, H - 1, BG)
# outer frame: 3px dark edge with a 1px highlight inside it (the fork's look)
rect(0, 0, W - 1, 2, EDGE)
rect(0, H - 3, W - 1, H - 1, EDGE)
rect(0, 0, 2, H - 1, EDGE)
rect(W - 3, 0, W - 1, H - 1, EDGE)
rect(3, 3, W - 4, 3, EDGE_HI)
rect(3, 3, 3, H - 4, EDGE_HI)

# --- chest block: 9 x 6 cells at (7,17) ------------------------------------------------
# Six rows is what makes ONE sheet serve both chest sizes, exactly as Beta does: a small
# chest draws only the first three and skips source rows 71..125.
cell_grid(7, 17, 9, 6)

# --- player panel: the self-contained v=126..221 band ----------------------------------
# Beta blits this whole band as the second rect, so it must carry its own top edge.
rect(3, 126, W - 4, 126, EDGE_HI)
cell_grid(7, 139, 9, 3)   # main inventory rows -> bevels at 139/157/175
cell_grid(7, 197, 9, 1)   # hotbar             -> bevel at 197

im.save(OUT)
print(f"wrote {OUT}  ({W}x{H} used in {SHEET}x{SHEET})")
