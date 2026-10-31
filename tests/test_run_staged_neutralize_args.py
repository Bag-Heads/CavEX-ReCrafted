#!/usr/bin/env python3
"""Host-side regression for the run_staged_wii_hardware.ps1 abort/cleanup
hardening (2026-09-02, cw_run_args.txt hardening lane).

Background (see PROJECT_MEMORY.md / the lane task): a boot that dies before
reaching hw_run_parse_file's post-FAT read (source/game/hw_run.c) never runs
the consume-once delete of sd:/apps/CavEX/cw_run_args.txt, so a staged,
fully-valid-looking profile from an earlier, interrupted run can linger on
the card and accidentally arm the NEXT boot. run_staged_wii_hardware.ps1 now
wraps the stage+collect flow in a try/finally that neutralizes
cw_run_args.txt (overwrites it with a single "#" comment line, which
hw_run_parse_file's own `if(tok[0]=='#') break;` treats as zero tokens, the
same as a missing/disabled file) via tools/put_wii_stage_file.ps1 whenever
the run does not end with positive heartbeat evidence that the console
already consumed it.

This does not prove real Wii hardware behavior (that is hw_run_parse_file's
own C-side test, tests/test_hw_run_args_parse.c). It proves the PC-side
orchestrator's cleanup DECISION: neutralize fires when a staged run gets
zero telemetry heartbeats before this script's own exit, and does NOT fire
when the caller explicitly asked to leave a staged run alone (-NoCollect).

Two scenarios, one mock stage server flavor each:
  1. Stage succeeds, telemetry never connects (nothing listens on the
     picked port) -> collect_staged_wii.ps1's own first-heartbeat/connect
     timeout produces a FAIL (or no verdict file at all, if the collector
     errors out before writing one) -> the script must open a SECOND stage
     connection and PUT "#\\n" over sd:/apps/CavEX/cw_run_args.txt.
  2. Same staging, but -NoCollect is passed -> the script must exit right
     after staging WITHOUT ever opening that second connection, leaving the
     originally staged (real, armed) content in place.
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
    # Identical minimal fixture to tests/test_run_staged_power_cycle.py --
    # just enough of a DOL header for the local launch-plan preflight and
    # the stage server's own header validator to accept it.
    dol = bytearray(0x200)
    put_be32(dol, 0x00, 0x100)
    put_be32(dol, 0x48, 0x80004000)
    put_be32(dol, 0x90, 0x100)
    put_be32(dol, 0xD8, 0x80006000)
    put_be32(dol, 0xDC, 0x2000)
    put_be32(dol, 0xE0, 0x80004020)
    put_be32(dol, 0x124, 0x5F617267)
    path.write_bytes(bytes(dol))


def send_json(conn: socket.socket, obj: dict) -> None:
    conn.sendall(json.dumps(obj, separators=(",", ":")).encode("ascii") + b"\n")


def handle_one_stage_write(conn: socket.socket, line: str, events: list[str],
                            writes: dict[str, list[bytes]], tag: str) -> None:
    parts = line.split(" ")
    assert parts[0] in ("CWSTAGE4", "CWSTAGE4R"), line
    size = int(parts[1])
    expected_sha = parts[2]
    target = parts[3]
    payload = read_exact(conn, size)
    actual_sha = hashlib.sha256(payload).hexdigest()
    assert actual_sha == expected_sha, (target, actual_sha, expected_sha)
    writes.setdefault(target, []).append(bytes(payload))
    events.append(f"{tag} {target}")
    send_json(conn, {"status": "PASS", "reason": "staged", "bytes": size,
                      "sha256": expected_sha})


def handle_main_connection(conn: socket.socket, first_line: str,
                            events: list[str], writes: dict[str, list[bytes]]) -> None:
    """boot.dol + cw_run_args.txt stage, then CWRUNPREP1/CWRUN1 -- the same
    shape tests/test_run_staged_power_cycle.py's handle_stage already
    proved against the real stage_wii_dol.ps1 (run with -LaunchAfterStage,
    which unconditionally sends CWRUNPREP1 and then CWRUN1)."""
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
            handle_one_stage_write(conn, line, events, writes, "stage")
            size, sha = int(line.split(" ")[1]), line.split(" ")[2]
            received[line.split(" ")[3]] = (size, sha)
            line = read_line(conn)
            continue
        if parts[0] in ("CWRUNPREP1", "CWRUN1"):
            dol_size, dol_sha = received[parts[1]]
            args_size, args_sha = received[parts[2]]
            launching = parts[0] == "CWRUN1"
            events.append("run" if launching else "runprep")
            send_json(conn, {
                "status": "PASS",
                "reason": "launching" if launching else "run_ready",
                "protocol": "CWSTAGE4",
                "dol_bytes": dol_size, "dol_sha256": dol_sha,
                "args_bytes": args_size, "args_sha256": args_sha,
                "entry_point": "0x80004020", "min_address": "0x80004000",
                "max_address": "0x80008000", "sections": 2, "argv_count": 11,
                "argv_marker_address": "0x80004024",
                "argv_marker_file_offset": 292,
                "argv_struct_address": "0x80004028", "argv_struct_bytes": 24,
                "command_line_address": "0x80006000",
                "command_line_bytes": max(1, args_size),
            })
            if launching:
                return
            line = read_line(conn)
            continue
        raise AssertionError(line)


def handle_neutralize_connection(conn: socket.socket, events: list[str],
                                  writes: dict[str, list[bytes]]) -> None:
    """put_wii_stage_file.ps1's own protocol shape: CWHELLO1 -> CWAUTH1 ->
    exactly one CWSTAGE4/CWSTAGE4R -> the client closes immediately after
    reading the PASS reply, with no further messages expected."""
    first = read_line(conn)
    assert first == "CWHELLO1", first
    send_json(conn, {"status": "PASS", "reason": "hello", "protocol": "CWSTAGE4",
                      "capabilities": ["put", "auth"], "token_provisioned": False})
    line = read_line(conn)
    assert line == "CWAUTH1" or line.startswith("CWAUTH1 "), line
    send_json(conn, {"status": "PASS", "reason": "no_token_provisioned"})
    events.append("neutralize-auth")
    line = read_line(conn)
    handle_one_stage_write(conn, line, events, writes, "neutralize")


def start_stage_server(events: list[str], writes: dict[str, list[bytes]],
                        *, expect_neutralize: bool):
    ready: list[int] = []
    errors: list[BaseException] = []

    def server() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", 0))
                s.listen(4)
                s.settimeout(45)
                ready.append(s.getsockname()[1])
                conn, _addr = s.accept()
                with conn:
                    handle_main_connection(conn, read_line(conn), events, writes)
                if expect_neutralize:
                    conn2, _addr2 = s.accept()
                    with conn2:
                        handle_neutralize_connection(conn2, events, writes)
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    while not ready and thread.is_alive():
        thread.join(0.01)
    if not ready:
        raise AssertionError("mock stage server did not bind")
    return ready[0], thread, errors


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_staged(repo: Path, powershell: str, dol: Path, stage_port: int,
               telemetry_port: int, run_id: str, extra_args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            powershell, "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", "tools/run_staged_wii_hardware.ps1",
            "-WiiHost", "127.0.0.1",
            "-StagePort", str(stage_port),
            "-TelemetryPort", str(telemetry_port),
            "-StageConnectTimeoutSeconds", "5",
            "-TelemetryConnectTimeoutSeconds", "1",
            "-FirstHeartbeatTimeoutSeconds", "1",
            "-StageMailboxConnectTimeoutSeconds", "5",
            "-RunId", run_id,
            "-DolPath", str(dol),
            "-LaunchAfterStage",
            "-SkipStageMailboxFetch",
            "-AllowDirty",
            "-NeutralizeConnectTimeoutSeconds", "5",
            *extra_args,
        ],
        cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=90,
    )


def check_server(stage_thread: threading.Thread, stage_errors: list[BaseException],
                  label: str) -> None:
    stage_thread.join(10)
    if stage_thread.is_alive():
        raise AssertionError(f"[{label}] mock stage server did not exit")
    if stage_errors:
        raise AssertionError(f"[{label}] mock stage server error") from stage_errors[0]


def test_fail_before_launch_neutralizes(repo: Path, powershell: str) -> None:
    """Scenario 1: staged, then zero telemetry -> must neutralize."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=True)
    telemetry_port = unused_tcp_port()  # nothing ever listens here

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"neutralize_fail_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                           run_id, [])

    check_server(stage_thread, stage_errors, "fail-neutralize")

    output = proc.stdout
    if proc.returncode == 0:
        sys.stderr.write(output)
        raise AssertionError("staged run with no telemetry unexpectedly succeeded")

    assert events == [
        "auth",
        "stage sd:/apps/CavEX/boot.dol",
        "stage sd:/apps/CavEX/cw_run_args.txt",
        "runprep",
        "run",
        "neutralize-auth",
        "neutralize sd:/apps/CavEX/cw_run_args.txt",
    ], events

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    neutralized_content = writes["sd:/apps/CavEX/cw_run_args.txt"][1]
    assert b"--cw-run-id" in armed_content, armed_content
    assert neutralized_content == b"#\n", neutralized_content
    assert "[run-staged] cw_run_args.txt neutralized" in output, output
    print("PASS test_fail_before_launch_neutralizes")


