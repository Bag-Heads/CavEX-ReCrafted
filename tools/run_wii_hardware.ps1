param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [ValidateSet("heartbeat_selftest","far_idle_2m","far_walk_6m","perf_stress_5m","watchdog_inject","redstone_clock_2m","menus_dwell","menus_dwell_creative","save_migrate","mp_connect_smoke","mp_lan_walk_10m","mp_reconnect_soak","keepup_walk_6m","split_anchor_6m","split_play_2m","split_play_8m","split_actions_6m","split_recovery_6m","mp_chat_login_3m","mp_chat_chord_1m","mp_play_10m","mp_pause_menu_3m","split_portal_quit_4m","split_dims_6m","mob_motion_2m","host_vehicle_smoke_8m","pool_selftest_3m","split_regrow_8m","split_offer_piston_2m","world_copy_convert","save_reload_3m")]
    [string]$Profile = "far_idle_2m",
    # G9 observer/controller diagnostic only: the existing stress fixture and
    # 600-tick checkpoint cadence, with a 600 s window instead of 300 s.
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
    # colcap-1006 R5 item 2: pool_selftest_3m column placement.
    [ValidateSet("", "rotating", "lowest")]
    [string]$PoolPlacement = "",
    # residency-1005/4 (closure items 11-12): residency gate forwarding.
    # -RequireResidency makes the collector FAIL unless its residency gate
    # PASSes; -ResidencyRequireMoving overrides the profile-derived moving legs
    # (host|guest[:slot] list, or none); -ResidencyAllowInduced accepts ONE
    # failure per matching rzi mark; -ResidencyFault host@<s>|guest@<s> arms the
    # release DOL's runtime fault (--cw-rz-fault) and implies the induced option.
    [switch]$RequireResidency,
    [string[]]$ResidencyRequireMoving = @(),
    [switch]$ResidencyAllowInduced,
    [string]$ResidencyFault = "",
    [string]$Seed = "",
    # --cw-mp-username <name> (contract section 8 ruling 1 / section 9 ruling
    # 7): the mp_chat_login_3m run nonce the server receipt is matched against.
    # "" (default) means "derive it from the run id" for that profile and omit
    # the flag entirely for every other one -- byte-identical to before this
    # parameter existed. Validated GAME-side like -MpEndpoint on the staging
    # script: hw_run_parse_args takes [A-Za-z0-9_]{1,16} on MP profiles only.
    [string]$MpUsername = "",
    [int]$TelemetryPort = 39017,
    [string]$TelemetryHost = "",
    [string]$TelemetryNetmask = "",
    [string]$RunId = "",
    [string]$OutRoot = "",
    [string]$SdRoot = "",
    [string]$WiiloadExe = "C:\msys64\opt\devkitpro\tools\bin\wiiload.exe",
    [string]$Python = "",
    [ValidateSet("pull","udp")]
    [string]$TelemetryMode = "pull",
    [int]$TelemetryConnectTimeoutSeconds = 90,
    [int]$FirstHeartbeatTimeoutSeconds = 45,
    [int]$StaleTimeoutSeconds = 5,
    [int]$DumpWaitSeconds = 45,
    [int]$WiiloadAttempts = 8,
    [int]$WiiloadRetrySeconds = 10,
    [switch]$AllowHbcUpload,
    [switch]$SkipBuild,
    [switch]$AllowDirty,
    [switch]$PowerCycleOnFailure,
    [string]$PowerCycleUrl = "",
    [string]$PowerOffUrl = "",
    [string]$PowerOnUrl = "",
    [string]$PowerCycleCommand = "",
    [ValidateSet("GET","POST")]
    [string]$PowerCycleMethod = "GET",
    [int]$PowerCycleDelaySeconds = 5,
    [int]$PowerCycleTimeoutSeconds = 10
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


