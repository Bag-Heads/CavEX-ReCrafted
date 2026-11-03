param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [string]$DolPath = "",
    [string]$TargetPath = "sd:/apps/CavEX/boot.dol",
    [int]$Port = 39019,
    [int]$TelemetryPort = 39017,
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
    # colcap-1006 R5 item 2: pool_selftest_3m column placement (rotating|lowest).
    [ValidateSet("", "rotating", "lowest")]
    [string]$PoolPlacement = "",
    [string]$ResidencyFault = "",
    [string]$AutoloadName = "",
    [string]$RunId = "",
    [string]$Netmask = "",
    [string]$WiiloadExe = "C:\msys64\opt\devkitpro\tools\bin\wiiload.exe",
    [int]$WiiloadAttempts = 8,
    [int]$WiiloadRetrySeconds = 10,
    [int]$WaitForHbcSeconds = 0,
    [int]$StageConnectTimeoutSeconds = 240,
    [int]$ChunkSize = 8192,
    [int]$ChunkDelayMs = 2,
    [string]$MakeExe = "C:\msys64\usr\bin\make.exe",
    # Stage-server auth (CWSTAGE4, 2026-09): sent as CWAUTH1 right after
    # connecting, before any CWSTAGE4/CWRUNPREP1/CWRUN1 request. Resolution
    # order: this param, then $env:CW_STAGE_TOKEN, then tools\.stage_token
    # (gitignored), then empty (fine while the card is soft-launch /
    # unprovisioned -- see docs/stage_server_auth_2026-09-02.md).
    [string]$StageToken = "",
    [switch]$AssumeServerRunning,
    [switch]$AllowHbcUpload,
    [switch]$NoRunArgs,
    [switch]$LaunchAfterStage,
    [switch]$SkipServerBuild,
    [switch]$SkipLocalLaunchPlanPreflight,
    [switch]$BuildStageServerOnly,
    [switch]$AllowDirty,
    # Hands-free hardware batch (2026-08-18): armed runs now auto-exit back to
    # the loader shortly after profile_done by default
    # (hw_run_auto_exit_ready). Pass this to keep a human-observed run on
    # screen after profile_done instead -- it stages "--cw-auto-exit off" in
    # cw_run_args.txt, which source/game/hw_run.c honors per-run.
    [switch]$NoAutoExit,
    # Seconds the armed run lingers after profile_done before auto-exiting.
    # The pull collector counts heartbeats that arrive AFTER profile_done too;
    # 0 keeps the game-side default (5s), which can cut long profiles short.
    [int]$ExitGraceSeconds = 0,
    # Stage-2 LOD reach gate (docs/stage2_lod_plan_2026-09-03.md 3): stages
    # "--cw-far-view-distance N" in cw_run_args.txt, overriding the profile's
    # own far_view_distance default (far_idle_2m/far_walk_6m otherwise pin
    # 6; every other profile leaves the normal config/default in control).
    # 0 (default) omits the flag entirely -- byte-identical to before this
    # param existed. hw_run_parse_args (source/game/hw_run.c) clamps any
    # other requested value into [MAX_VIEW_DISTANCE, MAX_FAR_VIEW_DISTANCE_
    # CAP] itself (5-16 on Wii, 10-16 on PC, both landing on the Beta "Far"
    # setting of 16 chunks) and logs a line when clamping
    # actually changed the value, so an out-of-range value here is not
    # rejected on this side -- pass -1 to explicitly force "leave the normal
    # config/default in control" on a profile that would otherwise pin one.
    [int]$FarViewDistance = 0,
    # S2 measurement (docs/stage2_lod_plan_2026-09-03.md): stages
    # "--cw-far-storage voxel|skin" in cw_run_args.txt. "" (default) omits
    # the flag entirely -- byte-identical to before this param existed;
    # config.json's "lod.far_storage" (default "voxel") stays in control.
    # NOTE (2026-09-03): source/main.c has nothing to consume this override
    # WITH yet on this branch -- the lod.far_storage/world_set_far_storage
    # config plumbing lands with the still-unmerged lane/stage2-s2-far-
    # storage lineage (see source/game/hw_run.h's enum hw_run_far_storage
    # comment for the exact commits). Staging the flag today is inert until
    # that lineage merges and a follow-up wires cfg->far_storage into main.c.
    [ValidateSet("", "voxel", "skin")]
    [string]$FarStorage = "",
    # T2 far-send share (2026-09-04, lane/far-send-share; default flipped to
    # 8, 2026-09-04, lane/far-share-default-8; docs/platform_perf_notes.md):
    # stages "--cw-far-send-near-owed N" in cw_run_args.txt, overriding
    # config.json's "lod.far_send_near_owed" (default 8 = the far ring may
    # take the near send loop's LEFTOVER per-tick budget while up to 8 near
    # chunks are still owed -- N=16 was measured to fill the far ring but peak
    # the cost, so 8 is the bounded setting; 0 is still the strict gate where
    # the far ring only fills once the whole near square is sent, the
    # far_walk_6m "far_missing == the whole annulus for the entire walk"
    # measurement this lane exists to improve).
    # -1 explicitly forces "leave the config in control". 0 here (this
    # PowerShell parameter's OWN default, distinct from the config default
    # above) omits the flag entirely, byte-identical to before this staging
    # param existed -- pass a value explicitly (0 included) to override the
    # game-side default on a staged run. hw_run_parse_args clamps values above
    # HW_RUN_FAR_SEND_NEAR_OWED_MAX (32) itself.
    [int]$FarSendNearOwed = 0,
    # Harness fixed-seed lane (2026-09-04): stages "--cw-seed N" in
    # cw_run_args.txt so two harness runs of the SAME profile see identical
    # "+new" terrain instead of the random per-create mix (screen_select_
    # world.c's sworld_finish_create) -- the noise source this flag exists to
    # kill for reproducible throughput A/Bs. "" (default) omits the flag
    # entirely -- byte-identical to before this param existed. String, not
    # [int]/[long]: hw_run_parse_args (source/game/hw_run.c) accepts a full
    # int64_t range via strtoll and rejects anything malformed (empty/
    # non-numeric/trailing-garbage) with "invalid --cw-seed value" -- no
    # range validation happens on THIS side, matching FarStorage's pattern
    # above (a bad value surfaces at Wii boot via the game's own error, not a
    # PowerShell-side guess at the valid range).
    [string]$Seed = "",
    # --cw-mp-endpoint <host:port> (2026-09-12): which server an unattended
    # mp_connect_smoke run joins, staged in cw_run_args.txt so one card can be
    # pointed at a chosen host without editing its config.json "mp.server"
    # (same stage-protocol gap -Set exists for). "" (default) omits the flag
    # entirely -- byte-identical to before this param existed, and the card's
    # own mp.server stays in control. Validated GAME-side only, matching
    # -FarStorage/-Seed above: hw_run_parse_args rejects a missing/zero/
    # out-of-range port, an empty host, a value over 63 characters, and the
    # flag on ANY profile other than mp_connect_smoke ("invalid
    # --cw-mp-endpoint value" / "requires --cw-profile mp_connect_smoke" at
    # Wii boot) rather than this script guessing the valid shapes.
    [string]$MpEndpoint = "",
    # --cw-mp-username <name> (2026-09-17, contract section 8 ruling 1 and
    # section 9 ruling 7): the mp_chat_login_3m run nonce. The server receipt
    # (tmp/wii_runs/<run id>/mc173-diag.log) names the player that submitted
    # `/login`, so the run and the receipt are bound by this one string:
    # "cw" + the first 6 hex characters of sha256(<run id>), computed below by
    # Get-ChatLoginUsername, echoed by the device in p0.session's `username`,
    # and required by wii_telemetry_server.py's ChatLoginEvidence. "" (default)
    # derives it for mp_chat_login_3m and omits the flag for every other
    # profile -- byte-identical to before this param existed. Validated
    # GAME-side like -MpEndpoint above (the value must match
    # [A-Za-z0-9_]{1,16} and the flag is refused on non-MP profiles); it is
    # never written back to the card's config.json.
    [string]$MpUsername = "",
    [switch]$TpsCpu,
    # --cw-settle: post-world_loaded settle observer (source/main.c, split
    # from -TpsCpu 2026-09-07, lane/observer-arm-flags so the tps census and
    # the settle observer can be bisected independently on real hardware --
    # before this switch existed, -TpsCpu armed BOTH). Omitted (default):
    # byte-identical to before this switch existed.
    [switch]$Settle,
    # --cw-carve-census: allocation census observer (source/main.c,
    # carve_observer_start()). Omitted (default): byte-identical to before
    # this switch existed.
    [switch]$CarveCensus,
    # --cw-tel-nonblock: opt-in non-blocking telemetry-send DIAGNOSTIC
    # (2026-09-08, sender-wedge lane; see hw_telemetry.h's
    # hw_telemetry_set_nonblock_experiment doc). Omitted (default):
    # byte-identical to before this switch existed -- the telemetry socket
    # stays blocking, the 2026-09-06 root fix.
    [switch]$TelemetryNonBlock,
    # --cw-set key=value (repeatable, up to HW_RUN_CW_SET_MAX (8) --
    # hw_run.h/hw_run.c): run-scoped INTEGER config override for a staged
    # hardware run (2026-09-08, lane/carve-observer-boot) -- e.g.
    # "memory.displaylist_compact=1" for a paired compact-0-vs-1 A/B on the
    # same build/seed, without editing the staged card's config.json (the
    # stage protocol writes only the .dol and cw_run_args.txt). Each element
    # is staged verbatim as its own "--cw-set <element>" line; a malformed
    # element is ignored on the game side (counted, not fatal -- hw_run.c's
    # --cw-set branch), same as every other value validated game-side rather
    # than here (FarStorage's pattern). Empty array (default): byte-identical
    # to before this param existed.
    [string[]]$Set = @()
)

