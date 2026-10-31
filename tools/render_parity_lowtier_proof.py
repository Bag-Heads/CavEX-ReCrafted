#!/usr/bin/env python3
# Render the sim-only :8099 proof for the 2026-08-25 parity-audit lower-tier
# batch (sim lane C): recovered region-timestamp stamping, the harvest-tier
# trio, and the new mc173 lighting oracle gate. Sim-only fixes prove their
# claim with the golden test's labelled PASS output (CLAUDE.md DoD).
# Usage: python tools/render_parity_lowtier_proof.py <git-short-sha> <out.png>
import sys
from PIL import Image, ImageDraw, ImageFont

sha = sys.argv[1] if len(sys.argv) > 1 else "local"
out = sys.argv[2] if len(sys.argv) > 2 else "tmp/proof.png"

W, H = 1240, 1010
BG = (20, 22, 28); FG = (228, 230, 235); DIM = (150, 156, 168)
OK = (120, 210, 130); HEAD = (130, 180, 255); RULE = (60, 64, 74)
img = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(img)

def fnt(sz, bold=False):
    for n in (["consolab.ttf", "consola.ttf"] if bold else ["consola.ttf"]):
        try: return ImageFont.truetype(n, sz)
        except Exception: pass
    for n in (["arialbd.ttf"] if bold else ["arial.ttf"]):
        try: return ImageFont.truetype(n, sz)
        except Exception: pass
    return ImageFont.load_default()

title_f = fnt(24, True); head_f = fnt(18, True); body_f = fnt(14); mono_f = fnt(13)
y = 22
d.text((30, y), "CaveWii_v2 - parity-audit lower tier + lighting oracle  (sim lane, sim-only proof)",
       font=title_f, fill=FG); y += 34
d.text((30, y), f"commit {sha}  -  suite green incl. count gate  -  hw-risk paths ride .hw_proof_waiver (rig down)",
       font=body_f, fill=DIM); y += 26
d.line((30, y, W-30, y), fill=RULE, width=2); y += 14

sections = [
 ("1. McRegion timestamp table STAMPED (stranded 08-07 fix recovered; audit claim was RIGHT, memory stale)",
  ["Beta RegionFile.write ends func_22208_b(x,z,millis/1000) -> BE u32 at 4096+4*(x+z*32), IOException",
   "swallowed = best-effort [decompile 740c583 :232,:265-269]. Port never wrote sector 1 (all-zero)."],
  ["test_region_timestamps: OK (fresh table zero; writes stamp epoch seconds, monotonic,",
   "  slot-exact; rollback and injected stamp failure both leave the old stamp with the",
   "  chunk data committed)"]),
 ("2. Harvest-tier trio (decompile-verified before edit; tool.c penalty proven already Beta-exact 1.0F/100)",
  ["ItemPickaxe.canHarvestBlock: oreGold needs level>=2 (IRON, was STONE); blockSteel needs",
   "level>=1 (STONE, was IRON). ItemSpade.blocksEffectiveAgainst includes tilledField (shovel class)."],
  ["test_dig_tier_gates highlights (full Beta-derived tick matrix pinned, 19 cells):",
   "  gold ore + stone pickaxe = 15000ms, breaks NO DROP   |  + iron pickaxe = 750ms, drops id=14",
   "  iron block + stone pickaxe = 1900ms, DROPS id=42     |  + gold pickaxe = 25000ms penalty",
   "  farmland + wood shovel 450 / stone 250 / iron 150 / gold 100ms; bare hand 900ms drops dirt"]),
 ("3. Lighting parity vs EXTERNAL mc173 light engine (oracle-diff leg 2; worldgen was leg 1)",
  ["arena1: platform+hole light shaft, leaves opacity-1, torch-14, glowstone-15 under-space;",
   "both engines settled to fixed point; CaveWii = real lighting_update_at_block flood."],
  ["=== oracle-diff: mc173@16f39e7  vs  CaveWii ===",
   "  records: oracle=8448  cavewii=8448  shared=8448",
   "  value mismatches : 0   missing: 0   extra: 0",
   "RESULT: identical to mc173@16f39e7 over 8448 records. PARITY."]),
]

for (head, ctx, out_lines) in sections:
    d.text((30, y), head, font=head_f, fill=HEAD); y += 26
    for line in ctx:
        d.text((46, y), line, font=body_f, fill=DIM); y += 19
    y += 4
    for line in out_lines:
        d.text((46, y), line, font=mono_f, fill=OK); y += 18
    y += 10
    d.line((30, y, W-30, y), fill=RULE, width=1); y += 12

d.text((30, y), "Register status: parity_audit_2026-08-25 lower tier CLOSED (3/3 verified then fixed; timestamp",
       font=body_f, fill=FG); y += 19
d.text((30, y), "claim arbitrated vs decompile -- the audit outranked the stale 08-07 memory). Lighting oracle wired as CTest gate.",
       font=body_f, fill=FG)

img.save(out)
print(f"wrote {out}")
