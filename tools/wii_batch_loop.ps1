# Hands-free Wii hardware batch loop with VERIFIED console orientation.
#
# Grew out of the 2026-08-18 scratchpad loop that ran the first 7/7 hardware
# board, hardened after two 2026-08-19 field failures:
#   * PHANTOM PORTS: a bare TCP connect to 39019/39017 can succeed once even
#     when no live service is behind it (the "phantom single-accept").
#     Orientation therefore never trusts a bare connect: the stage server must
#     answer CWHELLO1, the game telemetry port must push bytes, and HBC must
#     accept TWICE in a row.
#   * WRONG-SCREEN STALL: the old loop assumed the console sits at HBC. This
#     one orients first (stage server / game / HBC / dark) and acts from any
#     of them.
#
# Typical use (from the worktree/repo the DOL was built in):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\wii_batch_loop.ps1 `
#     -WiiHost 192.0.2.249 -Profiles heartbeat_selftest -ExpectedBuild <stamp>
#
# watchdog_inject wedges the console BY DESIGN: run it last; the loop stops
# after it on purpose.
param(
    [Parameter(Mandatory=$true)][string]$WiiHost,
    [Parameter(Mandatory=$true)][string[]]$Profiles,
    [Parameter(Mandatory=$true)][string]$ExpectedBuild,
    [double]$ExitGraceSeconds = 45,
    [int]$OrientTimeoutSeconds = 14400,
    [int]$AutoExitWaitSeconds = 720,
    [string]$StageDol = "",
    [string]$WiiloadExe = "C:\msys64\opt\devkitpro\tools\bin\wiiload.exe",
    [int]$HbcPort = 4299,
    [int]$StagePort = 39019,
    [int]$TelemetryPort = 39017
)
$ErrorActionPreference = "Continue"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if(-not $StageDol) {
    $StageDol = Join-Path $PSScriptRoot "wii_stage_server\wii_stage_server.dol"
}
$Runner = Join-Path $PSScriptRoot "run_staged_wii_hardware.ps1"

function Write-Log { param([string]$Msg)
    Write-Output ("[loop] ({0}) {1}" -f (Get-Date).ToString('HH:mm:ss'), $Msg)
}

function Connect-Tcp { param([int]$Port,[int]$TimeoutMs = 3000)
    $c = New-Object Net.Sockets.TcpClient
    try {
        $r = $c.BeginConnect($WiiHost, $Port, $null, $null)
        if($r.AsyncWaitHandle.WaitOne($TimeoutMs) -and $c.Connected) { return $c }
    } catch {}
    $c.Close()
    return $null
}

# HBC's wiiload listener sends no banner, so a handshake probe is impossible;
# two successful connects 2s apart defeat the single-accept phantom instead.
function Test-HbcListening {
    $a = Connect-Tcp -Port $HbcPort
    if($null -eq $a) { return $false }
    $a.Close()
    Start-Sleep -Seconds 2
    $b = Connect-Tcp -Port $HbcPort
    if($null -eq $b) { return $false }
    $b.Close()
    return $true
}

# VERIFIED stage-server probe: a live server answers CWHELLO1 with a JSON
# hello line; a phantom accept answers nothing.
function Test-StageServer {
    $c = Connect-Tcp -Port $StagePort
    if($null -eq $c) { return $false }
    $ok = $false
    try {
        $s = $c.GetStream(); $s.ReadTimeout = 5000
        $hello = [Text.Encoding]::ASCII.GetBytes("CWHELLO1`n")
        $s.Write($hello, 0, $hello.Length)
        $buf = New-Object byte[] 512
        $n = $s.Read($buf, 0, $buf.Length)
        if($n -gt 0) {
            $resp = [Text.Encoding]::ASCII.GetString($buf, 0, $n)
            $ok = $resp.Contains('"reason":"hello"')
        }
    } catch {}
    $c.Close()
    return $ok
}

