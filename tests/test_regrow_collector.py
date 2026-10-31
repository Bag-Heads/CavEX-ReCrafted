"""colcap-1006 (ASTRA-CC-R5 item 3): split_regrow_8m grading (RegrowEvidence)
and the allocator cost block ("pac"), through the production collector.

The tape is built from a VERBATIM real-Wii heartbeat (cc_m1 tick 30) given
the current pool schema, the device's own event shapes (column_pool_grow /
_shrink as the C formatter emits them, anchor_cycle as main.c emits it):

  * the honest route -- 4 -> 8 slabs while loading, separated 0-90 s,
    reunited 90-180 s with three hand-backs to 5 slabs, reseparated with
    three successful carves back to 8 -- certifies (PASS.json) and the
    summary reports drain, regrowth, the warmed-heap largest block and
    the per-op allocator costs;
  * every way the claim can be missing FAILS: no drain, a failed regrow
    carve, regrowth stopping at 6, transitions out of order / refused /
    late, an OOM/spill/truncation, the carve or MEM1 floor, pac missing or
    malformed, a short window, historical (cpv-less) evidence;
  * "pac" on an anchor profile is parsed and summarised, never required
    (retained pre-R5 tapes still certify).
"""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "regrow_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)

PROFILE = "split_regrow_8m"
FIX_M1 = ROOT / "tests/fixtures/cc_m1_s12345_anchor_pool.ndjson"
SLAB = 2638080


def base_hb():
    for line in FIX_M1.read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        if row.get("event") == "heartbeat" and row["tick"] == 30:
            for k in ("cpv", "cps", "dll", "dlb"):
                row.pop(k, None)
            return row
    raise AssertionError("fixture heartbeat missing")


BASE = base_hb()


def hb(tick, ms, slabs, used, seq, shrinks, measuring=True, **over):
    per = [32] * slabs
    left = slabs * 32 - used
    for i in range(slabs - 1, -1, -1):
        take = min(per[i], left)
        per[i] -= take
        left -= take
    row = copy.deepcopy(BASE)
    row.update(event="heartbeat", run_id="rg", profile=PROFILE, tick=tick,
               cpv=3, column_pool_used=used, column_pool_peak=242,
               column_pool_slabs=slabs, column_pool_overflow=0,
               column_pool_carve_fail=0, budget_valid=True, budget_wii=True,
               budget_resident=used, budget_live_backings=used,
               budget_reserved=0, budget_prepeak=0, budget_peak=0,
               budget_waits=0, budget_phase=0, cps=per, dll=3000, dlb=9000000,
               cpe=[seq, shrinks, 0, 0], anchor_measuring=measuring,
               anchor_ms=ms, anchor_run_faults=[0, 0, 0],
               mem2_carve_free_kb=5000, mem1_free_kb=6200,
               pac=[3, 900, 400, 4, 300, 120, 2, 50, 40])
    row.update(over)
    return row


def grow(seq, before, success=True, largest=19000000):
    return dict(event="column_pool_grow", run_id="rg", seq=seq,
                requested_bytes=SLAB, before_valid=True,
                free_bytes_before=20000000,
                largest_allocatable_bytes_before=largest,
                slabs_before=before, slabs_after=before + 1 if success else before,
                success=success)


def shrink(seq, before):
    return dict(event="column_pool_shrink", run_id="rg", seq=seq,
                slab=before - 1, released_bytes=SLAB, slabs_before=before,
                slabs_after=before - 1)


def cycle(phase, ms, offset, slabs, used, ok=True):
    return dict(event="anchor_cycle", run_id="rg", phase=phase, anchor_ms=ms,
                offset=offset, ok=ok, slabs=slabs, used=used)


def route():
    rows = [hb(1, 0, 4, 0, 0, 0, measuring=False)]
    rows += [grow(1, 4), grow(2, 5), grow(3, 6), grow(4, 7)]
    t = 2
    for ms in range(0, 90000, 1000):
        rows.append(hb(t, ms, 8, 240, 4, 0)); t += 1
    rows.append(cycle("reunited", 90016, 0, 8, 240))
    rows.append(hb(t, 95000, 8, 200, 4, 0)); t += 1
    rows += [shrink(5, 8), shrink(6, 7), shrink(7, 6)]
    for ms in range(100000, 180000, 1000):
        rows.append(hb(t, ms, 5, 121, 7, 3)); t += 1
    rows.append(cycle("reseparated", 180016, 256, 5, 121))
    rows += [grow(8, 5, largest=9000000), grow(9, 6, largest=6000000),
             grow(10, 7, largest=3500000)]
    for ms in range(185000, 361000, 1000):
        rows.append(hb(t, ms, 8, 242, 10, 3)); t += 1
    rows.append(dict(event="profile_done", run_id="rg", status="ok"))
    return rows


def grade(rows):
    td = tempfile.TemporaryDirectory()
    c = collector.Collector(collector._selftest_args(
        Path(td.name), "rg", PROFILE, expected_build="", duration=0))
    c.world_loaded_seen = True
    for row in rows:
        c.process(row)
        if c.done:
            break
    c.hb_count = max(c.hb_count, c.profile["min_hb"])
    if not c.done:
        c._finish_normal("duration reached")
    c.write_result()
    files = {n for n in ("PASS.json", "FAIL.json", "NONCERTIFYING.json")
             if (Path(td.name) / n).is_file()}
    c._td = td
    return c, files


