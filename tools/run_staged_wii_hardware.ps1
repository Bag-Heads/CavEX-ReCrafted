param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [ValidateSet("none","heartbeat_selftest","far_idle_2m","far_walk_6m","perf_stress_5m","watchdog_inject","redstone_clock_2m","menus_dwell","menus_dwell_creative","save_migrate","mp_connect_smoke","mp_lan_walk_10m","mp_reconnect_soak","keepup_walk_6m","split_anchor_6m","split_play_2m","split_play_8m","split_actions_6m","split_recovery_6m","mp_chat_login_3m","mp_chat_chord_1m","mp_play_10m","mp_pause_menu_3m","split_portal_quit_4m","split_dims_6m","mob_motion_2m","host_vehicle_smoke_8m","pool_selftest_3m","split_regrow_8m","split_offer_piston_2m","world_copy_convert","save_reload_3m")]
    [string]$Profile = "heartbeat_selftest",
    [switch]$ServiceDiagnostic,
    [Alias("cw-vehicle-kind")]
    [ValidateSet("", "boat", "pig", "cart")]
    [string]$VehicleKind = "",
    [string]$VehicleManifest = "",
    [ValidateRange(8,32)]
    [int]$SaveMigrateRegions = 8,
    # world_copy_convert (ui-build-1026/T16a): --cw-wcc-variant and the
    # per-run worlds root id --cw-wcc-root (sd:/apps/CavEX/wcc_runs/<id>,
    # where the fixture is staged; recover reuses the interrupt run's id).
    [ValidateSet("", "ok", "low_space", "write_error", "interrupt", "recover")]
    [string]$WccVariant = "",
    [ValidatePattern('^$|^[A-Za-z0-9_]{1,40}$')]
    [string]$WccRoot = "",
    [ValidateSet(0,1,2,3,5)]
    [int]$PortalRemainingTicks = 2,
    [ValidateRange(0,1048576)]
    [int]$AnchorSep = 0,
    # colcap-1006 R5 item 2: forwarded to stage_wii_dol.ps1 -PoolPlacement.
    [ValidateSet("", "rotating", "lowest")]
    [string]$PoolPlacement = "",
    [string]$AutoloadName = "",
    [int]$StagePort = 39019,
    [int]$TelemetryPort = 39017,
    [string]$RunId = "",
    [string]$OutRoot = "",
    [string]$DolPath = "",
    [string]$TargetPath = "sd:/apps/CavEX/boot.dol",
    [int]$StageConnectTimeoutSeconds = 600,
    [int]$TelemetryConnectTimeoutSeconds = 600,
    [int]$FirstHeartbeatTimeoutSeconds = 45,
    [int]$StaleTimeoutSeconds = 5,
    [int]$ChunkSize = 8192,
    [int]$ChunkDelayMs = 2,
    [int]$StageMailboxConnectTimeoutSeconds = 0,
    [string]$Python = "",
    [switch]$NoCollect,
    [switch]$RequireResidency,
    [string[]]$ResidencyRequireMoving = @(),
    [switch]$ResidencyAllowInduced,
    [string]$ResidencyFault = "",
    [switch]$ReleaseGate,
    [switch]$RequireTntEvidence,
    [switch]$LaunchAfterStage,
    [switch]$SkipStageMailboxFetch,
    [switch]$AllowDirty,
    [switch]$PowerCycleOnFailure,
    [string]$PowerCycleUrl = "",
    [string]$PowerOffUrl = "",
    [string]$PowerOnUrl = "",
    [string]$PowerCycleCommand = "",
    [ValidateSet("GET","POST")]
    [string]$PowerCycleMethod = "GET",
    [int]$PowerCycleDelaySeconds = 5,
    [int]$PowerCycleTimeoutSeconds = 10,
    # Forwarded to stage_wii_dol.ps1: post-profile_done linger before auto-exit.
    [int]$ExitGraceSeconds = 0,
    # Forwarded to collect_staged_wii.ps1: override the git-derived build stamp
    # (needed when tool-only commits move HEAD past the commit baked in the DOL).
    [string]$ExpectedBuild = "",
    # Abort/cleanup hardening (2026-09-02, see Invoke-NeutralizeRunArgs below):
    # how long the neutralize-cw_run_args.txt PUT retries connecting to the
    # stage server before giving up. Deliberately short relative to the other
    # *ConnectTimeoutSeconds defaults above -- by the time cleanup runs, this
    # script already spoke to the same host:port successfully once this
    # invocation, so a long retry window here only prolongs an already-bad
    # abort with a silent multi-minute hang instead of a prompt warning.
    [int]$NeutralizeConnectTimeoutSeconds = 30,
    # Forwarded to stage_wii_dol.ps1 (see its own param comment): stages
    # "--cw-far-view-distance N" for the Stage-2 LOD reach gate (docs/
    # stage2_lod_plan_2026-09-03.md 3 -- measure reach 6 vs 8 vs 11). 0
    # (default) omits the flag, so behavior is byte-identical to before this
    # param existed. Out-of-range values are clamped (with a logged line),
    # not rejected, by hw_run_parse_args on the game side.
    [int]$FarViewDistance = 0,
    # Forwarded to stage_wii_dol.ps1 (see its own param comment): stages
    # "--cw-far-storage voxel|skin" for the S2 measurement. "" (default)
    # omits the flag, byte-identical to before this param existed.
    [ValidateSet("", "voxel", "skin")]
    [string]$FarStorage = "",
    # Forwarded to stage_wii_dol.ps1; 0 omits the override.
    [int]$FarSendNearOwed = 0,
    # Forwarded to stage_wii_dol.ps1; empty omits the fixed seed.
    [string]$Seed = "",
    [switch]$AllowUnseeded,
    # Forwarded to stage_wii_dol.ps1 (see its own param comment): stages
    # "--cw-mp-endpoint <host:port>", the run-local server an unattended
    # mp_connect_smoke run joins instead of the card's config.json
    # "mp.server". "" (default) omits the flag, byte-identical to before this
    # param existed. Validated game-side (hw_run_parse_args), including the
    # "mp_connect_smoke profile only" rule.
    [string]$MpEndpoint = "",
    # Forwarded to stage_wii_dol.ps1: "--cw-mp-username <name>", the per-run
    # nonce name an mp_chat_login_3m run joins with (contract section 9 ruling
    # 7; the collector re-derives it). "" (default) omits the flag.
    [string]$MpUsername = "",
    [switch]$TpsCpu,
    # Forwarded to stage_wii_dol.ps1: "--cw-settle" (post-world_loaded
    # settle observer, independent of -TpsCpu since 2026-09-07,
    # lane/observer-arm-flags). Omitted (default): byte-identical to before
    # this switch existed.
    [switch]$Settle,
    # Forwarded to stage_wii_dol.ps1: "--cw-carve-census" (allocation
    # census observer). Omitted (default): byte-identical to before this
    # switch existed.
    [switch]$CarveCensus,
    # Forwarded to stage_wii_dol.ps1: "--cw-tel-nonblock" (opt-in
    # non-blocking telemetry-send diagnostic, 2026-09-08 sender-wedge lane).
    # Omitted (default): byte-identical to before this switch existed -- the
    # telemetry socket stays blocking.
    [switch]$TelemetryNonBlock,
    # Forwarded to stage_wii_dol.ps1: repeatable "--cw-set key=value" run-arg
    # (2026-09-08, lane/carve-observer-boot) for a run-scoped config override
    # -- e.g. -Set "memory.displaylist_compact=1" for a paired compact-0-vs-1
    # A/B on the same build/seed. Empty array (default): byte-identical to
    # before this param existed.
    [string[]]$Set = @()
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
if($NoCollect -and $RequireResidency) {
    throw "-NoCollect cannot certify required residency or a residency fault."
}
if($Profile -eq "none" -and $RequireResidency) {
    throw "Profile none cannot certify required residency."
}
if(($Profile -eq "host_vehicle_smoke_8m") -ne (-not [string]::IsNullOrWhiteSpace($VehicleKind))) {
    throw "-VehicleKind boat|pig|cart is required only for host_vehicle_smoke_8m."
}
if($Profile -eq "host_vehicle_smoke_8m" -and [string]::IsNullOrWhiteSpace($VehicleManifest)) {
    throw "host_vehicle_smoke_8m requires -VehicleManifest (frozen release/config/mode provenance)."
}

