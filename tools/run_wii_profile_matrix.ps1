param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [ValidateSet("staged","direct")]
    [string]$Mode = "staged",
    [string]$Profiles = "heartbeat_selftest",
    [ValidateRange(8,32)]
    [int]$SaveMigrateRegions = 8,
    [ValidateSet(0,1,2,3,5)]
    [int]$PortalRemainingTicks = 2,
    [string]$RunIdPrefix = "",
    [string]$OutRoot = "",
    [string]$DolPath = "",
    [int]$StagePort = 39019,
    [int]$TelemetryPort = 39017,
    [int]$StageConnectTimeoutSeconds = 600,
    [int]$TelemetryConnectTimeoutSeconds = 600,
    [int]$StageMailboxConnectTimeoutSeconds = 20,
    [int]$ChunkSize = 8192,
    [int]$ChunkDelayMs = 2,
    [string]$WiiloadExe = "C:\msys64\opt\devkitpro\tools\bin\wiiload.exe",
    [ValidateSet("pull","udp")]
    [string]$TelemetryMode = "pull",
    [int]$WiiloadAttempts = 8,
    [int]$WiiloadRetrySeconds = 10,
    [switch]$AllowHbcUpload,
    [switch]$SkipBuild,
    [switch]$AllowDirty,
    [switch]$ContinueOnFailure,
    [switch]$ResumePassed,
    [switch]$LaunchAfterStage,
    [switch]$SkipStageMailboxFetch,
    [switch]$PowerCycleOnFailure,
    # Hands-free hardware batch (2026-08-18): unlike -PowerCycleOnFailure (which
    # only fires after a FAILED profile), this fires after EVERY profile that
    # has a next one queued -- pass or fail. It exists because an armed run now
    # auto-exits back to the loader on its own (source/game/hw_run.c
    # hw_run_auto_exit_ready), but exit-to-loader does not by itself put the
    # console back into a stageable state: unless the Wii's boot path
    # (Priiloader autoboot) is configured to relaunch "CaveWii Stage Server" on
    # power-up, the NEXT staged push in this loop has nothing listening on
    # StagePort to connect to. This flag is the PC-side half of that loop --
    # the Wii-side half (Priiloader autoboot pointed at the resident stage
    # server) is a one-time physical-console configuration step this repo
    # cannot perform or verify; see docs/real_hardware_alternate_paths_2026-06-27.md
    # section 3. Do not combine with -PowerCycleOnFailure's own failure-cycle --
    # when both are set, this flag's between-every-profile cycle supersedes it
    # (no double power-cycle on a failed profile).
    [switch]$PowerCycleBetweenProfiles,
    [string]$PowerCycleUrl = "",
    [string]$PowerOffUrl = "",
    [string]$PowerOnUrl = "",
    [string]$PowerCycleCommand = "",
    [ValidateSet("GET","POST")]
    [string]$PowerCycleMethod = "GET",
    [int]$PowerCycleDelaySeconds = 5,
    [int]$PowerCycleTimeoutSeconds = 10,
    # Staged-mode pass-throughs; their defaults omit both overrides.
    [int]$FarSendNearOwed = 0,
    [ValidateRange(0,1048576)]
    [int]$AnchorSep = 0,
    # World seed forwarded to EVERY profile (staged and direct). Seedguard
    # (94626650) makes run_staged_wii_hardware.ps1 refuse walk-gated profiles
    # (far_walk_6m, keepup_walk_6m, split_anchor_6m, ...) without -Seed, so an
    # unseeded matrix could never run them; 12345 is the fixed default. Pass
    # -Seed "" to go unseeded (walk-gated staged profiles then refuse).
    [string]$Seed = "12345"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# The staged runner appends _s<seed> (seed parsed as int64) or _unseeded to
# every RunId it is given, and that suffixed id is what PASS.json records.
# Mirror its rule exactly so -ResumePassed validates against the real id.
if(-not [string]::IsNullOrWhiteSpace($Seed)) {
    $parsedMatrixSeed = 0L
    if(-not [long]::TryParse($Seed, [ref]$parsedMatrixSeed)) {
        throw "-Seed must be a signed 64-bit integer."
    }
    $StagedRunIdSuffix = "_s$parsedMatrixSeed"
} else {
    $StagedRunIdSuffix = "_unseeded"
}

function Test-HasText {
    param([string]$Value)
    return -not [string]::IsNullOrWhiteSpace($Value)
}

