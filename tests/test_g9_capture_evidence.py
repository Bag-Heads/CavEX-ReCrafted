"""Synthetic bounded-window collector and production recorder replay."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from g9_capture_evidence import G9CaptureEvidence, render_g9_capture
import wii_telemetry_server as collector


def row(kind, tb, *data, drain=0, generation=0):
    return [kind, tb >> 32, tb & 0xffffffff, drain, generation,
            *data, *([0] * (15 - len(data)))]


def test_capture(executable=None):
    # Cross a TB low-word wrap. All categories and tail boundaries are paired
    # within ONE tick; checkpoint stages are included in that full interval.
    t = 0xffffffff - 500
    rows = [row(1, t, 12, 1 << 10, 10),
        row(4, t+10, 1, 0, 42, 99),
        row(2, t+1000, 12, 2, 63, 63, 2, 0, 0, 0, 10, 1234, 0, 0, 1 << 10),
        row(4, t+1010, 2, 0, 42, 99, 1000),
        row(11, t+1100, 1, 0, 2), row(11, t+1200, 2, 0, 2),
        row(5, t+1500, 0, 42, 99, 1, 2, drain=1),
        row(10, t+1501, 1, 0, 0, 0, 1, 1, 1, drain=1, generation=7),
        row(10, t+1502, 2, 0, 0, 0, 1, 1, 3, drain=1, generation=7),
        row(2, t+2000, 11, 1, 64, 64, 2, 0, 0, 0, 10, 5678, 0, 0, 1 << 10, drain=1, generation=7),
        row(3, t+2500, 10, 2, 1, 0, drain=1, generation=7),
        row(3, t+2501, 11, 1, 0, 1 << 11, drain=1, generation=7),
        row(2, t+3000, 11, 1, 64, 64, 4, 0, 123, 456, 8, 5678, 0, 0, 1 << 11, drain=1, generation=7),
        row(3, t+3201, 11, 2, 1, 0, drain=1, generation=7),
        row(10, t+4000, 3, 4, 1, 8000, 1, 1, 5, 100, drain=1, generation=7),
        row(10, t+4001, 4, 4, 1, 0, 1, 1, 5, drain=1, generation=7),
        # Missed IRQ deadlines cannot be stretched into a 3ms CPU estimate.
        row(2, t+6000, 12, 2, 65, 63, 2, 0, 0, 0, 8, 1234, 2000, 2, 0, drain=1, generation=7),
        row(10, t+6001, 5, 0, 1, 100, 1, 1, 1, drain=1, generation=7),
        row(6, t+6100, 100, 0, (t+6099) >> 32, (t+6099) & 0xffffffff, drain=1, generation=7),
        row(7, t+6200, drain=1, generation=7), row(8, t+6300, drain=1, generation=7),
        row(14, t+6500, 5000, drain=1, generation=7), row(9, t+6600, drain=1, generation=7),
        row(12, t+10000, 1000, 6000, 1, 4)]
    events = [dict(event="g9_capture", v=1, tb_hz=1000000, tick=123,
                   offset=i, total=len(rows), rt_max=1000, drain_max=5000,
                   pop_max=100, checkpoint_tick_us=10000, incomplete=0, rows=rows[i:i+8]) for i in range(0, len(rows), 8)]
    seal = dict(event="g9_capture_seal", v=1, candidates=99, saturated=0,
                capacity=4096, slots=9, ticks=[123])
    p = G9CaptureEvidence()
    for e in events: p.note(e)
    assert not p.report()["valid"]  # no completion seal
    p.note(seal)
    report = p.report(); assert report["valid"], report
    w = report["windows"][0]
    assert w["whole_tick_us"] == 10000 and w["unattributed_us"] == 6000
    assert w["state_est_us"] == dict(server_compute=2000, runnable_wait=1000, blocked=1000)
    assert w["main_occupancy_us"] == dict(render=2500, input=700)
    assert w["irq"] == dict(late_samples=1, max_late_us=2000, missed_deadlines=6, runtime_us=None, sampler_irq_us=0, max_sampler_irq_us=0)
    assert w["paired_components"] == dict(rt_us=1000, wt_us=6000, valid=True, tail_missed=4)
    assert w["checkpoints"][0]["elapsed_us"] == 100
    d = w["drains"][0]
    assert (d["leases"], d["drops"], len(d["ACKs"])) == (1, dict(timer=1), 1)
    assert d["pre_restore_us"] == 4600 and d["restore_tail_us"] == 100
    assert d["cleanup_us"] == 100 and d["record_mutex_tail_us"] == 200 and d["record_return_tail_us"] == 100
    assert "leases=1" in render_g9_capture(report) and "IRQ runtime unknown" in render_g9_capture(report)
    # PPC uses the same physical intervals and identical CPU estimates.
    ppc = G9CaptureEvidence()
    for e in events:
        converted = []
        for r in e["rows"]:
            r = r.copy(); tb = ((r[1] << 32) | r[2]) * 6075 // 100
            r[1], r[2] = tb >> 32, tb & 0xffffffff
            if r[0] == 6:
                pop = ((r[7] << 32) | r[8]) * 6075 // 100
                r[7], r[8] = pop >> 32, pop & 0xffffffff
            converted.append(r)
        ppc.note({**e, "tb_hz": 60750000, "rows": converted})
    ppc.note(seal)
    assert ppc.report()["valid"] and abs(ppc.report()["windows"][0]["whole_tick_us"] - 10000) < .02
    for damaged in (events[1:], [events[0], events[0], *events[1:]],
                    [{**events[0], "rows": [[True] * 20]}, *events[1:]]):
        q = G9CaptureEvidence()
        for e in damaged: q.note(e)
        q.note(seal); assert not q.report()["valid"]
    q = G9CaptureEvidence()
    malformed = [r.copy() for r in rows]
    malformed[10][5] = 0xffffffff  # reject before an enormous bit shift
    for i in range(0, len(rows), 8):
        q.note({**events[i//8], "rows": malformed[i:i+8]})
    q.note(seal); assert not q.report()["valid"]
    q = G9CaptureEvidence()
    for e in events: q.note(e)
    q.note({**seal, "ticks": [123, 124]}); assert not q.report()["valid"]
    with tempfile.TemporaryDirectory() as td:
        out = Path(td)
        c = collector.Collector(collector._selftest_args(out / "collector", "r21", "heartbeat_selftest"))
        for e in [*events, seal]: c.process({**e, "run_id": "r21"})
        assert json.loads((c.out / "g9_capture.json").read_text())["valid"]
        assert "leases=1" in (c.out / "g9_capture_table.md").read_text()
        assert c.hb_count == 0 and not c.pass_reason
        c.process({**seal, "run_id": "stale"}); assert not c.g9_capture.errors
        if executable:
            tape = out / "recorder.ndjson"
            subprocess.run([executable, str(tape)], check=True, capture_output=True)
            batches = [json.loads(line) for line in tape.read_text().splitlines()
                       if line.startswith('{"event":"g9_capture')]
            recorder = G9CaptureEvidence()
            for e in batches:
                recorder.note(e)
                if e["event"] == "g9_capture_seal":
                    r = recorder.report()
                    if not e["saturated"]:
                        assert r["valid"], r
                        assert set(e["ticks"]) == {1, 2, 3, 4, 5, 8, 9, 10}
                    else:
                        assert not r["valid"] and r["seal_matches"]
                        assert len(recorder.windows[1]["rows"]) == 4096
                    recorder = G9CaptureEvidence()
    print("G9 capture collector: synthetic/SDK TB, paired tick/checkpoint/tails, lease counts, wait/IRQ gaps, MAIN, missing/retry/truncation PASS")


if __name__ == "__main__":
    test_capture(sys.argv[1] if len(sys.argv) > 1 else None)
