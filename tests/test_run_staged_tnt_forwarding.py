"""The staged runner passes the TNT requirement through both wrapper layers."""
import argparse
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_wii_profile_matrix import (  # noqa: E402
    start_stage_server, unused_tcp_port, write_minimal_launchable_dol)

PARSER = argparse.ArgumentParser()
PARSER.add_argument("--powershell", required=True)
ARGS = PARSER.parse_args()
sys.argv = [sys.argv[0]]


class TntForwardingTest(unittest.TestCase):
    def test_runner_and_collector_accept_tnt_requirement(self):
        runner = (ROOT / "tools/run_staged_wii_hardware.ps1").read_text()
        collector = (ROOT / "tools/collect_staged_wii.ps1").read_text()
        self.assertTrue("[switch]$RequireTntEvidence" in runner)
        self.assertTrue('$collectArgs += "-RequireTntEvidence"' in runner)
        self.assertTrue("[switch]$RequireTntEvidence" in collector)
        self.assertTrue('$args += "--require-tnt-evidence"' in collector)

    def test_real_runner_forwards_to_collector_process(self):
        events, staged = [], []
        port, thread, errors = start_stage_server(events, staged, runs=1)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dol = root / "minimal.dol"
            write_minimal_launchable_dol(dol)
            argv_file = root / "collector-args.txt"
            if os.name == "nt":
                shim = root / "capture-python.cmd"
                shim.write_text(f'@echo off\necho %* > "{argv_file}"\nexit /b 0\n')
            else:
                shim = root / "capture-python.sh"
                shim.write_text(f'#!/bin/sh\nprintf "%s\\n" "$*" > "{argv_file}"\n')
                shim.chmod(0o755)
            env = os.environ.copy()
            if os.name == "nt":
                env["PSModulePath"] = r"C:\Windows\system32\WindowsPowerShell\v1.0\Modules"
            result = subprocess.run([
                ARGS.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", "tools/run_staged_wii_hardware.ps1",
                "-WiiHost", "127.0.0.1", "-Profile", "heartbeat_selftest",
                "-RunId", "tnt_forward_mock", "-OutRoot", str(root / "run"),
                "-DolPath", str(dol), "-StagePort", str(port),
                "-TelemetryPort", str(unused_tcp_port()),
                "-StageConnectTimeoutSeconds", "5",
                "-TelemetryConnectTimeoutSeconds", "1",
                "-StageMailboxConnectTimeoutSeconds", "5",
                "-Python", str(shim), "-ExpectedBuild", "abc_06.27",
                "-RequireTntEvidence", "-LaunchAfterStage",
                "-SkipStageMailboxFetch", "-AllowDirty",
            ], cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
            thread.join(5)
            self.assertFalse(thread.is_alive(), result.stdout + result.stderr)
            self.assertFalse(errors, str(errors) + result.stdout + result.stderr)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(argv_file.exists(), result.stdout + result.stderr)
            self.assertIn("--require-tnt-evidence", argv_file.read_text())


if __name__ == "__main__":
    unittest.main()