function Get-ExpectedBuildStamp {
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

function Invoke-PowerCycleHook {
    $powerArgs = New-Object System.Collections.Generic.List[string]
    $powerArgs.Add("-NoProfile")
    $powerArgs.Add("-ExecutionPolicy")
    $powerArgs.Add("Bypass")
    $powerArgs.Add("-File")
    $powerArgs.Add((Join-Path $RepoRoot "tools\invoke_wii_power_cycle.ps1"))
    if($PowerCycleOnFailure) {
        # The caller decides whether to run this function; keep the helper
        # configuration explicit and let it reject ambiguous modes.
    }
    if(Test-HasText $PowerCycleUrl) {
        $powerArgs.Add("-PowerCycleUrl")
        $powerArgs.Add($PowerCycleUrl)
    }
    if(Test-HasText $PowerOffUrl) {
        $powerArgs.Add("-PowerOffUrl")
        $powerArgs.Add($PowerOffUrl)
    }
    if(Test-HasText $PowerOnUrl) {
        $powerArgs.Add("-PowerOnUrl")
        $powerArgs.Add($PowerOnUrl)
    }
    if(Test-HasText $PowerCycleCommand) {
        $powerArgs.Add("-PowerCycleCommand")
        $powerArgs.Add($PowerCycleCommand)
    }
    $powerArgs.Add("-Method")
    $powerArgs.Add($PowerCycleMethod)
    $powerArgs.Add("-DelaySeconds")
    $powerArgs.Add("$PowerCycleDelaySeconds")
    $powerArgs.Add("-TimeoutSeconds")
    $powerArgs.Add("$PowerCycleTimeoutSeconds")
    try {
        & powershell @powerArgs
        if($LASTEXITCODE -ne 0) {
            Write-Warning "matrix power-cycle hook failed with exit code $LASTEXITCODE"
        }
    } catch {
        Write-Warning "matrix power-cycle hook failed: $_"
    }
}

function Test-ResumePassArtifact {
    param(
        [string]$PassPath,
        [string]$RunId,
        [string]$Profile,
        [string]$ExpectedBuild
    )
    if(-not (Test-Path -LiteralPath $PassPath)) {
        return $false
    }
    $validateArgs = New-Object System.Collections.Generic.List[string]
    $validateArgs.Add("-NoProfile")
    $validateArgs.Add("-ExecutionPolicy")
    $validateArgs.Add("Bypass")
    $validateArgs.Add("-File")
    $validateArgs.Add((Join-Path $RepoRoot "tools\validate_wii_pass.ps1"))
    $validateArgs.Add("-PassPath")
    $validateArgs.Add($PassPath)
    $validateArgs.Add("-ExpectedRunId")
    $validateArgs.Add($RunId)
    $validateArgs.Add("-ExpectedProfile")
    $validateArgs.Add($Profile)
    $validateArgs.Add("-ExpectedBuild")
    $validateArgs.Add($ExpectedBuild)
    $oldErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & powershell @validateArgs 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    foreach($line in $output) {
        if($exitCode -eq 0) {
            Write-Host $line
        } else {
            Write-Warning ([string]$line)
        }
    }
    return $exitCode -eq 0
}

function Resolve-Profiles {
    param([string]$Value)
    $allowed = @(
        "heartbeat_selftest",
        "far_idle_2m",
        "far_walk_6m",
        "perf_stress_5m",
        "watchdog_inject",
        "redstone_clock_2m",
        "menus_dwell",
        "save_migrate",
        "keepup_walk_6m",
        "split_anchor_6m",
        "split_play_2m",
        "split_play_8m",
        # mp_play_10m (2026-09-21, push-0921 lane B): the unattended LAN play
        # proof. NOTE: this list is NOT the -Profile ValidateSet the other five
        # rig tools carry and tests/test_profile_registration.py sweeps -- it is
        # this script's own matrix allow-list, and it was already missing every
        # mp_* profile plus menus_dwell_creative, split_actions_6m and
        # split_recovery_6m before this line was added. That pre-existing gap is
        # reported, not silently widened here.
        "mp_play_10m",
        "mp_pause_menu_3m",
        "split_portal_quit_4m",
        # split_dims_6m (splitdims-profile-0926): the split-dimension
        # acceptance profile Astra's final matrix names. One leg (no dwell
        # sweep): -PortalRemainingTicks is forwarded like any single profile
        # and is inert for it.
        "split_dims_6m"
    )
    $resolved = New-Object System.Collections.Generic.List[string]
    foreach($raw in ($Value -split ",")) {
        $name = $raw.Trim()
        if([string]::IsNullOrWhiteSpace($name)) {
            continue
        }
        if($allowed -notcontains $name) {
            throw "unknown matrix profile '$name'"
        }
        $resolved.Add($name)
    }
    if($resolved.Count -eq 0) {
        throw "at least one profile is required"
    }
    return @($resolved)
}

function Read-RunResult {
    param(
        [string]$RunOutRoot,
        [int]$ExitCode
    )
    $pass = Join-Path $RunOutRoot "PASS.json"
    $fail = Join-Path $RunOutRoot "FAIL.json"
    $path = ""
    $artifact = $null
    if($ExitCode -ne 0 -and (Test-Path $fail)) {
        $path = $fail
    } elseif($ExitCode -eq 0 -and (Test-Path $pass)) {
        $path = $pass
    } elseif($ExitCode -eq 0 -and (Test-Path $fail)) {
        $path = $fail
    }
    if(Test-HasText $path) {
        try {
            $artifact = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
        } catch {
            $artifact = $null
        }
    }
    return [pscustomobject]@{
        exit_code = $ExitCode
        artifact_path = $path
        status = if($ExitCode -ne 0) { "FAIL" } elseif($artifact -and $artifact.status) { [string]$artifact.status } else { "PASS" }
        failure_class = if($artifact -and $artifact.failure_class) { [string]$artifact.failure_class } else { "" }
        heartbeats = if($artifact -and ($artifact.PSObject.Properties.Name -contains "heartbeats")) { [int]$artifact.heartbeats } else { 0 }
    }
}

if([string]::IsNullOrWhiteSpace($RunIdPrefix)) {
    $RunIdPrefix = "matrix_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
}
if([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $RepoRoot "tmp\wii_matrix\$RunIdPrefix"
}
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$ProfileList = @(Resolve-Profiles -Value $Profiles)
$expandedProfiles = New-Object System.Collections.Generic.List[string]
foreach($name in $ProfileList) {
    if($name -eq "split_portal_quit_4m") {
        foreach($n in @(0,1,2,3,5)) { $expandedProfiles.Add("$name@$n") }
    } else { $expandedProfiles.Add($name) }
}
$ProfileList = @($expandedProfiles.ToArray())
$expectedBuild = Get-ExpectedBuildStamp

Write-Host "[matrix] mode=$Mode prefix=$RunIdPrefix profiles=$($ProfileList -join ',')"
Write-Host "[matrix] out=$OutRoot"
Write-Host "[matrix] expected_build=$expectedBuild"
if($Mode -eq "staged" -and -not $LaunchAfterStage) {
    Write-Warning "staged matrix without -LaunchAfterStage still requires manual Wii launch per profile"
}
if($PowerCycleBetweenProfiles) {
    Write-Warning "PowerCycleBetweenProfiles set: this PC-side loop assumes Priiloader on the Wii autoboots 'CaveWii Stage Server' on power-up. If that autoboot is not configured, each power-cycle leaves the console at the System Menu / HBC and the next stage push will fail to connect -- this flag cannot verify or configure that for you."
}

$results = New-Object System.Collections.Generic.List[object]
$anyFailure = $false
$skippedCount = 0
$portalSweepRequested = @($ProfileList | Where-Object { $_ -like 'split_portal_quit_4m@*' }).Count -gt 0
$portalWinningLeg = -1

for($i = 0; $i -lt $ProfileList.Count; $i++) {
    $profileEntry = $ProfileList[$i]
    $portalLeg = $profileEntry -like 'split_portal_quit_4m@*'
    $profile = if($portalLeg) { 'split_portal_quit_4m' } else { $profileEntry }
    $legN = if($portalLeg) { [int]($profileEntry.Split('@')[1]) } else { $PortalRemainingTicks }
    $runId = "{0}_{1:D2}_{2}" -f $RunIdPrefix, ($i + 1), ($profileEntry -replace '@', '_n')
    $runOut = Join-Path $OutRoot $runId
    # The id the child runner records in PASS.json: the staged runner adds the
    # seed suffix; run_wii_hardware.ps1 (direct) keeps -RunId verbatim.
    $effectiveRunId = if($Mode -eq "staged") { "$runId$StagedRunIdSuffix" } else { $runId }
    New-Item -ItemType Directory -Force -Path $runOut | Out-Null
    $started = (Get-Date).ToUniversalTime().ToString("o")
    Write-Host "[matrix] START $($i + 1)/$($ProfileList.Count) run_id=$runId profile=$profile"
    if($ResumePassed) {
        $pass = Join-Path $runOut "PASS.json"
        if(Test-Path -LiteralPath $pass) {
            Write-Host "[matrix] RESUME validate run_id=$runId profile=$profile pass=$pass"
            if(Test-ResumePassArtifact -PassPath $pass -RunId $effectiveRunId -Profile $profile -ExpectedBuild $expectedBuild) {
                $ended = (Get-Date).ToUniversalTime().ToString("o")
                $runResult = Read-RunResult -RunOutRoot $runOut -ExitCode 0
                $record = [pscustomobject]@{
                    index = $i + 1
                    run_id = $runId
                    effective_run_id = $effectiveRunId
                    profile = $profile
                    started_utc = $started
                    ended_utc = $ended
                    out_root = $runOut
                    status = $runResult.status
                    exit_code = 0
                    failure_class = $runResult.failure_class
                    heartbeats = $runResult.heartbeats
                    skipped = $true
                    artifact_path = $runResult.artifact_path
                }
                $results.Add($record)
                $skippedCount += 1
                if($portalLeg) { $portalWinningLeg = $legN }
                Write-Host "[matrix] SKIP valid PASS run_id=$runId profile=$profile"
                continue
            }
            Write-Warning "[matrix] existing PASS did not validate; rerunning run_id=$runId profile=$profile"
        }
    }

    $args = New-Object System.Collections.Generic.List[string]
    $args.Add("-NoProfile")
    $args.Add("-ExecutionPolicy")
    $args.Add("Bypass")
    $args.Add("-File")
    if($Mode -eq "staged") {
        $args.Add((Join-Path $RepoRoot "tools\run_staged_wii_hardware.ps1"))
        $args.Add("-WiiHost")
        $args.Add($WiiHost)
        $args.Add("-Profile")
        $args.Add($profile)
        $args.Add("-SaveMigrateRegions")
        $args.Add("$SaveMigrateRegions")
        $args.Add("-PortalRemainingTicks")
        $args.Add("$legN")
        $args.Add("-RunId")
        $args.Add($runId)
        $args.Add("-OutRoot")
        $args.Add($runOut)
        $args.Add("-StagePort")
        $args.Add("$StagePort")
        $args.Add("-TelemetryPort")
        $args.Add("$TelemetryPort")
        $args.Add("-StageConnectTimeoutSeconds")
        $args.Add("$StageConnectTimeoutSeconds")
        $args.Add("-TelemetryConnectTimeoutSeconds")
        $args.Add("$TelemetryConnectTimeoutSeconds")
        $args.Add("-StageMailboxConnectTimeoutSeconds")
        $args.Add("$StageMailboxConnectTimeoutSeconds")
        $args.Add("-ChunkSize")
        $args.Add("$ChunkSize")
        $args.Add("-ChunkDelayMs")
        $args.Add("$ChunkDelayMs")
        if(Test-HasText $DolPath) {
            $args.Add("-DolPath")
            $args.Add($DolPath)
        }
        if($LaunchAfterStage) {
            $args.Add("-LaunchAfterStage")
        }
        if($SkipStageMailboxFetch) {
            $args.Add("-SkipStageMailboxFetch")
        }
        if($AllowDirty) {
            $args.Add("-AllowDirty")
        }
        if($FarSendNearOwed -ne 0) {
            $args.Add("-FarSendNearOwed")
            $args.Add("$FarSendNearOwed")
        }
        $args.Add("-AnchorSep")
        $args.Add("$AnchorSep")
        if(Test-HasText $Seed) {
            $args.Add("-Seed")
            $args.Add($Seed)
        }
    } else {
        $args.Add((Join-Path $RepoRoot "tools\run_wii_hardware.ps1"))
        if(Test-HasText $Seed) {
            $args.Add("-Seed")
            $args.Add($Seed)
        }
        $args.Add("-WiiHost")
        $args.Add($WiiHost)
        $args.Add("-Profile")
        $args.Add($profile)
        $args.Add("-SaveMigrateRegions")
        $args.Add("$SaveMigrateRegions")
        $args.Add("-PortalRemainingTicks")
        $args.Add("$legN")
        $args.Add("-RunId")
        $args.Add($runId)
        $args.Add("-OutRoot")
        $args.Add($runOut)
        $args.Add("-TelemetryPort")
        $args.Add("$TelemetryPort")
        $args.Add("-TelemetryMode")
        $args.Add($TelemetryMode)
        $args.Add("-WiiloadExe")
        $args.Add($WiiloadExe)
        $args.Add("-WiiloadAttempts")
        $args.Add("$WiiloadAttempts")
        $args.Add("-WiiloadRetrySeconds")
        $args.Add("$WiiloadRetrySeconds")
        if($AllowHbcUpload) {
            $args.Add("-AllowHbcUpload")
        }
        if($SkipBuild) {
            $args.Add("-SkipBuild")
        }
        if($AllowDirty) {
            $args.Add("-AllowDirty")
        }
    }
    $oldErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & powershell @args
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    $ended = (Get-Date).ToUniversalTime().ToString("o")
    $runResult = Read-RunResult -RunOutRoot $runOut -ExitCode $exitCode
    $record = [pscustomobject]@{
        index = $i + 1
        run_id = $runId
        effective_run_id = $effectiveRunId
        profile = $profile
        started_utc = $started
        ended_utc = $ended
        out_root = $runOut
        status = $runResult.status
        exit_code = $runResult.exit_code
        failure_class = $runResult.failure_class
        heartbeats = $runResult.heartbeats
        skipped = $false
        artifact_path = $runResult.artifact_path
    }
    $results.Add($record)
    if($portalLeg -and $exitCode -eq 0) { $portalWinningLeg = $legN }
    if($exitCode -ne 0) {
        if(-not $portalLeg) { $anyFailure = $true }
        Write-Warning "[matrix] FAIL run_id=$runId profile=$profile exit=$exitCode class=$($runResult.failure_class)"
        # -PowerCycleBetweenProfiles (below) already cycles after every profile
        # including failures, so only fall back to the failure-only cycle when
        # it is NOT set -- otherwise a failed profile would power-cycle twice.
        if($PowerCycleOnFailure -and -not $PowerCycleBetweenProfiles) {
            Write-Warning "[matrix] invoking configured Wii power-cycle hook before continuing"
            Invoke-PowerCycleHook
        }
        if(-not $ContinueOnFailure -and -not $portalLeg) {
            break
        }
    } else {
        Write-Host "[matrix] PASS run_id=$runId profile=$profile"
    }
    # Hands-free hardware batch: cycle power before the NEXT profile (skip
    # after the last one -- nothing left to stage). Reached on both PASS and
    # a continued FAILURE (the -not $ContinueOnFailure branch above already
    # broke out of the loop for a fatal failure, so this line is never
    # reached in that case).
    if($PowerCycleBetweenProfiles -and $i -lt ($ProfileList.Count - 1)) {
        Write-Host "[matrix] power-cycling for the next profile (hands-free batch)"
        Invoke-PowerCycleHook
    }
}

if($portalSweepRequested -and $portalWinningLeg -lt 0) {
    $anyFailure = $true
    Write-Warning '[matrix] FAIL race_not_reached: no N leg passed the checklist-6b collector'
}

$resultPath = Join-Path $OutRoot "matrix_results.json"
$summaryPath = Join-Path $OutRoot "matrix_summary.md"
[pscustomobject]@{
    mode = $Mode
    run_id_prefix = $RunIdPrefix
    recorded_profiles = $results.Count
    started_profiles = $results.Count - $skippedCount
    skipped_profiles = $skippedCount
    requested_profiles = $ProfileList.Count
    any_failure = $anyFailure
    portal_quit_sweep = if($portalSweepRequested) {
        [pscustomobject]@{ status = if($portalWinningLeg -ge 0) { 'PASS' } else { 'FAIL' };
            reason = if($portalWinningLeg -ge 0) { '' } else { 'race_not_reached' };
            winning_leg = $portalWinningLeg }
    } else { $null }
    results = @($results.ToArray())
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding utf8

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# Wii Profile Matrix")
$lines.Add("")
$lines.Add("- Mode: $Mode")
$lines.Add("- Prefix: $RunIdPrefix")
$lines.Add("- Requested profiles: $($ProfileList.Count)")
$lines.Add("- Recorded profiles: $($results.Count)")
$lines.Add("- Started profiles: $($results.Count - $skippedCount)")
$lines.Add("- Skipped profiles: $skippedCount")
$lines.Add("- Any failure: $anyFailure")
if($portalSweepRequested) {
    $lines.Add("- Portal quit sweep: $(if($portalWinningLeg -ge 0) { 'PASS' } else { 'FAIL race_not_reached' }); winning N=$portalWinningLeg")
}
$lines.Add("")
$lines.Add("| # | Profile | Status | Exit | Failure class | Heartbeats | Skipped |")
$lines.Add("|---|---|---|---:|---|---:|---|")
foreach($r in $results) {
    $lines.Add("| $($r.index) | $($r.profile) | $($r.status) | $($r.exit_code) | $($r.failure_class) | $($r.heartbeats) | $($r.skipped) |")
}
$lines | Set-Content -LiteralPath $summaryPath -Encoding utf8

Write-Host "[MATRIX] results=$resultPath summary=$summaryPath failures=$anyFailure"
if($anyFailure) {
    exit 1
}
exit 0
