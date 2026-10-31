"""split_dims_6m collector verdicts (lane B, splitdims-profile-0926).

The device publishes what each split view shows (main.c split_dims_fill:
dim_v0 / dim_v1 / views_live / dims_active / dims_phase). The collector's
SplitDimsEvidence certifies a run only when the two views really showed two
dimensions -- host view in the Overworld, guest view in the Nether -- for a
sustained part of the measured window, with the guest walking there, both
views live, same-dimension separation inside the 256-block envelope, no
missing heartbeat and no allocation failure. A run whose views never
diverged (every run until guest portal travel lands) is dims_not_active and
can never PASS.

Host switch first (splitdims-hostswitch-0927): the host goes to the Nether
and back (two A2-T COMMITs), each view bound to its own world slot while the
views differ, the vacated slot drains to 0 columns and a release is applied
only after that drain; then the guest crosses.

Heartbeats here are synthetic, shaped like the C formatter's output (field
names pinned by test_b1_guest_eye_heartbeat's split_dims block).
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "dims_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)

RUN = "dims_run"


def hb(tick, v1=0, live=2, phase="seek", measuring=True, gx=0.5, gz=0.5,
       v0=0, s1=None, **extra):
    active = live == 2 and v1 != -128 and v1 != v0
    row = dict(event="heartbeat", run_id=RUN, profile="split_dims_6m",
               tick=tick, guest=live == 2, gy=65.62, gx=gx, gz=gz,
               x=0.5, z=0.5, dim_v0=v0, dim_v1=v1, views_live=live,
               dims_active=active, dims_phase=phase, srv_dim_host=v0,
               srv_dim_guest=(v1 if s1 is None else s1),
               anchor_measuring=measuring, world_loaded=True, oom=0,
               tel_fail=0, tel_wto=0, tel_hb_repl=0, tel_drop=0, tel_abd=0,
               # host switch done: out and back, drained, released
               host_phase="done", host_commits=2, slot1_cols=0, rel_sent=1,
               rel_rx=1, rel_slot=1,
               budget_valid=True, budget_wii=True, budget_phase=2, budget_resident=162,
               budget_live_backings=162, budget_reserved=0, budget_prepeak=0,
               budget_peak=162, budget_waits=0, transfer_request=0,
               guest_commits=int(v1 == -1 and live == 2), transfer_actor=255,
               transfer_state=0, transfer_held=False, transfer_source_draining=False,
               transfer_restoring=False, column_pool_used=162, column_pool_peak=162,
               column_pool_slabs=8, column_pool_overflow=0, column_pool_carve_fail=0,
               mem2_carve_free_kb=4096,
               wslot_v0=(1 if v0 == -1 else 0),
               wslot_v1=(-1 if live != 2 else 1 if v1 == -1 else 0))
    row.update(extra)
    return row


HOST_TRIP_END = 78  # first heartbeat after the host switch
GUEST_ACROSS = 94   # first heartbeat with the guest view in the Nether


def host_trip():
    """Heartbeats 0-77: the guest clears the host frame (0-4), the host frame
    lights (5-9), the host is in the Nether on world slot 1 for 65 heartbeats
    (10-74) while the guest stays home on slot 0, it comes back (75-77) and
    the vacated slot drains 40 -> 10 -> 0 with the release applied on 77."""
    pre = dict(host_commits=0, slot1_cols=-1, rel_sent=0, rel_rx=0, rel_slot=-1)
    rows = [hb(t, phase="clear", gz=0.5 + t * 0.4, host_phase="wait", **pre)
            for t in range(5)]
    rows += [hb(t, phase="host_trip", gz=2.5, host_phase="wait", **pre)
             for t in range(5, 10)]
    away = dict(pre, host_commits=1, slot1_cols=81)
    rows += [hb(t, v0=-1, phase="host_trip", gz=2.5, host_phase="away", **away)
             for t in range(10, 75)]
    for t, cols in zip(range(75, 78), (40, 10, 0)):
        rel = dict(rel_sent=1, rel_rx=1, rel_slot=1) if cols == 0 else {}
        rows.append(hb(t, phase="host_trip", gz=2.5, host_phase="back",
                       **dict(pre, host_commits=2, slot1_cols=cols, **rel)))
    return rows


def active_run():
    """360 measured heartbeats: the host switch (0-77), then 10 s walking to
    the pane, 6 s in it (the last two with the guest view re-admitting), then
    the guest in the Nether walking 0.5 block per heartbeat."""
    rows = host_trip()
    rows += [hb(t, gz=2.5 + (t - HOST_TRIP_END) * 0.3)
             for t in range(HOST_TRIP_END, HOST_TRIP_END + 10)]
    rows += [hb(t, phase="hold", gz=5.5) for t in range(88, 92)]
    rows += [hb(t, phase="hold", live=1, v1=-128, gz=5.5) for t in (92, 93)]
    rows += [hb(t, v1=-1, phase="across", gx=0.7, slot1_cols=60,
                gz=0.7 + (t - GUEST_ACROSS) * 0.5)
             for t in range(GUEST_ACROSS, 360)]
    return rows


def not_active_run():
    """The run today: the guest reaches the pane, holds 30 s, nothing crosses,
    it walks on in the Overworld (well inside 256 blocks)."""
    rows = host_trip()
    rows += [hb(t, gz=0.5 + t * 0.1) for t in range(78, 88)]
    rows += [hb(t, phase="hold", gz=5.5) for t in range(88, 118)]
    rows += [hb(t, phase="timeout", gz=5.5 + (t - 118) * 0.2)
             for t in range(118, 360)]
    return rows


def evidence(rows):
    e = collector.SplitDimsEvidence()
    for r in rows:
        e.note(r)
    return e


def test_active_run_passes():
    e = evidence(active_run())
    assert e.verdict() is None, e.verdict()
    s = e.summary()
    assert s["state"] == "dims_active" and s["failure_class"] is None
    assert s["active_measured"] == 360 - GUEST_ACROSS
    assert s["first_active_tick"] == GUEST_ACROSS
    assert s["host_commits"] == 2 and s["host_back_tick"] == 75
    assert s["drain_tick"] == 77 and s["release_tick"] == 77
    assert s["nether_walk_blocks"] >= 16
    print("ok   host Nether-and-back, drain, release, then a sustained "
          "host-Overworld / guest-Nether run PASSes")


def test_not_active_is_fail():
    e = evidence(not_active_run())
    msg, cls = e.verdict()
    assert cls == "dims_not_active", (msg, cls)
    assert "timeout" in msg
    assert e.summary()["state"] == "dims_not_active"
    # Even a perfect run otherwise (never left seek) is dims_not_active.
    msg, cls = evidence([hb(t) for t in range(360)]).verdict()
    assert cls == "dims_not_active"
    print("ok   views that never diverged = dims_not_active (FAIL)")


def test_rejections():
    rows = active_run()
    bad = [dict(r) for r in rows]
    del bad[100]["dims_phase"]
    assert evidence(bad).verdict()[1] == "split_dims_fields_missing"
    bad = [dict(r) for r in rows]
    bad[100]["dims_active"] = False  # claims no divergence it shows
    assert evidence(bad).verdict()[1] == "split_dims_inconsistent"
    bad = [dict(r) for r in rows]
    bad[3]["dims_active"] = True  # claims divergence it does not show
    assert evidence(bad).verdict()[1] == "split_dims_inconsistent"
    bad = [dict(r) for r in rows]
    bad[200].update(dim_v0=-1, dim_v1=0)  # host view in the Nether
    assert evidence(bad).verdict()[1] == "split_dims_host_left_overworld"
    bad = [dict(r) for r in rows]
    bad[200].update(views_live=1, dim_v1=-128, dims_active=False)
    assert evidence(bad).verdict()[1] == "split_dims_view_lost"
    bad = [dict(r) for r in rows]
    bad[-1].update(dim_v1=0, srv_dim_guest=0, dims_active=False)  # bounced back
    assert evidence(bad).verdict()[1] == "split_dims_not_sustained"
    short = active_run()[:GUEST_ACROSS] + [hb(t, v1=-1, phase="across",
                                              gz=t * 0.5)
                                           for t in range(GUEST_ACROSS, 120)]
    short += [hb(t, v1=0, phase="across", gz=20.0) for t in range(120, 360)]
    assert evidence(short).verdict()[1] == "split_dims_active_too_short"
    still = [dict(r) for r in rows]
    for r in still[GUEST_ACROSS:]:
        r.update(gx=0.7, gz=0.7)
    assert evidence(still).verdict()[1] == "split_dims_guest_not_walking"
    far = [dict(r) for r in rows]
    far[5]["gz"] = 300.0  # same dimension, 299.5 blocks apart
    e = evidence(far)
    assert e.verdict() is None  # recorded, never graded (free separation)
    assert "outside the 256-block" in e.summary()["separation_note"]
    assert evidence(rows).summary()["separation_note"] is None
    # Fable F1: a view on a Nether payload while the body is simulated in the
    # Overworld is not divergence.
    lie = [dict(r) for r in rows]
    for r in lie[GUEST_ACROSS:]:
        r["srv_dim_guest"] = 0
    assert evidence(lie).verdict()[1] == "split_dims_view_body_disagree"
    host_lie = [dict(r) for r in rows]
    host_lie[100]["srv_dim_host"] = -1
    assert evidence(host_lie).verdict()[1] == "split_dims_view_body_disagree"
    # During the crossing the server flips before the view: tolerated.
    crossing = [dict(r) for r in rows]
    for r in crossing[88:92]:
        r["srv_dim_guest"] = -1
    assert evidence(crossing).verdict() is None
    no_srv = [dict(r) for r in rows]
    del no_srv[30]["srv_dim_guest"]
    assert evidence(no_srv).verdict()[1] == "split_dims_fields_missing"
    # Cross-dimension rows are independent worlds: not separation-graded.
    across = [dict(r) for r in rows]
    across[300]["gz"] = 5000.0
    assert evidence(across).verdict() is None
    # Heartbeats outside the measured window are structure-checked only.
    loading = [hb(t, live=1, v1=-128, measuring=False) for t in range(5)]
    assert evidence(loading + rows).verdict() is None
    print("ok   missing/inconsistent fields, host leaves, view lost, bounce, "
          "short, still guest, view/body disagreement FAIL by class; "
          "separation is a note")


def test_host_switch_rejections():
    rows = active_run()
    # no host switch at all: the guest still crosses, the host never left
    none = [dict(r) for r in rows]
    for r in none:
        if r["tick"] < HOST_TRIP_END:
            r.update(dim_v0=0, srv_dim_host=0, dims_active=False, wslot_v0=0)
        r.update(host_commits=0, rel_sent=0, rel_rx=0, rel_slot=-1,
                 host_phase=("timeout" if r["tick"] >= 5 else "wait"))
    assert evidence(none).verdict()[1] == "split_dims_host_commits_short"
    # one commit only (the host is still away at the end of the trip)
    one = [dict(r) for r in rows]
    for r in one:
        r.update(host_commits=min(r["host_commits"], 1), rel_sent=0, rel_rx=0,
                 rel_slot=-1)
    assert evidence(one).verdict()[1] == "split_dims_host_commits_short"
    # the vacated world never drains (and so is never released)
    stuck = [dict(r) for r in rows]
    for r in stuck[75:]:
        r.update(slot1_cols=max(r["slot1_cols"], 40), rel_sent=0, rel_rx=0,
                 rel_slot=-1)
    assert evidence(stuck).verdict()[1] == "split_dims_no_drain"
    # drained, but the release never arrives
    silent = [dict(r) for r in rows]
    for r in silent[75:]:
        r.update(rel_rx=0, rel_slot=-1)
    assert evidence(silent).verdict()[1] == "split_dims_release_missing"
    # a release applied while the world still held columns
    early = [dict(r) for r in rows]
    early[75].update(rel_sent=1, rel_rx=1, rel_slot=1)
    early[76].update(rel_sent=1, rel_rx=1, rel_slot=1)
    assert evidence(early).verdict()[1] == "split_dims_release_before_drain"
    # while the views differ, both render the same world slot
    shared = [dict(r) for r in rows]
    shared[40]["wslot_v0"] = 0
    assert evidence(shared).verdict()[1] == "split_dims_views_share_world"
    shared = [dict(r) for r in rows]
    shared[200]["wslot_v1"] = 0
    assert evidence(shared).verdict()[1] == "split_dims_views_share_world"
    unbound = [dict(r) for r in rows]
    unbound[40]["wslot_v0"] = -1
    assert evidence(unbound).verdict()[1] == "split_dims_views_share_world"
    # the server leads the host view briefly across a transfer: tolerated
    lead = [dict(r) for r in rows]
    for r in lead[8:10]:
        r["srv_dim_host"] = -1
    assert evidence(lead).verdict() is None
    # ... but not for longer than the transit allowance
    lag = [dict(r) for r in rows]
    for r in lag[10:17]:
        r.update(dim_v0=0, dims_active=False, wslot_v0=0)
        r["srv_dim_host"] = -1
    assert evidence(lag).verdict()[1] == "split_dims_view_body_disagree"
    # the host view in the Nether once the trip is over
    late = [dict(r) for r in rows]
    late[80].update(dim_v0=-1, srv_dim_host=-1, dims_active=True, wslot_v0=1)
    assert evidence(late).verdict()[1] == "split_dims_host_left_overworld"
    # missing / mistyped host fields, releases the server never sent
    bad = [dict(r) for r in rows]
    del bad[20]["slot1_cols"]
    assert evidence(bad).verdict()[1] == "split_dims_fields_missing"
    bad = [dict(r) for r in rows]
    bad[20]["host_phase"] = "sideways"
    assert evidence(bad).verdict()[1] == "split_dims_fields_missing"
    bad = [dict(r) for r in rows]
    bad[200]["rel_rx"] = 2
    assert evidence(bad).verdict()[1] == "split_dims_inconsistent"
    print("ok   host switch: no/one commit, no drain, no release, early "
          "release, shared/unbound view world, long host lag, host away "
          "after the trip, bad host fields FAIL by class")


def test_unbound_guest_same_dimension():
    # The recovered G8 build rendered a live guest through the host fallback
    # until the first host trip exposed the wrong dimension. Reject the
    # missing binding even while both actors still share the Overworld.
    rows = active_run()
    rows[2]["wslot_v1"] = -1
    assert evidence(rows).verdict()[1] == "split_dims_guest_unbound"
    rows = not_active_run()
    rows[2]["wslot_v1"] = -1
    assert evidence(rows).verdict()[1] == "split_dims_guest_unbound"


def run_collector(name, rows, require_wii=False):
    td = tempfile.mkdtemp(prefix=f"dims_{name}_")
    args = collector._selftest_args(Path(td), RUN, "split_dims_6m")
    args.require_wii_budget = require_wii
    c = collector.Collector(args)
    with contextlib.redirect_stdout(io.StringIO()):  # per-heartbeat log lines
        for r in rows:
            c.process(r)
            if c.done:
                break
        if not c.done:
            c.process(dict(event="profile_done", run_id=RUN, status="ok"))
    return c, Path(td)


def test_transfer_budget():
    for changes, reason in (
        ({"budget_live_backings": 255, "budget_reserved": 2, "budget_peak": 257},
         "split_dims_budget_exceeded"),
        ({"budget_peak": 257}, "split_dims_budget_exceeded"),
        ({"budget_valid": False}, "split_dims_budget_invalid"),
        ({"budget_reserved": True}, "split_dims_budget_missing"),
        ({"budget_phase": 1}, "split_dims_budget_invalid"),
        ({"budget_wii": False}, "split_dims_budget_invalid"),
        ({"column_pool_slabs": 0}, "split_dims_budget_missing"),
        ({"column_pool_peak": 257}, "split_dims_column_pool_exceeded"),
        ({"column_pool_slabs": 9}, "split_dims_column_pool_exceeded"),
        ({"column_pool_overflow": 1}, "split_dims_column_pool_exceeded"),
        ({"column_pool_carve_fail": 1}, "split_dims_column_pool_exceeded"),
        ({"column_pool_slabs": 8, "mem2_carve_free_kb": 2245},
         "split_dims_budget_headroom"),
    ):
        rows = active_run()
        rows[100].update(changes)
        assert evidence(rows).verdict()[1] == reason, (changes, evidence(rows).verdict())
    rows = active_run()
    del rows[100]["budget_peak"]
    assert evidence(rows).verdict()[1] == "split_dims_budget_missing"
    rows = active_run()
    for row in rows:
        row["guest_commits"] = 0
    assert evidence(rows).verdict()[1] == "split_dims_guest_commit_missing"
    # Preserve an oversized legacy transition separately; it must never reset
    # or excuse a peak after the explicit enforcement boundary.
    rows = active_run()
    for row in rows[:5]:
        row.update(budget_phase=1, budget_live_backings=489,
                   budget_resident=330, budget_prepeak=489, budget_peak=0)
    for row in rows[5:]:
        row.update(budget_prepeak=489, budget_peak=256,
                   column_pool_used=162, column_pool_peak=256, column_pool_slabs=8,
                   mem2_carve_free_kb=2246)
    assert evidence(rows).verdict() is None
    hidden = [dict(row) for row in rows]
    hidden[2]["budget_prepeak"] = 0
    assert evidence(hidden).verdict()[1] == "split_dims_budget_invalid"
    reset = [dict(row) for row in rows]
    reset[100]["budget_prepeak"] = 0
    assert evidence(reset).verdict()[1] == "split_dims_budget_invalid"
    rows[100]["budget_peak"] = 162
    assert evidence(rows).verdict()[1] == "split_dims_budget_invalid"
    pc = active_run()
    for row in pc:
        row.update(budget_wii=False, column_pool_used=0, column_pool_peak=0,
                   column_pool_slabs=0, mem2_carve_free_kb=-1)
    assert evidence(pc).verdict() is None
    assert evidence(pc).summary()["hardware_budget"] == "not_graded"
    c, _ = run_collector("pc_not_wii", pc, require_wii=True)
    assert c.failure_class == "split_dims_budget_platform"
    c, _ = run_collector("wii_required", active_run(), require_wii=True)
    assert not c.failure_class.startswith("split_dims"), c.failure_class
    print("ok   coherent budget, retained/in-flight cap, peak, activation and physical pool gates")


def _boundary(changes, row=100):
    rows = active_run()
    rows[row].update(changes)
    return evidence(rows)


def test_256_column_ceiling_boundaries():
    # exact boundaries: 256 columns / 8 slabs / 2246 KiB pass, one past fails
    ok = {"budget_live_backings": 256, "budget_peak": 256, "column_pool_peak": 256,
          "column_pool_slabs": 8, "mem2_carve_free_kb": 2246}
    rows = active_run()
    for r in rows[5:]:
        r.update(ok)
    assert evidence(rows).verdict() is None, evidence(rows).verdict()
    assert _boundary({"budget_live_backings": 257, "budget_peak": 257}).verdict()[1] \
        == "split_dims_budget_exceeded"
    assert _boundary({"column_pool_peak": 256}).verdict() is None
    assert _boundary({"column_pool_peak": 257}).verdict()[1] == "split_dims_column_pool_exceeded"
    assert _boundary({"column_pool_slabs": 8}).verdict() is None
    assert _boundary({"column_pool_slabs": 9}).verdict()[1] == "split_dims_column_pool_exceeded"
    assert _boundary({"mem2_carve_free_kb": 2246}).verdict() is None
    assert _boundary({"mem2_carve_free_kb": 2245}).verdict()[1] == "split_dims_budget_headroom"
    assert collector.SPLIT_CARVE_FLOOR_KB == 2246
    print("ok   256-column / 8-slab / 2246 KiB boundaries")


def test_contiguity_and_grow_events():
    # missing new fields: unavailable, never 0, never a failure
    e = evidence(active_run())
    assert e.verdict() is None
    s = e.summary()
    assert s["carve_largest_allocatable_min_bytes"] == "unavailable", s
    assert s["column_pool_grow_events"] == 0
    rows = active_run()
    for i, r in enumerate(rows):
        r.update(mem2_carve_largest_valid=True,
                 mem2_carve_largest_allocatable_bytes=3000000 + i)
    assert evidence(rows).summary()["carve_largest_allocatable_min_bytes"] == 3000000
    rows = active_run()
    for r in rows:
        r.update(mem2_carve_largest_valid=False,
                 mem2_carve_largest_allocatable_bytes=0)
    assert evidence(rows).summary()["carve_largest_allocatable_min_bytes"] == "unavailable"
    e = evidence(active_run())
    e.note_pool_grow(dict(event="column_pool_grow", success=True))
    assert e.pool_grow_count == 1 and e.verdict() is None
    e.note_pool_grow(dict(event="column_pool_grow", success=False, slab=9))
    v = e.verdict()
    assert v[1] == "split_dims_column_pool_grow_failed" and '"slab": 9' in v[0], v
    td = tempfile.mkdtemp()
    args = collector._selftest_args(Path(td), RUN, "split_dims_6m")
    c = collector.Collector(args)
    with contextlib.redirect_stdout(io.StringIO()):
        c.process(dict(event="column_pool_grow", run_id=RUN, success=False))
    assert c.done and c.failure_class == "split_dims_column_pool_grow_failed", c.failure_class
    print("ok   contiguity telemetry unavailable-not-zero, grow event gate")


def test_collector_wiring():
    c, out = run_collector("inactive", not_active_run())
    assert c.done and c.failure_class == "dims_not_active", (c.failure_class,
                                                             c.fail_reason)
    with contextlib.redirect_stdout(io.StringIO()):
        assert c.write_result() == 1
    verdict = json.loads((out / "FAIL.json").read_text())
    assert verdict["split_dimensions"]["state"] == "dims_not_active"
    assert verdict["split_dimensions"]["last_phase"] == "timeout"
    # Structural lies fail at the heartbeat, not at completion.
    bad = active_run()
    bad[50] = dict(bad[50], dims_active=False)
    c, _ = run_collector("liar", bad)
    assert c.done and c.failure_class == "split_dims_inconsistent"
    # A dims-active run clears every split-dimension gate; what fails it on
    # this synthetic tape is the anchor evidence it does not carry.
    c, out = run_collector("active", active_run())
    assert c.failure_class not in (
        "dims_not_active", "split_dims_heartbeat_missing", "split_dims_oom",
        "too_few_heartbeats"), (c.failure_class, c.fail_reason)
    assert not c.failure_class.startswith("split_dims"), c.failure_class
    assert c.split_dims.summary()["state"] == "dims_active"
    assert c.split_dims_rows[-1]["budget_peak"] == 162
    assert "column_pool_slabs" in c.split_dims_rows[-1]
    print(f"     (synthetic active tape then stops at {c.failure_class})")
    # A missing heartbeat fails a dims-active run -- during the host trip
    # too.
    for gap_tick in (200, 40, 76):
        gappy = [r for r in active_run() if r["tick"] != gap_tick]
        c, _ = run_collector(f"gap{gap_tick}", gappy)
        assert c.failure_class == "split_dims_heartbeat_missing", (
            gap_tick, c.failure_class)
    # So does any allocation failure.
    oom = active_run()
    for r in oom[300:]:
        r["oom"] = 1
    c, _ = run_collector("oom", oom)
    assert c.failure_class == "split_dims_oom", c.failure_class
    print("ok   collector: dims_not_active FAIL.json, heartbeat-time rejection, "
          "active run clears the dims gates, missing heartbeat, oom")


if __name__ == "__main__":
    test_active_run_passes()
    test_not_active_is_fail()
    test_rejections()
    test_host_switch_rejections()
    test_unbound_guest_same_dimension()
    test_transfer_budget()
    test_256_column_ceiling_boundaries()
    test_contiguity_and_grow_events()
    test_collector_wiring()
    print("PASS test_split_dims_collector")
