param(
    [Alias("ProjectRoot")]
    [string]$SourceRoot = "",
    [Alias("TargetRoot")]
    [string]$StageRoot = "",
    [string]$ConfigSource = "config.json",
    [string]$AutobootWorldValue = "",
    [switch]$EnableAutobootLatest,
    [switch]$EnableRuntimeTrace,
    [switch]$EnableMovementBenchmark,
    [switch]$EnableStackVerify,
    [switch]$EnableContainerSmoke,
    [string]$StackVerifyItemId = "3",
    [string]$StackVerifyCount = "3",
    [switch]$IncludeSaves,
    [switch]$ResetSaves
)

$ErrorActionPreference = "Stop"

if([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Split-Path -Parent $PSScriptRoot
}

if([string]::IsNullOrWhiteSpace($StageRoot)) {
    $StageRoot = Join-Path $SourceRoot "build\dolphin-app\CaveWii_v2"
}

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Content
    )

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

if(-not (Test-Path $SourceRoot)) {
    throw "Source root not found: $SourceRoot"
}

$dolPath = Join-Path $SourceRoot "build\wii\CaveWii_v2.dol"
if(-not (Test-Path $dolPath)) {
    throw "Built DOL not found: $dolPath"
}

$configSourcePath = Join-Path $SourceRoot $ConfigSource
if(-not (Test-Path $configSourcePath)) {
    throw "Config source not found: $configSourcePath"
}

$preserveSavesRoot = $null
$stageSavesPath = Join-Path $StageRoot "saves"

if((Test-Path $stageSavesPath) -and -not $ResetSaves -and -not $IncludeSaves) {
    $preserveSavesRoot = Join-Path $env:TEMP ("cavewii_v2-stage-saves-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $preserveSavesRoot | Out-Null
    Copy-Item (Join-Path $stageSavesPath '*') $preserveSavesRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Remove-Item $StageRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $StageRoot | Out-Null

$copyPaths = @(
    @{ Source = $dolPath; Target = "boot.dol"; Recurse = $false },
    @{ Source = $configSourcePath; Target = "config.json"; Recurse = $false },
    @{ Source = (Join-Path $SourceRoot "icon.png"); Target = "icon.png"; Recurse = $false },
    @{ Source = (Join-Path $SourceRoot "meta.xml"); Target = "meta.xml"; Recurse = $false },
    @{ Source = (Join-Path $SourceRoot "assets"); Target = "assets"; Recurse = $true }
)

if($IncludeSaves) {
    $copyPaths += @{ Source = (Join-Path $SourceRoot "saves"); Target = "saves"; Recurse = $true }
}

foreach($entry in $copyPaths) {
    if(-not (Test-Path $entry.Source)) {
        continue
    }

    $targetPath = Join-Path $StageRoot $entry.Target
    if($entry.Recurse) {
        New-Item -ItemType Directory -Force -Path $targetPath | Out-Null
        Copy-Item (Join-Path $entry.Source '*') $targetPath -Recurse
    } else {
        Copy-Item $entry.Source $targetPath
    }
}

New-Item -ItemType Directory -Force -Path (Join-Path $StageRoot "saves") | Out-Null

if($preserveSavesRoot -and (Test-Path $preserveSavesRoot)) {
    Copy-Item (Join-Path $preserveSavesRoot '*') (Join-Path $StageRoot "saves") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $preserveSavesRoot -Recurse -Force -ErrorAction SilentlyContinue
}

$configPath = Join-Path $StageRoot "config.json"
$config = Get-Content $configPath -Raw | ConvertFrom-Json

if(-not $config.debug) {
    $config | Add-Member -NotePropertyName "debug" -NotePropertyValue ([pscustomobject]@{})
}

$autobootValue = if(-not [string]::IsNullOrWhiteSpace($AutobootWorldValue)) {
    $AutobootWorldValue
} elseif($EnableAutobootLatest) {
    "latest"
} else {
    "off"
}
$traceValue = if($EnableRuntimeTrace) { "file" } else { "off" }
$benchmarkValue = if($EnableMovementBenchmark) { "on" } else { "off" }
$stackVerifyValue = if($EnableStackVerify) { "place_stack" } else { "off" }
$containerSmokeValue = if($EnableContainerSmoke) { "open_basic" } else { "off" }

$config.debug | Add-Member -NotePropertyName "autoboot_world" `
    -NotePropertyValue $autobootValue `
    -Force
$config.debug | Add-Member -NotePropertyName "runtime_trace" `
    -NotePropertyValue $traceValue `
    -Force
$config.debug | Add-Member -NotePropertyName "movement_benchmark" `
    -NotePropertyValue $benchmarkValue `
    -Force
$config.debug | Add-Member -NotePropertyName "stack_verify" `
    -NotePropertyValue $stackVerifyValue `
    -Force
$config.debug | Add-Member -NotePropertyName "stack_verify_item_id" `
    -NotePropertyValue $StackVerifyItemId `
    -Force
$config.debug | Add-Member -NotePropertyName "stack_verify_count" `
    -NotePropertyValue $StackVerifyCount `
    -Force
$config.debug | Add-Member -NotePropertyName "container_smoke" `
    -NotePropertyValue $containerSmokeValue `
    -Force

$configJson = $config | ConvertTo-Json -Depth 8
Write-Utf8NoBom -Path $configPath -Content $configJson

if($EnableRuntimeTrace) {
    Remove-Item (Join-Path $StageRoot "saves\\debug-runtime.log") -Force -ErrorAction SilentlyContinue
}

[pscustomobject]@{
    StageRoot = $StageRoot
    BootDol = (Join-Path $StageRoot "boot.dol")
    ConfigSource = $ConfigSource
    AutobootWorld = $config.debug.autoboot_world
    RuntimeTrace = $config.debug.runtime_trace
    MovementBenchmark = $config.debug.movement_benchmark
    StackVerify = $config.debug.stack_verify
    ContainerSmoke = $config.debug.container_smoke
}