$RepoRoot = Split-Path -Parent $PSScriptRoot
if([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "wii_{0}_{1}" -f $Profile, (Get-Date -Format "yyyyMMdd_HHmmss")
}
if([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $RepoRoot "tmp\wii_runs\$RunId"
}
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

# BEGIN prior-verdict quarantine
# Reusing a run id must never let a previous attempt supply this run's verdict.
# Move all old verdict artifacts before any collector, build, or Wii action.
foreach($stale in @("PASS.json", "FAIL.json", "summary.md")) {
    $stalePath = Join-Path $OutRoot $stale
    if(Test-Path -LiteralPath $stalePath) {
        $stamp = (Get-Item -LiteralPath $stalePath).LastWriteTime.ToString("yyyyMMdd_HHmmss")
        $destination = Join-Path $OutRoot ("{0}.stale-{1}-{2}" -f $stale, $stamp, [guid]::NewGuid().ToString("N"))
        Move-Item -LiteralPath $stalePath -Destination $destination -ErrorAction Stop
        Write-Host "[run-wii] quarantined stale $stale (mtime $stamp) from a previous attempt"
    }
}
# END prior-verdict quarantine

function Get-ProfileDuration {
    param([string]$Name)
    switch($Name) {
        # 100s since the quiesce fix: see stage_wii_dol.ps1's duration table.
        "heartbeat_selftest" { 100 }
        "far_idle_2m" { 120 }
        "far_walk_6m" { 360 }
        "perf_stress_5m" { 300 }
        "watchdog_inject" { 30 }
        "redstone_clock_2m" { 120 }
        "mob_motion_2m" { 120 }
        "pool_selftest_3m" { 180 }
        "split_regrow_8m" { 360 }
        "menus_dwell" { 240 }
        "menus_dwell_creative" { 240 } # same script/timing as menus_dwell -- see hw_run.h's HW_RUN_PROFILE_MENUS_DWELL_CREATIVE comment
        "save_migrate" { 300 }
        "world_copy_convert" { 300 } # hw_run.c HW_RUN_PROFILE_WORLD_COPY_CONVERT
        "save_reload_3m" { 180 } # hw_run.c HW_RUN_PROFILE_SAVE_RELOAD_3M (driver 170 s cap)
        "mp_connect_smoke" { 120 }
        "mp_lan_walk_10m" { 600 }
        "mp_reconnect_soak" { 480 }
        "split_anchor_6m" { 360 }
        # split_play_2m (2026-09-07): two REAL bodies, 120 s measured
        # window -- see hw_run.h HW_RUN_PROFILE_SPLIT_PLAY_2M.
        "split_play_2m" { 120 }
        # split_play_8m (2026-09-10): six MEASURED minutes on the readiness epoch.
        "split_play_8m" { 360 }
        # split_actions_6m (2026-09-14, S1-d): same six measured minutes
        # on the readiness epoch, with the scripted guest actions armed.
        "split_actions_6m" { 360 }
        # split_recovery_6m (2026-09-17): scripted split recovery proxy; same window as split_actions_6m
        "split_recovery_6m" { 360 }
        # mp_chat_login_3m (2026-09-17, b1-chat-keyboard-0917): the scripted OSK
        # chat-login proof. 180 s is the contract's TOTAL cap (section 3) and
        # matches hw_run.c's default and wii_telemetry_server.py's
        # PROFILES["mp_chat_login_3m"]["duration"] exactly -- the same "duration
        # matches everywhere" discipline the mp_* profiles above keep.
        "mp_chat_login_3m" { 180 }
        # mp_play_10m (2026-09-21, push-0921 lane B): the unattended LAN play
        # proof. THIS TABLE'S NUMBER IS USED THREE WAYS below, and the third
        # is why it may not be shortened: (1) the harness process budget
        # ($collectorTimeout), (2) the collector's --duration (its run
        # timeout), (3) the DEVICE run arg --cw-duration, which OVERRIDES
        # hw_run.c's cfg->duration_seconds (hw_run.c:894). It is not the
        # heartbeat floor -- that is validate_wii_pass.ps1 / PROFILES min_hb.
        # 1260 s = hw_run.c's own default for this profile, deliberately 60 s
        # LONGER than mp_play_core.h's MPP_TOTAL_CAP_MS (1 200 000 ms) so a
        # stuck run reports the driver's named failure INSIDE the profile
        # window (hw_run.c:490-517). Staging 1200 here would cut the device
        # off at the exact millisecond of its own cap and lose that verdict.
        # Reconciled 2026-09-21 (push-0921/B6) from 1200. A legitimate run
        # (~13 min) ends on its own long before either number.
        "mp_play_10m" { 1260 }
        "mp_pause_menu_3m" { 180 }
        "split_portal_quit_4m" { 240 }
        "split_dims_6m" { 360 }
        "host_vehicle_smoke_8m" { 480 }
        "split_offer_piston_2m" { 140 }
        "keepup_walk_6m" { 360 } # deterministic keep-up profile; same window as far_walk_6m
        default { 120 }
    }
}

function Get-ChatLoginUsername {
    param([string]$Name)
    # Contract section 9 ruling 7: the mp_chat_login_3m run nonce is
    # "cw" + the first 6 hex characters of sha256(<run id string>), lowercase
    # (8 chars, inside Beta's 16-character limit). The rig passes it as
    # --cw-mp-username, the device echoes it in p0.session's `username`, and
    # the collector requires the mc173-diag receipt's player to equal it --
    # which is what binds one server receipt to one run. Keep this derivation
    # byte-identical to wii_telemetry_server.py's chat_login_username().
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Name))
    } finally {
        $sha.Dispose()
    }
    $hex = -join ($bytes | ForEach-Object { $_.ToString("x2") })
    return "cw" + $hex.Substring(0, 6)
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

