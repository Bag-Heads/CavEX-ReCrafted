"""L225: native producer windows through the live collector, plus old tapes."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "fps_window_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", type=Path, required=True)
    args = ap.parse_args()
    # Real formatter/sampler output, not a Python copy of the C algorithm.
    wire = subprocess.run([str(args.bin.resolve()), "--fps-window-fixtures"],
                          check=True, capture_output=True, text=True).stdout.splitlines()
    rows = [json.loads(line) for line in wire]
    expected = [[0, 0], [60, 1000000], [0, 0], [60, 1000000],
                [60, 1100000], [180, 3000000], [0, 0], [60, 1000000],
                [0, 0], [60, 1000000], [0, 1000000]]
    assert len(rows) == len(expected)
    for row, (frames, us) in zip(rows, expected):
        parsed = collector.parse_heartbeat_fps_window(row)
        rate = frames * 1_000_000 / us if us else 0
        assert abs(parsed - rate) <= .0051 and parsed == row["wf"]
        assert "fps" not in row
        assert row["sd_trace_max_us"] == 100000 and row["sd_trace_over_30ms"] == 1
        assert row["perf_max_us"] == 116667

    with tempfile.TemporaryDirectory(prefix="cw-fps-window-") as td:
        c = collector.Collector(collector._selftest_args(
            Path(td), "fps-window", "heartbeat_selftest", expected_build="", duration=0))
        # Deliberately lose heartbeats 1..3 and 6..9 on the wire. Each surviving
        # producer window stands alone; receive spacing cannot distort its rate.
        for i in (0, 4, 5, 10):
            assert not collector.ingest_telemetry_line(c, wire[i])
            ctx = c.failure_context()
            assert ctx["fps_window"] == collector.parse_heartbeat_fps_window(rows[i])
            assert ctx["fps"] == rows[i]["wf"]
        assert c.hb_count == 4
        # If a tape contains both keys, the explicitly windowed rate wins.
        mixed = {**rows[5], "fps": 17.0}
        assert not collector.ingest_telemetry_line(c, json.dumps(mixed))
        assert c.failure_context()["fps"] == 60.0
        assert mixed["fps"] == 17.0

    # An authentic pre-L225 hardware excerpt still enters the live JSON parser.
    fixture = ROOT / "tests/fixtures/cc_c1_s12345_anchor_pool.ndjson"
    old_line = next(line for line in fixture.read_text(encoding="utf-8").splitlines()
                    if json.loads(line).get("event") == "heartbeat")
    old = json.loads(old_line)
    assert "wf" not in old
    with tempfile.TemporaryDirectory(prefix="cw-fps-legacy-") as td:
        c = collector.Collector(collector._selftest_args(
            Path(td), old["run_id"], old["profile"], expected_build="", duration=0))
        c.anchor.pool.historical = True
        assert not collector.ingest_telemetry_line(c, old_line)
        assert c.hb_count == 1
        assert c.failure_context()["fps_window"] is None
        assert c.failure_context()["fps"] == old["fps"]

    # Missing or corrupt evidence cannot invent an average. Existing gates stay
    # unchanged; invalid optional data is unknown.
    for bad in (None, [], "60", True, -1, float("inf"), float("nan"), 1 << 10000):
        assert collector.parse_heartbeat_fps_window({"wf": bad}) is None
    assert collector.parse_heartbeat_fps_window({"fps": 60}) is None
    print("PASS: native FPS windows, stalls, dropped heartbeats, live parser and legacy tape")


if __name__ == "__main__":
    main()
