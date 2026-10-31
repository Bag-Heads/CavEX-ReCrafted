<#
    put_wii_stage_file.ps1 -- generic CWSTAGE4 file PUT for the CaveWii_v2
    stage server (tools/wii_stage_server), with local/remote sha256
    verification.

    Promoted from a scratchpad one-off (put_kz.ps1, used to land
    sd:/assets/art/kz.png) into a supported tools/ CLI. There was previously
    no generic "put a file onto the Wii SD card" script -- stage_wii_dol.ps1
    is boot.dol-specific (it also drives build/wiiload/runprep/telemetry),
    so anything else (texture-pack assets, manifest files) had to be staged
    by hand-editing a copy of it or writing a fresh one-off.

    Mirrors tools/fetch_wii_stage_file.ps1's conventions: every protocol
    line is read byte-at-a-time directly off the raw NetworkStream (see the
    Read-AsciiLine comment below -- NEVER wrap it in a StreamReader), the
    server's reply is checked against the LOCAL file's own byte-count/sha256
    (not just "status":"PASS"), and any mismatch/failure exits non-zero.
    -Verbose prints the per-chunk CHUNK ack progress via Write-Verbose.

    tools/stage_wii_dol.ps1's Send-StageFile function speaks the identical
    CWSTAGE4/CWSTAGE4R wire format and chunk-ack loop; this script is a
    small, standalone sibling rather than a dot-sourced import of it,
    following the pattern already established between fetch_wii_stage_file.
    ps1 and collect_staged_wii.ps1 -- neither of which dot-sources a shared
    helper file either. Each stage-server client script in tools/ instead
    carries its own copy of the small Connect-TcpWithTimeout / Read-AsciiLine
    / Get-BytesSha256Hex / Get-ResolvedStageToken / Invoke-StageAuth
    primitives; stage_wii_dol.ps1 is >1500 lines with a mandatory -WiiHost
    param and side-effecting top-level logic (builds the stage server,
    resolves ARP, etc.), so dot-sourcing it here would run all of that just
    to reach one function -- inlining the same small primitives this file's
    siblings already inline is the established, safer pattern.

    Server-side allow-list (tools/wii_stage_server/source/stage_auth.h's
    valid_sd_path()): sd:/apps/, sd:/texturepacks/, sd:/assets/, and the
    exact root paths in stage_manifest_paths.h -- anything else (including
    sd:/apps/CavEX/stage_token.txt itself) is refused server-side with
    "bad_target". See docs/stage_server_auth_2026-09-02.md for the full
    CWHELLO1/CWAUTH1/CWSTAGE4 handshake and auth soft-launch behaviour.

    Example:
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\put_wii_stage_file.ps1 `
        -WiiHost 192.0.2.72 -LocalPath assets\art\kz.png -RemotePath sd:/assets/art/kz.png -Verbose
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [int]$Port = 39019,
    [Parameter(Mandatory=$true)]
    [string]$LocalPath,
    [Parameter(Mandatory=$true)]
    [string]$RemotePath,
    [int]$ConnectTimeoutSeconds = 60,
    [int]$ChunkSize = 8192,
    [int]$ChunkDelayMs = 2,
    # Stage-server auth (CWSTAGE4, 2026-09) -- see docs/stage_server_auth_2026-09-02.md.
    # Resolution order: this param, $env:CW_STAGE_TOKEN, tools\.stage_token, empty.
    [string]$StageToken = "",
    # CWSTAGE4 fails if the target already exists; CWSTAGE4R replaces it.
    # Defaults to replace, matching Send-StageFile's -ReplaceBeforeWrite
    # default use in stage_wii_dol.ps1 and put_kz.ps1's prior behaviour --
    # re-staging an asset over itself is the common case for this CLI.
    [bool]$Replace = $true
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

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

function Send-AsciiLine {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Line
    )
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Line + "`n")
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
}

# Byte-at-a-time reads directly off the raw NetworkStream -- NEVER wrap this
# stream in a System.IO.StreamReader. A StreamReader reads ahead into its
# own internal buffer to find line terminators; any byte it buffers but
# doesn't hand back is invisible to (and lost from) a later direct
# $Stream.Read() on raw payload/ack bytes sharing the same connection. This
# exact mistake, made by an earlier copy of stage_wii_dol.ps1's
# Drain-StageLines poll loop (StreamReader + tight ReadTimeout polling),
# reproduced real byte-for-byte SD-card corruption on real Wii hardware
# (2026-09-02) -- the file passed the server's own tmp/rename/sha256
# verification, but a later CWFETCH1 GET of the same path came back with the
# file's own header reinserted mid-stream. See tools/stage_wii_dol.ps1's
# Read-AsciiLine comment for the full writeup; this function is the same
# fix, byte-at-a-time, and is why every stage-server client script in
# tools/ implements its own copy of it instead of pooling I/O through any
# buffered .NET reader type.
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