$RepoRoot = Split-Path -Parent $PSScriptRoot

# perf-seed-pin (docs/intent/perf-seed-pin.md, 2026-09-26): require an
# explicit seed for measurement runs so the RunId records it. Most profiles
# below also default to 12345 in hw_run.c; mp_lan_walk_10m does not pin a
# local world seed. -AllowUnseeded bypasses this tool's explicit-seed guard.
$measurementProfiles = @(
    "far_walk_6m", "keepup_walk_6m", "mp_lan_walk_10m", "split_anchor_6m",
    "far_idle_2m", "perf_stress_5m", "split_play_8m", "redstone_clock_2m",
    "split_regrow_8m"
)
$profilePinnedSeedProfiles = @(
    "far_walk_6m", "keepup_walk_6m", "split_anchor_6m",
    "far_idle_2m", "perf_stress_5m", "split_play_8m", "redstone_clock_2m",
    "split_recovery_6m", "split_portal_quit_4m", "split_dims_6m", "mob_motion_2m", "host_vehicle_smoke_8m",
    "pool_selftest_3m", "split_regrow_8m", "split_offer_piston_2m", "save_reload_3m"
)
$hasSeed = -not [string]::IsNullOrWhiteSpace($Seed)
if($Profile -in $measurementProfiles -and -not $hasSeed -and -not $AllowUnseeded) {
    throw "Profile $Profile requires -Seed because it is a measurement profile; the tool records an explicit seed in the RunId. Most local-world profiles default to seed 12345 in hw_run.c, but mp_lan_walk_10m does not. Use -AllowUnseeded to override."
}
if($hasSeed) {
    $parsedSeed = 0L
    if(-not [long]::TryParse($Seed, [ref]$parsedSeed)) {
        throw "-Seed must be a signed 64-bit integer."
    }
    $seedSuffix = "_s$parsedSeed"
} elseif($Profile -in $profilePinnedSeedProfiles) {
    $seedSuffix = "_s12345"
} else {
    $seedSuffix = "_unseeded"
}
if([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "staged_$($Profile)_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
}
$RunId += $seedSuffix
if([string]::IsNullOrWhiteSpace($DolPath)) {
    $DolPath = Join-Path $RepoRoot "CaveWii_v2.dol"
}
if($StageMailboxConnectTimeoutSeconds -le 0) {
    $StageMailboxConnectTimeoutSeconds = $StageConnectTimeoutSeconds
}

if([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $RepoRoot "tmp\wii_runs\$RunId"
}
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

Write-Host "[run-staged] run_id=$RunId profile=$Profile"
Write-Host "[run-staged] requirement: launch 'DOL Stage Server' on the Wii first"
if($LaunchAfterStage) {
    Write-Host "[run-staged] experimental CWRUN1 launch requested; this path is hardware-unconfirmed."
}

function Test-HasText {
    param([string]$Value)
    return -not [string]::IsNullOrWhiteSpace($Value)
}

function Assert-PowerCycleConfigured {
    if(-not $PowerCycleOnFailure) {
        return
    }
    if((Test-HasText $PowerCycleUrl) -or
       ((Test-HasText $PowerOffUrl) -and (Test-HasText $PowerOnUrl)) -or
       (Test-HasText $PowerCycleCommand)) {
        return
    }
    throw "PowerCycleOnFailure requires PowerCycleUrl, PowerOffUrl+PowerOnUrl, or PowerCycleCommand"
}

function Invoke-ConfiguredPowerCycle {
    $powerArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\invoke_wii_power_cycle.ps1"),
        "-Method", $PowerCycleMethod,
        "-DelaySeconds", "$PowerCycleDelaySeconds",
        "-TimeoutSeconds", "$PowerCycleTimeoutSeconds"
    )
    if(Test-HasText $PowerCycleUrl) {
        $powerArgs += @("-PowerCycleUrl", $PowerCycleUrl)
    }
    if(Test-HasText $PowerOffUrl) {
        $powerArgs += @("-PowerOffUrl", $PowerOffUrl)
    }
    if(Test-HasText $PowerOnUrl) {
        $powerArgs += @("-PowerOnUrl", $PowerOnUrl)
    }
    if(Test-HasText $PowerCycleCommand) {
        $powerArgs += @("-PowerCycleCommand", $PowerCycleCommand)
    }
    try {
        & (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | Select-Object -First 1) @powerArgs
        if($LASTEXITCODE -ne 0) {
            Write-Warning "power-cycle hook failed with exit code $LASTEXITCODE"
        }
    } catch {
        Write-Warning "power-cycle hook failed: $_"
    }
}

Assert-PowerCycleConfigured

function Get-StageMailboxOutName {
    param([string]$RemotePath)
    # SEQ-SUFFIXED hang reports (source/platform/watchdog.h wd_report_path_for_seq).
    # wd_write_report opens with fopen("w"), which truncates, so before 2026-07-29 report N
    # destroyed report N-1 -- the 2026-07-29 card read seq=6, five reports lost. Reports
    # 2..WD_REPORT_SEQ_MAX now carry the suffix; report 1 keeps the canonical name handled
    # by the switch below. Matched by pattern rather than 14 more switch arms, and BEFORE
    # the switch so the generic default cannot claim them.
    if($RemotePath -match '^sd:/(apps/CavEX/)?hang_report_(\d+)\.txt$') {
        $fallbackTag = ""
        if($matches[1]) { $fallbackTag = "_fallback" }
        return "previous_hang_report_$($matches[2])$fallbackTag.txt"
    }
    switch($RemotePath) {
        "sd:/cw_boot_trace.txt" { return "previous_cw_boot_trace.txt" }
        "sd:/cw_tel_trace.txt" { return "previous_cw_tel_trace.txt" }
        "sd:/hang_report.txt" { return "previous_hang_report.txt" }
        "sd:/fbguard_report.txt" { return "previous_fbguard_report.txt" }
        # App-dir FALLBACK reports, written when the sd:/ root open fails (see
        # source/platform/hw_telemetry.h HW_TELEMETRY_*_REPORT_FALLBACK_PATH). Named apart
        # from the primaries so a run that produced both does not overwrite one with the
        # other -- the report's own "id build=... boot=..." line says which boot wrote it.
        "sd:/apps/CavEX/hang_report.txt" { return "previous_hang_report_fallback.txt" }
        "sd:/apps/CavEX/fbguard_report.txt" { return "previous_fbguard_report_fallback.txt" }
        "sd:/lighting_bench.txt" { return "previous_lighting_bench.txt" }
        default {
            return $RemotePath.Replace("sd:/", "sd_").Replace("/", "_").Replace("\", "_").Replace(":", "")
        }
    }
}

function Collect-StageMailbox {
    $mailboxRoot = Join-Path $OutRoot "stage_mailbox"
    New-Item -ItemType Directory -Force -Path $mailboxRoot | Out-Null
    $manifestFile = Join-Path $mailboxRoot "stage_manifest_before_run.json"
    Write-Host "[stage-mailbox] collecting stage manifest before staging new run"
    $manifestArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\get_wii_stage_manifest.ps1"),
        "-WiiHost", $WiiHost,
        "-Port", "$StagePort",
        "-ConnectTimeoutSeconds", "$StageMailboxConnectTimeoutSeconds",
        "-OutFile", $manifestFile
    )
    & (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | Select-Object -First 1) @manifestArgs
    if($LASTEXITCODE -ne 0) {
        throw "get_wii_stage_manifest.ps1 failed with exit code $LASTEXITCODE"
    }

    $manifest = Get-Content -Raw -LiteralPath $manifestFile | ConvertFrom-Json
    $fetched = 0
    # The app-dir FALLBACK reports are the ones the watchdog writes when the sd:/ root open
    # fails, i.e. on exactly the boot where the two root paths hold nothing -- collecting
    # only the roots meant that evidence stayed on the card. They are OPTIONAL here, unlike
    # the four above: a stage server built before they joined
    # tools/wii_stage_server/source/stage_manifest_paths.h cannot list them, and a missing
    # fallback must not fail a hardware run that is otherwise collectable. Rebuild and
    # relaunch the stage server to turn the warning off.
    # Reports 2..WD_REPORT_SEQ_MAX of a boot (source/platform/watchdog.h). Built rather than
    # listed so this stays in step with the C constant by editing ONE number. Both the root
    # and app-dir forms, because the watchdog falls back per report.
    $reportSeqMax = 8 # KEEP IN STEP with WD_REPORT_SEQ_MAX
    $seqReports = @()
    foreach($seq in 2..$reportSeqMax) {
        $seqReports += "sd:/hang_report_$seq.txt"
        $seqReports += "sd:/apps/CavEX/hang_report_$seq.txt"
    }
    foreach($remote in (@(
        "sd:/cw_boot_trace.txt",
        "sd:/cw_tel_trace.txt",
        "sd:/hang_report.txt",
        "sd:/fbguard_report.txt",
        "sd:/apps/CavEX/hang_report.txt",
        "sd:/apps/CavEX/fbguard_report.txt",
        "sd:/lighting_bench.txt",
        # The stage server's OWN previous-boot trace (main.c boot_trace_rotate(),
        # tools/wii_stage_server/source/stage_manifest_paths.h
        # CW_STAGE_PREV_BOOT_TRACE_PATH), rotated ON-DEVICE at boot from
        # sd:/cw_stage_boot.txt so its final "exit: ..." line survives the next
        # boot's truncate. Fetched here alongside previous_cw_boot_trace.txt for
        # the same reason -- pulled BEFORE this run stages/relaunches anything,
        # so a card holding an older stage server's copy is still collected.
        "sd:/previous_cw_stage_boot.txt"
    ) + $seqReports)) {
        # The seq-suffixed reports are OPTIONAL for the same reason the app-dir fallbacks
        # are: a stage server built before they joined stage_manifest_paths.h cannot list
        # them, and a missing one must not fail an otherwise-collectable hardware run. The
        # stage server's own previous-boot trace is OPTIONAL for the identical reason: a
        # stage server built before it joined stage_manifest_paths.h cannot list it either.
        $optional = $remote.StartsWith("sd:/apps/CavEX/") -or
            ($remote -match 'hang_report_\d+\.txt$') -or
            ($remote -eq "sd:/previous_cw_stage_boot.txt") -or
            ($remote -eq "sd:/cw_tel_trace.txt")
        $entry = @($manifest.files | Where-Object { $_.path -eq $remote }) | Select-Object -First 1
        if($null -eq $entry) {
            if($optional) {
                Write-Warning "stage manifest did not include ${remote}; rebuild/relaunch the stage server to collect the app-dir fallback report"
                continue
            }
            throw "stage manifest did not include ${remote}; rebuild/relaunch the v1.5 stage server"
        }
        if(-not $entry.exists) {
            Write-Host "[stage-mailbox] no previous $remote"
            continue
        }
        $outFile = Join-Path $mailboxRoot (Get-StageMailboxOutName $remote)
        $fetchArgs = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $RepoRoot "tools\fetch_wii_stage_file.ps1"),
            "-WiiHost", $WiiHost,
            "-Port", "$StagePort",
            "-ConnectTimeoutSeconds", "$StageMailboxConnectTimeoutSeconds",
            "-RemotePath", $remote,
            "-OutFile", $outFile
        )
        & (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | Select-Object -First 1) @fetchArgs
        if($LASTEXITCODE -ne 0) {
            throw "fetch_wii_stage_file.ps1 failed for ${remote} with exit code $LASTEXITCODE"
        }
        $fetched++
    }
    Write-Host "[stage-mailbox] manifest=$manifestFile fetched=$fetched"
}

