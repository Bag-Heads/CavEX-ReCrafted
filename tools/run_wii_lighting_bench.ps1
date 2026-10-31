param(
    [string]$WiiHost = "",
    [int]$StagePort = 39019,
    [string]$DolPath = "",
    [string]$TargetPath = "sd:/apps/CaveWiiLightingBench/boot.dol",
    [string]$RemoteResultPath = "sd:/lighting_bench.txt",
    [string]$OutRoot = "",
    [int]$StageConnectTimeoutSeconds = 240,
    [int]$FetchConnectTimeoutSeconds = 300,
    [int]$ChunkSize = 8192,
    [int]$ChunkDelayMs = 2,
    [switch]$SkipBuild,
    [switch]$CleanBuild,
    [switch]$BuildOnly,
    [switch]$FetchOnly,
    [switch]$NoLaunch,
    [switch]$AllowDirty,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BenchRoot = Join-Path $RepoRoot "tools\wii_lighting_bench"
if([string]::IsNullOrWhiteSpace($DolPath)) {
    $DolPath = Join-Path $BenchRoot "wii_lighting_bench.dol"
}
if([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $RepoRoot ("tmp\wii_lighting_bench\" + (Get-Date -Format "yyyyMMdd_HHmmss"))
}

function Get-Sha256Hex {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-Md5Hex {
    param([string]$Path)
    return (Get-FileHash -Algorithm MD5 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Parse-LightingBenchResult {
    param([string]$Text)
    $line = ($Text -split "`r?`n" | Where-Object {
        $_ -match '^bench_lighting_budget:'
    } | Select-Object -First 1)
    if([string]::IsNullOrWhiteSpace($line)) {
        throw "lighting benchmark result missing bench_lighting_budget line"
    }
    $pattern = '^bench_lighting_budget: budget=(\d+) iters=(\d+) avg_slice_us=([0-9]+(?:\.[0-9]+)?) ns_per_pop=([0-9]+(?:\.[0-9]+)?) avg_light_writes=([0-9]+(?:\.[0-9]+)?) grid=(\d+)x(\d+)x(\d+)$'
    if($line -notmatch $pattern) {
        throw "lighting benchmark result line has unexpected format: $line"
    }
    $budget = [int]$Matches[1]
    if($budget -ne 4096) {
        throw "lighting benchmark result budget mismatch: expected 4096 got $budget"
    }
    return [pscustomobject]@{
        line = $line
        budget = $budget
        iters = [int]$Matches[2]
        avg_slice_us = [double]$Matches[3]
        ns_per_pop = [double]$Matches[4]
        avg_light_writes = [double]$Matches[5]
        grid_x = [int]$Matches[6]
        grid_y = [int]$Matches[7]
        grid_z = [int]$Matches[8]
    }
}

function Invoke-SelfTest {
    $sample = "bench_lighting_budget: budget=4096 iters=200 avg_slice_us=215.00 ns_per_pop=52.49 avg_light_writes=2716.0 grid=48x96x48`n"
    $parsed = Parse-LightingBenchResult $sample
    if($parsed.budget -ne 4096 -or $parsed.grid_x -ne 48 -or $parsed.grid_y -ne 96 -or $parsed.grid_z -ne 48) {
        throw "selftest parsed incorrect benchmark values"
    }
    $bad = $sample.Replace("budget=4096", "budget=8192")
    $failed = $false
    try {
        $null = Parse-LightingBenchResult $bad
    } catch {
        $failed = $true
    }
    if(-not $failed) {
        throw "selftest failed to reject wrong budget"
    }
    Write-Host "PASS run_wii_lighting_bench selftest"
}

function Build-LightingBenchDol {
    $make = "C:\msys64\usr\bin\make.exe"
    if(-not (Test-Path $make)) {
        throw "missing MSYS make at $make"
    }
    $tmp = Join-Path $RepoRoot ".buildtmp\wii_lighting_bench_tmp"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $oldDevkitPro = $env:DEVKITPRO
    $oldDevkitPpc = $env:DEVKITPPC
    $oldPath = $env:PATH
    $oldTmp = $env:TMP
    $oldTemp = $env:TEMP
    $oldTmpDir = $env:TMPDIR
    try {
        $env:DEVKITPRO = "C:\msys64\opt\devkitpro"
        $env:DEVKITPPC = "C:\msys64\opt\devkitpro\devkitPPC"
        $env:TMP = $tmp
        $env:TEMP = $tmp
        $env:TMPDIR = $tmp
        $env:PATH = "C:\msys64\opt\devkitpro\devkitPPC\bin;C:\msys64\usr\bin;$env:PATH"
        Push-Location $BenchRoot
        try {
            if($CleanBuild) {
                & $make DEVKITPRO=$env:DEVKITPRO DEVKITPPC=$env:DEVKITPPC clean
                if($LASTEXITCODE -ne 0) {
                    throw "wii_lighting_bench clean failed"
                }
            }
            & $make DEVKITPRO=$env:DEVKITPRO DEVKITPPC=$env:DEVKITPPC
            if($LASTEXITCODE -ne 0) {
                throw "wii_lighting_bench build failed"
            }
        } finally {
            Pop-Location
        }
    } finally {
        $env:DEVKITPRO = $oldDevkitPro
        $env:DEVKITPPC = $oldDevkitPpc
        $env:PATH = $oldPath
        $env:TMP = $oldTmp
        $env:TEMP = $oldTemp
        $env:TMPDIR = $oldTmpDir
    }
    if(-not (Test-Path $DolPath)) {
        throw "missing benchmark DOL after build: $DolPath"
    }
}

function Write-DolSummary {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path
    $sha = Get-Sha256Hex $Path
    $md5 = Get-Md5Hex $Path
    Write-Host "[BENCH-DOL] $Path bytes=$($item.Length) md5=$md5 sha256=$sha"
    return [pscustomobject]@{
        path = $Path
        bytes = [int64]$item.Length
        md5 = $md5
        sha256 = $sha
    }
}

function Get-ManifestFileEntry {
    param(
        [object]$Manifest,
        [string]$RemotePath
    )
    return @($Manifest.files | Where-Object { $_.path -eq $RemotePath }) |
        Select-Object -First 1
}

function Save-StageManifestBeforeFetch {
    param(
        [string]$RemotePath,
        [string]$OutDirectory
    )
    $manifestFile = Join-Path $OutDirectory "stage_manifest_before_fetch.json"
    $manifestArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\get_wii_stage_manifest.ps1"),
        "-WiiHost", $WiiHost,
        "-Port", "$StagePort",
        "-ConnectTimeoutSeconds", "$FetchConnectTimeoutSeconds",
        "-OutFile", $manifestFile
    )
    $manifestOutput = & powershell @manifestArgs
    $manifestExitCode = $LASTEXITCODE
    $manifestOutput | ForEach-Object { Write-Host $_ }
    if($manifestExitCode -ne 0) {
        throw "get_wii_stage_manifest.ps1 failed with exit code $manifestExitCode. Relaunch CaveWii Stage Server and retry with -FetchOnly."
    }
    $manifest = Get-Content -Raw -LiteralPath $manifestFile | ConvertFrom-Json
    $entry = Get-ManifestFileEntry -Manifest $manifest -RemotePath $RemotePath
    if($null -eq $entry) {
        throw "stage manifest did not include ${RemotePath}; rebuild/relaunch the stage server with lighting benchmark manifest support"
    }
    if(-not $entry.exists) {
        throw "stage manifest reports ${RemotePath} is missing; run the benchmark on the Wii before fetching"
    }
    Write-Host "[BENCH-MANIFEST] $manifestFile $RemotePath bytes=$($entry.bytes) sha256=$($entry.sha256)"
    return [pscustomobject]@{
        path = $manifestFile
        remote_path = $RemotePath
        bytes = [int64]$entry.bytes
        sha256 = [string]$entry.sha256
    }
}

if($SelfTest) {
    Invoke-SelfTest
    exit 0
}

if(-not $FetchOnly -and -not $SkipBuild) {
    Build-LightingBenchDol
}
if(-not $FetchOnly -and -not (Test-Path $DolPath)) {
    throw "missing benchmark DOL: $DolPath"
}

$dolSummary = $null
if(Test-Path $DolPath) {
    $DolPath = (Resolve-Path $DolPath).Path
    $dolSummary = Write-DolSummary $DolPath
}

if($BuildOnly) {
    exit 0
}
if([string]::IsNullOrWhiteSpace($WiiHost)) {
    throw "WiiHost is required unless -SelfTest or -BuildOnly is used"
}

New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

if(-not $FetchOnly) {
    $runId = "lighting_bench_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
    $stageArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\stage_wii_dol.ps1"),
        "-WiiHost", $WiiHost,
        "-DolPath", $DolPath,
        "-TargetPath", $TargetPath,
        "-Port", "$StagePort",
        "-StageConnectTimeoutSeconds", "$StageConnectTimeoutSeconds",
        "-ChunkSize", "$ChunkSize",
        "-ChunkDelayMs", "$ChunkDelayMs",
        "-Profile", "heartbeat_selftest",
        "-RunId", $runId,
        "-AssumeServerRunning"
    )
    if(-not $NoLaunch) {
        Write-Warning "Requesting experimental CWRUN1 launch for the benchmark; this path is hardware-unconfirmed until a real Wii result file is fetched."
        $stageArgs += "-LaunchAfterStage"
    }
    if($AllowDirty) {
        $stageArgs += "-AllowDirty"
    }
    Write-Host "[bench-stage] using generic stage_wii_dol.ps1; ignore its CaveWii_v2 collector hint for this benchmark and use this wrapper's BENCH/NEXT lines."
    & powershell @stageArgs
    if($LASTEXITCODE -ne 0) {
        throw "stage_wii_dol.ps1 failed with exit code $LASTEXITCODE"
    }
    if($NoLaunch) {
        Write-Host "[NEXT] Launch the staged benchmark from HBC, then relaunch CaveWii Stage Server and rerun this script with -FetchOnly."
        exit 0
    }
    Write-Host "[NEXT] After the benchmark writes $RemoteResultPath, relaunch CaveWii Stage Server if needed; this script will now try to fetch the result."
}

$resultFile = Join-Path $OutRoot "lighting_bench.txt"
$manifestSummary = Save-StageManifestBeforeFetch -RemotePath $RemoteResultPath -OutDirectory $OutRoot
$fetchArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $RepoRoot "tools\fetch_wii_stage_file.ps1"),
    "-WiiHost", $WiiHost,
    "-Port", "$StagePort",
    "-ConnectTimeoutSeconds", "$FetchConnectTimeoutSeconds",
    "-RemotePath", $RemoteResultPath,
    "-OutFile", $resultFile
)
& powershell @fetchArgs
if($LASTEXITCODE -ne 0) {
    throw "fetch_wii_stage_file.ps1 failed with exit code $LASTEXITCODE. If the benchmark already ran, relaunch CaveWii Stage Server and retry with -FetchOnly."
}

$text = Get-Content -Raw -LiteralPath $resultFile
$parsed = Parse-LightingBenchResult $text
$record = [pscustomobject]@{
    status = "PASS"
    created_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    remote_result_path = $RemoteResultPath
    local_result_path = $resultFile
    stage_manifest = $manifestSummary
    dol = $dolSummary
    result = $parsed
    note = "This proves only the fetched benchmark result for the staged artifact; broader freeze claims still require their own profile PASS."
}
$jsonFile = Join-Path $OutRoot "lighting_bench_result.json"
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $jsonFile -Encoding UTF8
Write-Host "[BENCH-RESULT] $($parsed.line)"
Write-Host "[BENCH-JSON] $jsonFile"
