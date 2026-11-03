param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [ValidateSet("heartbeat_selftest","far_idle_2m","far_walk_6m","perf_stress_5m","watchdog_inject","redstone_clock_2m","menus_dwell","save_migrate")]
    [string]$Profile = "heartbeat_selftest",
    [ValidateRange(8,32)]
    [int]$SaveMigrateRegions = 8,
    [string]$SdRoot = "",
    [int]$SdWaitSeconds = 900,
    [int]$StageWaitSeconds = 900,
    [int]$TelemetryWaitSeconds = 900,
    [int]$FirstHeartbeatTimeoutSeconds = 45,
    [int]$StaleTimeoutSeconds = 5,
    [int]$StageMailboxConnectTimeoutSeconds = 0,
    [int]$StagePort = 39019,
    [int]$TelemetryPort = 39017,
    [string]$StageNetmask = "",
    [string]$RunId = "",
    [switch]$SkipProvision,
    [switch]$AllowFixedDrive,
    [switch]$NoCollect,
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
    [int]$PowerCycleTimeoutSeconds = 10
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

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

function Get-DefaultStageNetmask {
    param([string]$RemoteHost)
    try {
        $localIp = Get-LocalIpForRemote $RemoteHost
        $addr = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -eq $localIp } |
            Select-Object -First 1
        if($addr) {
            $mask = Convert-PrefixToNetmask ([int]$addr.PrefixLength)
            Write-Host "[net] local $localIp/$($addr.PrefixLength) -> stage netmask $mask"
            return $mask
        }
    } catch {
    }
    return ""
}

function Find-RemovableSdRoot {
    $volumes = Get-Volume -ErrorAction SilentlyContinue |
        Where-Object {
            $_.DriveLetter -and
            $_.DriveType -eq "Removable" -and
            $_.FileSystem -match "^(FAT|FAT32|exFAT)$"
        } |
        Sort-Object DriveLetter
    if(@($volumes).Count -eq 1) {
        return "$($volumes[0].DriveLetter):\"
    }
    if(@($volumes).Count -gt 1) {
        $names = ($volumes | ForEach-Object { "$($_.DriveLetter): $($_.FileSystemLabel) $($_.FileSystem)" }) -join ", "
        throw "Multiple removable FAT/exFAT drives found; pass -SdRoot explicitly. Candidates: $names"
    }
    return ""
}

