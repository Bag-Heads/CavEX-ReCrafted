<#
  dolphin_prove.ps1 -- one-shot HEADLESS Dolphin prover for config-driven demos.

  No human, no GDI/display capture, no synthetic input: stage a .dol + a
  prepared SD-root config, boot Dolphin batch (-b -e), poll the live
  Load\WiiSD.raw (read-only, via tools/extract_sd_proof.py) until the demo's
  verdict file piston_blast_result.txt appears on the SD root (demos fopen
  relative paths -> SD root; the chest_selftest_result.txt precedent,
  source/network/server_local.c:2859), then kill Dolphin, extract the verdict
  + ss_*.png / screenshot.png (debug.auto_screenshot EFB dumps, source/main.c:
  521,1061) + perf_log.txt into -OutDir, and grade the verdict.

  Rig doc: .claude/skills/run-dolphin-demo/SKILL.md (SD-ROOT config, delete
  WiiSD.raw after every edit, batch boot). GFX.ini gets [Hacks]
  EFBToTextureEnable=False + EFBAccessEnable=True patched in idempotently --
  that is what makes the game's EFB->RAM screenshot readback real instead of
  black.

  Exit codes:
    0  verdict file contains 'VERDICT PASS'
    3  verdict file contains 'VERDICT SKIP'
    1  anything else: FAIL verdict, timeout, verdict file never appeared
    2  rig/staging failure (missing inputs, md5 mismatch, python absent, ...)

  Usage:
    powershell -ExecutionPolicy Bypass -File tools\dolphin_prove.ps1 `
      -ConfigJson tmp\prove_config.json [-Dol <path.dol>] [-TimeoutSec 120] `
      [-OutDir tmp\dolphin_prove]
#>
param(
  [string]$Dol = "$((Split-Path $PSScriptRoot -Parent) -replace '\','/')/CaveWii_v2.dol",
  [string]$ConfigJson = '',
  [int]$TimeoutSec = 120,
  [string]$OutDir = "$((Split-Path $PSScriptRoot -Parent) -replace '\','/')/tmp/dolphin_prove",
  # The armed demo's verdict file on the SD root (each smoke demo writes its
  # own: piston_blast_result.txt, piston_sticky_result.txt,
  # nether_roundtrip_result.txt, tnt_chain_result.txt).
  # TimeoutSec is effectively the RUN DURATION (Dolphin locks WiiSD.raw, so
  # polling cannot see the verdict mid-run): size it per demo -- blast/sticky
  # ~100-180s, tnt_chain ~240s, nether_roundtrip >= 300s (3 world switches).
  [string]$VerdictFile = 'piston_blast_result.txt',
  # Preserve the test profile's saves/ across this run (the cold-boot reload
  # prover's boot 2: sync saves out of boot 1's WiiSD.raw first via
  # extract_sd_proof.py syncsaves, then run with -KeepSaves).
  [switch]$KeepSaves
)
$ErrorActionPreference = 'Stop'

$DOLPHIN  = "$env:USERPROFILE\Desktop\Tools\Dolphin\stable-2603\Dolphin-x64\Dolphin.exe"
$USERDIR  = "$env:USERPROFILE\Desktop\Tools\Dolphin\cavewii_v2-fresh-user"
$SYNCROOT = "$USERDIR\Load\WiiSDSync"
$BOOTDOL  = "$SYNCROOT\apps\CavEX\boot.dol"
$SDCONFIG = "$SYNCROOT\config.json"
$SDRAW    = "$USERDIR\Load\WiiSD.raw"
$GFXINI   = "$USERDIR\Config\GFX.ini"
$VERDICT  = $VerdictFile
$POLLSEC  = 10
# Stale proof files that must never survive into a fresh run's evidence.
# (perf_log.txt is opened "w" fresh each boot -- a leftover one is a lie.
# EVERY *_result.txt is swept, not just this run's, so a stale sibling demo's
# verdict can never be mistaken for evidence.)
$STALE    = @('screenshot.png', 'perf_log.txt')
$STALEGLOB = 'ss_*.png'
$STALEGLOB2 = '*_result.txt'

$EXTRACTOR = Join-Path $PSScriptRoot 'extract_sd_proof.py'

function Fail([string]$msg) { Write-Host "FATAL: $msg"; exit 2 }