if($SkipStageMailboxFetch) {
    Write-Warning "SkipStageMailboxFetch set: previous stage-server proof files will not be fetched before staging"
} else {
    Collect-StageMailbox
}

# Abort/cleanup hardening (2026-09-02): a run that is interrupted (Ctrl+C)
# or throws anywhere between "cw_run_args.txt is staged on the card" and
# "this script reaches its own normal completion" leaves that file exactly
# as staged -- still describing a real, un-consumed profile.
# hw_run_parse_file (source/game/hw_run.c) only consumes (deletes) the file
# on a boot that actually reaches the post-FAT read AND arms
# (hw_run.enabled); a boot that dies before then -- the reported incident:
# three PRE-FAT deaths, each with a stale armed cw_run_args.txt already on
# the card from an earlier paused run -- leaves a stale, fully-valid-looking
# profile for the NEXT boot to accidentally inherit. $script:runArmed
# tracks whether THIS invocation has staged (or attempted to stage) a real
# profile; $script:skipNeutralize tracks whether we have POSITIVE evidence
# that cleanup is unnecessary -- either the user explicitly asked to leave a
# staged run alone (-NoCollect), or the collector observed at least one
# heartbeat. A heartbeat can only exist if hw_run.enabled fired on the
# console (source/main.c gates the whole telemetry subsystem on it), and
# enabling always precedes the consume-once delete of cw_run_args.txt -- so
# heartbeat evidence, not merely "this script didn't throw", is what proves
# the card's copy is already gone. This matters because the collector's own
# "zero heartbeats ever received" timeout (wii_telemetry_server.py's
# booted_no_heartbeat/no_telemetry failure classes) still writes a FAIL.json
# and lets this script exit NORMALLY (no exception) -- that is exactly the
# reported incident's shape (three PRE-FAT deaths, each with a stale armed
# cw_run_args.txt from an earlier paused run), so treating "no exception" as
# "safe, skip cleanup" would silently reopen the same hole. The `finally`
# below neutralizes cw_run_args.txt to a single "#" comment line -- which
# hw_run_parse_file's own `if(tok[0]=='#') break;` treats as zero tokens,
# i.e. disabled, the same as a missing file -- whenever that positive
# evidence is absent.
#
# The overwrite is idempotent and safe to attempt even when the card's copy
# was already consumed (deleted) by a successful boot: the stage server's
# CWSTAGE4R temp+rename write does not require the target to pre-exist
# (tools/wii_stage_server/source/main.c's `remove(target)` before the
# rename tolerates a missing file -- confirmed by reading that handler: it
# always writes a fresh sd:/apps/CavEX/cw_run_args.txt.tmp, verifies
# its sha256, then does `remove(target); rename(tmp, target)` unconditionally,
# the SAME truncate-then-atomic-replace path for every CWSTAGE4/CWSTAGE4R
# write -- never an append), so a spurious neutralize on an already-clean
# card just leaves a harmless placeholder behind.
#
# CAVEAT (documented, not overclaimed): Ctrl+C reliably reaches this
# `finally` only when the interrupt lands in this script's own managed
# PowerShell code (between statements, or blocked in a .NET stream/socket
# call). If it lands while a child `powershell.exe` (stage_wii_dol.ps1 /
# collect_staged_wii.ps1) owns the console, that CHILD can be killed without
# unwinding this parent's `try` -- in that case, neutralize by hand:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\put_wii_stage_file.ps1 `
#     -WiiHost <host> -LocalPath <a file containing just "#"> `
#     -RemotePath sd:/apps/CavEX/cw_run_args.txt -Replace $true
# or simply rerun this script, which stages a fresh, valid cw_run_args.txt
# over the stale one.
$script:runArmed = $false
$script:skipNeutralize = $false

