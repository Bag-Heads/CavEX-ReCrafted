"""ci-1101/37: the drain long-wait "lqd_w" heartbeat attribution.

hw1101c (real-Wii perf_stress_5m) showed one server tick with wt=87449 us whose
light drain spent lqd[10]=71206 us in a mutex wait -- with no way to say WHICH
mutex or WHICH thread held it. The heartbeat now carries, only when a drain
owner waited >= 10 ms, "lqd_w":[wait_us,mutex,holder_id,holder_tag,stripe,
"name"]. This test drives the REAL collector parser and asserts that

  1. a well-formed key is parsed and surfaced in failure_context;
  2. an absent key (every quiet heartbeat, every older tape) is just "no
     attribution" -- no key, no error;
  3. a malformed key never raises and never becomes a failure;
  4. the live checkpoint print renders the lock and holder.

No hardware, no socket, no file writes besides the collector's temp output.
"""
import importlib.util
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "lqd_wait_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)

GOOD = [71206, 0x1F4A3C08, 0xDEADBEEF, 3, 1, "column_backing"]


def _hb(**extra):
    # The minimal session-valid heartbeat the collector's heartbeat branch reads.
    base = {"event": "heartbeat", "tick": 96, "profile": "perf_stress_5m",
            "run_id": "lqd-wait", "build_id": "b", "lqd_v": 3,
            "lqd": [0] * 22, "d1": [1, 0, 0, 0, 0], "player_health": 20}
    base.update(extra)
    return base


def test_parse():
    d = collector.parse_lqd_wait({"lqd_w": GOOD})
    assert d == {"wait_us": 71206, "mutex": 0x1F4A3C08, "holder_id": 0xDEADBEEF,
                 "holder_tag": 3, "stripe": 1, "name": "column_backing"}
    # Absent, or a value that names no wait, is simply no attribution.
    for ev in ({}, {"lqd_w": None}, {"lqd_w": [0, 0, 0, 0, 0, ""]},
               {"lqd_w": [0, 1, 2, 3, 4, "x"]}):
        assert collector.parse_lqd_wait(ev) is None
    # Malformed values never raise and never match.
    for bad in ([71206], [71206, 0, 0, 0, 0], [71206, 0, 0, 0, 0, "n", 9],
                [71206, -1, 0, 0, 0, "n"], [71206, 0, 0, 0, 0, 5],
                [71206, True, 0, 0, 0, "n"], "71206", True, 42, 1.5,
                [71206, 0, 0, 0, 0, None]):
        assert collector.parse_lqd_wait({"lqd_w": bad}) is None
    # Empty name is legal (unregistered stripe) -- still attributed.
    d = collector.parse_lqd_wait({"lqd_w": [71206, 7, 8, 0, 0, ""]})
    assert d["name"] == "" and d["stripe"] == 0
    print("PASS parse: good attributed; absent/malformed never raise")


def _collect(events):
    with tempfile.TemporaryDirectory(prefix="cw-lqd-wait-") as td:
        c = collector.Collector(collector._selftest_args(
            Path(td), "lqd-wait", "perf_stress_5m", expected_build="", duration=0))
        for ev in events:
            if c.done:
                break
            assert not collector.ingest_telemetry_line(c, json.dumps(ev))
        return c


def test_collector():
    # A quiet heartbeat keeps the run clean and carries no attribution.
    quiet = _collect([_hb()])
    assert quiet.lqd_wait is None
    assert quiet.failure_context()["lqd_wait"] is None

    # A heartbeat with the key surfaces it; the worst wait of the run wins.
    c = _collect([_hb(lqd_w=GOOD),
                  _hb(tick=97, lqd_w=[9000, 1, 2, 3, 0, "short"]),
                  _hb(tick=98, lqd_w=[88000, 0xAA, 0xBB, 2, 0, "region_io"])])
    assert c.lqd_wait["wait_us"] == 88000
    assert c.lqd_wait["stripe"] == 0 and c.lqd_wait["name"] == "region_io"
    assert c.failure_context()["lqd_wait"] == c.lqd_wait

    # A malformed key on an otherwise valid heartbeat changes nothing.
    m = _collect([_hb(lqd_w=[71206])])
    assert m.lqd_wait is None
    assert m.failure_class is None, m.fail_reason

    # The live print carries the lock and holder.
    line = collector.format_lqd_wait({"lqd_w": GOOD})
    assert "us:71206" in line and "column_backing" in line and "tag3" in line
    assert collector.format_lqd_wait({}) == ""
    print("PASS collector: parsed key surfaced, quiet/malformed intact")


def main():
    test_parse()
    test_collector()
    print("test_lqd_wait_collector: PASS")


if __name__ == "__main__":
    main()
