"""R24 lifecycle regression using verbatim ce98952_10.09 hardware tapes.

u_r24{a,b}_s12345 contain NO shutdown/profile_done rows. Do not fabricate
that missing evidence: these tests prove early completion and missing-tail
rejection. v_r24a_s12345 supplies the successful shutdown replay and four
single-row mutations. Its 2 MB tape stays external, SHA-pinned below; absence
is an explicit FAIL rather than a green skip.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch
from test_telemetry_recorded_pull import ReplaySocket

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("shutdown_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collector
spec.loader.exec_module(collector)
TAPE_HASHES = {
    "u_r24a_s12345": "5640430d36ee038b2035690346859bdc57e936e900f7c2aed4c49beaa7a9fb8e",
    "u_r24b_s12345": "2bae9db29645148828bf4c316288f7b63c1dae2ebc635949fd68771edcd57e89",
    "v_r24a_s12345": "3d46187cdd5df6b5a86371de3ff3e89dfd3323d0fbbb5fb9da844ddc33e4a4c3",
}


ProductionCollector = collector.Collector


class ShutdownReplayCollector(ProductionCollector):
    """Test-only deferral: grade the historical tail without certifying survival.

    The production death grader still evaluates every original row. Its first
    rejection is retained; enforcement is deferred until result writing solely
    so an earlier missing d1 cannot mask the independent shutdown regression.
    Neither the tape nor the production collector policy is changed.
    """
    def note_death_telemetry(self, event):
        problem = super().note_death_telemetry(event)
        if problem and not getattr(self, "survival_problem", None):
            self.survival_problem = problem
        return None

    def write_result(self):
        problem = getattr(self, "survival_problem", None)
        assert problem, "historical tape unexpectedly supplied survival evidence"
        self.death_telemetry["historical_survival_rejection"] = {
            "reason": problem[0], "failure_class": problem[1],
            "claim": "no authoritative death evidence",
        }
        if self.pass_reason:
            self.death_telemetry["shutdown_verdict_reason"] = self.pass_reason
            self.pass_reason = None
            self.fail(*problem)
        return super().write_result()


def replay_success_and_mutations():
    run_id = "v_r24a_s12345"
    tape = ROOT / "tmp/wii_runs" / run_id / "telemetry.ndjson"
    assert tape.is_file(), f"FAIL: external SHA-pinned fixture absent: {tape}"
    raw = tape.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == TAPE_HASHES[run_id]
    records = raw.splitlines(keepends=True)
    events = [json.loads(row) for row in records]
    # Verbatim ce98952_10.09 real-Wii tape: shutdown line 1727, done line 1729.
    shutdown = [i for i, e in enumerate(events) if "checkpoint_shutdown_us" in e]
    done = [i for i, e in enumerate(events) if e.get("event") == "profile_done"]
    assert shutdown == [1726] and done == [1728]
    assert events[shutdown[0]]["checkpoint_shutdown_us"] == 2179
    assert events[shutdown[0]]["ok"] == 1 and events[done[0]]["status"] == "ok"
    beats = sum(e.get("event") == "heartbeat" for e in events)
    assert beats == 297
    cases = (
        ("verbatim_success", None, None, None, None),
        ("missing_checkpoint_shutdown_us", shutdown[0], None,
         "checkpoint_shutdown_timeout", "missing checkpoint_shutdown_us"),
        ("missing_profile_done", done[0], None,
         "checkpoint_shutdown_timeout", "missing profile_done"),
        ("corrupt_checkpoint_shutdown", shutdown[0],
         (b'"checkpoint_shutdown_us":2179', b'"checkpoint_shutdown_us":-1'),
         "checkpoint_shutdown_failed", "invalid checkpoint_shutdown_us"),
        ("profile_done_fail", done[0], (b'"status":"ok"', b'"status":"fail"'),
         "checkpoint_shutdown_failed", "profile_done status='fail'"),
    )
    for name, index, replacement, failure_class, reason in cases:
        rows = records.copy()
        if index is not None:
            if replacement is None:
                del rows[index]
                assert rows == records[:index] + records[index + 1:]
            else:
                old, new = replacement
                assert rows[index].count(old) == 1
                rows[index] = rows[index].replace(old, new, 1)
                assert [i for i, (a, b) in enumerate(zip(records, rows)) if a != b] == [index]
        # Death enforcement is deferred by the explicit test-only replay
        # collector; parsing, shutdown grading and TCP fragmentation stay real.
        # Keep the socket open after the tape so missing-tail cases exercise
        # the bounded shutdown deadline, rather than an unrelated EOF gate.
        sock = ReplaySocket(rows, 1024, True, False, heartbeat_step=300 / beats)
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            out = Path(td)
            args = collector._selftest_args(
                out, run_id, "perf_stress_5m", duration=300,
                expected_build="ce98952_10.09", connect_timeout=1,
                tcp_connect_host="socket-double", tcp_connect_port=1)
            with patch.object(collector.socket, "create_connection", return_value=sock), \
                    patch.object(collector, "Collector", ShutdownReplayCollector), \
                    patch.object(collector.time, "monotonic", side_effect=lambda: sock.now):
                result = collector.run_tcp_pull(args)
            passed = failure_class is None
            assert result != 0, name
            report = json.loads((out / "FAIL.json").read_text())
            assert not (out / "PASS.json").exists(), name
            assert report["death_telemetry"]["historical_survival_rejection"]["claim"] == "no authoritative death evidence"
            assert (out / "telemetry.ndjson").read_text().splitlines() == [
                row.decode("utf-8").rstrip() for row in rows], name
            if passed:
                assert report["failure_class"] == "death_telemetry_missing", report
                assert report["death_telemetry"]["shutdown_verdict_reason"] == "profile_done received; checkpoint shutdown drained", report
                assert report["checkpoint_shutdown"]["device_reconciled"], report
                assert report["checkpoint_shutdown"]["profile_done"]["status"] == "ok"
            else:
                assert report["failure_class"] == failure_class, (name, report)
                assert reason in report["reason"], (name, report)
                if replacement is None:
                    other = "profile_done" if index == shutdown[0] else "checkpoint_shutdown_us"
                    assert f"missing {other}" not in report["reason"], (name, report)
        print(f"PASS {name}: {'shutdown reconciled; survival FAIL (no authoritative death evidence)' if passed else failure_class + ': ' + reason}")


def main():
    runner = (ROOT / "tools/run_wii_hardware.ps1").read_text()
    assert 'if($Profile -eq "perf_stress_5m") { $collectorTimeout += 180 }' in runner
    assert collector.CHECKPOINT_SHUTDOWN_WAIT_SECONDS == 180
    for run_id in ("u_r24a_s12345", "u_r24b_s12345"):
        tape = ROOT / "tmp/wii_runs" / run_id / "telemetry.ndjson"
        assert hashlib.sha256(tape.read_bytes()).hexdigest() == TAPE_HASHES[run_id]
        lines = tape.read_text(encoding="utf-8").splitlines(True)
        events = [json.loads(line) for line in lines]
        assert not any(e.get("event") == "profile_done" or "checkpoint_shutdown_us" in e for e in events)
        now = [100.0]
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()), \
                patch.object(collector.time, "monotonic", side_effect=lambda: now[0]):
            c = ShutdownReplayCollector(collector._selftest_args(
                Path(td), run_id, "perf_stress_5m", duration=300,
                expected_build="ce98952_10.09"))
            for line in lines:
                assert not c.done
                collector.ingest_telemetry_line(c, line)
            assert not c.done, c.fail_reason
            now[0] = 400.0
            c.last_hb = now[0]  # isolate the duration gate, no fake wire row
            c.tick()
            assert not c.done, "duration-only PASS drops the shutdown tail"
            now[0] = 580.0
            c.tick()
            assert c.done and c.failure_class == "checkpoint_shutdown_timeout", c.fail_reason
            assert "profile_done" in c.fail_reason and "checkpoint_shutdown_us" in c.fail_reason
            assert c.write_result() != 0
            report = json.loads((Path(td) / "FAIL.json").read_text())
            assert report["checkpoint_shutdown"]["profile_done"] is None
            assert report["checkpoint_shutdown"]["rows"] == []
            assert report["checkpoint_shutdown"]["last_counters"]["ckio"] == next(
                e["ckio"] for e in reversed(events) if "ckio" in e)
            assert (Path(td) / "telemetry.ndjson").read_text().splitlines() == [line.rstrip() for line in lines]
        print(f"PASS {run_id}: duration stays open; absent lifecycle tail FAILs (no invented rows)")
    replay_success_and_mutations()


if __name__ == "__main__":
    main()
