param(
    [string]$PassPath = "",
    [string]$ExpectedRunId = "",
    [ValidateSet("heartbeat_selftest","far_idle_2m","far_walk_6m","perf_stress_5m","watchdog_inject","redstone_clock_2m","menus_dwell","save_migrate","mp_connect_smoke","mp_lan_walk_10m","mp_reconnect_soak","keepup_walk_6m","split_anchor_6m","split_play_2m","split_play_8m","split_actions_6m","split_recovery_6m","menus_dwell_creative","mp_chat_login_3m","mp_chat_chord_1m","mp_play_10m","mp_pause_menu_3m","split_portal_quit_4m","split_dims_6m","mob_motion_2m","host_vehicle_smoke_8m","pool_selftest_3m","split_regrow_8m","split_offer_piston_2m","world_copy_convert","save_reload_3m","")]
    [string]$ExpectedProfile = "",
    [string]$ExpectedBuild = "",
    [Alias("cw-vehicle-kind")]
    [ValidateSet("", "boat", "pig", "cart")]
    [string]$VehicleKind = "",
    [string]$VehicleManifest = "",
    [string[]]$AggregatePassPaths = @(),
    [string]$Python = "python",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

function Get-BuildStampHash {
    # Comparable identity of a CW_BUILD_ID: the git-hash part (incl. the '+'
    # dirty marker) before the '_MM.dd' suffix. The date is baked at BUILD time
    # while the expected stamp is recomputed at VALIDATE time, so full equality
    # spuriously failed any build that crossed midnight before its run
    # (mirrors build_stamp_hash in tools/wii_telemetry_server.py).
    param([string]$Stamp)
    if([string]::IsNullOrEmpty($Stamp)) { return "" }
    $i = $Stamp.LastIndexOf("_")
    if($i -gt 0) { return $Stamp.Substring(0, $i) }
    return $Stamp
}

function Test-BuildStampsMatch {
    # Hash-only comparison, EXCEPT degraded stamps: 'nogit'/empty hashes carry
    # their only distinguishing bit in the date suffix, so they fall back to
    # full equality (mirrors build_stamps_match in wii_telemetry_server.py).
    param([string]$Actual, [string]$Expected)
    $a = Get-BuildStampHash $Actual
    $e = Get-BuildStampHash $Expected
    if($a -in @("", "nogit") -or $e -in @("", "nogit")) {
        return ([string]$Actual) -eq ([string]$Expected)
    }
    return $a -eq $e
}

function Get-MinHeartbeatsForProfile {
    param([string]$Profile)
    switch($Profile) {
        "heartbeat_selftest" { return 50 }
        "far_idle_2m" { return 100 }
        "far_walk_6m" { return 300 }
        "perf_stress_5m" { return 240 }
        "watchdog_inject" { return 5 }
        "redstone_clock_2m" { return 100 }
        "mob_motion_2m" { return 100 }
        "pool_selftest_3m" { return 30 }
        # Matches wii_telemetry_server.py PROFILES["save_reload_3m"]["min_hb"].
        "save_reload_3m" { return 30 }
        "split_regrow_8m" { return 300 }
        # Matches wii_telemetry_server.py PROFILES["split_offer_piston_2m"]["min_hb"].
        "split_offer_piston_2m" { return 10 }
        "host_vehicle_smoke_8m" { return 10 }
        "save_migrate" { return 5 }
        # Matches wii_telemetry_server.py PROFILES["world_copy_convert"]["min_hb"].
        "world_copy_convert" { return 5 }
        # Matches wii_telemetry_server.py's PROFILES["mp_connect_smoke"]["min_hb"]
        # exactly (audit finding 5: this case was missing, so an mp_connect_smoke
        # PASS with as few as 1 heartbeat validated locally even though the
        # collector's real bar is 60 -- an offline-validator loophole a PASS
        # could slip through without ever meeting the collector's own gate).
        "mp_connect_smoke" { return 60 }
        # Matches wii_telemetry_server.py's PROFILES["mp_lan_walk_10m"]["min_hb"]:
        # a 600s window minus the ~25-30s measured connect/DHCP/login/chunk-
        # stream settle (same overhead mp_connect_smoke absorbs) leaves ~570s
        # of walk-under-load; 480 keeps a comfortable margin below that.
        "mp_lan_walk_10m" { return 480 }
        # Matches wii_telemetry_server.py's PROFILES["mp_reconnect_soak"]["min_hb"]:
        # heartbeats emit ~1/s regardless of MP connect state, so a 480s
        # window leaves ~80s of margin for the first cycle's DHCP/connect
        # settle -- see that file's profile comment for the full accounting.
        "mp_reconnect_soak" { return 400 }
        # mp_play_10m (push-0921/B5): DRIVER-OWNED completion, like
        # mp_chat_login_3m -- the run ends on its last assertion, not on the
        # 1200 s backstop, so the collector's binding floor is a CADENCE over
        # the run's own measured play (MpPlayEvidence.heartbeat_floor). Unlike
        # mp_chat_login_3m, whose run can honestly be seconds long and which
        # therefore has NO case here at all (it falls to the permissive default
        # of 1), this profile's measured play has a contractual 600 000 ms
        # FLOOR: 0.8 beats/s across it is 480, and a run shorter than that
        # cannot be a PASS in the first place. So the number is stated rather
        # than defaulted -- it can never punish an honest fast run, and it
        # closes the offline-validator loophole audit finding 5 found on
        # mp_connect_smoke. Matches wii_telemetry_server.py's
        # PROFILES["mp_play_10m"]["min_hb"].
        "mp_play_10m" { return 480 }
        "mp_pause_menu_3m" { return 5 }
        "split_portal_quit_4m" { return 20 }
        "split_dims_6m" { return 300 }
        default { return 1 }
    }
}

function Assert-WiiPassArtifact {
    param(
        [Parameter(Mandatory=$true)]
        [string]$Path,
        [string]$RunId = "",
        [string]$Profile = "",
        [string]$Build = ""
    )
    if(-not (Test-Path -LiteralPath $Path)) {
        throw "PASS artifact does not exist: $Path"
    }
    try {
        $pass = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    } catch {
        throw "PASS artifact is not valid JSON: $Path"
    }
    # Reject PC diagnostic provenance before any status/profile relabeling can
    # select a different validator path. The filename is never provenance.
    if($pass.vehicle_pc_replay -or ([string]$pass.status).StartsWith("PC_REPLAY") -or
       $pass.evidence_provenance -eq "pc_diagnostic_replay" -or
       ([string]$pass.vehicle_certification).Contains("pc_replay") -or
       ([string]$pass.certification_scope).Contains("pc_replay") -or
       $pass.noncertifying -eq $true -or $pass.source_tape -eq "source-tape.ndjson" -or
       $null -ne $pass.PSObject.Properties["source_tape_sha256"]) {
        throw "PC replay provenance/status is noncertifying regardless of artifact filename."
    }
    if($pass.status -ne "PASS") {
        throw "PASS artifact status mismatch: expected PASS got '$($pass.status)'"
    }
    if(-not [string]::IsNullOrWhiteSpace($RunId) -and $pass.run_id -ne $RunId) {
        throw "PASS artifact run_id mismatch: expected $RunId got '$($pass.run_id)'"
    }
    if(-not [string]::IsNullOrWhiteSpace($Profile) -and $pass.profile -ne $Profile) {
        throw "PASS artifact profile mismatch: expected $Profile got '$($pass.profile)'"
    }
    if($pass.profile -eq "host_vehicle_smoke_8m") {
        if([string]::IsNullOrWhiteSpace($VehicleKind) -or [string]::IsNullOrWhiteSpace($VehicleManifest)) {
            throw "vehicle PASS requires -VehicleKind and -VehicleManifest."
        }
        if($null -eq $pass.vehicle_smoke -or $pass.vehicle_smoke.kind -ne $VehicleKind -or
           $pass.vehicle_smoke.complete -ne $true -or $pass.vehicle_smoke.profile_done_count -ne 1) {
            throw "vehicle PASS missing complete per-kind evidence summary."
        }
        # Recompute all predicates from verbatim wire rows, including late/extra
        # completions; a copied summary or run_ok cannot authorize a PASS.
        & $Python (Join-Path $PSScriptRoot "wii_telemetry_server.py") `
            --vehicle-validate-pass $Path --vehicle-manifest $VehicleManifest `
            --cw-vehicle-kind $VehicleKind --expected-build $Build
        if($LASTEXITCODE -ne 0) { throw "vehicle PASS raw replay/manifest validation failed." }
    } elseif(-not [string]::IsNullOrWhiteSpace($VehicleKind)) {
        throw "-VehicleKind is allowed only for host_vehicle_smoke_8m."
    }
    $minHeartbeats = Get-MinHeartbeatsForProfile ([string]$pass.profile)
    try {
        $heartbeats = [int]$pass.heartbeats
    } catch {
        throw "PASS artifact heartbeats is not an integer"
    }
    if($heartbeats -lt $minHeartbeats) {
        throw "PASS artifact has too few heartbeats for $($pass.profile): $heartbeats < $minHeartbeats"
    }
    if($null -eq $pass.last_heartbeat) {
        throw "PASS artifact missing last_heartbeat provenance"
    }
    if($pass.last_heartbeat.event -ne "heartbeat") {
        throw "PASS artifact last_heartbeat.event mismatch: expected heartbeat got '$($pass.last_heartbeat.event)'"
    }
    if(-not [string]::IsNullOrWhiteSpace($RunId) -and $pass.last_heartbeat.run_id -ne $RunId) {
        throw "PASS artifact last_heartbeat.run_id mismatch: expected $RunId got '$($pass.last_heartbeat.run_id)'"
    }
    if($pass.last_heartbeat.profile -ne $pass.profile) {
        throw "PASS artifact last_heartbeat.profile mismatch: expected $($pass.profile) got '$($pass.last_heartbeat.profile)'"
    }
    if(-not [string]::IsNullOrWhiteSpace($Build) -and
       -not (Test-BuildStampsMatch ([string]$pass.last_heartbeat.build_id) $Build)) {
        throw "PASS artifact last_heartbeat.build_id mismatch: expected $Build got '$($pass.last_heartbeat.build_id)'"
    }
    if([string]::IsNullOrWhiteSpace([string]$pass.last_heartbeat.build_id)) {
        throw "PASS artifact last_heartbeat.build_id is empty"
    }
    if($pass.profile -eq "save_migrate") {
        $hb = $pass.last_heartbeat
        foreach($field in @("smig_state", "smig_ms", "smig_regions",
                            "smig_chunks", "smig_signs", "smig_expected",
                            "smig_verified", "smig_chests",
                            "smig_chests_expected", "smig_chests_verified",
                            "smig_spawners", "smig_spawners_expected",
                            "smig_spawners_verified")) {
            if($null -eq $hb.$field) {
                throw "save_migrate PASS missing last_heartbeat.$field"
            }
        }
        if([int]$hb.smig_state -ne 4) {
            throw "save_migrate did not reach verified state 4: $($hb.smig_state)"
        }
        $migrationMs = [int64]$hb.smig_ms
        # The uncalibrated first-run hard gate is the profile's declared 300s
        # window. Do not relax it after a slow/failed run: exceeding it is the
        # I/O-cliff finding this profile exists to expose. Covers the FULL
        # v2+v3+v4 chain now (source/network/server_local.c's shared
        # save_migrate_chain_start), not just the v2 sign stage.
        if($migrationMs -le 0 -or $migrationMs -gt 300000) {
            throw "save_migrate elapsed time outside (0,300000] ms: $migrationMs"
        }
        $expected = [int]$hb.smig_expected
        if($expected -lt 32 -or $expected % 4 -ne 0) {
            throw "save_migrate expected-sign count invalid/default weakened: $expected"
        }
        $regions = $expected / 4
        if([int]$hb.smig_regions -lt $regions) {
            throw "save_migrate scanned too few regions: $($hb.smig_regions) < $regions"
        }
        if([int]$hb.smig_chunks -lt $regions) {
            throw "save_migrate scanned too few chunks: $($hb.smig_chunks) < $regions"
        }
        if([int]$hb.smig_signs -ne $expected -or
           [int]$hb.smig_verified -ne $expected) {
            throw "save_migrate sign evidence mismatch: rewritten=$($hb.smig_signs) verified=$($hb.smig_verified) expected=$expected"
        }
        # v3 (iron chest) and v4 (spawner TE) chain links: one fixture each
        # per region, so their expected count is the region count, not the
        # sign count. A world that reached smig_state 4 without these two
        # sub-migrations genuinely completing would otherwise pass on v2
        # evidence alone -- exactly the gap this profile was widened to close.
        $chestExpected = [int]$hb.smig_chests_expected
        if($chestExpected -ne $regions) {
            throw "save_migrate chest-expected count does not match region count: $chestExpected != $regions"
        }
        if([int]$hb.smig_chests -ne $chestExpected -or
           [int]$hb.smig_chests_verified -ne $chestExpected) {
            throw "save_migrate chest evidence mismatch: rewritten=$($hb.smig_chests) verified=$($hb.smig_chests_verified) expected=$chestExpected"
        }
        $spawnerExpected = [int]$hb.smig_spawners_expected
        if($spawnerExpected -ne $regions) {
            throw "save_migrate spawner-expected count does not match region count: $spawnerExpected != $regions"
        }
        if([int]$hb.smig_spawners -ne $spawnerExpected -or
           [int]$hb.smig_spawners_verified -ne $spawnerExpected) {
            throw "save_migrate spawner evidence mismatch: synthesized=$($hb.smig_spawners) verified=$($hb.smig_spawners_verified) expected=$spawnerExpected"
        }
        if($hb.world_loaded -ne $true) {
            throw "save_migrate reopened world is not loaded/playable"
        }
    }
    if($pass.profile -eq "mp_reconnect_soak") {
        # Re-derived from the WIRE last_heartbeat fields (mrs_cycles/
        # mrs_target/mrs_wedge), same discipline as the save_migrate block
        # above: an offline validator must not trust the collector's own
        # rollup, it must recompute the verdict from the raw evidence.
        $hb = $pass.last_heartbeat
        foreach($field in @("mrs_cycles", "mrs_target", "mrs_wedge")) {
            if($null -eq $hb.$field) {
                throw "mp_reconnect_soak PASS missing last_heartbeat.$field"
            }
        }
        if($hb.mrs_wedge -ne $false) {
            throw "mp_reconnect_soak wedged (last_heartbeat.mrs_wedge=true) -- no wedge is a hard requirement"
        }
        $target = [int]$hb.mrs_target
        if($target -lt 1) {
            throw "mp_reconnect_soak last_heartbeat.mrs_target invalid: $target"
        }
        if([int]$hb.mrs_cycles -lt $target) {
            throw "mp_reconnect_soak completed too few cycles: $($hb.mrs_cycles) < $target"
        }
    }
    return [pscustomobject]@{
        run_id = [string]$pass.run_id
        profile = [string]$pass.profile
        build_id = [string]$pass.last_heartbeat.build_id
        heartbeats = $heartbeats
    }
}

function Write-TestPass {
    param(
        [string]$Path,
        [string]$RunId = "run42",
        [string]$Profile = "heartbeat_selftest",
        [string]$Build = "abc_06.29",
        [int]$Heartbeats = 50,
        [switch]$NoLastHeartbeat,
        [switch]$SaveMigrateEvidence,
        [switch]$MpReconnectSoakEvidence,
        [int]$MrsCycles = 6,
        [int]$MrsTarget = 6,
        [bool]$MrsWedge = $false
    )
    $obj = [ordered]@{
        status = "PASS"
        reason = "profile_done received"
        run_id = $RunId
        profile = $Profile
        heartbeats = $Heartbeats
        latest = [ordered]@{
            event = "profile_done"
            run_id = $RunId
            profile = $Profile
        }
    }
    if(-not $NoLastHeartbeat) {
        $obj.last_heartbeat = [ordered]@{
            event = "heartbeat"
            run_id = $RunId
            profile = $Profile
            build_id = $Build
            tick = $Heartbeats
        }
        if($SaveMigrateEvidence) {
            $obj.last_heartbeat.world_loaded = $true
            $obj.last_heartbeat.smig_state = 4
            $obj.last_heartbeat.smig_ms = 47000
            $obj.last_heartbeat.smig_regions = 12
            $obj.last_heartbeat.smig_chunks = 40
            $obj.last_heartbeat.smig_signs = 32
            $obj.last_heartbeat.smig_expected = 32
            $obj.last_heartbeat.smig_verified = 32
            $obj.last_heartbeat.smig_chests = 8
            $obj.last_heartbeat.smig_chests_expected = 8
            $obj.last_heartbeat.smig_chests_verified = 8
            $obj.last_heartbeat.smig_spawners = 8
            $obj.last_heartbeat.smig_spawners_expected = 8
            $obj.last_heartbeat.smig_spawners_verified = 8
        }
        if($MpReconnectSoakEvidence) {
            $obj.last_heartbeat.mrs_cycles = $MrsCycles
            $obj.last_heartbeat.mrs_target = $MrsTarget
            $obj.last_heartbeat.mrs_wedge = $MrsWedge
        }
    }
    $obj | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Invoke-SelfTest {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("validate_wii_pass_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    try {
        $good = Join-Path $root "good.json"
        Write-TestPass -Path $good
        $ok = Assert-WiiPassArtifact -Path $good -RunId "run42" -Profile "heartbeat_selftest" -Build "abc_06.29"
        if($ok.heartbeats -ne 50 -or $ok.build_id -ne "abc_06.29") {
            throw "selftest accepted good PASS but returned wrong summary"
        }

        $missing = Join-Path $root "missing_last_heartbeat.json"
        Write-TestPass -Path $missing -NoLastHeartbeat
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $missing -RunId "run42" -Profile "heartbeat_selftest" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "missing last_heartbeat"
        }
        if(-not $failed) {
            throw "selftest failed to reject PASS without last_heartbeat"
        }

        $wrongBuild = Join-Path $root "wrong_build.json"
        Write-TestPass -Path $wrongBuild -Build "old_06.28"
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $wrongBuild -RunId "run42" -Profile "heartbeat_selftest" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "build_id mismatch"
        }
        if(-not $failed) {
            throw "selftest failed to reject PASS with wrong build"
        }

        # Cross-midnight build (same hash, different date suffix) must validate.
        $midnight = Join-Path $root "midnight.json"
        Write-TestPass -Path $midnight -Build "abc_06.28"
        $ok = Assert-WiiPassArtifact -Path $midnight -RunId "run42" -Profile "heartbeat_selftest" -Build "abc_06.29"
        if($ok.build_id -ne "abc_06.28") {
            throw "selftest rejected same-hash cross-midnight build stamp"
        }

        # Degraded 'nogit' stamps compare by full string: a stale nogit .dol
        # from another day must NOT validate.
        $nogit = Join-Path $root "nogit.json"
        Write-TestPass -Path $nogit -Build "nogit_06.20"
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $nogit -RunId "run42" -Profile "heartbeat_selftest" -Build "nogit_06.29"
        } catch {
            $failed = $_.Exception.Message -match "build_id mismatch"
        }
        if(-not $failed) {
            throw "selftest accepted a cross-day nogit build stamp"
        }

        $short = Join-Path $root "short.json"
        Write-TestPass -Path $short -Profile "far_idle_2m" -Heartbeats 99
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $short -RunId "run42" -Profile "far_idle_2m" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "too few heartbeats"
        }
        if(-not $failed) {
            throw "selftest failed to reject too-short far_idle_2m PASS"
        }

        # Audit finding 5: mp_connect_smoke previously had no
        # Get-MinHeartbeatsForProfile case at all and fell through to the
        # default of 1 -- a PASS with as few as 1 heartbeat would have
        # validated locally even though the collector's real bar
        # (wii_telemetry_server.py PROFILES["mp_connect_smoke"]["min_hb"]) is
        # 60. Below-bar must reject, at/above-bar must accept.
        $mpSmokeShort = Join-Path $root "mp_connect_smoke_short.json"
        Write-TestPass -Path $mpSmokeShort -Profile "mp_connect_smoke" -Heartbeats 59
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $mpSmokeShort -RunId "run42" -Profile "mp_connect_smoke" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "too few heartbeats"
        }
        if(-not $failed) {
            throw "selftest failed to reject too-short mp_connect_smoke PASS (loophole regression)"
        }

        $mpSmokeOk = Join-Path $root "mp_connect_smoke_ok.json"
        Write-TestPass -Path $mpSmokeOk -Profile "mp_connect_smoke" -Heartbeats 60
        $ok = Assert-WiiPassArtifact -Path $mpSmokeOk -RunId "run42" -Profile "mp_connect_smoke" -Build "abc_06.29"
        if($ok.heartbeats -ne 60) {
            throw "selftest rejected an at-bar mp_connect_smoke PASS"
        }

        $saveMigrate = Join-Path $root "save_migrate.json"
        Write-TestPass -Path $saveMigrate -Profile "save_migrate" `
            -Heartbeats 5 -SaveMigrateEvidence
        $ok = Assert-WiiPassArtifact -Path $saveMigrate -RunId "run42" `
            -Profile "save_migrate" -Build "abc_06.29"
        if($ok.heartbeats -ne 5) {
            throw "selftest rejected valid save_migrate evidence"
        }

        $saveMigrateBad = Join-Path $root "save_migrate_bad.json"
        Write-TestPass -Path $saveMigrateBad -Profile "save_migrate" `
            -Heartbeats 5 -SaveMigrateEvidence
        $bad = Get-Content -Raw -LiteralPath $saveMigrateBad | ConvertFrom-Json
        $bad.last_heartbeat.smig_verified = 31
        $bad | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $saveMigrateBad -Encoding UTF8
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $saveMigrateBad -RunId "run42" `
                -Profile "save_migrate" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "sign evidence mismatch"
        }
        if(-not $failed) {
            throw "selftest accepted incomplete save_migrate wall verification"
        }

        # v3 (iron chest) chain link: same shape as the sign check above, but
        # this is the coverage gap the chain-widening closed -- a PASS that
        # only proved v2 (signs) would have said nothing about v3/v4 ever
        # running for real.
        $saveMigrateBadChest = Join-Path $root "save_migrate_bad_chest.json"
        Write-TestPass -Path $saveMigrateBadChest -Profile "save_migrate" `
            -Heartbeats 5 -SaveMigrateEvidence
        $badChest = Get-Content -Raw -LiteralPath $saveMigrateBadChest | ConvertFrom-Json
        $badChest.last_heartbeat.smig_chests_verified = 7
        $badChest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $saveMigrateBadChest -Encoding UTF8
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $saveMigrateBadChest -RunId "run42" `
                -Profile "save_migrate" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "chest evidence mismatch"
        }
        if(-not $failed) {
            throw "selftest accepted incomplete save_migrate chest verification"
        }

        # v4 (spawner TE) chain link.
        $saveMigrateBadSpawner = Join-Path $root "save_migrate_bad_spawner.json"
        Write-TestPass -Path $saveMigrateBadSpawner -Profile "save_migrate" `
            -Heartbeats 5 -SaveMigrateEvidence
        $badSpawner = Get-Content -Raw -LiteralPath $saveMigrateBadSpawner | ConvertFrom-Json
        $badSpawner.last_heartbeat.smig_spawners_verified = 7
        $badSpawner | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $saveMigrateBadSpawner -Encoding UTF8
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $saveMigrateBadSpawner -RunId "run42" `
                -Profile "save_migrate" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "spawner evidence mismatch"
        }
        if(-not $failed) {
            throw "selftest accepted incomplete save_migrate spawner verification"
        }

        # mp_reconnect_soak: full target cycle count, no wedge -> accept.
        $mrsOk = Join-Path $root "mp_reconnect_soak_ok.json"
        Write-TestPass -Path $mrsOk -Profile "mp_reconnect_soak" `
            -Heartbeats 400 -MpReconnectSoakEvidence -MrsCycles 6 -MrsTarget 6 -MrsWedge $false
        $ok = Assert-WiiPassArtifact -Path $mrsOk -RunId "run42" `
            -Profile "mp_reconnect_soak" -Build "abc_06.29"
        if($ok.heartbeats -ne 400) {
            throw "selftest rejected valid mp_reconnect_soak evidence"
        }

        # Too few cycles must reject, even with mrs_wedge false.
        $mrsShort = Join-Path $root "mp_reconnect_soak_short.json"
        Write-TestPass -Path $mrsShort -Profile "mp_reconnect_soak" `
            -Heartbeats 400 -MpReconnectSoakEvidence -MrsCycles 5 -MrsTarget 6 -MrsWedge $false
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $mrsShort -RunId "run42" `
                -Profile "mp_reconnect_soak" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "too few cycles"
        }
        if(-not $failed) {
            throw "selftest accepted an mp_reconnect_soak PASS with too few cycles"
        }

        # A wedge must reject even with the full target cycle count reached
        # (the C-side state machine is sticky: cycles_completed can only
        # equal the target on the SAME tick phase becomes DONE, which never
        # coexists with a true wedge -- but the offline validator must not
        # rely on that internal invariant holding, so it checks explicitly).
        $mrsWedged = Join-Path $root "mp_reconnect_soak_wedged.json"
        Write-TestPass -Path $mrsWedged -Profile "mp_reconnect_soak" `
            -Heartbeats 400 -MpReconnectSoakEvidence -MrsCycles 6 -MrsTarget 6 -MrsWedge $true
        $failed = $false
        try {
            $null = Assert-WiiPassArtifact -Path $mrsWedged -RunId "run42" `
                -Profile "mp_reconnect_soak" -Build "abc_06.29"
        } catch {
            $failed = $_.Exception.Message -match "wedged"
        }
        if(-not $failed) {
            throw "selftest accepted a wedged mp_reconnect_soak PASS"
        }
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "PASS validate_wii_pass selftest"
}

