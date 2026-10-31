# Effective settings of main.c's guarded perf_stress service diagnostic.
# Keep strings in card config: --cw-set supports integers only.
function Get-ServiceDiagnosticSettings {
    return [ordered]@{
        service_diagnostic_int = 1
        service_observer_int = 1
        music_continuous_int = 1
        perf_stress_hold_health_int = 1
        music_force = "on"
        autopilot = "off"
        autopilot_place = "off"
    }
}

function Assert-ServiceDiagnosticConfig {
    param($Config)
    foreach($entry in (Get-ServiceDiagnosticSettings).GetEnumerator()) {
        $property = $Config.debug.PSObject.Properties[$entry.Key]
        # ConvertFrom-Json may represent integers as Int64 in PowerShell 7.
        $matchesValue = $null -ne $property -and $(if($entry.Value -is [int]) {
            ($property.Value -is [int] -or $property.Value -is [long]) -and $property.Value -eq $entry.Value
        } else {
            $property.Value -is [string] -and $property.Value -ceq $entry.Value
        })
        if(-not $matchesValue) {
            throw "service diagnostic config read-back mismatch: debug.$($entry.Key)"
        }
    }
}

function Set-StagedServiceDiagnosticConfig {
    param([string]$WiiHost, [int]$Port, [int]$ConnectTimeoutSeconds, [string]$StageToken = "")
    $tools = $PSScriptRoot
    $local = [IO.Path]::GetTempFileName()
    $readback = [IO.Path]::GetTempFileName()
    try {
        # CWD is loader-dependent; provision_wii_sd also maintains both copies.
        foreach($remote in @("sd:/config.json", "sd:/apps/CavEX/config.json")) {
            $common = @("-WiiHost", $WiiHost,
                "-Port", "$Port", "-ConnectTimeoutSeconds", "$ConnectTimeoutSeconds")
            if($StageToken) { $common += @("-StageToken", $StageToken) }
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools "fetch_wii_stage_file.ps1") @common -RemotePath $remote -OutFile $local
            if($LASTEXITCODE -ne 0) { throw "service diagnostic config fetch failed: $remote" }
            $config = Get-Content -Raw -LiteralPath $local | ConvertFrom-Json
            if($null -eq $config.debug) {
                $config | Add-Member -Force NoteProperty debug ([pscustomobject]@{})
            }
            foreach($entry in (Get-ServiceDiagnosticSettings).GetEnumerator()) {
                $config.debug | Add-Member -Force NoteProperty $entry.Key $entry.Value
            }
            [IO.File]::WriteAllText($local, ($config | ConvertTo-Json -Depth 100), [Text.UTF8Encoding]::new($false))
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools "put_wii_stage_file.ps1") @common -RemotePath $remote -LocalPath $local
            if($LASTEXITCODE -ne 0) { throw "service diagnostic config PUT failed: $remote" }
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools "fetch_wii_stage_file.ps1") @common -RemotePath $remote -OutFile $readback
            if($LASTEXITCODE -ne 0) { throw "service diagnostic config read-back failed: $remote" }
            Assert-ServiceDiagnosticConfig (Get-Content -Raw -LiteralPath $readback | ConvertFrom-Json)
            Write-Host "[service-diagnostic] verified $remote; human input, autopilot off"
        }
    } finally {
        Remove-Item -LiteralPath $local, $readback -ErrorAction SilentlyContinue
    }
}