function Wait-ForSdRoot {
    param(
        [string]$RequestedRoot,
        [int]$TimeoutSeconds
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastPrint = [datetime]::MinValue
    while((Get-Date) -lt $deadline) {
        if(-not [string]::IsNullOrWhiteSpace($RequestedRoot) -and (Test-Path $RequestedRoot)) {
            return (Resolve-Path $RequestedRoot).Path
        }
        $detected = Find-RemovableSdRoot
        if(-not [string]::IsNullOrWhiteSpace($detected) -and (Test-Path $detected)) {
            return (Resolve-Path $detected).Path
        }
        if(((Get-Date) - $lastPrint).TotalSeconds -ge 10) {
            Write-Host "[wait-sd] insert the Wii SD card; waiting up to ${TimeoutSeconds}s"
            $lastPrint = Get-Date
        }
        Start-Sleep -Seconds 2
    }
    throw "timed out waiting for SD card"
}

if([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "wii_$($Profile)_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
}
if([string]::IsNullOrWhiteSpace($StageNetmask)) {
    $StageNetmask = Get-DefaultStageNetmask $WiiHost
}
if($StageMailboxConnectTimeoutSeconds -le 0) {
    $StageMailboxConnectTimeoutSeconds = $StageWaitSeconds
}

Write-Host "[wait-run] run_id=$RunId profile=$Profile"
if($LaunchAfterStage) {
    Write-Host "[wait-run] experimental CWRUN1 launch requested; this path is hardware-unconfirmed."
}

if(-not $SkipProvision) {
    $resolvedSd = Wait-ForSdRoot -RequestedRoot $SdRoot -TimeoutSeconds $SdWaitSeconds
    Write-Host "[provision] SD detected at $resolvedSd"

    # Close the "stale cw_run_args.txt" sentinel leak (2026-08-17): stage_wii_dol.ps1
    # writes sd:/apps/CavEX/cw_run_args.txt over the CWSTAGE3 network protocol for
    # EVERY staged run, and nothing in that network pipeline ever removes it -- main.c
    # reads it unconditionally on every boot (both sd-path conventions below), so a
    # card that finishes a hw-run session and then goes back to normal play (or a bare
    # HBC launch that skips staging entirely) silently re-arms whatever profile/
    # autoload the LAST staged run used -- the same failure shape as the pre-2026-07-30
    # junk-worlds 121/122/123 incident, for the staged-hardware pipeline instead of
    # config.json.
    #
    # This is the one point in the whole pipeline where the card is a plain MOUNTED
    # FILESYSTEM (a real drive letter, pre-network, pre-Wii-boot) rather than something
    # reachable only through the stage server's CWSTAGE3 TCP protocol -- which cannot do
    # this job anyway: it is unreachable the instant CaveWii_v2 takes over the Wii from
    # the stage server (true for both the default manual HOME+HBC-relaunch flow and the
    # experimental -LaunchAfterStage CWRUN1 chainload -- the Wii runs exactly one
    # resident app at a time), and its CWSTAGE3 header parse rejects a zero-byte payload
    # outright (`size == 0` in tools/wii_stage_server/source/main.c), so even a network
    # "truncate" would need a brand-new stage-server protocol verb -- Wii-resident C
    # that would need its own real-hardware proof round for a problem plain Remove-Item
    # solves for free right here. Runs unconditionally (no opt-out flag), mirroring
    # provision_wii_sd.ps1's existing "autoload" config.json discipline immediately
    # below: never armed unless THIS run explicitly asks for it.
    #
    # Known gap (documented, not silently swallowed): a card staged via bare
    # run_staged_wii_hardware.ps1 calls that skip this provisioning step (the normal
    # "round 2, 3, ..." loop in TEST_PLAN.txt) is not touched between those rounds --
    # each round's own stage_wii_dol.ps1 write already overwrites the previous round's
    # args with that round's own fresh, intended content, so nothing leaks there. What
    # this closes is the LAST staged run's args surviving into normal play; that is
    # exactly cleared the next time the card comes back through here.
    foreach($cwRunArgsRel in @("cw_run_args.txt", "apps\CavEX\cw_run_args.txt")) {
        $cwRunArgsPath = Join-Path $resolvedSd $cwRunArgsRel
        if(Test-Path -LiteralPath $cwRunArgsPath) {
            Remove-Item -LiteralPath $cwRunArgsPath -Force
            Write-Host "[provision] removed stale $cwRunArgsPath"
        }
    }

    $provisionArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\provision_wii_sd.ps1"),
        "-SdRoot", $resolvedSd
    )
    if(-not [string]::IsNullOrWhiteSpace($StageNetmask)) {
        $provisionArgs += @("-StageNetmask", $StageNetmask)
    }
    if($AllowFixedDrive) {
        $provisionArgs += "-AllowFixedDrive"
    }
    & powershell @provisionArgs
    if($LASTEXITCODE -ne 0) {
        throw "provision_wii_sd.ps1 failed with exit code $LASTEXITCODE"
    }
    Write-Host "[action] eject the SD, put it in the Wii, and launch 'DOL Stage Server' from HBC."
} else {
    Write-Host "[wait-run] provisioning skipped; launch 'DOL Stage Server' from HBC."
}

Write-Host "[wait-run] waiting for stage server by starting the staged hardware runner."
Write-Host "[wait-run] the runner will connect when ${WiiHost}:$StagePort becomes available."

$runArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $RepoRoot "tools\run_staged_wii_hardware.ps1"),
    "-WiiHost", $WiiHost,
    "-Profile", $Profile,
    "-SaveMigrateRegions", "$SaveMigrateRegions",
    "-RunId", $RunId,
    "-StagePort", "$StagePort",
    "-TelemetryPort", "$TelemetryPort",
    "-StageConnectTimeoutSeconds", "$StageWaitSeconds",
    "-TelemetryConnectTimeoutSeconds", "$TelemetryWaitSeconds",
    "-FirstHeartbeatTimeoutSeconds", "$FirstHeartbeatTimeoutSeconds",
    "-StaleTimeoutSeconds", "$StaleTimeoutSeconds",
    "-StageMailboxConnectTimeoutSeconds", "$StageMailboxConnectTimeoutSeconds"
)
if($NoCollect) {
    $runArgs += "-NoCollect"
}
if($LaunchAfterStage) {
    $runArgs += "-LaunchAfterStage"
}
if($SkipStageMailboxFetch) {
    $runArgs += "-SkipStageMailboxFetch"
}
if($AllowDirty) {
    $runArgs += "-AllowDirty"
}
if($PowerCycleOnFailure) {
    $runArgs += "-PowerCycleOnFailure"
}
if(-not [string]::IsNullOrWhiteSpace($PowerCycleUrl)) {
    $runArgs += @("-PowerCycleUrl", $PowerCycleUrl)
}
if(-not [string]::IsNullOrWhiteSpace($PowerOffUrl)) {
    $runArgs += @("-PowerOffUrl", $PowerOffUrl)
}
if(-not [string]::IsNullOrWhiteSpace($PowerOnUrl)) {
    $runArgs += @("-PowerOnUrl", $PowerOnUrl)
}
if(-not [string]::IsNullOrWhiteSpace($PowerCycleCommand)) {
    $runArgs += @("-PowerCycleCommand", $PowerCycleCommand)
}
$runArgs += @(
    "-PowerCycleMethod", $PowerCycleMethod,
    "-PowerCycleDelaySeconds", "$PowerCycleDelaySeconds",
    "-PowerCycleTimeoutSeconds", "$PowerCycleTimeoutSeconds"
)

& powershell @runArgs
exit $LASTEXITCODE
