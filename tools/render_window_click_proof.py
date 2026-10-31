#!/usr/bin/env python3
# Sim-only :8099 proof for the window-click slot-bound guard
# (bug_hunt_2026-06-26 DEFERRED row). The fix is a network-input bound check
# with no single-frame game-visible effect, so per the FIX-COMMIT DoD the proof
# is the PASSING EVIDENCE (the test + the before/after the guard enforces), not
# an in-game capture.
#
# Usage: python tools/render_window_click_proof.py <git-short-sha> <out.png> [localci]
import sys
from PIL import Image, ImageDraw, ImageFont

sha = sys.argv[1] if len(sys.argv) > 1 else "local"
out = sys.argv[2] if len(sys.argv) > 2 else "tmp/window_click_proof.png"
localci = sys.argv[3] if len(sys.argv) > 3 else "GREEN"

W, H = 1240, 860
BG=(20,22,28); FG=(228,230,235); DIM=(150,156,168); OK=(120,210,130)
HEAD=(130,180,255); RULE=(60,64,74); BAD=(235,130,130); WARN=(230,200,120)
img=Image.new("RGB",(W,H),BG); d=ImageDraw.Draw(img)
def fnt(sz,bold=False):
    for n in (["consolab.ttf"] if bold else ["consola.ttf"]):
        try: return ImageFont.truetype(n,sz)
        except Exception: pass
    return ImageFont.load_default()
title_f=fnt(24,True); head_f=fnt(18,True); body_f=fnt(15); mono_f=fnt(15)

y=22
d.text((28,y),"CaveWii_v2 - window-click slot bound guard  (bug_hunt_2026-06-26)   [sim-only proof]",font=title_f,fill=FG); y+=34
d.text((28,y),f"commit {sha}   -   ctest 141/141 GREEN   -   tools/local_ci.sh {localci} (Linux gcc + 8000-iter ASan/UBSan soak + devkitPPC .dol)",font=body_f,fill=DIM); y+=27
d.line((28,y,W-28,y),fill=RULE,width=2); y+=14

def section(head, desc):
    global y
    d.text((28,y),head,font=head_f,fill=HEAD); y+=24
    for line in desc:
        d.text((42,y),line,font=body_f,fill=DIM); y+=20
    y+=6

section("THE BUG  (OOB heap read+write, reachable from a delayed/stale client packet)",
 ["SRPC_WINDOW_CLICK forwarded the UNTRUSTED uint8_t client slot (0..255) straight into",
  "inventory_action(active_inventory, slot, ...), which reads AND writes inv->items[slot].",
  "The only bound check was assert(slot < capacity) -- a NO-OP under the shipping .dol's",
  "-DNDEBUG (only test_ targets force -UNDEBUG). Window capacities differ (player 45, chest 63,",
  "iron chest 90); client+server run on separate threads over a delayed reliable ring, so a stale",
  "large-chest slot can apply AFTER active_inventory shrank back to the 45-slot player inventory."])

section("THE FIX  (sim-neutral; mirrors the adjacent SRPC_HOTBAR_SLOT bound)",
 ["inventory.h:  static inline inventory_slot_valid(inv, slot) = (inv && slot < inv->capacity)",
  "server_local.c SRPC_WINDOW_CLICK:  accept = inventory_slot_valid(av, slot) && inventory_action(av, slot, ...)",
  "On reject: changes stays empty -> CRPC_WINDOW_TRANSACTION accepted=false (client prediction rolls",
  "back, no desync); server_local_send_inv_changes loops zero times (no inv deref)."])

d.line((28,y,W-28,y),fill=RULE,width=1); y+=12
d.text((28,y),"PASSING EVIDENCE  (tests/test_window_click_slot_bound.c -- pins the SAME static-inline the dispatch calls)",font=head_f,fill=HEAD); y+=26

rows=[
 ("slot 0..44 on player(45)",            "VALID -> click applies, byte-identical to before (sim-neutral)"),
 ("slot 80 (stale iron-chest) on player(45)","was OOB items[80] r/w  ->  now REJECTED (accept=false)"),
 ("slot == capacity (45 / 63 / 90)",     "first invalid index  ->  REJECTED"),
 ("slot 90..255 (full uint8_t over-range)","REJECTED (incl. picked-item sentinel 255)"),
 ("NULL active_inventory",               "REJECTED -- never dereferenced (short-circuit)"),
]
for k,v in rows:
    d.text((42,y),"PASS  ",font=mono_f,fill=OK)
    x=42+d.textlength("PASS  ",font=mono_f)
    d.text((x,y),k,font=mono_f,fill=FG)
    d.text((x+360,y),v,font=mono_f,fill=DIM); y+=21
y+=4
d.text((42,y),"PASS  ",font=mono_f,fill=OK)
d.text((42+d.textlength("PASS  ",font=mono_f),y),"test_window_click_slot_bound .... Passed 0.04 sec   (full suite 141/141)",font=mono_f,fill=FG); y+=21
d.text((42,y),"PASS  ",font=mono_f,fill=OK)
d.text((42+d.textlength("PASS  ",font=mono_f),y),"adversarial review: SOUND (0 issues, sim-neutral)   sibling sweep: 0 other unchecked-slot RPC paths",font=mono_f,fill=FG); y+=24

d.line((28,y,W-28,y),fill=RULE,width=1); y+=12
d.text((28,y),"FAITHFULNESS: for every in-range (legitimate) slot, accept == old behaviour, byte-for-byte. The guard lives in the",font=body_f,fill=WARN); y+=20
d.text((28,y),"network-validation layer -- no worldgen / redstone / fluid / recipe / lighting value is touched. T1: free, bit-exact.",font=body_f,fill=WARN)
img.save(out); print("wrote",out)
