param(
    [string]$SdRoot = "E:\",
    [string]$AppName = "CaveWii_v2",
    [string]$StageAppName = "CaveWiiStage",
    [int]$StagePort = 39019,
    [string]$StageNetmask = "",
    [switch]$SkipDol,
    # Saves are NOT touched by default -- see the IncludeSaves block below for why.
    # -SkipSaves is kept so existing call sites and docs stay valid; it is now a
    # no-op because skipping IS the default.
    [switch]$SkipSaves,
    [switch]$IncludeSaves,
    [string]$Autoload = "",
    [switch]$SkipStageServer,
    [switch]$AllowFixedDrive,
    # Which pack config.json selects. "" = AUTO: pick texturepacks/beta-1.7.3 when it is
    # present in the repo, else "assets". Until 2026-07-30 this was hard-wired to "assets"
    # with no way out, so EVERY provision run staged the authentic Beta pack and then
    # DESELECTED it -- and the resulting "all the textures look like fCavEX" report reads
    # exactly like an asset bug. Pass -TexturePack assets to force the bundled art.
    [string]$TexturePack = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Copy-TreeClean {
    param([string]$Source, [string]$Dest)
    if(-not (Test-Path $Source)) { return }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    robocopy $Source $Dest /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if($LASTEXITCODE -gt 7) {
        throw "robocopy failed: $Source -> $Dest ($LASTEXITCODE)"
    }
}

# Additive copy: overwrite same-named files, but NEVER delete anything already in
# the destination. Used for the player's saves, where /MIR would be destructive.
function Copy-TreeAdditive {
    param([string]$Source, [string]$Dest)
    if(-not (Test-Path $Source)) { return }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    robocopy $Source $Dest /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if($LASTEXITCODE -gt 7) {
        throw "robocopy failed: $Source -> $Dest ($LASTEXITCODE)"
    }
}

function Write-JsonNoBom {
    param([string]$Path, [object]$Object)
    $json = $Object | ConvertTo-Json -Depth 32
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $json + "`n", $utf8)
}

function Write-TextNoBom {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
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
        $names = ($volumes | ForEach-Object { "$($_.DriveLetter): $($_.FileSystemLabel) $($_.FileSystem)" }) -join ", "
        throw "Multiple removable FAT/exFAT drives found; pass -SdRoot explicitly. Candidates: $names"
    }
    return ""
}

function Assert-SdTarget {
    param([string]$Root)
    $driveName = [System.IO.Path]::GetPathRoot((Resolve-Path $Root).Path).TrimEnd("\").TrimEnd(":")
    $volume = Get-Volume -DriveLetter $driveName -ErrorAction SilentlyContinue
    if($volume -and $volume.DriveType -ne "Removable" -and -not $AllowFixedDrive) {
        throw "Refusing to provision non-removable drive $Root ($($volume.DriveType)). Pass -AllowFixedDrive only if this is definitely the SD card."
    }
}

function Get-SourceFingerprint {
    $paths = @("config_wii.json", "assets")
    if(Test-Path (Join-Path $RepoRoot "texturepacks\beta-1.7.3")) {
        $paths += "texturepacks\beta-1.7.3"
    }
    $hashes = New-Object System.Collections.Generic.List[string]
    foreach($p in $paths) {
        $full = Join-Path $RepoRoot $p
        if(Test-Path $full -PathType Leaf) {
            $hashes.Add("$p=$((Get-FileHash $full -Algorithm SHA256).Hash)")
        } elseif(Test-Path $full -PathType Container) {
            Get-ChildItem $full -Recurse -File | Sort-Object FullName | ForEach-Object {
                $rel = $_.FullName.Substring($RepoRoot.Length + 1)
                $hashes.Add("$rel=$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)")
            }
        }
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes(($hashes -join "`n"))
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "")
    } finally {
        $sha.Dispose()
    }
}

if(-not (Test-Path $SdRoot)) {
    $detected = Find-RemovableSdRoot
    if(-not [string]::IsNullOrWhiteSpace($detected)) {
        Write-Host "SD root $SdRoot not found; using detected removable drive $detected"
        $SdRoot = $detected
    }
}
if(-not (Test-Path $SdRoot)) {
    throw "SD root does not exist and no removable FAT/FAT32/exFAT drive was detected: $SdRoot"
}
$SdRoot = (Resolve-Path $SdRoot).Path
Assert-SdTarget $SdRoot

$AppRoot = Join-Path $SdRoot "apps\$AppName"
New-Item -ItemType Directory -Force -Path $AppRoot | Out-Null

