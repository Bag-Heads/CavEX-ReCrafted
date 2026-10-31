param(
    [string]$ProjectRoot = "",
    [string]$SourceUserRoot = "$env:USERPROFILE\Desktop\Tools\Dolphin\cavewii_v2-clean-user",
    [string]$World1UserRoot = "$env:USERPROFILE\Desktop\Tools\Dolphin\cavewii_v2-world1-user",
    [string]$FreshUserRoot = "$env:USERPROFILE\Desktop\Tools\Dolphin\cavewii_v2-fresh-user"
)

$ErrorActionPreference = "Stop"

if([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

if(-not (Test-Path $ProjectRoot)) {
    throw "Project root not found: $ProjectRoot"
}

$stageScript = Join-Path $ProjectRoot "tools\stage_dolphin_app.ps1"
$syncScript = Join-Path $ProjectRoot "tools\sync_dolphin_controls.ps1"
$sourceSaves = Join-Path $SourceUserRoot "Load\WiiSDSync\apps\CavEX\saves"
$sourceWorld1 = Join-Path $sourceSaves "World1"
$backupRoot = Join-Path $ProjectRoot "build\archive\world-backups"
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backupPath = Join-Path $backupRoot ("World1-" + $timestamp)

function Copy-OptionalTree {
    param(
        [string]$Source,
        [string]$Destination
    )

    if(-not (Test-Path $Source)) {
        return
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item (Join-Path $Source '*') $Destination -Recurse -Force
}

function Initialize-UserRoot {
    param(
        [string]$SourceRoot,
        [string]$TargetRoot
    )

    Remove-Item $TargetRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $TargetRoot | Out-Null

    foreach($name in @("Config", "GameSettings", "ResourcePacks", "Styles", "Themes")) {
        Copy-OptionalTree -Source (Join-Path $SourceRoot $name) `
                          -Destination (Join-Path $TargetRoot $name)
    }

    New-Item -ItemType Directory -Force -Path (Join-Path $TargetRoot "Load\WiiSDSync\apps") | Out-Null
}

function Stage-AppIntoUserRoot {
    param(
        [string]$Root,
        [switch]$WithWorld1
    )

    $appRoot = Join-Path $Root "Load\WiiSDSync\apps\CavEX"

    & powershell -ExecutionPolicy Bypass -File $stageScript `
        -SourceRoot $ProjectRoot `
        -StageRoot $appRoot
    if($LASTEXITCODE -ne 0) {
        throw "Failed staging app into $appRoot"
    }

    if($WithWorld1) {
        $targetSaves = Join-Path $appRoot "saves"
        New-Item -ItemType Directory -Force -Path $targetSaves | Out-Null
        Copy-Item $sourceWorld1 (Join-Path $targetSaves "World1") -Recurse -Force
    }

    & powershell -ExecutionPolicy Bypass -File $syncScript -UserRoot $Root
    if($LASTEXITCODE -ne 0) {
        throw "Failed syncing controls for $Root"
    }
}

if(-not (Test-Path $SourceUserRoot)) {
    throw "Source Dolphin user root not found: $SourceUserRoot"
}

if(-not (Test-Path $sourceWorld1)) {
    throw "Live World1 not found: $sourceWorld1"
}

New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
Copy-Item $sourceWorld1 $backupPath -Recurse -Force

Initialize-UserRoot -SourceRoot $SourceUserRoot -TargetRoot $World1UserRoot
Initialize-UserRoot -SourceRoot $SourceUserRoot -TargetRoot $FreshUserRoot

Stage-AppIntoUserRoot -Root $World1UserRoot -WithWorld1
Stage-AppIntoUserRoot -Root $FreshUserRoot

[pscustomobject]@{
    BackupWorld1 = $backupPath
    World1UserRoot = $World1UserRoot
    World1BootDol = (Join-Path $World1UserRoot "Load\WiiSDSync\apps\CavEX\boot.dol")
    FreshUserRoot = $FreshUserRoot
    FreshBootDol = (Join-Path $FreshUserRoot "Load\WiiSDSync\apps\CavEX\boot.dol")
}
