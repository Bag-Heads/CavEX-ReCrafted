#!/usr/bin/env python3
"""
Acceptance test for the shipped tutorial world builder
(tools/tutorial_world/build_tutorial.py, layout-spec v1).

    python tests/test_tutorial_build.py

Builds the whole world ONCE, saves it into a temporary directory, reads it
back through worldgen_lib.load_world, and asserts:
  * check_problems() == [] (World.problems() + the builder's own rules), and
    raw World.problems() holds only the two documented library false
    positives (floor torch on a fence; MobSpawner tile entity);
  * no dark spawn spot on any walkable cell (roads, floors, plaza, yards,
    causeway, quarry, spiral, gallery) -- recomputed on the RELOADED save;
  * every sign line <= 15 characters; every chest non-empty;
  * 1024 chunks in one region; spawn/time as the spec says; deterministic.
"""
import os
import re
import sys
import tempfile
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "tutorial_world"))
import worldgen_lib as wl  # noqa: E402
import build_tutorial as bt  # noqa: E402

_T = None
_DIR = None


def setUpModule():
    global _T, _DIR
    _T = bt.build()
    _DIR = tempfile.TemporaryDirectory()
    _T.w.save(os.path.join(_DIR.name, "CaveWii Tutorial"), light=False)


def tearDownModule():
    _DIR.cleanup()


class TutorialBuild(unittest.TestCase):
    def saved(self):
        return os.path.join(_DIR.name, "CaveWii Tutorial")

    def test_no_problems(self):
        self.assertEqual(bt.check_problems(_T.w), [])
        allowed = re.compile(r"^(no mount block: 50@|tile entity without its "
                             r"block at)")
        raw = [p for p in _T.w.problems() if not allowed.match(p)]
        self.assertEqual(raw, [])

    def test_region_and_level(self):
        files = sorted(os.listdir(os.path.join(self.saved(), "region")))
        self.assertEqual(files, ["r.0.0.mcr"])
        self.assertEqual(len(_T.w.chunks), 1024)
        self.assertEqual(_T.w.spawn, (200, 65, 304))
        self.assertEqual(_T.w.time, 1000)

    def test_walkable_cells_lit_after_reload(self):
        back = wl.load_world(self.saved())
        cols = {}
        for (x, y, z) in _T.walk:
            cols.setdefault((x, z), set()).add(y)
        self.assertGreater(len(cols), 3000)
        dark = []
        for (x, z), ys in sorted(cols.items()):
            dark += [p for p in back.dark_spawn_spots(x, z, x, z) if p[1] in ys]
        self.assertEqual(dark, [])
        for p in _T.growth:
            self.assertGreaterEqual(back.light_at(*p)[1], 9, p)

    def test_floating_sign_is_mob_safe(self):
        # no dark spawnable cell anywhere on the floating CAVEX sign
        back = wl.load_world(self.saved())
        x1, z1, x2, z2, ylo = _T.cavex_box
        self.assertGreaterEqual(z2 - z1, 50)       # the whole 5-letter sign
        bad = [p for p in back.dark_spawn_spots(x1, z1, x2, z2)
               if p[1] >= ylo]
        self.assertEqual(bad, [])

    def test_floating_sign_shadow_on_water(self):
        # every column the floating sign covers looks straight down onto
        # water: its skylight shadow never lands on land, a road or a yard
        back = wl.load_world(self.saved())
        x1, z1, x2, z2, ylo = _T.cavex_box
        cols = 0
        for x in range(x1, x2 + 1):
            for z in range(z1, z2 + 1):
                ys = [y for y in range(ylo, wl.WORLD_HEIGHT)
                      if back.get(x, y, z)[0] != 0]
                if not ys:
                    continue
                cols += 1
                y = min(ys) - 1
                while y > 0 and back.get(x, y, z)[0] == 0:
                    y -= 1
                self.assertIn(back.get(x, y, z)[0], (8, 9),
                              "sign shadow on land at %d,%d" % (x, z))
        self.assertGreaterEqual(cols, 100)

    def test_signs_and_chests(self):
        back = wl.load_world(self.saved())
        n_sign = n_chest = 0
        for c in back.chunks.values():
            for te in c.tile_entities.values():
                kind = wl.child(te, "id").value
                if kind == "Sign":
                    n_sign += 1
                    for k in range(1, 5):
                        self.assertLessEqual(
                            len(wl.child(te, "Text%d" % k).value), 15)
                elif kind == "Chest":
                    n_chest += 1
                    self.assertTrue(wl.child(te, "Items").value,
                                    "empty chest at %s" % (
                                        wl.child(te, "x").value,))
        self.assertEqual(n_sign, len(_T.signs))
        self.assertEqual(n_chest, len(_T.chests))
        self.assertGreaterEqual(n_chest, 12)

    def test_spawners_and_dark_cell(self):
        self.assertEqual(len(_T.spawners), 2)
        self.assertGreaterEqual(_T.dark_cell_spots, 10)

    def test_deterministic(self):
        other = bt.build()
        for key, c in _T.w.chunks.items():
            o = other.w.chunks[key]
            self.assertEqual(bytes(c.blocks), bytes(o.blocks), key)
            self.assertEqual(bytes(c.meta), bytes(o.meta), key)
        self.assertEqual(sorted(_T.walk), sorted(other.walk))


if __name__ == "__main__":
    unittest.main()
