#!/usr/bin/env python3
"""
Unit tests for tools/tutorial_world (NBT writer, McRegion packer, light,
tile-entity layout, light-table drift against source/block/*.c, determinism).

    python tests/test_tutorial_worldgen_lib.py [--fixture-out DIR]

--fixture-out DIR: after the tests pass, write the demo scene to
DIR/Tutorial Demo and the mob chunk (build_animals) to DIR/Tutorial Animals
for tests/test_tutorial_world_load.c (ctest fixture
`tutorial_world`, see CMakeLists.txt).
"""
import gzip
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "tutorial_world"))
import worldgen_lib as wl  # noqa: E402
import light_tables  # noqa: E402
import demo_scene  # noqa: E402
import preview  # noqa: E402


class NbtTests(unittest.TestCase):
    def test_round_trip_every_type(self):
        root = wl.Compound([
            ("b", wl.Byte(-5)), ("s", wl.Short(-300)), ("i", wl.Int(1 << 30)),
            ("l", wl.Long(-(1 << 40))), ("f", wl.Float(1.5)),
            ("d", wl.Double(-2.25)), ("ba", wl.ByteArray(b"\x00\xff")),
            ("str", wl.String("Hello")),
            ("li", wl.List(wl.TAG_INT, [wl.Int(1), wl.Int(2)])),
            ("empty", wl.List(wl.TAG_BYTE, [])),
            ("c", wl.Compound([("x", wl.Int(7))])),
        ])
        name, back = wl.decode_root(wl.encode_root(root, "r"))
        self.assertEqual(name, "r")
        self.assertEqual(back, root)

    def test_known_bytes(self):
        # TAG_Compound "" { TAG_Byte "a" = 1 } -- the encoder idiom of
        # tools/gen_level_template.py.
        self.assertEqual(wl.encode_root(wl.Compound([("a", wl.Byte(1))])),
                         b"\x0a\x00\x00\x01\x00\x01a\x01\x00")

    def test_item_canonical_order(self):
        # item_nbt.c canonical(): Slot, id, Count, Damage -- 36 bytes.
        raw = wl.encode_payload(wl.item_tag(3, 263, 2, 0))
        self.assertEqual(raw, bytes([1, 0, 4]) + b"Slot" + bytes([3])
                         + bytes([2, 0, 2]) + b"id" + struct.pack(">h", 263)
                         + bytes([1, 0, 5]) + b"Count" + bytes([2])
                         + bytes([2, 0, 6]) + b"Damage" + b"\x00\x00" + b"\x00")


class TileEntityTests(unittest.TestCase):
    def test_sign_rules(self):
        with self.assertRaises(ValueError):
            wl.sign_te(0, 0, 0, ["sixteen chars!!!"])
        with self.assertRaises(ValueError):
            wl.sign_te(0, 0, 0, ["trailing "])
        with self.assertRaises(ValueError):
            wl.sign_te(0, 0, 0, ["a", "b", "c", "d", "e"])
        te = wl.sign_te(1, 2, 3, ["fifteen chars!!"])
        self.assertEqual([n for n, _ in te.value],
                         ["id", "x", "y", "z", "Text1", "Text2", "Text3", "Text4"])

    def test_items_sorted_and_empty_list_type(self):
        te = wl.chest_te(0, 0, 0, [(5, 1, 1), (0, 2, 1)])
        items = wl.child(te, "Items")
        self.assertEqual([wl.child(i, "Slot").value for i in items.value], [0, 5])
        empty = wl.child(wl.furnace_te(0, 0, 0), "Items")
        self.assertEqual((empty.elem, empty.value), (wl.TAG_BYTE, []))
        with self.assertRaises(ValueError):
            wl.chest_te(0, 0, 0, [(27, 1, 1)])


def build_animals():
    """Fixture for tests/test_tutorial_world_load.c: one chunk (0,0), grass
    floor at y 64, a sign, and one of each Beta passive mob (the sheep red)."""
    w = wl.World("Tutorial Animals", seed=1, spawn=(8, 65, 8))
    w.fill(0, 0, 0, 15, 0, 15, "bedrock")
    w.fill(0, 1, 0, 15, 63, 15, "stone")
    w.fill(0, 64, 0, 15, 64, 15, "grass")
    w.sign(2, 65, 2, ["PEN"])
    w.mob(4.5, 65.0, 4.5, "Sheep", color="red")
    w.mob(6.5, 65.0, 4.5, "Cow", yaw=90.0)
    w.mob(8.5, 65.0, 4.5, "Pig")
    w.mob(10.5, 65.0, 4.5, "Chicken")
    return w


