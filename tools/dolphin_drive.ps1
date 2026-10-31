<#
  dolphin_drive.ps1 -- synthetic input driver for CaveWii_v2 running in Dolphin.

  Drives the game the way a player does: host keyboard/mouse -> SendInput (DirectInput
  scancodes) -> Dolphin emulated Wiimote+Nunchuk -> CaveWii input layer. No human at the
  keyboard, no AutoHotkey install required.

  Pairs with the clean Config\WiimoteNew.ini profile (Wiimote1 = DInput/0/Keyboard Mouse):
    MOVE   W/A/S/D      -> D-Pad Up/Left/Down/Right (ids 0/2/1/3)
    LOOK   T/G/F/H      -> Nunchuk Stick Up/Down/Left/Right (camera rx/ry; letters chosen
                           over arrows so SendInput needs no extended-scancode handling)
    JUMP   SPACE        -> Wiimote B (id 5)
    MINE   J (hold)     -> Nunchuk Z (id 100)   [input_held(IB_ACTION1)]
    PLACE  K (tap)      -> Nunchuk C (id 101)    [input_pressed(IB_ACTION2), edge]
    SCROLL C / V        -> Wiimote - / + (ids 9/8)
    INV    I            -> Wiimote 1 (id 6)
    SHOT   P            -> Wiimote 2 (id 7, in-game screenshot)
    MENU   E / ESC      -> Wiimote A / Home (ids 4/10)
    IR     mouse        -> Wiimote IR pointer (menus only)

  Actions:
    -Action launch                 boot Dolphin (fullscreen, autoload +new) detached
    -Action kill                   stop all Dolphin processes
    -Action focus                  bring Dolphin to foreground
    -Action shot   -Out <png>      capture the game display (default DISPLAY2) to <png>
    -Action shotfull -Out <png>    capture the full virtual desktop to <png>
    -Action seq    -Steps "<dsl>"  run a step sequence (see DSL below)

  Step DSL (steps separated by ';' or newlines; first ':' splits cmd from args):
    focus               re-grab foreground
    down:KEY            key down (held until 'up')
    up:KEY              key up
    tap:KEY[:MS]        down, wait MS (default 70), up  -- for edge actions (place)
    wait:MS             sleep MS
    mdown|mup|rdown|rup left/right mouse button down/up
    mmove:X:Y           absolute mouse move to virtual-desktop pixel (X,Y)
    shot:PATH           capture game display (DISPLAY2) to PATH
    shotfull:PATH       capture full virtual desktop to PATH
  KEY names: W A S D SPACE ESC T G F H J K C V I P E UP DOWN LEFT RIGHT
#>
param(
  [Parameter(Mandatory=$true)][ValidateSet('launch','kill','focus','shot','shotfull','seq')]
  [string]$Action,
  [string]$Steps = '',
  [string]$Out = '',
  [string]$Region = 'display2'
)
$ErrorActionPreference = 'Stop'

$DOLPHIN = "$env:USERPROFILE\Desktop\Tools\Dolphin\stable-2603\Dolphin-x64\Dolphin.exe"
$USERDIR = "$env:USERPROFILE\Desktop\Tools\Dolphin\cavewii_v2-fresh-user"
$DOL     = "$USERDIR\Load\WiiSDSync\apps\CavEX\boot.dol"

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] p, int cb);

  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] struct HARDWAREINPUT { public uint uMsg; public ushort wParamL, wParamH; }
  [StructLayout(LayoutKind.Explicit)] struct InputUnion {
    [FieldOffset(0)] public MOUSEINPUT mi;
    [FieldOffset(0)] public KEYBDINPUT ki;
    [FieldOffset(0)] public HARDWAREINPUT hi;
  }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public InputUnion u; }

  const uint INPUT_MOUSE=0, INPUT_KEYBOARD=1;
  const uint KEYEVENTF_KEYUP=0x0002, KEYEVENTF_SCANCODE=0x0008, KEYEVENTF_EXTENDEDKEY=0x0001;
  const uint MOUSEEVENTF_MOVE=0x0001, MOUSEEVENTF_ABSOLUTE=0x8000, MOUSEEVENTF_VIRTUALDESK=0x4000;

  static void SendOne(INPUT i){ INPUT[] a = new INPUT[]{i}; SendInput(1,a,Marshal.SizeOf(typeof(INPUT))); }

  public static void Key(ushort scan, bool up, bool ext){
    INPUT i = new INPUT(); i.type=INPUT_KEYBOARD;
    i.u.ki.wVk=0; i.u.ki.wScan=scan;
    uint f = KEYEVENTF_SCANCODE; if(up) f|=KEYEVENTF_KEYUP; if(ext) f|=KEYEVENTF_EXTENDEDKEY;
    i.u.ki.dwFlags=f; SendOne(i);
  }
  public static void MouseBtn(uint flags){
    INPUT i = new INPUT(); i.type=INPUT_MOUSE; i.u.mi.dwFlags=flags; SendOne(i);
  }
  public static void MouseMoveAbs(int nx, int ny){
    INPUT i = new INPUT(); i.type=INPUT_MOUSE; i.u.mi.dx=nx; i.u.mi.dy=ny;
    i.u.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK; SendOne(i);
  }
}
'@