def test_nocollect_does_not_neutralize(repo: Path, powershell: str) -> None:
    """Scenario 2: explicit -NoCollect must leave the staged file alone."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"neutralize_nocollect_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                           run_id, ["-NoCollect"])

    check_server(stage_thread, stage_errors, "nocollect")

    output = proc.stdout
    if proc.returncode != 0:
        sys.stderr.write(output)
        raise AssertionError("-NoCollect run unexpectedly failed")

    assert events == [
        "auth",
        "stage sd:/apps/CavEX/boot.dol",
        "stage sd:/apps/CavEX/cw_run_args.txt",
        "runprep",
        "run",
    ], events
    # Not a bare "neutralize" not in output substring check: the run_id
    # chosen for THIS test deliberately contains "neutralize" (for readable
    # ctest -R filtering) and is itself echoed into the output/collector
    # command-line suggestion, which would make that check a false positive.
    # Assert the absence of the SPECIFIC Invoke-NeutralizeRunArgs log lines
    # instead.
    assert "[run-staged] run did not reach normal completion" not in output, output
    assert "cw_run_args.txt neutralized" not in output, output
    assert len(writes["sd:/apps/CavEX/cw_run_args.txt"]) == 1
    print("PASS test_nocollect_does_not_neutralize")


def test_far_view_distance_forwarded(repo: Path, powershell: str) -> None:
    """Stage-2 LOD reach gate (docs/stage2_lod_plan_2026-09-03.md 3):
    -FarViewDistance on run_staged_wii_hardware.ps1 must reach the staged
    cw_run_args.txt as a "--cw-far-view-distance N" token -- the same
    -NoCollect shape as test_nocollect_does_not_neutralize above (no
    telemetry server needed, just the armed content actually written)."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"fvd_forwarded_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                           run_id, ["-NoCollect", "-FarViewDistance", "8"])

    check_server(stage_thread, stage_errors, "far-view-distance-forwarded")

    output = proc.stdout
    if proc.returncode != 0:
        sys.stderr.write(output)
        raise AssertionError("-FarViewDistance run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert b"--cw-far-view-distance 8" in armed_content, armed_content
    print("PASS test_far_view_distance_forwarded")


def test_far_view_distance_absent_by_default(repo: Path, powershell: str) -> None:
    """Default behavior (no -FarViewDistance) must NOT stage the flag at
    all -- byte-identical cw_run_args.txt shape to before this param
    existed."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"fvd_absent_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                           run_id, ["-NoCollect"])

    check_server(stage_thread, stage_errors, "far-view-distance-absent")

    output = proc.stdout
    if proc.returncode != 0:
        sys.stderr.write(output)
        raise AssertionError("default (no -FarViewDistance) run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert b"--cw-far-view-distance" not in armed_content, armed_content
    print("PASS test_far_view_distance_absent_by_default")


def test_far_storage_forwarded(repo: Path, powershell: str) -> None:
    """S2 measurement (docs/stage2_lod_plan_2026-09-03.md): -FarStorage on
    run_staged_wii_hardware.ps1 must reach the staged cw_run_args.txt as a
    "--cw-far-storage skin" token."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"far_storage_forwarded_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                           run_id, ["-NoCollect", "-FarStorage", "skin"])

    check_server(stage_thread, stage_errors, "far-storage-forwarded")

    output = proc.stdout
    if proc.returncode != 0:
        sys.stderr.write(output)
        raise AssertionError("-FarStorage run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert b"--cw-far-storage skin" in armed_content, armed_content
    print("PASS test_far_storage_forwarded")


def test_far_storage_absent_by_default(repo: Path, powershell: str) -> None:
    """Default behavior (no -FarStorage) must NOT stage the flag at all --
    byte-identical cw_run_args.txt shape to before this param existed."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"far_storage_absent_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                           run_id, ["-NoCollect"])

    check_server(stage_thread, stage_errors, "far-storage-absent")

    output = proc.stdout
    if proc.returncode != 0:
        sys.stderr.write(output)
        raise AssertionError("default (no -FarStorage) run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert b"--cw-far-storage" not in armed_content, armed_content
    print("PASS test_far_storage_absent_by_default")


def test_far_send_and_seed_forwarded(repo: Path, powershell: str) -> None:
    """Both newer stage arguments must survive the wrapper boundary."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"far_send_seed_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                          run_id, ["-NoCollect", "-FarSendNearOwed", "16",
                                   "-Seed", "12345"])

    check_server(stage_thread, stage_errors, "far-send-seed-forwarded")
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise AssertionError("far-send/seed forwarding run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert b"--cw-far-send-near-owed 16" in armed_content, armed_content
    assert b"--cw-seed 12345" in armed_content, armed_content
    print("PASS test_far_send_and_seed_forwarded")


def test_settle_and_carve_census_forwarded(repo: Path, powershell: str) -> None:
    """lane/observer-arm-flags (2026-09-07): -Settle and -CarveCensus on
    run_staged_wii_hardware.ps1 must each reach the staged cw_run_args.txt
    as their own "--cw-settle" / "--cw-carve-census" token, independent of
    -TpsCpu -- the split that lets the settle observer and the tps census
    be bisected separately on real hardware (before this lane, main.c armed
    settle_observer_start() on tps_cpu alone, with no run arg for
    carve_census at all)."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"settle_carve_forwarded_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                          run_id, ["-NoCollect", "-Settle", "-CarveCensus"])

    check_server(stage_thread, stage_errors, "settle-carve-census-forwarded")
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise AssertionError("-Settle/-CarveCensus run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert b"--cw-settle" in armed_content, armed_content
    assert b"--cw-carve-census" in armed_content, armed_content
    assert b"--cw-tps-cpu" not in armed_content, armed_content
    print("PASS test_settle_and_carve_census_forwarded")


def test_settle_and_carve_census_absent_by_default(repo: Path,
                                                    powershell: str) -> None:
    """Default behavior (neither switch passed) must NOT stage either flag
    -- byte-identical cw_run_args.txt shape to before these switches
    existed."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"settle_carve_absent_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                          run_id, ["-NoCollect"])

    check_server(stage_thread, stage_errors, "settle-carve-census-absent")
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise AssertionError(
            "default (no -Settle/-CarveCensus) run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert b"--cw-settle" not in armed_content, armed_content
    assert b"--cw-carve-census" not in armed_content, armed_content
    print("PASS test_settle_and_carve_census_absent_by_default")


def test_none_stages_exact_noop(repo: Path, powershell: str) -> None:
    """Neutral plain-play staging must contain no harness arguments."""
    events: list[str] = []
    writes: dict[str, list[bytes]] = {}
    stage_port, stage_thread, stage_errors = start_stage_server(
        events, writes, expect_neutralize=False)
    telemetry_port = unused_tcp_port()

    with tempfile.TemporaryDirectory() as td:
        dol = Path(td) / "minimal.dol"
        write_minimal_launchable_dol(dol)
        run_id = f"plain_play_mock_{stage_port}"
        proc = run_staged(repo, powershell, dol, stage_port, telemetry_port,
                          run_id, ["-Profile", "none"])

    check_server(stage_thread, stage_errors, "none-stages-noop")
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise AssertionError("-Profile none run unexpectedly failed")

    armed_content = writes["sd:/apps/CavEX/cw_run_args.txt"][0]
    assert armed_content == b"noop\n", armed_content
    assert "plain-play mode" in proc.stdout, proc.stdout
    print("PASS test_none_stages_exact_noop")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()
    repo = Path(args.repo).resolve()

    test_fail_before_launch_neutralizes(repo, args.powershell)
    test_nocollect_does_not_neutralize(repo, args.powershell)
    test_far_view_distance_forwarded(repo, args.powershell)
    test_far_view_distance_absent_by_default(repo, args.powershell)
    test_far_storage_forwarded(repo, args.powershell)
    test_far_storage_absent_by_default(repo, args.powershell)
    test_far_send_and_seed_forwarded(repo, args.powershell)
    test_settle_and_carve_census_forwarded(repo, args.powershell)
    test_settle_and_carve_census_absent_by_default(repo, args.powershell)
    test_none_stages_exact_noop(repo, args.powershell)

    print("PASS test_run_staged_neutralize_args")
    return 0


if __name__ == "__main__":
    sys.exit(main())
