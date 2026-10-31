"""Run-bound TNT evidence gate through the production collector."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import wii_telemetry_server as telemetry  # noqa: E402


class TntEvidenceTest(unittest.TestCase):
    @staticmethod
    def beat(st=2, **changes):
        row = dict(event="heartbeat", run_id="tnt-run",
                   profile="heartbeat_selftest", build_id="abc_06.27",
                   tick=10, fps=60, tnt_valid=1, tnt_st=st,
                   tnt_bits=7 if st == 2 else 0, tnt_primed_left=0,
                   tnt_carved=8 if st == 2 else 0,
                   tnt_pad=75 if st == 2 else 0, tnt_gen=2,
                   tnt_tick=700 if st in (2, 3) else 1 if st == 1 else 100)
        row.update(changes)
        return row

    def grade(self, rows, route="direct"):
        with tempfile.TemporaryDirectory() as td:
            args = telemetry._selftest_args(Path(td), "tnt-run",
                                                "heartbeat_selftest",
                                                require_tnt_evidence=True)
            c = telemetry.Collector(args)
            c.world_loaded_seen = True
            for row in rows:
                c.process(row)
                if c.done:
                    break
            c.hb_count = max(c.hb_count, c.profile["min_hb"])
            if not c.done:
                if route == "profile_done":
                    c.process(dict(event="profile_done", run_id="tnt-run",
                                   status="ok"))
                elif route == "tps_seal":
                    c.tps_profile_done = True
                    c.process(dict(event="tps_end", run_id="tnt-run",
                                   records=0, stream_valid=True))
                else:
                    c._finish_normal("duration reached")
            artifact = Path(td) / ("PASS.json" if c.pass_reason else "FAIL.json")
            return c.pass_reason, c.failure_class, artifact.exists()

    def test_required_but_absent_verdict_fails(self):
        with tempfile.TemporaryDirectory() as td:
            args = telemetry._selftest_args(Path(td), "tnt-red",
                                                "heartbeat_selftest",
                                                require_tnt_evidence=True)
            collector = telemetry.Collector(args)
            collector.hb_count = collector.profile["min_hb"]
            collector.world_loaded_seen = True
            collector._finish_normal("test completion")
            self.assertEqual(collector.failure_class, "tnt_evidence_failed")
            self.assertFalse((Path(td) / "PASS.json").exists())

    def test_valid_terminal_passes_each_completion_route(self):
        for route in ("direct", "profile_done", "tps_seal"):
            with self.subTest(route=route):
                passed, failure, _ = self.grade(
                    [self.beat(st=0, tnt_bits=0, tnt_carved=0,
                               tnt_pad=0, tnt_tick=0),
                     self.beat(st=1, tnt_bits=0, tnt_carved=0,
                               tnt_pad=0, tnt_tick=1), self.beat()], route)
                self.assertIsNotNone(passed)
                self.assertIsNone(failure)

    def test_terminal_fail_skip_and_incomplete_rejected(self):
        for rows in ([self.beat(st=3)], [self.beat(st=4)],
                     [self.beat(st=1)], [self.beat(),
                                        self.beat(tnt_valid=0)]):
            with self.subTest(rows=rows):
                passed, failure, _ = self.grade(rows)
                self.assertIsNone(passed)
                self.assertEqual(failure, "tnt_evidence_failed")

    def test_terminal_may_repeat_but_may_not_change(self):
        passed, failure, _ = self.grade([self.beat(), self.beat()])
        self.assertIsNotNone(passed)
        self.assertIsNone(failure)
        for changed in (self.beat(tnt_gen=3),
                        self.beat(tnt_carved=9),
                        self.beat(st=1, tnt_bits=0, tnt_carved=0,
                                  tnt_pad=0, tnt_tick=1),
                        self.beat(st=3, tnt_bits=6)):
            with self.subTest(changed=changed):
                passed, failure, _ = self.grade([self.beat(), changed])
                self.assertIsNone(passed)
                self.assertEqual(failure, "tnt_evidence_failed")

    def test_numeric_and_identity_requirements(self):
        broken = [self.beat(tnt_bits=6), self.beat(tnt_primed_left=1),
                  self.beat(tnt_carved=7), self.beat(tnt_carved=76),
                  self.beat(tnt_gen=-1), self.beat(tnt_valid=True),
                  self.beat(profile="far_idle_2m"),
                  self.beat(build_id="wrong"), self.beat(build_id=1),
                  self.beat(run_id="wrong")]
        missing = self.beat()
        del missing["tnt_pad"]
        broken.append(missing)
        for beat in broken:
            with self.subTest(beat=beat):
                passed, failure, _ = self.grade([beat])
                self.assertIsNone(passed)
                self.assertEqual(failure, "tnt_evidence_failed")

    def test_expected_dump_cannot_bypass_terminal_pass(self):
        for state in (1, 2):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as td:
                args = telemetry._selftest_args(Path(td), "tnt-run",
                                                    "watchdog_inject",
                                                    require_tnt_evidence=True,
                                                    expected_build="abc_06.27")
                collector = telemetry.Collector(args)
                beat = self.beat(st=state, profile="watchdog_inject")
                collector.process(beat)
                collector.hb_count = collector.profile["min_hb"]
                collector.process(dict(event="watchdog_dump", run_id="tnt-run",
                                       cause="HANG", dump_id=1, seq=0,
                                       last=True, text="HANG evidence\n"))
                if state == 1:
                    self.assertIsNone(collector.pass_reason)
                    self.assertEqual(collector.failure_class,
                                     "tnt_evidence_failed")
                    self.assertFalse((Path(td) / "PASS.json").exists())
                else:
                    self.assertIsNotNone(collector.pass_reason)
                    self.assertIsNone(collector.failure_class)


if __name__ == "__main__":
    unittest.main()