function Invoke-NeutralizeRunArgs {
    Write-Warning "[run-staged] run did not reach normal completion -- neutralizing sd:/apps/CavEX/cw_run_args.txt via the stage server so no stale profile arms the next boot"
    $neutralFile = [System.IO.Path]::GetTempFileName()
    try {
        [System.IO.File]::WriteAllText($neutralFile, "#`n", [System.Text.Encoding]::ASCII)
        $putArgs = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $RepoRoot "tools\put_wii_stage_file.ps1"),
            "-WiiHost", $WiiHost,
            "-Port", "$StagePort",
            "-ConnectTimeoutSeconds", "$NeutralizeConnectTimeoutSeconds",
            "-LocalPath", $neutralFile,
            "-RemotePath", "sd:/apps/CavEX/cw_run_args.txt"
            # No -Replace: put_wii_stage_file.ps1's own [bool]$Replace already
            # defaults to $true (CWSTAGE4R, replace-existing), which is what we
            # want here. Passing a string like "True" through a -File CLI
            # invocation does NOT bind to a [bool] parameter (PowerShell's
            # argument-transformation for [bool] rejects arbitrary strings --
            # verified: `-Replace True`/`-Replace 1` both throw
            # ParameterArgumentTransformationError) -- confirmed the hard way
            # while writing this, so the default is relied on deliberately,
            # not by oversight.
        )
        & (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | Select-Object -First 1) @putArgs
        if($LASTEXITCODE -ne 0) {
            Write-Warning "[run-staged] neutralize PUT failed (exit $LASTEXITCODE) -- cw_run_args.txt may still be armed; clear it by hand (or rerun this script) before the next boot"
        } else {
            Write-Host "[run-staged] cw_run_args.txt neutralized (comment-only; hw_run_parse_file treats it as disabled)"
        }
    } catch {
        Write-Warning "[run-staged] neutralize attempt threw: $_ -- cw_run_args.txt may still be armed; clear it by hand (or rerun this script) before the next boot"
    } finally {
        Remove-Item -LiteralPath $neutralFile -ErrorAction SilentlyContinue
    }
}

