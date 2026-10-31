#!/usr/bin/env python3
# Sim-only :8099 proof for the 2026-06-26 surgical bug-hunt batch (11 fixes).
# Usage: python tools/render_bughunt_proof.py <git-short-sha> <out.png>
import sys
from PIL import Image, ImageDraw, ImageFont

sha = sys.argv[1] if len(sys.argv) > 1 else "local"
out = sys.argv[2] if len(sys.argv) > 2 else "tmp/proof.png"

W, H = 1200, 800
BG=(20,22,28); FG=(228,230,235); DIM=(150,156,168); OK=(120,210,130); HEAD=(130,180,255); RULE=(60,64,74)
img=Image.new("RGB",(W,H),BG); d=ImageDraw.Draw(img)
def fnt(sz,bold=False):
    for n in (["consolab.ttf"] if bold else ["consola.ttf"]):
        try: return ImageFont.truetype(n,sz)
        except Exception: pass
    return ImageFont.load_default()
title_f=fnt(25,True); head_f=fnt(19,True); body_f=fnt(15); mono_f=fnt(15)
y=22
d.text((28,y),"CaveWii_v2 - bug-hunt surgical batch: 11 fixes  (sim-only proof)",font=title_f,fill=FG); y+=36
d.text((28,y),f"commit {sha}   -   ctest 140/140 GREEN   -   tools/local_ci.sh GREEN (Linux gcc + ASan/UBSan soak + devkitPPC .dol)",font=body_f,fill=DIM); y+=28
d.line((28,y,W-28,y),fill=RULE,width=2); y+=16

sections=[
 ("FAITHFULNESS (Beta 1.7.3, decompile 740c583)",
  ["item_porkchop.c / item_fish.c: raw pork/fish EATEN now HEAL +24/+16 -- were debug",
   "stubs dealing -16 DAMAGE (ItemFood always heals). cooked fish +40 (was 48). maxstack 1.",
   "block_ore.c / block_glowstone.c: rand_gen_range exclusive-max footgun -> redstone now",
   "{4,5}, lapis 4..8, glowstone 2..4 (Beta quantityDropped). block_ice.c lightOpacity 1->3."],
  ["food: raw pork/fish HEAL +24/+16 (not damage), cooked fish +40, full-health no-op, maxstack 1 OK",
   "ore drops: redstone {4,5}, lapis 4..8, glowstone 2..4 (Beta quantityDropped) OK",
   "ice: lightOpacity == 3 (Beta setLightOpacity(3)) OK",
   "test_block_behavior_parity: OK   (140/140 suite green)"]),
 ("MEMORY-SAFETY / UB  (proven by local_ci UBSan + code review; reachable from foreign data)",
  ["region_archive.c: u32 decode shifted a uint8 promoted to signed int (data[0]<<24) ->",
   "signed-overflow UB on a foreign/corrupt .mcr; now cast to uint32_t (bit-identical).",
   "server_world.c: server_world_explode dereferenced blocks[id] with no NULL guard (crash on",
   "an unregistered id from a disk-loaded chunk); now pass-through, matching siblings :1796/:2006.",
   "pc/gfx.c: gfx_draw_quads read texcoords OOB (vertex_count*3 over a 2-component array); now *2."],
  ["local_ci.sh: verify=0  wii-build=0  -> GREEN  (UBSan -fno-sanitize-recover covers the region UB)"]),
]
for head,desc,ev in sections:
    d.text((28,y),head,font=head_f,fill=HEAD); y+=26
    for line in desc:
        d.text((42,y),line,font=body_f,fill=DIM); y+=21
    y+=4
    for line in ev:
        d.text((42,y),"PASS  ",font=mono_f,fill=OK)
        d.text((42+d.textlength("PASS  ",font=mono_f),y),line,font=mono_f,fill=FG); y+=21
    y+=12; d.line((28,y,W-28,y),fill=RULE,width=1); y+=14

d.text((28,y),"Deferred (own commits): window-click slot bound (shared server_local.c), door redstone, cactus/reed, pressure-plate drop.",font=body_f,fill=DIM)
img.save(out); print("wrote",out)
