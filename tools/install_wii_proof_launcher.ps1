param(
    [string]$SdRoot = "",
    [string]$AppName = "CaveWiiStage",
    [int]$Port = 39019,
    [string]$Netmask = "",
    [string]$StageDol = "",
    [string]$MakeExe = "C:\msys64\usr\bin\make.exe",
    [switch]$SkipBuild,
    [switch]$AllowFixedDrive,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ServerRoot = Join-Path $RepoRoot "tools\wii_stage_server"

function Write-TextNoBom {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function Write-JsonNoBom {
    param([string]$Path, [object]$Object)
    $json = $Object | ConvertTo-Json -Depth 32
    Write-TextNoBom -Path $Path -Text ($json + "`n")
}

function Get-FileSha256Lower {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant()
}

function Find-RemovableSdRoot {
    $volumes = Get-Volume -ErrorAction SilentlyContinue |
        Where-Object {
            $_.DriveLetter -and
            $_.DriveType -eq "Removable" -and
            $_.FileSystem -match "^(FAT|FAT32|exFAT)$"
        } |
        Sort-Object DriveLetter
    if(@($volumes).Count -eq 1) {
        return "$($volumes[0].DriveLetter):\"
    }
    if(@($volumes).Count -gt 1) {
        $names = ($volumes | ForEach-Object {
            "$($_.DriveLetter): $($_.FileSystemLabel) $($_.FileSystem)"
        }) -join ", "
        throw "Multiple removable FAT/exFAT drives found; pass -SdRoot explicitly. Candidates: $names"
    }
    return ""
}

function Assert-SdTarget {
    param([string]$Root)
    $resolved = (Resolve-Path $Root).Path
    $driveRoot = [System.IO.Path]::GetPathRoot($resolved)
    if([string]::IsNullOrWhiteSpace($driveRoot)) {
        throw "Could not determine drive root for $Root"
    }
    $driveName = $driveRoot.TrimEnd("\").TrimEnd(":")
    $volume = Get-Volume -DriveLetter $driveName -ErrorAction SilentlyContinue
    if($volume -and $volume.DriveType -ne "Removable" -and -not $AllowFixedDrive) {
        throw "Refusing to install proof launcher to non-removable drive $Root ($($volume.DriveType)). Pass -AllowFixedDrive only if this is definitely the SD card."
    }
    return $resolved
}

function Build-StageServer {
    if($SkipBuild) {
        return
    }
    if(-not (Test-Path $MakeExe)) {
        throw "make.exe not found: $MakeExe"
    }
    $tmp = Join-Path $RepoRoot ".buildtmp\wii_stage_server_tmp"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $stageDolPath = Join-Path $ServerRoot "wii_stage_server.dol"
    $oldDevkitPro = $env:DEVKITPRO
    $oldDevkitPpc = $env:DEVKITPPC
    $oldPath = $env:PATH
    $oldTmp = $env:TMP
    $oldTemp = $env:TEMP
    $oldTmpDir = $env:TMPDIR
    $beforeSha = ""
    if(Test-Path $stageDolPath) {
        $beforeSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $stageDolPath).Hash.ToLowerInvariant()
    }
    try {
        $env:DEVKITPRO = "C:\msys64\opt\devkitpro"
        $env:DEVKITPPC = "C:\msys64\opt\devkitpro\devkitPPC"
        $env:TMP = $tmp
        $env:TEMP = $tmp
        $env:TMPDIR = $tmp
        $env:PATH = "C:\msys64\opt\devkitpro\devkitPPC\bin;C:\msys64\usr\bin;$env:PATH"
        Write-Host "[stage-server-build] tmp=$tmp"
        if(-not [string]::IsNullOrWhiteSpace($beforeSha)) {
            Write-Host "[stage-server-build] before_sha256=$beforeSha"
        }
        Push-Location $ServerRoot
        try {
            & $MakeExe DEVKITPRO=$env:DEVKITPRO DEVKITPPC=$env:DEVKITPPC clean
            if($LASTEXITCODE -ne 0) {
                throw "wii_stage_server clean failed with exit code $LASTEXITCODE"
            }
            if(Test-Path $stageDolPath) {
                throw "wii_stage_server clean left stale DOL in place: $stageDolPath"
            }
            & $MakeExe DEVKITPRO=$env:DEVKITPRO DEVKITPPC=$env:DEVKITPPC
            if($LASTEXITCODE -ne 0) {
                throw "wii_stage_server build failed with exit code $LASTEXITCODE"
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
    if(-not (Test-Path $stageDolPath)) {
        throw "wii_stage_server build did not produce $stageDolPath"
    }
    $afterSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $stageDolPath).Hash.ToLowerInvariant()
    $afterMd5 = (Get-FileHash -Algorithm MD5 -LiteralPath $stageDolPath).Hash.ToLowerInvariant()
    $afterBytes = (Get-Item -LiteralPath $stageDolPath).Length
    Write-Host "[stage-server-build] after_md5=$afterMd5 sha256=$afterSha bytes=$afterBytes"
}

if($SelfTest) {
    $SdRoot = Join-Path $RepoRoot ("tmp\install_wii_proof_launcher_selftest_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
    $AllowFixedDrive = $true
    $SkipBuild = $true
    New-Item -ItemType Directory -Force -Path $SdRoot | Out-Null
    Write-Host "[selftest] using temp SD root $SdRoot"
}

if([string]::IsNullOrWhiteSpace($StageDol)) {
    $StageDol = Join-Path $ServerRoot "wii_stage_server.dol"
}

if([string]::IsNullOrWhiteSpace($SdRoot)) {
    $SdRoot = Find-RemovableSdRoot
}
if([string]::IsNullOrWhiteSpace($SdRoot)) {
    throw "No SD root specified and no single removable FAT/FAT32/exFAT drive was detected"
}
if(-not (Test-Path $SdRoot)) {
    throw "SD root does not exist: $SdRoot"
}
$SdRoot = Assert-SdTarget $SdRoot

Build-StageServer

if(-not (Test-Path $StageDol)) {
    throw "missing stage server DOL: $StageDol"
}
$StageDol = (Resolve-Path $StageDol).Path
$sourceSha = Get-FileSha256Lower $StageDol
$sourceSize = (Get-Item $StageDol).Length

$appRoot = Join-Path $SdRoot "apps\$AppName"
New-Item -ItemType Directory -Force -Path $appRoot | Out-Null
$bootDol = Join-Path $appRoot "boot.dol"
Copy-Item $StageDol $bootDol -Force

$destSha = Get-FileSha256Lower $bootDol
$destSize = (Get-Item $bootDol).Length
if($destSha -ne $sourceSha -or $destSize -ne $sourceSize) {
    throw "stage launcher copy verification failed: source $sourceSha/$sourceSize dest $destSha/$destSize"
}

$safeNetmask = [System.Security.SecurityElement]::Escape($Netmask)
$argXml = "    <arg>--port</arg>`n    <arg>$Port</arg>"
if(-not [string]::IsNullOrWhiteSpace($Netmask)) {
    $argXml += "`n    <arg>--netmask</arg>`n    <arg>$safeNetmask</arg>"
}

$meta = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>DOL Stage Server</name>
  <coder>CaveWii</coder>
  <version>1.5</version>
  <release_date>$(Get-Date -Format yyyyMMdd)000000</release_date>
  <short_description>LAN DOL staging server</short_description>
  <long_description>Proof-launcher v1.6: persistent TCP upload/fetch/manifest/run-prep server with CWSTAGE4 SHA-256 verified writes to SD, CWFETCH1 SHA-256 verified reads from SD, CWMANIFEST1 SD proof/crash/benchmark inventory, CWRUNPREP1 DOL/args launch-readiness validation, and experimental CWRUN1 MEM2 DOL launch. Only peers in the console's own subnet are accepted; CWAUTH1 gates every other request against sd:/apps/CavEX/stage_token.txt when that file is provisioned (soft-launch no-op when it is absent -- see docs/stage_server_auth_2026-09-02.md). CWRUN1 is compiled and protocol-tested but still needs real Wii proof; default staging remains manual unless -LaunchAfterStage is explicit.</long_description>
  <arguments>
$argXml
  </arguments>
</app>
"@
Write-TextNoBom -Path (Join-Path $appRoot "meta.xml") -Text $meta

$readme = @"
CaveWii proof launcher v1.6 / stage server

Installed boot.dol sha256:
$destSha

Protocol:
- CWSTAGE4/CWSTAGE4R verified upload, with SD-side SHA-256 returned in PASS. An
  old CWSTAGE3/CWSTAGE3R client is refused loudly (stale_protocol_CWSTAGE3_...);
  upgrade the PC-side tool instead of retrying.
- CWFETCH1 verified download, with SD-side SHA-256 returned before bytes.
- CWMANIFEST1 SD proof inventory, including known crash/provenance/benchmark files
  such as sd:/hang_report.txt, sd:/fbguard_report.txt, and sd:/lighting_bench.txt.
- CWRUNPREP1 DOL header, libogc argv marker, launch-plan address, and cw_run_args.txt validation.
- CWRUN1 experimental MEM2 DOL launch after repeating the same SD-side readiness validation.
- CWHELLO1 capability probe (always answered, even to an unauthenticated/
  out-of-subnet-adjacent peer trying to diagnose why); its JSON reply carries
  token_provisioned so a client can tell whether CWAUTH1 is required.
- CWAUTH1 <token> auth handshake (docs/stage_server_auth_2026-09-02.md): only
  peers inside the console's own subnet are accepted at all; once
  sd:/apps/CavEX/stage_token.txt exists on the card, every request other
  than CWHELLO1/CWAUTH1 is refused (auth_required) until a matching CWAUTH1 is
  sent. SOFT-LAUNCH: while that file is absent, CWAUTH1 always succeeds and
  replies "no_token_provisioned" -- run tools/provision_wii_sd.ps1 to close
  that window.
- Persistent listener on TCP port $Port until HOME is pressed.
- CWRUN1 is hardware-unconfirmed; use -LaunchAfterStage only for explicit launcher testing.

Use:
1. Launch "DOL Stage Server" from the Homebrew Channel.
2. Leave the Wii on the stage-server screen.
3. On the PC, run:
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stage_wii_dol.ps1 -WiiHost <wii-ip> -AssumeServerRunning -Profile heartbeat_selftest
4. The staging script asks the Wii stage server to validate the staged DOL header and cw_run_args.txt with CWRUNPREP1 before it prints the collector command.
5. Default path: press HOME on the stage server, launch CaveWii_v2 from HBC, then run the collector command printed by the staging script.
6. Experimental path: add -LaunchAfterStage to send CWRUN1 after CWRUNPREP1. Treat this as hardware-unconfirmed until a real Wii run proves the jump.

Fetch a crash report or manifest after booting this app:
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_wii_stage_file.ps1 -WiiHost <wii-ip> -RemotePath sd:/hang_report.txt

Inventory known proof files after booting this app:
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\get_wii_stage_manifest.ps1 -WiiHost <wii-ip>
"@
Write-TextNoBom -Path (Join-Path $appRoot "README.txt") -Text $readme

$gitShort = (& git -C $RepoRoot rev-parse --short=7 HEAD 2>$null).Trim()
if([string]::IsNullOrWhiteSpace($gitShort)) { $gitShort = "nogit" }
$dirty = $false
if(& git -C $RepoRoot status --porcelain --untracked-files=no 2>$null) {
    $dirty = $true
}
$manifest = [pscustomobject]@{
    app = $AppName
    installed_at = (Get-Date).ToString("o")
    repo = $RepoRoot
    git_short = $gitShort
    dirty_tree = $dirty
    stage_server = [pscustomobject]@{
        source_dol = $StageDol
        installed_dol = $bootDol
        sha256 = $destSha
        bytes = $destSize
        port = $Port
        netmask = $Netmask
        version = "v1.6-subnet-token-auth"
        capabilities = @(
            "persistent-listener",
            "CWSTAGE4-sha256-put",
            "sd-target-hash-pass",
            "CWHELLO1-capabilities",
            "CWFETCH1-sha256-fetch",
            "CWMANIFEST1-proof-inventory",
            "CWRUNPREP1-dol-args-readiness",
            "CWRUN1-experimental-mem2-dol-launch",
            "CWAUTH1-subnet-token-auth"
        )
    }
}
Write-JsonNoBom -Path (Join-Path $appRoot "proof_launcher_manifest.json") -Object $manifest
Write-JsonNoBom -Path (Join-Path $SdRoot "CAVEWII_PROOF_LAUNCHER.json") -Object $manifest

Write-Host "[proof-launcher] installed $bootDol"
Write-Host "[proof-launcher] sha256=$destSha bytes=$destSize"
Write-Host "[proof-launcher] manifest=$(Join-Path $appRoot 'proof_launcher_manifest.json')"
if($SelfTest) {
    Write-Host "[selftest] PASS"
}
