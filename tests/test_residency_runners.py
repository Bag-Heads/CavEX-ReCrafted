#!/usr/bin/env python3
"""No-Wii checks for residency hardware runner forwarding and failure propagation."""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
COLLECT_CALL = ("& (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | "
                "Select-Object -First 1) @collectArgs")
PWSH = Path(os.environ.get(
    "CW_PWSH", str(Path.home() / ".cache/codex-runtimes") + "/"
    "codex-primary-runtime/dependencies/native/powershell/pwsh.exe"))
if not PWSH.is_file():
    PWSH = shutil.which("pwsh") or shutil.which("powershell")


def run_ps(body: str, *args: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "test.ps1"
        path.write_text(body, encoding="utf-8")
        return subprocess.run([str(PWSH), "-NoProfile", "-File", str(path), *args],
                              cwd=ROOT, capture_output=True, text=True, timeout=30)


def section(path: str, start: str, end: str) -> str:
    src = (TOOLS / path).read_text(encoding="utf-8")
    return src[src.index(start):src.index(end, src.index(start))]


def assert_args():
    staged = section("run_staged_wii_hardware.ps1", "$stageArgs = @(",
                     'Write-Host "[run-staged] staging DOL')
    collected = section("run_staged_wii_hardware.ps1", "$collectArgs = @(",
                        COLLECT_CALL)
    collector = section("collect_staged_wii.ps1", "$args = @(",
                        "& $pythonExe @args")
    direct = section("run_wii_hardware.ps1", "# BEGIN residency collector args",
                     "# END residency collector args")
    direct_call = section("run_wii_hardware.ps1",
                          "$collectorArgs += @(Get-ResidencyCollectorArgs",
                          "function Start-Collector")
    script = """
$RepoRoot='C:/repo'; $WiiHost='127.0.0.1'; $DolPath='dol'; $TargetPath='target'
$Profile='split_play_8m'; $SaveMigrateRegions=8; $PortalRemainingTicks=2
$AnchorSep=0; $RunId='run'; $StagePort=1; $TelemetryPort=2
$StageConnectTimeoutSeconds=1; $ChunkSize=8; $ChunkDelayMs=0
$VehicleKind=''; $PoolPlacement=''; $AutoloadName=''; $Set=@()
$RequireResidency=$true; $ResidencyRequireMoving=@('host','guest:1')
$ResidencyAllowInduced=$true; $ResidencyFault='guest@90'
$TelemetryConnectTimeoutSeconds=1; $FirstHeartbeatTimeoutSeconds=1
$StaleTimeoutSeconds=1; $OutRoot='out'; $Python=''; $ExpectedBuild=''
""" + staged + "\nWrite-Output ('STAGE=' + ($stageArgs -join '|'))\n" + collected + "\nWrite-Output ('COLLECT=' + ($collectArgs -join '|'))\n" + direct + "\n$collectorArgs=@(); $RequireResidency=$true; $ResidencyAllowInduced=$true\n" + direct_call + "\nWrite-Output ('DIRECT=' + ($collectorArgs -join '|'))\n" + """
$collector='collector.py'; $expectedBuild='build'; $ConnectTimeout=1
$RequireTntEvidence=$false; $ReleaseGate=$false
""" + collector + "\nWrite-Output ('PYTHON=' + ($args -join '|'))\n"
    proc = run_ps(script)
    assert proc.returncode == 0, proc.stderr
    got = dict(line.split("=", 1) for line in proc.stdout.splitlines() if "=" in line)
    assert "-ResidencyFault|guest@90" in got["STAGE"], got
    for label in ("COLLECT", "PYTHON", "DIRECT"):
        value = got[label]
        if label == "COLLECT":
            assert "-RequireResidency" in value and "-ResidencyAllowInduced" in value
            assert "-ResidencyRequireMoving|host,guest:1" in value
            assert "-ResidencyFault|guest@90" in value
        else:
            assert "--require-residency" in value
            assert "--residency-require-moving|host,guest:1" in value
            assert "--residency-allow-induced" in value
            assert "--residency-expect-fault|guest@90" in value


def assert_staged_text():
    block = section("stage_wii_dol.ps1", '$argText = "--cw-run-id',
                    '$argText += "`n"')
    proc = run_ps("""
$RunId='run'; $TelemetryPort=2; $Profile='far_walk_6m'; $duration=360
$VehicleKind=''; $PortalRemainingTicks=2; $SaveMigrateRegions=8
$AutoloadName=''; $Netmask=''; $NoAutoExit=$false; $ExitGraceSeconds=0
$FarViewDistance=0; $FarStorage=''; $FarSendNearOwed=0; $Seed=''
$MpEndpoint=''; $MpUsername=''; $AnchorSep=0; $PoolPlacement=''
$ResidencyFault='host@90'; $TpsCpu=$false; $Settle=$false
$CarveCensus=$false; $TelemetryNonBlock=$false; $Set=@()
""" + block + "\nWrite-Output $argText\n")
    assert proc.returncode == 0, proc.stderr
    assert "--cw-rz-fault host@90" in proc.stdout, proc.stdout
    assert proc.stdout.count("--cw-rz-fault") == 1, proc.stdout


def assert_preflight():
    for script, extra in (
        ("run_staged_wii_hardware.ps1", ["-NoCollect", "-RequireResidency"]),
        ("run_staged_wii_hardware.ps1", ["-NoCollect", "-ResidencyFault", "host@90"]),
        ("run_staged_wii_hardware.ps1", ["-ResidencyFault", "host@0"]),
        ("run_wii_hardware.ps1", ["-ResidencyFault", "host@3601"]),
        ("run_wii_hardware.ps1", ["-ResidencyRequireMoving", "host:4"]),
    ):
        proc = subprocess.run([str(PWSH), "-NoProfile", "-File",
                               str(TOOLS / script), "-WiiHost", "127.0.0.1", *extra],
                              cwd=ROOT, capture_output=True, text=True, timeout=20)
        assert proc.returncode != 0, (script, extra, proc.stdout, proc.stderr)
        assert "residency" in (proc.stderr + proc.stdout).lower(), proc


def assert_exit_status():
    # Execute the staged runner's real collector-result tail with a fake child.
    # It writes an artifact carrying each gate status and returns nonzero.
    tail = section("run_staged_wii_hardware.ps1", COLLECT_CALL,
                   "} finally {")
    for status in ("FAIL", "UNPROVEN"):
        with tempfile.TemporaryDirectory() as td:
            ps = """
param([string]$OutRoot, [string]$GateStatus)
$PowerCycleOnFailure=$false; $collectArgs=@(); $script:skipNeutralize=$false
function powershell {
    '{"heartbeats":1,"residency_gate":{"status":"' + $GateStatus + '"}}' |
        Set-Content -LiteralPath (Join-Path $OutRoot 'FAIL.json')
    'residency gate ' + $GateStatus | Set-Content -LiteralPath (Join-Path $OutRoot 'summary.md')
    $global:LASTEXITCODE=1
}
""" + tail
            proc = run_ps(ps, td, status)
            assert proc.returncode == 1 and (Path(td) / "FAIL.json").is_file(), (
                status, proc.stdout, proc.stderr)
            assert status in (Path(td) / "FAIL.json").read_text(), status


if __name__ == "__main__":
    if not PWSH:
        print("SKIP residency runner execution: PowerShell unavailable")
        raise SystemExit(0)
    assert_args()
    assert_staged_text()
    assert_preflight()
    assert_exit_status()
    print("PASS test_residency_runners")
