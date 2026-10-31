#!/usr/bin/env python3
"""Host-side regression test for tools/run_wii_lighting_bench.ps1.

This does not prove real Wii timing. It proves the PC runner can parse benchmark
output and speak the stage-server fetch protocol against a local mock, so the
hardware path does not silently drift while waiting for console runs.
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
from pathlib import Path


RESULT_PAYLOAD = (
    b"bench_lighting_budget: budget=4096 iters=200 avg_slice_us=333.25 "
    b"ns_per_pop=81.36 avg_light_writes=2716.0 grid=48x96x48\n"
)


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


def run_cmd(cmd: list[str], cwd: Path, timeout: int = 60) -> str:
    proc = subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise AssertionError(f"command failed with exit {proc.returncode}: {cmd}")
    return proc.stdout


def start_server(handler, connections: int = 1):
    ready: list[int] = []
    errors: list[BaseException] = []

    def server() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", 0))
                ready.append(s.getsockname()[1])
                s.listen(1)
                for _ in range(connections):
                    conn, _addr = s.accept()
                    with conn:
                        handler(conn)
        except BaseException as exc:  # propagated after client exits
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("mock server did not bind a port")
    return ready[0], thread, errors


def fetch_handler(conn: socket.socket) -> None:
    hello = read_line(conn)
    assert hello == "CWHELLO1", hello
    conn.sendall(
        b'{"status":"PASS","reason":"hello","protocol":"CWSTAGE4",'
        b'"capabilities":["fetch","manifest","auth"],"port":0,'
        b'"token_provisioned":false}\n'
    )
    request = read_line(conn)
    if request == "CWAUTH1" or request.startswith("CWAUTH1 "):
        conn.sendall(b'{"status":"PASS","reason":"no_token_provisioned"}\n')
        request = read_line(conn)
    sha = hashlib.sha256(RESULT_PAYLOAD).hexdigest()
    if request == "CWMANIFEST1":
        reply = {
            "status": "PASS",
            "reason": "manifest",
            "protocol": "CWSTAGE4",
            "port": 0,
            "capabilities": ["fetch", "manifest"],
            "files": [
                {"path": "sd:/hang_report.txt", "exists": False, "bytes": 0},
                {"path": "sd:/fbguard_report.txt", "exists": False, "bytes": 0},
                {
                    "path": "sd:/lighting_bench.txt",
                    "exists": True,
                    "bytes": len(RESULT_PAYLOAD),
                    "sha256": sha,
                },
            ],
        }
        conn.sendall((json.dumps(reply, separators=(",", ":")) + "\n").encode())
        return
    assert request == "CWFETCH1 sd:/lighting_bench.txt", request
    header = (
        f'{{"status":"PASS","reason":"fetch","bytes":{len(RESULT_PAYLOAD)},'
        f'"sha256":"{sha}"}}\n'
    ).encode("ascii")
    conn.sendall(header + RESULT_PAYLOAD)


def run_fetch_mock(repo: Path, powershell: str, temp_root: Path) -> None:
    port, thread, errors = start_server(fetch_handler, connections=2)
    out_root = temp_root / "fetch"
    output = run_cmd(
        [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            "tools/run_wii_lighting_bench.ps1",
            "-WiiHost",
            "127.0.0.1",
            "-StagePort",
            str(port),
            "-FetchOnly",
            "-FetchConnectTimeoutSeconds",
            "5",
            "-OutRoot",
            str(out_root),
        ],
        repo,
    )
    thread.join(5)
    if thread.is_alive():
        raise AssertionError("fetch mock server did not exit")
    if errors:
        raise errors[0]
    assert "[BENCH-MANIFEST]" in output, output
    assert "[BENCH-RESULT]" in output, output
    manifest = json.loads(
        (out_root / "stage_manifest_before_fetch.json").read_text(
            encoding="utf-8-sig"
        )
    )
    entry = next(
        item for item in manifest["files"] if item["path"] == "sd:/lighting_bench.txt"
    )
    assert entry["exists"] is True
    assert entry["bytes"] == len(RESULT_PAYLOAD)
    assert entry["sha256"] == hashlib.sha256(RESULT_PAYLOAD).hexdigest()
    result = json.loads(
        (out_root / "lighting_bench_result.json").read_text(encoding="utf-8-sig")
    )
    assert result["status"] == "PASS"
    assert result["remote_result_path"] == "sd:/lighting_bench.txt"
    assert result["stage_manifest"]["remote_path"] == "sd:/lighting_bench.txt"
    assert result["stage_manifest"]["bytes"] == len(RESULT_PAYLOAD)
    assert result["stage_manifest"]["sha256"] == hashlib.sha256(
        RESULT_PAYLOAD
    ).hexdigest()
    assert result["result"]["budget"] == 4096
    assert result["result"]["avg_slice_us"] == 333.25
    assert result["result"]["grid_x"] == 48
    assert result["result"]["grid_y"] == 96
    assert result["result"]["grid_z"] == 48


def stage_handler(conn: socket.socket) -> None:
    received: dict[str, tuple[int, str, bytes]] = {}
    while True:
        line = read_line(conn)
        parts = line.split(" ")
        if parts[0] == "CWAUTH1":
            conn.sendall(b'{"status":"PASS","reason":"no_token_provisioned"}\n')
            continue
        if parts[0] in ("CWSTAGE4", "CWSTAGE4R"):
            assert len(parts) == 4, line
            size = int(parts[1])
            expected_sha = parts[2]
            target = parts[3]
            payload = read_exact(conn, size)
            actual_sha = hashlib.sha256(payload).hexdigest()
            assert actual_sha == expected_sha, (target, actual_sha, expected_sha)
            received[target] = (size, expected_sha, payload)
            reply = {
                "status": "PASS",
                "reason": "staged",
                "bytes": size,
                "sha256": expected_sha,
            }
            conn.sendall((json.dumps(reply, separators=(",", ":")) + "\n").encode())
            continue
        if parts[0] == "CWRUNPREP1":
            assert len(parts) == 3, line
            dol_size, dol_sha, _ = received[parts[1]]
            args_size, args_sha, _ = received[parts[2]]
            reply = {
                "status": "PASS",
                "reason": "run_ready",
                "protocol": "CWSTAGE4",
                "dol_bytes": dol_size,
                "dol_sha256": dol_sha,
                "args_bytes": args_size,
                "args_sha256": args_sha,
                "entry_point": "0x80003f00",
                "min_address": "0x80003f00",
                "max_address": "0x80100000",
                "sections": 3,
                "argv_count": 9,
                "argv_marker_address": "0x80003f04",
                "argv_marker_file_offset": 256,
                "argv_struct_address": "0x80003f08",
                "argv_struct_bytes": 24,
                "command_line_address": "0x800f0000",
                "command_line_bytes": max(1, args_size),
            }
            conn.sendall((json.dumps(reply, separators=(",", ":")) + "\n").encode())
            return
        raise AssertionError(line)


def run_stage_mock_if_artifact_exists(repo: Path, powershell: str) -> None:
    dol = repo / "tools" / "wii_lighting_bench" / "wii_lighting_bench.dol"
    verifier = repo / "build-verify" / "verify_wii_launch_plan.exe"
    if not dol.exists() or not verifier.exists():
        print("SKIP stage mock: benchmark DOL or launch-plan verifier is not built")
        return

    port, thread, errors = start_server(stage_handler)
    output = run_cmd(
        [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            "tools/run_wii_lighting_bench.ps1",
            "-WiiHost",
            "127.0.0.1",
            "-StagePort",
            str(port),
            "-SkipBuild",
            "-NoLaunch",
            "-AllowDirty",
            "-StageConnectTimeoutSeconds",
            "5",
        ],
        repo,
        timeout=90,
    )
    thread.join(5)
    if thread.is_alive():
        raise AssertionError("stage mock server did not exit")
    if errors:
        raise errors[0]
    assert "[stage-result]" in output, output
    assert "[RUNPREP]" in output, output
    assert "[NEXT] Launch the staged benchmark from HBC" in output, output


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    powershell = args.powershell
    run_cmd(
        [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            "tools/run_wii_lighting_bench.ps1",
            "-SelfTest",
        ],
        repo,
    )
    with tempfile.TemporaryDirectory() as td:
        run_fetch_mock(repo, powershell, Path(td))
    run_stage_mock_if_artifact_exists(repo, powershell)
    print("PASS test_wii_lighting_bench_runner")
    return 0


if __name__ == "__main__":
    sys.exit(main())
