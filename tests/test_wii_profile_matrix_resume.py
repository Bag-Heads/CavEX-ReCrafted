#!/usr/bin/env python3
"""Host-side regression for validated matrix resume support.

This does not prove real Wii autonomy. It proves the matrix runner only resumes
from a prior PASS artifact after the shared PASS validator accepts the current
run id, profile, and build stamp. A stale PASS must not mask a failed rerun.

Seedguard (94626650): the staged runner records <run_id>_s<seed> (or
_unseeded) in PASS.json, and the matrix forwards -Seed (default 12345) to
every profile, so resume must validate against the SUFFIXED id; a PASS
recorded under another seed, or the pre-seedguard bare id, is rerun.
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import tempfile
from datetime import datetime
from pathlib import Path


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


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expected_build_stamp(repo: Path) -> str:
    rev = subprocess.run(
        ["git", "rev-parse", "--short=7", "HEAD"],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout.strip()
    if not rev:
        rev = "nogit"
    dirty = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=no"],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout
    if dirty:
        rev += "+"
    return f"{rev}_{datetime.now().strftime('%m.%d')}"


def write_pass(path: Path, run_id: str, profile: str, build_id: str) -> None:
    obj = {
        "status": "PASS",
        "reason": "profile_done received",
        "run_id": run_id,
        "profile": profile,
        "heartbeats": 50,
        "latest": {
            "event": "profile_done",
            "run_id": run_id,
            "profile": profile,
        },
        "last_heartbeat": {
            "event": "heartbeat",
            "run_id": run_id,
            "profile": profile,
            "build_id": build_id,
            "tick": 50,
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2), encoding="utf-8")


def run_matrix(
    *,
    repo: Path,
    powershell: str,
    out_root: Path,
    run_id_prefix: str,
    stage_port: int,
    telemetry_port: int,
    dol: Path | None = None,
    seed: str | None = None,
) -> subprocess.CompletedProcess[str]:
    cmd = [
        powershell,
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
        "heartbeat_selftest",
        "-RunIdPrefix",
        run_id_prefix,
        "-OutRoot",
        str(out_root),
        "-StagePort",
        str(stage_port),
        "-TelemetryPort",
        str(telemetry_port),
        "-StageConnectTimeoutSeconds",
        "1",
        "-TelemetryConnectTimeoutSeconds",
        "1",
        "-StageMailboxConnectTimeoutSeconds",
        "1",
        "-LaunchAfterStage",
        "-SkipStageMailboxFetch",
        "-AllowDirty",
        "-ResumePassed",
    ]
    if dol is not None:
        cmd.extend(["-DolPath", str(dol)])
    if seed is not None:
        cmd.extend(["-Seed", seed])
    return subprocess.run(
        cmd,
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=90,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    expected_build = expected_build_stamp(repo)

    with tempfile.TemporaryDirectory() as td:
        temp_root = Path(td)

        valid_out = temp_root / "valid_resume"
        valid_run = "matrix_resume_01_heartbeat_selftest"
        write_pass(
            valid_out / valid_run / "PASS.json",
            run_id=f"{valid_run}_s12345",
            profile="heartbeat_selftest",
            build_id=expected_build,
        )
        valid_proc = run_matrix(
            repo=repo,
            powershell=args.powershell,
            out_root=valid_out,
            run_id_prefix="matrix_resume",
            stage_port=unused_tcp_port(),
            telemetry_port=unused_tcp_port(),
        )
        if valid_proc.returncode != 0:
            sys.stderr.write(valid_proc.stdout)
            raise AssertionError("valid resume PASS was not accepted")
        valid_doc = json.loads(
            (valid_out / "matrix_results.json").read_text(encoding="utf-8-sig")
        )
        assert valid_doc["requested_profiles"] == 1, valid_doc
        assert valid_doc["recorded_profiles"] == 1, valid_doc
        assert valid_doc["started_profiles"] == 0, valid_doc
        assert valid_doc["skipped_profiles"] == 1, valid_doc
        assert valid_doc["any_failure"] is False, valid_doc
        valid_result = valid_doc["results"][0]
        assert valid_result["status"] == "PASS", valid_result
        assert valid_result["exit_code"] == 0, valid_result
        assert valid_result["skipped"] is True, valid_result
        assert valid_result["heartbeats"] == 50, valid_result
        assert valid_result["effective_run_id"] == f"{valid_run}_s12345", valid_result
        assert "[matrix] SKIP valid PASS" in valid_proc.stdout, valid_proc.stdout
        assert "[PASS-VALIDATED]" in valid_proc.stdout, valid_proc.stdout

        seeded_out = temp_root / "seeded_resume"
        seeded_run = "matrix_seeded_01_heartbeat_selftest"
        write_pass(
            seeded_out / seeded_run / "PASS.json",
            run_id=f"{seeded_run}_s-77",
            profile="heartbeat_selftest",
            build_id=expected_build,
        )
        seeded_proc = run_matrix(
            repo=repo,
            powershell=args.powershell,
            out_root=seeded_out,
            run_id_prefix="matrix_seeded",
            stage_port=unused_tcp_port(),
            telemetry_port=unused_tcp_port(),
            seed="-77",
        )
        if seeded_proc.returncode != 0:
            sys.stderr.write(seeded_proc.stdout)
            raise AssertionError("explicit -Seed resume PASS was not accepted")
        assert "[matrix] SKIP valid PASS" in seeded_proc.stdout, seeded_proc.stdout

        # A PASS under the pre-seedguard bare id, or under a different seed,
        # was not produced by this matrix's seed: it must be rerun, not resumed.
        for label, recorded in (("bare", ""), ("otherseed", "_s999")):
            mis_out = temp_root / f"mismatch_{label}"
            mis_run = f"matrix_mis{label}_01_heartbeat_selftest"
            mis_dol = temp_root / f"mismatch_{label}.dol"
            write_minimal_launchable_dol(mis_dol)
            write_pass(
                mis_out / mis_run / "PASS.json",
                run_id=f"{mis_run}{recorded}",
                profile="heartbeat_selftest",
                build_id=expected_build,
            )
            mis_proc = run_matrix(
                repo=repo,
                powershell=args.powershell,
                out_root=mis_out,
                run_id_prefix=f"matrix_mis{label}",
                stage_port=unused_tcp_port(),
                telemetry_port=unused_tcp_port(),
                dol=mis_dol,
            )
            if mis_proc.returncode == 0:
                sys.stderr.write(mis_proc.stdout)
                raise AssertionError(f"{label} run_id PASS was accepted")
            assert "[matrix] existing PASS did not validate; rerunning" in mis_proc.stdout, mis_proc.stdout
            assert "[matrix] SKIP valid PASS" not in mis_proc.stdout, mis_proc.stdout

        stale_out = temp_root / "stale_resume"
        stale_run = "matrix_stale_01_heartbeat_selftest"
        dol = temp_root / "minimal.dol"
        write_minimal_launchable_dol(dol)
        write_pass(
            stale_out / stale_run / "PASS.json",
            run_id=f"{stale_run}_s12345",
            profile="heartbeat_selftest",
            build_id="stale_01.01",
        )
        stale_proc = run_matrix(
            repo=repo,
            powershell=args.powershell,
            out_root=stale_out,
            run_id_prefix="matrix_stale",
            stage_port=unused_tcp_port(),
            telemetry_port=unused_tcp_port(),
            dol=dol,
        )
        if stale_proc.returncode == 0:
            sys.stderr.write(stale_proc.stdout)
            raise AssertionError("stale resume PASS was accepted")
        stale_doc = json.loads(
            (stale_out / "matrix_results.json").read_text(encoding="utf-8-sig")
        )
        assert stale_doc["requested_profiles"] == 1, stale_doc
        assert stale_doc["recorded_profiles"] == 1, stale_doc
        assert stale_doc["started_profiles"] == 1, stale_doc
        assert stale_doc["skipped_profiles"] == 0, stale_doc
        assert stale_doc["any_failure"] is True, stale_doc
        stale_result = stale_doc["results"][0]
        assert stale_result["status"] == "FAIL", stale_result
        assert stale_result["exit_code"] != 0, stale_result
        assert stale_result["skipped"] is False, stale_result
        assert stale_result["artifact_path"] == "", stale_result
        assert "[matrix] existing PASS did not validate; rerunning" in stale_proc.stdout
        assert "[matrix] SKIP valid PASS" not in stale_proc.stdout

    print("PASS test_wii_profile_matrix_resume")
    return 0


if __name__ == "__main__":
    sys.exit(main())
