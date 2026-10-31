"""colcap-1006 (ASTRA-CC-R3 items 2-3, R4 points 9-10): anchor-run column-pool
evidence in the collector, through the REAL parser (ingest_telemetry_line ->
Collector -> AnchorEvidence -> ColumnPoolEvidence), not a re-implementation.

What this pins:
  * every gate violation ACCUMULATES (bounded at 32 retained, with an honest
    count of the rest) -- carve floor AND measured-window spill both reported;
  * grow/shrink events are validated, ordered (a current build numbers them
    gap-free, so a gap is a lost event), chained on slabs_before, and give
    every slab a stable carve-order identity across hand-back and regrowth;
  * "cpv":3 heartbeats are REQUIRED on a live run (missing, outdated or
    malformed evidence fails), budget_valid must be true, cumulative shrink /
    shrink_fail / lost counters are checked, and every snapshot is reconciled
    against the events up to its stamped seq -- even when the heartbeat's
    priority lane overtakes the events on the wire;
  * reclaimable slabs apply the device's own 16-slot headroom rule;
  * historical tapes (pre-cpv3) replay for what they carry, labelled
    non-certifying.

Hardware fixtures (VERBATIM raw lines, byte-for-byte):
  * tests/fixtures/cc_c1_s12345_anchor_pool.ndjson -- real Wii cc_c1
    (build 1b334e5): four column_pool_grow events, heartbeats 27-30/184-188.
  * tests/fixtures/cc_m1_s12345_anchor_pool.ndjson -- real Wii cc_m1
    (build 1090e6f, cpv 2): four grows, heartbeats 28-31 (slab 8 empty) and
    183-187 (the carve trough: no slab empty).
Excerpts are non-contiguous, so the frame-continuity gate also fires there;
that is asserted explicitly. Full tapes are graded too when present locally.
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
    "anchor_pool_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)

FIX_C1 = ROOT / "tests/fixtures/cc_c1_s12345_anchor_pool.ndjson"
FIX_M1 = ROOT / "tests/fixtures/cc_m1_s12345_anchor_pool.ndjson"
FULL_C1 = ROOT / "tmp/wii_runs/cc_c1_s12345/telemetry.ndjson"
FULL_M1 = Path.home() / (".codex/worktrees/colcap-meas/CaveWii_v2/tmp/"
               "wii_runs/cc_m1_s12345/telemetry.ndjson")
CARVE = "dedicated carve below 2246 KiB reserve"
SPILL = "allocation failure, spill or truncation in measured window"
FRAMES = "missing per-frame samples"
SLAB = 2638080
NO_EVIDENCE = "no current-schema column pool evidence in the run"


def lines_of(path):
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(f)


def feed(lines, profile="split_anchor_6m", historical=None):
    """The real live ingest path; `historical` is decided exactly as
    replay_run_dir decides it when None."""
    rows = [json.loads(l) for l in lines if l.strip()]
    run_id = next(r["run_id"] for r in rows if r.get("run_id"))
    td = tempfile.TemporaryDirectory()
    c = collector.Collector(collector._selftest_args(
        Path(td.name), run_id, profile, expected_build="", duration=0))
    if historical is None:
        historical = not any(r.get("event") == "heartbeat"
                             and r.get("cpv") == collector.COLUMN_POOL_CPV
                             for r in rows)
    c.anchor.pool.historical = historical
    for line in lines:
        if collector.ingest_telemetry_line(c, line):
            break
    c._td = td
    return c


def hb_of(path, tick):
    return next(json.loads(l) for l in lines_of(path)
                if json.loads(l).get("event") == "heartbeat"
                and json.loads(l)["tick"] == tick)


# ---------------------------------------------------------------- fixtures
def test_fixtures_are_verbatim_hardware():
    for path, build, ticks in ((FIX_C1, "1b334e5", [27, 28, 29, 30, 184, 185, 186, 187, 188]),
                               (FIX_M1, "1090e6f", [28, 29, 30, 31, 183, 184, 185, 186, 187])):
        rows = [json.loads(l) for l in lines_of(path)]
        grows = [r for r in rows if r["event"] == "column_pool_grow"]
        assert [(g["seq"], g["slabs_before"], g["slabs_after"]) for g in grows] == [
            (1, 4, 5), (2, 5, 6), (3, 6, 7), (4, 7, 8)]
        hbs = [r for r in rows if r["event"] == "heartbeat"]
        assert [h["tick"] for h in hbs] == ticks
        assert all(h["build_id"].startswith(build) for h in hbs)
    assert all("cpv" not in json.loads(l) for l in lines_of(FIX_C1))
    assert {json.loads(l).get("cpv") for l in lines_of(FIX_M1)} == {None, 2}
    print("PASS fixtures: verbatim cc_c1 (no cpv) and cc_m1 (cpv 2) excerpts")


def test_cc_c1_historical_both_violations():
    c = feed(lines_of(FIX_C1))
    a = c.anchor
    assert a.pool.historical
    assert set(a.errors) == {CARVE, SPILL, FRAMES}, a.errors
    assert a.errors.index(CARVE) < a.errors.index(SPILL)
    s = a.pool_summary()
    assert (s["grows_ok"], s["slabs_boot"], s["slabs_reconstructed"],
            s["slabs_high_water"]) == (4, 4, 8, 8)
    assert s["evidence"].startswith("historical") and s["occupancy"] is None
    print("PASS cc_c1 excerpt: carve + spill; 4 grows 4 -> 8; historical label")


def test_cc_m1_historical_occupancy():
    c = feed(lines_of(FIX_M1))
    a = c.anchor
    assert a.pool.historical
    assert set(a.errors) == {CARVE, SPILL, FRAMES}, a.errors
    assert a.errors.index(CARVE) < a.errors.index(SPILL)
    s = a.pool_summary()
    assert (s["grows_ok"], s["slabs_reconstructed"], s["slabs_high_water"]) == (4, 8, 8)
    o = s["occupancy"]
    assert o["slab_identity"].startswith("index (historical")
    # Ticks 28-31: slab 8 empty, 162 used -> 7*32 - 162 = 62 >= 16 free after
    # a hand-back: ONE slab reclaimable. The trough: nothing empty.
    assert o["empty_nonboot_slabs_max"] == 1 and o["reclaimable_slabs_max"] == 1
    assert o["reclaimable_bytes_max"] == collector.COLUMN_POOL_SLAB_BYTES == SLAB
    assert o["heartbeats_reclaimable"] == 4
    m = o["at_measured_carve_min"]
    assert (m["tick"], m["carve_free_kb"], m["cps"], m["empty_nonboot"],
            m["reclaimable"], m["dll"], m["dlb"]) == (
        185, 893, [22, 27, 22, 19, 18, 9, 24, 21], 0, 0, 4527, 11905792), m
    assert o["dl_bytes_max"] == 11905792 and o["dl_lists_max"] == 4534
    print("PASS cc_m1 excerpt: carve + spill; slab 8 reclaimable at t28-31, "
          "none at the 893 KiB trough (4527 lists / 11.9 MB)")


def test_full_tapes_if_present():
    for path, name in ((FULL_C1, "cc_c1"), (FULL_M1, "cc_m1")):
        if not path.is_file():
            print("SKIP full %s replay: %s not present" % (name, path))
            continue
        c = feed(lines_of(path))
        if not c.done:
            c._finish_normal("offline replay")
        assert CARVE in c.anchor.errors and SPILL in c.anchor.errors, c.anchor.errors
        assert c.anchor.errors.index(CARVE) < c.anchor.errors.index(SPILL)
        assert c.failure_class == "anchor_feasibility_failed"
        s = c.anchor.pool_summary()
        assert (s["grows_ok"], s["slabs_boot"], s["slabs_reconstructed"]) == (4, 4, 8), s
        print("PASS full %s replay: %s; pool 4 -> 8 in 4 grows" % (name, c.fail_reason))


# ------------------------------------------------- current-schema (live) rules
def cur(base, slabs=8, cps=None, seq=0, shrinks=0, shrink_fail=0, lost=0, **over):
    cps = cps if cps is not None else [32] * 4 + [20, 10, 2, 0][: max(0, slabs - 4)]
    hb = copy.deepcopy(base)
    hb.update(cpv=3, column_pool_used=sum(cps), column_pool_peak=max(242, sum(cps)),
              column_pool_slabs=slabs, column_pool_overflow=0,
              column_pool_carve_fail=0, budget_valid=True, budget_wii=True,
              budget_resident=sum(cps), budget_live_backings=sum(cps),
              budget_reserved=0, budget_prepeak=0, budget_peak=0,
              budget_waits=0, budget_phase=0, cps=list(cps), dll=3000,
              dlb=9000000, cpe=[seq, shrinks, shrink_fail, lost])
    hb.update(over)
    return hb


def grow(seq, before, success=True, **extra):
    ev = dict(event="column_pool_grow", seq=seq, requested_bytes=SLAB,
              before_valid=True, free_bytes_before=20000000,
              largest_allocatable_bytes_before=19000000, slabs_before=before,
              slabs_after=before + 1 if success else before, success=success)
    ev.update(extra)
    return ev


def shrink(seq, before, slab=None, **extra):
    ev = dict(event="column_pool_shrink", seq=seq,
              slab=before - 1 if slab is None else slab, released_bytes=SLAB,
              slabs_before=before, slabs_after=before - 1)
    ev.update(extra)
    return ev


def pool(items, profile="split_anchor_6m", historical=False, finish=True):
    a = collector.AnchorEvidence(profile)
    a.pool.historical = historical
    for item in items:
        if item.get("event") in collector.COLUMN_POOL_EVENTS:
            a.note_pool_event(item)
        else:
            a.pool.note_heartbeat(item)
    if finish:
        a.pool.finish()
        if not historical and not any(i.get("event") not in collector.COLUMN_POOL_EVENTS
                                      for i in items):
            # Event-only sequences: the run-level "no snapshot at all" rule
            # (R5) must fire; it is asserted here once and removed so the
            # event-ordering cases below stay about ordering.
            assert NO_EVIDENCE in a.errors, a.errors
            a.errors.remove(NO_EVIDENCE)
    return a


BASE = None


def base():
    global BASE
    if BASE is None:
        BASE = hb_of(FIX_M1, 30)
        for k in ("cpv", "cps", "dll", "dlb"):
            BASE.pop(k, None)
    return BASE


GROWS = [grow(1, 4), grow(2, 5), grow(3, 6), grow(4, 7)]


def test_live_requires_current_evidence():
    b = base()
    ok = pool(GROWS + [cur(b, seq=4)])
    assert ok.errors == [], ok.errors
    assert pool(GROWS + [b]).errors == ["missing current-schema column pool evidence",
                                      "no current-schema column pool evidence in the run"]
    old = dict(b, cpv=2, cps=[32] * 4 + [20, 10, 2, 0], dll=1, dlb=1,
               column_pool_used=160, column_pool_peak=242, column_pool_slabs=8,
               column_pool_overflow=0, column_pool_carve_fail=0, budget_valid=True,
               budget_wii=True, budget_resident=0, budget_live_backings=0,
               budget_reserved=0, budget_prepeak=0, budget_peak=0, budget_waits=0,
               budget_phase=0)
    assert pool(GROWS + [old]).errors == ["outdated column pool evidence schema (cpv 2, need 3)",
                                        "no current-schema column pool evidence in the run"]
    assert pool(GROWS + [old], historical=True).errors == []
    assert pool(GROWS + [cur(b, seq=4), b]).errors == ["column pool evidence disappeared mid-run"]
    assert pool(GROWS + [cur(b, seq=4, budget_valid=False)]).errors == [
        "transfer budget snapshot was not coherent"]
    for over, want in ((dict(cpe=None), "missing or malformed column pool event stamp"),
                       (dict(cpe=[4, 0, 0]), "missing or malformed column pool event stamp"),
                       (dict(cpe=[4, 0, 0, -1]), "missing or malformed column pool event stamp"),
                       (dict(cps=[32] * 9), "per-slab occupancy disagrees with the column pool counters"),
                       (dict(cps="x"), "missing or malformed per-slab occupancy evidence"),
                       (dict(cpv=4), "missing or malformed column pool evidence"),
                       (dict(column_pool_used=-1), "missing or malformed column pool evidence")):
        h = cur(b, seq=4)
        h.update(over)
        a = pool(GROWS + [h])
        assert a.errors[:1] == [want], (over, a.errors)
    print("PASS live: current-schema evidence required; missing/outdated/"
          "malformed/incoherent-budget rejected; historical accepted")


def test_reconcile_and_overtaking():
    b = base()
    # The heartbeat (seq 4) overtakes events 3 and 4 on the wire: pending,
    # then reconciled when they arrive.
    a = pool([grow(1, 4), grow(2, 5), cur(b, seq=4), grow(3, 6), grow(4, 7)])
    assert a.errors == [] and a.pool.resolved == 1 and not a.pool.pending
    # A snapshot whose events never arrive is missing evidence at the end.
    a = pool([grow(1, 4), cur(b, seq=4)])
    assert a.errors == ["column pool events missing for heartbeat-stamped seq 4"], a.errors
    # Sol's repro: 5 reconstructed slabs beside 8 reported -> disagreement.
    a = pool([grow(1, 4), cur(b, slabs=8, seq=1)])
    assert a.errors == ["heartbeat pool snapshot disagrees with the ordered events"], a.errors
    # Cumulative hand-back counter must match the events too.
    a = pool(GROWS + [shrink(5, 8)] + [cur(b, slabs=7, cps=[32] * 4 + [20, 10, 2], seq=5, shrinks=0)])
    assert a.errors == ["heartbeat pool snapshot disagrees with the ordered events"], a.errors
    a = pool(GROWS + [shrink(5, 8)] + [cur(b, slabs=7, cps=[32] * 4 + [20, 10, 2], seq=5, shrinks=1)])
    assert a.errors == [], a.errors
    # Reordered events: seq 2 before seq 1, and a gap (lost event).
    a = pool([grow(2, 4), grow(1, 4)])
    assert "column pool event lost (sequence gap)" in a.errors
    assert "column pool event sequence regressed" in a.errors
    a = pool([grow(1, 4), grow(3, 5)])
    assert a.errors == ["column pool event lost (sequence gap)"], a.errors
    a = pool([grow(1, 4), grow(2, 6)])
    assert a.errors == ["column pool event chain broken (a grow/shrink event was lost)"]
    # Historical tapes may carry gaps (old builds skipped repeat failures).
    assert pool([grow(1, 4), grow(3, 5)], historical=True).errors == []
    # A regressing stamp is rejected.
    a = pool(GROWS + [cur(b, seq=4), cur(b, seq=3)])
    assert a.errors == ["column pool event stamp regressed"], a.errors
    print("PASS reconcile: overtaking heartbeat resolved; missing events, "
          "slab/shrink disagreement, reorder, gap, chain and stamp regress rejected")


def test_shrink_fail_lost_and_simultaneous():
    b = base()
    a = pool(GROWS + [cur(b, seq=4, shrink_fail=1, lost=2, column_pool_overflow=3,
                          column_pool_carve_fail=1)])
    assert a.errors == ["physical column pool overflow or carve failure",
                        "column slab hand-back refused by the heap (shrink_fail)",
                        "column pool events lost on the device (queue overflow)"], a.errors
    # The spill + carve + pool violations on ONE real heartbeat, all kept.
    hb = cur(hb_of(FIX_M1, 185), seq=4, shrink_fail=1)
    for k in ("cps",):
        pass
    a = collector.AnchorEvidence("split_anchor_6m")
    for ev in GROWS:
        a.note_pool_event(ev)
    a.first_tick, a.first_ms = 1, 1
    a.fault_baseline = list(hb["anchor_load_faults"])
    a.last_total = hb["anchor_frame_total"] - len(hb["anchor_frame_us"])
    a.note(hb)
    assert set(a.errors) == {SPILL, CARVE,
                             "column slab hand-back refused by the heap (shrink_fail)"}, a.errors
    print("PASS simultaneous: overflow + shrink_fail + lost, and spill + carve + "
          "shrink_fail on one real heartbeat, all reported")


def test_identity_and_reclaim_headroom():
    b = base()
    # Sol's repro: 8 slabs, 224 used, one slab empty: zero headroom -> NOT
    # reclaimable (the old summary claimed 2,638,080 B).
    a = pool(GROWS + [cur(b, seq=4, cps=[32] * 7 + [0])])
    o = a.pool_summary()["occupancy"]
    assert o["empty_nonboot_slabs_max"] == 1 and o["reclaimable_slabs_max"] == 0
    assert o["reclaimable_bytes_max"] == 0
    # 208 used: 224 - 208 = 16 -> exactly reclaimable (boundary).
    a = pool(GROWS + [cur(b, seq=4, cps=[32] * 6 + [16, 0])])
    assert a.pool_summary()["occupancy"]["reclaimable_slabs_max"] == 1
    a = pool(GROWS + [cur(b, seq=4, cps=[32] * 6 + [17, 0])])
    assert a.pool_summary()["occupancy"]["reclaimable_slabs_max"] == 0
    # Stable identity: hand back index 5 (carve id 5), regrow (id 8).
    a = pool(GROWS + [cur(b, seq=4, cps=[32] * 4 + [10, 0, 7, 9]),
                      shrink(5, 8, slab=5),
                      cur(b, slabs=7, seq=5, shrinks=1, cps=[32] * 4 + [11, 6, 8]),
                      grow(6, 7),
                      cur(b, slabs=8, seq=6, shrinks=1, cps=[32] * 4 + [12, 5, 7, 3])])
    assert a.errors == [], a.errors
    mins = a.pool_summary()["occupancy"]["slab_min_live"]
    assert mins == {"0": 32, "1": 32, "2": 32, "3": 32, "4": 10, "5": 0,
                    "6": 5, "7": 7, "8": 3}, mins
    assert a.pool_summary()["occupancy"]["slab_identity"].startswith("carve order")
    print("PASS reclaim: 16-slot headroom applied (224 used -> 0, 208 -> 1, "
          "209 -> 0); per-slab minima follow carve identity across hand-back/regrow")


def test_dl_owner_census():
    """R4 point 7: "dlo" is parsed, the remainder computed, guard bytes
    derived, and the sample at the measured carve minimum kept."""
    b = base()
    dlo = [6000000, 2000, 3000000, 1000, 400000, 150, 200000, 80,
           1500000, 2500000, 5600000, 1200]
    h1 = cur(b, seq=4, dlb=9905792, dll=3300, mem2_carve_free_kb=4000, dlo=list(dlo))
    dlo2 = list(dlo)
    dlo2[0] += 1000000
    h2 = cur(b, seq=4, dlb=11905792, dll=4527, mem2_carve_free_kb=893, dlo=dlo2)
    a = pool(GROWS + [h1, h2])
    assert a.errors == [], a.errors
    d = a.pool_summary()["dl_owners"]
    m = d["at_measured_carve_min"]
    assert m["carve_free_kb"] == 893 and m["view0_bytes"] == 7000000
    assert m["remainder_bytes"] == 11905792 - (7000000 + 3000000 + 400000 + 200000)
    assert m["guard_bytes"] == 1200 * 256
    assert d["at_dlb_max"]["dlb"] == 11905792 and d["samples"] == 2
    for bad in ([1] * 11, [1] * 11 + [-1], "x", [1] * 11 + [2 ** 32]):
        a = pool(GROWS + [cur(b, seq=4, dlo=bad)])
        assert a.errors == ["malformed display-list owner census"], (bad, a.errors)
    print("PASS dl owner census: remainder, guard bytes, carve-min sample; "
          "malformed rejected")


def test_c_formatted_shrink_event():
    line = ('{"event":"column_pool_shrink","seq":5,"slab":7,'
            '"released_bytes":2638080,"slabs_before":8,"slabs_after":7}')
    a = pool(GROWS + [json.loads(line)])
    s = a.pool_summary()
    assert a.errors == [] and s["shrinks"] == 1 and s["slabs_reconstructed"] == 7
    assert s["slabs_high_water"] == 8 and s["shrunk_bytes"] == SLAB
    print("PASS C-formatted column_pool_shrink line chains after 4 grows: 8 -> 7")


def test_malformed_events():
    bad = "malformed column pool event"
    cases = [
        grow(0, 4), grow(1, 4, slabs_after=4), grow(1, 4, success=False, slabs_after=5),
        grow(1, 4, requested_bytes=0), grow(1, 4, before_valid="yes"),
        grow(1, 4, before_valid=False), grow(1, 4, largest_allocatable_bytes_before=3 * 10 ** 7),
        grow(1, 10, slabs_after=11), {**grow(1, 4), "seq": "1"}, {**grow(1, 4), "success": None},
        dict(event="column_pool_grow", seq=1),
        shrink(1, 5, slab=3), shrink(1, 4), shrink(1, 6, slab=6),
        shrink(1, 6, released_bytes=0), shrink(1, 6, slabs_after=4),
    ]
    for ev in cases:
        a = pool([ev], finish=False)
        assert a.errors == [bad], (ev, a.errors)
        assert a.pool_summary()["events"] == 0
    print("PASS events: %d malformed shapes rejected" % len(cases))


def test_routed_on_every_anchor_profile():
    for profile in ("split_anchor_6m", "split_play_8m", "split_play_2m"):
        c = feed([json.dumps(dict(grow(1, 4), run_id="r")) + "\n",
                  json.dumps(dict(grow(2, 5), run_id="r")) + "\n"],
                 profile=profile, historical=False)
        assert c.anchor.pool_summary()["slabs_reconstructed"] == 6, profile
    print("PASS events consumed on split_anchor_6m / split_play_8m / split_play_2m")


def test_error_accumulation_is_bounded():
    a = collector.AnchorEvidence("split_anchor_6m")
    for i in range(10000):
        a.error = "violation %d" % i
        a.error = "violation 0"  # a retained repeat never re-counts
    assert len(a.errors) == collector.ANCHOR_MAX_ERRORS
    assert a.errors_overflow == 10000 - collector.ANCHOR_MAX_ERRORS
    assert not hasattr(a, "_errors_dropped")
    assert a.error.endswith("(+9968 further violation reports beyond the 32 "
                            "retained; repeats not distinguished)")
    print("PASS accumulation: 32 retained, 9968 counted, nothing else kept")


# ------------------------------------------- R5 (Sol R4 Major 1) rejections
SEQ0 = "sequence-zero column pool state is not the %d boot slabs (%d slabs, %d hand-backs, no events)"


def test_sequence_zero_is_the_boot_slabs():
    b = base()
    four = [32, 32, 20, 0]
    # The honest boot state: 4 slabs at seq 0 on a Wii build, 0 on PC.
    assert pool([cur(b, slabs=4, cps=four, seq=0)]).errors == []
    pc = dict(b, **collector.PC_POOL_EVIDENCE)
    assert pool([pc]).errors == []
    # Sol's repro: eight slabs at seq 0 with no grow event -- rejected, and
    # never adopted as a baseline the later events then chain from.
    a = pool([cur(b, slabs=8, seq=0)] + GROWS + [cur(b, seq=4)])
    assert a.errors[0] == SEQ0 % (4, 8, 0), a.errors
    a = pool([cur(b, slabs=8, seq=0)])
    assert a.errors == [SEQ0 % (4, 8, 0)], a.errors
    # PC build (budget_wii false) claiming a pool, or seq 0 claiming a
    # hand-back, or a Wii pool below its boot slabs at any seq.
    assert pool([cur(b, slabs=4, cps=four, seq=0, budget_wii=False)]).errors[0] == SEQ0 % (0, 4, 0)
    assert pool([cur(b, slabs=4, cps=four, seq=0, shrinks=1)]).errors[0] == SEQ0 % (4, 4, 1)
    assert SEQ0 % (4, 4, 0) in pool([cur(b, slabs=4, cps=four, seq=0, shrink_fail=1)]).errors
    assert pool([grow(1, 4), cur(b, slabs=3, cps=[32, 32, 1], seq=1)]).errors[0] == \
        "column pool below its 4 boot slabs"
    # An event chain that starts above the boot slabs.
    a = pool([grow(1, 8, slabs_after=9)], finish=False)
    assert a.errors[0] == ("column pool event chain does not start from the 4 boot "
                           "slabs (first slabs_before 8)"), a.errors
    # Same for a historical tape (the boot count is fixed across builds).
    a = pool([grow(1, 6)], historical=True)
    assert a.errors[0].startswith("column pool event chain does not start"), a.errors
    print("PASS seq 0: exactly 4 boot slabs (0 on PC), no hand-back; 8-at-seq-0 "
          "and a chain starting at 8 rejected; never below boot")


def full_rows(path):
    return [json.loads(l) for l in lines_of(path) if l.strip()]


def grade(rows, profile=None):
    """Full ingest -> run end -> write_result, exactly the artifact path."""
    lines = [json.dumps(r) + "\n" for r in rows]
    profile = profile or next(r["profile"] for r in rows
                              if r.get("event") == "heartbeat" and r.get("profile"))
    c = feed(lines, profile=profile)
    if not c.done:
        c._finish_normal("offline replay")
    c.write_result()
    out = Path(c.args.out)
    files = {n for n in ("PASS.json", "FAIL.json", "NONCERTIFYING.json")
             if (out / n).is_file()}
    return c, files


RETAINED = {
    "cc_f1": ROOT / "tmp/wii_runs/cc_f1_s12345/telemetry.ndjson",
    "cc_f2": ROOT / "tmp/wii_runs/cc_f2_s12345/telemetry.ndjson",
    "cc_f3": ROOT / "tmp/wii_runs/cc_f3_s12345/telemetry.ndjson",
    "cc_c1": FULL_C1,
    "cc_m1": FULL_M1,
}


def test_retained_tapes_and_mutations():
    """Replay EVERY retained hardware tape through the artifact path: f1/f2/f3
    still certify (PASS.json), c1/m1 still fail. Then mutate the certifying
    f1 tape each way Sol named: none may yield PASS.json."""
    want = {"cc_f1": "PASS.json", "cc_f2": "PASS.json", "cc_f3": "PASS.json",
            "cc_c1": "FAIL.json", "cc_m1": "FAIL.json"}
    for name, path in RETAINED.items():
        if not path.is_file():
            print("SKIP retained %s: %s not present" % (name, path))
            continue
        c, files = grade(full_rows(path))
        assert files == {want[name]}, (name, files, c.fail_reason)
        print("PASS retained %s -> %s (%s)" % (name, want[name],
                                              c.fail_reason or c.pass_reason))
    if not RETAINED["cc_f1"].is_file():
        print("SKIP f1 mutations: tape not present")
        return
    rows = full_rows(RETAINED["cc_f1"])
    hb_idx = [i for i, r in enumerate(rows) if r.get("event") == "heartbeat" and "cpv" in r]
    ev_idx = [i for i, r in enumerate(rows) if r.get("event") in collector.COLUMN_POOL_EVENTS]

    def mutated(fn):
        m = copy.deepcopy(rows)
        fn(m)
        return m

    def seq0_eight(m):
        for i in hb_idx:
            if m[i]["cpe"][0] == 0:
                m[i]["column_pool_slabs"] = 8
                m[i]["cps"] = m[i]["cps"] + [0] * 4
                m[i]["column_pool_peak"] = max(m[i]["column_pool_peak"], m[i]["column_pool_used"])
        for i in reversed(ev_idx):
            if m[i]["event"] == "column_pool_grow":
                del m[i]

    def drop_cpv_all(m):
        for i in hb_idx:
            m[i].pop("cpv")

    def drop_cpv_one(m):
        m[hb_idx[len(hb_idx) // 2]].pop("cpv")

    def budget_invalid(m):
        m[hb_idx[len(hb_idx) // 2]]["budget_valid"] = False

    def reorder(m):
        a, b = ev_idx[0], ev_idx[1]
        m[a], m[b] = m[b], m[a]

    def drop_event(m):
        del m[ev_idx[1]]

    cases = (("8 slabs at seq 0, no grow events", seq0_eight, "FAIL.json",
              "sequence-zero column pool state"),
             ("cpv removed from every heartbeat (historical)", drop_cpv_all,
              "NONCERTIFYING.json", None),
             ("cpv removed from one heartbeat", drop_cpv_one, "FAIL.json",
              "column pool evidence disappeared mid-run"),
             ("budget_valid=false on one heartbeat", budget_invalid, "FAIL.json",
              "transfer budget snapshot was not coherent"),
             ("first two pool events reordered", reorder, "FAIL.json",
              "column pool event"),
             ("one grow event lost", drop_event, "FAIL.json", "column pool event"))
    for label, fn, want_file, want_msg in cases:
        c, files = grade(mutated(fn), profile="split_anchor_6m")
        assert files == {want_file}, (label, files, c.fail_reason, c.anchor.errors)
        if want_msg:
            assert any(want_msg in e for e in c.anchor.errors), (label, c.anchor.errors)
        print("PASS f1 mutation [%s] -> %s" % (label, want_file))


if __name__ == "__main__":
    test_fixtures_are_verbatim_hardware()
    test_cc_c1_historical_both_violations()
    test_cc_m1_historical_occupancy()
    test_full_tapes_if_present()
    test_live_requires_current_evidence()
    test_reconcile_and_overtaking()
    test_shrink_fail_lost_and_simultaneous()
    test_identity_and_reclaim_headroom()
    test_dl_owner_census()
    test_c_formatted_shrink_event()
    test_malformed_events()
    test_routed_on_every_anchor_profile()
    test_error_accumulation_is_bounded()
    test_sequence_zero_is_the_boot_slabs()
    test_retained_tapes_and_mutations()
    print("PASS test_anchor_pool_collector")
