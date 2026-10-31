"""Verbatim eabb36a_10.10 x tapes; diagnostic replay, never hardware PASS."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile

FIXTURES = Path(__file__).parent / "fixtures"
PINNED = {
    "x_g9a_s12345": (
        "d87ed1b6849a7f9ef29a6288c99043bb40c53c4c12047220d1358152da63ed8e",
        "63bb2efeef2cac22852f6968290d20921f35d79d047cf8132e1e81c6c41e3635",
        [1, 2, 395, 432, 7417, 7446, 7447, 7450, 7469, 7470, 7471, 7472, 7473, 7474]),
    "x_g9b_s12345": (
        "2414238dd411f04f8f681b9c1936228ab6522b5e8208e3b9ffe12c36d438dd7c",
        "1d1f96146dc3abc7831a926c5304cf256245a94f1ea2e60839e8bed1b9ce4bd7",
        [1, 3, 394, 431, 7131, 7153, 7154, 7157, 7178, 7179, 7180, 7181, 7182, 7183]),
}


def fixtures():
    manifest = json.loads((FIXTURES / "g9_x_provenance.json").read_text())
    assert set(manifest) == set(PINNED)
    for run_id, info in manifest.items():
        tape_sha, fixture_sha, source_lines = PINNED[run_id]
        assert (info["tape_sha256"], info["sha256"], info["lines"]) == (tape_sha, fixture_sha, source_lines)
        raw = (FIXTURES / info["fixture"]).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == info["sha256"]
        lines = raw.splitlines(keepends=True)
        assert len(lines) == len(info["lines"])
        assert [hashlib.sha256(line).hexdigest() for line in lines] == info["row_sha256"]
        source = Path(__file__).resolve().parents[1] / info["source"]
        if source.exists():
            tape = source.read_bytes()
            assert hashlib.sha256(tape).hexdigest() == info["tape_sha256"]
            all_lines = tape.splitlines(keepends=True)
            assert lines == [all_lines[n - 1] for n in info["lines"]]
        yield run_id, info, [line.decode() for line in lines]


def death_cases(wire, executable):
    with contextlib.redirect_stdout(io.StringIO()):
        output = subprocess.check_output([str(executable), "--completion-red"], text=True)
    wire_rows = [line + "\n" for line in output.splitlines() if line.startswith("{")]
    produced = [json.loads(line) for line in wire_rows if json.loads(line).get("event") == "heartbeat"]
    assert len(produced) == 2 and len(wire_rows) == 4
    dead, living = produced
    assert dead["player_health"] == 0 and dead["d1"][1] == 1
    assert living["player_health"] == 160 and living["d1"][1:] == [0, 0, 0, 0]
    for label, pair, expected in (("dead terminal", wire_rows[:2], "player_died"),
                                  ("guarded living terminal", wire_rows[2:], None)):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            c = wire.Collector(wire._selftest_args(Path(td), "completion-test", "perf_stress_5m"))
            for line in pair:
                wire.ingest_telemetry_line(c, line)
            assert c.failure_class == expected, (label, c.failure_class, c.fail_reason)
            if expected is None:
                assert not c.done and c.perf_survival_guard["refills"] == 1
                assert c.perf_survival_guard["minimum_health"] == 152
        print("PASS real terminal producer/formatter/parser", label)
    source = (Path(__file__).resolve().parents[1] / "source/main.c").read_text(encoding="utf-8")
    completion = source.split("if(drain_checkpoints) {", 1)[1].split("hw_telemetry_emit_profile_done(checkpoint_ok", 1)[0]
    assert completion.index("server_local_shutdown(&server)") < completion.index("s_completion_heartbeat_server = &server")
    assert completion.index("s_completion_heartbeat_server = &server") < completion.index("emit_hw_heartbeat(")
    assert "server_local_completion_heartbeat(s_completion_heartbeat_server, &hb)" in source
    for run_id, info, lines in fixtures():
        for mutation, expected in (("verbatim", "death_telemetry_unavailable"),
                                   ("living producer", None), ("dead producer", "player_died"),
                                   ("missing death", "death_telemetry_missing"),
                                   ("missing guard", "survival_guard_missing"),
                                   ("changed session", "death_telemetry_session_changed")):
            with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
                c = wire.Collector(wire._selftest_args(Path(td), run_id, "perf_stress_5m",
                    duration=300, expected_build=info["build"]))
                beats = [json.loads(line) for line in lines if json.loads(line).get("event") == "heartbeat"]
                final_tick = beats[-1]["tick"]
                for line in lines:
                    event = json.loads(line)
                    if event.get("event") == "tps_end":
                        # Retained for diagnosis only: the bounded survival
                        # excerpt does not include 278 matching TPS aggregates.
                        # Sending an unmatched seal would test a different gap.
                        continue
                    # Keep RED exact. GREEN/negative variants replace only the
                    # terminal retirement payload with real producer values;
                    # guard counters/context stay the preceding verbatim run
                    # record. These are declared schema mutations, not receipts.
                    if mutation != "verbatim" and event.get("event") == "perf_survival_guard" and event["heartbeat_tick"] == final_tick:
                        if mutation == "missing guard":
                            continue
                        prior = next(json.loads(l) for l in lines if json.loads(l).get("event") == "perf_survival_guard" and json.loads(l)["heartbeat_tick"] != final_tick)
                        event["record"] = prior["record"]
                        line = json.dumps(event) + "\n"
                    if mutation != "verbatim" and event.get("event") == "heartbeat" and event["tick"] == final_tick:
                        chosen = dead if mutation == "dead producer" else living
                        event["d1"] = [beats[0]["d1"][0], *chosen["d1"][1:]]
                        event["player_health"] = chosen["player_health"]
                        if "g1" in chosen: event["g1"] = chosen["g1"]
                        else: event.pop("g1", None)
                        if mutation == "missing death": del event["d1"]
                        if mutation == "changed session": event["d1"][0] += 1
                        line = json.dumps(event) + "\n"
                    if c.done: break  # Same first-failure close as live TCP.
                    wire.ingest_telemetry_line(c, line)
                assert c.failure_class == expected, (mutation, c.failure_class, c.fail_reason)
                if expected is None:
                    assert not c.done, "missing profile_done cannot certify completion"
                    assert c.shutdown_done is None
                    assert any("checkpoint_shutdown_us" in r for r in c.shutdown_rows)
                    assert c.shutdown_error() == "missing profile_done"
                    assert not c.service_observer_report()["coverage_complete"]
                else:
                    assert c.done
            print("PASS x-tape", run_id, mutation, expected)


def observer_cases(wire):
    for run_id, info, lines in fixtures():
        with tempfile.TemporaryDirectory() as td:
            c = wire.Collector(wire._selftest_args(Path(td), run_id, "far_idle_2m"))
            for line in lines:
                if json.loads(line).get("event") == "service_observer":
                    wire.ingest_telemetry_line(c, line)
            report = c.service_observer_report()
            assert not report["coverage_complete"] and not report["final_seen"]
            assert any(e.startswith("missing=") for e in report["errors"])
            assert any(e.startswith("overflow=") for e in report["errors"])
            assert info["observer_sequence_contiguous"]  # Full tapes, not extraction gaps.
            assert not report["receipt_latency_proven"]
        print("PASS verbatim x observer loss/final rejection", run_id)
