#!/usr/bin/env python3
"""
Small TEST scene for the tutorial-world toolchain (not the tutorial itself).

    python tools/tutorial_world/demo_scene.py            # saves/Tutorial Demo
    python tools/tutorial_world/demo_scene.py --out DIR  # any save directory
        [--preview tmp/jobs/tutorial-1021/demo_top.png]

3x3 chunks (blocks 0..47): a grass island in shallow sea, a plank hut with a
stair gable roof, a door, wall torches, a wall sign, a chest with 3 items, a
lit furnace, a bed, a lever -> redstone torch -> wire -> repeater -> wire line,
and a wheat row at every growth stage next to a water source.

Known coordinates (tests/test_tutorial_world_load.c asserts on these):
  wall sign (22,66,24)  "Hello" / "CaveWii", faces south
  chest     (19,65,19)  slot0 wooden pickaxe, slot1 16 torches, slot2 3 bread
  furnace   (23,65,19)  lit, 4 iron ore + 2 coal, BurnTime 1200, CookTime 50
  torch     (19,67,21)  wall torch facing east -> block light 14
  rs torch  (16,65,28)  lit redstone torch -> block light 7
  wheat     (18+s,65,30) stage s = 0..7, farmland below, water (21,64,31)
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import worldgen_lib as wl  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_OUT = os.path.join(REPO, "saves", "Tutorial Demo")
DEFAULT_PREVIEW = os.path.join(REPO, "tmp", "jobs", "tutorial-1021",
                               "demo_top.png")

ITEM_WOODEN_PICKAXE, ITEM_BREAD, ITEM_COAL = 270, 297, 263


def build_demo():
    w = wl.World("Tutorial Demo", seed=20261021, spawn=(21, 65, 26),
                 time=1000, player_yaw=180.0,  # looking north at the hut
                 inventory=[(0, 50, 8)])     # 8 torches in the hotbar
    w.ensure_area(0, 0, 47, 47)

    # Terrain: island of radius 14 around (24,24); sea elsewhere.
    for x in range(48):
        for z in range(48):
            w.set(x, 0, z, "bedrock")
            w.fill(x, 1, z, x, 55, z, "stone")
            if (x - 24) ** 2 + (z - 24) ** 2 <= 14 ** 2:
                w.fill(x, 56, z, x, 60, z, "stone")
                w.fill(x, 61, z, x, 63, z, "dirt")
                w.set(x, 64, z, "grass")
            else:
                w.fill(x, 56, z, x, 58, z, "sand")
                w.fill(x, 59, z, x, 63, z, "water")

    # Hut: walls x 18..24, z 18..23, y 65..68 on a plank floor.
    w.fill(18, 64, 18, 24, 64, 23, "planks")
    w.hollow_box(18, 65, 18, 24, 68, 23, "planks")
    w.fill(19, 68, 19, 23, 68, 22, "air")           # open to the roof space
    for i in range(4):                               # stair gable, ridge on x
        y = 69 + i
        for x in range(17, 26):
            w.stairs(x, y, 17 + i, "north")
            w.stairs(x, y, 24 - i, "south")
        for x in (18, 24):                           # gable end walls
            for z in range(18 + i, 24 - i):
                w.set(x, y, z, "planks")
    w.door(21, 65, 23, "north")
    w.sign(22, 66, 24, ["Hello", "CaveWii"], facing="south", wall=True)
    w.torch(19, 67, 21, "east")
    w.torch(23, 67, 21, "west")
    w.chest(19, 65, 19, [(0, ITEM_WOODEN_PICKAXE, 1), (1, 50, 16),
                         (2, ITEM_BREAD, 3)])
    w.furnace(23, 65, 19, "south", lit=True,
              items=[(0, 15, 4), (1, ITEM_COAL, 2)],
              burn_time=1200, cook_time=50)
    w.bed(23, 65, 21, "north")

    # Redstone: lever on stone, torch on the far side, wire, repeater, wire.
    w.set(15, 65, 28, "stone")
    w.lever(14, 65, 28, "west")                      # off
    w.torch(16, 65, 28, "east", redstone=True, lit=True)
    w.wire(17, 65, 28, 15)
    w.wire(18, 65, 28, 14)
    w.repeater(19, 65, 28, "east", delay=2, powered=True)
    for k, x in enumerate(range(20, 23)):
        w.wire(x, 65, 28, 15 - k)

    # Wheat at every growth stage, watered from one source block.
    for s in range(8):
        w.crop(18 + s, 65, 30, s)
    w.water_source(21, 64, 31)
    return w


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--preview", default=None,
                    help="PNG path (default %s when --out is the default)"
                    % DEFAULT_PREVIEW)
    a = ap.parse_args(argv)
    w = build_demo()
    bad = w.problems()
    if bad:
        raise SystemExit("authoring problems:\n  " + "\n  ".join(bad))
    files = w.save(a.out)
    print("wrote %s: level.dat + %s" % (a.out, ", ".join(files)))
    prev = a.preview or (DEFAULT_PREVIEW if a.out == DEFAULT_OUT else None)
    if prev:
        import preview
        for p in preview.preview(w, prev, layers=(65, 66)):
            print("preview", p)


if __name__ == "__main__":
    main()