# Patch key=value pairs into one section of a Dolphin .ini without touching
# anything else; creates the file/section if absent. Idempotent.
function Set-IniSectionValues([string]$Path, [string]$Section, $Pairs) {
  $lines = @()
  if (Test-Path $Path) { $lines = @(Get-Content -Path $Path) }
  $header = "[$Section]"
  $secStart = -1
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i].Trim() -ieq $header) { $secStart = $i; break }
  }
  if ($secStart -lt 0) {
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -ne '') { $lines += '' }
    $lines += $header
    foreach ($k in $Pairs.Keys) { $lines += ('{0} = {1}' -f $k, $Pairs[$k]) }
  } else {
    $secEnd = $lines.Count
    for ($j = $secStart + 1; $j -lt $lines.Count; $j++) {
      if ($lines[$j].Trim() -match '^\[.+\]$') { $secEnd = $j; break }
    }
    $missing = @()
    foreach ($k in $Pairs.Keys) {
      $want = '{0} = {1}' -f $k, $Pairs[$k]
      $found = $false
      for ($j = $secStart + 1; $j -lt $secEnd; $j++) {
        if ($lines[$j] -match ('^\s*' + [regex]::Escape($k) + '\s*=')) {
          $lines[$j] = $want; $found = $true
        }
      }
      if (-not $found) { $missing += $want }
    }
    if ($missing.Count -gt 0) {
      $head = @($lines[0..$secStart])
      $tail = @()
      if ($secStart + 1 -le $lines.Count - 1) { $tail = @($lines[($secStart + 1)..($lines.Count - 1)]) }
      $lines = $head + $missing + $tail
    }
  }
  Set-Content -Path $Path -Value $lines -Encoding Ascii
  Write-Host "GFX.ini: [$Section] $(($Pairs.Keys | ForEach-Object { ('{0}={1}' -f $_, $Pairs[$_]) }) -join ' ')"
}

function Remove-StaleProof([string]$Dir) {
  foreach ($n in $STALE) {
    $p = Join-Path $Dir $n
    if (Test-Path $p) { Remove-Item -Path $p -Force -Confirm:$false; Write-Host "  deleted stale $p" }
  }
  Get-ChildItem -Path $Dir -Filter $STALEGLOB -File -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -Path $_.FullName -Force -Confirm:$false; Write-Host "  deleted stale $($_.FullName)"
  }
  Get-ChildItem -Path $Dir -Filter $STALEGLOB2 -File -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -Path $_.FullName -Force -Confirm:$false; Write-Host "  deleted stale $($_.FullName)"
  }
  # World-save contamination guard (adversarial-review finding): a world persisted
  # by an earlier graceful session could carry demo artifacts (e.g. a dropped
  # planks item) into a reloaded run and taint the verdict. This is the dedicated
  # test profile -- its worlds are disposable; the prover always runs autoload
  # "+new" on a clean slate.
  if ($KeepSaves) {
    Write-Host "  keeping saves/ (-KeepSaves: cold-boot reload verify phase)"
  } else {
    $saves = Join-Path $Dir 'saves'
    if (Test-Path $saves) {
      Remove-Item -Path $saves -Recurse -Force -Confirm:$false
      Write-Host "  deleted stale $saves (world-save contamination guard)"
    }
  }
}

# ---- validate inputs -------------------------------------------------------
if (-not $ConfigJson) { Fail '-ConfigJson is required (a prepared SD-root config.json)' }
if (-not (Test-Path $ConfigJson)) { Fail "config not found: $ConfigJson" }
if (-not (Test-Path $Dol)) { Fail "dol not found: $Dol" }
if (-not (Test-Path $DOLPHIN)) { Fail "Dolphin.exe not found: $DOLPHIN" }
if (-not (Test-Path $EXTRACTOR)) { Fail "extractor not found: $EXTRACTOR" }
$pythonCmd = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $pythonCmd) { $pythonCmd = Get-Command python -ErrorAction SilentlyContinue }
if (-not $pythonCmd) { Fail 'python not on PATH (extract_sd_proof.py needs python3 + pyfatfs)' }
$PYTHON = $pythonCmd.Source