def test_honest_route_certifies():
    c, files = grade(route())
    assert files == {"PASS.json"}, (c.fail_reason, c.regrow.errors)
    s = c.regrow.summary()
    assert [x["phase"] for x in s["cycle"]] == ["reunited", "reseparated"]
    assert s["drain_handbacks"] == 3 and s["slabs_by_phase"]["reunited"]["min"] == 5
    assert s["regrow_reached_slabs"] == 8
    assert s["regrow_min_largest_allocatable_bytes"] == 3500000
    cost = s["allocator_cost"]
    assert cost["col_alloc"]["avg_us"] == 300.0 and cost["col_alloc"]["max_us"] == 400
    assert cost["lock_wait"]["count"] == cost["heartbeats"] * 2
    out = json.loads((Path(c.args.out) / "PASS.json").read_text())
    assert out["regrow"]["regrow_reached_slabs"] == 8
    print("PASS honest route: drain 8 -> 5 (3 hand-backs), regrow 5 -> 8, "
          "costs %s" % json.dumps(cost["col_alloc"]))


def mutate(fn):
    rows = route()
    fn(rows)
    return rows


def test_failures():
    def idx(rows, pred):
        return next(i for i, r in enumerate(rows) if pred(r))

    def no_drain(rows):
        rows[:] = [r for r in rows if r.get("event") != "column_pool_shrink"]
        for r in rows:
            if r.get("event") == "heartbeat" and r.get("column_pool_slabs") == 5:
                r.update(column_pool_slabs=8, cps=[32] * 3 + [25] + [0] * 4,
                         cpe=[4, 0, 0, 0])
            if r.get("event") == "heartbeat" and r.get("cpe", [0])[0] == 10:
                r.update(cpe=[7, 0, 0, 0])
        for r in rows:
            if r.get("event") == "column_pool_grow" and r["seq"] >= 8:
                r["seq"] -= 3

    def regrow_fails(rows):
        i = idx(rows, lambda r: r.get("event") == "column_pool_grow" and r["seq"] == 10)
        rows[i] = grow(10, 7, success=False)

    def regrow_to_six(rows):
        rows[:] = [r for r in rows if not (r.get("event") == "column_pool_grow"
                                           and r["seq"] in (9, 10))]
        for r in rows:
            if r.get("event") == "heartbeat" and r.get("cpe", [0])[0] == 10:
                r.update(column_pool_slabs=6, cps=[32] * 6, column_pool_used=192,
                         cpe=[8, 3, 0, 0])

    cases = {
        "no drain": no_drain,
        "regrow carve failed": regrow_fails,
        "regrow stops at 6": regrow_to_six,
        "transitions out of order": lambda rows: rows.insert(1, cycle("reseparated", 180016, 256, 4, 0)),
        "transition refused": lambda rows: rows[idx(rows, lambda r: r.get("phase") == "reunited")].update(ok=False),
        "transition late": lambda rows: rows[idx(rows, lambda r: r.get("phase") == "reunited")].update(anchor_ms=99000),
        "spill in window": lambda rows: rows[-3].update(anchor_run_faults=[0, 1, 0]),
        "carve floor": lambda rows: rows[-3].update(mem2_carve_free_kb=2000),
        "mem1 floor": lambda rows: rows[-3].update(mem1_free_kb=4000),
        "pac missing": lambda rows: rows[-3].pop("pac"),
        "pac malformed": lambda rows: rows[-3].update(pac=[1, 2, 3]),
        "pac max > total": lambda rows: rows[-3].update(pac=[1, 2, 3, 0, 0, 0, 0, 0, 0]),
        "historical evidence": lambda rows: [r.pop("cpv", None) for r in rows],
    }

    def short(rows):
        rows[:] = [r for r in rows if not (r.get("event") == "heartbeat"
                                           and r.get("anchor_ms", 0) > 250000)]
    cases["short window"] = short
    for name, fn in cases.items():
        c, files = grade(mutate(fn))
        assert files == {"FAIL.json"}, (name, files, c.regrow.errors)
        print("PASS %s -> FAIL (%s)" % (name, (c.fail_reason or "")[:110]))


def test_pac_on_anchor_profile_is_parsed_not_required():
    a = collector.AnchorEvidence("split_anchor_6m")
    for ev in (grow(1, 4), grow(2, 5), grow(3, 6), grow(4, 7)):
        a.note_pool_event(ev)
    h = hb(2, 0, 8, 240, 4, 0, profile="split_anchor_6m")
    a.pool.note_heartbeat(h)
    h2 = dict(h)
    h2.pop("pac")
    a.pool.note_heartbeat(h2)
    a.pool.finish()
    assert a.errors == [], a.errors
    assert a.pool_summary()["allocator_cost"]["heartbeats"] == 1
    bad = collector.AnchorEvidence("split_anchor_6m")
    for ev in (grow(1, 4), grow(2, 5), grow(3, 6), grow(4, 7)):
        bad.note_pool_event(ev)
    bad.pool.note_heartbeat(dict(h, pac=[1] * 8))
    assert "malformed allocator cost (pac)" in bad.errors
    print("PASS pac on split_anchor_6m: parsed + summarised, optional, malformed rejected")


if __name__ == "__main__":
    test_honest_route_certifies()
    test_failures()
    test_pac_on_anchor_profile_is_parsed_not_required()
    print("PASS test_regrow_collector")
