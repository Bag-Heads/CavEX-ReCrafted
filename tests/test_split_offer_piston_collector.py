"""Synthetic good/bad tapes plus the actual Collector completion path."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from split_offer_piston_evidence import SplitOfferPistonEvidence, PROFILE, PREFIX, ORDER
from wii_telemetry_server import Collector, _selftest_args, ingest_telemetry_line


def good_tape():
    rows = []
    ticks = {"offered": 1, "fixture": 10, "before": 11, "extended": 16,
             "released": 17, "admitted": 18, "play_begin": 20, "play_end": 620}
    reasons = {"offered": "accept_delayed_fixture", "fixture": "blocks_only_fixture"}
    for name, phase in ORDER:
        row = dict(schema=1, profile=PROFILE, run_id="offer", event=PREFIX+name,
                   seq=len(rows)+1, reason="", origin="driver")
        if phase is not None:
            row["phase"] = phase
        else:
            admitted = name in ("admitted", "play_begin", "play_end")
            pushed = name in ("extended", "released", "admitted", "play_begin", "play_end")
            row.update(origin="server", tick=ticks[name], generation=7,
                       host=[.9, 5.62, -4], guest=[1.4625 if pushed else .9, 5.62, 4.5 if name == "play_end" else .5],
                       guest_corrections=int(pushed), host_corrections=0, guest_damage=0,
                       host_token=0, guest_token=int(pushed), health=160, host_health=160,
                       bound=True, confirmed=admitted, gameplay=admitted,
                       finished_loading=admitted, extended=pushed,
                       play_ms=30000 if name == "play_end" else 0, ok=True,
                       difficulty=0, superflat=True, world_slot=0, world_generation=1, dimension=0,
                       reason=reasons.get(name, ""))
        rows.append(row)
    return rows


def grade(rows, done="ok"):
    evidence = SplitOfferPistonEvidence("offer")
    for row in rows:
        evidence.note(row)
    if done is not None:
        evidence.note_profile_done(done)
    return evidence


class Tests(unittest.TestCase):
    def test_good_and_predicate_mutations(self):
        good = good_tape()
        self.assertIsNone(grade(good).complete_error())
        mutations = [
            ("extended", "guest_corrections", 2, "guest_correction_delta"),
            ("offered", "difficulty", 1, "fixture_environment"),
            ("offered", "superflat", False, "fixture_environment"),
            ("offered", "dimension", 1, "guest_world"),
            ("extended", "world_generation", 2, "guest_world_generation"),
            ("extended", "host_corrections", 1, "host_route_touched"),
            ("extended", "host_token", 1, "host_route_touched"),
            ("extended", "host", [1, 5.62, -4], "host_route_touched"),
            ("extended", "guest_damage", 8, "guest_damage"),
            ("extended", "guest", [.9, 5.62, .5], "guest_push_distance"),
            ("extended", "gameplay", True, "guest_admitted_early"),
            ("offered", "confirmed", True, "guest_admitted_early"),
            ("offered", "bound", False, "guest_bound"),
            ("extended", "generation", 8, "guest_generation"),
            ("extended", "extended", False, "extension_incomplete"),
            ("extended", "tick", 12, "extension_incomplete"),
            ("released", "guest_corrections", 2, "guest_correction_delta"),
            ("admitted", "gameplay", False, "play_admission_lost"),
            ("play_end", "play_ms", 29999, "play_window_short"),
            ("play_end", "tick", 21, "play_window_short"),
            ("play_end", "guest", [1.4625, 5.62, .5], "play_no_movement"),
            ("fixture", "origin", "driver", "server_origin"),
            ("extended", "guest", [float("nan"), 5.62, .5], "server_pose_guest"),
            ("phase", "seq", 2, "contiguous_seq"),
        ]
        for name, field, value, reason in mutations:
            with self.subTest(name=name, field=field):
                rows = copy.deepcopy(good)
                next(r for r in rows if r["event"] == PREFIX+name)[field] = value
                self.assertEqual(grade(rows).complete_error(), reason)

    def test_missing_reordered_and_completion(self):
        rows = good_tape()
        self.assertEqual(grade(rows, None).complete_error(), "profile_done_missing")
        self.assertEqual(grade(rows[:-1]).complete_error(), "missing_run_ok")
        self.assertEqual(grade(rows, "fail:fixture").complete_error(), "profile_done_not_ok")
        evidence = grade(rows)
        evidence.note_profile_done("ok")
        self.assertEqual(evidence.complete_error(), "duplicate_profile_done")
        swapped = copy.deepcopy(rows)
        swapped[7], swapped[8] = swapped[8], swapped[7]
        self.assertIsNotNone(grade(swapped).complete_error())
        missing = rows[:8]+rows[9:]
        self.assertIsNotNone(grade(missing).complete_error())

    def test_collector_requires_tape_not_duration(self):
        with tempfile.TemporaryDirectory() as td:
            c = Collector(_selftest_args(Path(td), "offer", PROFILE))
            c._finish_normal("duration elapsed")
            self.assertEqual(c.failure_class, "split_offer_piston_evidence_failed")
            self.assertEqual(c.fail_reason, "profile_done_missing")

    def test_collector_good_and_bad_wire(self):
        for bad in (False, True):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as td:
                c = Collector(_selftest_args(Path(td), "offer", PROFILE))
                # Ordinary heartbeat guards still apply in addition to the tape.
                for i in range(10):
                    c.process(dict(event="heartbeat", run_id="offer", profile=PROFILE,
                                   build_id="abc_06.27", tick=i, fps=60,
                                   world_loaded=True, server_us=1000, oom=0, tel_drop=0))
                rows = good_tape()
                if bad:
                    rows[8]["guest_corrections"] = 2
                for row in rows:
                    ingest_telemetry_line(c, json.dumps(row)+"\n")
                c.process(dict(event="profile_done", run_id="offer", status="ok"))
                c._finish_normal("profile_done received")
                if bad:
                    self.assertEqual(c.failure_class, "split_offer_piston_evidence_failed")
                    self.assertEqual(c.fail_reason, "guest_correction_delta")
                else:
                    self.assertIsNone(c.fail_reason)
                    self.assertTrue(c.pass_reason)

    def test_telemetry_drop_blocks_every_completion_path(self):
        # Astra final review 2 P1: a tel_drop heartbeat after profile_done must
        # never let tick() or _finish_normal() report PASS.
        for path in ("tick", "finish"):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as td:
                c = Collector(_selftest_args(Path(td), "offer", PROFILE))
                for i in range(10):
                    c.process(dict(event="heartbeat", run_id="offer", profile=PROFILE,
                                   build_id="abc_06.27", tick=i, fps=60,
                                   world_loaded=True, server_us=1000, oom=0, tel_drop=0))
                for row in good_tape():
                    ingest_telemetry_line(c, json.dumps(row)+"\n")
                c.process(dict(event="profile_done", run_id="offer", status="ok"))
                c.process(dict(event="heartbeat", run_id="offer", profile=PROFILE,
                               build_id="abc_06.27", tick=10, fps=60,
                               world_loaded=True, server_us=1000, oom=0, tel_drop=1))
                if path == "tick":
                    c.offer_piston_completion_at -= 5
                    c.tick()
                else:
                    c._finish_normal("profile_done received")
                self.assertIsNone(c.pass_reason)
                self.assertEqual(c.failure_class, "split_offer_piston_evidence_failed")
                self.assertTrue(c.fail_reason.startswith("heartbeat_fault"))

    def test_offline_validator_agrees_with_collector_min_heartbeats(self):
        # Astra re-check 2: the offline PASS validator and the live collector
        # must demand the same heartbeat floor for this profile.
        import re
        from wii_telemetry_server import PROFILES
        root = Path(__file__).resolve().parents[1]
        text = (root / "tools/validate_wii_pass.ps1").read_text(encoding="utf-8")
        m = re.search(r'"%s"\s*\{\s*return\s+(\d+)\s*\}' % PROFILE, text)
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1)), int(PROFILES[PROFILE]["min_hb"]))

    def test_collector_rejects_duplicate_json_keys(self):
        with tempfile.TemporaryDirectory() as td:
            c = Collector(_selftest_args(Path(td), "offer", PROFILE))
            ingest_telemetry_line(c, '{"event":"split_offer_piston.phase","event":"alive"}\n')
            self.assertEqual(c.failure_class, "split_offer_piston_evidence_failed")


if __name__ == "__main__":
    unittest.main()
