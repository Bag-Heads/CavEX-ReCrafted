<#
    dolphin_capture.ps1 — automated Dolphin run + screenshot capture for CaveWii_v2.

    Launches the Wii .dol in Dolphin, brings the window to the foreground,
    and captures full-screen PNGs at a series of timepoints. Optionally injects
    a scripted key sequence between captures (best-effort menu navigation).

    Native GDI capture (System.Drawing) is used rather than Dolphin's own
    screenshot hotkey so it works headless and regardless of focus tier.

    PREREQUISITE — the Wii build reads config + assets from the SD card Dolphin
    mounts (the WiiSDCardSyncFolder). If config_wii.json / assets/ are missing
    there, the .dol fails config load and main() bails (looks like a dark frame
    that never progresses). Use -StageSd to copy them from the repo first.

    Examples:
      # one-shot: stage SD, boot, grab 4 frames over 16s
      pwsh tools/dolphin_capture.ps1 -StageSd -At 4,8,12,16

      # boot and hold a frame every 5s for a minute (soak watch)
      pwsh tools/dolphin_capture.ps1 -At 5,10,15,20,30,40,50,60
#>
[CmdletBinding()]
param(
    [string]$Dolphin = "$env:USERPROFILE\Desktop\Tools\Dolphin\stable-2603\Dolphin-x64\Dolphin.exe",
    [string]$Dol     = "$(Split-Path $PSScriptRoot -Parent)\CaveWii_v2.dol",
    [string]$Repo    = "$(Split-Path $PSScriptRoot -Parent)",
    [string]$SdSync  = "$env:USERPROFILE\Desktop\Tools\Dolphin\stable-2603\Dolphin-x64\User\Load\WiiSDSync",
    [string]$OutDir  = "$env:USERPROFILE\AppData\Local\Temp\cavewii_shots",
    [int[]] $At      = @(4, 8, 12, 16),
    [switch]$StageSd,
    [switch]$OverlayOn = $true
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Win32 helpers: focus + synthetic keystrokes.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
}
"@

function Stage-Sd {
    $overlay = if ($OverlayOn) { "on" } else { "off" }
    $cfg = @"
{
    "paths": { "texturepack": "assets", "worlds": "saves" },
    "input": {
        "player_forward": [0, 200, 910], "player_backward": [1, 201, 911],
        "player_left": [2, 202, 912], "player_right": [3, 203, 913],
        "player_jump": [5, 210], "player_sneak": [211],
        "item_action_left": [100, 204], "item_action_right": [101, 205],
        "scroll_left": [9, 208, 213], "scroll_right": [8, 209, 212],
        "inventory": [6, 206], "open_menu": [10, 214],
        "gui_up": [0, 200, 900, 910, 920], "gui_down": [1, 201, 901, 911, 921],
        "gui_left": [2, 202, 902, 912, 922], "gui_right": [3, 203, 903, 913, 923],
        "gui_click": [4, 204], "gui_click_alt": [5, 205],
        "screenshot": [7], "debug_overlay": [207]
    },
    "debug": { "overlay": "$overlay" }
}
"@
    foreach ($dst in @($SdSync, "$SdSync\cavewii")) {
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Set-Content -Path "$dst\config_wii.json" -Value $cfg -Encoding utf8
        Copy-Item -Recurse -Force "$Repo\assets" "$dst\assets"
    }
    Write-Host "[stage] config_wii.json (overlay=$overlay) + assets -> SD root and cavewii/"
}

function Capture([string]$path) {
    $b  = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bm = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g  = [System.Drawing.Graphics]::FromImage($bm)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bm.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bm.Dispose()
}

if ($StageSd) { Stage-Sd }

Get-Process Dolphin -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Dol))     { throw "DOL not found: $Dol" }
if (-not (Test-Path $Dolphin)) { throw "Dolphin not found: $Dolphin" }

Write-Host "[run] DOL built $((Get-Item $Dol).LastWriteTime)"
$p = Start-Process -FilePath $Dolphin -ArgumentList "-e", $Dol -PassThru
Start-Sleep -Milliseconds 1500
try { [Win32]::ShowWindow($p.MainWindowHandle, 5) | Out-Null
      [Win32]::SetForegroundWindow($p.MainWindowHandle) | Out-Null } catch {}

$prev = 0
foreach ($t in $At) {
    Start-Sleep -Seconds ([math]::Max(0, $t - $prev)); $prev = $t
    if (-not (Get-Process Dolphin -ErrorAction SilentlyContinue)) {
        Write-Host "[cap] t=${t}s: Dolphin process gone"; break
    }
    $out = Join-Path $OutDir ("cavewii_t{0:D3}s.png" -f $t)
    Capture $out
    Write-Host "[cap] t=${t}s -> $out"
}
Write-Host "[done] shots in $OutDir"
