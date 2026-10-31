"""R7 production collector, including replay of the real C recorder output."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from g9_cpu_evidence import G9CpuEvidence, render_g9_cpu
import wii_telemetry_server as collector


def row(seq, kind, span, tb, *data, lost=0):
    return [seq, lost, kind, span, tb >> 32, tb & 0xffffffff, *data,
            *([0] * (18 - len(data)))]


def event(rows, hz=1000000, **kwargs):
    return dict(event="g9_cpu", v=1, tb_hz=hz, rows=rows, **kwargs)


def main():
    from test_g9_capture_evidence import test_capture
    test_capture(sys.argv[1] if len(sys.argv) > 1 else None)
    p = G9CpuEvidence()
    heartbeat = dict(event="heartbeat", g9p_v=1,
        g9p=[1, 500, 1, 16, 7, 5, 2, 0, 3, 1, 1, 1, 1, 1400, 1400, 0, 0, 0, 1, 0, 0, 0], tick=20)
    assert collector.g9_pacing_record(heartbeat)["sleeps"] == 1
    p.note(heartbeat)
    p.note(event([row(1, 6, 0, 100, 2, 12, 1, 1400, 15)]))
    assert p.report()["pacing"]["phase_counts"] == {"lighting": 1}
    assert p.report()["pacing"]["valid"]
    assert "lighting" in render_g9_cpu(p.report())
    for bad in ([True] * 22, [0] * 21, [0] * 22):
        p.note(dict(event="heartbeat", g9p_v=1, g9p=bad))
    p.note({**heartbeat, "g9p": [*heartbeat["g9p"][:11], 0, *heartbeat["g9p"][12:]]})
    assert len(p.errors) == 4 and not p.report()["pacing"]["valid"]
    p.note(event([row(2, 6, 0, 200, 99, 12, 1, 1000, 15)]))
    assert p.errors[-1] == "invalid pacing sleep"
    off_p = G9CpuEvidence(); off_p.note({**heartbeat, "g9p": [0, 2000, 1, 8, 10, 8, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]})
    assert off_p.report()["pacing"]["heartbeats"][-1]["foreign"] == 8
    # Cross the low-word rollover; priority inheritance is preserved in tape.
    t = 0xffffffff - 1000
    rows = [row(1, 5, 0, t, 11, 1), row(2, 5, 0, t, 13, 4),
            row(3, 1, 1, t, 12, 1, 0, 0, 2, 0, 0, 100, 7, 8, 1),
            row(4, 2, 1, t+1000, 12, 2, 63, 63, 2, 0, 0, 0, 4, 0x80000100, 0, 0),
            row(5, 2, 1, t+2000, 11, 1, 64, 64, 2, 0, 0, 0, 6, 0x80000200, 0, 0),
            row(6, 2, 1, t+5000, 13, 4, 65, 48, 4, 0, 42, 1, 9, 0x80000300, 2000, 2),
            row(7, 3, 1, t+5700, 12530, 5700, 3, 1, 2, 1, 0, 2, 500, 9, 9, 0)]
    m = G9CpuEvidence(); m.note(event(rows))
    s = m.report()["spans"][0]
    assert s["complete"] and s["pair_valid"]
    assert s["state_est_us"] == dict(server=1000, runnable_off_cpu=1000, blocked=1000)
    assert s["unattributed_us"] == 2700 and s["missed"] == 2
    assert s["submissions"] == 2 and s["stales"] == 1
    assert s["priority_samples"][-1]["effective"] == 65
    assert s["priority_samples"][-1]["base"] == 48
    assert s["cpu_est_us"]["sender@0000000d"] == 1000
    assert "12530" in render_g9_cpu(m.report())
    m.note(event([row(8, 4, 0, t+6000, 1, 1, 12, 0),
                  row(9, 4, 0, t+7000, 1, 2, 11, 44),
                  row(10, 4, 0, t+8000, 1, 2, 11, 45),
                  row(11, 4, 0, t+10000, 1, 3, 12, 45)]))
    w = m.report()["rpc"][0]
    assert w["consumer_us"] == 1000 and w["post_return_us"] == 3000
    assert w["matching_token_post_return_us"] == 2000
    # SDK timebase conversion yields the identical RPC interval.
    ppc = G9CpuEvidence()
    ppc.note(event([row(1, 5, 0, 0, 11, 1),
                   row(2, 4, 0, 0, 1, 1, 12, 0),
                   row(3, 4, 0, 60750, 1, 2, 11, 44),
                   row(4, 4, 0, 243000, 1, 3, 12, 44)], 60750000))
    assert ppc.report()["rpc"][0]["post_return_us"] == 3000
    off = G9CpuEvidence()
    off.note(event([row(1, 1, 1, 0, 12, 0, 1, 0),
                    row(2, 3, 1, 9000, 12530, 9000, 0, 0, 0)]))
    assert off.report()["spans"][0]["unattributed_us"] == 9000
    assert not off.report()["spans"][0]["cpu_est_us"]
    damaged = G9CpuEvidence()
    damaged.note(event(rows[:3], produced=12, lost=2))
    damaged.note(event([row(7, 3, 1, t+5700, 12530, 5700, 3, 1, 2, lost=2)], produced=12, lost=2))
    r = damaged.report()
    assert r["sequence_gaps"] == 8 and r["lost"] == 2
    assert not r["spans"][0]["pair_valid"]
    damaged.note(event([rows[0]]))
    damaged.note(event([[True] * 24]))
    damaged.note(dict(event="g9_cpu", v=2))
    assert len(damaged.report()["errors"]) == 3
    missing = G9CpuEvidence(); missing.note(event(rows[:3]))
    assert not missing.report()["spans"][0]["complete"]
    trace = G9CpuEvidence()
    trace.note(event([row(1, 7, 0, t, 1, 0, 1),
        row(2, 7, 7, t+100, 1, 4, 1, 0, 4000, 120, 4, 64, 64),
        row(3, 7, 7, t+2500, 1, 4, 2, 4096, 2000, 2520, 4, 64, 64),
        row(4, 7, 0, t+4000, 1, 0, 2)]))
    tr = trace.report()["main_trace"]
    assert tr["valid"] and len(tr["intervals"]) == 2
    assert tr["intervals"][0]["elapsed_us"] == 2400
    assert tr["intervals"][0]["end"]["work"] == 4096
    assert tr["intervals"][0]["begin"]["effective"] == 64
    incomplete = G9CpuEvidence(); incomplete.note(event([row(1, 7, 0, t, 1, 0, 1)]))
    assert not incomplete.report()["main_trace"]["valid"]
    for bad in ([row(1, 7, 0, t, 1, 11, 1)],
        [row(1, 7, 0, t, 1, 0, 3)], [row(1, 7, 0, t, 1, 0, 2)],
        [row(1, 7, 0, t, 1, 0, 1), row(2, 7, 0, t+1, 1, 0, 1)]):
        q = G9CpuEvidence(); q.note(event(bad)); assert q.errors
    regressed = G9CpuEvidence()
    regressed.note(event([row(1, 7, 0, t, 2, 0, 1),
        row(2, 7, 0, t+1, 1, 0, 1)]))
    assert regressed.errors == ["invalid MAIN boundary"]
    unknown = G9CpuEvidence()
    unknown.note(event([row(1, 7, 0, t, 1, 3, 1, 0, 1, 0xffffffff),
        row(2, 7, 0, t+1, 1, 3, 2)]))
    assert not unknown.report()["main_trace"]["valid"]
    with tempfile.TemporaryDirectory() as td:
        out = Path(td)
        c = collector.Collector(collector._selftest_args(out / "collect", "r7", "heartbeat_selftest"))
        c.process({**event(rows), "run_id": "r7"})
        assert (c.out / "g9_cpu_table.md").is_file()
        assert json.loads((c.out / "g9_cpu.json").read_text())["spans"][0]["pair_valid"]
        assert c.hb_count == 0 and not c.pass_reason  # diagnostics cannot certify Wii
        c.process({**event(rows), "run_id": "stale"})
        assert not c.g9_cpu.errors  # stale run filtered before diagnostic parser
        c.process(dict(event="g9_cpu", v=1, tb_hz=1000000, rows=None, run_id="r7"))
        assert c.g9_cpu.errors == ["invalid envelope or timebase"]
        c.process({**heartbeat, "run_id": "r7"})
        assert (c.out / "g9_pacing_records.ndjson").is_file()
        c.write_g9_cpu()
        assert json.loads((c.out / "g9_cpu.json").read_text())["pacing"]["heartbeats"][0]["slice_us"] == 500
        if len(sys.argv) > 1:
            tape = out / "pc.ndjson"
            subprocess.run([sys.argv[1], str(tape)], check=True)
            actual = G9CpuEvidence()
            for line in tape.read_text().splitlines(): actual.note(json.loads(line))
            result = actual.report()
            assert not result["errors"], result["errors"]
            assert len(result["spans"]) == 2 and all(s["pair_valid"] for s in result["spans"])
            assert result["lost"] == 8 and result["sequence_gaps"] == 7
            assert result["spans"][1]["samples"] == 2
            assert result["pacing"]["phase_counts"] == {"rpc": 1}
            assert len(result["main_trace"]["intervals"]) == 4
            assert not result["main_trace"]["incomplete"]
    print("G9 collector: CPU ownership/state/priority, RPC, sampler off, rollover/loss/malformed/partial/stale tapes PASS")


if __name__ == "__main__":
    main()