function Assert-PassJson {
    param(
        [string]$Line,
        [string]$Operation
    )
    try {
        $reply = $Line | ConvertFrom-Json
    } catch {
        throw "$Operation returned invalid JSON: $Line"
    }
    if($reply.status -ne "PASS") {
        throw "$Operation failed: $Line"
    }
    return $reply
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

function Invoke-StageAuth {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Token
    )
    $line = if([string]::IsNullOrWhiteSpace($Token)) { "CWAUTH1" } else { "CWAUTH1 $Token" }
    Send-AsciiLine -Stream $Stream -Line $line
    $reply = Read-AsciiLine -Stream $Stream
    $json = Assert-PassJson -Line $reply -Operation "auth"
    if($json.reason -eq "no_token_provisioned") {
        Write-Host "[stage-auth] server has no token provisioned (soft-launch) -- run:"
        Write-Host "  powershell -NoProfile -ExecutionPolicy Bypass -File tools\provision_wii_sd.ps1 -SdRoot <drive>"
        Write-Host "  to close this window before relying on auth."
    } else {
        Write-Host "[stage-auth] $reply"
    }
}

# Verifies the server's own PASS reply against what WE meant to send --
# byte count and sha256 both, not just "status":"PASS". A server that
# accepted a truncated/corrupted body but still said PASS (or a reply that
# silently omits sha256) must be treated as a failure here, the same way
# tools/stage_wii_dol.ps1's Assert-StagePass and tools/fetch_wii_stage_file.
# ps1's post-download hash check both refuse to trust "PASS" alone.
function Assert-PutPass {
    param(
        [string]$Line,
        [string]$RemotePath,
        [int64]$ExpectedBytes,
        [string]$ExpectedSha256
    )
    try {
        $reply = $Line | ConvertFrom-Json
    } catch {
        throw "put returned invalid JSON for ${RemotePath}: $Line"
    }
    if($reply.status -ne "PASS") {
        throw "put failed for ${RemotePath}: $Line"
    }
    if([int64]$reply.bytes -ne $ExpectedBytes) {
        throw "put byte-count mismatch for ${RemotePath}: expected $ExpectedBytes got $($reply.bytes)"
    }
    if(-not ($reply.PSObject.Properties.Name -contains "sha256")) {
        throw "stage server did not return SD-side sha256 for ${RemotePath}; rebuild/relaunch the stage server"
    }
    $actualSha256 = ([string]$reply.sha256).ToLowerInvariant()
    if($actualSha256 -ne $ExpectedSha256) {
        throw "put sha256 mismatch for ${RemotePath}: expected $ExpectedSha256 got $actualSha256"
    }
    return $reply
}

