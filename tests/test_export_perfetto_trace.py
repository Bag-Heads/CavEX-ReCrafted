import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "export_perfetto_trace", ROOT / "tools" / "export_perfetto_trace.py")
EXPORTER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(EXPORTER)


class PerfettoExportTests(unittest.TestCase):
    def fixture(self):
        rows = [
            {"event": "boot", "run_id": "run-a", "profile": "split_play_8m",
             "build_id": "build-a"},
            {"event": "heartbeat", "perf_duration_us": 1000, "tick": 1,
             "fps": 60.0, "ms_build_max": 1.0, "cpu_srv_us": [2, 3]},
            {"event": "heartbeat", "perf_duration_us": 2000, "tick": 2,
             "fps": 31.0, "ms_build_max": 99.0, "cpu_srv_us": [7, 1]},
            {"event": "heartbeat", "perf_duration_us": 10_000, "tick": 3,
             "fps": 58.0, "ms_build_max": 4.0, "cpu_srv_us": [4, 8]},
            {"event": "heartbeat", "perf_duration_us": 500, "tick": 0,
             "fps": 55.0, "ms_build_max": 2.0, "cpu_srv_us": [1, 1]},
            {"event": "heartbeat", "perf_duration_us": 1500, "tick": 1,
             "fps": 57.0, "ms_build_max": 3.0, "cpu_srv_us": [2, 2]},
            {"event": "profile_done", "run_id": "run-a", "status": "ok"},
        ]
        return [(index + 1, row) for index, row in enumerate(rows)]

    def test_device_clock_gaps_resets_and_peak_preservation(self):
        trace = EXPORTER.convert_records(
            self.fixture(), clock="perf_duration_us", sample_every=4, gap_us=5000)
        meta = trace["cavewii"]
        self.assertEqual(meta["clock_resets"], 1)
        self.assertEqual(meta["clock_gaps"], 1)
        self.assertEqual([b["timestamp_us"] for b in meta["boundaries"]], [None, None])
        events = trace["traceEvents"]
        gaps = [event for event in events if event.get("name") == "device clock gap"]
        self.assertEqual(gaps[0]["ts"], 10_000)
        self.assertEqual(gaps[0]["args"]["gap_us"], 8000)
        resets = [event for event in events if event.get("name") == "device clock reset"]
        self.assertEqual(resets[0]["ts"], 500)
        counters = [event for event in events if event.get("ph") == "C"]
        peak = [event for event in counters if event["args"].get("ms_build_max") == 99.0]
        self.assertEqual(len(peak), 1)
        self.assertEqual(peak[0]["args"]["cpu_srv_us[0]"], 7)
        self.assertEqual({event["tid"] for event in counters}, {1, 2})
        self.assertEqual({event["id"] for event in counters}, {"segment-0", "segment-1"})

    def test_auto_clock_and_deterministic_cli_output(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "telemetry.ndjson"
            output_a = directory / "a.json"
            output_b = directory / "b.json"
            source.write_text("".join(
                json.dumps(row, separators=(",", ":")) + "\n"
                for _, row in self.fixture()), encoding="utf-8")
            self.assertEqual(EXPORTER.main([str(source), str(output_a), "--sample-every", "3"]), 0)
            self.assertEqual(EXPORTER.main([str(source), str(output_b), "--sample-every", "3"]), 0)
            self.assertEqual(output_a.read_bytes(), output_b.read_bytes())
            trace = json.loads(output_a.read_text(encoding="utf-8"))
            self.assertEqual(trace["cavewii"]["clock"], "perf_duration_us")
            self.assertEqual(trace["cavewii"]["run_id"], "run-a")
            self.assertEqual(trace["cavewii"]["build_id"], "build-a")

    def test_no_device_clock_fails_instead_of_inventing_time(self):
        rows = [(1, {"event": "heartbeat", "tick": 7, "fps": 60.0})]
        with self.assertRaisesRegex(ValueError, "no usable device clock"):
            EXPORTER.convert_records(rows)

    def test_mixed_run_identity_is_rejected(self):
        rows = self.fixture()
        rows.append((99, {"event": "boot", "run_id": "run-b"}))
        with self.assertRaisesRegex(ValueError, "run_id changed"):
            EXPORTER.convert_records(rows)

    def test_downsampling_keeps_both_sides_of_reset(self):
        rows = [(i + 1, {"event": "heartbeat", "perf_duration_us": value})
                for i, value in enumerate([900, 950, 10, 20, 30])]
        trace = EXPORTER.convert_records(rows, sample_every=99)
        counters = [e for e in trace["traceEvents"] if e.get("ph") == "C"]
        self.assertTrue({950, 10}.issubset({e["ts"] for e in counters}))

    def test_invalid_ndjson_is_strict_or_explicitly_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "bad.ndjson"
            source.write_text('{"event":"boot"}\nnot-json\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, ":2: invalid JSON"):
                EXPORTER.read_ndjson(source)
            self.assertEqual(len(EXPORTER.read_ndjson(source, skip_invalid=True)), 1)


if __name__ == "__main__":
    unittest.main()