$ErrorActionPreference = "Stop"
if($ServiceDiagnostic -and ($Profile -ne "perf_stress_5m" -or $NoRunArgs)) {
    throw "-ServiceDiagnostic requires perf_stress_5m with run args."
}
if($ResidencyFault) {
    if($ResidencyFault -cnotmatch '^(host|guest)@([1-9][0-9]*)$' -or
       [long]$Matches[2] -gt 3600) {
        throw "-ResidencyFault must be host@seconds or guest@seconds (1..3600)."
    }
    if($Profile -eq "none" -or $NoRunArgs) {
        throw "-ResidencyFault requires an armed hardware profile and run arguments."
    }
}
if(($Profile -eq "host_vehicle_smoke_8m") -ne (-not [string]::IsNullOrWhiteSpace($VehicleKind))) {
    throw "-VehicleKind boat|pig|cart is required only for host_vehicle_smoke_8m."
}
if($Profile -eq "host_vehicle_smoke_8m" -and [string]::IsNullOrWhiteSpace($VehicleManifest)) {
    throw "host_vehicle_smoke_8m requires -VehicleManifest (frozen release/config/mode provenance)."
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
if([string]::IsNullOrWhiteSpace($DolPath)) {
    $DolPath = Join-Path $RepoRoot "CaveWii_v2.dol"
}
if([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "staged_$($Profile)_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
}
if(-not $BuildStageServerOnly) {
    $DolPath = (Resolve-Path $DolPath).Path
}
$ServerRoot = Join-Path $RepoRoot "tools\wii_stage_server"
$ServerDol = Join-Path $ServerRoot "wii_stage_server.dol"
# spaced-paths (2026-10-30): this script does not CWHELLO1-probe the server's
# capabilities, and an older server cuts a path at its first space (the stray
# ".../saves/CaveWii" file). A .dol target never needs a space, so refuse one
# here; spaced asset/world paths go through tools/put_wii_stage_file.ps1,
# which checks the "spaced-paths" capability first.
if($TargetPath -match '[\s"]' -or $TargetPath -match '[^\x20-\x7e]') {
    throw "refusing -TargetPath '$TargetPath': a .dol target must be printable ASCII without spaces or quotes (use tools/put_wii_stage_file.ps1 for spaced paths)"
}

function Convert-PrefixToNetmask {
    param([int]$PrefixLength)
    [uint32]$mask = 0
    if($PrefixLength -gt 0) {
        $mask = ([uint32]::MaxValue -shl (32 - $PrefixLength))
    }
    return "{0}.{1}.{2}.{3}" -f (($mask -shr 24) -band 255), (($mask -shr 16) -band 255), (($mask -shr 8) -band 255), ($mask -band 255)
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

function Get-ArpEntry {
    param([string]$RemoteHost)
    if($RemoteHost -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
        return ""
    }
    $line = arp -a 2>$null | Select-String -SimpleMatch $RemoteHost | Select-Object -First 1
    if($line) { return $line.Line.Trim() }
    return ""
}

function Update-ArpEntry {
    param(
        [string]$RemoteHost,
        [string]$SourceIp
    )
    if($RemoteHost -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
        return ""
    }
    try {
        & arp -d $RemoteHost 2>$null | Out-Null
    } catch {
        # Non-admin shells cannot delete ARP entries. The following ping still
        # refreshes a missing/stale entry when the host is present.
    }
    try {
        if(-not [string]::IsNullOrWhiteSpace($SourceIp)) {
            & ping -S $SourceIp -n 1 -w 800 $RemoteHost 2>$null | Out-Null
        } else {
            & ping -n 1 -w 800 $RemoteHost 2>$null | Out-Null
        }
    } catch {
    }
    return Get-ArpEntry $RemoteHost
}

function Connect-TcpWithTimeout {
    param(
        [string]$RemoteHost,
        [int]$RemotePort,
        [int]$TimeoutMs
    )
    $tcp = [System.Net.Sockets.TcpClient]::new()
    try {
        $iar = $tcp.BeginConnect($RemoteHost, $RemotePort, $null, $null)
        if(-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs, $false)) {
            $tcp.Close()
            return $null
        }
        $tcp.EndConnect($iar)
        return $tcp
    } catch {
        $tcp.Close()
        return $null
    }
}


# 2026-09-02 real-Wii fetch/stage corruption bug: Invoke-StageAuth,
# Send-StageFile's Drain-StageLines, and the CWRUNPREP1/CWRUN1 replies all
# used to read protocol lines through a [System.IO.StreamReader] wrapping
# the same NetworkStream that Send-StageFile also writes raw file bytes to
# directly. Two problems with that, both real:
#   1. Drain-StageLines called $Reader.ReadLine() in a tight non-blocking
#      poll loop by setting $Stream.ReadTimeout to 25ms and catching the
#      resulting IOException on every timeout. Reusing a StreamReader after
#      its underlying stream throws mid-Read leaves the StreamReader's
#      internal buffer/position in an UNDEFINED state (a documented .NET
#      hazard) -- exactly the "polls constantly, times out constantly"
#      shape this loop has on every single chunk write.
#   2. More generally, a StreamReader reads AHEAD into its own internal
#      buffer (it has to, to find line terminators) -- any byte it
#      buffered but did not hand back via ReadLine() is invisible to (and
#      lost from) a later DIRECT $Stream.Read() on raw payload bytes. This
#      file's own CWFETCH1-sibling tooling doesn't hit that today (nothing
#      here reads a binary reply body after using $reader), but an ad hoc
#      script built by copying this exact Send-StageFile/Drain-StageLines
#      pattern (scratchpad put_kz.ps1, staging sd:/apps/CavEX/assets/
#      art/kz.png) reproduced real byte-for-byte corruption on real Wii
#      hardware: the SD-side file passed this server's own 3-way tmp/
#      rename/expected sha256 verification, but a subsequent CWFETCH1 GET
#      of that same path (tools/fetch_wii_stage_file.ps1, which reads the
#      raw NetworkStream directly and never hit this bug itself) received
#      12572 bytes whose content had the file's own leading ~1.5KB header
#      reinserted mid-stream -- the signature of a receive-side reader
#      losing track of its position under exactly this StreamReader/
#      timeout-polling pattern.
#
# Fixed by dropping StreamReader entirely: every protocol line in this file
# is now read byte-at-a-time directly off the NetworkStream (the same safe,
# already-proven idiom tools/fetch_wii_stage_file.ps1's Read-AsciiLine
# uses), and the non-blocking poll during chunk writes checks
# $Stream.DataAvailable BEFORE reading instead of starting a read and
# racing its timeout -- a single-byte NetworkStream.Read() is all-or-throw
# (it can never hand back a partial byte), so there is no partial state
# left behind to corrupt on the next call.
function Read-AsciiLine {
    param([System.Net.Sockets.NetworkStream]$Stream)
    $bytes = New-Object System.Collections.Generic.List[byte]
    $one = New-Object byte[] 1
    while($true) {
        $n = $Stream.Read($one, 0, 1)
        if($n -le 0) {
            throw "stage server closed while reading line"
        }
        if($one[0] -eq 10) {
            break
        }
        if($one[0] -ne 13) {
            $bytes.Add($one[0])
        }
    }
    return [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
}

function Read-AsciiLineIfAvailable {
    param([System.Net.Sockets.NetworkStream]$Stream)
    if(-not $Stream.DataAvailable) {
        return $null
    }
    return Read-AsciiLine -Stream $Stream
}

function Get-BytesSha256Hex {
    param([byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($Bytes)
        return -join ($hash | ForEach-Object { $_.ToString("x2") })
    } finally {
        $sha.Dispose()
    }
}

function Get-ChatLoginUsername {
    param([string]$Name)
    # Contract section 9 ruling 7: the mp_chat_login_3m run nonce is
    # "cw" + the first 6 hex characters of sha256(<run id string>), lowercase
    # (8 chars, inside Beta's 16-character limit). Keep this derivation
    # byte-identical to wii_telemetry_server.py's chat_login_username() and to
    # run_wii_hardware.ps1's copy -- the collector re-derives it independently
    # and a mismatch FAILS the run's receipt, by design.
    $hex = Get-BytesSha256Hex ([System.Text.Encoding]::UTF8.GetBytes($Name))
    return "cw" + $hex.Substring(0, 6)
}

function Get-ResolvedStageToken {
    param([string]$Explicit)
    if(-not [string]::IsNullOrWhiteSpace($Explicit)) {
        return $Explicit.Trim()
    }
    if(-not [string]::IsNullOrWhiteSpace($env:CW_STAGE_TOKEN)) {
        return $env:CW_STAGE_TOKEN.Trim()
    }
    $tokenFile = Join-Path $RepoRoot "tools\.stage_token"
    if(Test-Path $tokenFile) {
        $fileToken = (Get-Content -Raw -LiteralPath $tokenFile).Trim()
        if(-not [string]::IsNullOrWhiteSpace($fileToken)) {
            return $fileToken
        }
    }
    return ""
}

# Sends CWAUTH1 and interprets the reply. Throws on outright auth failure
# (wrong/missing token against a PROVISIONED card); prints the soft-launch
# guidance and returns normally when the server has no token file yet, per
# docs/stage_server_auth_2026-09-02.md.
function Invoke-StageAuth {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Token
    )
    $line = if([string]::IsNullOrWhiteSpace($Token)) { "CWAUTH1`n" } else { "CWAUTH1 $Token`n" }
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($line)
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
    $reply = Read-AsciiLine -Stream $Stream
    if($null -eq $reply) {
        throw "stage server closed before CWAUTH1 reply"
    }
    try {
        $json = $reply | ConvertFrom-Json
    } catch {
        throw "stage server returned invalid CWAUTH1 JSON: $reply"
    }
    if($json.status -ne "PASS") {
        throw "stage auth failed: $reply (does -StageToken / `$env:CW_STAGE_TOKEN / tools\.stage_token match sd:/apps/CavEX/stage_token.txt on the card?)"
    }
    if($json.reason -eq "no_token_provisioned") {
        Write-Host "[stage-auth] server has no token provisioned (soft-launch) -- run:"
        Write-Host "  powershell -NoProfile -ExecutionPolicy Bypass -File tools\provision_wii_sd.ps1 -SdRoot <drive>"
        Write-Host "  to close this window before relying on auth."
    } else {
        Write-Host "[stage-auth] $reply"
    }
}

function Assert-StagePass {
    param(
        [string]$Line,
        [string]$RemotePath,
        [int64]$ExpectedBytes,
        [string]$ExpectedSha256
    )
    try {
        $reply = $Line | ConvertFrom-Json
    } catch {
        throw "stage server returned invalid JSON for ${RemotePath}: $Line"
    }
    if($reply.status -ne "PASS") {
        throw "stage failed for ${RemotePath}: $Line"
    }
    if([int64]$reply.bytes -ne $ExpectedBytes) {
        throw "stage byte-count mismatch for ${RemotePath}: expected $ExpectedBytes got $($reply.bytes)"
    }
    if(-not ($reply.PSObject.Properties.Name -contains "sha256")) {
        throw "stage server did not return SD-side sha256 for ${RemotePath}; rebuild/relaunch the stage server"
    }
    $actualSha256 = ([string]$reply.sha256).ToLowerInvariant()
    if($actualSha256 -ne $ExpectedSha256) {
        throw "stage sha256 mismatch for ${RemotePath}: expected $ExpectedSha256 got $actualSha256"
    }
    return $reply
}

function Assert-RunPrepPass {
    param(
        [string]$Line,
        [int64]$ExpectedDolBytes,
        [string]$ExpectedDolSha256,
        [int64]$ExpectedArgsBytes,
        [string]$ExpectedArgsSha256,
        [string]$ExpectedReason = "run_ready"
    )
    try {
        $reply = $Line | ConvertFrom-Json
    } catch {
        throw "stage server returned invalid runprep JSON: $Line"
    }
    if($reply.status -ne "PASS" -or $reply.reason -ne $ExpectedReason) {
        throw "runprep failed: $Line"
    }
    foreach($name in @(
        "dol_bytes","dol_sha256","args_bytes","args_sha256",
        "entry_point","min_address","max_address","argv_count",
        "argv_marker_address","argv_marker_file_offset",
        "argv_struct_address","argv_struct_bytes",
        "command_line_address","command_line_bytes"
    )) {
        if(-not ($reply.PSObject.Properties.Name -contains $name)) {
            throw "runprep reply missing $name; rebuild/relaunch the stage server: $Line"
        }
    }
    if([int64]$reply.dol_bytes -ne $ExpectedDolBytes) {
        throw "runprep DOL byte-count mismatch: expected $ExpectedDolBytes got $($reply.dol_bytes)"
    }
    $actualDolSha256 = ([string]$reply.dol_sha256).ToLowerInvariant()
    if($actualDolSha256 -ne $ExpectedDolSha256) {
        throw "runprep DOL sha256 mismatch: expected $ExpectedDolSha256 got $actualDolSha256"
    }
    if([int64]$reply.args_bytes -ne $ExpectedArgsBytes) {
        throw "runprep args byte-count mismatch: expected $ExpectedArgsBytes got $($reply.args_bytes)"
    }
    $actualArgsSha256 = ([string]$reply.args_sha256).ToLowerInvariant()
    if($actualArgsSha256 -ne $ExpectedArgsSha256) {
        throw "runprep args sha256 mismatch: expected $ExpectedArgsSha256 got $actualArgsSha256"
    }
    if([int64]$reply.argv_struct_bytes -ne 24) {
        throw "runprep argv struct size mismatch: expected 24 got $($reply.argv_struct_bytes)"
    }
    if([int64]$reply.argv_marker_file_offset -lt 256) {
        throw "runprep argv marker offset is not inside a DOL section: $($reply.argv_marker_file_offset)"
    }
    if([int64]$reply.command_line_bytes -le 0) {
        throw "runprep command line plan is empty"
    }
    return $reply
}

function Invoke-LocalLaunchPlanPreflight {
    param(
        [string]$LocalDolPath,
        [byte[]]$RunArgBytes
    )
    # CTest supplies its actual target path, including an out-of-tree build
    # directory and the host executable suffix. Standalone use keeps auto-build.
    $verifier = $env:CW_LAUNCH_PLAN_VERIFIER
    if([string]::IsNullOrWhiteSpace($verifier)) {
        $verifierName = if($env:OS -eq "Windows_NT") { "verify_wii_launch_plan.exe" } else { "verify_wii_launch_plan" }
        $verifier = Join-Path (Join-Path $RepoRoot "build-verify") $verifierName
    } elseif(-not (Test-Path -LiteralPath $verifier -PathType Leaf)) {
        throw "configured launch-plan verifier missing: $verifier"
    }
    if(-not (Test-Path $verifier)) {
        Write-Host "[local-runprep] verifier missing; building $verifier"
        $buildDir = Join-Path $RepoRoot "build-verify"
        if(-not (Test-Path (Join-Path $buildDir "build.ninja"))) {
            & cmake -B $buildDir -S $RepoRoot -G Ninja
            if($LASTEXITCODE -ne 0) {
                throw "failed to configure build-verify for local launch-plan verifier"
            }
        }
        & cmake --build $buildDir --target verify_wii_launch_plan --parallel 6
        if($LASTEXITCODE -ne 0 -or -not (Test-Path $verifier)) {
            throw "failed to build local launch-plan verifier at $verifier"
        }
    }
    $tmpDir = Join-Path $RepoRoot "tmp"
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
    $tmpArgs = Join-Path $tmpDir ("stage_wii_dol_local_args_{0}.txt" -f [guid]::NewGuid().ToString("N"))
    [System.IO.File]::WriteAllBytes($tmpArgs, $RunArgBytes)
    try {
        $line = & $verifier $LocalDolPath $tmpArgs
        $verifyExit = $LASTEXITCODE
    } finally {
        Remove-Item -LiteralPath $tmpArgs -Force -ErrorAction SilentlyContinue
    }
    if($verifyExit -ne 0) {
        throw "local launch-plan verifier failed: $line"
    }
    try {
        $reply = $line | ConvertFrom-Json
    } catch {
        throw "local launch-plan verifier returned invalid JSON: $line"
    }
    if($reply.status -ne "PASS") {
        throw "local launch-plan verifier rejected staged DOL: $line"
    }
    Write-Host "[local-runprep] entry=$($reply.entry_point) argv_struct=$($reply.argv_struct_address) cmd=$($reply.command_line_address):$($reply.command_line_bytes)"
}

function Invoke-StageServerBuild {
    if(-not (Test-Path $MakeExe)) {
        throw "make.exe not found: $MakeExe"
    }
    $tmp = Join-Path $RepoRoot ".buildtmp\wii_stage_server_tmp"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $oldDevkitPro = $env:DEVKITPRO
    $oldDevkitPpc = $env:DEVKITPPC
    $oldPath = $env:PATH
    $oldTmp = $env:TMP
    $oldTemp = $env:TEMP
    $oldTmpDir = $env:TMPDIR
    $beforeSha = ""
    if(Test-Path $ServerDol) {
        $beforeSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $ServerDol).Hash.ToLowerInvariant()
    }
    try {
        # Forward slashes: make runs recipes through bash, which eats backslashes
        # (then libogc's include dir, incl. arpa/inet.h, is not found).
        $env:DEVKITPRO = "C:/msys64/opt/devkitpro"
        $env:DEVKITPPC = "C:/msys64/opt/devkitpro/devkitPPC"
        $env:TMP = $tmp
        $env:TEMP = $tmp
        $env:TMPDIR = $tmp
        $env:PATH = "C:\msys64\opt\devkitpro\devkitPPC\bin;C:\msys64\usr\bin;$env:PATH"
        Write-Host "[stage-server-build] tmp=$tmp"
        if(-not [string]::IsNullOrWhiteSpace($beforeSha)) {
            Write-Host "[stage-server-build] before_sha256=$beforeSha"
        }
        Push-Location $ServerRoot
        try {
            & $MakeExe DEVKITPRO=$env:DEVKITPRO DEVKITPPC=$env:DEVKITPPC clean
            if($LASTEXITCODE -ne 0) {
                throw "stage server clean failed with exit code $LASTEXITCODE"
            }
            if(Test-Path $ServerDol) {
                throw "stage server clean left stale DOL in place: $ServerDol"
            }
            & $MakeExe DEVKITPRO=$env:DEVKITPRO DEVKITPPC=$env:DEVKITPPC
            if($LASTEXITCODE -ne 0) {
                throw "stage server build failed with exit code $LASTEXITCODE"
            }
        } finally {
            Pop-Location
        }
    } finally {
        $env:DEVKITPRO = $oldDevkitPro
        $env:DEVKITPPC = $oldDevkitPpc
        $env:PATH = $oldPath
        $env:TMP = $oldTmp
        $env:TEMP = $oldTemp
        $env:TMPDIR = $oldTmpDir
    }
    if(-not (Test-Path $ServerDol)) {
        throw "stage server build did not produce $ServerDol"
    }
    $afterSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $ServerDol).Hash.ToLowerInvariant()
    $afterMd5 = (Get-FileHash -Algorithm MD5 -LiteralPath $ServerDol).Hash.ToLowerInvariant()
    $afterBytes = (Get-Item -LiteralPath $ServerDol).Length
    Write-Host "[stage-server-build] after_md5=$afterMd5 sha256=$afterSha bytes=$afterBytes"
}

function Assert-HardwareProofTree {
    if($AllowDirty) {
        Write-Warning "AllowDirty set: staged DOL may not be source-clean."
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
            throw "refusing to stage hardware proof from dirty source tree; commit/stash changes or pass -AllowDirty for development only"
        }
    } finally {
        Pop-Location
    }
}