$baseConfigPath = Join-Path $RepoRoot "config_wii.json"
if(-not (Test-Path $baseConfigPath)) {
    throw "missing $baseConfigPath"
}
$cfg = Get-Content -Raw $baseConfigPath | ConvertFrom-Json
# autoload is OPT-IN (-Autoload "+new" or -Autoload "<world name>").
#
# This used to be forced to "+new" unconditionally, which is a hardware-demo
# behaviour that is actively hostile on a PLAY card: every boot skipped the menu
# and CREATED a brand-new world, so the 2026-07-30 session accumulated junk
# worlds 121/122/123 and the maintainer could not reach his own save. Provision
# for a demo run by asking for it; the default is the normal menu.
if($Autoload -ne "") {
    if(-not $cfg.PSObject.Properties["autoload"]) {
        $cfg | Add-Member -NotePropertyName autoload -NotePropertyValue $Autoload
    } else {
        $cfg.autoload = $Autoload
    }
    Write-Host "[provision] autoload: $Autoload"
} elseif($cfg.PSObject.Properties["autoload"]) {
    $cfg.PSObject.Properties.Remove("autoload")
}
if(-not $cfg.PSObject.Properties["debug"]) {
    $cfg | Add-Member -NotePropertyName debug -NotePropertyValue ([pscustomobject]@{})
}
if(-not $cfg.debug.PSObject.Properties["overlay"]) {
    $cfg.debug | Add-Member -NotePropertyName overlay -NotePropertyValue "off"
} else {
    $cfg.debug.overlay = "off"
}
if(-not $cfg.PSObject.Properties["paths"]) {
    $cfg | Add-Member -NotePropertyName paths -NotePropertyValue ([pscustomobject]@{})
}
# Resolve the pack ONCE, then apply it. See the -TexturePack note in the param block:
# silently overwriting the player's (or the previous run's) selection is how a staged Beta
# pack ends up unused on the card.
$packChoice = $TexturePack
if(-not $packChoice) {
    $packChoice = if(Test-Path (Join-Path $RepoRoot "texturepacks/beta-1.7.3")) {
        "texturepacks/beta-1.7.3"
    } else {
        "assets"
    }
}
if(-not $cfg.paths.PSObject.Properties["texturepack"]) {
    $cfg.paths | Add-Member -NotePropertyName texturepack -NotePropertyValue $packChoice
} else {
    $cfg.paths.texturepack = $packChoice
}
if(-not $cfg.paths.PSObject.Properties["worlds"]) {
    $cfg.paths | Add-Member -NotePropertyName worlds -NotePropertyValue "saves"
} else {
    $cfg.paths.worlds = "saves"
}

Write-Host "[provision] texturepack selected: $packChoice"

foreach($root in @($SdRoot, $AppRoot)) {
    Write-JsonNoBom -Path (Join-Path $root "config.json") -Object $cfg
    Copy-Item $baseConfigPath (Join-Path $root "config_wii.json") -Force
    Copy-TreeClean -Source (Join-Path $RepoRoot "assets") -Dest (Join-Path $root "assets")
    # SAVES ARE THE PLAYER'S, NOT OURS -- never mirrored, and never touched
    # unless explicitly asked for.
    #
    # Copy-TreeClean is robocopy /MIR, which DELETES anything in the destination
    # that is not in the source. Copying repo saves/ that way would have wiped the
    # maintainer's real worlds off the card and replaced them with the 118 test
    # worlds checked into the repo. That is one mistyped command away from losing
    # a world that only exists on an SD card. Even with -IncludeSaves we now copy
    # ADDITIVELY (no /MIR), so an existing card save is never deleted -- worst
    # case a same-named world is overwritten, which is what the flag asks for.
    if($IncludeSaves) {
        Copy-TreeAdditive -Source (Join-Path $RepoRoot "saves") -Dest (Join-Path $root "saves")
        Write-Host "[provision] saves: repo saves/ copied ADDITIVELY into $root\saves"
    }
    $betaPack = Join-Path $RepoRoot "texturepacks\beta-1.7.3"
    if(Test-Path $betaPack) {
        Copy-TreeClean -Source $betaPack -Dest (Join-Path $root "texturepacks\beta-1.7.3")
    }
}

if(-not $SkipDol) {
    $dol = Join-Path $RepoRoot "CaveWii_v2.dol"
    if(Test-Path $dol) {
        Copy-Item $dol (Join-Path $AppRoot "boot.dol") -Force
    } else {
        Write-Warning "CaveWii_v2.dol not found; run build-wii before booting from SD"
    }
}

