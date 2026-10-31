#!/usr/bin/env python3
"""Host-side regression for tools/run_wii_profile_matrix.ps1.

This does not prove real Wii autonomy. It proves the matrix runner sequences
multiple staged profile runs, preserves per-run FAIL artifacts, invokes the
configured power hook after each failure through the real staged runner, and
continues when explicitly told to continue.
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


def start_stage_server(events: list[str], staged_args: list[bytes], runs: int):
    ready: list[int] = []
    errors: list[BaseException] = []

    def send_json(conn: socket.socket, obj: dict) -> None:
        conn.sendall(json.dumps(obj, separators=(",", ":")).encode("ascii") + b"\n")

    def handle_one_write(conn: socket.socket, line: str, tag: str) -> None:
        parts = line.split(" ")
        assert parts[0] in ("CWSTAGE4", "CWSTAGE4R"), line
        size = int(parts[1])
        expected_sha = parts[2]
        target = parts[3]
        payload = read_exact(conn, size)
        actual_sha = hashlib.sha256(payload).hexdigest()
        assert actual_sha == expected_sha, (target, actual_sha, expected_sha)
        if target == "sd:/apps/CavEX/cw_run_args.txt" and tag == "stage":
            staged_args.append(payload)
        events.append(f"{tag} {target}")
        send_json(
            conn,
            {
                "status": "PASS",
                "reason": "staged",
                "bytes": size,
                "sha256": expected_sha,
            },
        )

    def handle_stage(conn: socket.socket, first_line: str) -> None:
        received: dict[str, tuple[int, str]] = {}
        line = first_line
        while True:
            parts = line.split(" ")
            if parts[0] == "CWAUTH1":
                send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
                line = read_line(conn)
                continue
            if parts[0] in ("CWSTAGE4", "CWSTAGE4R"):
                size, expected_sha, target = int(parts[1]), parts[2], parts[3]
                handle_one_write(conn, line, "stage")
                received[target] = (size, expected_sha)
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

    def handle_neutralize_connection(conn: socket.socket) -> None:
        """put_wii_stage_file.ps1's own protocol shape (2026-09-02
        cw_run_args.txt hardening, tools/put_wii_stage_file.ps1): CWHELLO1
        -> hello reply -> CWAUTH1 -> auth reply -> exactly one
        CWSTAGE4/CWSTAGE4R -> the client closes right after the PASS reply.
        run_staged_wii_hardware.ps1 opens a SEPARATE connection for this
        whenever a staged run does not end with positive heartbeat evidence
        (never true here -- neither test provides a telemetry mock), so one
        of these follows every main stage connection below."""
        first = read_line(conn)
        assert first == "CWHELLO1", first
        send_json(
            conn,
            {
                "status": "PASS",
                "reason": "hello",
                "protocol": "CWSTAGE4",
                "capabilities": ["put", "auth"],
                "token_provisioned": False,
            },
        )
        line = read_line(conn)
        assert line == "CWAUTH1" or line.startswith("CWAUTH1 "), line
        send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
        line = read_line(conn)
        handle_one_write(conn, line, "neutralize")

    def server() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", 0))
                s.listen(8)
                s.settimeout(30)
                ready.append(s.getsockname()[1])
                for _ in range(runs):
                    conn, _addr = s.accept()
                    with conn:
                        handle_stage(conn, read_line(conn))
                    conn2, _addr2 = s.accept()
                    with conn2:
                        handle_neutralize_connection(conn2)
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("stage server did not bind")
    return ready[0], thread, errors


def start_power_server(paths: list[str], expected: int):
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
            while len(paths) < expected:
                httpd.handle_request()
            httpd.server_close()
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("power server did not bind")
    return ready[0], thread, errors


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    profiles = ["heartbeat_selftest", "split_anchor_6m"]
    stage_events: list[str] = []
    staged_args: list[bytes] = []
    power_paths: list[str] = []
    stage_port, stage_thread, stage_errors = start_stage_server(
        stage_events, staged_args, runs=len(profiles)
    )
    power_port, power_thread, power_errors = start_power_server(
        power_paths, expected=len(profiles) * 2
    )
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        temp_root = Path(td)
        dol = temp_root / "minimal.dol"
        out_root = temp_root / "matrix"
        write_minimal_launchable_dol(dol)
        proc = subprocess.run(
            [
                args.powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "tools/run_wii_profile_matrix.ps1",
                "-WiiHost",
                "127.0.0.1",
                "-Mode",
                "staged",
                "-Profiles",
                ",".join(profiles),
                "-RunIdPrefix",
                "matrix_mock",
                "-OutRoot",
                str(out_root),
                "-DolPath",
                str(dol),
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
                "-LaunchAfterStage",
                "-SkipStageMailboxFetch",
                "-AllowDirty",
                "-FarSendNearOwed",
                "16",
                "-AnchorSep",
                "96",
                "-Seed",
                "12345",
                "-ContinueOnFailure",
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
            timeout=180,
        )

        stage_thread.join(5)
        power_thread.join(5)
        if stage_thread.is_alive():
            sys.stderr.write(proc.stdout)
            sys.stderr.write(f"\nstage_events={stage_events!r}\n")
            sys.stderr.write(f"power_paths={power_paths!r}\n")
            raise AssertionError("stage server did not exit")
        if power_thread.is_alive():
            sys.stderr.write(proc.stdout)
            sys.stderr.write(f"\nstage_events={stage_events!r}\n")
            sys.stderr.write(f"power_paths={power_paths!r}\n")
            raise AssertionError("power server did not receive expected requests")
        if stage_errors:
            raise stage_errors[0]
        if power_errors:
            raise power_errors[0]

        output = proc.stdout
        if proc.returncode == 0:
            sys.stderr.write(output)
            raise AssertionError("matrix unexpectedly succeeded despite failed runs")

        results_path = out_root / "matrix_results.json"
        summary_path = out_root / "matrix_summary.md"
        assert results_path.exists()
        assert summary_path.exists()
        result_doc = json.loads(results_path.read_text(encoding="utf-8-sig"))
        assert result_doc["started_profiles"] == 2, result_doc
        assert result_doc["requested_profiles"] == 2, result_doc
        assert result_doc["any_failure"] is True, result_doc
        assert [r["profile"] for r in result_doc["results"]] == profiles
        assert all(r["status"] == "FAIL" for r in result_doc["results"])
        assert all(r["failure_class"] == "telemetry_connect_failed"
                   for r in result_doc["results"]), result_doc
        for r in result_doc["results"]:
            assert Path(r["artifact_path"]).exists(), r

    assert stage_events.count("run") == 2, stage_events
    assert len(staged_args) == 2, staged_args
    assert all(b"--cw-far-send-near-owed 16" in value for value in staged_args), staged_args
    assert all(b"--cw-anchor-sep 96" in value for value in staged_args), staged_args
    assert any(b"--cw-profile split_anchor_6m" in value for value in staged_args), staged_args
    assert all(b"--cw-seed 12345" in value for value in staged_args), staged_args
    assert power_paths == ["/off", "/on", "/off", "/on"], power_paths
    assert "[MATRIX]" in output, output
    assert output.count("[POWER-CYCLE] mode=off-on") == 2, output

    print("PASS test_wii_profile_matrix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
