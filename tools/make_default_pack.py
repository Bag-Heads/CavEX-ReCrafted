#!/usr/bin/env python3
"""
make_default_pack.py -- generate the ORIGINAL / CC0 bundled default textures for
CaveWii_v2 (M3(B)). Every pixel here is generated procedurally from this script's
own color choices -- it contains ZERO Mojang bytes and is dedicated to the public
domain (CC0). The goal: a clean checkout is legally shippable and renders
recognizable (if minimal) placeholders, while the player supplies their own Beta
1.7.3 assets via a texture pack (tools/make_beta_pack.ps1) for the authentic look.

The mob-skin UV LAYOUT (which atlas region maps to which model face) is functional
and dictated by the engine's models (source/graphics/render_entity.c) -- it is NOT
copyrightable; only the pixels are, and those are original here. Skins are 64x32,
the universal layout the render_entity_* functions read.

Usage:
  python tools/make_default_pack.py            # the committed default subset
  python tools/make_default_pack.py --roster   # ALSO the full mob roster (overwrites
                                                # local assets/mob/*; rebuild your Beta
                                                # pack via make_beta_pack.ps1 if needed)
  python tools/make_default_pack.py --wolf     # ONLY the three CC0 wolf variants
  python tools/make_default_pack.py --out DIR   # write to DIR instead of ./assets
"""
import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow (PIL) required: pip install pillow")


def hashshade(x, y, amp):
    """Deterministic, seedless +/-amp dither so flat fills don't look dead."""
    h = (x * 73856093) ^ (y * 19349663)
    return ((h >> 4) & 0xFF) / 255.0 * (2 * amp) - amp


def clamp8(v):
    return max(0, min(255, int(round(v))))


def shade(c, d):
    return (clamp8(c[0] + d), clamp8(c[1] + d), clamp8(c[2] + d), 255 if len(c) < 4 else c[3])


def fill(px, x0, y0, x1, y1, color, dither=0):
    for y in range(y0, y1):
        for x in range(x0, x1):
            d = hashshade(x, y, dither) if dither else 0  # already in 0-255 color units
            px[x, y] = shade(color, d)


def img32(w=64, h=32):
    return Image.new("RGBA", (w, h), (0, 0, 0, 0))


# --- mob skins (64x32) ------------------------------------------------------
# Heads sit at atlas origin (0,0); the front (face) box of a standard 8x8x8 head
# is the (8,8)-(16,16) rect (the "south" UVRect in render_entity.c). We base-fill
# the whole sheet in the body colour (uniform-colour mobs: creeper, pig) and paint
# the face there; the biped (char) instead fills per body-region (head/body/arms/
# legs bounding boxes, which do not overlap in the biped layout).

def paint_eyes_mouth(px, color_eye, color_mouth, kind):
    # within the head-front rect (8,8)-(16,16)
    if kind == "creeper":
        for (ex, ey) in [(9, 10), (13, 10)]:           # two square eyes
            fill(px, ex, ey, ex + 2, ey + 2, color_eye)
        fill(px, 11, 12, 13, 16, color_mouth)            # vertical mouth bar
        fill(px, 9, 14, 11, 16, color_mouth)             # mouth prongs
        fill(px, 13, 14, 15, 16, color_mouth)
    elif kind == "pig":
        fill(px, 10, 10, 11, 11, color_eye)              # eyes
        fill(px, 13, 10, 14, 11, color_eye)
        fill(px, 11, 12, 13, 15, (214, 130, 138, 255))   # snout
        fill(px, 11, 13, 12, 14, color_mouth)            # nostrils
        fill(px, 12, 13, 13, 14, color_mouth)
    elif kind == "char":
        fill(px, 10, 11, 11, 12, color_eye)
        fill(px, 13, 11, 14, 12, color_eye)
        fill(px, 11, 13, 13, 14, color_mouth)


def mob_uniform(body, face_kind, eye, mouth, accent_legs=None):
    """A single-colour mob (creeper/pig): fill all + face; darken the leg region."""
    im = img32()
    px = im.load()
    fill(px, 0, 0, 64, 32, body, dither=18)
    if accent_legs:
        fill(px, 0, 16, 16, 26, accent_legs, dither=12)  # leg/foot region a touch darker
    paint_eyes_mouth(px, eye, mouth, face_kind)
    return im


