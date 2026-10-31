#!/usr/bin/env python3
"""Host-side regression: every run_wii_hardware.ps1 failure path honours
-PowerCycleOnFailure exactly once (KANBAN card, Astra 09-24 follow-up).

The sibling test_run_wii_power_cycle.py proves the COLLECTOR-VERDICT failure
path power-cycles. Before this test, the five throw paths that precede or
replace that verdict -- build-verify failure, missing .dol, missing wiiload
exe, wiiload attempts exhausted, collector timeout -- bypassed the hook
entirely, leaving a console that may need recovery untouched.

Each path is driven against a COPY of the runner in a scratch "repo" (so the
missing-.dol and failing-build cases need no edits to the real checkout) with
a fake wiiload, a fake collector and a -PowerCycleCommand that appends one
line to a counter file. Assertions per path: exit code nonzero, the original
throw message still printed (exit codes and messages unchanged), hook count
== 1 with the switch and == 0 without it. No Wii, no network beyond a UDP
socket connect to 127.0.0.1 that the runner does for its netmask probe.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# collector-timeout case: watchdog_inject is the shortest profile window (30 s)
# and every additive timeout knob is zeroed, so $collectorTimeout = 30 + 30.
COLLECTOR_TIMEOUT_S = 60


def write_text(path: Path, text: str, newline: str = "\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="ascii", newline=newline) as fh:
        fh.write(text)


def make_scratch_repo(real_repo: Path, root: Path) -> None:
    tools = root / "tools"
    tools.mkdir(parents=True)
    for name in ("run_wii_hardware.ps1", "invoke_wii_power_cycle.ps1"):
        shutil.copyfile(real_repo / "tools" / name, tools / name)
    # The runner's build-stamp derivation calls .Trim() on `git rev-parse HEAD`,
    # so the scratch root must be a git repo with one commit -- otherwise every
    # case dies on a null-method error before reaching the path under test.
    git = ["git", "-c", "user.name=test", "-c", "user.email=test@example.local"]
    subprocess.run(git + ["init", "-q"], cwd=root, check=True)
    subprocess.run(git + ["add", "tools"], cwd=root, check=True)
    subprocess.run(git + ["commit", "-q", "-m", "scratch"], cwd=root, check=True)
    # Fake collector: never writes a verdict, never exits on its own.
    write_text(tools / "wii_telemetry_server.py", "import time\ntime.sleep(600)\n")
    # Fake md5-gated build: deterministic failure with a recognisable code.
    write_text(
        root / ".claude" / "skills" / "build-wii" / "scripts" / "build-verify.sh",
        "#!/bin/sh\necho fake build-verify failing\nexit 3\n",
    )


def write_fake_wiiload(path: Path, exit_code: int) -> None:
    write_text(
        path,
        f"@echo off\r\necho fake wiiload %*\r\nexit /b {exit_code}\r\n",
        newline="",
    )


def hook_count(counter: Path) -> int:
    if not counter.exists():
        return 0
    return sum(1 for line in counter.read_text(encoding="utf-8").splitlines() if line.strip())


def run_case(
    powershell: str,
    root: Path,
    name: str,
    extra_args: list[str],
    power_cycle: bool,
    timeout: int,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    tag = f"{name}-{'cycle' if power_cycle else 'nocycle'}"
    counter = root / f"hook-{tag}.txt"
    out_root = root / f"run-{tag}"
    cmd = [
        powershell,
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(root / "tools" / "run_wii_hardware.ps1"),
        "-WiiHost",
        "127.0.0.1",
        "-RunId",
        f"failure_path_{tag}",
        "-OutRoot",
        str(out_root),
        "-AllowHbcUpload",
        "-AllowDirty",
        "-WiiloadAttempts",
        "1",
        "-WiiloadRetrySeconds",
        "0",
    ] + extra_args
    if power_cycle:
        cmd += [
            "-PowerCycleOnFailure",
            "-PowerCycleCommand",
            f"Add-Content -LiteralPath '{counter}' -Value hook",
            "-PowerCycleDelaySeconds",
            "0",
            "-PowerCycleTimeoutSeconds",
            "5",
        ]
    proc = subprocess.run(
        cmd,
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    return proc, counter


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()
    real_repo = Path(args.repo).resolve()

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="cw-pwrcycle-") as td:
        root = Path(td)
        make_scratch_repo(real_repo, root)
        dol = root / "CaveWii_v2.dol"
        wiiload_fail = root / "fake_wiiload_fail.cmd"
        wiiload_ok = root / "fake_wiiload_ok.cmd"
        write_fake_wiiload(wiiload_fail, 1)
        write_fake_wiiload(wiiload_ok, 0)
        missing_wiiload = root / "no_such_wiiload.exe"

        # name -> (extra args, expected message fragment, needs .dol, subprocess timeout)
        cases: list[tuple[str, list[str], str, bool, int]] = [
            (
                "build-failed",
                ["-WiiloadExe", str(wiiload_ok)],
                "build-verify failed with exit code 3",
                False,
                120,
            ),
            (
                "missing-dol",
                ["-SkipBuild", "-WiiloadExe", str(wiiload_ok)],
                "missing " + str(dol),
                False,
                120,
            ),
            (
                "missing-wiiload",
                ["-SkipBuild", "-WiiloadExe", str(missing_wiiload)],
                "missing wiiload exe: " + str(missing_wiiload),
                True,
                120,
            ),
            (
                "wiiload-exhausted",
                ["-SkipBuild", "-WiiloadExe", str(wiiload_fail)],
                "wiiload failed after 1 attempts",
                True,
                120,
            ),
            (
                "collector-timeout",
                [
                    "-SkipBuild",
                    "-WiiloadExe",
                    str(wiiload_ok),
                    "-Profile",
                    "watchdog_inject",
                    "-TelemetryConnectTimeoutSeconds",
                    "0",
                    "-FirstHeartbeatTimeoutSeconds",
                    "0",
                    "-StaleTimeoutSeconds",
                    "0",
                    "-DumpWaitSeconds",
                    "0",
                ],
                f"collector timed out after {COLLECTOR_TIMEOUT_S}s",
                True,
                COLLECTOR_TIMEOUT_S + 120,
            ),
        ]

        for name, extra, message, needs_dol, timeout in cases:
            if needs_dol:
                dol.write_bytes(b"not a real dol")
            elif dol.exists():
                dol.unlink()
            for power_cycle in (True, False):
                proc, counter = run_case(args.powershell, root, name, extra, power_cycle, timeout)
                expected = 1 if power_cycle else 0
                count = hook_count(counter)
                problems = []
                if proc.returncode == 0:
                    problems.append("exit code 0 (expected failure)")
                if message not in proc.stdout:
                    problems.append(f"message {message!r} not printed")
                if count != expected:
                    problems.append(f"hook called {count}x (expected {expected})")
                if power_cycle and "[POWER-CYCLE] mode=command" not in proc.stdout:
                    problems.append("hook completion marker missing")
                if "fake build-verify failing" in proc.stdout and name != "build-failed":
                    problems.append("build ran on a -SkipBuild path")
                tag = f"{name} switch={'on' if power_cycle else 'off'}"
                if problems:
                    failures.append(tag + ": " + "; ".join(problems))
                    print(f"FAIL {tag}: {'; '.join(problems)}")
                    print("  --- runner output ---")
                    for line in proc.stdout.splitlines()[-12:]:
                        print("  " + line)
                else:
                    print(f"PASS {tag} exit={proc.returncode} hook={count}")

    if failures:
        print(f"FAIL test_run_wii_hardware_failure_paths ({len(failures)} case(s))")
        return 1
    print("PASS test_run_wii_hardware_failure_paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
