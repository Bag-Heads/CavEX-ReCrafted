"""Exact tick attribution, malformed/incomplete tape rejection, counter wrap."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('analyze_tps_walk',
    Path(__file__).resolve().parents[1] / 'tools/analyze_tps_walk.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class AnalyzerTest(unittest.TestCase):
    def events(self):
        rows = [dict(event='tps_tick', update=i + 10, world_tick=i,
                     clock='thread_cycles_tsc_qpc_estimate', step_ns=99,
                     cpu_ns=3_000_000, wall_ns=w,
                     phase_ns=[0, 1_000_000, 2_000_000] + [0] * 8,
                     nested_ns=[99_000_000] + [0] * 20, anchor=[0.5, 70, 0.5, 96])
                for i, w in enumerate([10_000_000, 60_000_000, 50_000_000])]
        beat = dict(event='heartbeat', anchor_measuring=True, cpu_valid=1,
                    cpu_ticks=10, anchor_ms=1, cpu_packed_us=0xfffffff0)
        return [beat] + rows + [dict(beat, cpu_ticks=13, anchor_ms=1000,
                                    cpu_packed_us=0x20),
             dict(event='tps_end', records=3, stream_valid=True),
             dict(event='profile_done', status='ok')]

    def test_attribution(self):
        r = mod.analyze(self.events())
        self.assertEqual(r['wall_ms'], {'50': 50, '95': 60, '100': 60})
        self.assertEqual(r['over_50_ms'], 1)
        self.assertEqual(r['worst_20'][0]['dominant'], 'save')
        self.assertEqual(r['worst_20'][0]['update'], 11)
        self.assertEqual(r['phase_ms_per_tick']['sim'], 1)
        self.assertEqual(r['main_ms_per_tick']['cpu_packed_us'], 0.016)

    def test_rejects_incomplete_or_corrupt(self):
        original = self.events()
        for change in ('drop', 'duplicate', 'clock', 'invalid', 'schema', 'end', 'tail', 'phase'):
            e = copy.deepcopy(original)
            if change == 'drop': del e[2]
            if change == 'duplicate': e[2] = e[1]
            if change == 'clock': e[2]['clock'] = 'activity_masked_wall_estimate'
            if change == 'invalid': e[0]['cpu_valid'] = 0
            if change == 'schema': e[1]['phase_ns'].pop()
            if change == 'end': del e[-2]
            if change == 'tail': e[0]['anchor_ms'] = e[-3]['anchor_ms'] = 0
            if change == 'phase': e[1]['phase_ns'][0] = 1
            with self.subTest(change=change), self.assertRaises(ValueError):
                mod.analyze(e)

    def test_aggregated_totals_and_missing_packet(self):
        e = self.events()
        ticks = [r for r in e if r['event'] == 'tps_tick']
        summary = dict(event='tps_summary', seq=1, count=3, first=10, last=12,
                       clock=ticks[0]['clock'], step_ns=99,
                       cpu_ns=sum(r['cpu_ns'] for r in ticks),
                       wall_ns=sum(r['wall_ns'] for r in ticks), max_ns=60000000,
                       over50=1, phase_ns=[sum(r['phase_ns'][i] for r in ticks) for i in range(11)],
                       nested_ns=[sum(r['nested_ns'][i] for r in ticks) for i in range(21)])
        aggregate = [e[0], summary] + e[-3:]
        result = mod.analyze(aggregate)
        self.assertEqual(result['phase_ms_per_tick'], mod.analyze(e)['phase_ms_per_tick'])
        self.assertEqual(result['wall_ms'], {'50': None, '95': None, '100': 60})
        self.assertEqual(result['over_50_ms'], 1)
        self.assertEqual(result['main_ms_per_tick'], mod.analyze(e)['main_ms_per_tick'])
        self.assertIn('unavailable', mod.markdown(result, 'bounded'))
        for key, value in [('seq', 2), ('count', 4), ('first', 9), ('over50', 4)]:
            bad = copy.deepcopy(aggregate)
            bad[1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): mod.analyze(bad)

    def aggregate(self):
        ticks = [r for r in self.events() if r['event'] == 'tps_tick']
        summary = dict(event='tps_summary', seq=1, count=3, first=10, last=12,
                       clock=ticks[0]['clock'], step_ns=99,
                       cpu_ns=9_000_000, wall_ns=120_000_000, max_ns=60_000_000,
                       over50=1, phase_ns=[0, 3_000_000, 6_000_000] + [0] * 8,
                       nested_ns=[297_000_000] + [0] * 20,
                       census_version=3, census=[0] * len(mod.CENSUS))
        return [self.events()[0], summary, self.events()[-3], self.events()[-2]]

    def test_aggregated_census(self):
        events = self.aggregate()
        events[1]['census'][mod.CENSUS.index('save_calls')] = 6
        result = mod.analyze(events)
        self.assertEqual(result['census']['per_update']['save_calls'], 2)
        self.assertIn('Save calls/update: 2.000000', mod.markdown(result, 'aggregate'))
        for change in ('missing', 'short', 'negative', 'impossible', 'version'):
            bad = copy.deepcopy(events)
            if change == 'missing': del bad[1]['census']
            if change == 'short': bad[1]['census'].pop()
            if change == 'negative': bad[1]['census'][0] = -1
            if change == 'impossible': bad[1]['census'][2] = 1
            if change == 'version': bad[1]['census_version'] = 2
            with self.subTest(change=change), self.assertRaises(ValueError): mod.analyze(bad)

    def test_memoise_aggregate_partial_and_legacy_census(self):
        events = self.partial_window()
        row = next(e for e in events if e['event'] == 'tps_summary')
        for key, value in dict(block_count_cells=300, block_fill_cells=0,
                               block_consumed_cells=300, block_consumed_sources=12,
                               block_memo_fallbacks=2, save_calls=6).items():
            row['census'][mod.CENSUS.index(key)] = value
        report = mod.markdown(mod.analyze(events), 'merged')
        for text in ('early_abort', '| save | 2.000000 |', 'Save calls/update: 2.000000',
                     '100.000 / 0.000', '100.000 / 4.000',
                     'Memo fallback calls: block=2, sky=0.', 'Certificate lookup misses:'):
            self.assertIn(text, report)
        for version, length in ((1, 30), (2, 32)):
            old = copy.deepcopy(events)
            old_row = next(e for e in old if e['event'] == 'tps_summary')
            old_row.update(census_version=version, census=row['census'][:length])
            result = mod.analyze(old)
            self.assertEqual(result['census']['per_update']['block_count_cells'], 100)
            self.assertFalse(result['census']['consumed_available'])

    def test_maximum_summary_bytes(self):
        import json
        row = self.aggregate()[1]
        for key in ('seq', 'count', 'first', 'last', 'step_ns', 'over50'):
            row[key] = 2**32 - 1
        for key in ('cpu_ns', 'wall_ns', 'max_ns'):
            row[key] = 2**64 - 1
        for key in ('phase_ns', 'nested_ns', 'census'):
            row[key] = [2**64 - 1] * len(row[key])
        payload = json.dumps(row, separators=(',', ':'))
        self.assertEqual(len(payload), 1754)
        self.assertLess(len(payload), 2048)
        row['run_id'] = '\x01' * 63
        wire = json.dumps(row, separators=(',', ':')) + '\n'
        self.assertEqual(len(wire), 2145)
        self.assertLess(len(wire), 2304)

    def test_single_anchor_window(self):
        events = self.aggregate()
        for beat in (events[0], events[2]):
            beat.pop('anchor_measuring')
            beat.pop('anchor_ms')
        events.insert(0, dict(event='heartbeat', cpu_ticks=0, cpu_valid=1))
        events.insert(1, dict(event='tps_window', kind='single_anchor', settle_ms=5000))
        events[-1]['measured_ns'] = 2_000_000_000
        result = mod.analyze(events)
        self.assertEqual(result['measured_seconds'], 2)
        self.assertEqual(result['achieved_tps'], 1.5)
        self.assertEqual(result['main_ms_per_tick']['cpu_packed_us'], 0.016)
        for change in ('noseal', 'duplicate', 'reordered'):
            bad = copy.deepcopy(events)
            if change == 'noseal': del bad[-1]['measured_ns']
            if change == 'duplicate': bad.insert(1, bad[1])
            if change == 'reordered': bad.append(bad.pop(1))
            with self.subTest(change=change), self.assertRaises(ValueError): mod.analyze(bad)

    def test_profile_settle_table_matches_device(self):
        import runpy
        import sys
        root = Path(__file__).resolve().parents[1]
        sys.path.insert(0, str(root / 'tools'))
        try:
            profiles = runpy.run_path(str(root / 'tools/wii_telemetry_server.py'))['PROFILES']
        finally:
            sys.path.pop(0)
        self.assertIn('#define HW_RUN_TPS_SETTLE_MS 5000u',
                      (root / 'source/game/hw_run.h').read_text())
        # The anchor profiles gate on their own two-anchor readiness
        # instead of the post-load TPS settle, so they carry None.
        # split_play_2m (2026-09-07) is the same scenario with a REAL
        # second body, and must stay in step with hw_run_tps_measuring.
        # split_actions_6m (2026-09-14, S1-d) is the same anchor-gated
        # scenario with a scripted guest action sequence on top, so it
        # carries None here for the same reason the three above do.
        # split_recovery_6m (2026-09-17): scripted split recovery proxy; same window as split_actions_6m
        # split_portal_quit_4m (2026-09-23, push-0921 6b): scripted
        # Save-and-quit-before-the-portal-switch proxy on the same
        # split_recovery scenario; the device excludes it from the TPS
        # settle exactly as it excludes split_recovery_6m (hw_run.c), so it
        # carries None too.
        # split_regrow_8m (colcap-1006 R5 item 3): split_anchor_6m's
        # readiness-gated window (hw_run_tps_measuring) with the anchor
        # cycle on top, so it carries None as well.
        anchor_gated = ('split_anchor_6m', 'split_play_2m', 'split_play_8m',
                        'split_actions_6m', 'split_recovery_6m',
                        'split_portal_quit_4m', 'split_dims_6m',
                        'split_regrow_8m')
        for name, profile in profiles.items():
            self.assertEqual(profile['tps_settle_ms'],
                             None if name in anchor_gated else 5000)

    def partial_window(self, sealed=True):
        events = self.aggregate()
        for i, beat in enumerate((events[0], events[2]), 18):
            beat.pop('anchor_measuring')
            beat.pop('anchor_ms')
            beat['tick'] = i
        events.insert(0, dict(event='tps_window', kind='single_anchor', settle_ms=5000))
        events.insert(0, dict(event='heartbeat', tick=17, cpu_ticks=0, cpu_valid=1))
        events[-1].update(measured_ns=2_000_000_000, stream_valid=False, reason='early_abort')
        return events if sealed else events[:-1]

    def test_sealed_partial_reports_exact_buckets(self):
        events = self.partial_window()
        result = mod.analyze(events)
        self.assertTrue(result['partial'])
        self.assertTrue(result['sealed'])
        self.assertFalse(result['duration_estimated'])
        self.assertEqual(result['records'], 3)
        self.assertEqual(result['measured_seconds'], 2)
        self.assertEqual(result['phase_ms_per_tick']['save'], 2)
        report = mod.markdown(result, 'aborted')
        self.assertTrue(report.startswith('# UNSEALED/PARTIAL:'))
        self.assertIn('early_abort', report)
        self.assertIn('| save | 2.000000 |', report)

    def test_unsealed_partial_requires_opt_in_and_labels_estimate(self):
        events = self.partial_window(sealed=False)
        with self.assertRaisesRegex(ValueError, 'Missing/invalid tps_end'):
            mod.analyze(events)
        result = mod.analyze(events, allow_partial=True)
        self.assertTrue(result['partial'])
        self.assertFalse(result['sealed'])
        self.assertTrue(result['duration_estimated'])
        self.assertEqual(result['measured_seconds'], 2)
        self.assertEqual(result['achieved_tps'], 1.5)
        report = mod.markdown(result, 'historical')
        self.assertIn('UNSEALED/PARTIAL', report)
        self.assertIn('updates/s is approximate', report)
        self.assertIn('2.000 estimated heartbeat-span seconds', report)

    def test_partial_still_rejects_corrupt_streams(self):
        for sealed in (False, True):
            for change in ('gap', 'duplicate', 'reorder', 'range', 'invalid_clock', 'invalid_observer'):
                events = self.partial_window(sealed)
                first = events[3]
                second = dict(first, seq=2, first=13, last=15)
                events.insert(4, second)
                if sealed:
                    events[-1]['records'] = 6
                if change == 'gap': events.pop(3)
                if change == 'duplicate': events.insert(3, first)
                if change == 'reorder': events[3], events[4] = events[4], events[3]
                if change == 'range': second.update(first=14, last=16)
                if change == 'invalid_clock': first['clock'] = 'fake'
                if change == 'invalid_observer': events[2]['cpu_valid'] = 0
                with self.subTest(sealed=sealed, change=change), self.assertRaises(ValueError):
                    mod.analyze(events, allow_partial=True)
        for change in ('records', 'reason', 'duration', 'double_end', 'end_before_summary'):
            events = self.partial_window()
            if change == 'records': events[-1]['records'] = 4
            if change == 'reason': events[-1].pop('reason')
            if change == 'duration': events[-1]['measured_ns'] = 0
            if change == 'double_end': events.append(events[-1])
            if change == 'end_before_summary': events.insert(3, events.pop())
            with self.subTest(change=change), self.assertRaises(ValueError):
                mod.analyze(events, allow_partial=True)
        for change in ('no_before', 'no_after', 'heartbeat_gap', 'heartbeat_reverse', 'window_after_summary'):
            events = self.partial_window(sealed=False)
            if change == 'no_before': events.pop(0)
            if change == 'no_after': events.pop()
            if change == 'heartbeat_gap': events[-1]['tick'] += 1
            if change == 'heartbeat_reverse': events[-1]['tick'] = 17
            if change == 'window_after_summary': events.append(events.pop(1))
            with self.subTest(change=change), self.assertRaises(ValueError):
                mod.analyze(events, allow_partial=True)

    def test_partial_cli(self):
        import json
        import subprocess
        import sys
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            tape = Path(directory) / 'capture.ndjson'
            command = [sys.executable, str(Path(mod.__file__)), str(tape)]
            for sealed in (True, False):
                tape.write_text(''.join(json.dumps(e) + '\n' for e in self.partial_window(sealed)), encoding='utf-8')
                result = subprocess.run(command + ([] if sealed else ['--allow-partial']),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('UNSEALED/PARTIAL', result.stdout)
                self.assertIn('| save | 2.000000 |', result.stdout)

    def test_single_anchor_cli_minimum(self):
        import json
        import subprocess
        import sys
        import tempfile
        events = self.aggregate()
        for beat in (events[0], events[2]):
            beat.pop('anchor_measuring')
            beat.pop('anchor_ms')
        events.insert(0, dict(event='tps_window', kind='single_anchor', settle_ms=5000))
        events[-1]['measured_ns'] = 2_000_000_000
        with tempfile.TemporaryDirectory() as directory:
            tape = Path(directory) / 'capture.ndjson'
            tape.write_text(''.join(json.dumps(e) + '\n' for e in events), encoding='utf-8')
            command = [sys.executable, str(Path(mod.__file__)), str(tape)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('2.000 measured seconds', result.stdout)
            result = subprocess.run(command + ['--min-seconds', '3'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('need 3', result.stderr)
            tape.write_text(''.join(json.dumps(e) + '\n' for e in self.aggregate()), encoding='utf-8')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('need 360', result.stderr)

    def test_historical_data_is_not_fabricated(self):
        with self.assertRaisesRegex(ValueError, 'Historical heartbeat'):
            mod.analyze([dict(event='heartbeat', srv_us=[1] * 11)])

    def test_census(self):
        e = self.events()
        values = [0] * len(mod.CENSUS)
        for key, value in dict(save_calls=6, save_writes=3, save_unchanged=1,
                               save_unknown=1, save_compress_ns=6_000_000,
                               save_commit_ns=3_000_000, save_compare_ns=300_000,
                               sky_pairs=2, sky_duplicates=2, sky_consumed_cells=90,
                               sky_consumed_sources=6).items():
            values[mod.CENSUS.index(key)] = value
        summary = dict(event='tps_summary', census_version=3, first_record=0,
                       records=3, census=values)
        e.insert(-2, summary)
        r = mod.analyze(e)
        self.assertEqual(r['census']['per_update']['save_calls'], 2)
        self.assertEqual(r['census']['per_update']['save_compress_ns'], 2)
        self.assertAlmostEqual(r['census']['unchanged_save_share'], 1/3)
        self.assertIn('33.333%', mod.markdown(r, 'census'))
        self.assertIn('30.000 / 2.000', mod.markdown(r, 'census'))
        for version, length in ((1, 30), (2, 32)):
            historical = copy.deepcopy(e)
            historical[-3]['census_version'] = version
            historical[-3]['census'] = historical[-3]['census'][:length]
            old = mod.analyze(historical)
            self.assertEqual(old['census']['per_update']['save_calls'], 2)
            self.assertIn('unavailable', mod.markdown(old, 'census'))
        for field, value in [('records', 2), ('first_record', 1), ('census_version', 4)]:
            bad = copy.deepcopy(e)
            bad[-3][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                mod.analyze(bad)
        with self.assertRaises(ValueError):
            mod.analyze(e + [summary])

    def test_wii_clock_is_qualified(self):
        e = self.events()
        for r in e:
            if r['event'] == 'tps_tick': r['clock'] = 'activity_masked_wall_estimate'
        self.assertIn('activity_masked_wall_estimate', mod.markdown(mod.analyze(e), 'Wii'))


if __name__ == '__main__':
    unittest.main()
