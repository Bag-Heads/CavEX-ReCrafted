"""Build the startup-credits bootstrap textures + manifest (BOOT-CREDITS-1028).

Offline only. The game never decodes a PNG or touches a pack font for these
cards. Two raw RGBA8 textures (CWSA header + pixels), 950,272 B together:
  credits-frames.bin  576x384  the 6 BagHeads idle frames (B2-taps, v4),
                      3 across x 2 down, each 192x192, drawn 1:1
  credits-font.bin    128x128  a private copy of the bundled 8x8 bitmap font
                      (assets/font/default.png) with RGB forced to white so a
                      colour tint reproduces the design's glyph colour exactly
Why two RGBA8 textures (not one, not RGB5A3): frames + font in one GX-legal
texture is 576x512 = 1,179,648 B, over the 1 MiB budget; RGB5A3 would cut the
frames' 239-level soft alpha edge to 3 bits (visible fringe on black). Two
separate textures fit with ~98 KB to spare at full fidelity.
Card text is drawn at run time from the font (Beta metrics, Beta shadow), so
error/stall messages need no pre-rendered strings. credits.txt holds layout,
timing and animation (design-opus-v3/NOTES.md). Run from the repo root:
    python -I assets/gui/startup-credits/build_atlas.py . [frames_dir]
"""
import os
import struct
import sys

from PIL import Image

REPO = sys.argv[1] if len(sys.argv) > 1 else "."
FRAMES_DIR = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    REPO, "docs", "ui_design", "startup-credits-prototype", "frames-B2-taps")
OUT = os.path.join(REPO, "assets", "gui", "startup-credits")
W, H, SAFE = 640, 480, 32
ART, NFR = 192, 6
# Maintainer pick B2-taps (design-opus-v4/NOTES.md "B2"): (frame, ms) steps,
# frames reused; 5,700 ms loop, every step exactly one glow level from the
# last, wrap included.
B2_STEPS = [(0, 500), (1, 300), (2, 300), (3, 400), (2, 300), (1, 300), (0, 600),
            (5, 400), (0, 400),
            (1, 250), (2, 250), (3, 300), (4, 500), (3, 300), (2, 300), (1, 300)]
assert sum(ms for _, ms in B2_STEPS) == 5700
FADE_IN, DWELL, FADE_OUT, STALL = 200, 3000, 200, 10000
INK, ORANGE, FOOT = "F5EEE3", "FF982D", "B8B8B8"
FOOT_Y = 404

FONT = Image.open(os.path.join(REPO, "assets", "font", "default.png")).convert("RGBA")
_a = FONT.getchannel("A")
CW = []
for c in range(256):
    w = 0
    for x in range(8):
        if any(_a.getpixel(((c % 16) * 8 + x, (c // 16) * 8 + y)) for y in range(8)):
            w = x + 1
    CW.append(3 if c == 32 else w)


def fw(s, sc):
    return sum((CW[ord(ch)] + 1) * sc // 8 for ch in s)


def write_tex(name, im):
    raw = im.tobytes()
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(b"CWSA" + struct.pack(">II", im.width, im.height) + raw)
    return len(raw)


# ---- textures
sheet = Image.new("RGBA", (3 * ART, 2 * ART), (0, 0, 0, 0))
for i in range(NFR):
    fr = Image.open(os.path.join(FRAMES_DIR, "frame_%d.png" % i)).convert("RGBA")
    assert fr.size == (ART, ART), fr.size
    sheet.paste(fr, ((i % 3) * ART, (i // 3) * ART))
font = Image.new("RGBA", FONT.size, (255, 255, 255, 0))
font.putalpha(FONT.getchannel("A"))
total = write_tex("credits-frames.bin", sheet) + write_tex("credits-font.bin", font)
assert total <= 1 << 20, total

# ---- layout (cards_v3.py)
lines = ["startup-credits 2",
         "texture frames credits-frames.bin %d %d" % sheet.size,
         "texture font credits-font.bin %d %d" % font.size,
         "timing %d %d %d %d" % (FADE_IN, DWELL, FADE_OUT, STALL)]
for i in range(NFR):
    lines.append("sprite frame%d frames %d %d %d %d" % (i, (i % 3) * ART, (i // 3) * ART, ART, ART))


def text(x, y, sc, rgb, s):
    lines.append("text %d %d %d %s %s" % (x, y, sc, rgb, s))


def ctext(y, sc, rgb, s):
    text((W - fw(s, sc)) // 2, y, sc, rgb, s)


c1_h = 16 + 12 + ART + 24 + 40 + 5
y = SAFE + (H - 2 * SAFE - c1_h) // 2
lines.append("card")
ctext(y, 16, INK, "Brought to you by")
y += 16 + 12
lines.append("seq %d %d %s" % ((W - ART) // 2, y,
                               " ".join("frame%d:%d" % st for st in B2_STEPS)))
y += ART + 24
ctext(y, 40, ORANGE, "BagHeads")

c2 = [("Built on", 16), 40, ("CavEX", 16), 10, ("xtreme8000", 24), 30, ("fCavEX", 16), 10,
      (["yyy257", "ByteBit", "jilleb"], 24), 48, ("Not affiliated with Mojang or Nintendo.", 16)]
c2_h = sum(i if isinstance(i, int) else i[1] for i in c2) + 2
y = SAFE + (H - 2 * SAFE - c2_h) // 2
lines.append("card")
for it in c2:
    if isinstance(it, int):
        y += it
        continue
    s, sc = it
    if isinstance(s, list):
        x = (W - (sum(fw(t, sc) for t in s) + 24 * (len(s) - 1))) // 2
        for t in s:
            text(x, y, sc, INK, t)
            x += fw(t, sc) + 24
    else:
        ctext(y, sc, INK, s)
    y += sc
lines.append("footer %d 16 %s" % (FOOT_Y, FOOT))
lines.append("end")
with open(os.path.join(OUT, "credits.txt"), "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
for stale in ("credits-atlas.bin",):
    p = os.path.join(OUT, stale)
    if os.path.exists(p):
        os.remove(p)
print("frames %dx%d + font %dx%d = %d B" % (sheet.size + font.size + (total,)))