# ---- 1. kill any running Dolphin ------------------------------------------
$procs = Get-Process dolphin* -ErrorAction SilentlyContinue
if ($procs) {
  $procs | Stop-Process -Force -Confirm:$false
  Write-Host "killed $($procs.Count) Dolphin process(es)"
  Start-Sleep -Seconds 2
}

# ---- 2. stage: boot.dol (md5-gated) + SD-root config + stale sweep --------
Copy-Item -Path $Dol -Destination $BOOTDOL -Force
$srcMd5 = (Get-FileHash -Algorithm MD5 -Path $Dol).Hash
$dstMd5 = (Get-FileHash -Algorithm MD5 -Path $BOOTDOL).Hash
Write-Host "dol md5 src=$srcMd5"
Write-Host "dol md5 dst=$dstMd5"
if ($srcMd5 -ne $dstMd5) { Fail 'boot.dol md5 mismatch after copy' }

Copy-Item -Path $ConfigJson -Destination $SDCONFIG -Force
Write-Host "staged config: $ConfigJson -> $SDCONFIG"

Write-Host 'sweeping stale proof files from the SD sync root:'
Remove-StaleProof $SYNCROOT

# ---- 3. GFX.ini: real EFB->RAM readback (else screenshots come back black) -
Set-IniSectionValues $GFXINI 'Hacks' ([ordered]@{
  'EFBToTextureEnable' = 'False'
  'EFBAccessEnable'    = 'True'
})

# ---- 4. delete WiiSD.raw (MANDATORY resync after any config edit) ---------
if (Test-Path $SDRAW) {
  Remove-Item -Path $SDRAW -Force -Confirm:$false
  Write-Host "deleted $SDRAW (resync)"
}

# ---- prepare OutDir (and clear its own stale proof) -----------------------
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
Remove-StaleProof $OutDir

# ---- 5. launch Dolphin batch ----------------------------------------------
# Same invocation as dolphin_drive.ps1 'launch' (dolphin_drive.ps1:195-196):
# -u picks the portable user profile, -e boots the staged .dol, -b batch mode.
$proc = Start-Process -FilePath $DOLPHIN `
  -ArgumentList @('-u', $USERDIR, '-e', $BOOTDOL, '-b', '-C', 'Dolphin.Display.Fullscreen=True') -PassThru
$dolphinPid = $proc.Id
Write-Host "launched Dolphin PID $dolphinPid -> $BOOTDOL (timeout ${TimeoutSec}s)"

# ---- 6. poll the live SD image for the verdict file -----------------------
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$found = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds $POLLSEC
  if (Test-Path $SDRAW) {
    & $PYTHON $EXTRACTOR $SDRAW probe $VERDICT | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -eq 0) { $found = $true; break }
  } else {
    Write-Host '  poll: WiiSD.raw not created yet'
  }
  if ($proc.HasExited) {
    Write-Host "  WARN: Dolphin exited early (code $($proc.ExitCode)) -- image is final"
    break
  }
}
if (-not $found) { Write-Host "poll: $VERDICT did not appear within ${TimeoutSec}s" }

# one extra poll interval so trailing writes (screenshots, perf_log) land
Start-Sleep -Seconds $POLLSEC
try { Stop-Process -Id $dolphinPid -Force -Confirm:$false -ErrorAction Stop; Write-Host "stopped Dolphin PID $dolphinPid" }
catch { Write-Host "Dolphin PID $dolphinPid already gone" }
Start-Sleep -Seconds 3

# ---- 7. extract the proof set ---------------------------------------------
if (-not (Test-Path $SDRAW)) { Fail 'WiiSD.raw never appeared -- Dolphin did not build the SD image' }
& $PYTHON $EXTRACTOR $SDRAW extract $OutDir | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { Fail "extract_sd_proof.py extract failed (exit $LASTEXITCODE)" }

# ---- 8. grade the verdict --------------------------------------------------
$verdictPath = Join-Path $OutDir $VERDICT
if (-not (Test-Path $verdictPath)) {
  Write-Host "VERDICT: no $VERDICT extracted (demo never wrote it / timeout)"
  exit 1
}
$verdictText = Get-Content -Raw -Path $verdictPath
Write-Host "--- $VERDICT ---"
Write-Host $verdictText
Write-Host '----------------'
if ($verdictText -match 'VERDICT PASS') { exit 0 }
if ($verdictText -match 'VERDICT SKIP') { exit 3 }
exit 1