# Stage-server auth (CWSTAGE4, 2026-09; docs/stage_server_auth_2026-09-02.md):
# copy the shared secret onto the card at the exact path the Wii server reads
# (sd:/apps/CavEX/stage_token.txt -- the game's app dir, not the stage
# server's own $StageAppName dir; the token is per-game-instance, alongside
# cw_run_args.txt). tools\.stage_token is gitignored and OPTIONAL: while it
# does not exist locally, this is a no-op and the card stays in the
# SOFT-LAUNCH (unprovisioned) state the stage server already tolerates.
$stageTokenSource = Join-Path $RepoRoot "tools\.stage_token"
if(Test-Path $stageTokenSource) {
    Copy-Item $stageTokenSource (Join-Path $AppRoot "stage_token.txt") -Force
    Write-Host "[provision] stage auth: token staged to $(Join-Path $AppRoot 'stage_token.txt')"
} else {
    Write-Host "[provision] stage auth: tools\.stage_token not found locally -- card stays SOFT-LAUNCH (no token enforced). Create tools\.stage_token to provision one."
}

if(-not $SkipStageServer) {
    $installArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\install_wii_proof_launcher.ps1"),
        "-SdRoot", $SdRoot,
        "-AppName", $StageAppName,
        "-Port", "$StagePort",
        "-SkipBuild"
    )
    if(-not [string]::IsNullOrWhiteSpace($StageNetmask)) {
        $installArgs += @("-Netmask", $StageNetmask)
    }
    if($AllowFixedDrive) {
        $installArgs += "-AllowFixedDrive"
    }
    & powershell @installArgs
    if($LASTEXITCODE -ne 0) {
        throw "install_wii_proof_launcher.ps1 failed with exit code $LASTEXITCODE"
    }
}

$stamp = (& git -C $RepoRoot rev-parse --short=7 HEAD 2>$null).Trim()
if([string]::IsNullOrWhiteSpace($stamp)) { $stamp = "nogit" }
$dirty = (& git -C $RepoRoot status --porcelain --untracked-files=no 2>$null)
if($dirty) { $stamp = "$stamp+" }
$stamp = "{0}_{1}" -f $stamp, (Get-Date -Format "MM.dd")
$manifest = [pscustomobject]@{
    app = $AppName
    provisioned_at = (Get-Date).ToString("o")
    repo = $RepoRoot
    build_stamp = $stamp
    source_fingerprint = Get-SourceFingerprint
}
Write-JsonNoBom -Path (Join-Path $SdRoot "SD_MANIFEST.json") -Object $manifest
Write-JsonNoBom -Path (Join-Path $AppRoot "SD_MANIFEST.json") -Object $manifest

$testPlan = @"
CaveWii_v2 hardware proof card

Build on this card: $stamp

Normal proof loop:
1. Leave this SD card in the Wii.
2. Launch "CaveWii Stage Server" from HBC.
3. On the PC, run:
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_staged_wii_hardware.ps1 -WiiHost <wii-ip> -Profile heartbeat_selftest
4. The runner first saves the stage-server manifest plus any previous hang_report.txt / fbguard_report.txt / lighting_bench.txt under tmp\wii_runs\<run-id>\stage_mailbox, including the app-dir fallback copies the watchdog writes when the sd:/ root open fails (previous_*_report_fallback.txt).
5. Confirm the script prints a CWRUNPREP1 run_ready result.
6. Press HOME on the stage server, launch CaveWii_v2 from HBC, then let the collector continue.
7. Use far_idle_2m / far_walk_6m / perf_stress_5m / watchdog_inject / redstone_clock_2m / save_migrate for targeted proof.

Do not use automatic HBC wiiload retries as a fallback on this rig. Accepted HBC
uploads can black-screen the Wii and require a hard power hold; direct wiiload
runners require an explicit -AllowHbcUpload opt-in.

Stage-server auth (CWSTAGE4, docs/stage_server_auth_2026-09-02.md): only peers
on this console's own subnet are accepted. sd:/apps/CavEX/stage_token.txt
gates every other request behind CWAUTH1 once it exists on the card (create
tools\.stage_token locally and re-run this script to provision one); while it
is absent, auth is a soft-launch no-op and the PC tools print what to do.

Experimental direct launch path:
1. After the stage server has been real-hardware verified for CWSTAGE4 and CWRUNPREP1, run:
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_staged_wii_hardware.ps1 -WiiHost <wii-ip> -LaunchAfterStage -Profile heartbeat_selftest
2. Confirm the script prints a CWRUN1 launching result and then waits for CaveWii_v2 telemetry.
3. Treat CWRUN1 as hardware-unconfirmed until that real Wii run proves the MEM2 jump and argv delivery.

If config/assets change, re-run tools\provision_wii_sd.ps1.
"@
[System.IO.File]::WriteAllText((Join-Path $AppRoot "TEST_PLAN.txt"), $testPlan, [System.Text.UTF8Encoding]::new($false))

Write-Host "Provisioned $AppRoot"
if(-not $SkipStageServer) {
    Write-Host "Stage app: $(Join-Path $SdRoot \"apps\$StageAppName\")"
}
Write-Host "Manifest: $(Join-Path $SdRoot 'SD_MANIFEST.json')"