if($BuildStageServerOnly) {
    Invoke-StageServerBuild
    exit 0
}

Assert-HardwareProofTree

if(-not $AssumeServerRunning -and -not $AllowHbcUpload) {
    throw "refusing to upload the stage server through HBC by default: accepted HBC uploads have black-screened the Wii and required a hard power hold. Launch DOL Stage Server from SD and rerun with -AssumeServerRunning, or pass -AllowHbcUpload for an explicit recovery-risk experiment."
}


if([string]::IsNullOrWhiteSpace($Netmask)) {
    $localIp = Get-LocalIpForRemote $WiiHost
    if(Get-Command Get-NetIPAddress -ErrorAction SilentlyContinue) {
        $addr = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -eq $localIp } |
            Select-Object -First 1
    } else {
        $addr = [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
            ForEach-Object { $_.GetIPProperties().UnicastAddresses } |
            Where-Object { $_.Address.ToString() -eq $localIp } |
            Select-Object -First 1
    }
    if($addr) {
        $Netmask = Convert-PrefixToNetmask ([int]$addr.PrefixLength)
        Write-Host "[net] local $localIp/$($addr.PrefixLength) -> Wii netmask $Netmask"
    }
}

$bytes = [System.IO.File]::ReadAllBytes($DolPath)
if($VehicleKind) {
    $vehiclePacket = Get-Content -Raw -LiteralPath $VehicleManifest | ConvertFrom-Json
    if($vehiclePacket.contract -notin @("CONTRACT-v1.2", "CONTRACT-v1.3") -or
       $vehiclePacket.release_sha256 -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $DolPath).Hash.ToLowerInvariant() -or
       $vehiclePacket.collector_sha256 -ne (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $RepoRoot "tools/wii_telemetry_server.py")).Hash.ToLowerInvariant()) {
        throw "vehicle frozen manifest/release/collector mismatch before staging."
    }
    if(-not [Text.Encoding]::ASCII.GetString($bytes).Contains("CaveWii (impl. B1.7.3) $($vehiclePacket.build_id)")) {
        throw "vehicle frozen manifest build stamp absent from staged DOL."
    }
    $vehicleConfig = $vehiclePacket.kinds.$VehicleKind
    if($vehicleConfig.geometry_proven -ne $true -or $vehicleConfig.transfer_mode -notin @("contact", "fixture_request")) {
        throw "vehicle mode must be predeclared after PC geometry proof."
    }
    $vehicleConfigPath = [string]$vehicleConfig.config_path
    if(-not [IO.Path]::IsPathRooted($vehicleConfigPath)) {
        $vehicleConfigPath = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $VehicleManifest)) $vehicleConfigPath
    }
    if(-not (Test-Path -LiteralPath $vehicleConfigPath) -or
       $vehicleConfig.config_sha256 -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $vehicleConfigPath).Hash.ToLowerInvariant()) {
        throw "vehicle frozen effective configuration changed before staging."
    }
    if($Seed -and $Seed -ne "12345") { throw "vehicle seed must be 12345." }
    if($AutoloadName) { throw "vehicle profile creates its own fresh disposable world; competing autoload forbidden." }
}
$argBytes = $null
if(-not $NoRunArgs) {
    if($Profile -eq "none") {
        # A non-empty token is required by the stage server's launch-plan
        # validator. hw_run_parse_args intentionally ignores this unknown
        # token, leaving the hardware harness disabled for ordinary play.
        $argText = "noop`n"
    } else {
    $duration = switch($Profile) {
        # 100, not 60: since the post-PASS quiesce fix (df84d49) the game goes
        # silent at profile_done, so the trailing heartbeats that used to pad
        # this gate past min_hb=50 no longer exist, and the fixture world eats
        # ~29s of the duration before beat 1 (measured 2026-08-19: 31 beats in
        # a 60s run). 100s yields 50+ world-loaded beats; min_hb unchanged --
        # the gate now gets MORE in-world data, not a lower bar.
        "heartbeat_selftest" { 100 }
        # 150, not 120: the collector's pass window is min_hb=100 beats counted
        # from the FIRST heartbeat, and these fixture worlds take ~29s to load
        # on real hardware before beat 1 -- a 120s game-side run closes the
        # socket at ~91 beats, a deterministic miss (measured 2026-08-18: 92,
        # 91). 150s outlives load+window; min_hb stays unchanged.
        "far_idle_2m" { 150 }
        "far_walk_6m" { 360 }
        "perf_stress_5m" { 300 }
        "watchdog_inject" { 30 }
        "redstone_clock_2m" { 150 }
        "mob_motion_2m" { 150 }
        "menus_dwell" { 240 }
        "menus_dwell_creative" { 240 } # same script/timing as menus_dwell -- see hw_run.h's HW_RUN_PROFILE_MENUS_DWELL_CREATIVE comment
        "save_migrate" { 300 }
        "world_copy_convert" { 300 } # hw_run.c HW_RUN_PROFILE_WORLD_COPY_CONVERT
        # Pre-existing gap fixed in passing (2026-08-31, mp_lan_walk_10m build):
        # this profile had NO entry here and was silently staged with the 60s
        # `default` below instead of the 120s both hw_run.c's own
        # HW_RUN_PROFILE_MP_CONNECT_SMOKE default and wii_telemetry_server.py's
        # PROFILES["mp_connect_smoke"]["duration"] expect -- the exact
        # duration-table-mismatch trap this file's other entries exist to
        # avoid. 60s leaves almost no margin past the ~25s measured DHCP
        # settle before min_hb=60 heartbeats could ever arrive.
        "mp_connect_smoke" { 120 }
        # mp_lan_walk_10m (S5.3-era >3s-stall hunt profile, PROJECT_MEMORY):
        # un-padded, matching hw_run.c's HW_RUN_PROFILE_MP_LAN_WALK_10M default
        # and wii_telemetry_server.py's PROFILES["mp_lan_walk_10m"]["duration"]
        # -- same "duration matches everywhere" discipline as far_walk_6m/
        # mp_connect_smoke above (neither of those pad on this side either).
        "mp_lan_walk_10m" { 600 }
        # mp_reconnect_soak (S5.3 hunt plumbing): matches hw_run.c's
        # HW_RUN_PROFILE_MP_RECONNECT_SOAK default and wii_telemetry_
        # server.py's PROFILES["mp_reconnect_soak"]["duration"] exactly --
        # same "duration matches everywhere" discipline as mp_connect_smoke/
        # mp_lan_walk_10m above (un-padded on this side too).
        "mp_reconnect_soak" { 480 }
        # keepup_walk_6m (2026-09-04, deterministic keep-up profile): SAME
        # 360s window as far_walk_6m -- see hw_run.c's HW_RUN_PROFILE_
        # KEEPUP_WALK_6M comment and wii_telemetry_server.py's PROFILES
        # entry (same "duration matches everywhere" discipline).
        "split_anchor_6m" { 360 }
        # split_play_2m (2026-09-07): the first profile with a REAL second
        # gameplay body; same "duration matches everywhere" discipline.
        "split_play_2m" { 120 }
        "split_play_8m" { 360 }
        # split_actions_6m (2026-09-14, S1-d): split_play_8m window plus
        # the scripted guest action sequence.
        "split_actions_6m" { 360 }
        # split_recovery_6m (2026-09-17): scripted split recovery proxy; same window as split_actions_6m
        "split_recovery_6m" { 360 }
        # mp_chat_login_3m (2026-09-17, b1-chat-keyboard-0917): the scripted
        # OSK chat-login proof. 180 s is the contract's TOTAL cap (section 3),
        # matching hw_run.c's default and wii_telemetry_server.py's
        # PROFILES["mp_chat_login_3m"]["duration"] -- same "duration matches
        # everywhere" discipline as the mp_* profiles above, un-padded.
        "mp_chat_login_3m" { 180 }
        # mp_chat_chord_1m (2026-09-19, b1-chat-opener-0919): the
        # separate debug-chord leg. 60 s, matching hw_run.c and
        # wii_telemetry_server.py PROFILES["mp_chat_chord_1m"].
        "mp_chat_chord_1m" { 60 }
        # mp_play_10m (2026-09-21, push-0921 lane B): the unattended LAN play
        # proof. THIS TABLE IS THE DEVICE CLOCK ONLY: its number is written
        # into the staged cw_run_args as --cw-duration, which overrides
        # hw_run.c's cfg->duration_seconds (hw_run.c:894). It is not a
        # collector timeout and not the heartbeat floor. 1260 s = hw_run.c's
        # own default for this profile, deliberately 60 s LONGER than
        # mp_play_core.h's MPP_TOTAL_CAP_MS (1 200 000 ms) so a stuck run
        # reports the driver's named failure INSIDE the profile window
        # (hw_run.c:490-517); staging 1200 would cut the device off at the
        # exact millisecond of its own cap and lose that verdict. Reconciled
        # 2026-09-21 (push-0921/B6) from 1200 -- same "duration matches
        # everywhere" discipline as the mp_* profiles above, un-padded. A
        # legitimate run (~13 min) never reaches it; the driver ends the run.
        "mp_play_10m" { 1260 }
        "mp_pause_menu_3m" { 180 }
        "split_portal_quit_4m" { 240 }
        "split_dims_6m" { 360 }
        "host_vehicle_smoke_8m" { 480 }
        "split_offer_piston_2m" { 140 }
        # colcap-1006 R5 item 2: the self-test owns completion (170 s cap).
        "pool_selftest_3m" { 180 }
        # SPEC R12: the save/reload driver owns completion (170 s cap).
        "save_reload_3m" { 180 }
        # colcap-1006 R5 item 3: six measured minutes on the readiness clock, like split_anchor_6m.
        "split_regrow_8m" { 360 }
        "keepup_walk_6m" { 360 }
        default { 60 }
    }
    if($ServiceDiagnostic) { $duration = 600 }
    $argText = "--cw-run-id $RunId --cw-tel-host pull --cw-tel-port $TelemetryPort --cw-tel-mode pull --cw-profile $Profile --cw-duration $duration"
    if($VehicleKind) {
        $argText += " --cw-vehicle-kind $VehicleKind --cw-vehicle-transfer $($vehicleConfig.transfer_mode)"
    }
    if(@("split_portal_quit_4m", "split_dims_6m") -contains $Profile) {
        $argText += " --cw-portal-remaining-ticks $PortalRemainingTicks"
    }
    if($Profile -eq "save_migrate") {
        $argText += " --cw-save-migrate-regions $SaveMigrateRegions"
    }
    if($Profile -eq "world_copy_convert") {
        if([string]::IsNullOrWhiteSpace($WccRoot)) { throw "world_copy_convert requires -WccRoot" }
        $v = if($WccVariant) { $WccVariant } else { "ok" }
        $argText += " --cw-wcc-variant $v --cw-wcc-root $WccRoot"
    } elseif($WccVariant -or $WccRoot) {
        throw "-WccVariant/-WccRoot are only for world_copy_convert"
    }
    if(-not [string]::IsNullOrWhiteSpace($AutoloadName)) {
        if($AutoloadName -match '\s') {
            $nameHex = [BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes($AutoloadName)).Replace('-', '').ToLowerInvariant()
            $argText += " --cw-autoload-name-hex $nameHex"
        } else {
            $argText += " --cw-autoload-name $AutoloadName"
        }
    }
    if(-not [string]::IsNullOrWhiteSpace($Netmask)) {
        $argText += " --cw-netmask $Netmask"
    }
    if($NoAutoExit) {
        $argText += " --cw-auto-exit off"
    }
    if($ExitGraceSeconds -gt 0) {
        $argText += " --cw-exit-grace $ExitGraceSeconds"
    }
    if($FarViewDistance -ne 0) {
        $argText += " --cw-far-view-distance $FarViewDistance"
    }
    if(-not [string]::IsNullOrWhiteSpace($FarStorage)) {
        $argText += " --cw-far-storage $FarStorage"
    }
    if($FarSendNearOwed -ne 0) {
        $argText += " --cw-far-send-near-owed $FarSendNearOwed"
    }
    if(-not [string]::IsNullOrWhiteSpace($Seed)) {
        $argText += " --cw-seed $Seed"
    }
    if(-not [string]::IsNullOrWhiteSpace($MpEndpoint)) {
        $argText += " --cw-mp-endpoint $MpEndpoint"
    }
    # The chat-login run nonce (section 9 ruling 7): derived from the run id
    # unless the caller pinned one, and staged only when it has a value.
    if(@("mp_chat_login_3m","mp_chat_chord_1m","mp_play_10m","mp_pause_menu_3m") -contains $Profile -and [string]::IsNullOrWhiteSpace($MpUsername)) {
        $MpUsername = Get-ChatLoginUsername $RunId
    }
    if(-not [string]::IsNullOrWhiteSpace($MpUsername)) {
        $argText += " --cw-mp-username $MpUsername"
    }
    $argText += " --cw-anchor-sep $AnchorSep"
    if(($Profile -eq "pool_selftest_3m") -ne (-not [string]::IsNullOrWhiteSpace($PoolPlacement))) {
        throw "-PoolPlacement rotating|lowest is required for (and only for) pool_selftest_3m."
    }
    if($PoolPlacement) { $argText += " --cw-pool-placement $PoolPlacement" }
    if($ResidencyFault) { $argText += " --cw-rz-fault $ResidencyFault" }
    if($TpsCpu) { $argText += " --cw-tps-cpu" }
    if($Settle) { $argText += " --cw-settle" }
    if($CarveCensus) { $argText += " --cw-carve-census" }
    if($TelemetryNonBlock) { $argText += " --cw-tel-nonblock" }
    # Split each element on ',' before staging: a direct human call can pass a
    # real PowerShell array ("-Set 'a=1','b=2'", each element comma-free), but
    # run_staged_wii_hardware.ps1 forwards $Set across a powershell.exe
    # process boundary (raw argv, no PowerShell array-literal parsing there),
    # so it joins the whole array into ONE "-Set a=1,b=2" token first --
    # PowerShell's OWN "-Set X -Set Y" repeated-flag form errors ("specified
    # more than once") once it crosses that boundary, which is why a single
    # joined token is the forwarding-safe shape. Splitting here handles both
    # calling styles identically; a lone element with no comma is a no-op.
    $setCount = 0
    foreach($kv in $Set) {
        foreach($part in ($kv -split ',')) {
            if(-not [string]::IsNullOrWhiteSpace($part)) {
                if($part -cnotmatch '^[A-Za-z0-9_.]+=-?[0-9]+$') {
                    throw "-Set requires one integer key=value per entry: $part"
                }
                if(++$setCount -gt 6) { throw "run args support at most 6 --cw-set entries." }
                if($ServiceDiagnostic -and $part -match '^debug\.(service_diagnostic_int|service_observer_int|music_continuous_int|perf_stress_hold_health_int|music_force|autopilot|autopilot_place)=') {
                    throw "-Set conflicts with service diagnostic card config: $part"
                }
                $argText += " --cw-set $part"
            }
        }
    }
    $argText += "`n"
    }
    $argBytes = [System.Text.Encoding]::ASCII.GetBytes($argText)
    if([regex]::Matches($argText, '(?<!\S)--cw-set(?=\s|$)').Count -gt 6) {
        throw "run args support at most 6 --cw-set tokens."
    }
    if($argBytes.Length -gt 511) { throw "run args exceed the 511-byte limit." }
    if($SkipLocalLaunchPlanPreflight) {
        Write-Warning "SkipLocalLaunchPlanPreflight set: malformed DOL/args may burn a Wii staging attempt"
    } else {
        Invoke-LocalLaunchPlanPreflight -LocalDolPath $DolPath -RunArgBytes $argBytes
    }
}

