"""Derived observer rows, actual live parser. Hardware grading remains unproven.

The new schema has no historical Wii rows: this test explicitly does not grant
service acceptance. The C producer supplies the positive schema fixture.
"""
import importlib.util
import contextlib
import io
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
source_root = Path(os.environ.get("CW_SO_SOURCE_ROOT", Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(source_root / "tools"))
import wii_telemetry_server as wire


class ObserverTest(unittest.TestCase):
    root = source_root

    def test_verbatim_y_hardware_loss(self):
        from g9_y_regression import replay
        with tempfile.TemporaryDirectory() as out:
            replay(wire, self.root, lambda run: wire.Collector(
                wire._selftest_args(Path(out) / run, run, "far_idle_2m")))

    def test_y_census_real_sender_and_parser(self):
        fixture = self.root / "tests/fixtures/g9_y_y_g9a_s12345.ndjson.bin"
        self.assertGreater(len(sys.argv), 2, "sender resolved via CTest registration required")
        output = subprocess.check_output([sys.argv[2], "--observer-y-wire", str(fixture)], text=True)
        line = next(line for line in output.splitlines() if line.startswith('{"event"'))
        original = next(raw for raw in fixture.read_bytes().splitlines() if b'"seq":2463,' in raw)
        self.assertEqual(line.encode(), original)
        with tempfile.TemporaryDirectory() as out:
            c = wire.Collector(wire._selftest_args(Path(out), "y_g9a_s12345", "far_idle_2m"))
            wire.ingest_telemetry_line(c, line)
            self.assertEqual(c.service_rows[0]["seq"], 2463)
            self.assertEqual(c.service_rows[0]["loss"], [0, 0, 2, 0])
            self.assertFalse(c.service_observer_report()["coverage_complete"])

    def test_y_producer_transport_regressions(self):
        for case in ("transport-retention", "final-refusal", "shutdown-boundary", "terminal-transport"):
            with self.subTest(case=case):
                result = subprocess.run([sys.argv[1], case], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_y_shutdown_main_boundary(self):
        src = self.source("source/main.c")
        boundary = src.split("if(drain_checkpoints) {", 1)[1].split("s_completion_heartbeat_server", 1)[0]
        self.assertIn("service_observer_state(SO_SHUTDOWN)", boundary)
        self.assertLess(boundary.index("service_observer_state(SO_SHUTDOWN)"),
                        boundary.index("server_local_shutdown(&server)"))

    def test_service_exposure_gate_rejects_short_run(self):
        with tempfile.TemporaryDirectory() as out, contextlib.redirect_stdout(io.StringIO()):
            args = wire._selftest_args(Path(out), "so-test", "perf_stress_5m")
            args.require_service_exposure = True
            c = wire.Collector(args)
            for row in self.producer_rows():
                wire.ingest_telemetry_line(c, json.dumps(row))
            c.pass_("diagnostic exposure fixture")
            self.assertEqual(c.failure_class, "service_exposure_failed")
            self.assertIsNone(c.pass_reason)

    def test_exposure_fake_clock_positive_and_mutations(self):
        rows = self.producer_rows("rows-exposure")
        def report(mutate=lambda row: None, live=True):
            with tempfile.TemporaryDirectory() as out:
                args = wire._selftest_args(Path(out), "so-test", "perf_stress_5m")
                args.require_service_exposure = True
                c = wire.Collector(args)
                for row in json.loads(json.dumps(rows)):
                    mutate(row)
                    wire.ingest_telemetry_line(c, json.dumps(row))
                # Explicit fake guard/heartbeat clock for interval arithmetic;
                # complete observer rows still come from the real C producer.
                for tick in range(301):
                    c.perf_guard_context = {"summary": [2, 1, 1, 1], "barriers": 0,
                                            "heartbeat_tick": tick}
                    c.note_service_exposure_heartbeat(dict(tick=tick, up_ms=tick*1000,
                        d1=[1, 0, 0, 0, 0], player_health=160,
                        world_loaded=True, loading_screen=not live))
                return c.service_exposure_report()
        positive = report()
        self.assertTrue(positive["complete"], positive["errors"])
        self.assertEqual(positive["ordinary_durable_checkpoints"], 8)
        self.assertEqual(positive["admitted_alive_ms"], 300000)
        for reason, mutate in {
            "checkpoint deficit": lambda row: row.update(context=[r for r in row["context"]
                                               if r[0] != 5 or r[7] != 8]),
            "accelerated cadence": lambda row: [r.__setitem__(8, 81) for r in row["context"] if r[0] == 5],
            "inactive music": lambda row: [r.__setitem__(18, 0) for r in row["context"] if r[0] in (4, 5)],
            "lost context": lambda row: row.update(loss=[1, 0, 0, 0]),
        }.items():
            with self.subTest(reason=reason):
                self.assertFalse(report(mutate)["complete"])
        self.assertFalse(report(live=False)["complete"])

    def test_full_y_lifecycle_retained_diagnostic_rejected(self):
        # Full source tapes are optional in CI; bounded verbatim extracts above
        # are always shipped and SHA checked. No derivative becomes a receipt.
        from g9_y_regression import PINNED
        import hashlib
        for run, pin in PINNED.items():
            source = self.root / "tmp/wii_runs" / run / "telemetry.ndjson"
            if not source.exists():
                continue
            raw = source.read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), pin)
            for required in (False, True):
                with tempfile.TemporaryDirectory() as out, contextlib.redirect_stdout(io.StringIO()):
                    args = wire._selftest_args(Path(out), run, "perf_stress_5m", duration=300,
                                              expected_build="b587339_10.10")
                    args.require_service_exposure = required
                    c = wire.Collector(args)
                    for line in raw.splitlines():
                        wire.ingest_telemetry_line(c, line.decode())
                        if c.done:
                            break
                    self.assertEqual(c.failure_class, "service_exposure_failed" if required else None)
                    self.assertTrue(c.done)
                    self.assertEqual(bool(c.pass_reason), not required)
                    exposure = c.service_exposure_report()
                    self.assertEqual(exposure["ordinary_durable_checkpoints"], 6 if "a_" in run else 5)
                    self.assertLess(exposure["admitted_alive_ms"], 300000)

    def test_diagnostic_runner_duration(self):
        runner = self.source("tools/run_wii_hardware.ps1")
        self.assertIn("[switch]$ServiceDiagnostic", runner)
        self.assertIn("$duration = 600", runner)
        self.assertIn('"--require-service-exposure"', runner)
        self.assertIn('"--cw-duration", "$duration"', runner)
        diagnostic_args = runner.rsplit("if($ServiceDiagnostic) {", 1)[1].split("Write-Host", 1)[0]
        self.assertIn('"debug.music_continuous_int=1"', diagnostic_args)
        self.assertEqual(runner.count('"debug.music_continuous_int=1"'), 1)
        # No cadence or world-load change buys the extra exposure.
        fixture = self.source("source/network/server_local.c")
        self.assertIn("#define LQ_STRESS_CKPT 600", fixture)

    def test_verbatim_x_hardware_gaps(self):
        from g9_x_regression import observer_cases
        observer_cases(wire)

    def test_continuous_context_drain(self):
        self.assertEqual(subprocess.run([sys.argv[1], "--x-red"], capture_output=True).returncode, 0)
        self.assertEqual(subprocess.run([sys.argv[1], "deferred-metric"], capture_output=True).returncode, 0)

    def test_unassigned_input_still_cannot_certify_latency(self):
        with tempfile.TemporaryDirectory() as out:
            c = self.collector(out)
            for row in self.producer_rows("rows-x"):
                wire.ingest_telemetry_line(c, json.dumps(row))
            report = c.service_observer_report()
            self.assertTrue(report["final_seen"])
            self.assertEqual(report["input_unassigned_consumers"], 100)
            self.assertFalse(report["coverage_complete"])
            self.assertIn("missing fresh consumed input observations", report["errors"])
            self.assertFalse(report["receipt_latency_proven"])
            self.assertFalse(any(e.startswith(("missing=", "overflow=", "dropped=")) for e in report["errors"]))

    def source(self, path):
        return (self.root / path).read_text(encoding="utf-8")

    def producer_rows(self, case="rows"):
        return [json.loads(r) for r in subprocess.check_output(
            [sys.argv[1], case], text=True).splitlines()
            if r.startswith('{"event":"service_observer"')]

    def condition(self, source, anchor):
        """Extract a production if expression, balancing its C parentheses."""
        begin = source.index("if(", source.index(anchor)) + 3
        depth = 1
        end = begin
        while depth:
            if source[end] == "(":
                depth += 1
            elif source[end] == ")":
                depth -= 1
            end += 1
        return source[begin:end-1]

    def sender_condition(self, expr, **state):
        replacements = {
            'g9_capture_pending()': 'capture_pending',
            'service_observer_hold_transport()': 'observer_pending',
            'cw_tel_drained(&s_sender_ring)': 'fifo_drained',
            '!strncmp(line, "{\\\"event\\\":\\\"profile_done\\\",", 24)': 'is_done',
        }
        for old, new in replacements.items():
            expr = expr.replace(old, new)
        expr = re.sub(r'TEL_LOAD\(([^)]+)\)', lambda m: m[1].replace('.', '_'), expr)
        expr = expr.replace('&&', ' and ').replace('||', ' or ')
        expr = " ".join(re.sub(r'!(?!=)', ' not ', expr).split())
        return bool(eval(expr, {"__builtins__": {}}, state))

    def test_wii_sender_quiesce_model(self):
        # Model the EXACT production rejection expression, including the old
        # event-prefix restriction. No Wii IO/scheduler double can mask it.
        src = self.source("source/platform/hw_telemetry.c")
        begin = src.index("if(TEL_LOAD(s_quiesce_requested)", src.index("static bool emit_observer_packet_mode"))
        end = src.index("return false;", begin)
        expr = src[begin + 3:end].strip()[:-1]  # enclosing if ')' only
        expr = re.sub(r"strncmp\(line, .*?, 28\)", "not_service", expr)
        for name in ("s_quiesce_requested", "s_sender_started"):
            expr = expr.replace("TEL_LOAD(" + name + ")", name)
        expr = expr.replace("LWP_GetSelf() != s_sender_thread", "not_sender")
        expr = expr.replace("&&", " and ").replace("||", " or ")
        expr = " ".join(re.sub(r"!(?!=)", " not ", expr).split())
        def admitted(sender, event):
            return not eval(expr, {"__builtins__": {}}, dict(s_quiesce_requested=True,
                s_sender_started=True, not_sender=not sender, not_service=event != "service_observer"))
        for event in ("g9_cpu", "g9_capture", "service_observer"):
            with self.subTest(event=event):
                self.assertTrue(admitted(True, event))
                self.assertFalse(admitted(False, event))
        # Observer OFF + capture ON: pending rows decrease after quiesce,
        # then profile_done leaves FIFO and the drained quiesce gate opens.
        pending, fifo, quiesced = 41, ["profile_done"], False
        for _ in range(60):
            if pending and admitted(True, "g9_capture"):
                pending -= 1
            if not pending and fifo:
                fifo.pop()
            if not fifo and not pending:
                quiesced = True
        self.assertTrue(quiesced, "G9 capture tail blocked after quiesce")

    def test_final_before_profile_done_and_no_client_model(self):
        src = self.source("source/platform/hw_telemetry.c")
        profile = src.split("void hw_telemetry_emit_profile_done", 1)[1].split("void hw_telemetry_quiesce", 1)[0]
        final = "service_observer_request(true)" in profile
        sender = src.split("static void* sender_entry", 1)[1].split("if(TEL_LOAD(s_stop.requested))", 1)[0]
        pump = next(line.strip() for line in sender.splitlines() if "service_observer_pump" in line)
        # These are the production sender's two final-tail prerequisites.
        hold = self.condition(sender, "if(line && !TEL_LOAD(s_stop.requested)")
        quiesce = self.condition(sender, "if(TEL_LOAD(s_quiesce_requested)")
        for client in (False, True):
            for capture in (False, True):
                with self.subTest(client=client, capture=capture):
                    pumping = client or not pump.startswith("if(!s_pull")
                    pending, capture_rows, done, quiet = 41, 7 if capture else 0, False, False
                    for step in range(100):
                        g9_pumped = bool(capture_rows and client)
                        if g9_pumped:
                            capture_rows -= 1
                        if final and pumping and pending:
                            if client:
                                pending -= 1  # confirmed delivery only
                            elif step >= 50:
                                pending = 0  # terminal 1 s deadline at 20 ms/pump
                        state = dict(line=True, is_done=True, s_stop_requested=False,
                            s_quiesced=False, g9_pumped=g9_pumped,
                            capture_pending=bool(capture_rows), observer_pending=bool(pending),
                            s_quiesce_requested=True, fifo_drained=done,
                            s_pull=True, s_client_sock=0 if client else -1)
                        if not self.sender_condition(hold, **state):
                            done = True
                        state['fifo_drained'] = done
                        quiet |= self.sender_condition(quiesce, **state)
                    # Capture without a client retains its legacy pending tail;
                    # this observer fix must not add a completion dependency.
                    if client or not capture:
                        self.assertTrue(done, "final tail must finish before teardown, also without pull client")
                    if client:
                        self.assertTrue(quiet, "exact production quiesce gate must open")
        self.assertTrue(final, "freeze observer at profile completion")
        if final:
            self.assertLess(profile.index("service_observer_request(true)"), profile.index("send_line(line)"))

    def test_poll_drain_label_and_no_query_consumers(self):
        observer = self.source("source/platform/service_observer.c")
        self.assertTrue('poll_drain' in observer, "poll-drain timestamps must not claim report arrival")
        src = self.source("source/platform/input.c")
        for name in ("input_pressed", "input_held", "input_frame_pressed", "input_frame_held", "input_frame_released"):
            body = src.split("bool " + name + "(", 1)[1].split("\n}", 1)[0]
            self.assertNotIn("service_observer_", body, name + " must be a pure query")

    def test_seqlock_store_barriers(self):
        src = self.source("source/platform/service_observer.c")
        for name, next_write in (("static void latest_begin", None), ("static void measure", "uint32_t count")):
            body = src.split(name, 1)[1]
            if next_write:
                body = body.split(next_write, 1)[0]
            else:
                body = body.split("static void latest_end", 1)[0]
            self.assertIn("__atomic_thread_fence(__ATOMIC_RELEASE)", body)

    def test_backend_track_shutdown_stop_separation(self):
        src = self.source("source/platform/wii/audio.c")
        self.assertTrue("service_observer_audio(SO_AUDIO_TRACK_STOP" in src)
        self.assertTrue("service_observer_audio(SO_AUDIO_SHUTDOWN" in src)
        self.assertEqual(src.count("service_observer_audio(SO_AUDIO_STOP"), 1)

    def test_ring_wrap_mapping(self):
        src = self.source("source/platform/service_observer.c")
        caps = re.search(r"capacities\[4\] = \{([^}]+)\}", src)
        self.assertIsNotNone(caps)
        for value in map(int, caps[1].split(",")):
            self.assertEqual(value & (value-1), 0, "u32 wrapping index requires power-of-two capacity")

    def test_no_menu_or_loading_required(self):
        rows = self.producer_rows("rows-no-menu")
        with tempfile.TemporaryDirectory() as out:
            c = self.collector(out)
            for row in rows:
                wire.ingest_telemetry_line(c, json.dumps(row))
            self.assertTrue(c.service_observer_report()["coverage_complete"],
                            c.service_observer_report()["errors"])
        # A zeroed census must still fail if a real transition says it occurred.
        rows[-1]["context"].append([1, 140000, 0, 3, 5, 1, 3] + [0] * 13)
        with tempfile.TemporaryDirectory() as out:
            c = self.collector(out)
            for row in rows:
                wire.ingest_telemetry_line(c, json.dumps(row))
            self.assertIn("missing state observations: 3", c.service_observer_report()["errors"])

    def test_profile_done_mid_tick(self):
        with tempfile.TemporaryDirectory() as out:
            c = self.collector(out)
            for row in self.producer_rows("rows-mid-tick"):
                wire.ingest_telemetry_line(c, json.dumps(row))
            report = c.service_observer_report()
            self.assertTrue(report["coverage_complete"], report["errors"])
            self.assertEqual(report["tick_interval"]["end_us"], 67150)
            self.assertEqual(report["tick_interval"]["completed_ticks"], 1)
            self.assertEqual(report["metrics"]["tick_us"]["count"], 1)

    def test_scanned_frame_is_loss_not_abort(self):
        src = self.source("source/platform/wii/gfx.c")
        body = src.split("void gfx_finish(bool vsync)", 1)[1].split("GX_CopyDisp", 1)[0]
        self.assertNotIn("assert(frame != current_frame)", body)
        self.assertIn("service_observer_submit_safe((uintptr_t)frame, (uintptr_t)current_frame)", body)

    def test_real_c_behavior_regressions(self):
        for case in ("consume-bound", "multi-present", "foreign-spans", "final-mid-tick", "scanned-frame"):
            with self.subTest(case=case):
                self.assertEqual(subprocess.run([sys.argv[1], case], capture_output=True).returncode, 0)

    def test_nonzero_census_and_burst_audio_required(self):
        rows = self.producer_rows()
        def report(mutator):
            with tempfile.TemporaryDirectory() as out:
                c = self.collector(out)
                for row in json.loads(json.dumps(rows)):
                    mutator(row)
                    wire.ingest_telemetry_line(c, json.dumps(row))
                return c.service_observer_report()
        mutations = {
            "state observations": lambda r: [state.__setitem__(1, 0) for state in r["states"]],
            "input observations": lambda r: [d.__setitem__(1, 0) for d in r["input"]],
            "cap/checkpoint context": lambda r: r.update(context=[c for c in r["context"] if c[0] not in (4, 5)]),
            "music across bursts": lambda r: [c.__setitem__(18, 0) for c in r["context"] if c[0] in (4, 5)],
        }
        for reason, mutate in mutations.items():
            with self.subTest(reason=reason):
                result = report(mutate)
                self.assertFalse(result["coverage_complete"], reason)

    def collector(self, out):
        return wire.Collector(wire._selftest_args(Path(out), "so-test", "far_idle_2m"))

    def test_missing_final_overflow_and_bad_rows(self):
        with tempfile.TemporaryDirectory() as out:
            c = self.collector(out)
            self.assertFalse(c.service_observer_report()["coverage_complete"])
            row = {"event": "service_observer", "v": 2, "input_boundary": "poll_drain", "run_id": "so-test",
                   "seq": 1, "lane": 0, "at_us": 1000000, "final": 1,
                   "build": "abc_06.27", "config_id": 17, "origin_us": 100,
                   "clock": "monotonic_us", "resolution_us": 1,
                   "presentation": "render_submit_proxy", "reserve": "time_head_estimate",
                   "storage": 16000, "metrics": [], "context": [], "states": [],
                   "loss": [0, 1, 0, 0], "input": [], "audio": []}
            wire.ingest_telemetry_line(c, json.dumps(row))
            r = c.service_observer_report()
            self.assertFalse(r["coverage_complete"])
            self.assertIn("overflow", " ".join(r["errors"]))
            del row["loss"]
            row["seq"] = 2
            wire.ingest_telemetry_line(c, json.dumps(row))
            self.assertIn("schema", " ".join(c.service_observer_report()["errors"]))

    def test_c_producer_rows_and_sequence_loss(self):
        if len(sys.argv) < 2 or not Path(sys.argv[1]).exists():
            self.skipTest("C fixture executable not yet built")
        rows = subprocess.check_output([sys.argv[1], "rows"], text=True).splitlines()
        rows = [r for r in rows if r.startswith('{"event":"service_observer"')]
        self.assertTrue(rows)
        with tempfile.TemporaryDirectory() as out:
            c = self.collector(out)
            for line in rows:
                wire.ingest_telemetry_line(c, line)
            r = c.service_observer_report()
            self.assertFalse(r["hardware_verified"])
            self.assertFalse(r["presentation_proven"])
            self.assertEqual(r["metrics"]["frame_gap"]["max_us"], 75000)
            self.assertEqual(r["metrics"]["tick_us"]["max_us"], 65000)
            self.assertEqual(r["metrics"]["tick_us"]["maximum"]["tick"], 1)
            self.assertEqual(r["tick_interval"]["completed_ticks"], 1)
            self.assertEqual(r["tick_interval"]["end_us"], 67150)
            self.assertFalse(r["tick_interval"]["partial_tick_included"])
            self.assertEqual(r["metrics"]["frame_gap"]["bins"][5], 2)
            self.assertTrue(r["final_seen"])
            self.assertTrue(r["coverage_complete"], r["errors"])
            self.assertEqual(r["metrics"]["audio_reserve_estimate"]["min_us"], 0)
            self.assertEqual(r["metrics"]["audio_reserve_estimate"]["minimum"]["action"], 2)
            self.assertEqual(r["audio"][1:5], [1, 1, 1, 1])
            self.assertEqual(r["audio"][8:10], [1, 1])
            self.assertEqual(r["audio"][10], 133000)
            self.assertEqual(r["audio"][13], 1)
            self.assertEqual(r["metrics"]["poll_drain_presentation_age"]["count"], 1)
            self.assertFalse(r["receipt_latency_proven"])
        with tempfile.TemporaryDirectory() as out:
            c = self.collector(out)
            for line in rows[1:]:
                wire.ingest_telemetry_line(c, line)
            self.assertIn("sequence", " ".join(c.service_observer_report()["errors"]))
        # Actual producer rows, with individually corrupted mandatory fields.
        for mutation in (lambda r: r.update(config_id=0),
                         lambda r: r.update(storage=16385),
                         lambda r: r.update(at_us=0),
                         lambda r: r.update(build="wrong-build"),
                         lambda r: r.update(loss=[0, 0, 1, 0]),
                         lambda r: r.update(loss=[0, 0, 0, 1]),
                         lambda r: r["metrics"][0].__setitem__(16, 999)):
            with tempfile.TemporaryDirectory() as out:
                c = self.collector(out)
                for index, line in enumerate(rows):
                    row = json.loads(line)
                    if index == 1:
                        mutation(row)
                    wire.ingest_telemetry_line(c, json.dumps(row))
                self.assertFalse(c.service_observer_report()["coverage_complete"])

    def test_corrupt_completed_tick_boundary_rejected(self):
        for mutate in (lambda r: r.pop("tick_end_us"),
                       lambda r: r.update(tick_end_us="corrupt"),
                       lambda r: r.update(tick_end_us=r["origin_us"]),
                       lambda r: r.update(completed_ticks=2)):
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as out:
                c = self.collector(out)
                for row in self.producer_rows():
                    if row["final"] and row["lane"] == 1:
                        mutate(row)
                    wire.ingest_telemetry_line(c, json.dumps(row))
                self.assertFalse(c.service_observer_report()["coverage_complete"])


