import importlib.util
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("analyze_settle", Path(__file__).parents[1] / "tools/analyze_settle.py")
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)


def event(us, stage, job=1, x=0, order=0):
    return m.Event(us, 1, job, x, 0, 1, 1, 0, m.STAGES.index(stage), order)


class SettleReader(unittest.TestCase):
    def summary(self, seq=0, closed=1, size=438):
        raw = bytearray(size)
        m.struct.pack_into("<I", raw, 0, 7)
        m.struct.pack_into("<IIiiIHBBHBB", raw, 328,
                           closed, 10 if closed else 0, 0, 0, 1, 1, 0, 0,
                           closed, closed, 7 if closed else 3)
        if closed:
            m.struct.pack_into("<BII", raw, 356, 63, 9, 10)
            m.struct.pack_into("<Q", raw, 428, 100)
        return dict(event="settle", v=2, seq=seq, b=m.base64.b64encode(raw).decode())

    def test_v2_closed_path_and_deltas(self):
        rows = [self.summary(), self.summary(1, 0)]
        text = m.report(rows)
        self.assertIn("VALID settle v2", text)
        self.assertEqual(text.count("Closed episodes="), 1)
        self.assertIn("unattributed id=9 us=10", text)
        self.assertIn("dirty_install: 14", text)

    def test_v2_teardown_diagnostics(self):
        # The two bytes the reset-stall rerun is read by: a carried retirement
        # and the last drain's cost. Saturating, so 255 reads as "at least".
        quiet = self.summary()
        loud = bytearray(m.base64.b64decode(self.summary(1)["b"]))
        m.struct.pack_into("<BB", loud, 436, 3, 200)
        row = dict(event="settle", v=2, seq=1,
                   b=m.base64.b64encode(loud).decode())
        text = m.report([quiet, row])
        self.assertIn("VALID settle v2", text)
        self.assertIn("Teardown: deferred_retirements=3 last_drain_ms=200 "
                      "worst_drain_ms=200", text)

    def test_v2_accepts_pre_diagnostics_436_byte_capture(self):
        # BACK-COMPAT: every hardware capture taken before the teardown fields
        # existed is 436 B. Rejecting it made those captures unreadable, which
        # is exactly the reruns this lane needs to compare against.
        old = self.summary(size=436)
        text = m.report([old])
        self.assertIn("VALID settle v2", text)
        self.assertIn("dirty_install: 7", text)
        # Absent, and reported as absent -- never silently rendered as 0/0.
        self.assertIn("Teardown: not reported by 1/1 record(s)", text)
        self.assertNotIn("deferred_retirements=", text)
        # Mixed capture: the new record still reports, the old one is named.
        new = bytearray(m.base64.b64decode(self.summary(1)["b"]))
        m.struct.pack_into("<BB", new, 436, 1, 12)
        mixed = m.report([old, dict(event="settle", v=2, seq=1,
                                    b=m.base64.b64encode(new).decode())])
        self.assertIn("Teardown: deferred_retirements=1 last_drain_ms=12 "
                      "worst_drain_ms=12", mixed)
        self.assertIn("Teardown: not reported by 1/2 record(s)", mixed)
        # 437 is not a form that was ever emitted.
        bad = self.summary(size=437)
        self.assertIn("INVALID", m.report([bad]))

    def test_v2_rejects_damage(self):
        for field, value in (("b", "!"), ("seq", 2), ("v", 3)):
            row = self.summary()
            row[field] = value
            # Keep v2 detection for the mixed-version case.
            text = m.report([self.summary(), row])
            self.assertIn("INVALID", text)
        row = self.summary()
        raw = bytearray(m.base64.b64decode(row["b"]))
        m.struct.pack_into("<I", raw, 252, 1)
        row["b"] = m.base64.b64encode(raw).decode()
        self.assertIn("reconciliation failed", m.report([row]))

    def test_v2_exact_envelope_bound(self):
        raw = bytearray(b"\xff" * 438)
        # Production maximum: all 63 histogram and 19 reconciliation counters,
        # every identity/time field, all eight path hops, and both saturated
        # teardown-diagnostic bytes are populated. 436..438 all base64 to 584
        # chars, so the two diagnostics cost the envelope nothing -- 439 would
        # cost four chars and push the record past the 1,024 B budget below.
        raw[354] = 8
        for offset in range(356, 428, 9):
            raw[offset] = len(m.STAGES) - 1
        row = dict(event="settle", v=2, seq=4294967295,
                   b=m.base64.b64encode(raw).decode())
        body = m.json.dumps(row, separators=(",", ":"))
        self.assertEqual(len(body), 632)
        # hw_telemetry_emit_observer adds the 63-byte run ID; JSON control
        # characters are its worst six-byte escaping case. Include the LF sent
        # after each NDJSON record.
        row["run_id"] = "\x01" * 63
        record_bytes = len(m.json.dumps(row, separators=(",", ":"))) + 1
        self.assertEqual(record_bytes, 1023)
        self.assertLessEqual(record_bytes, 1024)
        self.assertLess(record_bytes, 9216)  # heartbeat-sized sender slot + NUL

    def test_last_blocker_not_overlapping_totals(self):
        begin = event(0, "ep_begin")
        # Job 2 is slower but a different chunk; it is not the final blocker.
        events = [begin, event(1, "gather", 2, 1), event(2, "submit", 2, 1),
                  event(3, "worker", 2, 1), event(5, "gather"), event(10, "submit"),
                  event(20, "worker"), event(25, "emit_begin"),
                  event(30, "vl_begin"), event(40, "emit_begin"),
                  event(45, "sleep_begin"), event(50, "sleep_end"),
                  event(55, "fin_begin"), event(60, "bfs_begin"),
                  event(65, "complete", 2, 1), event(70, "complete"),
                  event(80, "consume"), event(90, "accept"), event(100, "ep_end")]
        events = [m.Event(e.us,e.frame,e.job_id,e.cx,e.cz,e.generation,e.epoch,e.cy,e.flags,i)
                  for i,e in enumerate(events)]
        path = m.critical_path(events, events[0], events[-1])
        self.assertEqual(sum(b-a for a,b,_ in path), 100)
        self.assertIn((30,40,"vertex_light"), path)
        self.assertIn((45,50,"worker_sleep"), path)
        self.assertIn((10,20,"worker_queue"), path)
        self.assertEqual(path[-1], (90,100,"unattributed"))

    def test_superseded_job_blocks_successor(self):
        stages = [(0,"ep_begin",9),(1,"submit",1),(2,"worker",1),
                  (3,"dirty_light",2),(4,"gate_inflight",2),
                  (8,"complete",1),(9,"consume",1),(10,"stale",1),
                  (11,"gather",2),(12,"submit",2),(13,"worker",2),
                  (15,"complete",2),(16,"consume",2),(17,"accept",2),(20,"ep_end",2)]
        events=[event(t,st,job,order=i) for i,(t,st,job) in enumerate(stages)]
        path=m.critical_path(events,events[0],events[-1])
        self.assertIn((4,8,"worker_other"),path)
        self.assertEqual(sum(b-a for a,b,_ in path),20)

    def test_cancel_and_open(self):
        events=[event(0,"ep_begin"),event(5,"ep_cancel"),event(6,"ep_begin",2)]
        self.assertEqual([x[2] for x in m.episodes(events)],["cancelled","unfinished"])

    def test_overflow_and_gap_invalidate(self):
        raw=m.WIRE.pack(0,0,0,0,0,0,0,0,0).hex()
        _,errors=m.decode([{"event":"settle","v":1,"seq":2,"hex":raw},
                           {"event":"settle_counts","valid":0,"overflow":1,"lost":0}])
        self.assertTrue(any("sequence" in e for e in errors))
        self.assertTrue(any("overflow=1" in e for e in errors))

    def test_legacy_capture_is_not_attribution(self):
        self.assertIn("INVALID",m.report([{"event":"heartbeat"}]))


if __name__ == "__main__":
    unittest.main()
