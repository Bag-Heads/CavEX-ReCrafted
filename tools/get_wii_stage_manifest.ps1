param(
    [Parameter(Mandatory=$true)]
    [string]$WiiHost,
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
    $outRoot = Join-Path $RepoRoot "tmp\wii_stage_manifest"
    New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    $OutFile = Join-Path $outRoot ("stage_manifest_{0}.json" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
}

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
            Write-Host "[stage-manifest] waiting for stage server ${WiiHost}:$Port"
        }
        Start-Sleep -Milliseconds 500
    }
}
if($null -eq $client -or -not $client.Connected) {
    throw "could not connect to stage server ${WiiHost}:$Port"
}

try {
    $stream = $client.GetStream()
    $stream.ReadTimeout = 5000
    $stream.WriteTimeout = 5000

    Send-AsciiLine -Stream $stream -Line "CWHELLO1"
    $helloLine = Read-AsciiLine -Stream $stream
    $hello = Assert-PassJson -Line $helloLine -Operation "hello"
    Write-Host "[hello] $helloLine"

    Invoke-StageAuth -Stream $stream -Token (Get-ResolvedStageToken -Explicit $StageToken)

    Send-AsciiLine -Stream $stream -Line "CWMANIFEST1"
    $manifestLine = Read-AsciiLine -Stream $stream
    $manifest = Assert-PassJson -Line $manifestLine -Operation "manifest"
    if(-not ($manifest.PSObject.Properties.Name -contains "files")) {
        throw "manifest reply did not include files: $manifestLine"
    }

    foreach($file in @($manifest.files)) {
        if(-not ($file.PSObject.Properties.Name -contains "path") -or
           -not ($file.PSObject.Properties.Name -contains "exists") -or
           -not ($file.PSObject.Properties.Name -contains "bytes")) {
            throw "manifest file entry missing required fields: $($file | ConvertTo-Json -Compress)"
        }
        if($file.exists -and -not ($file.PSObject.Properties.Name -contains "sha256")) {
            throw "manifest existing file missing sha256: $($file.path)"
        }
    }

    $outDir = Split-Path -Parent $OutFile
    if(-not [string]::IsNullOrWhiteSpace($outDir)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }
    $manifest | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $OutFile -Encoding utf8
    $existing = @($manifest.files | Where-Object { $_.exists }).Count
    $missing = @($manifest.files | Where-Object { -not $_.exists }).Count
    Write-Host "[MANIFEST] $OutFile files=$(@($manifest.files).Count) existing=$existing missing=$missing protocol=$($hello.protocol)"
} finally {
    $client.Close()
}