if($ServiceDiagnostic) {
    . (Join-Path $PSScriptRoot "service_diagnostic_config.ps1")
    Set-StagedServiceDiagnosticConfig -WiiHost $WiiHost -Port $Port -ConnectTimeoutSeconds $StageConnectTimeoutSeconds -StageToken $StageToken
}

if(-not $AssumeServerRunning -and -not $SkipServerBuild) {
    Invoke-StageServerBuild
}
if(-not $AssumeServerRunning -and -not (Test-Path $ServerDol)) {
    throw "missing $ServerDol"
}

if($AssumeServerRunning) {
    Write-Host "[stage-server] assuming Wii stage server is already running on ${WiiHost}:$Port"
} else {
    $oldWiiload = $env:WIILOAD
    $env:WIILOAD = "tcp:$WiiHost"
    $serverArgs = @($ServerDol, "--port", "$Port")
    if(-not [string]::IsNullOrWhiteSpace($Netmask)) {
        $serverArgs += @("--netmask", $Netmask)
    }
    $launched = $false
    try {
        $attempt = 0
        $deadline = $null
        if($WaitForHbcSeconds -gt 0) {
            $deadline = (Get-Date).AddSeconds($WaitForHbcSeconds)
            Write-Host "[wiiload-stage] waiting up to ${WaitForHbcSeconds}s for HBC wiiload"
        }
        while(-not $launched) {
            $attempt++
            if($WaitForHbcSeconds -le 0 -and $attempt -gt $WiiloadAttempts) {
                break
            }
            if($WaitForHbcSeconds -gt 0 -and (Get-Date) -gt $deadline) {
                break
            }
            $attemptLabel = if($WaitForHbcSeconds -gt 0) {
                "$attempt, deadline $($deadline.ToString('HH:mm:ss'))"
            } else {
                "$attempt/$WiiloadAttempts"
            }
            if($attempt -eq 1 -or ($attempt % 3) -eq 0) {
                $arp = Update-ArpEntry -RemoteHost $WiiHost -SourceIp $localIp
                if([string]::IsNullOrWhiteSpace($arp)) {
                    Write-Host "[lan] no ARP reply for $WiiHost from $localIp"
                } else {
                    Write-Host "[lan] $arp"
                }
            }
            Write-Host "[wiiload-stage] attempt $attemptLabel"
            $safeRunId = $RunId -replace '[^A-Za-z0-9_.-]', '_'
            $wiiloadLogRoot = Join-Path $RepoRoot ".buildtmp\stage_wiiload"
            New-Item -ItemType Directory -Force -Path $wiiloadLogRoot | Out-Null
            $attemptOut = Join-Path $wiiloadLogRoot "${safeRunId}.attempt${attempt}.stdout.log"
            $attemptErr = Join-Path $wiiloadLogRoot "${safeRunId}.attempt${attempt}.stderr.log"
            $windowOptions = @{}
            if($env:OS -eq "Windows_NT") { $windowOptions.WindowStyle = "Hidden" }
            $p = Start-Process -FilePath $WiiloadExe -ArgumentList $serverArgs `
                -RedirectStandardOutput $attemptOut -RedirectStandardError $attemptErr `
                -Wait -PassThru @windowOptions
            $wiiloadExit = $p.ExitCode
            if(Test-Path $attemptOut) {
                Get-Content -LiteralPath $attemptOut | ForEach-Object {
                    if(-not [string]::IsNullOrWhiteSpace($_)) {
                        Write-Host $_
                    }
                }
            }
            if(Test-Path $attemptErr) {
                Get-Content -LiteralPath $attemptErr | ForEach-Object {
                    if(-not [string]::IsNullOrWhiteSpace($_)) {
                        Write-Host $_
                    }
                }
            }
            if($wiiloadExit -eq 0) {
                $launched = $true
                break
            }
            Write-Warning "[wiiload-stage] wiiload failed with exit code $wiiloadExit"
            if($WaitForHbcSeconds -gt 0 -or $attempt -lt $WiiloadAttempts) {
                Start-Sleep -Seconds $WiiloadRetrySeconds
            }
        }
    } finally {
        $env:WIILOAD = $oldWiiload
    }
    if(-not $launched) {
        throw "stage server wiiload failed"
    }
}
$client = $null
$deadline = (Get-Date).AddSeconds($StageConnectTimeoutSeconds)
$connectAttempt = 0
while($null -eq $client -and (Get-Date) -lt $deadline) {
    $connectAttempt++
    $client = Connect-TcpWithTimeout -RemoteHost $WiiHost -RemotePort $Port -TimeoutMs 1000
    if($null -eq $client) {
        if($connectAttempt -eq 1 -or ($connectAttempt % 5) -eq 0) {
            $arp = Update-ArpEntry -RemoteHost $WiiHost -SourceIp $localIp
            if([string]::IsNullOrWhiteSpace($arp)) {
                Write-Host "[stage-connect] no ARP reply for $WiiHost; waiting for stage server ${Port}"
            } else {
                Write-Host "[stage-connect] $arp; waiting for stage server ${Port}"
            }
        }
        Start-Sleep -Seconds 1
    }
}
if($null -eq $client -or -not $client.Connected) {
    throw "could not connect to stage server ${WiiHost}:$Port"
}

