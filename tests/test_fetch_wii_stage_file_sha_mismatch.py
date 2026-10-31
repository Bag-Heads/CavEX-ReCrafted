#!/usr/bin/env python3
"""Host-side regression for tools/fetch_wii_stage_file.ps1's sha256 check.

2026-09-02 real-Wii bug: the stage server's CWFETCH1 reply for
sd:/cw_boot_trace.txt declared sha256 31d4118... but the bytes it actually
streamed hashed to 094e307e... (tools/wii_stage_server/source/main.c's
fetch_file() used three independent disk reads -- file_size_bytes(),
hash_file_hex(), and a third fopen() for the stream -- any two of which
could observe a different snapshot of the file). The C server is Wii-only
(gccore/wiiuse/network headers) and is not part of the ctest-buildable PC
object pools, so it cannot be unit-tested directly here.

What CAN be pinned on the PC side is the other half of the fix's claim:
"the PC side computes the hash over exactly the streamed bytes". This test
runs a minimal mock stage server that reproduces the exact bug shape (a
CWFETCH1 reply whose declared sha256 does not match the payload that
follows it) and proves tools/fetch_wii_stage_file.ps1 refuses the file
instead of silently accepting corrupted/mismatched bytes -- plus a
matching-sha control proving a well-behaved reply still succeeds.
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

GOOD_PAYLOAD = b"phase=wii_after_fat build=abc run_id=x profile=heartbeat_selftest\n"
# Deliberately the wrong sha256 for GOOD_PAYLOAD -- reproduces the real-Wii
# symptom of a header/body mismatch (declared sha != hash of streamed bytes).
WRONG_SHA = "31d41180000000000000000000000000000000000000000000000000000000"


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


def run_cmd(cmd: list[str], cwd: Path, timeout: int = 60) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )


def start_one_shot_server(sha_to_declare: str):
    """Serves exactly one CWHELLO1 -> CWAUTH1 -> CWFETCH1 conversation, then
    closes. Always streams GOOD_PAYLOAD; the caller controls whether the
    declared sha256 matches it."""
    ready: list[int] = []
    errors: list[BaseException] = []

    def send_json(conn: socket.socket, obj: dict) -> None:
        conn.sendall(json.dumps(obj, separators=(",", ":")).encode("ascii") + b"\n")

    def server() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", 0))
                s.listen(1)
                s.settimeout(30)
                ready.append(s.getsockname()[1])
                conn, _addr = s.accept()
                with conn:
                    conn.settimeout(30)
                    first = read_line(conn)
                    assert first == "CWHELLO1", first
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
                    auth = read_line(conn)
                    assert auth == "CWAUTH1" or auth.startswith("CWAUTH1 "), auth
                    send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
                    fetch = read_line(conn)
                    assert fetch.startswith("CWFETCH1 "), fetch
                    send_json(
                        conn,
                        {
                            "status": "PASS",
                            "reason": "fetch",
                            "bytes": len(GOOD_PAYLOAD),
                            "sha256": sha_to_declare,
                        },
                    )
                    conn.sendall(GOOD_PAYLOAD)
        except BaseException as exc:  # noqa: BLE001 - surfaced via errors[]
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

    # --- Negative case: declared sha256 does NOT match the streamed bytes
    # (the exact real-Wii bug shape). fetch_wii_stage_file.ps1 must refuse.
    correct_sha = hashlib.sha256(GOOD_PAYLOAD).hexdigest()
    assert WRONG_SHA != correct_sha, "fixture bug: WRONG_SHA accidentally matches"
    port, thread, errors = start_one_shot_server(WRONG_SHA)
    with tempfile.TemporaryDirectory() as td:
        out_file = Path(td) / "boot_trace.txt"
        proc = run_cmd(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "tools/fetch_wii_stage_file.ps1",
                "-WiiHost",
                "127.0.0.1",
                "-Port",
                str(port),
                "-RemotePath",
                "sd:/cw_boot_trace.txt",
                "-OutFile",
                str(out_file),
                "-ConnectTimeoutSeconds",
                "5",
            ],
            repo,
        )
    thread.join(5)
    if thread.is_alive():
        raise AssertionError("mock stage server (mismatch case) did not exit")
    if errors:
        raise errors[0]
    if proc.returncode == 0:
        raise AssertionError(
            "fetch_wii_stage_file.ps1 accepted a sha256 mismatch instead of "
            f"failing; output:\n{proc.stdout}"
        )
    if "sha256 mismatch" not in proc.stdout:
        raise AssertionError(
            f"expected a 'sha256 mismatch' failure, got:\n{proc.stdout}"
        )

    # --- Positive control: a well-behaved reply (declared sha256 matches
    # the streamed bytes) still succeeds and writes the exact payload.
    port, thread, errors = start_one_shot_server(correct_sha)
    with tempfile.TemporaryDirectory() as td:
        out_file = Path(td) / "boot_trace.txt"
        proc = run_cmd(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "tools/fetch_wii_stage_file.ps1",
                "-WiiHost",
                "127.0.0.1",
                "-Port",
                str(port),
                "-RemotePath",
                "sd:/cw_boot_trace.txt",
                "-OutFile",
                str(out_file),
                "-ConnectTimeoutSeconds",
                "5",
            ],
            repo,
        )
        thread.join(5)
        if thread.is_alive():
            raise AssertionError("mock stage server (matching case) did not exit")
        if errors:
            raise errors[0]
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout)
            raise AssertionError(f"matching-sha fetch unexpectedly failed: {proc.stdout}")
        if not out_file.exists() or out_file.read_bytes() != GOOD_PAYLOAD:
            raise AssertionError("matching-sha fetch did not write the expected payload")

    print("PASS test_fetch_wii_stage_file_sha_mismatch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