class MobTests(unittest.TestCase):
    # Beta 1.7.3 Entity.addEntityID/writeToNBT + EntityLiving/EntitySheep/
    # EntityPig.writeEntityToNBT (see worldgen_lib.mob), as admitted by
    # entity_archive.c entity_beta_import_owned.
    COMMON = [("id", wl.TAG_STRING), ("Pos", wl.TAG_LIST),
              ("Motion", wl.TAG_LIST), ("Rotation", wl.TAG_LIST),
              ("FallDistance", wl.TAG_FLOAT), ("Fire", wl.TAG_SHORT),
              ("Air", wl.TAG_SHORT), ("OnGround", wl.TAG_BYTE),
              ("Health", wl.TAG_SHORT), ("HurtTime", wl.TAG_SHORT),
              ("DeathTime", wl.TAG_SHORT), ("AttackTime", wl.TAG_SHORT)]

    def shape(self, tag):
        return [(n, v.type) for n, v in tag.value]

    def test_exact_tags_per_kind(self):
        extra = {"Sheep": [("Sheared", wl.TAG_BYTE), ("Color", wl.TAG_BYTE)],
                 "Pig": [("Saddle", wl.TAG_BYTE)], "Cow": [], "Chicken": []}
        for kind, more in extra.items():
            t = wl.mob(1.5, 65.0, 2.5, kind)
            self.assertEqual(self.shape(t), self.COMMON + more, kind)
            self.assertEqual(wl.child(t, "id").value, kind)
            pos = wl.child(t, "Pos")
            self.assertEqual((pos.elem, [v.value for v in pos.value]),
                             (wl.TAG_DOUBLE, [1.5, 65.0, 2.5]))
            mot = wl.child(t, "Motion")
            self.assertEqual((mot.elem, len(mot.value)), (wl.TAG_DOUBLE, 3))
            rot = wl.child(t, "Rotation")
            self.assertEqual((rot.elem, len(rot.value)), (wl.TAG_FLOAT, 2))
            self.assertEqual(wl.child(t, "Fire").value, -1)
            self.assertEqual(wl.child(t, "Air").value, 300)
            self.assertEqual(wl.child(t, "OnGround").value, 1)
            self.assertEqual(wl.child(t, "Health").value,
                             4 if kind == "Chicken" else 10)

    def test_options_and_rejections(self):
        s = wl.mob(0.5, 65, 0.5, "Sheep", color="red", sheared=True)
        self.assertEqual((wl.child(s, "Color").value,
                          wl.child(s, "Sheared").value), (14, 1))
        self.assertEqual(wl.child(wl.mob(0.5, 65, 0.5, "Pig", saddle=True),
                                  "Saddle").value, 1)
        for bad in (lambda: wl.mob(0, 65, 0, "Zombie"),
                    lambda: wl.mob(0, 65, 0, "Cow", color=3),
                    lambda: wl.mob(0, 65, 0, "Sheep", saddle=True),
                    lambda: wl.mob(0, 65, 0, "Sheep", color=16)):
            with self.assertRaises(ValueError):
                bad()

    def test_known_bytes_cow(self):
        raw = wl.encode_payload(wl.mob(0.5, 65.0, 0.5, "Cow"))
        self.assertEqual(raw[:10], bytes([8, 0, 2]) + b"id" + b"\x00\x03Cow")
        self.assertEqual(raw[10:15], bytes([9, 0, 3]) + b"Po")
        self.assertTrue(raw.endswith(bytes([2, 0, 10]) + b"AttackTime"
                                     + b"\x00\x00" + b"\x00"))

    def test_chunk_round_trip_and_marker(self):
        w = build_animals()
        self.assertEqual(w.problems(), [])
        self.assertEqual(len(w.entities()), 4)
        with tempfile.TemporaryDirectory() as d:
            w.save(d)
            with open(os.path.join(d, "region", "r.0.0.mcr"), "rb") as f:
                (tag, _), = wl.read_region(f.read()).values()
            lv = wl.child(tag, "Level")
            ents = wl.child(lv, "Entities")
            self.assertEqual(ents.elem, wl.TAG_COMPOUND)
            self.assertEqual([wl.child(e, "id").value for e in ents.value],
                             ["Sheep", "Cow", "Pig", "Chicken"])
            # Pure-Beta chunk: the native marker would make a moved mob's
            # save fail (worldgen_lib.chunk_nbt).
            self.assertIsNone(wl.child(lv, "CaveWiiTileEntitiesComplete"))
            back = wl.load_world(d)
            self.assertEqual(back.entities(), w.entities())
        plain = wl.World("p")
        plain.set(0, 0, 0, "stone")
        plain.compute_light()
        _, root = wl.decode_root(plain.chunk_nbt(plain.chunk(0, 0)))
        self.assertEqual(wl.path(root, "Level.CaveWiiTileEntitiesComplete")
                         .value, 1)

    def test_problems_catch_bad_footing(self):
        w = wl.World("m")
        w.fill(0, 64, 0, 15, 64, 15, "grass")
        w.set(3, 66, 3, "stone")                      # head in stone
        w.mob(3.5, 65.0, 3.5, "Cow")
        w.mob(5.5, 66.0, 5.5, "Pig")                  # floating
        w.set(7, 65, 7, "fence")
        w.mob(7.5, 65.0, 7.5, "Sheep")                # inside a fence
        p = w.problems()
        self.assertEqual(len(p), 3, p)


