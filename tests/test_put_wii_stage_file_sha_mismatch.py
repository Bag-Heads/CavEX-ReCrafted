#!/usr/bin/env python3
"""Host-side regression for tools/put_wii_stage_file.ps1's sha256 check.

Mirrors tests/test_fetch_wii_stage_file_sha_mismatch.py's shape for the PUT
direction: put_wii_stage_file.ps1 (promoted 2026-09-02 from the scratchpad
put_kz.ps1 one-off) sends a CWSTAGE4R header carrying the LOCAL file's own
byte-count and sha256, streams the body, then must verify the stage
server's PASS reply against those same expected values -- not just trust a
bare "status":"PASS". This runs a minimal mock stage server that accepts a
CWHELLO1/CWAUTH1/CWSTAGE4R conversation, receives the uploaded body, and
replies with either a deliberately wrong sha256 (reproducing a server-side
verification bug or a corrupted-in-flight body) or the correct one, proving
put_wii_stage_file.ps1 refuses the former and accepts the latter.

spaced-paths (2026-10-30): also pins that a path with a space (the Tutorial
world, "About Isabella.txt") is sent WHOLE as the rest of the header line to a
server advertising "spaced-paths", and is never sent at all to one that does
not (an old server cut it into a stray ".../saves/CaveWii" file); and drives
tools/delete_wii_stage_path.ps1 (CWDELETE1) for a PASS, an allow-list refusal
the server reports, and a server too old to have "delete".
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

LOCAL_PAYLOAD = b"CaveWii stage-put regression payload\x00\x01\x02\xff\n"
# Deliberately the wrong sha256 for LOCAL_PAYLOAD -- reproduces a stage
# server whose PASS reply doesn't actually match what it wrote to the SD
# card (or a body corrupted in flight before this check would catch it).
WRONG_SHA = "31d41180000000000000000000000000000000000000000000000000000000"

STAGE4_HEADER_RE = re.compile(
    r"^(CWSTAGE4R?) (\d+) ([0-9a-fA-F]{64}) (\S.*)$"
)
BASE_CAPS = ["put", "fetch", "manifest", "runprep", "auth"]
NEW_CAPS = BASE_CAPS + ["saves-list", "saves-hash", "saves-meta",
                        "saves-archive", "spaced-paths", "delete"]


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


def recv_exact(conn: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(min(65536, n - len(buf)))
        if not chunk:
            raise RuntimeError("client closed while streaming put body")
        buf.extend(chunk)
    return bytes(buf)


def run_cmd(cmd: list[str], cwd: Path, timeout: int = 60) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )


def start_one_shot_server(sha_to_declare: str, caps=None, seen=None):
    """Serves exactly one CWHELLO1 -> CWAUTH1 -> CWSTAGE4/CWSTAGE4R
    conversation, receiving the full body, then closes. Always reports the
    real received byte-count; the caller controls whether the declared
    sha256 in the final PASS reply matches the payload."""
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
                            "capabilities": caps if caps is not None else BASE_CAPS,
                            "port": 0,
                            "token_provisioned": False,
                        },
                    )
                    try:
                        auth = read_line(conn)
                    except (RuntimeError, OSError):
                        if seen is not None:
                            seen.append(None)  # client refused after hello
                        return
                    assert auth == "CWAUTH1" or auth.startswith("CWAUTH1 "), auth
                    send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})

                    try:
                        header = read_line(conn)
                    except (RuntimeError, OSError):
                        if seen is not None:
                            seen.append(None)  # client refused before a put
                        return
                    if seen is not None:
                        seen.append(header)
                    m = STAGE4_HEADER_RE.match(header)
                    assert m, header
                    _proto, size_str, _client_sha, target = m.groups()
                    size = int(size_str)

                    body = recv_exact(conn, size)
                    assert len(body) == size

                    send_json(
                        conn,
                        {
                            "status": "PASS",
                            "reason": "staged",
                            "bytes": len(body),
                            "sha256": sha_to_declare,
                            "target": target,
                        },
                    )
        except BaseException as exc:  # noqa: BLE001 - surfaced via errors[]
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("mock stage server did not bind a port")
    return ready[0], thread, errors


def run_put(powershell: str, repo: Path, port: int, local_file: Path, remote_path: str):
    return run_cmd(
        [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            "tools/put_wii_stage_file.ps1",
            "-WiiHost",
            "127.0.0.1",
            "-Port",
            str(port),
            "-LocalPath",
            str(local_file),
            "-RemotePath",
            remote_path,
            "-ConnectTimeoutSeconds",
            "5",
        ],
        repo,
    )


def start_delete_server(reply: dict, caps, seen: list):
    """One CWHELLO1 -> CWAUTH1 -> CWDELETE1 conversation, shaped the way
    cw_saves_common.ps1's Invoke-CwHandshake expects. Records the CWDELETE1
    line (or None when the client hung up first) in seen."""
    ready: list[int] = []
    errors: list[BaseException] = []

    def send_json(conn, obj):
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
                    assert read_line(conn) == "CWHELLO1"
                    send_json(conn, {"status": "PASS", "reason": "hello",
                                     "protocol": "CWSTAGE4", "capabilities": caps,
                                     "saves_protocol": 1, "port": 0,
                                     "token_provisioned": False,
                                     "token_locked": False})
                    try:
                        auth = read_line(conn)
                    except (RuntimeError, OSError):
                        seen.append(None)
                        return
                    assert auth.startswith("CWAUTH1"), auth
                    send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
                    try:
                        line = read_line(conn)
                    except (RuntimeError, OSError):
                        seen.append(None)
                        return
                    seen.append(line)
                    send_json(conn, reply)
        except BaseException as exc:  # noqa: BLE001
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("mock delete server did not bind a port")
    return ready[0], thread, errors


def run_delete(powershell, repo, port, mode, remote_path, lock_dir):
    return run_cmd(
        [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
         "tools/delete_wii_stage_path.ps1", "-WiiHost", "127.0.0.1",
         "-Port", str(port), "-Mode", mode, "-RemotePath", remote_path,
         "-ConnectTimeoutSeconds", "5", "-LockPath", str(lock_dir)],
        repo,
    )


def _join(thread, errors):
    thread.join(10)
    if thread.is_alive():
        raise AssertionError("mock server did not exit")
    if errors:
        raise errors[0]


def check_spaced_and_delete(powershell: str, repo: Path, td: str) -> None:
    payload_sha = hashlib.sha256(LOCAL_PAYLOAD).hexdigest()
    local_file = Path(td) / "About Isabella.txt"
    local_file.write_bytes(LOCAL_PAYLOAD)
    lock_dir = Path(td) / "rig-lock"

    for remote in ("sd:/apps/CavEX/saves/CavEX Tutorial/level.dat",
                   "sd:/texturepacks/isabella-ii/About Isabella.txt",
                   "sd:/apps/CavEX/LICENSES/Isabella-II/About Isabella.txt"):
        # A server WITH spaced-paths gets the whole path as the line's tail.
        seen: list = []
        port, thread, errors = start_one_shot_server(payload_sha, NEW_CAPS, seen)
        proc = run_put(powershell, repo, port, local_file, remote)
        _join(thread, errors)
        if proc.returncode != 0:
            raise AssertionError(f"spaced put failed for {remote}:\n{proc.stdout}")
        m = STAGE4_HEADER_RE.match(seen[0] or "")
        assert m and m.group(4) == remote, (seen, remote)

    # A server WITHOUT spaced-paths never receives the put header.
    seen = []
    port, thread, errors = start_one_shot_server(payload_sha, BASE_CAPS, seen)
    proc = run_put(powershell, repo, port, local_file,
                   "sd:/apps/CavEX/saves/CavEX Tutorial/level.dat")
    _join(thread, errors)
    if proc.returncode == 0 or "spaced-paths" not in proc.stdout:
        raise AssertionError(f"spaced put to an old server was not refused:\n{proc.stdout}")
    assert seen == [None], f"old server received a put header: {seen}"

    # Client-side refusal happens before any connection.
    proc = run_put(powershell, repo, 1, local_file, "sd:/apps/x /y.txt")
    # PowerShell 7's concise error renderer can wrap between "ends" and
    # "with", adding gutter/ANSI formatting. Assert the diagnostic's words
    # without depending on the host terminal's line width.
    assert proc.returncode != 0 and re.search(
        r"starts or ends.*with a space", proc.stdout, re.S), proc.stdout

    # delete: PASS (tree, spaced); the line on the wire is whole.
    target = "sd:/apps/CavEX/saves/CavEX Tutorial"
    seen = []
    port, thread, errors = start_delete_server(
        {"status": "PASS", "reason": "deleted", "bytes": 5}, NEW_CAPS, seen)
    proc = run_delete(powershell, repo, port, "tree", target, lock_dir)
    _join(thread, errors)
    assert proc.returncode == 0, proc.stdout
    assert seen == [f"CWDELETE1 tree {target}"], seen
    assert "removed=5" in proc.stdout, proc.stdout
    assert not lock_dir.exists(), "delete tool left the rig lock behind"

    # delete: the stray file, file mode
    seen = []
    port, thread, errors = start_delete_server(
        {"status": "PASS", "reason": "deleted", "bytes": 1}, NEW_CAPS, seen)
    proc = run_delete(powershell, repo, port, "file",
                      "sd:/apps/CavEX/saves/CaveWii", lock_dir)
    _join(thread, errors)
    assert proc.returncode == 0 and seen == [
        "CWDELETE1 file sd:/apps/CavEX/saves/CaveWii"], (seen, proc.stdout)

    # delete: the server refuses a path outside its scope -> exit 7, named
    seen = []
    port, thread, errors = start_delete_server(
        {"status": "FAIL", "reason": "bad_target", "bytes": 0}, NEW_CAPS, seen)
    proc = run_delete(powershell, repo, port, "tree", "sd:/apps/CavEX/saves",
                      lock_dir)
    _join(thread, errors)
    assert proc.returncode == 7 and "bad_target" in proc.stdout, proc.stdout

    # delete: a server without "delete" -> exit 6, nothing past CWHELLO1 sent
    seen = []
    port, thread, errors = start_delete_server(
        {"status": "PASS", "reason": "deleted", "bytes": 1}, BASE_CAPS, seen)
    proc = run_delete(powershell, repo, port, "file", "sd:/assets/x.png", lock_dir)
    _join(thread, errors)
    assert proc.returncode == 6 and seen == [None], (seen, proc.stdout)

    # delete: client-side refusal ('..') -> exit 2, no connection needed
    proc = run_delete(powershell, repo, 1, "file", "sd:/apps/../x", lock_dir)
    assert proc.returncode == 2, proc.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    powershell = args.powershell

    correct_sha = hashlib.sha256(LOCAL_PAYLOAD).hexdigest()
    assert WRONG_SHA != correct_sha, "fixture bug: WRONG_SHA accidentally matches"

    with tempfile.TemporaryDirectory() as td:
        local_file = Path(td) / "kz.png"
        local_file.write_bytes(LOCAL_PAYLOAD)

        # --- Negative case: the server's PASS reply declares a sha256 that
        # does NOT match the uploaded bytes. put_wii_stage_file.ps1 must
        # refuse instead of reporting success.
        port, thread, errors = start_one_shot_server(WRONG_SHA)
        proc = run_put(powershell, repo, port, local_file, "sd:/assets/art/kz.png")
        thread.join(5)
        if thread.is_alive():
            raise AssertionError("mock stage server (mismatch case) did not exit")
        if errors:
            raise errors[0]
        if proc.returncode == 0:
            raise AssertionError(
                "put_wii_stage_file.ps1 accepted a sha256 mismatch instead of "
                f"failing; output:\n{proc.stdout}"
            )
        if "sha256 mismatch" not in proc.stdout:
            raise AssertionError(
                f"expected a 'sha256 mismatch' failure, got:\n{proc.stdout}"
            )

        # --- Positive control: a well-behaved reply (declared sha256
        # matches the uploaded bytes) still succeeds.
        port, thread, errors = start_one_shot_server(correct_sha)
        proc = run_put(powershell, repo, port, local_file, "sd:/assets/art/kz.png")
        thread.join(5)
        if thread.is_alive():
            raise AssertionError("mock stage server (matching case) did not exit")
        if errors:
            raise errors[0]
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout)
            raise AssertionError(f"matching-sha put unexpectedly failed: {proc.stdout}")
        if "sha256=" + correct_sha not in proc.stdout:
            raise AssertionError(
                f"expected success output to report sha256={correct_sha}, got:\n{proc.stdout}"
            )

        check_spaced_and_delete(powershell, repo, td)

    print("PASS test_put_wii_stage_file_sha_mismatch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
