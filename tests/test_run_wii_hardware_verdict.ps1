$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$source = Get-Content -Raw (Join-Path $repo "tools/run_wii_hardware.ps1")
$tailMatch = [regex]::Match($source, '(?ms)^\$pass = Join-Path \$OutRoot "PASS\.json".*\z')
if(-not $tailMatch.Success) { throw "runner verdict block not found" }
$startMatch = [regex]::Match($source, '(?ms)^function Start-Collector \{.*?^\}\r?\n')
if(-not $startMatch.Success) { throw "runner collector start block not found" }
$waitMatch = [regex]::Match($source, '(?ms)^Write-Host "\[wait\] collector pid=.*?(?=^\$pass = Join-Path)')
if(-not $waitMatch.Success) { throw "runner collector wait block not found" }
$quarantineMatch = [regex]::Match($source, '(?ms)^# BEGIN prior-verdict quarantine\r?\n(.*?)^# END prior-verdict quarantine\r?\n')
$quarantine = if($quarantineMatch.Success) { $quarantineMatch.Groups[1].Value } else { "" }

$root = Join-Path $env:TEMP ("cw-verdict-test-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path (Join-Path $root "tools") -Force | Out-Null
@'
param([string]$PassPath, [string]$ExpectedRunId, [string]$ExpectedProfile, [string]$ExpectedBuild)
$artifact = Get-Content -Raw $PassPath | ConvertFrom-Json
if($artifact.valid -ne $true -or $artifact.run_id -ne $ExpectedRunId -or
   $artifact.profile -ne $ExpectedProfile -or $artifact.build_id -ne $ExpectedBuild) {
    throw "mock validator rejected invalid PASS"
}
'@ | Set-Content (Join-Path $root "tools/validate_wii_pass.ps1")

$harness = @'
param([string]$Root, [string]$Scenario)
$ErrorActionPreference = "Stop"
$RepoRoot = $Root
$OutRoot = Join-Path $Root $Scenario
New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null
$RunId = "same-run"
$Profile = "far_idle_2m"
$expectedBuild = "abc_09.24"
$PowerCycleOnFailure = $false
$collector = [pscustomobject]@{ ExitCode = 0 }
$collectorTimeout = 10
function Invoke-ConfiguredPowerCycle {
    "invoked" | Set-Content (Join-Path $OutRoot "power-cycle-invoked.txt")
}
function Write-Pass([bool]$Valid) {
    @{ valid=$Valid; run_id=$RunId; profile=$Profile; build_id=$expectedBuild } |
        ConvertTo-Json | Set-Content (Join-Path $OutRoot "PASS.json")
    "pass summary" | Set-Content (Join-Path $OutRoot "summary.md")
}
if($Scenario -eq "stale-pass-fresh-fail") { Write-Pass $true }
__QUARANTINE__
switch($Scenario) {
    "stale-pass-fresh-fail" {
        "fresh failure" | Set-Content (Join-Path $OutRoot "FAIL.json")
        "fail summary" | Set-Content (Join-Path $OutRoot "summary.md")
    }
    "nonzero-with-pass" { Write-Pass $true; $collector.ExitCode = 7 }
    "nonzero-power-cycle" {
        Write-Pass $true
        $collector.ExitCode = 7
        $PowerCycleOnFailure = $true
    }
    "null-with-pass" { Write-Pass $true; $collector.ExitCode = $null }
    "collector-exit-zero" { Write-Pass $true }
    "collector-exit-seven" { Write-Pass $true }
    "both-verdicts" {
        Write-Pass $true
        "fresh failure" | Set-Content (Join-Path $OutRoot "FAIL.json")
    }
    "invalid-pass" { Write-Pass $false }
    "valid-pass" { Write-Pass $true }
}
if($Scenario -like "collector-exit-*") {
    $pythonExe = $env:ComSpec
    $exitCode = if($Scenario -eq "collector-exit-zero") { 0 } else { 7 }
    $collectorArgs = @("/c", "exit", "$exitCode")
    $collectorOut = Join-Path $OutRoot "collector.stdout.log"
    $collectorErr = Join-Path $OutRoot "collector.stderr.log"
    __START_COLLECTOR__
    $collector = Start-Collector
    __WAIT_COLLECTOR__
}
__VERDICT__
'@
$harness = $harness.Replace("__QUARANTINE__", $quarantine).Replace("__START_COLLECTOR__", $startMatch.Value).Replace("__WAIT_COLLECTOR__", $waitMatch.Value).Replace("__VERDICT__", $tailMatch.Value)
$harnessPath = Join-Path $root "harness.ps1"
$harness | Set-Content $harnessPath
$failures = 0
try {
    $ErrorActionPreference = "Continue"
    foreach($case in @("stale-pass-fresh-fail", "verdict-less", "nonzero-with-pass", "nonzero-power-cycle", "null-with-pass", "both-verdicts", "invalid-pass", "valid-pass", "collector-exit-zero", "collector-exit-seven")) {
        $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $harnessPath -Root $root -Scenario $case 2>&1
        $code = $LASTEXITCODE
        $printedPass = ($output -join "`n") -match '(?m)^\[PASS\] '
        $expectPass = $case -in @("valid-pass", "collector-exit-zero")
        $cycled = Test-Path (Join-Path (Join-Path $root $case) "power-cycle-invoked.txt")
        $expectCycle = $case -eq "nonzero-power-cycle"
        if(($code -eq 0) -ne $expectPass -or $printedPass -ne $expectPass -or $cycled -ne $expectCycle) {
            Write-Host "FAIL $case exit=$code passPrinted=$printedPass powerCycled=$cycled output=$($output -join ' | ')"
            $failures++
        } else {
            Write-Host "PASS $case exit=$code"
        }
    }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
if($failures) { exit 1 }
Write-Host "PASS run_wii_hardware verdict regression"
