#!/usr/bin/env python3
"""Host-side regression for direct run_wii_hardware power recovery wiring.

This uses a fake wiiload executable and localhost sockets. It proves the direct
runner invokes the configured power hook after collector failure and still
returns failure; it does not prove Wii launch, HBC, or hardware recovery.
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


def start_telemetry_closer(events: list[str]):
    ready: list[int] = []
    errors: list[BaseException] = []

    def server() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", 0))
                s.listen(1)
                s.settimeout(30)
                ready.append(s.getsockname()[1])
                conn, _addr = s.accept()
                events.append("telemetry_connect")
                conn.close()
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("telemetry closer did not bind")
    return ready[0], thread, errors


def start_power_server(paths: list[str]):
    ready: list[int] = []
    errors: list[BaseException] = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - stdlib callback name
            paths.append(self.path)
            self.send_response(204)
            self.end_headers()

        def log_message(self, _fmt: str, *_args: object) -> None:
            return

    def server() -> None:
        try:
            httpd = HTTPServer(("127.0.0.1", 0), Handler)
            ready.append(httpd.server_port)
            httpd.timeout = 30
            while len(paths) < 2:
                httpd.handle_request()
            httpd.server_close()
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("mock power server did not bind")
    return ready[0], thread, errors


def write_fake_wiiload(path: Path) -> None:
    if os.name == "nt":
        path.write_text("@echo off\r\necho fake wiiload %*\r\nexit /b 0\r\n", encoding="ascii")
    else:
        path.write_text('#!/bin/sh\necho fake wiiload "$@"\nexit 0\n', encoding="ascii")
        path.chmod(0o755)


def assert_default_refuses_hbc_upload(repo: Path, powershell: str, wiiload: Path) -> None:
    proc = subprocess.run(
        [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            "tools/run_wii_hardware.ps1",
            "-WiiHost",
            "127.0.0.1",
            "-Profile",
            "heartbeat_selftest",
            "-RunId",
            "direct_hbc_guard_mock",
            "-OutRoot",
            str(wiiload.parent / "guard_run"),
            "-WiiloadExe",
            str(wiiload),
            "-WiiloadAttempts",
            "1",
            "-SkipBuild",
            "-AllowDirty",
        ],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=60,
    )
    assert proc.returncode != 0, proc.stdout
    assert "refusing HBC wiiload upload by default" in proc.stdout, proc.stdout
    assert "fake wiiload" not in proc.stdout, proc.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    source_repo = Path(args.repo).resolve()

    telemetry_events: list[str] = []
    power_paths: list[str] = []
    telemetry_port, telemetry_thread, telemetry_errors = start_telemetry_closer(
        telemetry_events
    )
    power_port, power_thread, power_errors = start_power_server(power_paths)

    with tempfile.TemporaryDirectory() as td:
        # The fake uploader never interprets its input. Test the recovery
        # wiring in a scratch repo, independent of a local Wii build or SD.
        repo = Path(td) / "repo"
        (repo / "tools").mkdir(parents=True)
        for name in ("run_wii_hardware.ps1", "invoke_wii_power_cycle.ps1"):
            shutil.copyfile(source_repo / "tools" / name, repo / "tools" / name)
        # The real collector imports its sibling evidence graders at startup.
        for module in (source_repo / "tools").glob("*.py"):
            shutil.copyfile(module, repo / "tools" / module.name)
        (repo / "CaveWii_v2.dol").write_bytes(b"synthetic uploader input")
        git = ["git", "-c", "user.name=test", "-c", "user.email=test@example.local"]
        subprocess.run(git + ["init", "-q"], cwd=repo, check=True)
        subprocess.run(git + ["add", "tools"], cwd=repo, check=True)
        subprocess.run(git + ["commit", "-q", "-m", "fixture"], cwd=repo, check=True)
        wiiload = Path(td) / "fake_wiiload.cmd"
        write_fake_wiiload(wiiload)
        assert_default_refuses_hbc_upload(repo, args.powershell, wiiload)
        run_id = f"direct_power_cycle_mock_{telemetry_port}"
        out_root = Path(td) / "run"
        proc = subprocess.run(
            [
                args.powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "tools/run_wii_hardware.ps1",
                "-WiiHost",
                "127.0.0.1",
                "-Profile",
                "heartbeat_selftest",
                "-RunId",
                run_id,
                "-OutRoot",
                str(out_root),
                "-TelemetryPort",
                str(telemetry_port),
                "-WiiloadExe",
                str(wiiload),
                "-WiiloadAttempts",
                "1",
                "-AllowHbcUpload",
                "-SkipBuild",
                "-AllowDirty",
                "-PowerCycleOnFailure",
                "-PowerOffUrl",
                f"http://127.0.0.1:{power_port}/off",
                "-PowerOnUrl",
                f"http://127.0.0.1:{power_port}/on",
                "-PowerCycleDelaySeconds",
                "0",
                "-PowerCycleTimeoutSeconds",
                "5",
            ],
            cwd=repo,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=120,
        )

        collector_stderr = out_root / "collector.stderr.log"
        diagnostics = collector_stderr.read_text() if collector_stderr.is_file() else ""

    telemetry_thread.join(5)
    power_thread.join(5)
    if telemetry_thread.is_alive():
        raise AssertionError("telemetry closer did not exit\n" + proc.stdout + diagnostics)
    if power_thread.is_alive():
        raise AssertionError("mock power server did not receive both requests")
    if telemetry_errors:
        raise telemetry_errors[0]
    if power_errors:
        raise power_errors[0]

    output = proc.stdout
    if proc.returncode == 0:
        sys.stderr.write(output)
        raise AssertionError("direct run unexpectedly succeeded")
    assert telemetry_events == ["telemetry_connect"], telemetry_events
    assert power_paths == ["/off", "/on"], power_paths
    assert "fake wiiload" in output, output
    assert "[POWER-CYCLE] mode=off-on" in output, output
    assert "[FAIL]" in output, output

    print("PASS test_run_wii_power_cycle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
