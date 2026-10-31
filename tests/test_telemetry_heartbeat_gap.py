"""Heartbeat tick gaps are attributed to the device sender's own counters.

2026-09-26 (telreliable): real-Wii split_play_8m lost one whole heartbeat in
three G4 runs. The first diagnosis blamed UDP fragmentation, but staged runs
use the TCP pull stream -- and the heartbeat AFTER each gap already carried
the cause: tel_wto 0->1 with tel_xmax_us=255065 (the per-line send deadline
abandoned the heartbeat mid-line). The device fix (hw_telemetry.c,
hw_telemetry_line_is_critical) stops that; this collector change makes any
future gap name its cause in FAIL.json/summary.md instead of needing a
hand-run diagnosis. Attribution only -- no verdict reads it.

Counter values below are copied from
tmp/wii_runs/g4_1_splitplay_s12345/telemetry.ndjson ticks 132 and 134.
"""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "gap_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)

G4_BEFORE = {"tick": 132, "tel_fail": 0, "tel_wto": 0, "tel_hb_repl": 0,
             "tel_drop": 0, "tel_abd": 0, "tel_xmax_us": 1597,
             "tel_pmax_us": 248}
G4_AFTER = {"tick": 134, "tel_fail": 0, "tel_wto": 1, "tel_hb_repl": 0,
            "tel_drop": 0, "tel_abd": 0, "tel_xmax_us": 255065,
            "tel_pmax_us": 237}


def test_helper():
    gap = collector.heartbeat_gap(G4_BEFORE, G4_AFTER)
    assert gap is not None
    assert gap["missing_ticks"] == [133]
    assert gap["interval_ticks"] == [132, 134]
    assert gap["cause"] == "device_send_deadline", gap
    assert gap["deltas"]["tel_wto"] == 1 and gap["deltas"]["tel_fail"] == 0
    assert gap["tel_xmax_us"] == 255065

    # Consecutive, reset, missing or non-int ticks: not a gap.
    assert collector.heartbeat_gap(G4_BEFORE, dict(G4_BEFORE, tick=133)) is None
    assert collector.heartbeat_gap(G4_AFTER, G4_BEFORE) is None
    assert collector.heartbeat_gap({}, G4_AFTER) is None
    assert collector.heartbeat_gap(dict(G4_BEFORE, tick="132"), G4_AFTER) is None
    assert collector.heartbeat_gap(dict(G4_BEFORE, tick=True), G4_AFTER) is None

    # Cause precedence: a hard send failure outranks everything.
    both = dict(G4_AFTER, tel_fail=1)
    assert collector.heartbeat_gap(G4_BEFORE, both)["cause"] == "device_send_failure"
    repl = dict(G4_AFTER, tel_wto=0, tel_hb_repl=2)
    assert collector.heartbeat_gap(G4_BEFORE, repl)["cause"] == "device_heartbeat_superseded"
    drop = dict(G4_AFTER, tel_wto=0, tel_drop=5)
    assert collector.heartbeat_gap(G4_BEFORE, drop)["cause"] == "device_ring_or_nonblock_drop"
    # Every device counter flat: the host (or the network) lost it, or the
    # device lost it in a way it does not count. Say so, do not guess.
    flat = dict(G4_AFTER, tel_wto=0)
    gap = collector.heartbeat_gap(G4_BEFORE, flat)
    assert gap["cause"] == "unattributed"
    # A two-heartbeat gap lists both ticks.
    assert collector.heartbeat_gap(G4_BEFORE, dict(flat, tick=135))[
        "missing_ticks"] == [133, 134]
    # A counter that went backwards (device restart) is unknown, not negative.
    back = dict(G4_AFTER, tel_wto=0)
    gap = collector.heartbeat_gap(dict(G4_BEFORE, tel_wto=3), back)
    assert gap["deltas"]["tel_wto"] is None and gap["cause"] == "unattributed"
    # Pre-2026-09-08 heartbeats have no tel_wto at all: still well-formed.
    old = {k: v for k, v in G4_AFTER.items() if k != "tel_wto"}
    gap = collector.heartbeat_gap(G4_BEFORE, old)
    assert gap["deltas"]["tel_wto"] is None and gap["cause"] == "unattributed"
    print("PASS heartbeat_gap: G4 tick 133 -> device_send_deadline "
          "(tel_wto +1, tel_xmax_us 255065); precedence, resets and legacy "
          "heartbeats handled")


def heartbeat(run_id, tick, **extra):
    event = {"event": "heartbeat", "run_id": run_id,
             "profile": "heartbeat_selftest", "build_id": "abc_06.27",
             "tick": tick, "world_loaded": True, "fps": 60,
             "tel_fail": 0, "tel_wto": 0, "tel_hb_repl": 0, "tel_drop": 0,
             "tel_abd": 0}
    event.update(extra)
    return event


def test_collector_records_and_renders():
    with tempfile.TemporaryDirectory() as td:
        out = Path(td)
        args = collector._selftest_args(out, "gaprun", "heartbeat_selftest")
        c = collector.Collector(args)
        for tick in (1, 2, 3):
            c.process(heartbeat("gaprun", tick))
        c.process(heartbeat("gaprun", 5, tel_wto=1, tel_xmax_us=255065))
        c.process(heartbeat("gaprun", 6, tel_wto=1))
        assert [g["missing_ticks"] for g in c.telemetry_gaps] == [[4]]
        c._finish_normal("gap selftest")
        c.write_result()
        written = [p for p in ("PASS.json", "FAIL.json") if (out / p).is_file()]
        assert written, "collector wrote no verdict"
        result = json.loads((out / written[0]).read_text(encoding="utf-8"))
        assert result["telemetry_gaps"][0]["cause"] == "device_send_deadline"
        assert result["telemetry_gaps"][0]["missing_ticks"] == [4]
        summary = (out / "summary.md").read_text(encoding="utf-8")
        assert ("- Heartbeat gap: ticks [4] missing, cause=device_send_deadline"
                in summary), summary

        # A gap-free run renders no gap line and records an empty list.
        out2 = out / "clean"
        out2.mkdir()
        c = collector.Collector(collector._selftest_args(
            out2, "cleanrun", "heartbeat_selftest"))
        for tick in (1, 2, 3):
            c.process(heartbeat("cleanrun", tick))
        c._finish_normal("clean selftest")
        c.write_result()
        written = [p for p in ("PASS.json", "FAIL.json") if (out2 / p).is_file()]
        result = json.loads((out2 / written[0]).read_text(encoding="utf-8"))
        assert result["telemetry_gaps"] == []
        assert "Heartbeat gap" not in (out2 / "summary.md").read_text(encoding="utf-8")
    print("PASS collector: gap recorded in the verdict JSON and summary.md "
          "with its device cause; a clean run records none")


if __name__ == "__main__":
    test_helper()
    test_collector_records_and_renders()