def mob_wolf(body, eye, muzzle, collar=None):
    """Original CC0 64x32 wolf placeholder using the ModelWolf atlas regions."""
    im = img32()
    px = im.load()
    fill(px, 0, 0, 64, 32, body, dither=14)
    # ModelWolf head front and snout regions; deliberately simple and original.
    fill(px, 7, 4, 9, 6, eye)
    fill(px, 11, 4, 13, 6, eye)
    fill(px, 4, 13, 7, 16, muzzle, dither=6)
    fill(px, 5, 14, 7, 16, (40, 34, 32, 255))
    # Ears and paws make the variants readable at Wii distance.
    fill(px, 16, 14, 22, 18, shade(body, -28))
    fill(px, 0, 25, 8, 32, shade(body, -22))
    if collar:
        fill(px, 21, 8, 29, 11, collar, dither=4)
    return im


def mob_char(skin, hair, shirt, pants, eye, mouth):
    im = img32()
    px = im.load()
    fill(px, 0, 0, 64, 32, skin, dither=14)              # base = skin (head + arms)
    fill(px, 8, 0, 24, 8, hair, dither=10)               # hair: top of head
    fill(px, 0, 8, 8, 16, hair, dither=10)               # hair: back of head strip
    fill(px, 16, 16, 40, 32, shirt, dither=14)           # body region = shirt
    fill(px, 44, 20, 56, 32, shirt, dither=14)           # upper arms = sleeve
    fill(px, 0, 16, 16, 32, pants, dither=14)            # legs region = pants
    paint_eyes_mouth(px, eye, mouth, "char")
    return im


# --- non-mob -----------------------------------------------------------------

def minecart():
    im = Image.new("RGBA", (64, 32), (0, 0, 0, 0))
    px = im.load()
    wood = (154, 106, 58, 255)
    fill(px, 0, 0, 64, 32, wood, dither=16)
    for y in range(0, 32, 4):                            # horizontal plank seams
        fill(px, 0, y, 64, y + 1, (122, 82, 44, 255))
    fill(px, 0, 0, 64, 2, (136, 136, 144, 255))          # iron rim hints
    return im


def colormap(corner_hotwet, corner_hotdry, corner_cold):
    """256x256, sampled at pixel (ci, cj) with cj>=ci (lower-left triangle valid).
    ci=(1-temp)*255 (x: hot->left), cj=(1-humidity*temp)*255 (y: wet->top)."""
    im = Image.new("RGBA", (256, 256))  # RGBA8: biome_color.c reads a *4 stride
    px = im.load()
    hw, hd, cold = corner_hotwet, corner_hotdry, corner_cold
    for cj in range(256):
        for ci in range(256):
            fx = ci / 255.0   # 0 hot .. 1 cold
            fy = cj / 255.0   # 0 wet .. 1 dry
            # blend: wet->dry along y, and toward 'cold' along x
            wet = tuple(hw[k] * (1 - fy) + hd[k] * fy for k in range(3))
            c = tuple(clamp8(wet[k] * (1 - fx) + cold[k] * fx) for k in range(3))
            px[ci, cj] = (c[0], c[1], c[2], 255)
    return im


def water():
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    px = im.load()
    base = (32, 64, 160)
    for y in range(16):
        for x in range(16):
            d = int(hashshade(x, y, 14))  # already in color units
            px[x, y] = (clamp8(base[0] + d), clamp8(base[1] + d), clamp8(base[2] + d * 2), 110)
    return im


def shadow():
    im = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    px = im.load()
    cx, cy, r = 31.5, 31.5, 31.5
    for y in range(64):
        for x in range(64):
            dist = (((x - cx) ** 2 + (y - cy) ** 2) ** 0.5) / r
            a = max(0.0, 1.0 - dist)
            px[x, y] = (0, 0, 0, clamp8(a * a * 130))    # soft radial alpha
    return im