# VERIFIED game probe: the game's telemetry port pushes its pre-connect buffer
# immediately on accept; a phantom stays silent. NOTE: this consumes the
# game's single client slot -- only use while ORIENTING, never while a
# collector is attached.
function Test-GameTelemetry {
    $c = Connect-Tcp -Port $TelemetryPort
    if($null -eq $c) { return $false }
    $ok = $false
    try {
        $s = $c.GetStream(); $s.ReadTimeout = 5000
        $buf = New-Object byte[] 512
        $n = $s.Read($buf, 0, $buf.Length)
        $ok = ($n -gt 0)
    } catch {}
    $c.Close()
    return $ok
}

function Get-ConsoleState {
    if(Test-StageServer)    { return "stage" }
    if(Test-GameTelemetry)  { return "game" }
    if(Test-HbcListening)   { return "hbc" }
    return "dark"
}

function Send-StageServer {
    $env:WIILOAD = "tcp:$WiiHost"
    for($try = 1; $try -le 5; $try++) {
        & $WiiloadExe $StageDol
        if($LASTEXITCODE -eq 0) { return $true }
        Write-Log "wiiload attempt $try failed (exit $LASTEXITCODE); retrying in 8s"
        Start-Sleep -Seconds 8
    }
    return $false
}

function Wait-ForState { param([string[]]$Wanted,[int]$TimeoutSec)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while((Get-Date) -lt $deadline) {
        $s = Get-ConsoleState
        if($Wanted -contains $s) { return $s }
        Start-Sleep -Seconds 6
    }
    return "timeout"
}

$anyFail = $false
for($i = 0; $i -lt $Profiles.Count; $i++) {
    $p = $Profiles[$i]
    Write-Log "=== profile $($i+1)/$($Profiles.Count): $p ==="

    # Orient: reach a state we can launch from.
    $deadline = (Get-Date).AddSeconds($OrientTimeoutSeconds)
    $ready = $false
    while((Get-Date) -lt $deadline) {
        $state = Get-ConsoleState
        Write-Log "console state: $state"
        if($state -eq "stage") { $ready = $true; break }
        if($state -eq "hbc") {
            Write-Log "console at HBC; sending stage server"
            if(-not (Send-StageServer)) {
                Write-Log "STALLED: wiiload from HBC failed -- MANUAL RECOVERY NEEDED"
                exit 3
            }
            $s = Wait-ForState -Wanted @("stage") -TimeoutSec 300
            if($s -eq "stage") { $ready = $true; break }
            Write-Log "stage server never verified after push; re-orienting"
            continue
        }
        if($state -eq "game") {
            Write-Log "a game is already running; waiting up to $AutoExitWaitSeconds s for its auto-exit"
            $s = Wait-ForState -Wanted @("hbc","stage") -TimeoutSec $AutoExitWaitSeconds
            if($s -eq "timeout") {
                Write-Log "STALLED: running game never returned to HBC -- MANUAL RECOVERY NEEDED (power cycle)"
                exit 4
            }
            continue
        }
        Start-Sleep -Seconds 10
    }
    if(-not $ready) {
        Write-Log "STALLED: console never reached a launchable state in $OrientTimeoutSeconds s"
        exit 2
    }

    Start-Sleep -Seconds 4
    Write-Log "staging + launching $p (expected build $ExpectedBuild)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Runner `
        -WiiHost $WiiHost -Profile $p -LaunchAfterStage `
        -FirstHeartbeatTimeoutSeconds 150 -TelemetryConnectTimeoutSeconds 300 -SkipStageMailboxFetch `
        -ExitGraceSeconds $ExitGraceSeconds -ExpectedBuild $ExpectedBuild
    Write-Log "VERDICT profile=$p exit=$LASTEXITCODE"
    if($LASTEXITCODE -ne 0) { $anyFail = $true }

    if($p -eq "watchdog_inject") {
        Write-Log "watchdog_inject leaves the console wedged BY DESIGN -- stopping here, power off at will"
        break
    }

    if($i -lt ($Profiles.Count - 1)) {
        Write-Log "waiting for auto-exit back to HBC before the next profile"
        $s = Wait-ForState -Wanted @("hbc") -TimeoutSec $AutoExitWaitSeconds
        if($s -eq "timeout") {
            Write-Log "STALLED: game did not self-exit after $p -- MANUAL RECOVERY NEEDED"
            exit 4
        }
    }
}
Write-Log "LOOP FINISHED"
if($anyFail) { exit 1 } else { exit 0 }