function Get-LocalIpForRemote {
    param([string]$RemoteHost)
    $udp = [System.Net.Sockets.UdpClient]::new()
    try {
        $udp.Connect($RemoteHost, 9)
        return $udp.Client.LocalEndPoint.Address.ToString()
    } finally {
        $udp.Dispose()
    }
}

function Convert-PrefixToNetmask {
    param([int]$PrefixLength)
    if($PrefixLength -lt 0 -or $PrefixLength -gt 32) {
        return ""
    }
    [uint32]$mask = 0
    if($PrefixLength -gt 0) {
        $mask = ([uint32]::MaxValue -shl (32 - $PrefixLength))
    }
    return "{0}.{1}.{2}.{3}" -f (($mask -shr 24) -band 255), (($mask -shr 16) -band 255), (($mask -shr 8) -band 255), ($mask -band 255)
}

function Get-PrefixForLocalIp {
    param([string]$LocalIp)
    if(-not (Get-Command Get-NetIPAddress -ErrorAction SilentlyContinue)) {
        foreach($adapter in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
            foreach($address in $adapter.GetIPProperties().UnicastAddresses) {
                if($address.Address.ToString() -eq $LocalIp) { return $address.PrefixLength }
            }
        }
        return 0
    }
    $addr = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -eq $LocalIp } |
        Select-Object -First 1
    if($addr) { return [int]$addr.PrefixLength }
    return 0
}

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

$script:powerCycleArmed = $false
$script:powerCycleDone = $false