def save(im, out, rel):
    path = os.path.join(out, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
    print(f"  wrote {rel}  ({im.width}x{im.height})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "assets"))
    ap.add_argument("--roster", action="store_true",
                    help="also generate the full mob roster (overwrites local assets/mob/*)")
    ap.add_argument("--wolf", action="store_true",
                    help="generate only the three original CC0 wolf skins")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    print(f"[make_default_pack] writing CC0 default textures -> {out}")

    if args.wolf:
        save(mob_wolf((132, 126, 116, 255), (36, 30, 26, 255),
                      (174, 164, 148, 255)), out, "mob/wolf.png")
        save(mob_wolf((146, 137, 124, 255), (42, 34, 28, 255),
                      (190, 178, 160, 255), (38, 92, 184, 255)),
             out, "mob/wolf_tame.png")
        save(mob_wolf((103, 91, 82, 255), (202, 34, 28, 255),
                      (146, 128, 112, 255)), out, "mob/wolf_angry.png")
        print("[make_default_pack] done. (CC0 / public domain -- zero Mojang bytes)")
        return

    # P0: replace the tracked Mojang mob rips (creeper is the universal mob fallback)
    save(mob_uniform((79, 138, 59, 255), "creeper", (32, 46, 22, 255), (32, 46, 22, 255),
                     accent_legs=(64, 112, 47, 255)), out, "mob/creeper.png")
    save(mob_char((216, 160, 120, 255), (90, 58, 32, 255), (58, 138, 176, 255),
                  (58, 64, 128, 255), (40, 40, 90, 255), (120, 70, 70, 255)), out, "mob/char.png")
    save(mob_uniform((224, 144, 144, 255), "pig", (40, 26, 26, 255), (120, 70, 70, 255),
                     accent_legs=(196, 118, 118, 255)), out, "mob/pig.png")

    # P1: the missing default-pack files the M3(A) coverage gate flagged
    save(minecart(), out, "item/cart.png")
    save(colormap((89, 192, 59), (182, 174, 90), (128, 168, 128)), out, "misc/grasscolor.png")
    save(colormap((72, 176, 24), (154, 150, 58), (96, 144, 96)), out, "misc/foliagecolor.png")
    save(water(), out, "misc/water.png")
    save(shadow(), out, "misc/shadow.png")

    if args.roster:
        # Opt-in: simple uniform-colour placeholders for the rest of the roster. These
        # overwrite local assets/mob/* (rebuild your Beta pack via make_beta_pack.ps1).
        roster = {
            "mob/zombie.png":   ((73, 110, 73, 255), "char", (40, 40, 60, 255), (40, 40, 40, 255)),
            "mob/skeleton.png": ((196, 196, 196, 255), "char", (40, 40, 40, 255), (40, 40, 40, 255)),
            "mob/spider.png":   ((60, 50, 45, 255), "creeper", (160, 30, 30, 255), (30, 20, 18, 255)),
            "mob/cow.png":      ((80, 64, 48, 255), "pig", (30, 24, 20, 255), (40, 30, 24, 255)),
            "mob/chicken.png":  ((232, 232, 224, 255), "pig", (30, 30, 30, 255), (210, 140, 40, 255)),
            "mob/sheep.png":    ((224, 216, 208, 255), "pig", (40, 36, 34, 255), (90, 80, 76, 255)),
            "mob/squid.png":    ((90, 70, 130, 255), "pig", (30, 24, 40, 255), (60, 48, 90, 255)),
            "mob/slime.png":    ((110, 200, 100, 255), "pig", (40, 70, 36, 255), (70, 130, 64, 255)),
        }
        for rel, (body, kind, eye, mouth) in roster.items():
            save(mob_uniform(body, kind, eye, mouth), out, rel)
        # overlays (alpha): sheep wool + spider eyes
        fur = Image.new("RGBA", (64, 32), (0, 0, 0, 0)); fp = fur.load()
        fill(fp, 0, 0, 64, 32, (240, 236, 230, 200), dither=20)
        save(fur, out, "mob/sheep_fur.png")
        eyes = Image.new("RGBA", (64, 32), (0, 0, 0, 0)); ep = eyes.load()
        for (ex, ey) in [(9, 10), (13, 10)]:
            fill(ep, ex, ey, ex + 2, ey + 2, (200, 40, 40, 255))
        save(eyes, out, "mob/spider_eyes.png")

    print("[make_default_pack] done. (CC0 / public domain -- zero Mojang bytes)")


if __name__ == "__main__":
    main()
