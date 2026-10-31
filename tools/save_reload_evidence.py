"""save_reload_3m (SPEC R12) collector contract; imported by the Wii collector.

The device (source/game/save_reload_script.c + source/network/
save_reload_probe.c) sends, in order:

  save_reload_phase  markers create_world, world_loaded_1, fixture_applied,
                     pause_open, exit_focused, save_quit_clicked,
                     saved_at_title, reload_armed, world_loaded_2,
                     dwell_done (30 s in game after the reload; seq 1..10,
                     contiguous; a "fail" marker names the driver's reason)
  save_reload        the verdict: before (hashed immediately before the
                     production save-and-quit) and after (hashed at the
                     reload's load-completion pass, before the world clock
                     resumes) snapshots of inventory, chest, furnace, edited
                     blocks and player position
  profile_done       status "ok"

PASS needs ALL of: every marker in order; the verdict schema; the world
loaded twice (two client load edges AND two server load completions); exactly
one published save; the fixture applied and intact on both sides and
resident; the furnace smelting at the save (burn > 0); every before hash equal
to its after hash; the device status "ok"; profile_done "ok". The collector
recomputes the equality itself -- it never trusts the device's status alone.
"""
import math
import re

PROFILE = "save_reload_3m"
MARKERS = ("create_world", "world_loaded_1", "fixture_applied", "pause_open",
           "exit_focused", "save_quit_clicked", "saved_at_title",
           "reload_armed", "world_loaded_2", "dwell_done")
HASHES = ("inv", "chest", "furnace", "blocks", "pos")
_HEX16 = re.compile(r"^[0-9a-f]{16}$")


def _int(v):
    return type(v) is int and v >= 0


class SaveReloadEvidence:
    def __init__(self, run_id=None):
        self.run_id = run_id
        self.markers = []
        self.verdict = None
        self.done = None
        self.errors = []

    def _err(self, msg):
        if msg not in self.errors:
            self.errors.append(msg)

    def note(self, ev):
        kind = ev.get("event")
        if self.run_id is not None and ev.get("run_id") not in (None, self.run_id):
            self._err("run_id mismatch on %s" % kind)
            return
        if kind == "save_reload_phase":
            self._note_marker(ev)
        elif kind == "save_reload":
            if self.verdict is not None:
                self._err("duplicate save_reload verdict")
                return
            self.verdict = ev
            self._check_verdict(ev)

    def _note_marker(self, ev):
        if self.verdict is not None:
            self._err("phase marker after the verdict")
        marker = ev.get("marker")
        if marker == "fail":
            self._err("driver failed: %s (phase %s)"
                      % (ev.get("reason") or "?", ev.get("phase")))
            return
        if ev.get("seq") != len(self.markers) + 1:
            self._err("phase marker seq not contiguous")
        if len(self.markers) >= len(MARKERS) or marker != MARKERS[len(self.markers)]:
            self._err("phase marker out of order: %s" % marker)
            return
        self.markers.append(marker)

    def _check_snap(self, name, snap):
        if (type(snap) is not dict
                or any(not isinstance(snap.get(h), str) or not _HEX16.match(snap[h])
                       for h in HASHES)
                or type(snap.get("burn")) is not int or type(snap.get("cook")) is not int
                or any(type(snap.get(k)) not in (int, float)
                       or not math.isfinite(snap[k]) for k in ("x", "y", "z"))
                or type(snap.get("fixture_ok")) is not bool
                or type(snap.get("resident")) is not bool):
            self._err("malformed %s snapshot" % name)
            return False
        if not snap["fixture_ok"]:
            self._err("%s: fixture not intact (cells/chest/furnace/inventory)" % name)
        if not snap["resident"]:
            self._err("%s: fixture chunks not resident when hashed" % name)
        return True

    def _check_verdict(self, ev):
        if ev.get("schema") != 1:
            self._err("save_reload schema != 1")
            return
        if not all(_int(ev.get(k)) for k in ("client_loads", "server_loads", "saves",
                                             "after_loads", "applied", "ms")):
            self._err("malformed save_reload counters")
            return
        if not isinstance(ev.get("world"), str) or not ev["world"]:
            self._err("verdict names no world folder")
        if ev["client_loads"] != 2 or ev["server_loads"] != 2:
            self._err("world did not load exactly twice (client %d, server %d)"
                      % (ev["client_loads"], ev["server_loads"]))
        if ev["saves"] != 1:
            self._err("expected exactly one save-and-quit, saw %d" % ev["saves"])
        if ev["after_loads"] != 1:
            self._err("expected one post-fixture reload snapshot, saw %d" % ev["after_loads"])
        if ev["applied"] != 1:
            self._err("fixture never applied")
        before, after = ev.get("before"), ev.get("after")
        if not (self._check_snap("before", before) and self._check_snap("after", after)):
            return
        if before["burn"] <= 0:
            self._err("furnace was not smelting at the save (burn %d)" % before["burn"])
        diff = [h for h in HASHES if before[h] != after[h]]
        if diff:
            self._err("reload differs from the save: %s" % ",".join(diff))
        if ev.get("status") != "ok":
            self._err("device verdict %s" % ev.get("status"))

    def note_profile_done(self, status):
        if self.done is not None:
            self._err("duplicate profile_done")
        self.done = status
        if status != "ok":
            self._err("profile_done status %s" % status)

    def complete_error(self):
        if self.errors:
            return "; ".join(self.errors[:6])
        if len(self.markers) != len(MARKERS):
            return "missing phase marker %s" % MARKERS[len(self.markers)]
        if self.verdict is None:
            return "no save_reload verdict"
        if self.done is None:
            return "profile_done missing"
        return None

    def summary(self):
        v = self.verdict or {}
        return {"markers": list(self.markers), "world": v.get("world"),
                "client_loads": v.get("client_loads"),
                "server_loads": v.get("server_loads"), "saves": v.get("saves"),
                "before": v.get("before"), "after": v.get("after"),
                "device_status": v.get("status"), "profile_done": self.done,
                "error": self.complete_error()}