if($SelfTest) {
    Invoke-SelfTest
    exit 0
}

if($AggregatePassPaths.Count -gt 0) {
    if($AggregatePassPaths.Count -ne 3 -or [string]::IsNullOrWhiteSpace($VehicleManifest) -or
       [string]::IsNullOrWhiteSpace($ExpectedBuild) -or -not [string]::IsNullOrWhiteSpace($VehicleKind)) {
        throw "aggregate requires three PASS paths, one -VehicleManifest, -ExpectedBuild, and no per-run -VehicleKind."
    }
    $packetArgs = @((Join-Path $PSScriptRoot "wii_telemetry_server.py"), "--vehicle-validate-pass") +
        $AggregatePassPaths + @("--vehicle-aggregate", "--vehicle-manifest", $VehicleManifest,
                               "--expected-build", $ExpectedBuild)
    & $Python @packetArgs
    if($LASTEXITCODE -ne 0) { throw "vehicle aggregate replay/coverage failed." }
    Write-Host "[PASS-VALIDATED] vehicle boat/pig/cart aggregate (one manifest, six directed switches)"
    exit 0
}

if([string]::IsNullOrWhiteSpace($PassPath)) {
    throw "PassPath is required unless -SelfTest is used"
}
$summary = Assert-WiiPassArtifact -Path $PassPath -RunId $ExpectedRunId -Profile $ExpectedProfile -Build $ExpectedBuild
Write-Host "[PASS-VALIDATED] $PassPath run_id=$($summary.run_id) profile=$($summary.profile) build=$($summary.build_id) heartbeats=$($summary.heartbeats)"