try {

$stageArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $RepoRoot "tools\stage_wii_dol.ps1"),
    "-WiiHost", $WiiHost,
    "-DolPath", $DolPath,
    "-TargetPath", $TargetPath,
    "-Profile", $Profile,
    "-SaveMigrateRegions", "$SaveMigrateRegions",
    "-PortalRemainingTicks", "$PortalRemainingTicks",
    "-AnchorSep", "$AnchorSep",
    "-RunId", $RunId,
    "-Port", "$StagePort",
    "-TelemetryPort", "$TelemetryPort",
    "-StageConnectTimeoutSeconds", "$StageConnectTimeoutSeconds",
    "-ChunkSize", "$ChunkSize",
    "-ChunkDelayMs", "$ChunkDelayMs",
    "-AssumeServerRunning"
)
if($VehicleKind) { $stageArgs += @("-VehicleKind", $VehicleKind, "-VehicleManifest", $VehicleManifest) }
if($ServiceDiagnostic) { $stageArgs += "-ServiceDiagnostic" }
if($PoolPlacement) { $stageArgs += @("-PoolPlacement", $PoolPlacement) }
# T16a: only when set -- an empty string through -File is a MissingArgument.
if($WccVariant) { $stageArgs += @("-WccVariant", $WccVariant) }
if($WccRoot) { $stageArgs += @("-WccRoot", $WccRoot) }
if($ResidencyFault) { $stageArgs += @("-ResidencyFault", $ResidencyFault) }
if(-not [string]::IsNullOrWhiteSpace($AutoloadName)) {
    $stageArgs += @("-AutoloadName", $AutoloadName)
}
if($LaunchAfterStage) {
    $stageArgs += "-LaunchAfterStage"
}
if($AllowDirty) {
    $stageArgs += "-AllowDirty"
}
if($ExitGraceSeconds -gt 0) {
    $stageArgs += @("-ExitGraceSeconds", "$ExitGraceSeconds")
}
if($FarViewDistance -ne 0) {
    $stageArgs += @("-FarViewDistance", "$FarViewDistance")
}
if(-not [string]::IsNullOrWhiteSpace($FarStorage)) {
    $stageArgs += @("-FarStorage", $FarStorage)
}
if($FarSendNearOwed -ne 0) {
    $stageArgs += @("-FarSendNearOwed", "$FarSendNearOwed")
}
if(-not [string]::IsNullOrWhiteSpace($Seed)) {
    $stageArgs += @("-Seed", $Seed)
}
if(-not [string]::IsNullOrWhiteSpace($MpEndpoint)) {
    $stageArgs += @("-MpEndpoint", $MpEndpoint)
}
if(-not [string]::IsNullOrWhiteSpace($MpUsername)) {
    $stageArgs += @("-MpUsername", $MpUsername)
}
if($TpsCpu) { $stageArgs += "-TpsCpu" }
if($Settle) { $stageArgs += "-Settle" }
if($CarveCensus) { $stageArgs += "-CarveCensus" }
if($TelemetryNonBlock) { $stageArgs += "-TelemetryNonBlock" }
if($Set.Count -gt 0) {
    # ONE "-Set" token, comma-joined (never repeated "-Set ... -Set ..."):
    # $stageArgs crosses into a CHILD powershell.exe's raw argv (line ~447's
    # "& powershell @stageArgs"), which does not re-parse PowerShell's own
    # array-literal/comma syntax -- and repeating a named [string[]] param
    # errors "specified more than once" once bound from real argv (verified
    # empirically). stage_wii_dol.ps1's own -Set handling splits each
    # element back apart on ',' for exactly this reason.
    $stageArgs += @("-Set", ($Set -join ","))
}

