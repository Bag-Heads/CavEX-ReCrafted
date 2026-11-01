param(
    [string]$PowerCycleUrl = "",
    [string]$PowerOffUrl = "",
    [string]$PowerOnUrl = "",
    [string]$PowerCycleCommand = "",
    [ValidateSet("GET","POST")]
    [string]$Method = "GET",
    [int]$DelaySeconds = 5,
    [int]$TimeoutSeconds = 10,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

function Test-HasText {
    param([string]$Value)
    return -not [string]::IsNullOrWhiteSpace($Value)
}

function Assert-PowerCycleMode {
    param(
        [string]$CycleUrl,
        [string]$OffUrl,
        [string]$OnUrl,
        [string]$Command
    )
    $modes = 0
    if(Test-HasText $CycleUrl) { $modes++ }
    if(Test-HasText $Command) { $modes++ }
    if((Test-HasText $OffUrl) -or (Test-HasText $OnUrl)) {
        if(-not (Test-HasText $OffUrl) -or -not (Test-HasText $OnUrl)) {
            throw "PowerOffUrl and PowerOnUrl must be supplied together"
        }
        $modes++
    }
    if($modes -ne 1) {
        throw "configure exactly one power-cycle mode: PowerCycleUrl, PowerOffUrl+PowerOnUrl, or PowerCycleCommand"
    }
    if($DelaySeconds -lt 0) {
        throw "DelaySeconds must be >= 0"
    }
    if($TimeoutSeconds -le 0) {
        throw "TimeoutSeconds must be > 0"
    }
}

function Invoke-PowerUrl {
    param(
        [string]$Url,
        [string]$Label
    )
    Write-Host "[power] $Label $Method $Url"
    $reply = Invoke-WebRequest -UseBasicParsing -Uri $Url -Method $Method -TimeoutSec $TimeoutSeconds
    if($reply.StatusCode -lt 200 -or $reply.StatusCode -ge 300) {
        throw "$Label returned HTTP $($reply.StatusCode)"
    }
}

function Invoke-PowerCommand {
    param([string]$Command)
    Write-Host "[power] command"
    & (Get-Command powershell, pwsh -ErrorAction SilentlyContinue | Select-Object -First 1) -NoProfile -ExecutionPolicy Bypass -Command $Command
    if($LASTEXITCODE -ne 0) {
        throw "PowerCycleCommand failed with exit code $LASTEXITCODE"
    }
}

function Invoke-ConfiguredPowerCycle {
    param(
        [string]$CycleUrl,
        [string]$OffUrl,
        [string]$OnUrl,
        [string]$Command
    )
    Assert-PowerCycleMode -CycleUrl $CycleUrl -OffUrl $OffUrl -OnUrl $OnUrl -Command $Command
    if(Test-HasText $Command) {
        Invoke-PowerCommand -Command $Command
        Write-Host "[POWER-CYCLE] mode=command"
        return
    }
    if(Test-HasText $CycleUrl) {
        Invoke-PowerUrl -Url $CycleUrl -Label "cycle"
        Write-Host "[POWER-CYCLE] mode=url"
        return
    }
    Invoke-PowerUrl -Url $OffUrl -Label "off"
    if($DelaySeconds -gt 0) {
        Start-Sleep -Seconds $DelaySeconds
    }
    Invoke-PowerUrl -Url $OnUrl -Label "on"
    Write-Host "[POWER-CYCLE] mode=off-on delay_seconds=$DelaySeconds"
}

function Invoke-SelfTest {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("cavewii_power_cycle_selftest_{0}" -f ([System.Guid]::NewGuid().ToString("N")))
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    try {
        $marker = Join-Path $root "cycled.txt"
        $quotedMarker = "'" + ($marker -replace "'", "''") + "'"
        Invoke-ConfiguredPowerCycle -Command "Set-Content -LiteralPath $quotedMarker -Value cycled" -CycleUrl "" -OffUrl "" -OnUrl ""
        if(-not (Test-Path $marker)) {
            throw "selftest command mode did not create marker"
        }
        $failed = $false
        try {
            Invoke-ConfiguredPowerCycle -CycleUrl "http://127.0.0.1/cycle" -OffUrl "http://127.0.0.1/off" -OnUrl "http://127.0.0.1/on" -Command ""
        } catch {
            $failed = $_.Exception.Message -match "exactly one"
        }
        if(-not $failed) {
            throw "selftest did not reject multiple configured modes"
        }
        $failed = $false
        try {
            Invoke-ConfiguredPowerCycle -CycleUrl "" -OffUrl "http://127.0.0.1/off" -OnUrl "" -Command ""
        } catch {
            $failed = $_.Exception.Message -match "must be supplied together"
        }
        if(-not $failed) {
            throw "selftest did not reject incomplete off/on URLs"
        }
        Write-Host "PASS invoke_wii_power_cycle selftest"
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if($SelfTest) {
    Invoke-SelfTest
    exit 0
}

Invoke-ConfiguredPowerCycle -CycleUrl $PowerCycleUrl -OffUrl $PowerOffUrl -OnUrl $PowerOnUrl -Command $PowerCycleCommand
