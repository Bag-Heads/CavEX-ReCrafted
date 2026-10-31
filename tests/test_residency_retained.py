#!/usr/bin/env python3
"""Real Wii heartbeat tape regressions for the residency rule respec.

Fixtures are lossless gzip copies of original telemetry.ndjson, with source
hashes in manifest.json. Mutations are applied to decoded copies and then
serialized back to NDJSON so they pass through the production line parser.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "tests/fixtures/residency"
sys.path.insert(0, str(ROOT / "tools"))
import wii_telemetry_server as wts  # noqa: E402
import regrade_residency_tapes as regrade  # noqa: E402


def tape(name):
    data = gzip.decompress((FIX / f"{name}.ndjson.gz").read_bytes())
    manifest = json.loads((FIX / "manifest.json").read_text(encoding="utf-8"))
    assert hashlib.sha256(data).hexdigest() == manifest[name]["sha256"], name
    return data.decode("utf-8")


def verdict(text, profile, **opts):
    _, records = wts.residency_parse_tape_lines(text)
    return wts.residency_verdict_for_profile(records, profile, **opts)


def mutate(text, fn):
    events = [json.loads(line) for line in text.splitlines()]
    fn(events)
    return "\n".join(json.dumps(e, separators=(",", ":")) for e in events) + "\n"


def heartbeats(events):
    return [e for e in events if e.get("event") == "heartbeat" and "rza" in e]


def assert_verdict(name, text, profile, status, codes=(), **opts):
    v = verdict(text, profile, **opts)
    assert v["status"] == status, (name, v["status"], v["codes"], v["reasons"][:3])
    assert set(v["codes"]) == set(codes), (name, v["codes"], codes, v["reasons"][:3])
    return v


def test_retained_positives():
    manifest = json.loads((FIX / "manifest.json").read_text(encoding="utf-8"))
    for name, profile in (
        ("cc_f1_s12345", "split_anchor_6m"),
        ("cc_c1_s12345", "split_anchor_6m"),
        ("cc_m1_s12345", "split_anchor_6m"),
        ("i07_i04_s12345", "far_walk_6m"),
        ("i07_i05_s12345", "keepup_walk_6m"),
        ("i07_i05b_s12345", "keepup_walk_6m"),
        ("i07_i07_s12345", "split_play_8m"),
    ):
        original = manifest[name]
        assert original["profile"] == profile, (name, original["profile"], profile)
        assert_verdict(name, tape(name), profile, "PASS")
    for name in ("cc_c1_s12345", "cc_m1_s12345"):
        assert manifest[name]["original_overall"] == "FAIL"
        assert manifest[name]["original_failure_class"] == "anchor_feasibility_failed"
    assert manifest["i07_i05_s12345"]["original_failure_class"] == "scenario_not_engaged"
    for name in ("i07_i14_s12345", "i07_i14b_s12345"):
        assert manifest[name]["original_failure_class"] == "perf_server_budget_exceeded"


def test_retained_idle():
    for name in ("i07_i01_s12345", "i07_i02_s12345", "i07_i03_s12345",
                 "i07_i14_s12345", "i07_i14b_s12345"):
        m = json.loads((FIX / "manifest.json").read_text(encoding="utf-8"))[name]
        v = verdict(tape(name), m["profile"])
        assert v["status"] == "PASS", (name, v["status"], v["codes"])
        assert v["required_legs"] == [], (name, v["required_legs"])


def test_retained_missing_movement():
    for name in ("cc_f2_s12345", "i07_i06_s12345", "i07_i06b_s12345",
                 "i07_i08_s12345", "i07_i08b_s12345", "i07_i09_s12345"):
        m = json.loads((FIX / "manifest.json").read_text(encoding="utf-8"))[name]
        v = verdict(tape(name), m["profile"])
        assert v["status"] == "FAIL", (name, v["status"], v["codes"])
        assert "no_moving_segment" in v["codes"], (name, v["codes"])
    # cc_f2's guest covers more than a kilometre in Nether slot 1; the host
    # remains at zero in both required world slots.
    v = verdict(tape("cc_f2_s12345"), "split_dims_6m")
    assert all(s["moved_blocks"] == 0 for s in v["actors"][0]["segments"])
    assert any(s["world_slot"] == 1 and s["moved_blocks"] >= 32
               for s in v["actors"][1]["segments"])


BASE = "cc_f1_s12345"
PROFILE = "split_anchor_6m"


def add_served_demand(events):
    rows = heartbeats(events)
    for e in rows[len(rows)//2:]:
        e["rza"][0][wts.RZA_REQUESTS] += 1
        e["rza"][0][wts.RZA_LOADED] += 1


def test_real_tape_mutations():
    raw = tape(BASE)
    hb = heartbeats([json.loads(line) for line in raw.splitlines()])
    assert len(hb) >= 5

    def apply(name, fn, status, codes):
        assert_verdict(name, mutate(raw, fn), PROFILE, status, codes)

    def missing_mid(es):
        e = heartbeats(es)[len(hb) // 2]
        for k in ("rzv", "rzt", "rza", "rzi", "rzf"):
            e.pop(k, None)

    def missing_final(es):
        e = heartbeats(es)[-1]
        for k in ("rzv", "rzt", "rza", "rzi", "rzf"):
            e.pop(k, None)

    apply("missing middle", missing_mid, "UNPROVEN", ("missing_middle",))
    apply("missing final", missing_final, "UNPROVEN", ("missing_final",))
    def no_evidence(es):
        for e in heartbeats(es):
            for k in ("rzv", "rzt", "rza", "rzi", "rzf"):
                e.pop(k, None)
    apply("no evidence", no_evidence, "UNPROVEN", ("no_evidence",))
    apply("schema", lambda es: heartbeats(es)[len(hb)//2].__setitem__("rzv", 1),
          "FAIL", ("version_mismatch",))
    apply("null", lambda es: heartbeats(es)[len(hb)//2]["rza"][0].__setitem__(wts.RZA_REQUESTS, None),
          "FAIL", ("null_value",))
    apply("overflow", lambda es: heartbeats(es)[len(hb)//2]["rza"][0].__setitem__(wts.RZA_REQUESTS, 2**32),
          "FAIL", ("malformed_rza",))
    apply("reordered", lambda es: heartbeats(es)[len(hb)//2].__setitem__("rzt", 0),
          "FAIL", ("out_of_order",))
    bad = raw.splitlines()
    evidence_lines = [i for i, line in enumerate(bad)
                      if '"event":"heartbeat"' in line and '"rza"' in line]
    mid_line = evidence_lines[len(evidence_lines)//2]
    bad[mid_line] = '{"event":"heartbeat",'
    assert_verdict("malformed line", "\n".join(bad) + "\n", PROFILE,
                   "UNPROVEN", ("unparseable_line",))

    def counter_reset(es):
        rows = heartbeats(es)
        i = len(rows)//2
        for e in rows[:i]:
            e["rza"][0][wts.RZA_REQUESTS] = 1
        for e in rows[i:]:
            e["rza"][0][wts.RZA_REQUESTS] = 0
    apply("reset", counter_reset, "FAIL", ("counter_decrease",))

    def wrong_actor(es):
        for e in heartbeats(es):
            e["rza"][0] = [255] + [0] * 21
    apply("wrong actor", wrong_actor, "FAIL", ("missing_required_actor",))

    def wrong_world(es):
        for e in heartbeats(es):
            e["rza"][0][wts.RZA_WORLD_SLOT] = 1
    assert_verdict("wrong world", mutate(raw, wrong_world), PROFILE, "FAIL",
                   ("missing_required_actor",), require_moving="host:0")

    def demand(es, service):
        rows = heartbeats(es)
        for e in rows[len(rows)//2:]:
            e["rza"][0][wts.RZA_REQUESTS] += 1
            if service:
                e["rza"][0][wts.RZA_LOADED] += 1
    apply("demand without load", lambda es: demand(es, False), "FAIL", ("demand_unserved",))
    apply("demand served", lambda es: demand(es, True), "PASS", ())
    assert_verdict("zero demand", raw, PROFILE, "PASS", ())

    def request_on_identity_open(es):
        for e in heartbeats(es):
            if e["rza"][0][wts.RZA_BODY_EPOCH] == 2:
                e["rza"][0][wts.RZA_REQUESTS] = 1
                e["rza"][0][wts.RZA_LOADED] = 0
    apply("unserved request on epoch opening row", request_on_identity_open,
          "UNPROVEN", ("boundary_activity_unattributed",))

    def pending(es, n):
        heartbeats(es)[len(hb)//2]["rza"][0][wts.RZA_PENDING] = n
    served = mutate(raw, add_served_demand)
    assert_verdict("pending 7", mutate(served, lambda es: pending(es, 7)), PROFILE,
                   "FAIL", ("pending_limit",))
    assert_verdict("pending at end", mutate(served, lambda es: heartbeats(es)[-1]["rza"][0].__setitem__(wts.RZA_PENDING, 1)), PROFILE,
          "FAIL", ("pending_at_completion",))
    assert_verdict("unresolved state", mutate(served, lambda es: heartbeats(es)[-1]["rza"][0].__setitem__(wts.RZA_STATE, 3)), PROFILE,
                   "FAIL", ("unresolved_at_completion",))

    def unresolved_transition(es):
        rows = heartbeats(es)
        last_old = next(rows[i] for i in range(len(rows)-1) if rows[i]["rza"][0][wts.RZA_BODY_EPOCH] == 1
                        and rows[i+1]["rza"][0][wts.RZA_BODY_EPOCH] == 2)
        last_old["rza"][0][wts.RZA_STATE] = 2
        last_old["rza"][0][wts.RZA_STATE_AGE] = 1
    v = verdict(mutate(served, unresolved_transition), PROFILE)
    assert v["status"] == "FAIL" and set(v["codes"]) == {"identity_changed_unresolved", "demand_unserved"}, v["codes"]
    assert_verdict("delay 41", mutate(served, lambda es: heartbeats(es)[len(hb)//2]["rza"][0].__setitem__(wts.RZA_OLDEST_REQ_AGE, 41)), PROFILE,
          "FAIL", ("service_delay_limit",))
    assert_verdict("recovery 201", mutate(served, lambda es: (heartbeats(es)[len(hb)//2]["rza"][0].__setitem__(wts.RZA_STATE, 2),
                                     heartbeats(es)[len(hb)//2]["rza"][0].__setitem__(wts.RZA_STATE_AGE, 201))), PROFILE,
          "FAIL", ("recovery_limit",))
    def failure(es):
        rows = heartbeats(es)
        for e in rows[len(rows)//2:]:
            e["rza"][0][wts.RZA_RECOVERY_FAILURES] += 1
            e["rza"][0][wts.RZA_LAST_ATTEMPT_RESULT] = 2
            e["rza"][0][wts.RZA_ATTEMPT] = 1
    v = verdict(mutate(served, failure), PROFILE)
    assert v["status"] == "FAIL" and set(v["codes"]) == {"uninduced_failure"}, v["codes"]


def test_boundaries():
    raw = mutate(tape(BASE), add_served_demand)
    original = [json.loads(line) for line in raw.splitlines()]
    rows = heartbeats(original)
    assert len(rows) >= 5
    def change(field, value):
        return mutate(raw, lambda es: heartbeats(es)[len(rows)//2]["rza"][0].__setitem__(field, value))
    assert_verdict("pending 6", change(wts.RZA_PENDING, 6), PROFILE, "PASS")
    assert_verdict("delay 40", change(wts.RZA_OLDEST_REQ_AGE, 40), PROFILE, "PASS")
    assert_verdict("delay 41", change(wts.RZA_OLDEST_REQ_AGE, 41), PROFILE, "FAIL", ("service_delay_limit",))
    assert_verdict("pending 7", change(wts.RZA_PENDING, 7), PROFILE, "FAIL", ("pending_limit",))
    def recovery(es, value):
        row = heartbeats(es)[len(rows)//2]["rza"][0]
        row[wts.RZA_STATE], row[wts.RZA_STATE_AGE] = 2, value
    assert_verdict("recovery 200", mutate(raw, lambda es: recovery(es, 200)), PROFILE, "PASS")
    assert_verdict("recovery 201", mutate(raw, lambda es: recovery(es, 201)), PROFILE,
                   "FAIL", ("recovery_limit",))

    def set_movement(es, amount):
        rows = heartbeats(es)
        leg = [e for e in rows if e["rza"][0][wts.RZA_BODY_EPOCH] == 2]
        assert len(leg) >= 2
        start = leg[0]["rza"][0][wts.RZA_MOVED_DM]
        for i, e in enumerate(leg):
            e["rza"][0][wts.RZA_MOVED_DM] = start + amount*i//(len(leg)-1)
    assert_verdict("movement 320", mutate(raw, lambda es: set_movement(es, 320)), PROFILE, "PASS")
    assert_verdict("movement 319", mutate(raw, lambda es: set_movement(es, 319)), PROFILE,
                   "FAIL", ("no_moving_segment",))


def test_derived_overall_precedence():
    cases = (
        ("FAIL", "residency_gate_failed", "FAIL", "FAIL"),
        ("FAIL", "residency_gate_failed", "UNPROVEN", "UNPROVEN"),
        ("FAIL", "residency_gate_failed", "PASS", "UNPROVEN"),
        ("FAIL", "anchor_feasibility_failed", "PASS", "FAIL"),
        ("FAIL", "scenario_not_engaged", "UNPROVEN", "FAIL"),
        ("FAIL", "perf_server_budget_exceeded", "FAIL", "FAIL"),
        ("PASS", None, "PASS", "PASS"),
        ("PASS", None, "FAIL", "FAIL"),
        ("PASS", None, "UNPROVEN", "UNPROVEN"),
    )
    for old, failure_class, residency, expected in cases:
        actual, basis = regrade.derived_overall(old, failure_class, residency)
        assert actual == expected and basis, (old, failure_class, residency, actual, basis)


def test_retained_build_ids():
    manifest = json.loads((FIX / "manifest.json").read_text(encoding="utf-8"))
    for name, item in manifest.items():
        ids = {e.get("build_id") for e in (json.loads(line) for line in tape(name).splitlines())
               if e.get("event") == "boot"}
        assert ids == {item["build_id"]} and next(iter(ids)), (name, ids, item["build_id"])


def test_f7_provenance_and_verdicts():
    manifest = json.loads((FIX / "manifest.json").read_text())
    for tag in ("p1", "p2", "a", "d", "w", "r", "h", "g"):
        name = f"f7_{tag}_s12345"
        original = (FIX / f"{name}.verdict.json").read_bytes()
        assert hashlib.sha256(original).hexdigest() == manifest[name]["original_verdict_sha256"]
        raw = tape(name)
        if tag in ("d", "r"):
            v = verdict(raw, manifest[name]["profile"])
            assert v["status"] == "FAIL" and "no_moving_segment" in v["codes"]
        if tag in ("h", "g", "d", "r"):
            assert manifest[name]["original_overall"] == "FAIL"
    v = assert_verdict("F7 host coalesced", tape("f7_h_s12345"), "split_play_8m",
                       "PASS", allow_induced=True, expect_fault="host@90")
    assert v["induced_exempt"][0]["attempt_ticks"] is None
    assert v["induced_exempt"][0]["terminal_source"] == "legacy_bounded_terminal_phase"
    v = verdict(tape("f7_g_s12345"), "split_play_8m", allow_induced=True,
                expect_fault="guest@90")
    assert v["status"] == "FAIL" and "induced_no_retry" in v["codes"]


def test_f7_coalesced_negatives():
    raw = tape("f7_h_s12345")
    def check(fn, code):
        v = verdict(mutate(raw, fn), "split_play_8m", allow_induced=True,
                    expect_fault="host@90")
        assert v["status"] == "FAIL" and code in v["codes"], v
    for bit in (wts.RZF_TERMINAL, wts.RZF_NOTICE, wts.RZF_DISARMED, wts.RZF_RETRY):
        check(lambda es, bit=bit: [e.__setitem__("rzf", e["rzf"] & ~bit)
              for e in heartbeats(es)], "induced_fault_phases")
    check(lambda es: [e["rzi"][0].__setitem__(3, 999) for e in heartbeats(es)
                       if e.get("rzi")], "uninduced_failure")
    check(lambda es: [e["rza"][0].__setitem__(wts.RZA_RECOVERY_FAILURES, 2)
                       for e in heartbeats(es) if e["rza"][0][wts.RZA_RECOVERY_FAILURES]],
          "uninduced_failure")
    check(lambda es: [e["rza"][0].__setitem__(wts.RZA_RETRIES, 0)
                       for e in heartbeats(es)], "uninduced_failure")
    check(lambda es: [e["rza"][0].__setitem__(wts.RZA_BODY_EPOCH, 3)
                       for e in heartbeats(es) if e["rza"][0][wts.RZA_RECOVERY_FAILURES]],
          "uninduced_failure")


def test_f7_derived_exact_receipts():
    # Derived extension cases, NOT claimed verbatim hardware evidence.
    def receipts(es):
        for e in heartbeats(es):
            e["rze"] = [[0]*8, [0]*8]
            if e["rza"][0][wts.RZA_RECOVERY_FAILURES]:
                e["rze"][0] = [0, 1, 2, 1, 200, 2, 2, 1]
    raw = mutate(tape("f7_h_s12345"), receipts)
    v = assert_verdict("retained exact receipt", raw, "split_play_8m", "PASS",
                       allow_induced=True, expect_fault="host@90")
    assert v["induced_exempt"][0]["attempt_ticks"] == 200
    for fn, code in (
        (lambda es: heartbeats(es)[-1].pop("rze"), "missing_rze"),
        (lambda es: heartbeats(es)[-1]["rze"][0].__setitem__(4, 199), "mutated_rze"),
        (lambda es: heartbeats(es)[-1].__setitem__("rze", [[0], [0]]), "malformed_rze"),
    ):
        v = verdict(mutate(raw, fn), "split_play_8m", allow_induced=True)
        assert v["status"] == "FAIL" and code in v["codes"], v
    for field, value, code in ((2, 99, "uninduced_failure"),
                               (4, 201, "induced_late_terminal"),
                               (5, 3, "induced_receipt_mismatch"),
                               (6, 3, "induced_receipt_mismatch"),
                               (7, 2, "induced_receipt_mismatch")):
        def change(es):
            for e in heartbeats(es):
                if e["rze"][0][3]: e["rze"][0][field] = value
        v = verdict(mutate(raw, change), "split_play_8m", allow_induced=True,
                    expect_fault="host@90")
        assert v["status"] == "FAIL" and code in v["codes"], (field, v)


def test_r7_frozen_failures():
    manifest = json.loads((FIX / "manifest.json").read_text())
    for tag in ("dims", "recovery", "host90", "guest90"):
        name = f"r7_{tag}_s12345"
        original = (FIX / f"{name}.verdict.json").read_bytes()
        assert hashlib.sha256(original).hexdigest() == manifest[name]["original_verdict_sha256"]
        assert len(tape(name).encode()) == manifest[name]["bytes"]
        assert manifest[name]["build_id"] == "0443774_10.07"
    assert_verdict("R7 host", tape("r7_host90_s12345"), "split_play_8m", "PASS",
                   allow_induced=True, expect_fault="host@90")
    assert_verdict("R7 recovery residency", tape("r7_recovery_s12345"),
                   "split_recovery_6m", "PASS")
    rows = [json.loads(l) for l in tape("r7_recovery_s12345").splitlines()]
    # Verbatim rows 249/250/287/480. P2 got its OWN full 60s budget;
    # it stalled after leave/retirement/layout, waiting for the saved Join.
    markers = [e for e in rows if e.get("event") == "split_recovery"]
    base = [e for e in markers if e["marker"] == "baseline"]
    assert abs(base[0]["z"] - base[1]["z"]) > 80
    assert markers[-2]["marker"] == "host_only_layout"
    assert markers[-1]["phase"] == "p2" and markers[-1]["diag"] == "phase timeout"
    assert 60000 <= markers[-1]["ms"] - markers[14]["ms"] < 60100
    dims = tape("r7_dims_s12345")
    v = verdict(dims, "split_dims_6m")
    assert v["status"] == "FAIL" and "no_moving_segment" in v["codes"]
    assert all(s["moved_blocks"] == 0 for s in v["actors"][0]["segments"])
    ds = heartbeats([json.loads(l) for l in dims.splitlines()])
    timeout = next(e for e in ds if e.get("host_phase") == "timeout")
    assert timeout["host_commits"] == 1 and timeout["dim_v0"] == -1
    assert ds[-1]["host_commits"] == 2 and ds[-1]["dims_phase"] == "timeout"
    raw = tape("r7_guest90_s12345")
    v = verdict(raw, "split_play_8m", allow_induced=True, expect_fault="guest@90")
    assert v["status"] == "FAIL" and "induced_no_service" in v["codes"]
    assert "induced_no_retry" not in v["codes"]
    # Verbatim lines 437/440 bind the failed and successful attempts to
    # actor 1, WORLD SLOT 0. This was never a slot-1-world service failure.
    rows = heartbeats([json.loads(l) for l in raw.splitlines()])
    completed = next(e for e in rows if e["rze"][1][6])
    assert completed["rze"][1] == [0, 1, 2, 1, 200, 2, 2, 0]
    assert completed["rza"][1][wts.RZA_LOADED] == 0
    assert rows[-1]["rza"][1][wts.RZA_MOVED_DM] == 8503
    # Derived producer correction, NOT a regrade of the frozen FAIL: credit
    # the real late load proven separately by the native fault regression.
    def late_service(es):
        for e in heartbeats(es):
            if e["rze"][1][6]:
                e["rza"][1][wts.RZA_LOADED] = 1
                e["rza"][1][wts.RZA_SERVICE_DELAY_MAX] = 202
    corrected = mutate(raw, late_service)
    assert_verdict("derived late-load receipt", corrected, "split_play_8m", "PASS",
                   allow_induced=True, expect_fault="guest@90")
    def old_request(es):
        for e in heartbeats(es):
            if e["rza"][1][wts.RZA_BODY_EPOCH] == 2:
                e["rza"][1][wts.RZA_REQUESTS] = 1
    v = verdict(mutate(corrected, old_request), "split_play_8m",
                allow_induced=True, expect_fault="guest@90")
    assert v["status"] == "FAIL" and "induced_no_service" in v["codes"]
    def boundary_entry(es):
        for e in heartbeats(es):
            if e["rza"][1][wts.RZA_ATTEMPT] == 0:
                e["rza"][1][wts.RZA_BODY_EPOCH] = 1
    v = verdict(mutate(corrected, boundary_entry), "split_play_8m",
                allow_induced=True, expect_fault="guest@90")
    assert v["status"] == "FAIL" and "induced_no_service" in v["codes"]
    # The complete induced episode exempts only its own interval. A later
    # ordinary 41-tick refusal still fails the unchanged 40-tick gate.
    def later_refusal(es):
        row = heartbeats(es)[-2]["rza"][1]
        row[wts.RZA_STREAK_AGE] = 41
    v = verdict(mutate(corrected, later_refusal), "split_play_8m",
                allow_induced=True, expect_fault="guest@90")
    assert v["status"] == "FAIL" and "refusal_limit" in v["codes"]
    import test_r7_profile_routes
    test_r7_profile_routes.run()
    test_r7_profile_routes.run(mutate=True)


def test_r8_guest_coalesced_refusal_settlement():
    manifest = json.loads((FIX / "manifest.json").read_text())
    for tag in ("dims", "recovery", "host90", "guest90"):
        name = f"r8_{tag}_s12345"
        original = (FIX / f"{name}.verdict.json").read_bytes()
        assert hashlib.sha256(original).hexdigest() == manifest[name]["original_verdict_sha256"]
        assert len(tape(name).encode()) == manifest[name]["bytes"]
        assert manifest[name]["build_id"] == "f3f5349_10.07"
    assert_verdict("R8 host", tape("r8_host90_s12345"), "split_play_8m", "PASS",
                   allow_induced=True, expect_fault="host@90")
    raw = tape("r8_guest90_s12345")
    rows = heartbeats([json.loads(l) for l in raw.splitlines()])
    first = next(i for i, e in enumerate(rows) if e["rza"][1][wts.RZA_RECOVERY_FAILURES])
    assert rows[first]["rza"][1][wts.RZA_STATE] == wts.RZ_STATE_REFUSING
    assert rows[first]["rza"][1][wts.RZA_RETRIES] == 1
    assert rows[first]["rza"][1][wts.RZA_RECOVERY_EXITS_OK] == 1
    assert rows[first]["rze"][1] == [0, 1, 2, 1, 200, 2, 2, 0]
    assert rows[first + 1]["rza"][1][wts.RZA_STATE] == 0
    assert_verdict("R8 guest settled next beat", raw, "split_play_8m", "PASS",
                   allow_induced=True, expect_fault="guest@90")
    recovery = [json.loads(l) for l in tape("r8_recovery_s12345").splitlines()]
    assert [(recovery[n - 1]["marker"], recovery[n - 1]["z"])
            for n in (222, 223, 224, 225, 226)] == [
                ("block_placed", 43), ("block_placed", 43),
                ("block_placed", -40), ("baseline", -40.51),
                ("baseline", 42.53)]
    assert recovery[298 - 1]["marker"] == "world_loaded"
    assert recovery[299 - 1]["marker"] == "host_verified"
    assert recovery[581 - 1]["phase"] == "p4" and recovery[581 - 1]["diag"] == "phase timeout"
    dims = [json.loads(l) for l in tape("r8_dims_s12345").splitlines()]
    assert dims[249 - 1]["host_commits"] == 1
    assert dims[306 - 1]["host_commits"] == dims[369 - 1]["host_commits"] == 1
    assert dims[455 - 1]["host_phase"] == "timeout" and dims[455 - 1]["host_commits"] == 1
    assert dims[341 - 1]["player_health"] == 160
    assert dims[344 - 1]["player_health"] == 136
    assert dims[347 - 1]["x"] == 0.3 and abs(dims[347 - 1]["z"]) == 0.0
    dv = verdict(tape("r8_dims_s12345"), "split_dims_6m")
    assert any(s["world_slot"] == 1 and s["moved_dm"] == 343
               for s in dv["actors"][0]["segments"])

    def bad_settlement(es):
        for e in heartbeats(es):
            if e["rza"][1][wts.RZA_RECOVERY_FAILURES]:
                e["rza"][1][wts.RZA_STATE] = wts.RZ_STATE_REFUSING
                e["rza"][1][wts.RZA_STATE_AGE] = 1
    v = verdict(mutate(raw, bad_settlement), "split_play_8m",
                allow_induced=True, expect_fault="guest@90")
    assert v["status"] == "FAIL" and "induced_no_retry" in v["codes"]

    def settlement_at(es, delay):
        rows = heartbeats(es)
        first = next(i for i, e in enumerate(rows) if e["rza"][1][wts.RZA_RECOVERY_FAILURES])
        rows[first + 1]["rza"][1][wts.RZA_STATE] = wts.RZ_STATE_REFUSING
        rows[first + 2]["rzt"] = rows[first]["rzt"] + delay
    assert_verdict("settlement at 40 ticks", mutate(raw, lambda es: settlement_at(es, 40)),
                   "split_play_8m", "PASS", allow_induced=True, expect_fault="guest@90")
    v = verdict(mutate(raw, lambda es: settlement_at(es, 41)), "split_play_8m",
                allow_induced=True, expect_fault="guest@90")
    assert v["status"] == "FAIL" and "induced_no_retry" in v["codes"]

    def wake_hold_first(es):
        first = next(e for e in heartbeats(es) if e["rza"][1][wts.RZA_RECOVERY_FAILURES])
        first["rza"][1][wts.RZA_STATE] = wts.RZ_STATE_WAKE_HOLD
        first["rza"][1][wts.RZA_STATE_AGE] = 1
    assert_verdict("wake hold then settled", mutate(raw, wake_hold_first),
                   "split_play_8m", "PASS", allow_induced=True, expect_fault="guest@90")

    def missing_exit(es):
        for e in heartbeats(es):
            e["rza"][1][wts.RZA_RECOVERY_EXITS_OK] = 0
    v = verdict(mutate(raw, missing_exit), "split_play_8m",
                allow_induced=True, expect_fault="guest@90")
    assert v["status"] == "FAIL" and "induced_no_retry" in v["codes"]


def main():
    tests = [test_retained_positives, test_retained_idle, test_retained_missing_movement,
             test_real_tape_mutations, test_boundaries, test_derived_overall_precedence,
             test_retained_build_ids, test_f7_provenance_and_verdicts,
             test_f7_coalesced_negatives, test_f7_derived_exact_receipts,
             test_r7_frozen_failures, test_r8_guest_coalesced_refusal_settlement]
    failed = 0
    for fn in tests:
        try:
            fn()
        except Exception as exc:
            failed += 1
            print(f"FAIL {fn.__name__}: {exc}")
        else:
            print(f"PASS {fn.__name__}")
    print(f"{len(tests)-failed}/{len(tests)} retained residency tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
