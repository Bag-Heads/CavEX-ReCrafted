#!/usr/bin/env python3
"""Host-side regression test for tools/run_staged_wii_hardware.ps1.

This does not prove real Wii networking or DOL launch. It proves the PC-side
staged runner preserves all stage-mailbox proof files advertised by the
manifest before it stages the next DOL.
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


PAYLOADS = {
    "sd:/cw_boot_trace.txt": (
        b"phase=wii_after_fat build=abc run_id=previous "
        b"profile=heartbeat_selftest\n"
    ),
    "sd:/hang_report.txt": b"previous hang report\nphase=WDM_LIGHTING\n",
    "sd:/fbguard_report.txt": b"previous fbguard report\n",
    "sd:/lighting_bench.txt": (
        b"bench_lighting_budget: budget=4096 iters=200 avg_slice_us=333.25 "
        b"ns_per_pop=81.36 avg_light_writes=2716.0 grid=48x96x48\n"
    ),
}


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


def run_cmd(cmd: list[str], cwd: Path, timeout: int = 90) -> str:
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


def put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def write_minimal_launchable_dol(path: Path) -> None:
    dol = bytearray(0x200)
    put_be32(dol, 0x00, 0x100)  # text offset 0
    put_be32(dol, 0x48, 0x80004000)  # text address 0
    put_be32(dol, 0x90, 0x100)  # text length 0
    put_be32(dol, 0xD8, 0x80006000)  # bss address
    put_be32(dol, 0xDC, 0x2000)  # bss length
    put_be32(dol, 0xE0, 0x80004020)  # entry point
    put_be32(dol, 0x124, 0x5F617267)  # CW_DOL_ARGV_MAGIC at entry+4
    path.write_bytes(bytes(dol))


def start_stage_server(events: list[str]):
    ready: list[int] = []
    errors: list[BaseException] = []

    def send_json(conn: socket.socket, obj: dict) -> None:
        line = json.dumps(obj, separators=(",", ":")).encode("ascii") + b"\n"
        conn.sendall(line)

    def manifest_reply() -> dict:
        files = [
            {"path": "sd:/CAVEWII_PROOF_LAUNCHER.json", "exists": False, "bytes": 0},
            {
                "path": "sd:/apps/CaveWiiStage/proof_launcher_manifest.json",
                "exists": False,
                "bytes": 0,
            },
            {"path": "sd:/apps/CavEX/boot.dol", "exists": False, "bytes": 0},
            {
                "path": "sd:/apps/CavEX/cw_run_args.txt",
                "exists": False,
                "bytes": 0,
            },
        ]
        for path, payload in PAYLOADS.items():
            files.append(
                {
                    "path": path,
                    "exists": True,
                    "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
            )
        return {
            "status": "PASS",
            "reason": "manifest",
            "protocol": "CWSTAGE4",
            "port": 0,
            "capabilities": ["put", "fetch", "manifest", "runprep", "run-experimental", "auth"],
            "files": files,
        }

    def handle_hello(conn: socket.socket) -> None:
        send_json(
            conn,
            {
                "status": "PASS",
                "reason": "hello",
                "protocol": "CWSTAGE4",
                "capabilities": ["put", "fetch", "manifest", "runprep", "auth"],
                "port": 0,
                "token_provisioned": False,
            },
        )
        request = read_line(conn)
        if request == "CWAUTH1" or request.startswith("CWAUTH1 "):
            # Soft-launch (docs/stage_server_auth_2026-09-02.md): this mock
            # server never provisions a token, mirroring the live rig's
            # current no-token state -- always PASS.
            send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
            request = read_line(conn)
        if request == "CWMANIFEST1":
            events.append("manifest")
            send_json(conn, manifest_reply())
            return
        if request.startswith("CWFETCH1 "):
            remote = request[len("CWFETCH1 ") :]
            assert remote in PAYLOADS, remote
            payload = PAYLOADS[remote]
            events.append(f"fetch {remote}")
            send_json(
                conn,
                {
                    "status": "PASS",
                    "reason": "fetch",
                    "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                },
            )
            conn.sendall(payload)
            return
        raise AssertionError(request)

    def handle_stage(conn: socket.socket, first_line: str) -> None:
        received: dict[str, tuple[int, str, bytes]] = {}
        line = first_line
        while True:
            parts = line.split(" ")
            if parts[0] == "CWAUTH1":
                send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
                events.append("auth")
                line = read_line(conn)
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
                if target.endswith('/cw_run_args.txt'):
                    assert b'--cw-tps-cpu' in payload, payload
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
                assert len(parts) == 3, line
                dol_size, dol_sha, _ = received[parts[1]]
                args_size, args_sha, _ = received[parts[2]]
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
                s.listen(8)
                s.settimeout(30)
                ready.append(s.getsockname()[1])
                while True:
                    conn, _addr = s.accept()
                    with conn:
                        first = read_line(conn)
                        if first == "CWHELLO1":
                            handle_hello(conn)
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
        raise AssertionError("mock stage server did not bind a port")
    return ready[0], thread, errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    powershell = args.powershell
    events: list[str] = []
    port, thread, errors = start_stage_server(events)

    with tempfile.TemporaryDirectory() as td:
        temp_root = Path(td)
        dol = temp_root / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"mailbox_mock_{port}"
        output = run_cmd(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "tools/run_staged_wii_hardware.ps1",
                "-WiiHost",
                "127.0.0.1",
                "-StagePort",
                str(port),
                "-StageConnectTimeoutSeconds",
                "5",
                "-StageMailboxConnectTimeoutSeconds",
                "5",
                "-RunId",
                run_id,
                "-DolPath",
                str(dol),
                "-LaunchAfterStage",
                "-NoCollect",
                "-AllowDirty",
                "-TpsCpu",
            ],
            repo,
            timeout=120,
        )

    thread.join(5)
    if thread.is_alive():
        raise AssertionError("mock stage server did not exit")
    if errors:
        raise errors[0]

    expected_events = [
        "manifest",
        "fetch sd:/cw_boot_trace.txt",
        "fetch sd:/hang_report.txt",
        "fetch sd:/fbguard_report.txt",
        "fetch sd:/lighting_bench.txt",
        "auth",
        "stage sd:/apps/CavEX/boot.dol",
        "stage sd:/apps/CavEX/cw_run_args.txt",
        "runprep",
        "run",
    ]
    assert events == expected_events, events
    assert "fetched=4" in output, output
    assert "[launch-result]" in output, output

    # Seed-guard contract (94626650): the runner always suffixes the RunId with
    # _s<seed> or _unseeded, including an explicit -RunId. heartbeat_selftest is
    # not walk-gated, so an unseeded run is allowed and lands in <id>_unseeded.
    effective_run_id = f"{run_id}_unseeded"
    assert f"[run-staged] run_id={effective_run_id} " in output, output
    mailbox = repo / "tmp" / "wii_runs" / effective_run_id / "stage_mailbox"
    assert (mailbox / "stage_manifest_before_run.json").exists()
    assert (mailbox / "previous_cw_boot_trace.txt").read_bytes() == PAYLOADS[
        "sd:/cw_boot_trace.txt"
    ]
    assert (mailbox / "previous_hang_report.txt").read_bytes() == PAYLOADS[
        "sd:/hang_report.txt"
    ]
    assert (mailbox / "previous_fbguard_report.txt").read_bytes() == PAYLOADS[
        "sd:/fbguard_report.txt"
    ]
    assert (mailbox / "previous_lighting_bench.txt").read_bytes() == PAYLOADS[
        "sd:/lighting_bench.txt"
    ]

    print("PASS test_run_staged_mailbox")
    return 0


if __name__ == "__main__":
    sys.exit(main())