function Send-StageFile {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [byte[]]$Bytes,
        [string]$RemotePath,
        [switch]$ReplaceBeforeWrite
    )
    $expectedSha256 = Get-BytesSha256Hex $Bytes
    $proto = if($ReplaceBeforeWrite) { "CWSTAGE4R" } else { "CWSTAGE4" }
    $header = "{0} {1} {2} {3}`n" -f $proto, $Bytes.Length, $expectedSha256, $RemotePath
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
    $Stream.Write($headerBytes, 0, $headerBytes.Length)
    $offset = 0
    $doneLine = ""
    $chunk = [Math]::Max(1024, $ChunkSize)
    $progress = [pscustomobject]@{
        Acked = [int64]0
        Next = [int64]131072
    }
    function Drain-StageLines {
        param([switch]$WaitForLine)
        while($true) {
            if($WaitForLine) {
                # Blocks on the raw stream (no artificial timeout) until a
                # full line arrives or the server closes -- safe because a
                # single-byte NetworkStream.Read() never leaves a partial
                # line half-consumed.
                $line = Read-AsciiLine -Stream $Stream
            } else {
                $line = Read-AsciiLineIfAvailable -Stream $Stream
                if($null -eq $line) {
                    return ""
                }
            }
            if($line -match '"status":"FAIL"') {
                throw "stage failed: $line"
            }
            if($line -match '"status":"PASS"') {
                return $line
            }
            if($line -match '"bytes":([0-9]+)') {
                $progress.Acked = [int64]$Matches[1]
                if($progress.Acked -ge $progress.Next) {
                    Write-Host "[ack] $RemotePath $($progress.Acked) / $($Bytes.Length)"
                    while($progress.Acked -ge $progress.Next) {
                        $progress.Next += 131072
                    }
                }
            }
            if(-not $WaitForLine -and -not $Stream.DataAvailable) {
                return ""
            }
        }
    }

    while($offset -lt $Bytes.Length) {
        $n = [Math]::Min($chunk, $Bytes.Length - $offset)
        $Stream.Write($Bytes, $offset, $n)
        $Stream.Flush()
        $offset += $n
        if($ChunkDelayMs -gt 0) {
            Start-Sleep -Milliseconds $ChunkDelayMs
        }
        $line = Drain-StageLines
        if(-not [string]::IsNullOrWhiteSpace($line)) {
            $doneLine = $line
            break
        }
    }
    while([string]::IsNullOrWhiteSpace($doneLine)) {
        $line = Drain-StageLines -WaitForLine
        if(-not [string]::IsNullOrWhiteSpace($line)) {
            $doneLine = $line
            break
        }
    }
    return [pscustomobject]@{
        Line = $doneLine
        Sha256 = $expectedSha256
        Bytes = [int64]$Bytes.Length
        RemotePath = $RemotePath
    }
}