# name -> @(scancode, isExtended)
$KEYS = @{
  'W'=@(0x11,$false); 'A'=@(0x1E,$false); 'S'=@(0x1F,$false); 'D'=@(0x20,$false)
  'SPACE'=@(0x39,$false); 'ESC'=@(0x01,$false); 'ESCAPE'=@(0x01,$false)
  'T'=@(0x14,$false); 'G'=@(0x22,$false); 'F'=@(0x21,$false); 'H'=@(0x23,$false)
  'J'=@(0x24,$false); 'K'=@(0x25,$false); 'C'=@(0x2E,$false); 'V'=@(0x2F,$false)
  'I'=@(0x17,$false); 'P'=@(0x19,$false); 'E'=@(0x12,$false)
  'UP'=@(0x48,$true); 'DOWN'=@(0x50,$true); 'LEFT'=@(0x4B,$true); 'RIGHT'=@(0x4D,$true)
}

function Resolve-Key([string]$n){
  $k = $KEYS[$n.Trim().ToUpper()]
  if (-not $k) { throw "unknown key '$n'" }
  return $k
}
function Key-Down([string]$n){ $k = Resolve-Key $n; [Win]::Key([uint16]$k[0], $false, [bool]$k[1]) }
function Key-Up([string]$n){ $k = Resolve-Key $n; [Win]::Key([uint16]$k[0], $true,  [bool]$k[1]) }

function Get-DolphinProc {
  Get-Process Dolphin -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}
function Focus-Dolphin {
  $p = Get-DolphinProc
  if ($p) {
    [Win]::ShowWindow($p.MainWindowHandle, 9) | Out-Null   # SW_RESTORE
    [Win]::BringWindowToTop($p.MainWindowHandle) | Out-Null
    [Win]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    try { (New-Object -ComObject WScript.Shell).AppActivate($p.Id) | Out-Null } catch {}
    Start-Sleep -Milliseconds 140
    return $true
  }
  Write-Output "WARN: no Dolphin window found"
  return $false
}

function Get-Region([string]$region){
  if ($region -eq 'full') { return [System.Windows.Forms.SystemInformation]::VirtualScreen }
  $s = [System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1
  if (-not $s) { $s = [System.Windows.Forms.Screen]::PrimaryScreen }
  return $s.Bounds
}
function Capture([string]$path, [string]$region){
  if (-not $path) { throw "shot needs -Out / a path" }
  $dir = Split-Path $path -Parent
  if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
  $r = Get-Region $region
  $bmp = New-Object System.Drawing.Bitmap $r.Width, $r.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.X, $r.Y, 0, 0, $bmp.Size)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  Write-Output ("shot {0}  region={1} {2}x{3}@{4},{5}" -f $path,$region,$r.Width,$r.Height,$r.X,$r.Y)
}

function MouseMove-Virt([int]$x, [int]$y){
  $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $nx = [int](($x - $vs.X) * 65535.0 / [Math]::Max(1,($vs.Width  - 1)))
  $ny = [int](($y - $vs.Y) * 65535.0 / [Math]::Max(1,($vs.Height - 1)))
  [Win]::MouseMoveAbs($nx, $ny)
}

function Run-Seq([string]$steps){
  Focus-Dolphin | Out-Null
  $tokens = $steps -split '[;\r\n]+'
  foreach ($raw in $tokens) {
    $s = $raw.Trim()
    if (-not $s) { continue }
    $idx = $s.IndexOf(':')
    if ($idx -lt 0) { $cmd = $s.ToLower(); $arg = '' }
    else { $cmd = $s.Substring(0,$idx).ToLower(); $arg = $s.Substring($idx+1) }
    switch ($cmd) {
      'focus'    { Focus-Dolphin | Out-Null }
      'down'     { Key-Down $arg }
      'up'       { Key-Up $arg }
      'tap'      {
                   $a = $arg -split ':'
                   $ms = if ($a.Count -ge 2) { [int]$a[1] } else { 70 }
                   Key-Down $a[0]; Start-Sleep -Milliseconds $ms; Key-Up $a[0]
                 }
      'wait'     { Start-Sleep -Milliseconds ([int]$arg) }
      'mdown'    { [Win]::MouseBtn(0x0002) }
      'mup'      { [Win]::MouseBtn(0x0004) }
      'rdown'    { [Win]::MouseBtn(0x0008) }
      'rup'      { [Win]::MouseBtn(0x0010) }
      'mmove'    { $a = $arg -split ':'; MouseMove-Virt ([int]$a[0]) ([int]$a[1]) }
      'shot'     { Capture $arg 'display2' }
      'shotfull' { Capture $arg 'full' }
      default    { Write-Output "WARN: unknown step '$s'" }
    }
  }
  Write-Output "seq done"
}

switch ($Action) {
  'launch' {
    $existing = Get-Process Dolphin -ErrorAction SilentlyContinue
    if ($existing) { Write-Output "WARN: Dolphin already running (PID $($existing.Id -join ','))" }
    $p = Start-Process -FilePath $DOLPHIN `
      -ArgumentList @('-u',$USERDIR,'-e',$DOL,'-b','-C','Dolphin.Display.Fullscreen=True') -PassThru
    Write-Output "launched Dolphin PID $($p.Id) -> $DOL"
  }
  'kill' {
    $procs = Get-Process Dolphin -ErrorAction SilentlyContinue
    if ($procs) { $procs | Stop-Process -Force; Write-Output "killed $($procs.Count) Dolphin process(es)" }
    else { Write-Output "no Dolphin process to kill" }
  }
  'focus'    { Focus-Dolphin | Out-Null }
  'shot'     { Capture $Out $Region }
  'shotfull' { Capture $Out 'full' }
  'seq'      { Run-Seq $Steps }
}
