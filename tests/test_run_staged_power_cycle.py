#!/usr/bin/env python3
"""Host-side regression for staged-run power-cycle-on-failure wiring.

This does not prove a smart plug, SwitchBot, Shelly, Priiloader, or Wii
recovery path. It proves the PC runner invokes the configured power hook after a
failed staged telemetry collection and still returns the original failure.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


def read_line(conn: socket.socket) -> str:
    data = bytearray()
    while True:
        b = conn.recv(1)
        if not b:
            raise RuntimeError("client closed while reading line")
        if b == b"\n":
            return data.decode("ascii").rstrip("\r")
        if b != b"\r":
            data.extend(b)


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(min(65536, size - len(data)))
        if not chunk:
            raise RuntimeError(f"client closed after {len(data)} / {size} bytes")
        data.extend(chunk)
    return bytes(data)


def put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def write_minimal_launchable_dol(path: Path) -> None:
    dol = bytearray(0x200)
    put_be32(dol, 0x00, 0x100)
    put_be32(dol, 0x48, 0x80004000)
    put_be32(dol, 0x90, 0x100)
    put_be32(dol, 0xD8, 0x80006000)
    put_be32(dol, 0xDC, 0x2000)
    put_be32(dol, 0xE0, 0x80004020)
    put_be32(dol, 0x124, 0x5F617267)
    path.write_bytes(bytes(dol))


def start_stage_server(events: list[str]):
    ready: list[int] = []
    errors: list[BaseException] = []

    def send_json(conn: socket.socket, obj: dict) -> None:
        conn.sendall(json.dumps(obj, separators=(",", ":")).encode("ascii") + b"\n")

    def send_manifest(conn: socket.socket) -> None:
        send_json(
            conn,
            {
                "status": "PASS",
                "reason": "manifest",
                "protocol": "CWSTAGE4",
                "capabilities": ["put", "manifest", "runprep", "run-experimental", "auth"],
                "files": [],
            },
        )

    def handle_stage(conn: socket.socket, first_line: str) -> None:
        received: dict[str, tuple[int, str]] = {}
        line = first_line
        while True:
            parts = line.split(" ")
            if parts[0] == "CWAUTH1":
                send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
                events.append("auth")
                line = read_line(conn)
                continue
            if parts[0] in ("CWSTAGE4", "CWSTAGE4R"):
                size = int(parts[1])
                expected_sha = parts[2]
                target = parts[3]
                payload = read_exact(conn, size)
                actual_sha = hashlib.sha256(payload).hexdigest()
                assert actual_sha == expected_sha, (target, actual_sha, expected_sha)
                received[target] = (size, expected_sha)
                events.append(f"stage {target}")
                send_json(
                    conn,
                    {
                        "status": "PASS",
                        "reason": "staged",
                        "bytes": size,
                        "sha256": expected_sha,
                    },
                )
                line = read_line(conn)
                continue
            if parts[0] in ("CWRUNPREP1", "CWRUN1"):
                dol_size, dol_sha = received[parts[1]]
                args_size, args_sha = received[parts[2]]
                launching = parts[0] == "CWRUN1"
                events.append("run" if launching else "runprep")
                send_json(
                    conn,
                    {
                        "status": "PASS",
                        "reason": "launching" if launching else "run_ready",
                        "protocol": "CWSTAGE4",
                        "dol_bytes": dol_size,
                        "dol_sha256": dol_sha,
                        "args_bytes": args_size,
                        "args_sha256": args_sha,
                        "entry_point": "0x80004020",
                        "min_address": "0x80004000",
                        "max_address": "0x80008000",
                        "sections": 2,
                        "argv_count": 11,
                        "argv_marker_address": "0x80004024",
                        "argv_marker_file_offset": 292,
                        "argv_struct_address": "0x80004028",
                        "argv_struct_bytes": 24,
                        "command_line_address": "0x80006000",
                        "command_line_bytes": max(1, args_size),
                    },
                )
                if launching:
                    return
                line = read_line(conn)
                continue
            raise AssertionError(line)

    def server() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", 0))
                s.listen(4)
                s.settimeout(30)
                ready.append(s.getsockname()[1])
                while True:
                    conn, _addr = s.accept()
                    with conn:
                        first = read_line(conn)
                        if first == "CWHELLO1":
                            events.append("manifest")
                            send_json(
                                conn,
                                {
                                    "status": "PASS",
                                    "reason": "hello",
                                    "protocol": "CWSTAGE4",
                                    "capabilities": ["put", "manifest", "runprep", "auth"],
                                    "token_provisioned": False,
                                },
                            )
                            request = read_line(conn)
                            if request == "CWAUTH1" or request.startswith("CWAUTH1 "):
                                send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
                                request = read_line(conn)
                            assert request == "CWMANIFEST1", request
                            send_manifest(conn)
                            continue
                        handle_stage(conn, first)
                        return
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("mock stage server did not bind")
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


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def assert_stage_default_refuses_hbc_upload(
    repo: Path, powershell: str, dol: Path
) -> None:
    proc = subprocess.run(
        [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            "tools/stage_wii_dol.ps1",
            "-WiiHost",
            "127.0.0.1",
            "-DolPath",
            str(dol),
            "-RunId",
            "stage_hbc_guard_mock",
            "-SkipServerBuild",
            "-AllowDirty",
        ],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=60,
    )
    assert proc.returncode != 0, proc.stdout
    assert "refusing to upload the stage server through HBC by default" in proc.stdout
    assert "[wiiload-stage]" not in proc.stdout, proc.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    stage_events: list[str] = []
    power_paths: list[str] = []
    stage_port, stage_thread, stage_errors = start_stage_server(stage_events)
    power_port, power_thread, power_errors = start_power_server(power_paths)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        assert_stage_default_refuses_hbc_upload(repo, args.powershell, dol)
        run_id = f"power_cycle_mock_{stage_port}"
        proc = subprocess.run(
            [
                args.powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "tools/run_staged_wii_hardware.ps1",
                "-WiiHost",
                "127.0.0.1",
                "-StagePort",
                str(stage_port),
                "-TelemetryPort",
                str(telemetry_port),
                "-StageConnectTimeoutSeconds",
                "5",
                "-TelemetryConnectTimeoutSeconds",
                "1",
                "-StageMailboxConnectTimeoutSeconds",
                "5",
                "-RunId",
                run_id,
                "-DolPath",
                str(dol),
                "-LaunchAfterStage",
                "-SkipStageMailboxFetch",
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

    stage_thread.join(5)
    power_thread.join(5)
    if stage_thread.is_alive():
        raise AssertionError("mock stage server did not exit")
    if power_thread.is_alive():
        raise AssertionError("mock power server did not receive both requests")
    if stage_errors:
        raise stage_errors[0]
    if power_errors:
        raise power_errors[0]

    output = proc.stdout
    if proc.returncode == 0:
        sys.stderr.write(output)
        raise AssertionError("staged run unexpectedly succeeded")
    assert stage_events == [
        "auth",
        "stage sd:/apps/CavEX/boot.dol",
        "stage sd:/apps/CavEX/cw_run_args.txt",
        "runprep",
        "run",
    ], stage_events
    assert power_paths == ["/off", "/on"], power_paths
    assert "[POWER-CYCLE] mode=off-on" in output, output
    assert "[FAIL]" in output, output

    print("PASS test_run_staged_power_cycle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