if __name__ == "__main__":
    # R18: these hardware tapes hold a private rig IP and stay out of the
    # public export (docs/release-allowlist.json); skip (77) where absent.
    _r18_tapes = ("g9_g9_1_s12345.ndjson.bin", "g9_x_x_g9a_s12345.ndjson.bin",
                  "g9_x_x_g9b_s12345.ndjson.bin", "g9_y_y_g9a_s12345.ndjson.bin",
                  "g9_y_y_g9b_s12345.ndjson.bin", "g9_x_provenance.json", "g9_y_provenance.json")
    _r18_root = __import__("pathlib").Path(__file__).resolve().parent / "fixtures"
    if not all((_r18_root / _t).is_file() for _t in _r18_tapes):
        print("SKIP: private G9 hardware tapes are not in this tree (public export)")
        raise SystemExit(77)
    if os.environ.get("CW_SO_REGRESSIONS_ONLY"):
        names = [name for name in unittest.defaultTestLoader.getTestCaseNames(ObserverTest)
                 if name not in ("test_c_producer_rows_and_sequence_loss", "test_missing_final_overflow_and_bad_rows")]
        result = unittest.TextTestRunner().run(unittest.TestSuite(ObserverTest(name) for name in names))
        sys.exit(not result.wasSuccessful())
    unittest.main(argv=[sys.argv[0]])