function Invoke-ConfiguredPowerCycle {
    # At most one hook call per run: the verdict path and the failure trap
    # below both route here, and a hook that runs twice would double-cycle a
    # console that only needed one recovery.
    if($script:powerCycleDone) {
        Write-Warning "power-cycle hook already invoked this run; skipping"
        return
    }
    $script:powerCycleDone = $true
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

function Assert-HardwareProofTree {
    if($AllowDirty) {
        Write-Warning "AllowDirty set: hardware proof will not be source-clean."
        return
    }
    Push-Location $RepoRoot
    try {
        $tracked = @(& git status --porcelain --untracked-files=no 2>$null)
        $untracked = @(& git ls-files --others --exclude-standard -- `
            source tests tools docs CMakeLists.txt Makefile 2>$null)
        if($tracked.Count -gt 0 -or $untracked.Count -gt 0) {
            Write-Host "[dirty] tracked changes:"
            $tracked | ForEach-Object { Write-Host "  $_" }
            Write-Host "[dirty] untracked source/test/tool/doc files:"
            $untracked | ForEach-Object { Write-Host "  $_" }
            throw "refusing hardware proof from dirty source tree; commit/stash changes or pass -AllowDirty for development only"
        }
    } finally {
        Pop-Location
    }
}

function Get-SourceFingerprint {
    $paths = @("config_wii.json", "assets")
    $hashes = New-Object System.Collections.Generic.List[string]
    foreach($p in $paths) {
        $full = Join-Path $RepoRoot $p
        if(Test-Path $full -PathType Leaf) {
            $hashes.Add("$p=$((Get-FileHash $full -Algorithm SHA256).Hash)")
        } elseif(Test-Path $full -PathType Container) {
            Get-ChildItem $full -Recurse -File | Sort-Object FullName | ForEach-Object {
                $rel = $_.FullName.Substring($RepoRoot.Length + 1)
                $hashes.Add("$rel=$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)")
            }
        }
    }
    $text = ($hashes -join "`n")
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "")
    } finally {
        $sha.Dispose()
    }
}

if($TelemetryMode -eq "pull") {
    if([string]::IsNullOrWhiteSpace($TelemetryHost)) {
        $TelemetryHost = "pull"
    }
    if([string]::IsNullOrWhiteSpace($TelemetryNetmask)) {
        $localIp = Get-LocalIpForRemote $WiiHost
        $prefix = Get-PrefixForLocalIp $localIp
        if($prefix -gt 0) {
            $TelemetryNetmask = Convert-PrefixToNetmask $prefix
            Write-Host "[net] local $localIp/$prefix -> Wii telemetry netmask $TelemetryNetmask"
        }
    }
} elseif([string]::IsNullOrWhiteSpace($TelemetryHost)) {
    $TelemetryHost = Get-LocalIpForRemote $WiiHost
}

$expectedBuild = Get-ExpectedBuildStamp
$duration = Get-ProfileDuration $Profile
if($ServiceDiagnostic) { $duration = 600 }
$collectorConnectBudget = if($TelemetryMode -eq "pull") { $TelemetryConnectTimeoutSeconds } else { 0 }
$collectorTimeout = $collectorConnectBudget + $FirstHeartbeatTimeoutSeconds + $duration + 30
# R24: match CHECKPOINT_SHUTDOWN_WAIT_SECONDS in wii_telemetry_server.py.
# Leave the existing 30 s verdict-writing margin after the bounded IO tail.
if($Profile -eq "perf_stress_5m") { $collectorTimeout += 180 }
if($Profile -eq "split_anchor_6m" -or $Profile -eq "split_regrow_8m") { $collectorTimeout += 840 }
# split_play_2m waits on the SAME two-anchor readiness gate before it starts
# measuring, plus the guest Join handshake.
if($Profile -eq "split_play_2m") { $collectorTimeout += 840 }
if($Profile -eq "split_play_8m" -or $Profile -eq "split_actions_6m" -or $Profile -eq "split_recovery_6m") { $collectorTimeout += 840 }
if($Profile -eq "watchdog_inject") {
    # The collector's expect_dump fail window (stale + dump-wait) runs AFTER the
    # last heartbeat; without this budget the hard kill below can beat the
    # collector's own no_dump FAIL.json write, losing the run's classification.
    $collectorTimeout += $StaleTimeoutSeconds + $DumpWaitSeconds
}
$dolPath = Join-Path $RepoRoot "CaveWii_v2.dol"
$buildLog = Join-Path $OutRoot "build-wii.log"
$wiiloadLog = Join-Path $OutRoot "wiiload.log"
$collectorOut = Join-Path $OutRoot "collector.stdout.log"
$collectorErr = Join-Path $OutRoot "collector.stderr.log"

Assert-PowerCycleConfigured
Assert-HardwareProofTree

if(-not $AllowHbcUpload) {
    throw "refusing HBC wiiload upload by default: recent real Wii runs can leave the console on a black screen requiring a hard power hold. Use the staged SD path, or pass -AllowHbcUpload for an explicit one-shot HBC experiment."
}

if($SdRoot -and (Test-Path (Join-Path $SdRoot "SD_MANIFEST.json"))) {
    try {
        $manifest = Get-Content -Raw (Join-Path $SdRoot "SD_MANIFEST.json") | ConvertFrom-Json
        $current = Get-SourceFingerprint
        if($manifest.source_fingerprint -and $manifest.source_fingerprint -ne $current) {
            Write-Warning "SD_MANIFEST.json is stale versus current config/assets. Run tools/provision_wii_sd.ps1 before trusting asset-sensitive proof."
        }
    } catch {
        Write-Warning "Could not read SD manifest: $_"
    }
}

# BEGIN failure power-cycle trap
# Every throw from here on (build-verify failure, missing .dol, missing wiiload
# exe, wiiload attempts exhausted, collector timeout) is a run that may leave
# the console needing recovery, exactly like a collector FAIL verdict. The trap
# runs the configured hook once when -PowerCycleOnFailure is set, then `break`
# re-throws the SAME error record so the message and the nonzero exit code are
# unchanged. Pre-arm throws (dirty tree, HBC refusal, hook misconfiguration)
# happen before any build or Wii action and fall through untouched.
$script:powerCycleArmed = $true
trap {
    if($PowerCycleOnFailure -and $script:powerCycleArmed -and -not $script:powerCycleDone) {
        Write-Warning "[run-wii] run failed before a verdict ($($_.Exception.Message)); invoking configured Wii power-cycle hook"
        Invoke-ConfiguredPowerCycle
    }
    break
}
# END failure power-cycle trap

if(-not $SkipBuild) {
    $bash = "C:\msys64\usr\bin\bash.exe"
    if(-not (Test-Path $bash)) { $bash = "bash" }
    Push-Location $RepoRoot
    try {
        Write-Host "[build] md5-gated Wii build -> $buildLog"
        & $bash ".claude/skills/build-wii/scripts/build-verify.sh" "-j8" 2>&1 |
            Tee-Object -FilePath $buildLog
        if($LASTEXITCODE -ne 0) {
            throw "build-verify failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}
if(-not (Test-Path $dolPath)) {
    throw "missing $dolPath"
}
if($VehicleKind) {
    & $pythonExe (Join-Path $RepoRoot "tools/wii_telemetry_server.py") `
        --vehicle-check-manifest --vehicle-manifest $VehicleManifest --vehicle-dol $dolPath `
        --cw-vehicle-kind $VehicleKind --expected-build $expectedBuild
    if($LASTEXITCODE -ne 0) { throw "vehicle frozen release/manifest preflight failed." }
    $vehiclePacket = Get-Content -Raw -LiteralPath $VehicleManifest | ConvertFrom-Json
    $vehicleMode = [string]$vehiclePacket.kinds.$VehicleKind.transfer_mode
    if($Seed -and $Seed -ne "12345") { throw "vehicle seed must be 12345." }
}
if(-not (Test-Path $WiiloadExe)) {
    throw "missing wiiload exe: $WiiloadExe"
}

$pythonExe = Resolve-Python
$collectorArgs = @(
    (Join-Path $RepoRoot "tools\wii_telemetry_server.py"),
    "--port", "$TelemetryPort",
    "--out", "$OutRoot",
    "--run-id", "$RunId",
    "--profile", "$Profile",
    "--expected-build", "$expectedBuild",
    "--first-heartbeat-timeout", "$FirstHeartbeatTimeoutSeconds",
    "--stale-timeout", "$StaleTimeoutSeconds",
    "--dump-wait", "$DumpWaitSeconds",
    "--duration", "$duration"
)
if($ServiceDiagnostic) { $collectorArgs += "--require-service-exposure" }
if($TelemetryMode -eq "pull") {
    $collectorArgs += @(
        "--tcp-connect-host", "$WiiHost",
        "--tcp-connect-port", "$TelemetryPort",
        "--connect-timeout", "$TelemetryConnectTimeoutSeconds"
    )
}

if($VehicleKind) { $collectorArgs += @("--cw-vehicle-kind", $VehicleKind, "--vehicle-manifest", $VehicleManifest) }

# BEGIN residency collector args (tests/test_residency_gate.py executes this block)
function Get-ResidencyCollectorArgs([bool]$Require, [string[]]$RequireMoving, [bool]$AllowInduced, [string]$Fault) {
    $out = @()
    if($Require -or $Fault) { $out += "--require-residency" }
    $legs = (@($RequireMoving) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join ","
    if($legs) { $out += @("--residency-require-moving", $legs) }
    # A runtime fault run is an induced run: the collector must know, but the
    # exception still applies only to the rzi-marked actor/world/body/attempt.
    if($AllowInduced -or -not [string]::IsNullOrWhiteSpace($Fault)) { $out += "--residency-allow-induced" }
    if($Fault) { $out += @("--residency-expect-fault", $Fault) }
    return $out
}
function Get-ResidencyRunArgs([string]$Fault) {
    if([string]::IsNullOrWhiteSpace($Fault)) { return @() }
    return @("--cw-rz-fault", $Fault)
}
# END residency collector args
$collectorArgs += @(Get-ResidencyCollectorArgs ([bool]$RequireResidency) $ResidencyRequireMoving ([bool]$ResidencyAllowInduced) $ResidencyFault)

function Start-Collector {
    Write-Host "[collect] $pythonExe $($collectorArgs -join ' ')"
    $windowOptions = @{}
    if($env:OS -eq "Windows_NT") { $windowOptions.WindowStyle = "Hidden" }
    $p = Start-Process -FilePath $pythonExe -ArgumentList $collectorArgs `
        -RedirectStandardOutput $collectorOut -RedirectStandardError $collectorErr `
        -PassThru @windowOptions
    $null = $p.Handle
    return $p
}

$collector = $null
if($TelemetryMode -eq "udp") {
    $collector = Start-Collector
    Start-Sleep -Seconds 1
}

$oldWiiload = $env:WIILOAD
$env:WIILOAD = "tcp:$WiiHost"
$wiiloadOk = $false
try {
    $wiiloadArgs = @(
        $dolPath,
        "--cw-run-id", $RunId,
        "--cw-tel-host", $TelemetryHost,
        "--cw-tel-port", "$TelemetryPort",
        "--cw-tel-mode", "$TelemetryMode",
        "--cw-profile", $Profile,
        "--cw-anchor-sep", "$AnchorSep",
        "--cw-duration", "$duration"
    )
    if($ServiceDiagnostic) {
        $wiiloadArgs += @("--cw-set", "debug.service_diagnostic_int=1",
            "--cw-set", "debug.service_observer_int=1",
            "--cw-set", "debug.music_continuous_int=1",
            "--cw-set", "debug.perf_stress_hold_health_int=1")
        Write-Host "[run-wii] guarded service diagnostic: 600 seconds; >=8 ordinary checkpoints and >=300 admitted/alive seconds required; controller input is human."
    }
    if($VehicleKind) { $wiiloadArgs += @("--cw-vehicle-kind", $VehicleKind, "--cw-vehicle-transfer", $vehicleMode) }
    if($PoolPlacement) { $wiiloadArgs += @("--cw-pool-placement", $PoolPlacement) }
    # Forward explicit --cw-seed; otherwise hw_run.c applies profile defaults.
    if($Seed) { $wiiloadArgs += @("--cw-seed", $Seed) }
    $wiiloadArgs += @(Get-ResidencyRunArgs $ResidencyFault)
    # Every profile that joins the LAN fixture under a per-run nonce: the
    # collector re-derives the same name and binds the server's receipts to it.
    if(@("mp_chat_login_3m","mp_chat_chord_1m","mp_play_10m","mp_pause_menu_3m") -contains $Profile -and [string]::IsNullOrWhiteSpace($MpUsername)) {
        $MpUsername = Get-ChatLoginUsername $RunId
    }
    if(@("split_portal_quit_4m", "split_dims_6m") -contains $Profile) {
        $wiiloadArgs += @("--cw-portal-remaining-ticks", "$PortalRemainingTicks")
    }
    if(-not [string]::IsNullOrWhiteSpace($MpUsername)) {
        $wiiloadArgs += @("--cw-mp-username", $MpUsername)
    }
    if($Profile -eq "save_migrate") {
        $wiiloadArgs += @("--cw-save-migrate-regions", "$SaveMigrateRegions")
    }
    if($Profile -eq "world_copy_convert") {
        if([string]::IsNullOrWhiteSpace($WccRoot)) { throw "world_copy_convert requires -WccRoot" }
        $wiiloadArgs += @("--cw-wcc-variant", $(if($WccVariant) { $WccVariant } else { "ok" }),
            "--cw-wcc-root", $WccRoot)
    } elseif($WccVariant -or $WccRoot) {
        throw "-WccVariant/-WccRoot are only for world_copy_convert"
    }
    if(-not [string]::IsNullOrWhiteSpace($TelemetryNetmask)) {
        $wiiloadArgs += @("--cw-netmask", $TelemetryNetmask)
    }
    for($attempt = 1; $attempt -le $WiiloadAttempts; $attempt++) {
        Write-Host "[wiiload] attempt $attempt/$WiiloadAttempts WIILOAD=$env:WIILOAD $WiiloadExe $($wiiloadArgs -join ' ')"
        "[attempt $attempt] $(Get-Date -Format o)" | Out-File -FilePath $wiiloadLog -Append -Encoding utf8
        $attemptOut = Join-Path $OutRoot "wiiload.attempt${attempt}.stdout.log"
        $attemptErr = Join-Path $OutRoot "wiiload.attempt${attempt}.stderr.log"
        $windowOptions = @{}
        if($env:OS -eq "Windows_NT") { $windowOptions.WindowStyle = "Hidden" }
        $p = Start-Process -FilePath $WiiloadExe -ArgumentList $wiiloadArgs `
            -RedirectStandardOutput $attemptOut -RedirectStandardError $attemptErr `
            -Wait -PassThru @windowOptions
        $wiiloadExit = $p.ExitCode
        if(Test-Path $attemptOut) {
            Get-Content $attemptOut | Tee-Object -FilePath $wiiloadLog -Append
        }
        if(Test-Path $attemptErr) {
            Get-Content $attemptErr | Tee-Object -FilePath $wiiloadLog -Append
        }
        if($wiiloadExit -eq 0) {
            $wiiloadOk = $true
            break
        }
        if($attempt -lt $WiiloadAttempts) {
            Start-Sleep -Seconds $WiiloadRetrySeconds
        }
    }
    if(-not $wiiloadOk) {
        if($collector -and -not $collector.HasExited) {
            Stop-Process -Id $collector.Id -Force
        }
        throw "wiiload failed after $WiiloadAttempts attempts"
    }
} finally {
    $env:WIILOAD = $oldWiiload
}

if($TelemetryMode -eq "pull") {
    $collector = Start-Collector
}

Write-Host "[wait] collector pid=$($collector.Id), timeout=${collectorTimeout}s"
$exited = Wait-Process -Id $collector.Id -Timeout $collectorTimeout -ErrorAction SilentlyContinue
$collector.Refresh()
if(-not $collector.HasExited) {
    Stop-Process -Id $collector.Id -Force
    throw "collector timed out after ${collectorTimeout}s; see $OutRoot"
}
$collector.WaitForExit()

$pass = Join-Path $OutRoot "PASS.json"
$fail = Join-Path $OutRoot "FAIL.json"
$failureReason = $null
if($null -eq $collector.ExitCode) {
    $failureReason = "collector exit code unavailable after exit"
} elseif($collector.ExitCode -ne 0) {
    $failureReason = "collector exited with code $($collector.ExitCode)"
} elseif(Test-Path $fail) {
    $failureReason = "collector wrote FAIL.json"
} elseif(-not (Test-Path $pass)) {
    $failureReason = "collector exited without PASS.json or FAIL.json"
} else {
    try {
        & (Join-Path $RepoRoot "tools\validate_wii_pass.ps1") `
            -PassPath $pass `
            -ExpectedRunId $RunId `
            -ExpectedProfile $Profile `
            -ExpectedBuild $expectedBuild `
            -VehicleKind $VehicleKind -VehicleManifest $VehicleManifest -Python $pythonExe
        if(-not $?) { throw "validator reported failure" }
    } catch {
        $failureReason = "PASS.json validation failed: $_"
    }
}
if($failureReason -and $Profile -eq "world_copy_convert" -and
   (Test-Path (Join-Path $OutRoot "DEVICE_EVIDENCE.json")) -and -not (Test-Path $fail)) {
    # T16a: the device half is complete (PENDING_CARD). This is NOT a pass:
    # PASS.json is written only by the offline card verdict (T16a-RIG.md).
    $failureReason = "PENDING_CARD: device evidence complete; fetch the card record and run wii_telemetry_server.py --world-copy-verdict --out $OutRoot"
}
if($failureReason) {
    Write-Host "[FAIL] $failureReason; see $OutRoot"
    $summary = Join-Path $OutRoot "summary.md"
    if(Test-Path $summary) { Get-Content $summary }
    if($PowerCycleOnFailure) {
        Write-Warning "[run-wii] collection failed; invoking configured Wii power-cycle hook"
        Invoke-ConfiguredPowerCycle
    }
    exit 1
}
Write-Host "[PASS] $pass"
$summary = Join-Path $OutRoot "summary.md"
if(Test-Path $summary) { Get-Content $summary }
exit 0
