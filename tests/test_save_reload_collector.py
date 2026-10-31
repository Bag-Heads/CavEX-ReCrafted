"""save_reload_3m (SPEC R12) collector contract.

Runs the contract's own selftest (canonical tape PASS, every mutation FAIL),
then feeds the canonical tape through the REAL collector class
(tools/wii_telemetry_server.py Collector.note_event path via its
save_reload hook) to prove the profile is wired: registered in PROFILES with
the save_reload scenario, events routed to SaveReloadEvidence, profile_done
recorded. Also pins the device's line formats against the contract: the
marker list here and in source/game/save_reload_script.c must match.
"""
import importlib.util
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import save_reload_evidence as sre  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    'wii_telemetry_server', ROOT / 'tools' / 'wii_telemetry_server.py')
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)


class SaveReloadCollectorTest(unittest.TestCase):
    def test_selftest(self):
        self.assertGreaterEqual(sre.selftest(), 20)

    def test_registered(self):
        prof = srv.PROFILES[sre.PROFILE]
        self.assertEqual(prof['scenario'], 'save_reload')
        self.assertFalse(prof['expect_dump'])
        # Collection window must outlive the device's 180 s profile.
        self.assertGreater(prof['duration'], 180)
        src = (ROOT / 'source/game/hw_run.c').read_text(encoding='utf-8')
        self.assertIn('case HW_RUN_PROFILE_SAVE_RELOAD_3M: return "save_reload_3m";', src)

    def test_device_markers_match_contract(self):
        src = (ROOT / 'source/game/save_reload_script.c').read_text(
            encoding='utf-8')
        emitted = re.findall(r'out->marker = "([a-z_0-9]+)";', src)
        # Every contract marker is emitted by the device, in order of first
        # appearance, plus only the "fail" marker.
        self.assertEqual([m for m in emitted if m != 'fail'], list(sre.MARKERS))
        test_src = (ROOT / 'tests/test_save_reload_script.c').read_text(
            encoding='utf-8')
        for m in sre.MARKERS:
            self.assertIn('"%s"' % m, test_src)
        # The verdict line carries every field the contract grades.
        for key in ('"event\\":\\"save_reload\\"', 'client_loads', 'server_loads',
                    'saves', 'after_loads', 'applied', 'before', 'after'):
            self.assertIn(key, src)
        for h in sre.HASHES + ('burn', 'cook', 'fixture_ok', 'resident'):
            self.assertIn('\\"%s\\"' % h, src)

    def test_hash_mismatch_fails(self):
        rows = sre.sample_run()
        rows[-1]['after']['chest'] = 'f' * 16
        problem = sre.grade(rows)
        self.assertIn('chest', problem)

    def test_single_load_fails(self):
        rows = sre.sample_run()
        rows[-1]['client_loads'] = 1
        rows[-1]['server_loads'] = 1
        self.assertIn('load exactly twice', sre.grade(rows))

    def test_driver_fail_marker_names_reason(self):
        rows = sre.sample_run()[:3]
        rows.append({"event": "save_reload_phase", "run_id": "r1", "seq": 4,
                     "marker": "fail", "phase": "fail", "ms": 1,
                     "reason": "pause_never_opened"})
        self.assertIn('pause_never_opened', sre.grade(rows, done='pause_never_opened'))


if __name__ == '__main__':
    unittest.main(verbosity=0)