class RegionTests(unittest.TestCase):
    def test_pack_layout(self):
        recs = {(0, 0): zlib.compress(b"a" * 10), (31, 1): b"x" * 5000}
        blob = wl.pack_region(recs, timestamp=0xFFFFFFFF)
        self.assertEqual(len(blob) % wl.SECTOR, 0)
        e0 = struct.unpack_from(">I", blob, 0)[0]
        e1 = struct.unpack_from(">I", blob, (31 + 32) * 4)[0]
        self.assertEqual(e0, (2 << 8) | 1)
        self.assertEqual(e1, (3 << 8) | 2)  # 5005 bytes -> 2 sectors
        stamp = struct.unpack_from(">I", blob, wl.SECTOR)[0]
        self.assertEqual(stamp & 0x80000000, 0)  # canonical flag clear
        length, ctype = struct.unpack_from(">IB", blob, 3 * wl.SECTOR)
        self.assertEqual((length, ctype), (5001, 2))

    def test_negative_chunks_and_round_trip(self):
        w = wl.World("neg")
        w.set(-1, 10, -17, "stone")
        w.chest(-1, 11, -17, [(0, 1, 1)])
        self.assertEqual(wl.region_file_name(-1, -2), "r.-1.-1.mcr")
        with tempfile.TemporaryDirectory() as d:
            files = w.save(d)
            self.assertEqual(files, ["r.-1.-1.mcr"])
            back = wl.load_world(d)
            self.assertEqual(back.get(-1, 10, -17), (1, 0))
            self.assertIsNotNone(back.tile_entity(-1, 11, -17))
            with open(os.path.join(d, "region", files[0]), "rb") as f:
                recs = wl.read_region(f.read())
            (lv,) = [wl.child(t, "Level") for t, _ in recs.values()]
            sizes = {n: len(v.value) for n, v in lv.value
                     if v.type == wl.TAG_BYTE_ARRAY}
            self.assertEqual(sizes, {"Blocks": 32768, "Data": 16384,
                                     "SkyLight": 16384, "BlockLight": 16384,
                                     "HeightMap": 256})
            self.assertEqual((wl.child(lv, "xPos").value,
                              wl.child(lv, "zPos").value), (-1, -2))


class BedTests(unittest.TestCase):
    def test_beta_bed_metadata(self):
        # PAR-BED-META: BlockBed.headBlockToFootBlockMap order
        # {+Z, -X, -Z, +X}; foot = dir, head = dir | 8.
        w = wl.World("b")
        cases = {"south": (0, (0, 1)), "west": (1, (-1, 0)),
                 "north": (2, (0, -1)), "east": (3, (1, 0))}
        for i, (facing, (meta, (dx, dz))) in enumerate(sorted(cases.items())):
            x, z = 2 + 4 * i, 8
            w.bed(x, 64, z, facing)
            self.assertEqual(w.get(x, 64, z), (26, meta), facing)
            self.assertEqual(w.get(x + dx, 64, z + dz), (26, meta | 8), facing)
        self.assertEqual(wl.SAVE_VERSION, 5)


