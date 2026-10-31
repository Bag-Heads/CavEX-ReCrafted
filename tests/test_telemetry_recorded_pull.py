"""Replay the recorded 502eea93 stream through the actual pull-reader loop.

The socket double supplies TCP fragments, not parsed events. Virtual time
advances at 1 Hz; --realtime also waits one wall second per recorded heartbeat.
No injected failure is represented as proof of the terminal hardware cause.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import socket
import sys
import tempfile
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("recorded_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)
FIXTURE = ROOT / "tests/fixtures/teldeath2_502eea93.ndjson"


class ReplaySocket:
    def __init__(self, records, width, stale, realtime, tail=b"", heartbeat_step=1.0):
        self.records = iter(records)
        self.pending = b""
        self.width = width
        self.now = 100.0
        self.stale = stale
        self.realtime = realtime
        self.closed = False
        self.tail = tail
        self.heartbeat_step = heartbeat_step

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.closed = True

    def settimeout(self, seconds):
        assert seconds == 0.25

    def recv(self, size):
        assert not self.closed and size == 4096
        if not self.pending:
            row = next(self.records, None)
            if row is None:
                if self.tail:
                    data, self.tail = self.tail[:self.width], self.tail[self.width:]
                    return data
                if self.stale:
                    self.now += 0.25
                    raise socket.timeout()
                return b""
            if json.loads(row)["event"] == "heartbeat":
                self.now += self.heartbeat_step
                if self.realtime:
                    time.sleep(1.0)
            self.pending = row
        data, self.pending = self.pending[:self.width], self.pending[self.width:]
        return data


def replay(width, stale=False, realtime=False, partial=False):
    records = FIXTURE.read_bytes().splitlines(keepends=True)
    events = [json.loads(row) for row in records]
    assert len(events) == 64
    # A failed libogc send may queue a prefix that is absent from the saved
    # NDJSON. Explicitly inject that unknown condition in a separate replay.
    tail = b'{"event":"heartbeat","tick"' if partial else b""
    sock = ReplaySocket(records, width, stale, realtime, tail)
    with tempfile.TemporaryDirectory() as td:
        args = collector._selftest_args(
            Path(td), events[0]["run_id"], "split_anchor_6m",
            expected_build="502eea9_09.06", connect_timeout=1,
            tcp_connect_host="socket-double", tcp_connect_port=1)
        with patch.object(collector.socket, "create_connection", return_value=sock), \
             patch.object(collector.time, "monotonic", side_effect=lambda: sock.now):
            result = collector.run_tcp_pull(args)
        report = json.loads((Path(td) / "FAIL.json").read_text())
        replayed = [json.loads(row) for row in (Path(td) / "telemetry.ndjson").read_text().splitlines()]
        assert result != 0  # deliberately no profile_done in the failed capture
        assert replayed == events, (len(replayed), report)
        assert report["heartbeats"] == 57
        assert report["failure_class"] == (
            "heartbeat_stale_after_start" if stale else "socket_closed_after_heartbeat")
        assert report["max_tel_fail"] == 0
    print(f"PASS pull replay: all 64 records / 57 heartbeats, fragment={width}, "
          f"1 Hz, partial_tail={partial}, "
          f"terminal={'open socket with no more bytes' if stale else 'explicit EOF'}")


def replay_perf_shutdown_timeout():
    # Verbatim R24 ce98952_10.09 tapes: shutdown rows were not captured.
    # Socket/clock double only; no synthetic telemetry, Wii or network.
    for run_id in ("u_r24a_s12345", "u_r24b_s12345"):
        records = (ROOT / "tmp/wii_runs" / run_id / "telemetry.ndjson").read_bytes().splitlines(keepends=True)
        events = [json.loads(row) for row in records]
        beats = sum(e.get("event") == "heartbeat" for e in events)
        sock = ReplaySocket(records, 1024, True, False, heartbeat_step=300 / beats)
        with tempfile.TemporaryDirectory() as td:
            args = collector._selftest_args(
                Path(td), run_id, "perf_stress_5m", duration=300,
                expected_build="ce98952_10.09", connect_timeout=1,
                tcp_connect_host="socket-double", tcp_connect_port=1)
            with patch.object(collector.socket, "create_connection", return_value=sock), \
                 patch.object(collector.time, "monotonic", side_effect=lambda: sock.now):
                result = collector.run_tcp_pull(args)
            report = json.loads((Path(td) / "FAIL.json").read_text())
            assert result != 0 and report["failure_class"] == "checkpoint_shutdown_timeout", report
            replayed = [json.loads(row) for row in (Path(td) / "telemetry.ndjson").read_text().splitlines()]
            assert replayed == events
            assert report["elapsed_seconds"] >= 480
        print(f"PASS pull {run_id}: retains full tape and bounded missing-shutdown FAIL")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--realtime", action="store_true")
    args = ap.parse_args()
    for width in (1, 1024, 4096):
        replay(width)
    replay(1024, stale=True, realtime=args.realtime)
    replay(1024, stale=True, partial=True)
    replay_perf_shutdown_timeout()