Write-Host "[run-staged] staging DOL and cw_run_args.txt via ${WiiHost}:$StagePort"
# Owns cleanup responsibility for the card's cw_run_args.txt from this point
# on, REGARDLESS of whether the attempt below succeeds -- see the
# $script:runArmed comment above (a failed/partial stage attempt is exactly
# one of the "fails before launch" cases the neutralize path exists for).
$script:runArmed = $true
& (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | Select-Object -First 1) @stageArgs
if($LASTEXITCODE -ne 0) {
    throw "stage_wii_dol.ps1 failed with exit code $LASTEXITCODE"
}

if($Profile -eq "none") {
    Write-Host "[run-staged] plain-play mode staged; hardware telemetry/autopilot is disabled."
    $script:skipNeutralize = $true
    exit 0
}

if($NoCollect) {
    Write-Host "[run-staged] staged only; collection skipped"
    # Explicit user intent: leave the staged, armed cw_run_args.txt in place
    # for a manual HOME/HBC launch -- not a failure, do not neutralize it.
    $script:skipNeutralize = $true
    exit 0
}

if($LaunchAfterStage) {
    Write-Host "[run-staged] CWRUN1 was requested; collector will wait for CaveWii_v2 telemetry immediately."
} else {
    Write-Host "[run-staged] ACTION: press HOME on the Wii stage server, then launch CaveWii_v2 from HBC."
}
Write-Host "[run-staged] collector will wait up to ${TelemetryConnectTimeoutSeconds}s for telemetry."

$collectArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $RepoRoot "tools\collect_staged_wii.ps1"),
    "-WiiHost", $WiiHost,
    "-RunId", $RunId,
    "-Profile", $Profile,
    "-TelemetryPort", "$TelemetryPort",
    "-ConnectTimeout", "$TelemetryConnectTimeoutSeconds",
    "-FirstHeartbeatTimeoutSeconds", "$FirstHeartbeatTimeoutSeconds",
    "-StaleTimeoutSeconds", "$StaleTimeoutSeconds",
    "-OutRoot", $OutRoot
)
if($VehicleKind) { $collectArgs += @("-VehicleKind", $VehicleKind, "-VehicleManifest", $VehicleManifest) }
if($ServiceDiagnostic) { $collectArgs += "-ServiceDiagnostic" }
if($ReleaseGate) { $collectArgs += "-ReleaseGate" }
if($RequireTntEvidence) { $collectArgs += "-RequireTntEvidence" }
if($RequireResidency) { $collectArgs += "-RequireResidency" }
if($ResidencyRequireMoving.Count -gt 0) {
    $collectArgs += @("-ResidencyRequireMoving", ($ResidencyRequireMoving -join ","))
}
if($ResidencyAllowInduced) { $collectArgs += "-ResidencyAllowInduced" }
if($ResidencyFault) { $collectArgs += @("-ResidencyFault", $ResidencyFault) }
if(-not [string]::IsNullOrWhiteSpace($Python)) {
    $collectArgs += @("-Python", $Python)
}
if(-not [string]::IsNullOrWhiteSpace($ExpectedBuild)) {
    $collectArgs += @("-ExpectedBuild", $ExpectedBuild)
}