class LightTests(unittest.TestCase):
    def test_table_matches_game_source(self):
        self.assertEqual(light_tables.extract_light_table(REPO),
                         light_tables.BAKED_LIGHT_TABLE)

    def test_sealed_room_torch(self):
        w = wl.World("t")
        w.fill(0, 0, 0, 15, 20, 15, "stone")
        w.fill(4, 5, 4, 10, 8, 10, "air")
        w.torch(7, 5, 7)
        w.compute_light()
        self.assertEqual(w.light_at(7, 5, 7), (0, 14))
        self.assertEqual(w.light_at(8, 5, 7), (0, 13))
        self.assertEqual(w.light_at(10, 8, 10), (0, 14 - 3 - 3 - 3))
        self.assertEqual(w.light_at(3, 5, 7), (0, 0))  # inside stone

    def test_sky_and_water(self):
        w = wl.World("t")
        w.fill(0, 0, 0, 15, 10, 15, "stone")
        w.fill(0, 11, 0, 15, 13, 15, "water")
        w.compute_light()
        self.assertEqual(w.chunk(0, 0).heightmap[0], 14)
        self.assertEqual(w.light_at(5, 14, 5)[0], 15)
        self.assertEqual(w.light_at(5, 13, 5)[0], 12)
        self.assertEqual(w.light_at(5, 11, 5)[0], 6)
        self.assertEqual(w.light_at(5, 10, 5)[0], 0)

    def test_overhang_spreads_sideways(self):
        w = wl.World("t")
        w.fill(0, 0, 0, 15, 9, 15, "stone")
        w.fill(0, 12, 0, 7, 12, 15, "stone")       # roof over x 0..7
        w.compute_light()
        self.assertEqual(w.light_at(8, 11, 4)[0], 15)
        self.assertEqual(w.light_at(7, 11, 4)[0], 14)
        self.assertEqual(w.light_at(5, 10, 4)[0], 15 - 3)  # 3 cells from x=8


class DemoTests(unittest.TestCase):
    def test_demo_clean_and_deterministic(self):
        w = demo_scene.build_demo()
        self.assertEqual(w.problems(), [])
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, "a"), os.path.join(d, "b")
            demo_scene.build_demo().save(a)
            demo_scene.build_demo().save(b)
            for rel in ("level.dat", os.path.join("region", "r.0.0.mcr")):
                with open(os.path.join(a, rel), "rb") as fa, \
                        open(os.path.join(b, rel), "rb") as fb:
                    self.assertEqual(fa.read(), fb.read(), rel)
            with open(os.path.join(a, "level.dat"), "rb") as f:
                _, root = wl.decode_root(gzip.decompress(f.read()))
            self.assertEqual(wl.path(root, "Data.CaveWiiSaveVersion").value, 5)
            self.assertEqual(wl.path(root, "Data.Player.Health").value, 20)  # Beta half-hearts
            self.assertEqual(wl.path(root, "Data.Player.CaveWiiHealth").value, 160)
            back = wl.load_world(a)
            self.assertEqual(back.light_at(19, 67, 21), (8, 14))
            self.assertEqual(back.light_at(23, 65, 19)[1], 13)
            self.assertEqual(back.get(18 + 5, 65, 30), (59, 5))
            out = preview.preview(back, os.path.join(d, "p.png"), layers=(65,))
            for p in out:
                with open(p, "rb") as f:
                    self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")


def main():
    argv = list(sys.argv)
    fixture = None
    if "--fixture-out" in argv:
        k = argv.index("--fixture-out")
        fixture = argv[k + 1]
        del argv[k:k + 2]
    prog = unittest.main(argv=argv, exit=False)
    if not prog.result.wasSuccessful():
        sys.exit(1)
    if fixture:
        out = os.path.join(fixture, "Tutorial Demo")
        shutil.rmtree(out, ignore_errors=True)
        w = demo_scene.build_demo()
        w.save(out)
        print("fixture written:", out)
        out = os.path.join(fixture, "Tutorial Animals")
        shutil.rmtree(out, ignore_errors=True)
        build_animals().save(out)
        print("fixture written:", out)


if __name__ == "__main__":
    main()
