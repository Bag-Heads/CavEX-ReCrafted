param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
    [string]$RemotePath = "sd:/hang_report.txt",
    [string]$OutFile = "",
    [int]$Port = 39019,
    [int]$ConnectTimeoutSeconds = 60,
    # Stage-server auth (CWSTAGE4, 2026-09) -- see docs/stage_server_auth_2026-09-02.md.
    # Resolution order: this param, $env:CW_STAGE_TOKEN, tools\.stage_token, empty.
    [string]$StageToken = ""
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
    Send-AsciiLine -Stream $Stream -Line ("CWAUTH1" + $(if([string]::IsNullOrWhiteSpace($Token)) { "" } else { " $Token" }))
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

if([string]::IsNullOrWhiteSpace($OutFile)) {
    $safe = $RemotePath.Replace("sd:/", "sd_").Replace("/", "_").Replace("\", "_").Replace(":", "")
    if([string]::IsNullOrWhiteSpace($safe)) {
        $safe = "stage_fetch.bin"
    }
    $outRoot = Join-Path $RepoRoot "tmp\wii_stage_fetch"
    New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    $OutFile = Join-Path $outRoot ("{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss"), $safe)
}

# 2026-09-02 (stage-server send fix): a CWFETCH1 body that ends SHORT is now
# a real, expected -- if rare -- outcome. The server's send loop refuses to
# retry a libogc/IOS net_send that failed after queuing an unknown prefix
# (tools/wii_stage_server/source/stage_send.h), because retrying is exactly
# what used to hand this client a full byte count of duplicated bytes. It
# aborts and closes instead, so the honest failure shape moved from "silent
# corruption" to "truncated stream". A GET is idempotent, so retry it whole
# -- but ONLY that shape: a sha256 mismatch or a byte-count mismatch is an
# integrity failure that must stay fatal on the first occurrence, never
# papered over by another attempt.
$TruncatedMarker = "stage server closed after"
$MaxFetchAttempts = 3

function Invoke-StageFetch {
    param(
        [string]$RemoteHost,
        [int]$RemotePort,
        [string]$Remote,
        [string]$Destination,
        [int]$ConnectSeconds,
        [string]$Token
    )

    $deadline = (Get-Date).AddSeconds($ConnectSeconds)
    $client = $null
    $attempt = 0
    while($null -eq $client -and (Get-Date) -lt $deadline) {
        $attempt++
        $remainingMs = [int][Math]::Max(1, ($deadline - (Get-Date)).TotalMilliseconds)
        $attemptTimeoutMs = [Math]::Min(1000, $remainingMs)
        $client = Connect-TcpWithTimeout -RemoteHost $RemoteHost -RemotePort $RemotePort -TimeoutMs $attemptTimeoutMs
        if($null -eq $client -or -not $client.Connected) {
            $client = $null
            if($attempt -eq 1 -or ($attempt % 10) -eq 0) {
                Write-Host "[stage-fetch] waiting for stage server ${RemoteHost}:$RemotePort"
            }
            Start-Sleep -Milliseconds 500
        }
    }
    if($null -eq $client -or -not $client.Connected) {
        throw "could not connect to stage server ${RemoteHost}:$RemotePort"
    }

    try {
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $stream.WriteTimeout = 5000

        Send-AsciiLine -Stream $stream -Line "CWHELLO1"
        $helloLine = Read-AsciiLine -Stream $stream
        $hello = Assert-PassJson -Line $helloLine -Operation "hello"
        Write-Host "[hello] $helloLine"

        Invoke-StageAuth -Stream $stream -Token $Token

        Send-AsciiLine -Stream $stream -Line "CWFETCH1 $Remote"
        $fetchLine = Read-AsciiLine -Stream $stream
        $fetch = Assert-PassJson -Line $fetchLine -Operation "fetch"
        if(-not ($fetch.PSObject.Properties.Name -contains "sha256")) {
            throw "fetch reply did not include sha256: $fetchLine"
        }
        $expectedBytes = [int64]$fetch.bytes
        $expectedSha = ([string]$fetch.sha256).ToLowerInvariant()
        Write-Host "[fetch] $fetchLine"

        $outDir = Split-Path -Parent $Destination
        if(-not [string]::IsNullOrWhiteSpace($outDir)) {
            New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        }
        $fs = [System.IO.File]::Open($Destination, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
        try {
            $buf = New-Object byte[] 8192
            $remaining = $expectedBytes
            while($remaining -gt 0) {
                $want = [Math]::Min($buf.Length, $remaining)
                $n = $stream.Read($buf, 0, [int]$want)
                if($n -le 0) {
                    throw "$TruncatedMarker $($expectedBytes - $remaining) / $expectedBytes bytes"
                }
                $fs.Write($buf, 0, $n)
                $remaining -= $n
            }
        } finally {
            $fs.Dispose()
        }

        $actualSha = (Get-FileHash -Algorithm SHA256 $Destination).Hash.ToLowerInvariant()
        $actualBytes = (Get-Item $Destination).Length
        if($actualBytes -ne $expectedBytes) {
            throw "fetch byte-count mismatch: expected $expectedBytes got $actualBytes"
        }
        if($actualSha -ne $expectedSha) {
            throw "fetch sha256 mismatch: expected $expectedSha got $actualSha"
        }
        Write-Host "[FETCHED] $Remote -> $Destination bytes=$actualBytes sha256=$actualSha protocol=$($hello.protocol)"
    } finally {
        $client.Close()
    }
}

$token = Get-ResolvedStageToken -Explicit $StageToken
for($fetchAttempt = 1; $fetchAttempt -le $MaxFetchAttempts; $fetchAttempt++) {
    try {
        Invoke-StageFetch -RemoteHost $WiiHost -RemotePort $Port -Remote $RemotePath `
            -Destination $OutFile -ConnectSeconds $ConnectTimeoutSeconds -Token $token
        break
    } catch {
        $message = "$($_.Exception.Message)"
        $isTruncated = $message.StartsWith($TruncatedMarker)
        if(-not $isTruncated -or $fetchAttempt -ge $MaxFetchAttempts) {
            throw
        }
        Write-Warning "[stage-fetch] truncated body on attempt $fetchAttempt/$MaxFetchAttempts ($message); retrying whole fetch"
        Start-Sleep -Milliseconds 500
    }
}
