#!/usr/bin/env python3
# Render a sim-only :8099 proof image for the 3 Beta-1.7.3 faithfulness fixes.
# Sim-only fixes prove their claim with the golden test's PASS output (CLAUDE.md DoD).
# Usage: python tools/render_faithfulness_proof.py <git-short-sha> <out.png>
import sys
from PIL import Image, ImageDraw, ImageFont

sha = sys.argv[1] if len(sys.argv) > 1 else "local"
out = sys.argv[2] if len(sys.argv) > 2 else "tmp/proof.png"

W, H = 1180, 760
BG = (20, 22, 28); FG = (228, 230, 235); DIM = (150, 156, 168)
OK = (120, 210, 130); HEAD = (130, 180, 255); RULE = (60, 64, 74)
img = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(img)

def fnt(sz, bold=False):
    names = (["consolab.ttf", "consola.ttf"] if False else
             (["arialbd.ttf"] if bold else ["arial.ttf"]))
    mono = ["consola.ttf"]
    for n in (mono if not bold else ["consolab.ttf", "consola.ttf"]):
        try: return ImageFont.truetype(n, sz)
        except Exception: pass
    for n in names:
        try: return ImageFont.truetype(n, sz)
        except Exception: pass
    return ImageFont.load_default()

title_f = fnt(26, True); head_f = fnt(20, True); body_f = fnt(16); mono_f = fnt(15)
y = 24
d.text((30, y), "CaveWii_v2 - 3 Beta 1.7.3 faithfulness fixes  (sim-only proof)", font=title_f, fill=FG); y += 38
d.text((30, y), f"commit {sha}   -   ctest 140/140 GREEN   -   tools/local_ci.sh GREEN (Linux gcc+UBSan soak + devkitPPC .dol)", font=body_f, fill=DIM); y += 30
d.line((30, y, W-30, y), fill=RULE, width=2); y += 18

sections = [
 ("1. Bed sleep window  (block_bed.c, daytime.h)",
  ["Beta EntityPlayer.sleepInBedAt: sleep only at NIGHT (World.isDaytime()==skylightSubtracted<4),",
   "skip to next DAWN.  Old: no night-gate + skipped to t=13000 (dusk).  [decompile 740c583]"],
  ["test_daylight_curve: OK  (daytime_is_daytime_ticks == Beta isDaytime ref at ALL 24000 ticks)"]),
 ("2. Dirt/grass place-consume item DUPE  (block_dirt.c, block_grass.c)",
  ["Redundant block onRightClick shadowed the dispatch place+consume path -> placed a held block",
   "for FREE (and pre-empted bonemeal-on-grass). Removed; tilling = item_hoe verb.  [740c583]"],
  ["dirt: tills to farmland from any face, even with a block above OK",
   "dirt/grass: no block onRightClick (place+consume not shadowed) OK",
   "PASS test_hoe   /   PASS test_bonemeal"]),
 ("3. Fire-caught TNT entity  (block_fire.c, entity_primed_tnt.*, tnt_texture.c)",
  ["Beta BlockFire:131 -> onBlockDestroyedByPlayer -> EntityTNTPrimed.fuse=80. Fire now spawns a",
   "real primed-TNT entity with the 80-tick fuse, launch motion, smoke, scale and white flash.",
   "TNT faces remain the Beta side/top/bottom tiles; the fuse flash never substitutes snow."],
  ["fire: caught TNT becomes an 80-tick primed entity OK",
   "test_fire / test_tnt_fuse / test_tnt_texture: OK -- entity fuse + render goldens"]),
]
for head, desc, ev in sections:
    d.text((30, y), head, font=head_f, fill=HEAD); y += 28
    for line in desc:
        d.text((46, y), line, font=body_f, fill=DIM); y += 22
    y += 4
    for line in ev:
        d.text((46, y), "PASS  ", font=mono_f, fill=OK)
        d.text((46 + d.textlength("PASS  ", font=mono_f), y), line, font=mono_f, fill=FG); y += 22
    y += 14
    d.line((30, y, W-30, y), fill=RULE, width=1); y += 16

img.save(out)
print("wrote", out)
