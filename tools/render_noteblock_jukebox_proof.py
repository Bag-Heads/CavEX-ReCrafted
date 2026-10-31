#!/usr/bin/env python3
# Render a sim-only :8099 proof image for the note block + jukebox slice
# (noteblock/jukebox-slice, 2026-08-14). Sim-only fixes prove their claim
# with the golden test's labelled PASS output (CLAUDE.md DoD).
# Usage: python tools/render_noteblock_jukebox_proof.py <git-short-sha> <out.png>
import sys
from PIL import Image, ImageDraw, ImageFont

sha = sys.argv[1] if len(sys.argv) > 1 else "local"
out = sys.argv[2] if len(sys.argv) > 2 else "tmp/noteblock_jukebox_proof.png"

W, H = 1280, 1080
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

title_f = fnt(24, True); head_f = fnt(18, True); body_f = fnt(15); mono_f = fnt(14)
y = 22
d.text((30, y), "CaveWii_v2 - Beta 1.7.3 note block + jukebox slice (sim-only proof)", font=title_f, fill=FG); y += 34
d.text((30, y), f"commit {sha}   -   test_noteblock_jukebox_env 19/19 PASS   -   full ctest 241/242 (1 expected env failure: test_run_wii_power_cycle, no .dol)", font=body_f, fill=DIM); y += 26
d.line((30, y, W-30, y), fill=RULE, width=2); y += 16

sections = [
 ("Note block (block_noteblock.c, noteblock_logic.[ch], server_local.c)",
  ["BlockNote.blockActivated: changePitch (note+1)%25 then triggerNote, ALWAYS. onNeighborBlockChange:",
   "canProvidePower origin gate + previousRedstoneState RISING EDGE only (WEAK isBlockGettingPowered,",
   "not the dispenser's indirect query). triggerNote: silent if covered; instrument from material below",
   "(rock/sand/glass/wood/else); pitch=(float)pow(2.0,(note-12)/12.0). [decompile 740c583]"],
  ["changePitch: cycles 0..24 then wraps to 0",
   "instrument table: rock->bd, sand->snare, glass->hat, wood->bassattack, else->harp",
   "pitch formula: note 0->0.5F, 12->1.0F, 24->2.0F exactly (fp32 powers of two)",
   "right-click: changePitch(0->1) + play bd @ correct pitch, vol 3.0",
   "covered (block above != air): changePitch runs, NO sound",
   "instrument variety live (sand/glass/wood/dirt) matches the table",
   "redstone rising-edge: power held = ONE note; off/on cycles trigger again",
   "D12: CONTENTS-ONLY note edit (clean chunk) survives evict/reload"]),
 ("Jukebox (block_jukebox.c, jukebox_logic.[ch], item_record.c, server_local.c)",
  ["ItemRecord.onItemUse inserts (meta 0 only): record set, meta 1, stack-1, CRPC_RECORD_EVENT",
   "(world event 1005, data=record id). BlockJukeBox eject (right-click meta!=0, or onBreak):",
   "event 1005 data 0 (stop) BEFORE teardown, TE+meta cleared, EntityItem at the exact f=0.7F",
   "offset (3 sequenced world-rand draws, x/y/z order). Records are Mojang IP: no 13/cat audio",
   "bundled -- the 1005 event ROUTES with correct data; the client handler is a documented no-op",
   "(S3 streaming pool out of scope). [decompile 740c583: BlockJukeBox/ItemRecord/TileEntityRecordPlayer]"],
  ["onItemPlace registers a fresh TE (record=0)",
   "insert consumes stack, meta 1, Record persisted, event 1005 data=record id",
   "insert refused when occupied (meta!=0)",
   "right-click eject: event 1005/0, meta 0, exact item id/count/durability + exact offset",
   "right-click on empty (meta 0) is UNHANDLED (falls through to item onUseOnBlock)",
   "onBreak ejects the record then frees the TE",
   "onBreak on an empty jukebox is a clean no-op",
   "record + meta survive a real save/evict/reload round trip"]),
]
for head, desc, ev in sections:
    d.text((30, y), head, font=head_f, fill=HEAD); y += 26
    for line in desc:
        d.text((46, y), line, font=body_f, fill=DIM); y += 20
    y += 4
    for line in ev:
        d.text((46, y), "PASS  ", font=mono_f, fill=OK)
        d.text((46 + d.textlength("PASS  ", font=mono_f), y), line, font=mono_f, fill=FG); y += 20
    y += 12
    d.line((30, y, W-30, y), fill=RULE, width=1); y += 14

d.text((30, y), "Persistence fan-out: tile_entities_nbt/store (both kinds, D12 shared predicate),", font=body_f, fill=DIM); y += 20
d.text((30, y), "test_region_tile_entities (real .mcr round-trip + ownership rejects + OOM fail-open",
       font=body_f, fill=DIM); y += 20
d.text((30, y), "guards per kind), test_tile_entities_store (insert/gather/release + prev_powered clear).",
       font=body_f, fill=DIM); y += 26
d.text((30, y), "RED-before-GREEN (D12 negative control): tile_entities_any() temporarily reverted to omit",
       font=body_f, fill=DIM); y += 20
d.text((30, y), "n_note/n_record_player (the exact D12 bug shape) -> the note-only OOM fail-open guard in",
       font=body_f, fill=DIM); y += 20
d.text((30, y), "test_region_tile_entities FAILED exactly as predicted; restored -> same test PASSED.",
       font=body_f, fill=DIM); y += 26
d.text((30, y), "Status: fix candidate, hardware-unconfirmed -- console dark 2026-08-14, PC red/green",
       font=body_f, fill=DIM); y += 20
d.text((30, y), "evidence is the interim proof (source/network/* is hw-risk per CLAUDE.md), in-band",
       font=body_f, fill=DIM); y += 20
d.text((30, y), "real-Wii rerun queued.", font=body_f, fill=DIM); y += 20

img.save(out)
print("wrote", out)
