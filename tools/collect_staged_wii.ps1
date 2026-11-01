param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [Parameter(Mandatory=$true)]
    [string]$RunId,
    [ValidateSet("heartbeat_selftest","far_idle_2m","far_walk_6m","perf_stress_5m","watchdog_inject","redstone_clock_2m","menus_dwell","menus_dwell_creative","save_migrate","mp_connect_smoke","mp_lan_walk_10m","mp_reconnect_soak","keepup_walk_6m","split_anchor_6m","split_play_2m","split_play_8m","split_actions_6m","split_recovery_6m","mp_chat_login_3m","mp_chat_chord_1m","mp_play_10m","mp_pause_menu_3m","split_portal_quit_4m","split_dims_6m","mob_motion_2m","host_vehicle_smoke_8m","pool_selftest_3m","split_regrow_8m","split_offer_piston_2m","world_copy_convert","save_reload_3m")]
    [string]$Profile = "heartbeat_selftest",
    [switch]$ServiceDiagnostic,
    [Alias("cw-vehicle-kind")]
    [ValidateSet("", "boat", "pig", "cart")]
    [string]$VehicleKind = "",
    [string]$VehicleManifest = "",
    [int]$TelemetryPort = 39017,
    [int]$ConnectTimeout = 240,
    [int]$FirstHeartbeatTimeoutSeconds = 45,
    [int]$StaleTimeoutSeconds = 5,
    [string]$OutRoot = "",
    [string]$Python = "",
    [switch]$ReleaseGate,
    [switch]$RequireTntEvidence,
    [switch]$RequireResidency,
    [string[]]$ResidencyRequireMoving = @(),
    [switch]$ResidencyAllowInduced,
    [string]$ResidencyFault = "",
    [string]$ExpectedBuild = ""
)

$ErrorActionPreference = "Stop"
if($ServiceDiagnostic -and $Profile -ne "perf_stress_5m") {
    throw "-ServiceDiagnostic requires perf_stress_5m."
}
if($ResidencyRequireMoving.Count -gt 0) {
    $legs = ($ResidencyRequireMoving -join ",").Trim()
    if($legs -cnotmatch '^(none|(host|guest)(:[0-3])?(,(host|guest)(:[0-3])?)*)$') {
        throw "-ResidencyRequireMoving must be host|guest[:0-3] comma separated, or none."
    }
}
if($ResidencyFault) {
    if($ResidencyFault -cnotmatch '^(host|guest)@([1-9][0-9]*)$' -or
       [long]$Matches[2] -gt 3600) {
        throw "-ResidencyFault must be host@seconds or guest@seconds (1..3600)."
    }
    $RequireResidency = $true
    $ResidencyAllowInduced = $true
}
if(($Profile -eq "host_vehicle_smoke_8m") -ne (-not [string]::IsNullOrWhiteSpace($VehicleKind))) {
    throw "-VehicleKind boat|pig|cart is required only for host_vehicle_smoke_8m."
}
if($Profile -eq "host_vehicle_smoke_8m" -and [string]::IsNullOrWhiteSpace($VehicleManifest)) {
    throw "host_vehicle_smoke_8m requires -VehicleManifest (frozen release/config/mode provenance)."
}

if($ReleaseGate -and $Profile -notin @("keepup_walk_6m", "far_walk_6m")) {
    throw "-ReleaseGate requires keepup_walk_6m or far_walk_6m; other profiles collect stability evidence without this switch."
}
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-Python {
    if(-not [string]::IsNullOrWhiteSpace($Python)) { return $Python }
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if($cmd) { return $cmd.Source }
    $cmd = Get-Command python3 -ErrorAction SilentlyContinue
    if($cmd) { return $cmd.Source }
    $cmd = Get-Command py -ErrorAction SilentlyContinue
    if($cmd) { return $cmd.Source }
    throw "python not found on PATH"
}