function Send-StagePutFile {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [byte[]]$Bytes,
        [string]$RemotePath,
        [bool]$ReplaceExisting,
        [int]$Chunk,
        [int]$DelayMs
    )
    $expectedSha256 = Get-BytesSha256Hex $Bytes
    $proto = if($ReplaceExisting) { "CWSTAGE4R" } else { "CWSTAGE4" }
    $header = "{0} {1} {2} {3}`n" -f $proto, $Bytes.Length, $expectedSha256, $RemotePath
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
    $Stream.Write($headerBytes, 0, $headerBytes.Length)
    $Stream.Flush()
    Write-Verbose "[put] $($proto.TrimEnd()) $RemotePath bytes=$($Bytes.Length) sha256=$expectedSha256"

    $offset = 0
    $doneLine = ""
    $chunkSize = [Math]::Max(1024, $Chunk)

    function Drain-PutLines {
        param([switch]$WaitForLine)
        while($true) {
            if($WaitForLine) {
                # Blocks on the raw stream (no artificial timeout) until a
                # full line arrives or the server closes -- safe because a
                # single-byte NetworkStream.Read() never leaves a partial
                # line half-consumed (see Read-AsciiLine's comment above).
                $line = Read-AsciiLine -Stream $Stream
            } else {
                $line = Read-AsciiLineIfAvailable -Stream $Stream
                if($null -eq $line) {
                    return ""
                }
            }
            if($line -match '"status":"FAIL"') {
                throw "stage server rejected put: $line"
            }
            if($line -match '"status":"PASS"') {
                return $line
            }
            if($line -match '"status":"CHUNK".*"bytes":([0-9]+)') {
                Write-Verbose "[ack] $RemotePath $($Matches[1]) / $($Bytes.Length)"
            }
            if(-not $WaitForLine -and -not $Stream.DataAvailable) {
                return ""
            }
        }
    }

    while($offset -lt $Bytes.Length) {
        $n = [Math]::Min($chunkSize, $Bytes.Length - $offset)
        $Stream.Write($Bytes, $offset, $n)
        $Stream.Flush()
        $offset += $n
        if($DelayMs -gt 0) {
            Start-Sleep -Milliseconds $DelayMs
        }
        $line = Drain-PutLines
        if(-not [string]::IsNullOrWhiteSpace($line)) {
            $doneLine = $line
            break
        }
    }
    while([string]::IsNullOrWhiteSpace($doneLine)) {
        $line = Drain-PutLines -WaitForLine
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

# spaced-paths (2026-10-30): the path is sent as the rest of the header
# line, so it may contain spaces -- but only to a server advertising
# "spaced-paths" (checked after CWHELLO1 below); an older server cuts it at
# the first space and writes a stray file (the ".../saves/CaveWii" incident).
# Same rules as cw_saves_common.ps1's Get-CwStagePathProblem.
function Get-StagePathProblem {
    param([string]$Path)
    if(-not $Path.StartsWith("sd:/")) { return "path must start with sd:/" }
    foreach($ch in $Path.ToCharArray()) {
        if([int]$ch -lt 0x20 -or [int]$ch -gt 0x7e) { return ("byte 0x{0:x2} is not printable ASCII" -f [int]$ch) }
    }
    if($Path.Contains('"')) { return "a double quote cannot be sent in a path" }
    if($Path.Contains('..')) { return "'..' is refused" }
    if($Path.Substring(4).Contains('//')) { return "empty path segment '//'" }
    foreach($seg in $Path.Substring(4).Split('/')) {
        if($seg.Length -gt 0 -and ($seg.StartsWith(' ') -or $seg.EndsWith(' '))) {
            return "segment '$seg' starts or ends with a space"
        }
    }
    if($Path.Length -gt 255) { return "path longer than 255 bytes" }
    return $null
}
$pathProblem = Get-StagePathProblem $RemotePath
if($null -ne $pathProblem) {
    throw "refusing -RemotePath '$RemotePath': $pathProblem"
}

if(-not (Test-Path -LiteralPath $LocalPath)) {
    throw "local file not found: $LocalPath"
}
$resolvedLocalPath = (Resolve-Path -LiteralPath $LocalPath).Path
$bytes = [System.IO.File]::ReadAllBytes($resolvedLocalPath)
$localSha256 = Get-BytesSha256Hex $bytes
Write-Host "[local] $resolvedLocalPath bytes=$($bytes.Length) sha256=$localSha256"

$token = Get-ResolvedStageToken -Explicit $StageToken

$deadline = (Get-Date).AddSeconds($ConnectTimeoutSeconds)
$client = $null
$attempt = 0
while($null -eq $client -and (Get-Date) -lt $deadline) {
    $attempt++
    $remainingMs = [int][Math]::Max(1, ($deadline - (Get-Date)).TotalMilliseconds)
    $attemptTimeoutMs = [Math]::Min(1000, $remainingMs)
    $client = Connect-TcpWithTimeout -RemoteHost $WiiHost -RemotePort $Port -TimeoutMs $attemptTimeoutMs
    if($null -eq $client -or -not $client.Connected) {
        $client = $null
        if($attempt -eq 1 -or ($attempt % 10) -eq 0) {
            Write-Host "[stage-put] waiting for stage server ${WiiHost}:$Port"
        }
        Start-Sleep -Milliseconds 500
    }
}
if($null -eq $client -or -not $client.Connected) {
    throw "could not connect to stage server ${WiiHost}:${Port}"
}

try {
    $stream = $client.GetStream()
    $stream.ReadTimeout = 30000
    $stream.WriteTimeout = 30000

    Send-AsciiLine -Stream $stream -Line "CWHELLO1"
    $helloLine = Read-AsciiLine -Stream $stream
    $hello = Assert-PassJson -Line $helloLine -Operation "hello"
    Write-Verbose "[hello] $helloLine"
    if($RemotePath.Contains(' ')) {
        $caps = @()
        if($hello.PSObject.Properties.Name -contains 'capabilities') { $caps = @($hello.capabilities) }
        if(-not ($caps -contains 'spaced-paths')) {
            throw "CW_STAGE_SERVER_TOO_OLD: '$RemotePath' has a space but the stage server does not advertise 'spaced-paths' (it would write a stray file cut at the first space) -- push a stage server built from this tree first"
        }
    }

    Invoke-StageAuth -Stream $stream -Token $token

    $putResult = Send-StagePutFile -Stream $stream -Bytes $bytes -RemotePath $RemotePath `
        -ReplaceExisting $Replace -Chunk $ChunkSize -DelayMs $ChunkDelayMs
    Write-Host "[put-result] $($putResult.Line)"
    $reply = Assert-PutPass -Line $putResult.Line -RemotePath $putResult.RemotePath `
        -ExpectedBytes $putResult.Bytes -ExpectedSha256 $putResult.Sha256

    Write-Host "[PUT] $resolvedLocalPath -> $RemotePath bytes=$($putResult.Bytes) sha256=$($putResult.Sha256) protocol=$($hello.protocol)"
} finally {
    $client.Close()
}

exit 0