try {
    $stream = $client.GetStream()

    $resolvedToken = Get-ResolvedStageToken -Explicit $StageToken
    Invoke-StageAuth -Stream $stream -Token $resolvedToken

    $stageResult = Send-StageFile -Stream $stream -Bytes $bytes -RemotePath $TargetPath -ReplaceBeforeWrite
    Write-Host "[stage-result] $($stageResult.Line)"
    $null = Assert-StagePass -Line $stageResult.Line -RemotePath $stageResult.RemotePath -ExpectedBytes $stageResult.Bytes -ExpectedSha256 $stageResult.Sha256

    if(-not $NoRunArgs) {
        $argsResult = Send-StageFile -Stream $stream -Bytes $argBytes -RemotePath "sd:/apps/CavEX/cw_run_args.txt"
        Write-Host "[args-result] $($argsResult.Line)"
        $null = Assert-StagePass -Line $argsResult.Line -RemotePath $argsResult.RemotePath -ExpectedBytes $argsResult.Bytes -ExpectedSha256 $argsResult.Sha256

        $runPrepHeader = "CWRUNPREP1 $TargetPath sd:/apps/CavEX/cw_run_args.txt`n"
        $runPrepBytes = [System.Text.Encoding]::ASCII.GetBytes($runPrepHeader)
        $stream.Write($runPrepBytes, 0, $runPrepBytes.Length)
        $stream.Flush()
        $runPrepLine = Read-AsciiLine -Stream $stream
        if($null -eq $runPrepLine) {
            throw "stage server closed before runprep reply"
        }
        Write-Host "[runprep-result] $runPrepLine"
        $runPrep = Assert-RunPrepPass -Line $runPrepLine -ExpectedDolBytes $stageResult.Bytes -ExpectedDolSha256 $stageResult.Sha256 -ExpectedArgsBytes $argsResult.Bytes -ExpectedArgsSha256 $argsResult.Sha256
        Write-Host "[RUNPREP] entry=$($runPrep.entry_point) mem=$($runPrep.min_address)..$($runPrep.max_address) argv=$($runPrep.argv_count) argv_struct=$($runPrep.argv_struct_address) cmd=$($runPrep.command_line_address):$($runPrep.command_line_bytes)"
        if($LaunchAfterStage) {
            $runHeader = "CWRUN1 $TargetPath sd:/apps/CavEX/cw_run_args.txt`n"
            $runBytes = [System.Text.Encoding]::ASCII.GetBytes($runHeader)
            $stream.Write($runBytes, 0, $runBytes.Length)
            $stream.Flush()
            $runLine = Read-AsciiLine -Stream $stream
            if($null -eq $runLine) {
                throw "stage server closed before launch reply"
            }
            Write-Host "[launch-result] $runLine"
            $launch = Assert-RunPrepPass -Line $runLine -ExpectedDolBytes $stageResult.Bytes -ExpectedDolSha256 $stageResult.Sha256 -ExpectedArgsBytes $argsResult.Bytes -ExpectedArgsSha256 $argsResult.Sha256 -ExpectedReason "launching"
            Write-Host "[LAUNCH] entry=$($launch.entry_point) mem=$($launch.min_address)..$($launch.max_address) argv=$($launch.argv_count)"
        }
    }
} finally {
    $client.Close()
}