function Get-ExpectedBuildStamp {
    # The run under test executes the STAGED artifact, not the tree's HEAD --
    # any commit after staging (e.g. a tools fix) makes a git-derived
    # expectation reject the honest run (observed 2026-08-08: expected
    # 3895d71_08.08, console correctly running 5280079_08.08). Authority
    # order: explicit override > stamp embedded in the built .dol (the
    # artifact that was staged) > git HEAD as a last resort, loudly.
    if(-not [string]::IsNullOrWhiteSpace($ExpectedBuild)) {
        return $ExpectedBuild
    }
    $dol = Join-Path $RepoRoot "CaveWii_v2.dol"
    if(Test-Path $dol) {
        $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dol))
        $m = [regex]::Match($text, "CaveWii \(impl\. B1\.7\.3\) ([0-9a-f]+(\+[0-9a-f]+)?_[0-9]{2}\.[0-9]{2})")
        if($m.Success) {
            return $m.Groups[1].Value
        }
        Write-Warning "[collect-staged] no embedded build stamp found in CaveWii_v2.dol; falling back to git HEAD"
    } else {
        Write-Warning "[collect-staged] CaveWii_v2.dol not found; falling back to git HEAD (may not match the staged build)"
    }
    Push-Location $RepoRoot
    try {
        $hash = (& git rev-parse --short=7 HEAD 2>$null).Trim()
        if([string]::IsNullOrWhiteSpace($hash)) { $hash = "nogit" }
        $dirty = (& git status --porcelain --untracked-files=no 2>$null)
        if($dirty) { $hash = "$hash+" }
        return "{0}_{1}" -f $hash, (Get-Date -Format "MM.dd")
    } finally {
        Pop-Location
    }
}

if([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $RepoRoot "tmp\wii_runs\$RunId"
}
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

# A verdict file left by a previous attempt against the same run id would be
# reported as THIS run's verdict below if the collector dies verdict-less --
# and a stale PASS.json would even validate (same run id/profile/build), a
# false-PASS hole (observed with a stale FAIL 2026-08-08). Quarantine any
# pre-existing verdicts before the collector starts.
foreach($stale in @("PASS.json", "FAIL.json", "summary.md")) {
    $stalePath = Join-Path $OutRoot $stale
    if(Test-Path $stalePath) {
        $stamp = (Get-Item $stalePath).LastWriteTime.ToString("yyyyMMdd_HHmmss")
        Move-Item -Force $stalePath (Join-Path $OutRoot "$stale.stale-$stamp")
        Write-Host "[collect-staged] quarantined stale $stale (mtime $stamp) from a previous attempt"
    }
}

$pythonExe = Resolve-Python
$expectedBuild = Get-ExpectedBuildStamp
$collector = Join-Path $RepoRoot "tools\wii_telemetry_server.py"
$args = @(
    $collector,
    "--out", $OutRoot,
    "--run-id", $RunId,
    "--profile", $Profile,
    "--expected-build", $expectedBuild,
    "--tcp-connect-host", $WiiHost,
    "--tcp-connect-port", "$TelemetryPort",
    "--connect-timeout", "$ConnectTimeout",
    "--first-heartbeat-timeout", "$FirstHeartbeatTimeoutSeconds",
    "--stale-timeout", "$StaleTimeoutSeconds"
)

Write-Host "[collect-staged] run_id=$RunId profile=$Profile expected_build=$expectedBuild"
Write-Host "[collect-staged] waiting for CaveWii telemetry on ${WiiHost}:$TelemetryPort"
if($VehicleKind) { $args += @("--cw-vehicle-kind", $VehicleKind, "--vehicle-manifest", $VehicleManifest) }
if($ServiceDiagnostic) { $args += @("--duration", "600", "--require-service-exposure") }
if($ReleaseGate) { $args += "--release-gate" }
if($RequireTntEvidence) { $args += "--require-tnt-evidence" }
if($RequireResidency) { $args += "--require-residency" }
if($ResidencyRequireMoving.Count -gt 0) {
    $args += @("--residency-require-moving", ($ResidencyRequireMoving -join ","))
}
if($ResidencyAllowInduced) { $args += "--residency-allow-induced" }
if($ResidencyFault) { $args += @("--residency-expect-fault", $ResidencyFault) }
if($Profile -eq 'split_dims_6m') { $args += '--require-wii-budget' }
& $pythonExe @args

$pass = Join-Path $OutRoot "PASS.json"
$fail = Join-Path $OutRoot "FAIL.json"
if(Test-Path $pass) {
    & (Join-Path $RepoRoot "tools\validate_wii_pass.ps1") `
        -PassPath $pass `
        -ExpectedRunId $RunId `
        -ExpectedProfile $Profile `
        -ExpectedBuild $expectedBuild `
        -VehicleKind $VehicleKind -VehicleManifest $VehicleManifest -Python $pythonExe
    Write-Host "[PASS] $pass"
    Get-Content (Join-Path $OutRoot "summary.md")
    exit 0
}
if(Test-Path $fail) {
    Write-Host "[FAIL] $fail"
    Get-Content (Join-Path $OutRoot "summary.md")
    exit 1
}
throw "collector exited without PASS.json or FAIL.json; see $OutRoot"
