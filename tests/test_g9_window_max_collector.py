"""ASTRA-G9-R5 points 9/13: the perf_stress_5m 50,000 us gate reads the
per-window rt/wt MAXIMA (lqd v2 entries 14/15), proven on the REAL g9_1 tape.

The retained tape (tmp/wii_runs/g9_g9_1_s12345/telemetry.ndjson, build
9ed1689e6, lqd v1) is replayed through the collector exactly as --replay
does, three ways:
  1. as recorded: FAIL perf_server_budget_exceeded with wt = 1,127,127 (the
     one checkpoint tick the last-tick sample happened to catch); the
     historical verdict is preserved;
  2. every srv_rt_us/srv_wt_us sample zeroed, lqd kept v1: the budget gate
     cannot see any spike (a v1 tape with unlucky sampling); the failure
     class is NOT the budget;
  3. the same zeroed samples with lqd extended to v2 and the window maximum
     1,127,127 placed at entry 15 of heartbeat 146 (SPIKE-DIAG section 1):
     the gate FAILs on the window maximum with the same figure.
The tape is read-only; the collector writes to a temporary directory."""
import importlib.util
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))  # the collector imports its siblings
spec = importlib.util.spec_from_file_location(
    "g9_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules["g9_collector"] = collector
spec.loader.exec_module(collector)

# Bundled verbatim (md5 d0c84fb42d0d45ef4fdfc893d90f1e10) from tmp/wii_runs/g9_g9_1_s12345/telemetry.ndjson.
TAPE = ROOT / "tests/fixtures/g9_g9_1_s12345.ndjson.bin"
RUN_ID = "g9_g9_1_s12345"  # the tape's own run_id (heartbeats are keyed on it)
SPIKE_WT = 1127127
SPIKE_HB = 146


def grade(lines):
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "replay"
        out.mkdir()
        # exactly replay_run_dir's construction for a non-TNT replay: no
        # build pin (the selftest default would reject the real build stamp)
        c = collector.Collector(collector._selftest_args(
            out, RUN_ID, "perf_stress_5m", expected_build="", duration=0))
        for line in lines:
            if c.done:
                break
            if collector.ingest_telemetry_line(c, line):
                break
        if not c.done:
            # the tape has no profile_done: grade what there is, as --replay does
            c._finish_normal("offline replay of " + str(TAPE))
        return c


def rows(mutate=None):
    out = []
    hb_index = 0
    for line in TAPE.read_text(encoding="utf-8", errors="replace").splitlines(True):
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            out.append(line)
            continue
        if isinstance(ev, dict) and ev.get("event") == "heartbeat":
            # The tape predates the d1 death snapshot the collector now
            # requires; supply a session-valid zero-death record so the
            # budget gate (the subject here) is what grades the run.
            ev.setdefault("d1", [1, 0, 0, 0, 0])
            # The g9_1 run predates the stress survival guard and its body
            # died mid-run; the later death gate would end grading there,
            # before the hb 146 spike this test is about. Grade a live body.
            if isinstance(ev.get("player_health"), int) and ev["player_health"] <= 0                     and ev["player_health"] != -1:
                ev["player_health"] = 20
            if mutate:
                mutate(ev, hb_index)
            hb_index += 1
            line = json.dumps(ev, separators=(",", ":")) + "\n"
        out.append(line)
    return out


def zero_samples(ev, _i):
    ev["srv_rt_us"] = 0
    ev["srv_wt_us"] = 0


def zero_samples_window_max(ev, i):
    zero_samples(ev, i)
    lqd = list(ev.get("lqd") or [0] * 14)
    lqd = (lqd + [0] * 22)[:22]
    if i == SPIKE_HB:
        lqd[15] = SPIKE_WT
    ev["lqd"] = lqd


def main():
    a = [0] * 22
    a[12] = 3
    a[0], a[2], a[4], a[6], a[8], a[10] = 54, 14078, 123, 602820, 400000, 20000
    a[1], a[3], a[7] = 314, 80443, 138761
    r = collector.g9_drain_records({"lqd_v": 3, "lqd": a})
    assert r[0]["pops"] == 14078 and r[0]["us"] == 602820
    assert r[0]["yield_us"] == 400000 and r[0]["recovery_us"] == 123
    assert r[1]["pops"] == 80443 and r[1]["us"] == 138761
    assert collector.g9_drain_records({"lqd": [0] * 14}) == []
    assert collector.g9_drain_records({"lqd": [0] * 22}) == []
    a[6] = 0xffffffff
    assert collector.g9_drain_records({"lqd_v": 3, "lqd": a})[0]["us"] == 0xffffffff
    assert collector.g9_drain_records({"event": "g9_drain_diag", "pops": 42})[0]["pops"] == 42
    assert TAPE.is_file(), "missing real tape " + str(TAPE)
    as_recorded = grade(rows())
    print("as recorded:", as_recorded.failure_class, as_recorded.fail_reason,
          "max_wt", as_recorded.max_wt_us)
    assert as_recorded.failure_class == "perf_server_budget_exceeded"
    assert as_recorded.max_wt_us == SPIKE_WT

    blind = grade(rows(zero_samples))
    print("samples zeroed, lqd v1:", blind.failure_class, "max_wt", blind.max_wt_us)
    assert blind.max_wt_us == 0
    assert blind.failure_class != "perf_server_budget_exceeded"

    windowed = grade(rows(zero_samples_window_max))
    print("samples zeroed, lqd v2 window max:", windowed.failure_class,
          windowed.fail_reason, "max_wt", windowed.max_wt_us)
    assert windowed.failure_class == "perf_server_budget_exceeded"
    assert windowed.max_wt_us == SPIKE_WT
    assert ("wt=%d" % SPIKE_WT) in windowed.fail_reason
    print("test_g9_window_max_collector: PASS")


if __name__ == "__main__":
    # R18: these hardware tapes hold a private rig IP and stay out of the
    # public export (docs/release-allowlist.json); skip (77) where absent.
    _r18_tapes = ("g9_g9_1_s12345.ndjson.bin", "g9_x_x_g9a_s12345.ndjson.bin",
                  "g9_x_x_g9b_s12345.ndjson.bin", "g9_y_y_g9a_s12345.ndjson.bin",
                  "g9_y_y_g9b_s12345.ndjson.bin", "g9_x_provenance.json", "g9_y_provenance.json")
    _r18_root = __import__("pathlib").Path(__file__).resolve().parent / "fixtures"
    if not all((_r18_root / _t).is_file() for _t in _r18_tapes):
        print("SKIP: private G9 hardware tapes are not in this tree (public export)")
        raise SystemExit(77)
    main()