def sample_run(run_id="r1"):
    """A canonical passing tape (the selftest's and the PC test's base)."""
    snap = {"inv": "0123456789abcdef", "chest": "1111111111111111",
            "furnace": "2222222222222222", "blocks": "3333333333333333",
            "pos": "4444444444444444", "burn": 1400, "cook": 37,
            "x": 0.5, "y": 4.0, "z": 0.5, "fixture_ok": True, "resident": True}
    rows = [{"event": "save_reload_phase", "run_id": run_id, "seq": i + 1,
             "marker": m, "phase": "x", "ms": 1000 * i, "client_loads": 1,
             "reason": ""} for i, m in enumerate(MARKERS)]
    rows.append({"event": "save_reload", "schema": 1, "run_id": run_id,
                 "status": "ok", "phase": "done", "ms": 60000,
                 "world": "cw_save_reload", "client_loads": 2,
                 "server_loads": 2, "saves": 1, "after_loads": 1, "applied": 1,
                 "before": dict(snap), "after": dict(snap)})
    return rows


def grade(rows, done="ok", run_id="r1"):
    ev = SaveReloadEvidence(run_id)
    for r in rows:
        ev.note(r)
    if done is not None:
        ev.note_profile_done(done)
    return ev.complete_error()


def selftest():
    """The contract must PASS its canonical tape and FAIL each mutation."""
    assert grade(sample_run()) is None, grade(sample_run())
    import copy
    muts = []

    def mut(fn):
        rows = copy.deepcopy(sample_run())
        fn(rows)
        muts.append(rows)
    mut(lambda r: r[-1]["after"].__setitem__("chest", "9" * 16))
    mut(lambda r: r[-1]["after"].__setitem__("pos", "8" * 16))
    mut(lambda r: r[-1]["after"].__setitem__("inv", "7" * 16))
    mut(lambda r: r[-1]["after"].__setitem__("furnace", "6" * 16))
    mut(lambda r: r[-1]["after"].__setitem__("blocks", "5" * 16))
    mut(lambda r: r[-1].__setitem__("client_loads", 1))
    mut(lambda r: r[-1].__setitem__("server_loads", 3))
    mut(lambda r: r[-1].__setitem__("saves", 0))
    mut(lambda r: r[-1].__setitem__("applied", 0))
    mut(lambda r: r[-1]["before"].__setitem__("burn", 0))
    mut(lambda r: r[-1]["before"].__setitem__("fixture_ok", False))
    mut(lambda r: r[-1]["after"].__setitem__("resident", False))
    mut(lambda r: r[-1].__setitem__("status", "hash_mismatch"))
    mut(lambda r: r[-1]["after"].__setitem__("inv", "XYZ"))
    mut(lambda r: r.pop(5))           # save_quit_clicked missing
    mut(lambda r: r.pop(-2))          # dwell_done missing (no post-reload window)
    mut(lambda r: r.pop())            # no verdict
    mut(lambda r: r.__setitem__(1, dict(r[1], marker="pause_open")))
    for rows in muts:
        assert grade(rows) is not None, rows
    assert grade(sample_run(), done="save_never_published") is not None
    assert grade(sample_run(), done=None) is not None
    assert grade(sample_run("other"), run_id="r1") is not None
    return len(muts) + 3


if __name__ == "__main__":
    import sys
    if "--selftest" in sys.argv:
        print("save_reload_evidence selftest: %d mutations refused, canonical PASS"
              % selftest())