$md5 = (Get-FileHash -Algorithm MD5 $DolPath).Hash.ToLowerInvariant()
$sha256 = (Get-FileHash -Algorithm SHA256 $DolPath).Hash.ToLowerInvariant()
Write-Host "[STAGED] $TargetPath bytes=$($bytes.Length) md5=$md5 sha256=$sha256"
if(-not $NoRunArgs) {
    if($Profile -eq "none") {
        Write-Host "[RUN-ARGS] profile=none args=noop; CaveWii_v2 boots in plain-play mode (hardware harness disabled)."
        if($LaunchAfterStage) {
            Write-Host "[NEXT] Stage server should have jumped into CaveWii_v2 in plain-play mode."
        } else {
            Write-Host "[NEXT] Stage server remains listening; press HOME and launch CaveWii_v2 from HBC for plain play."
        }
    } else {
        Write-Host "[RUN-ARGS] run_id=$RunId profile=$Profile telemetry=tcp-pull:$TelemetryPort"
        $collect = "powershell -NoProfile -ExecutionPolicy Bypass -File tools\collect_staged_wii.ps1 -WiiHost $WiiHost -RunId $RunId -Profile $Profile -TelemetryPort $TelemetryPort"
        if($LaunchAfterStage) {
            Write-Host "[NEXT] Stage server should have jumped into CaveWii_v2; run:"
        } else {
            Write-Host "[NEXT] Stage server remains listening for more stages; press HOME when ready, launch CaveWii_v2 from HBC, then run:"
        }
        Write-Host "[NEXT] $collect"
    }
}
