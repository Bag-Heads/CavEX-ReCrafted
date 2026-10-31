<#
    delete_wii_stage_path.ps1 -- CWDELETE1 client for the CaveWii_v2 stage
    server (tools/wii_stage_server). Removes a file, an EMPTY directory, or a
    directory tree from the Wii SD card.

    -Mode file   one file (refuses a directory: not_a_file)
    -Mode dir    an empty directory (refuses a non-empty one: dir_not_empty)
    -Mode tree   a directory and everything under it

    Scope is decided by the SERVER (stage_delete.h), never by this script:
    only the PUT allow-list (sd:/apps/..., sd:/texturepacks/...,
    sd:/assets/..., the exact root telemetry files), never an app dir itself,
    never a whole top-level app subdir recursively (one world at a time, not
    saves/), never an app's boot.dol, never sd:/apps/CaveWiiStage/, never the
    stage token. A refused path comes back FAIL bad_target and this exits 7.
    A path that is already gone is PASS "absent" (exit 0), so cleanup can be
    re-run. Needs a server advertising "delete" (exit 6 otherwise) and, for a
    path with a space, "spaced-paths".

    Takes the rig lock like the saves tools (-LockHeldBy when the caller
    already holds it).

    Examples:
      # the stray FILE the old space-truncating put left behind
      tools\delete_wii_stage_path.ps1 -WiiHost 192.0.2.72 -Mode file `
        -RemotePath "sd:/apps/CavEX/saves/CaveWii"
      tools\delete_wii_stage_path.ps1 -WiiHost 192.0.2.72 -Mode tree `
        -RemotePath "sd:/apps/CavEX/saves/CaveWii Tutorial"
#>
[CmdletBinding()]
param(
    [string]$WiiHost = "",
    [int]$Port = 39019,
    [ValidateSet('file','dir','tree')]
    [string]$Mode = 'file',
    [string]$RemotePath = "",
    [string]$StageToken = "",
    [int]$ConnectTimeoutSeconds = 60,
    [string]$LockPath = "C:/tmp/cw-rig-lock",
    [string]$LockHeldBy = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cw_saves_common.ps1')

if([string]::IsNullOrWhiteSpace($WiiHost) -or [string]::IsNullOrWhiteSpace($RemotePath)) {
    Write-CwFail "-WiiHost and -RemotePath are required"
    exit $script:CwSavesExit.Refused
}
$problem = Get-CwStagePathProblem -Path $RemotePath
if($null -ne $problem) {
    Write-CwFail "refusing -RemotePath '$RemotePath': $problem"
    exit $script:CwSavesExit.Refused
}

$lock = $null
$client = $null
try {
    $lock = Enter-CwRigLock -LockPath $LockPath -LockHeldBy $LockHeldBy `
        -Owner ("delete_wii_stage_path.ps1 / $env:USERNAME / " + (Get-Date -Format 'o'))
} catch {
    Write-CwFail $_.Exception.Message
    exit $script:CwSavesExit.Refused
}

try {
    $client = Connect-CwStage -RemoteHost $WiiHost -RemotePort $Port -TimeoutSeconds $ConnectTimeoutSeconds
    $stream = $client.GetStream()
    $stream.ReadTimeout = 30000
    $stream.WriteTimeout = 30000
    try {
        $hs = Invoke-CwHandshake -Stream $stream -RequireCapabilities @('delete') `
            -Token (Get-CwStageToken -Explicit $StageToken -RepoRoot $RepoRoot)
        Assert-CwSpacedPathSupported -Capabilities $hs.Capabilities -Path $RemotePath
    } catch {
        if($_.Exception.Message -match 'CW_SAVES_SERVER_TOO_OLD') {
            Write-CwFail $_.Exception.Message
            exit $script:CwSavesExit.ServerTooOld
        }
        throw
    }
    Write-Host "[hello] $($hs.HelloLine)"

    $r = Invoke-CwStageDelete -Stream $stream -Mode $Mode -RemotePath $RemotePath
    Write-Host "[delete-result] $($r.Line)"
    if($r.Reply.status -ne 'PASS') {
        Write-CwFail "delete $Mode '$RemotePath' refused: reason=$($r.Reply.reason) removed=$($r.Reply.bytes)"
        exit $script:CwSavesExit.ServerFail
    }
    Write-Host "[DELETE] $Mode $RemotePath -> $($r.Reply.reason) removed=$($r.Reply.bytes)"
} catch {
    Write-CwFail $_.Exception.Message
    exit $script:CwSavesExit.ServerFail
} finally {
    if($null -ne $client) { $client.Close() }
    if($null -ne $lock) { Exit-CwRigLock -Lock $lock }
}
exit $script:CwSavesExit.Ok