& (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | Select-Object -First 1) @collectArgs
$collectExit = $LASTEXITCODE
if($PowerCycleOnFailure -and $collectExit -ne 0) {
    Write-Warning "[run-staged] collection failed; invoking configured Wii power-cycle hook"
    Invoke-ConfiguredPowerCycle
}

# Heartbeat-evidence check (see the $script:skipNeutralize comment above): a
# PASS verdict requires profile_done, unreachable without at least one
# heartbeat, so it always counts as evidence. A FAIL verdict may ALSO carry
# real heartbeats (a profile that armed, ran, and then genuinely failed --
# the file is already gone from the card in that case too); only read its
# "heartbeats" field (tools/wii_telemetry_server.py's Collector.hb_count) to
# tell that apart from the zero-heartbeat/never-booted case this hardening
# exists for. A FAIL.json that fails to parse is treated as NO evidence
# (safe default: neutralize anyway -- the overwrite is a no-op if the file
# is already gone).
$passPath = Join-Path $OutRoot "PASS.json"
$failPath = Join-Path $OutRoot "FAIL.json"
if(Test-Path $passPath) {
    $script:skipNeutralize = $true
} elseif(Test-Path $failPath) {
    try {
        $verdict = Get-Content -Raw -LiteralPath $failPath | ConvertFrom-Json
        if([int]$verdict.heartbeats -gt 0) {
            $script:skipNeutralize = $true
        }
    } catch {
        Write-Warning "[run-staged] could not parse $failPath to check heartbeat count ($_); leaving cw_run_args.txt neutralize armed to be safe"
    }
}
exit $collectExit

} finally {
    if($script:runArmed -and -not $script:skipNeutralize) {
        Invoke-NeutralizeRunArgs
    }
}
