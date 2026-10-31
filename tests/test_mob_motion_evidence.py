"""mob_motion_2m evidence gate through the production collector (lane C,
mob motion units S2 proof). Every check the fixture records has a bad trace
here that must FAIL, next to the one good trace that must PASS."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import wii_telemetry_server as telemetry  # noqa: E402

PROFILE = "mob_motion_2m"


def beat(tick=10, **mm):
    row = dict(event="heartbeat", run_id="mm-run", profile=PROFILE,
               build_id="abc_06.27", tick=tick, fps=60, world_loaded=1)
    row.update(mm)
    return row


def mm(st, **changes):
    """One fixture snapshot; st 2 defaults to the PC-measured good values."""
    d = dict(mm_valid=1, mm_st=st, mm_bits=0, mm_recur_ok=0, mm_recur_bad=0,
             mm_rise_milli=0, mm_step_milli=0, mm_gen=3, mm_tick=1)
    if st == 2:
        d.update(mm_bits=7, mm_recur_ok=31, mm_rise_milli=1143,
                 mm_step_milli=1000, mm_tick=400)
    if st == 3:
        d.update(mm_bits=5, mm_recur_ok=31, mm_rise_milli=1143,
                 mm_step_milli=0, mm_tick=400)
    if st == 4:
        d.update(mm_tick=60)
    d.update(changes)
    return d


class MobMotionEvidenceTest(unittest.TestCase):
    def grade(self, rows, profile=PROFILE):
        with tempfile.TemporaryDirectory() as td:
            args = telemetry._selftest_args(Path(td), "mm-run", profile)
            c = telemetry.Collector(args)
            c.world_loaded_seen = True
            for row in rows:
                c.process(row)
                if c.done:
                    break
            c.hb_count = max(c.hb_count, c.profile["min_hb"])
            if not c.done:
                c._finish_normal("duration reached")
            passed = c.pass_reason is not None and c.failure_class is None
            return c.pass_reason, c.failure_class, passed, c

    def good_rows(self):
        return ([beat(t) for t in range(1, 5)]
                + [beat(t, **mm(1, mm_tick=t)) for t in range(5, 25)]
                + [beat(t, **mm(2)) for t in range(25, 40)])

    # ---- the one good trace -------------------------------------------------
    def test_good_trace_passes(self):
        reason, cls, passed, c = self.grade(self.good_rows())
        self.assertTrue(reason, cls)
        self.assertTrue(passed)
        self.assertEqual(c.mm_terminal[1], 2)

    # ---- absent / incomplete ------------------------------------------------
    def test_no_evidence_at_all_fails(self):
        _, cls, passed, _ = self.grade([beat(t) for t in range(1, 30)])
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    def test_pending_forever_fails(self):
        rows = [beat(t, **mm(1, mm_tick=t)) for t in range(1, 30)]
        _, cls, passed, _ = self.grade(rows)
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    def test_fixture_fail_verdict_fails(self):
        rows = [beat(1, **mm(1)), beat(2, **mm(3))]
        _, cls, passed, _ = self.grade(rows)
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    def test_fixture_skip_verdict_fails(self):
        _, cls, passed, _ = self.grade([beat(1, **mm(1)), beat(2, **mm(4))])
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    # ---- one bad trace per recorded check -----------------------------------
    def bad_pass(self, **changes):
        rows = [beat(1, **mm(1)), beat(2, **mm(2, **changes))]
        _, cls, passed, _ = self.grade(rows)
        self.assertEqual(cls, "mob_motion_evidence_failed", changes)
        self.assertFalse(passed, changes)

    def test_lift_check_missing_bit_fails(self):
        self.bad_pass(mm_bits=6)

    def test_lift_too_small_fails(self):
        self.bad_pass(mm_rise_milli=499)

    def test_recurrence_miss_fails(self):
        self.bad_pass(mm_recur_bad=1)

    def test_recurrence_too_few_ticks_fails(self):
        self.bad_pass(mm_recur_ok=7)

    def test_recurrence_bit_missing_fails(self):
        self.bad_pass(mm_bits=5)

    def test_step_landing_off_fails(self):
        self.bad_pass(mm_step_milli=999)

    def test_step_bit_missing_fails(self):
        self.bad_pass(mm_bits=3)

    def test_pass_before_verdict_tick_fails(self):
        self.bad_pass(mm_tick=399)

    # ---- snapshot integrity -------------------------------------------------
    def test_terminal_changed_later_fails(self):
        rows = [beat(1, **mm(2)), beat(2, **mm(2, mm_recur_ok=32))]
        _, cls, passed, _ = self.grade(rows)
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    def test_terminal_then_vanished_fails(self):
        _, cls, passed, _ = self.grade([beat(1, **mm(2)), beat(2)])
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    def test_non_integer_field_fails(self):
        _, cls, passed, _ = self.grade([beat(1, **mm(2, mm_recur_ok="31"))])
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    def test_pending_with_payload_fails(self):
        _, cls, passed, _ = self.grade([beat(1, **mm(1, mm_recur_ok=4))])
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    def test_fail_contradicting_bits_fails(self):
        _, cls, passed, _ = self.grade([beat(1, **mm(3, mm_bits=7))])
        self.assertEqual(cls, "mob_motion_evidence_failed")
        self.assertFalse(passed)

    # ---- F2 cost counters: summed, recorded, never a gate -------------------
    def test_cost_counters_summed_on_perf_stress(self):
        rows = [beat(t, mm_moves=100 + t, mm_move_us=900 + t, mm_env_walks=100,
                     mm_env_ents=4000) for t in range(1, 6)]
        for r in rows:
            r["profile"] = "perf_stress_5m"
            # perf_stress now also gates the d1 death snapshot; a live
            # zero-death session keeps the run graded on these counters.
            r["d1"] = [1, 0, 0, 0, 0]
            r["player_health"] = 20
        with tempfile.TemporaryDirectory() as td:
            args = telemetry._selftest_args(Path(td), "mm-run", "perf_stress_5m")
            c = telemetry.Collector(args)
            for r in rows:
                c.process(r)
            self.assertEqual(c.mm_cost["beats"], 5)
            self.assertEqual(c.mm_cost["moves"], 5 * 100 + 15)
            self.assertEqual(c.mm_cost["move_us"], 5 * 900 + 15)
            self.assertEqual(c.mm_cost["max_move_us"], 905)
            self.assertEqual(c.mm_cost["env_ents"], 20000)
            self.assertNotEqual(c.failure_class, "mob_motion_evidence_failed")

    def test_other_profiles_ignore_mm_keys(self):
        """A non-mob_motion profile never gates on the fixture keys."""
        with tempfile.TemporaryDirectory() as td:
            args = telemetry._selftest_args(Path(td), "mm-run", "far_idle_2m")
            c = telemetry.Collector(args)
            c.process(dict(beat(1, **mm(3)), profile="far_idle_2m"))
            self.assertNotEqual(c.failure_class, "mob_motion_evidence_failed")


if __name__ == "__main__":
    unittest.main()
