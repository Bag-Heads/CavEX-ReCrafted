#!/usr/bin/env python3
"""residency-1005/4 closure items 9-11: the collector's residency gate.

Every case writes RAW NDJSON text and drives it through BOTH collector entry
points with identical options:
  * the tape CLI  (`python tools/wii_telemetry_server.py --residency-gate-tape
    <file> [opts]`, a real subprocess -- its exit code is checked), and
  * the live Collector (parse_collector_args -> Collector.process per decoded
    line / note_bad_line per undecodable line -> write_result -> the
    residency_gate recorded in PASS.json/FAIL.json),
and requires the same verdict status and reason codes from both.

A small tape generator exercises precise state transitions. Lossless retained
Wii tapes and their mutations provide independent producer-grounded coverage;
both runner paths and offline replay are exercised without Wii contact.
Run standalone: `python tests/test_residency_gate.py` (exit 0 = all pass).
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
COLLECTOR = TOOLS / "wii_telemetry_server.py"
RUNNER = TOOLS / "run_wii_hardware.ps1"
sys.path.insert(0, str(TOOLS))
import wii_telemetry_server as wts  # noqa: E402

RUN_ID = "rzgate"
FIELDS = ("slot", "gen", "epoch", "attempt", "state", "streak_age", "state_age",
          "last_ticks", "last_result", "pending", "oldest_req_age", "requests",
          "loaded", "service_delay_max", "latency_max", "exits_ok", "failures",
          "recovery_ticks_max", "retries", "notices", "vetoes", "moved_dm")
assert len(FIELDS) == wts.RZA_FIELDS == 22
ABSENT = {"slot": 255}
ALL_PHASES = (wts.RZF_ARMED | wts.RZF_ENTERED | wts.RZF_TERMINAL | wts.RZF_NOTICE
              | wts.RZF_RETRY | wts.RZF_DISARMED)


# ---------------------------------------------------------------- generator
def rza_row(spec):
    """Row dict (FIELDS names) -> 22-int list; None stays None, a list given
    as "raw" replaces the row verbatim (malformed-shape cases)."""
    if spec is None:
        spec = ABSENT
    if "raw" in spec:
        return spec["raw"]
    vals = {k: 0 for k in FIELDS}
    vals.update(spec)
    return [vals[k] for k in FIELDS]


def beat(i, host, guest=None, **extra):
    """One heartbeat dict. extra: rzi, rzf, x/z/gx/gz, omit_rz, rzt, rzv."""
    ev = {"event": "heartbeat", "run_id": RUN_ID, "profile": extra.pop("profile", "far_walk_6m"),
          "tick": 100 + i * 20, "world_loaded": True}
    hx = host.get("moved_dm", 0) / 10.0 if host and host.get("slot", 0) != 255 else 0.0
    ev["x"], ev["z"] = extra.pop("x", 8.0 + hx), extra.pop("z", 8.0)
    if guest is not None and guest.get("slot", 0) != 255:
        ev["guest"] = True
        ev["gx"], ev["gz"] = extra.pop("gx", 8.0 + guest.get("moved_dm", 0) / 10.0), extra.pop("gz", 40.0)
    else:
        ev["guest"] = False
    if not extra.pop("omit_rz", False):
        ev["rzv"] = extra.pop("rzv", 2)
        ev["rzt"] = extra.pop("rzt", 1000 + i * 20)
        ev["rza"] = [rza_row(host), rza_row(guest)]
        rzi = extra.pop("rzi", None)
        if rzi is not None:
            ev["rzi"] = rzi
        ev["rzf"] = extra.pop("rzf", 0)
    ev.update(extra)
    return ev


def tape(beats, profile="far_walk_6m", preamble=True):
    """beats: list of (host_spec, guest_spec, extra) -> NDJSON text. A plain
    heartbeat without residency keys leads (allowed before rza begins)."""
    lines = []
    if preamble:
        lines.append(json.dumps({"event": "heartbeat", "run_id": RUN_ID, "profile": profile,
                                 "tick": 80, "x": 8.0, "z": 8.0, "guest": False}))
    for i, (h, g, extra) in enumerate(beats):
        extra = dict(extra or {})
        extra.setdefault("profile", profile)
        lines.append(json.dumps(beat(i, h, g, **extra), allow_nan=True))
    return "\n".join(lines) + "\n"


def mover(n=6, slot=0, gen=1, epoch=1, start=0, step_dm=100, step_req=3, dm_start=None, **over):
    """n rows of an actor walking in one segment: requests/loads/moved grow."""
    rows = []
    for i in range(n):
        r = {"slot": slot, "gen": gen, "epoch": epoch,
             "requests": start + step_req * i, "loaded": start + step_req * i,
             "moved_dm": (start * 10 if dm_start is None else dm_start) + step_dm * i,
             "notices": 0}
        r.update(over)
        rows.append(r)
    return rows


def idle(n=6, slot=0, gen=1, epoch=1, **over):
    rows = []
    for _ in range(n):
        r = {"slot": slot, "gen": gen, "epoch": epoch}
        r.update(over)
        rows.append(r)
    return rows


def zipb(hosts, guests=None, extras=None):
    n = len(hosts)
    guests = guests or [None] * n
    extras = extras or [{}] * n
    return list(zip(hosts, guests, extras))


# ---------------------------------------------------------------- drivers
def run_tape_cli(text, profile, opts):
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "tape.ndjson"
        path.write_text(text, encoding="utf-8")
        cmd = [sys.executable, str(COLLECTOR), "--residency-gate-tape", str(path),
               "--profile", profile] + list(opts)
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if proc.returncode not in (0, 1):
        raise AssertionError(f"tape CLI crashed rc={proc.returncode}: {proc.stderr[-2000:]}")
    if "Traceback" in proc.stderr:
        raise AssertionError(f"tape CLI raised: {proc.stderr[-2000:]}")
    verdict, _ = json.JSONDecoder().raw_decode(proc.stdout.lstrip())
    return verdict, proc.returncode


def run_live(text, profile, opts):
    with tempfile.TemporaryDirectory() as td:
        args = wts.parse_collector_args(["--out", td, "--run-id", RUN_ID, "--profile", profile,
                                         "--require-residency"] + list(opts))
        c = wts.Collector(args)
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink):
            for line in text.splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError as exc:
                    c.note_bad_line(exc)
                    continue
                c.process(event)
            c.write_result()
        result_path = Path(td) / ("PASS.json" if (Path(td) / "PASS.json").exists() else "FAIL.json")
        result = json.loads(result_path.read_text(encoding="utf-8"))
    return result["residency_gate"], result


RESULTS = []


def check(name, text, profile, opts=(), expect="FAIL", codes=(), absent_codes=()):
    """Both entry points; identical verdicts; expected status + reason codes."""
    tv, rc = run_tape_cli(text, profile, opts)
    lv, _ = run_live(text, profile, opts)
    problems = []
    if isinstance(expect, str):
        expect = (expect,)
    if tv["status"] not in expect:
        problems.append(f"tape status {tv['status']} not in {expect}: {tv['reasons']}")
    if (rc == 0) != (tv["status"] == "PASS"):
        problems.append(f"tape CLI exit {rc} disagrees with status {tv['status']}")
    if "PASS" not in expect and rc == 0:
        problems.append("tape CLI exited 0 on a non-PASS case")
    for code in codes:
        if code not in tv["codes"]:
            problems.append(f"missing reason code {code!r}; got {tv['codes']}: {tv['reasons']}")
    for code in absent_codes:
        if code in tv["codes"]:
            problems.append(f"unexpected reason code {code!r}: {tv['reasons']}")
    if (lv["status"], lv["codes"]) != (tv["status"], tv["codes"]):
        problems.append(f"live verdict {lv['status']} {lv['codes']} != tape {tv['status']} {tv['codes']}")
    if problems:
        raise AssertionError(f"{name}: " + " | ".join(problems))
    return tv


def case(fn):
    RESULTS.append(fn)
    return fn


# ---------------------------------------------------------------- baselines
def base_host(profile="far_walk_6m", n=6):
    return tape(zipb(mover(n)), profile)


def base_two(profile="split_play_8m", n=6):
    return tape(zipb(mover(n), mover(n)), profile)


@case
def baseline_single_pass():
    check("baseline", base_host(), "far_walk_6m", expect="PASS")
    check("baseline_two", base_two(), "split_play_8m", expect="PASS")


# ---------------------------------------------------------------- item 9
@case
def missing_middle():
    b = zipb(mover())
    b[3] = (b[3][0], None, {"omit_rz": True})
    check("missing_middle", tape(b), "far_walk_6m", expect=("UNPROVEN", "FAIL"), codes=["missing_middle"])


@case
def missing_final():
    b = zipb(mover())
    b[-1] = (b[-1][0], None, {"omit_rz": True})
    check("missing_final", tape(b), "far_walk_6m", expect=("UNPROVEN", "FAIL"), codes=["missing_final"])
    # an overflow substitute heartbeat is missing evidence too
    b = zipb(mover())
    b[-1] = (b[-1][0], None, {"omit_rz": True, "overflow": True})
    check("missing_final_overflow", tape(b), "far_walk_6m", expect=("UNPROVEN", "FAIL"),
          codes=["missing_final"])


@case
def malformed_rza():
    rows = mover()
    rows[3] = {"raw": [0] * 21}
    check("malformed_rza", tape(zipb(rows)), "far_walk_6m", codes=["malformed_rza"])
    b = zipb(mover())
    b[2] = (b[2][0], None, {"rzv": 1})
    check("malformed_rza_version", tape(b), "far_walk_6m", codes=["version_mismatch"])
    rows = mover()
    rows[2] = dict(rows[2], requests=2.5)
    check("malformed_rza_float", tape(zipb(rows)), "far_walk_6m", codes=["malformed_rza"])
    rows = mover()
    rows[2] = dict(rows[2], loaded=-1)
    check("malformed_rza_negative", tape(zipb(rows)), "far_walk_6m", codes=["malformed_rza"])
    rows = mover()
    rows[2] = dict(rows[2], moved_dm=2 ** 32)
    check("malformed_rza_over_u32", tape(zipb(rows)), "far_walk_6m", codes=["malformed_rza"])


@case
def null_counter():
    rows = mover()
    rows[3] = dict(rows[3], requests=None)
    check("null_counter", tape(zipb(rows)), "far_walk_6m", codes=["null_value"])


@case
def nonfinite_position():
    b = zipb(mover())
    b[3] = (b[3][0], None, {"x": float("nan")})
    text = tape(b)
    assert "NaN" in text
    check("nonfinite_position", text, "far_walk_6m", codes=["nonfinite_position"])
    b = zipb(mover())
    b[3] = (b[3][0], None, {"z": float("inf")})
    check("nonfinite_position_inf", tape(b), "far_walk_6m", codes=["nonfinite_position"])
    # the C formatter printing %f of NaN gives a bare `nan` -> undecodable line
    raw = base_host().splitlines()
    raw[4] = raw[4].replace('"x": ', '"x": nan, "x_was": ', 1)
    check("nonfinite_position_raw_nan", "\n".join(raw) + "\n", "far_walk_6m",
          expect=("UNPROVEN", "FAIL"), codes=["unparseable_line"])


@case
def invalid_identity():
    rows = mover()
    rows[2] = dict(rows[2], slot=7)
    check("invalid_identity_slot", tape(zipb(rows)), "far_walk_6m", codes=["invalid_identity"])
    rows = mover()
    rows[2] = dict(rows[2], gen=0)
    check("invalid_identity_gen0", tape(zipb(rows)), "far_walk_6m", codes=["invalid_identity"])
    g = [dict(ABSENT, state=2, state_age=5)] * 6
    check("invalid_identity_absent_live", tape(zipb(mover(), g), "split_anchor_6m"),
          "split_anchor_6m", codes=["invalid_identity"])


@case
def out_of_order():
    b = zipb(mover())
    b[4] = (b[4][0], None, {"rzt": 1000})
    check("out_of_order", tape(b), "far_walk_6m", codes=["out_of_order"])


@case
def failure_then_reset():
    rows = mover()
    rows[2] = dict(rows[2], failures=1, last_result=2, last_ticks=200, attempt=1)
    rows[3] = dict(rows[3], attempt=1)
    # rows 4.. "reset" the session counter back to 0
    for r in rows[4:]:
        r["attempt"] = 1
    check("failure_then_reset", tape(zipb(rows)), "far_walk_6m",
          codes=["counter_decrease", "uninduced_failure"])


@case
def failed_zero_ages():
    rows = mover()
    rows[-1] = dict(rows[-1], state=3, streak_age=0, state_age=0)
    check("failed_zero_ages", tape(zipb(rows)), "far_walk_6m", codes=["unresolved_at_completion"])


@case
def actor_disappears_unresolved():
    g = idle(4) + [ABSENT, ABSENT]
    g[3] = dict(g[3], state=2, state_age=50, attempt=1)
    check("actor_disappears_unresolved", tape(zipb(mover(), g), "split_anchor_6m"),
          "split_anchor_6m", codes=["actor_disappeared_unresolved"])


# ---------------------------------------------------------------- item 10
@case
def world_jump_only():
    # small walk in slot 0, then a transfer jump far away in slot 1: the
    # displacement is huge, admitted movement in each segment is not.
    h = mover(3, step_dm=20) + mover(3, slot=1, gen=1, epoch=2, start=6, step_dm=20)
    for r in h[3:]:
        r["moved_dm"] = 40 + (r["moved_dm"] - 60)
    ex = [{}] * 3 + [{"x": 5000.0 + i} for i in range(3)]
    check("world_jump_only", tape(zipb(h, None, ex)), "far_walk_6m", codes=["no_moving_segment"])


@case
def inherited_requests_loads():
    seg1 = mover(3, step_dm=50)                       # requests+loads, 10 blocks
    seg2 = [{"slot": 0, "gen": 1, "epoch": 2, "requests": 6, "loaded": 6,
             "moved_dm": 100 + 300 * i} for i in range(3)]   # 60 blocks, no new requests
    text = tape(zipb(seg1 + seg2))
    v = check("inherited_requests_loads", text, "far_walk_6m", expect="PASS")
    host = v["actors"][0]
    assert host["requests"] > 0 and host["moved_blocks"] >= 32, host  # old whole-tape gate would PASS
    assert host["segments"][-1]["gate_state"] == "NO_DEMAND"


@case
def same_slot_new_generation():
    seg1 = mover(3, step_dm=50)
    seg2 = [{"slot": 0, "gen": 2, "epoch": 1, "requests": 6, "loaded": 6,
             "moved_dm": 100 + 300 * i} for i in range(3)]
    check("same_slot_new_generation", tape(zipb(seg1 + seg2)), "far_walk_6m",
          expect="PASS")


@case
def body_replacement():
    g1 = mover(3, step_dm=50)
    g2 = [{"slot": 0, "gen": 1, "epoch": 2, "requests": 6, "loaded": 6,
           "moved_dm": 100 + 300 * i} for i in range(3)]
    guest = g1 + [dict(g1[-1], slot=255)] + g2
    host = mover(7)
    v = check("body_replacement", tape(zipb(host, guest), "split_play_8m"), "split_play_8m",
              expect="PASS")
    assert v["actors"][1]["segments"][-1]["gate_state"] == "NO_DEMAND"


@case
def same_world_teleport():
    seg1 = mover(3, step_dm=125)                        # 25 blocks
    seg2 = mover(3, epoch=2, start=6, step_dm=125)      # +25 blocks after the teleport
    for r in seg2:
        r["moved_dm"] = 250 + (r["moved_dm"] - 60)
    ex = [{}] * 3 + [{"x": 900.0 + i} for i in range(3)]
    v = check("same_world_teleport", tape(zipb(seg1 + seg2, None, ex)), "far_walk_6m",
              codes=["no_moving_segment"])
    assert v["actors"][0]["moved_blocks"] >= 32, v["actors"][0]   # cumulative would pass


@case
def failure_before_transfer():
    h = mover(3) + mover(4, slot=1, epoch=2, start=6, step_dm=150, dm_start=200)
    h[1] = dict(h[1], state=2, state_age=100, attempt=1)
    h[2] = dict(h[2], state=3, state_age=10, attempt=1, failures=1, last_result=2, last_ticks=200)
    for r in h[3:]:
        r.update(attempt=1, failures=1, last_result=2, last_ticks=200)
    check("failure_before_transfer", tape(zipb(h), "split_dims_6m"), "split_dims_6m",
          opts=["--residency-require-moving", "host:1"], codes=["uninduced_failure"],
          absent_codes=[])


@case
def two_world_legs():
    h = mover(4, step_dm=150) + mover(4, slot=1, epoch=2, start=12, step_dm=150, dm_start=450)
    text = tape(zipb(h), "split_dims_6m")
    v = check("two_world_legs", text, "split_dims_6m", expect="PASS")
    assert v["required_legs"] == ["host:0", "host:1"], v["required_legs"]
    check("two_world_legs_explicit", text, "split_dims_6m",
          opts=["--residency-require-moving", "host:0,host:1"], expect="PASS")
    # independent evidence: the slot-1 leg alone too short -> that leg FAILs
    h2 = mover(4, step_dm=150) + mover(4, slot=1, epoch=2, start=12, step_dm=20, dm_start=450)
    v = check("two_world_legs_short_nether", tape(zipb(h2), "split_dims_6m"), "split_dims_6m",
              codes=["no_moving_segment"])
    assert all("host:1" in r for r in v["reasons"]), v["reasons"]


# ---------------------------------------------------------------- item 11
@case
def single_player():
    check("single_player", base_host(), "far_walk_6m", expect="PASS")
    check("single_player_idle_host", tape(zipb(idle(6, requests=3, loaded=3))), "far_walk_6m",
          codes=["no_moving_segment"])


@case
def synthetic_anchor():
    text = tape(zipb(mover()), "split_anchor_6m")
    v = check("synthetic_anchor", text, "split_anchor_6m", expect="PASS")
    assert v["required_legs"] == ["host"], v
    check("synthetic_anchor_as_route_a", text, "split_play_8m", codes=["missing_required_actor"])


@case
def two_actual_actors():
    check("two_actual_actors", base_two(), "split_play_8m", expect="PASS")
    check("two_actual_actors_idle_guest", tape(zipb(mover(), idle(6)), "split_play_8m"),
          "split_play_8m", codes=["no_moving_segment"])


@case
def idle_partner_failure():
    g = idle(6)
    g[2] = dict(g[2], state=3, attempt=1, failures=1, last_result=2, last_ticks=200, state_age=5)
    for r in g[3:]:
        r.update(attempt=1, failures=1, last_result=2, last_ticks=200, retries=1, exits_ok=1)
    check("idle_partner_failure", tape(zipb(mover(), g), "split_anchor_6m"), "split_anchor_6m",
          codes=["uninduced_failure"])


MARK = [0, 0, 1, 1, 1]   # host, slot 0, world gen 1, body epoch 1, attempt 1


def induced_beats(mark=MARK, last_ticks=200, retry=True, rzf_final=ALL_PHASES,
                  notice_at=3, actor_rows=None):
    """Host walks; attempt 1 is armed, enters (with >40-tick refusal that the
    exception covers), terminates FAILED at 200 ticks with a notice, then a
    deliberate retry (attempt 2) recovers."""
    h = mover(7)
    for r in h:
        r.update(attempt=0)
    h[2].update(state=2, state_age=120, streak_age=120, pending=2, oldest_req_age=120, attempt=1)
    h[3].update(state=3, state_age=10, streak_age=140, attempt=1, failures=1, last_result=2,
                last_ticks=last_ticks)
    for r in h[4:]:
        r.update(attempt=1, failures=1, last_result=2, last_ticks=last_ticks)
    if retry:
        h[4].update(state=2, state_age=20, streak_age=160, attempt=2, retries=1)
        for r in h[5:]:
            r.update(state=0, attempt=2, retries=1, exits_ok=1, last_result=1, last_ticks=25,
                     recovery_ticks_max=25, latency_max=165)
    for i, r in enumerate(h):
        r["notices"] = 1 if i >= notice_at else 0
    phases = [0, wts.RZF_ARMED, wts.RZF_ARMED | wts.RZF_ENTERED,
              wts.RZF_ARMED | wts.RZF_ENTERED | wts.RZF_TERMINAL | wts.RZF_NOTICE | wts.RZF_DISARMED]
    phases.append(phases[-1] | wts.RZF_RETRY)
    phases += [rzf_final, rzf_final]
    phases = [p & rzf_final if i < 5 else p for i, p in enumerate(phases)]
    ex = [{"rzf": phases[i], **({"rzi": [mark]} if i >= 1 and mark else {})} for i in range(7)]
    return zipb(h, None, ex)


INDUCED = ["--residency-allow-induced"]


@case
def induced_success():
    text = tape(induced_beats())
    v = check("induced_success", text, "far_walk_6m", opts=INDUCED, expect="PASS")
    assert len(v["induced_exempt"]) == 1 and v["induced_exempt"][0]["mark"] == MARK, v
    # without the option the same failure is uninduced; no global allow-all
    check("induced_success_without_option", text, "far_walk_6m", codes=["uninduced_failure"])
    # rzf must show armed|entered|terminal|notice|disarmed
    check("induced_success_not_disarmed",
          tape(induced_beats(rzf_final=ALL_PHASES & ~wts.RZF_DISARMED)), "far_walk_6m",
          opts=INDUCED, codes=["induced_fault_phases"])
    # a notice that only arrives after the failure sample is too late
    check("induced_success_late_notice", tape(induced_beats(notice_at=4)), "far_walk_6m",
          opts=INDUCED, codes=["induced_no_notice"])


@case
def induced_wrong_actor():
    check("induced_wrong_actor", tape(induced_beats(mark=[1, 0, 1, 1, 1])), "far_walk_6m",
          opts=INDUCED, codes=["uninduced_failure"])


@case
def induced_wrong_episode():
    check("induced_wrong_episode_attempt", tape(induced_beats(mark=[0, 0, 1, 1, 9])), "far_walk_6m",
          opts=INDUCED, codes=["uninduced_failure"])
    check("induced_wrong_episode_epoch", tape(induced_beats(mark=[0, 0, 1, 2, 1])), "far_walk_6m",
          opts=INDUCED, codes=["uninduced_failure"])
    check("induced_wrong_episode_gen", tape(induced_beats(mark=[0, 0, 2, 1, 1])), "far_walk_6m",
          opts=INDUCED, codes=["uninduced_failure"])


@case
def induced_no_retry():
    b = induced_beats(retry=False)
    for h, _, _ in b[4:]:
        h.update(state=0, state_age=0, streak_age=0, pending=0, oldest_req_age=0)
    check("induced_no_retry", tape(b), "far_walk_6m", opts=INDUCED, codes=["induced_no_retry"])


@case
def induced_late_terminal():
    check("induced_late_terminal", tape(induced_beats(last_ticks=201)), "far_walk_6m",
          opts=INDUCED, codes=["induced_late_terminal"])


@case
def induced_unmarked_failure():
    check("induced_unmarked_failure", tape(induced_beats(mark=None)), "far_walk_6m",
          opts=INDUCED, codes=["uninduced_failure"])
    # one marked failure is exempt, a second (unmarked) one is not
    b = induced_beats()
    for h, _, _ in b[6:]:
        h.update(failures=2, attempt=3, last_result=2, last_ticks=200, state=3, state_age=1)
    b.append((dict(b[-1][0], state=0, state_age=0), None, dict(b[-1][2])))
    check("induced_second_unmarked_failure", tape(b), "far_walk_6m", opts=INDUCED,
          codes=["uninduced_failure"])


# ---------------------------------------------------------------- negatives CLI
@case
def gate_negatives_cli():
    """--residency-gate-negatives over a v2 two-actor tape: authentic PASS and
    every single-fault mutation FAIL/UNPROVEN (exit 0)."""
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "tape.ndjson"
        path.write_text(base_two(), encoding="utf-8")
        proc = subprocess.run([sys.executable, str(COLLECTOR), "--residency-gate-tape", str(path),
                               "--residency-gate-negatives"], capture_output=True, text=True,
                              timeout=120)
    assert proc.returncode == 0, proc.stdout[-3000:] + proc.stderr[-2000:]
    assert '"profile": "split_play_8m"' in proc.stdout, "tape profile not derived from heartbeats"
    for name in ("missing_middle", "null_counter", "failed_zero_ages", "guest_no_progress"):
        assert f"negative {name}: " in proc.stdout, name


# ---------------------------------------------------------------- PowerShell
@case
def no_demand_and_demand_closure():
    v = check("zero demand movement", tape(zipb(mover(step_req=0))), "far_walk_6m", expect="PASS")
    assert v["actors"][0]["segments"][0]["gate_state"] == "NO_DEMAND"
    rows = mover()
    for row in rows:
        row["loaded"] = 0
    check("requested zero loads", tape(zipb(rows)), "far_walk_6m", codes=["demand_unserved"])
    # Speculative requests may be retired: requests != loads is valid.
    rows = mover()
    for i, row in enumerate(rows):
        row["loaded"] = i
    v = check("retired speculative demand", tape(zipb(rows)), "far_walk_6m", expect="PASS")
    assert v["actors"][0]["segments"][0]["gate_state"] == "DEMAND_SERVED"
    for field in ("attempt", "latency_max", "exits_ok", "recovery_ticks_max", "retries", "notices"):
        guest = idle()
        for row in guest[3:]:
            row[field] = 1
        check("idle guest activity " + field, tape(zipb(mover(step_req=0), guest)),
              "split_anchor_6m", codes=["demand_unserved"])
    rows = mover()
    rows[-1]["pending"] = 1
    check("pending completion", tape(zipb(rows)), "far_walk_6m", codes=["pending_at_completion"])


@case
def profile_policies_and_precedence():
    for profile in ("heartbeat_selftest", "redstone_clock_2m", "far_idle_2m", "perf_stress_5m"):
        v = check(profile, tape(zipb(idle()), profile), profile, expect="PASS")
        assert v["required_legs"] == []
    check("none cannot waive movement", tape(zipb(idle())), "far_walk_6m",
          opts=["--residency-require-moving", "none"], codes=["no_moving_segment"])
    check("unknown policy", tape(zipb(idle()), "pool_selftest_3m"), "pool_selftest_3m",
          expect="UNPROVEN", codes=["missing_leg_policy"])
    v = check("explicit idle policy", tape(zipb(idle()), "pool_selftest_3m"), "pool_selftest_3m",
              opts=["--residency-require-moving", "none"], expect="PASS")
    assert v["moving_override"] == "none"
    assert wts.residency_tape_profile([{"event": "heartbeat", "profile": "future_profile"}],
                                     "far_idle_2m", False) == "future_profile"
    assert wts.residency_tape_profile([{"event": "heartbeat"}], "far_idle_2m", False) \
        == "unidentified_profile"
    rows = mover()
    rows[1]["pending"] = 7
    b = zipb(rows)
    b[3] = (b[3][0], None, {"omit_rz": True})
    check("violation beats gap", tape(b), "far_walk_6m", codes=["pending_limit", "missing_middle"])
    for field in ("requests", "attempt", "latency_max", "exits_ok", "retries", "notices"):
        rows = mover(3, step_req=0, step_dm=0) + mover(6, epoch=2, step_req=0)
        for row in rows[3:]:
            row[field] = 1
        check("boundary activity " + field, tape(zipb(rows)), "far_walk_6m",
              expect="UNPROVEN", codes=["boundary_activity_unattributed"])


@case
def expected_fault_contract():
    opts = ["--residency-expect-fault", "host@90"]
    v = check("expected fault", tape(induced_beats()), "far_walk_6m", opts=opts, expect="PASS")
    assert v["expect_fault"] == "host@90" and v["allow_induced"]
    assert v["actors"][0]["segments"][0]["gate_state"] == "INDUCED_RECOVERED"
    check("fault missing", base_host(), "far_walk_6m", opts=opts, codes=["induced_not_exercised"])
    check("wrong expected actor", tape(induced_beats()), "far_walk_6m",
          opts=["--residency-expect-fault", "guest@90"], codes=["induced_not_exercised"])
    b = induced_beats()
    for _, _, extra in b:
        extra["rzf"] |= wts.RZF_REFUSED
    check("fault refused", tape(b), "far_walk_6m", opts=opts, codes=["induced_not_exercised"])
    for phase in (wts.RZF_ARMED, wts.RZF_ENTERED, wts.RZF_TERMINAL,
                  wts.RZF_NOTICE, wts.RZF_DISARMED, wts.RZF_RETRY):
        b = induced_beats(rzf_final=ALL_PHASES & ~phase)
        check("missing phase " + str(phase), tape(b), "far_walk_6m", opts=opts,
              codes=["induced_fault_phases", "induced_not_exercised"])
    b = induced_beats()
    b[3][2]["rzf"] &= ~(wts.RZF_TERMINAL | wts.RZF_NOTICE)
    check("late terminal notice", tape(b), "far_walk_6m", opts=opts,
          codes=["induced_fault_phases", "induced_not_exercised"])
    b = induced_beats()
    for h, _, _ in b[4:]:
        h["gen"] = 2
    check("retry in another world", tape(b), "far_walk_6m", opts=opts,
          codes=["induced_no_retry", "identity_changed_unresolved"])
    b = induced_beats()
    for h, _, _ in b:
        h["requests"] = h["loaded"] = 0
    check("fault without service", tape(b), "far_walk_6m", opts=opts,
          codes=["induced_no_service", "induced_not_exercised"])
    b = induced_beats()
    for h, _, _ in b[5:]:
        h["moved_dm"] = 500
    check("fault without resumed movement", tape(b), "far_walk_6m", opts=opts,
          codes=["induced_no_movement", "induced_not_exercised"])
    # A second actor's service cannot satisfy the marked host's episode.
    b = induced_beats()
    other = mover(len(b))
    for i, (h, _, ex) in enumerate(b):
        h["requests"] = h["loaded"] = 0
        b[i] = h, other[i], ex
    check("foreign actor service", tape(b), "far_walk_6m", opts=opts,
          codes=["induced_no_service"])


@case
def retained_hardware_and_runner_regressions():
    import test_residency_retained
    assert test_residency_retained.main() == 0
    proc = subprocess.run([sys.executable, str(ROOT / "tests/test_residency_runners.py")],
                          capture_output=True, text=True, timeout=180)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    print(proc.stdout.strip())


@case
def replay_required_residency():
    import test_residency_retained as retained
    raw = retained.tape("cc_f1_s12345")
    for status in ("PASS", "FAIL", "UNPROVEN"):
        events = [json.loads(line) for line in raw.splitlines()]
        beats = [e for e in events if e.get("event") == "heartbeat" and "rza" in e]
        if status == "FAIL":
            for e in beats:
                e["rza"][0][wts.RZA_MOVED_DM] = 0
        elif status == "UNPROVEN":
            for key in ("rza", "rzt", "rzv", "rzi", "rzf"):
                beats[-1].pop(key, None)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)
            tape_path = path / "telemetry.ndjson"
            contents = "\n".join(json.dumps(e) for e in events) + "\n"
            tape_path.write_text(contents, encoding="utf-8")
            proc = subprocess.run([sys.executable, str(COLLECTOR), "--replay", td,
                                   "--require-residency"], capture_output=True, text=True, timeout=120)
            assert proc.returncode == (0 if status == "PASS" else 1), proc.stdout[-4000:] + proc.stderr
            marker = "[replay] residency: "
            line = next(line for line in proc.stdout.splitlines() if line.startswith(marker))
            verdict = json.loads(line[len(marker):])
            assert verdict["status"] == status, verdict
            assert tape_path.read_text(encoding="utf-8") == contents
            assert sorted(p.name for p in path.iterdir()) == ["telemetry.ndjson"]


def _ps_exe():
    for name in ("powershell.exe", "powershell", "pwsh"):
        exe = shutil.which(name)
        if exe:
            return exe
    return None


@case
def powershell_forwarding():
    """Executes the runner's own residency argument block (between its BEGIN/
    END markers) in PowerShell and checks the exact collector / DOL args;
    then proves the collector's real parser accepts what the runner emits.
    Falls back to a static check (and says so) only if no PowerShell exists
    or it does not start within 120 s."""
    src = RUNNER.read_text(encoding="utf-8")
    begin = src.index("# BEGIN residency collector args")
    end = src.index("# END residency collector args")
    block = src[begin:end]
    # static: the parameters exist and both lists are fed from the block
    for needle in ("[switch]$RequireResidency", "[string[]]$ResidencyRequireMoving",
                   "[switch]$ResidencyAllowInduced", "[string]$ResidencyFault",
                   "$collectorArgs += @(Get-ResidencyCollectorArgs ([bool]$RequireResidency) "
                   "$ResidencyRequireMoving ([bool]$ResidencyAllowInduced) $ResidencyFault)",
                   "$wiiloadArgs += @(Get-ResidencyRunArgs $ResidencyFault)"):
        assert needle in src, f"runner lacks: {needle}"
    assert src.index("Get-ResidencyCollectorArgs ([bool]$RequireResidency)") < src.index("function Start-Collector")
    scenarios = [
        ("$false", "@()", "$false", "''", "", ""),
        ("$true", "@('host,guest')", "$false", "''",
         "--require-residency --residency-require-moving host,guest", ""),
        ("$true", "@('host','guest:1')", "$false", "'guest@30'",
         "--require-residency --residency-require-moving host,guest:1 --residency-allow-induced --residency-expect-fault guest@30",
         "--cw-rz-fault guest@30"),
        ("$false", "@('none')", "$true", "''",
         "--residency-require-moving none --residency-allow-induced", ""),
    ]
    script = [block]
    for i, (req, mov, allow, fault, _, _) in enumerate(scenarios):
        script.append(f"Write-Output ('C{i}=' + ((@(Get-ResidencyCollectorArgs {req} {mov} {allow} {fault})) -join ' '))")
        script.append(f"Write-Output ('R{i}=' + ((@(Get-ResidencyRunArgs {fault})) -join ' '))")
    exe = _ps_exe()
    mode = "static-only (no PowerShell found)"
    if exe:
        with tempfile.TemporaryDirectory() as td:
            ps1 = Path(td) / "rz_args.ps1"
            ps1.write_text("\r\n".join(script) + "\r\n", encoding="utf-8")
            try:
                proc = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-ExecutionPolicy",
                                       "Bypass", "-File", str(ps1)], capture_output=True, text=True,
                                      timeout=120)
            except subprocess.TimeoutExpired:
                proc = None
                mode = "static-only (PowerShell did not start within 120 s)"
        if proc is not None:
            assert proc.returncode == 0, proc.stderr
            got = dict(line.split("=", 1) for line in proc.stdout.splitlines() if "=" in line)
            for i, (_, _, _, _, want_c, want_r) in enumerate(scenarios):
                assert got.get(f"C{i}", None) == want_c, (i, got.get(f"C{i}"), want_c)
                assert got.get(f"R{i}", None) == want_r, (i, got.get(f"R{i}"), want_r)
            mode = "executed the runner block in " + Path(exe).name
    # the collector's real argparse accepts every emitted argument list
    with tempfile.TemporaryDirectory() as td:
        for _, _, _, _, want_c, _ in scenarios:
            args = wts.parse_collector_args(["--out", td, "--profile", "split_play_8m"]
                                            + want_c.split())
            c = wts.Collector(args)
            if "--residency-require-moving host,guest:1" in want_c:
                assert c.residency_required_legs == [(0, None), (1, None), (1, 1)], c.residency_required_legs
            if "none" in want_c:
                assert c.residency_required_legs == [(0, None), (1, None)]
            assert c.residency_allow_induced == ("--residency-allow-induced" in want_c)
    print(f"     [powershell_forwarding: {mode}]")


# ---------------------------------------------------------------- main
def main() -> int:
    failed = 0
    for fn in RESULTS:
        try:
            fn()
        except Exception as exc:  # noqa: BLE001 -- report every case
            failed += 1
            print(f"FAIL {fn.__name__}: {type(exc).__name__}: {exc}", flush=True)
        else:
            print(f"PASS {fn.__name__}", flush=True)
    print(f"{len(RESULTS) - failed}/{len(RESULTS)} residency gate cases passed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
