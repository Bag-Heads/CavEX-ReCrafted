#!/usr/bin/env python3
"""Host-side regression for tools/collect_staged_wii.ps1 PASS validation.

This does not prove real Wii telemetry. It proves the staged collector wrapper
does not stop at "PASS.json exists": it runs the shared PASS validator against a
real collector-produced artifact containing last_heartbeat provenance.
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


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


def expected_build(repo: Path) -> str:
    """Mirror tools/collect_staged_wii.ps1's Get-ExpectedBuildStamp EXACTLY: the
    run under test executes the STAGED artifact, not live git HEAD -- any commit
    after staging (e.g. a tools-only fix) drifts a git-derived expectation and
    rejects an honestly-running older build (regressed the real collector once,
    2026-08-08 -- see 7448ed7). Authority is the stamp embedded in the built
    CaveWii_v2.dol (the "CaveWii (impl. B1.7.3) <stamp>" F3-header marker), with
    git HEAD only as a last-resort fallback when the .dol is absent -- matching
    the PowerShell script's fallback chain so this test can never independently
    drift from what the script it's validating actually does.
    """
    dol = repo / "CaveWii_v2.dol"
    if dol.exists():
        text = dol.read_bytes().decode("ascii", errors="replace")
        m = re.search(
            r"CaveWii \(impl\. B1\.7\.3\) ([0-9a-f]+(?:\+[0-9a-f]+)?_[0-9]{2}\.[0-9]{2})",
            text,
        )
        if m:
            return m.group(1)
    sha = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "--short=7", "HEAD"],
        text=True,
    ).strip()
    dirty = subprocess.check_output(
        ["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no"],
        text=True,
    )
    if dirty:
        sha += "+"
    return f"{sha}_{time.strftime('%m.%d')}"


def start_telemetry_server(run_id: str, build_id: str, heartbeats: int = 50):
    ready: list[int] = []
    errors: list[BaseException] = []

    def send_event(conn: socket.socket, event: dict) -> None:
        conn.sendall((json.dumps(event, separators=(",", ":")) + "\n").encode("utf-8"))

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
                    for tick in range(1, heartbeats + 1):
                        send_event(
                            conn,
                            {
                                "event": "heartbeat",
                                "run_id": run_id,
                                "profile": "heartbeat_selftest",
                                "build_id": build_id,
                                "tick": tick,
                                "fps": 60.0,
                                "srv_rt_us": tick,
                                "srv_wt_us": tick + 1,
                                "oom": 0,
                                "mesh_trunc": 0,
                                "wd_main": 0,
                                "wd_srv": 0,
                                "wd_worker": 0,
                                "world_loaded": True,
                            },
                        )
                    send_event(
                        conn,
                        {
                            "event": "profile_done",
                            "run_id": run_id,
                            "profile": "heartbeat_selftest",
                            "status": "ok",
                        },
                    )
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("telemetry mock did not bind")
    return ready[0], thread, errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    run_id = "collect_validator_mock"
    build_id = expected_build(repo)
    port, thread, errors = start_telemetry_server(run_id, build_id)

    with tempfile.TemporaryDirectory() as td:
        out_root = Path(td) / "run"
        output = run_cmd(
            [
                args.powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "tools/collect_staged_wii.ps1",
                "-Python",
                sys.executable,
                "-WiiHost",
                "127.0.0.1",
                "-RunId",
                run_id,
                "-Profile",
                "heartbeat_selftest",
                "-TelemetryPort",
                str(port),
                "-ConnectTimeout",
                "5",
                "-OutRoot",
                str(out_root),
            ],
            repo,
            timeout=90,
        )
        thread.join(5)
        if thread.is_alive():
            raise AssertionError("telemetry mock did not exit")
        if errors:
            raise errors[0]

        pass_json = json.loads((out_root / "PASS.json").read_text(encoding="utf-8"))
        assert pass_json["last_heartbeat"]["event"] == "heartbeat"
        assert pass_json["last_heartbeat"]["build_id"] == build_id
        assert "[PASS-VALIDATED]" in output, output
        assert "[PASS]" in output, output

    print("PASS test_collect_staged_validator")
    return 0


if __name__ == "__main__":
    sys.exit(main())
