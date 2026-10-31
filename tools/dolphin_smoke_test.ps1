param(
    [string]$DolphinExe = "$env:USERPROFILE\Desktop\Tools\Dolphin\dev-2603-49\Dolphin-x64\Dolphin.exe",
    [string]$DolPath = "",
    [string]$ProjectRoot = "",
    [string]$DolphinUserRoot = "",
    [switch]$SkipMenuInput,
    [switch]$RunPassiveObserve,
    [switch]$RunMovementStress,
    [switch]$RunStackVerify,
    [switch]$RunContainerSmoke,
    [switch]$RunAutonomousExplore,
    [int]$ExploreSegments = 8,
    [int]$ExploreHoldMs = 10000
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$defaultStageRoot = Join-Path $repoRoot "build\dolphin-app\CaveWii_v2"
$defaultSdSyncRoot = ""
if($DolphinUserRoot) {
    $defaultSdSyncRoot = Join-Path $DolphinUserRoot "Load\WiiSDSync\apps\CavEX"
}

if([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    if($defaultSdSyncRoot -and (Test-Path $defaultSdSyncRoot)) {
        $ProjectRoot = $defaultSdSyncRoot
    } elseif(Test-Path $defaultStageRoot) {
        $ProjectRoot = $defaultStageRoot
    } else {
        $ProjectRoot = $repoRoot
    }
}

if([string]::IsNullOrWhiteSpace($DolPath)) {
    $stagedBootDol = Join-Path $ProjectRoot "boot.dol"
    if(Test-Path $stagedBootDol) {
        $DolPath = $stagedBootDol
    } else {
        $DolPath = Join-Path $repoRoot "build\wii\CaveWii_v2.dol"
    }
}

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Content
    )

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SmokeWin32 {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
  }

  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);

  [DllImport("user32.dll")]
  public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

  [DllImport("user32.dll")]
  public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

  [DllImport("user32.dll")]
  public static extern bool PrintWindow(IntPtr hwnd, IntPtr hDC, uint nFlags);

  [DllImport("user32.dll")]
  public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

  [DllImport("user32.dll")]
  public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

  [StructLayout(LayoutKind.Sequential)]
  public struct POINT {
    public int X;
    public int Y;
  }

  [DllImport("user32.dll")]
  public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);

  [DllImport("user32.dll")]
  public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

  [DllImport("user32.dll")]
  public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);

  [DllImport("user32.dll")]
  public static extern int GetWindowTextLength(IntPtr hWnd);

  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

  [DllImport("user32.dll")]
  public static extern bool SetCursorPos(int X, int Y);

  [DllImport("user32.dll")]
  public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);

  [DllImport("user32.dll")]
  public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);

  [StructLayout(LayoutKind.Sequential)]
  public struct INPUT {
    public uint type;
    public INPUTUNION U;
  }

  [StructLayout(LayoutKind.Explicit)]
  public struct INPUTUNION {
    [FieldOffset(0)]
    public KEYBDINPUT ki;
  }

  [StructLayout(LayoutKind.Sequential)]
  public struct KEYBDINPUT {
    public ushort wVk;
    public ushort wScan;
    public uint dwFlags;
    public uint time;
    public IntPtr dwExtraInfo;
  }

  [DllImport("user32.dll", SetLastError = true)]
  public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

  [DllImport("user32.dll")]
  public static extern uint MapVirtualKey(uint uCode, uint uMapType);

  public const int SW_RESTORE = 9;
  public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
  public const uint SWP_NOSIZE = 0x0001;
  public const uint SWP_NOMOVE = 0x0002;
  public const uint SWP_SHOWWINDOW = 0x0040;
  public const uint PW_CLIENTONLY = 0x00000001;
  public const uint PW_RENDERFULLCONTENT = 0x00000002;
  public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
  public const uint MOUSEEVENTF_LEFTUP = 0x0004;
  public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
  public const uint MOUSEEVENTF_RIGHTUP = 0x0010;
  public const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020;
  public const uint MOUSEEVENTF_MIDDLEUP = 0x0040;
  public const uint MOUSEEVENTF_XDOWN = 0x0080;
  public const uint MOUSEEVENTF_XUP = 0x0100;
  public const uint XBUTTON1 = 0x0001;
  public const uint KEYEVENTF_KEYUP = 0x0002;
  public const uint KEYEVENTF_SCANCODE = 0x0008;
  public const uint KEYEVENTF_EXTENDEDKEY = 0x0001;
  public const uint INPUT_KEYBOARD = 1;
}
"@

function Wait-MainWindow {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while((Get-Date) -lt $deadline) {
        $Process.Refresh()
        if($Process.HasExited) {
            throw "Dolphin exited before a main window appeared."
        }
        if($Process.MainWindowHandle -ne 0) {
            return
        }
        Start-Sleep -Milliseconds 250
    }

    throw "Timed out waiting for Dolphin window."
}

function Get-ProcessWindows {
    param([System.Diagnostics.Process]$Process)

    $targetPid = [uint32]$Process.Id
    $rows = New-Object System.Collections.Generic.List[object]
    [SmokeWin32]::EnumWindows({
        param($hWnd, $lParam)

        $windowPid = [uint32]0
        [SmokeWin32]::GetWindowThreadProcessId($hWnd, [ref]$windowPid) | Out-Null
        if($windowPid -eq $targetPid) {
            $len = [SmokeWin32]::GetWindowTextLength($hWnd)
            $buffer = New-Object System.Text.StringBuilder ($len + 1)
            [SmokeWin32]::GetWindowText($hWnd, $buffer, $buffer.Capacity) | Out-Null
            $rows.Add([PSCustomObject]@{
                Handle = $hWnd
                Title = $buffer.ToString()
            }) | Out-Null
        }

        return $true
    }, [IntPtr]::Zero) | Out-Null

    return $rows
}

function Wait-GameWindow {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while((Get-Date) -lt $deadline) {
        if($Process.HasExited) {
            throw "Dolphin exited before the emulation window appeared."
        }

        $window = Get-ProcessWindows -Process $Process |
            Where-Object { $_.Title -like '* | *' } |
            Sort-Object { $_.Title.Length } -Descending |
            Select-Object -First 1

        if($window) {
            return $window
        }

        Start-Sleep -Milliseconds 250
    }

    throw "Timed out waiting for Dolphin emulation window."
}

function Find-WindowByTitle {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$Title
    )

    return Get-ProcessWindows -Process $Process |
        Where-Object { $_.Title -eq $Title } |
        Select-Object -First 1
}

function Focus-Window {
    param([IntPtr]$Handle)

    [SmokeWin32]::ShowWindow($Handle, [SmokeWin32]::SW_RESTORE) | Out-Null
    [SmokeWin32]::SetWindowPos(
        $Handle,
        [SmokeWin32]::HWND_TOPMOST,
        0, 0, 0, 0,
        [SmokeWin32]::SWP_NOMOVE -bor [SmokeWin32]::SWP_NOSIZE -bor [SmokeWin32]::SWP_SHOWWINDOW
    ) | Out-Null
    [SmokeWin32]::SetForegroundWindow($Handle) | Out-Null
    Start-Sleep -Milliseconds 300
}

function Get-WindowRect {
    param([IntPtr]$Handle)

    $rect = New-Object SmokeWin32+RECT
    if(-not [SmokeWin32]::GetWindowRect($Handle, [ref]$rect)) {
        throw "Failed to read Dolphin window bounds."
    }
    return $rect
}

function Get-ClientRectScreen {
    param([IntPtr]$Handle)

    $rect = New-Object SmokeWin32+RECT
    if(-not [SmokeWin32]::GetClientRect($Handle, [ref]$rect)) {
        throw "Failed to read Dolphin client bounds."
    }

    $origin = New-Object SmokeWin32+POINT
    $origin.X = 0
    $origin.Y = 0
    if(-not [SmokeWin32]::ClientToScreen($Handle, [ref]$origin)) {
        throw "Failed to translate Dolphin client bounds."
    }

    return [PSCustomObject]@{
        Left = $origin.X
        Top = $origin.Y
        Right = $origin.X + ($rect.Right - $rect.Left)
        Bottom = $origin.Y + ($rect.Bottom - $rect.Top)
    }
}

function Save-WindowCapture {
    param(
        [IntPtr]$Handle,
        [string]$Path
    )

    Focus-Window -Handle $Handle
    Start-Sleep -Milliseconds 150
    $rect = Get-ClientRectScreen -Handle $Handle
    $width = [Math]::Max(1, $rect.Right - $rect.Left)
    $height = [Math]::Max(1, $rect.Bottom - $rect.Top)

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    }
    catch {
        $hdc = $graphics.GetHdc()
        try {
            $printed = [SmokeWin32]::PrintWindow($Handle, $hdc, [SmokeWin32]::PW_RENDERFULLCONTENT)
            if(-not $printed) {
                [void][SmokeWin32]::PrintWindow($Handle, $hdc, [SmokeWin32]::PW_CLIENTONLY)
            }
        }
        finally {
            $graphics.ReleaseHdc($hdc)
        }
    }

    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
}

function Prime-WindowInput {
    param([IntPtr]$Handle)

    # Dolphin sometimes ignores synthetic keyboard input until its client area
    # has received a real pointer event. Right click is safe on world-select.
    Click-Window -Handle $Handle -Button Right -XRatio 0.92 -YRatio 0.92 -DelayMs 180
}

function Get-VirtualKeyCode {
    param([string]$KeyName)

    switch($KeyName.ToUpperInvariant()) {
        "P" { return [byte]0x50 }
        "O" { return [byte]0x4F }
        "W" { return [byte]0x57 }
        "A" { return [byte]0x41 }
        "S" { return [byte]0x53 }
        "D" { return [byte]0x44 }
        "DOWN" { return [byte]0x28 }
        "UP" { return [byte]0x26 }
        "LEFT" { return [byte]0x25 }
        "RIGHT" { return [byte]0x27 }
        "E" { return [byte]0x45 }
        "SPACE" { return [byte]0x20 }
        "LSHIFT" { return [byte]0xA0 }
        "ENTER" { return [byte]0x0D }
        "ESCAPE" { return [byte]0x1B }
        default { throw "Unsupported key name: $KeyName" }
    }
}

function Add-UniqueConfigBinding {
    param(
        [System.Object[]]$Values,
        [int]$Value
    )

    $result = New-Object System.Collections.Generic.List[object]
    if($Values) {
        foreach($entry in $Values) {
            $result.Add($entry) | Out-Null
        }
    }

    $alreadyPresent = $false
    foreach($entry in $result) {
        if([int]$entry -eq $Value) {
            $alreadyPresent = $true
            break
        }
    }

    if(-not $alreadyPresent) {
        $result.Add($Value) | Out-Null
    }

    return @($result.ToArray())
}

function Enable-AutonomousBindings {
    param([string]$Root)

    $configPath = Join-Path $Root "config.json"
    if(-not (Test-Path $configPath)) {
        return
    }

    $config = Get-Content $configPath -Raw | ConvertFrom-Json
    if(-not $config.input) {
        return
    }

    $config.input.gui_click = Add-UniqueConfigBinding -Values $config.input.gui_click -Value 80
    $config.input.gui_click_alt = Add-UniqueConfigBinding -Values $config.input.gui_click_alt -Value 79

    $configJson = $config | ConvertTo-Json -Depth 8
    Write-Utf8NoBom -Path $configPath -Content $configJson
}

function Enable-ContainerSmokeConfig {
    param([string]$Root)

    $configPath = Join-Path $Root "config.json"
    if(-not (Test-Path $configPath)) {
        return
    }

    $config = Get-Content $configPath -Raw | ConvertFrom-Json
    if(-not $config.debug) {
        $config | Add-Member -NotePropertyName "debug" -NotePropertyValue ([pscustomobject]@{})
    }

    $config.debug | Add-Member -NotePropertyName "container_smoke" `
        -NotePropertyValue "open_basic" `
        -Force

    $configJson = $config | ConvertTo-Json -Depth 8
    Write-Utf8NoBom -Path $configPath -Content $configJson
}

function Send-KeyDown {
    param([string]$KeyName)

    $vk = [byte](Get-VirtualKeyCode -KeyName $KeyName)
    $scan = [uint16][SmokeWin32]::MapVirtualKey($vk, 0)
    $input = New-Object SmokeWin32+INPUT
    $input.type = [SmokeWin32]::INPUT_KEYBOARD
    $input.U.ki.wVk = $vk
    $input.U.ki.wScan = $scan
    $input.U.ki.dwFlags = 0
    if($KeyName -in @("UP", "DOWN", "LEFT", "RIGHT")) {
        $input.U.ki.dwFlags = $input.U.ki.dwFlags -bor [SmokeWin32]::KEYEVENTF_EXTENDEDKEY
    }
    [void][SmokeWin32]::SendInput(1, @($input), [Runtime.InteropServices.Marshal]::SizeOf([type][SmokeWin32+INPUT]))
    [SmokeWin32]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
}

function Send-KeyUp {
    param([string]$KeyName)

    $vk = [byte](Get-VirtualKeyCode -KeyName $KeyName)
    $scan = [uint16][SmokeWin32]::MapVirtualKey($vk, 0)
    $input = New-Object SmokeWin32+INPUT
    $input.type = [SmokeWin32]::INPUT_KEYBOARD
    $input.U.ki.wVk = $vk
    $input.U.ki.wScan = $scan
    $input.U.ki.dwFlags = [SmokeWin32]::KEYEVENTF_KEYUP
    if($KeyName -in @("UP", "DOWN", "LEFT", "RIGHT")) {
        $input.U.ki.dwFlags = $input.U.ki.dwFlags -bor [SmokeWin32]::KEYEVENTF_EXTENDEDKEY
    }
    [void][SmokeWin32]::SendInput(1, @($input), [Runtime.InteropServices.Marshal]::SizeOf([type][SmokeWin32+INPUT]))
    [SmokeWin32]::keybd_event($vk, 0, [SmokeWin32]::KEYEVENTF_KEYUP, [UIntPtr]::Zero)
}

function Send-KeyToWindow {
    param(
        [IntPtr]$Handle,
        [string]$KeyName,
        [int]$DelayMs = 700
    )

    Focus-Window -Handle $Handle
    Send-KeyDown -KeyName $KeyName
    Start-Sleep -Milliseconds 120
    Send-KeyUp -KeyName $KeyName
    Start-Sleep -Milliseconds $DelayMs
}

function Hold-KeysToWindow {
    param(
        [IntPtr]$Handle,
        [string[]]$Keys,
        [int]$HoldMs = 1000,
        [int]$DelayMs = 500
    )

    Focus-Window -Handle $Handle
    foreach($key in $Keys) {
        Send-KeyDown -KeyName $key
        Start-Sleep -Milliseconds 20
    }

    Start-Sleep -Milliseconds $HoldMs

    for($i = $Keys.Length - 1; $i -ge 0; $i--) {
        Send-KeyUp -KeyName $Keys[$i]
        Start-Sleep -Milliseconds 20
    }

    Start-Sleep -Milliseconds $DelayMs
}

function Move-CursorToWindowPoint {
    param(
        [IntPtr]$Handle,
        [double]$XRatio = 0.5,
        [double]$YRatio = 0.5,
        [int]$DelayMs = 300
    )

    Focus-Window -Handle $Handle
    $rect = Get-ClientRectScreen -Handle $Handle
    $x = [int]($rect.Left + (($rect.Right - $rect.Left) * $XRatio))
    $y = [int]($rect.Top + (($rect.Bottom - $rect.Top) * $YRatio))
    [SmokeWin32]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds $DelayMs
}

function Run-MovementStressSequence {
    param(
        [IntPtr]$Handle,
        [string]$ArtifactRoot
    )

    Hold-KeysToWindow -Handle $Handle -Keys @("W") -HoldMs 2200 -DelayMs 700
    Start-Sleep -Seconds 3
    Save-WindowCapture -Handle $Handle -Path (Join-Path $ArtifactRoot "03a-forward.png")

    Hold-KeysToWindow -Handle $Handle -Keys @("W","D") -HoldMs 2600 -DelayMs 700
    Start-Sleep -Seconds 4
    Save-WindowCapture -Handle $Handle -Path (Join-Path $ArtifactRoot "03b-forward-right.png")

    Hold-KeysToWindow -Handle $Handle -Keys @("W","A") -HoldMs 2600 -DelayMs 700
    Start-Sleep -Seconds 4
    Save-WindowCapture -Handle $Handle -Path (Join-Path $ArtifactRoot "03c-forward-left.png")

    Hold-KeysToWindow -Handle $Handle -Keys @("D") -HoldMs 1800 -DelayMs 700
    Start-Sleep -Seconds 3
    Save-WindowCapture -Handle $Handle -Path (Join-Path $ArtifactRoot "03d-strafe-right.png")

    Hold-KeysToWindow -Handle $Handle -Keys @("W","SPACE") -HoldMs 1700 -DelayMs 700
    Start-Sleep -Seconds 3
    Save-WindowCapture -Handle $Handle -Path (Join-Path $ArtifactRoot "03e-jump-forward.png")
}

function Run-AutonomousExploreSequence {
    param(
        [IntPtr]$Handle,
        [string]$ArtifactRoot,
        [int]$Segments = 8,
        [int]$HoldMs = 10000
    )

    $segments = [Math]::Max(1, $Segments)
    $holdMs = [Math]::Max(2000, $HoldMs)

    Prime-WindowInput -Handle $Handle
    Move-CursorToWindowPoint -Handle $Handle -XRatio 0.65 -YRatio 0.50 -DelayMs 250
    Hold-KeysToWindow -Handle $Handle -Keys @("D") -HoldMs 900 -DelayMs 250
    Hold-KeysToWindow -Handle $Handle -Keys @("W","SPACE") -HoldMs 1500 -DelayMs 400

    for($i = 0; $i -lt $segments; $i++) {
        $segmentIndex = $i + 1
        $keys = @("W")
        $segmentHoldMs = $holdMs
        $settleSeconds = 3
        $lookX = 0.55
        $lookY = 0.50

        switch($i % 6) {
            0 {
                $lookX = 0.72
                $keys = @("W","D","SPACE")
                $segmentHoldMs = [Math]::Max(2400, [int]($holdMs * 0.55))
            }
            1 {
                $lookX = 0.35
                $keys = @("W","A")
                $segmentHoldMs = [Math]::Max(2600, [int]($holdMs * 0.45))
            }
            2 {
                $lookX = 0.58
                $lookY = 0.46
                $keys = @("W","SPACE")
                $segmentHoldMs = [Math]::Max(2200, [int]($holdMs * 0.40))
            }
            3 {
                $lookX = 0.62
                $keys = @("W","D")
                $segmentHoldMs = [Math]::Max(2600, [int]($holdMs * 0.50))
            }
            4 {
                $lookX = 0.45
                $keys = @("W","A","SPACE")
                $segmentHoldMs = [Math]::Max(2200, [int]($holdMs * 0.45))
            }
            5 {
                $lookX = 0.55
                $keys = @("W")
                $segmentHoldMs = [Math]::Max(5000, [int]($holdMs * 1.1))
            }
        }

        Move-CursorToWindowPoint -Handle $Handle -XRatio $lookX -YRatio $lookY -DelayMs 220
        Hold-KeysToWindow -Handle $Handle -Keys $keys -HoldMs $segmentHoldMs -DelayMs 500
        Start-Sleep -Seconds $settleSeconds

        $captureName = "04-explore-{0:D2}.png" -f $segmentIndex
        Save-WindowCapture -Handle $Handle -Path (Join-Path $ArtifactRoot $captureName)
    }
}

function Select-ExistingWorld {
    param([IntPtr]$Handle)

    # World select consumes IB_GUI_CLICK and does not need pointer targeting:
    # clicking anywhere activates the highlighted row. Use a real mouse event
    # so the Dolphin dev click bridge is covered by smoke tests.
    Prime-WindowInput -Handle $Handle
    Click-Window -Handle $Handle -Button Left -XRatio 0.50 -YRatio 0.20 -DelayMs 350
    Click-Window -Handle $Handle -Button Left -XRatio 0.50 -YRatio 0.55 -DelayMs 350
    Send-KeyToWindow -Handle $Handle -KeyName "P" -DelayMs 350
    Send-KeyToWindow -Handle $Handle -KeyName "P" -DelayMs 700
}

function Select-CreateWorld {
    param([IntPtr]$Handle)

    Prime-WindowInput -Handle $Handle
    Send-KeyToWindow -Handle $Handle -KeyName "S" -DelayMs 250
    Send-KeyToWindow -Handle $Handle -KeyName "P" -DelayMs 350
    Send-KeyToWindow -Handle $Handle -KeyName "P" -DelayMs 700
    Click-Window -Handle $Handle -Button Left -XRatio 0.50 -YRatio 0.20 -DelayMs 350
    Click-Window -Handle $Handle -Button Left -XRatio 0.50 -YRatio 0.55 -DelayMs 700
}

function Click-Window {
    param(
        [IntPtr]$Handle,
        [ValidateSet("Left","Right","Middle","X1")]
        [string]$Button = "Left",
        [double]$XRatio = 0.5,
        [double]$YRatio = 0.5,
        [int]$DelayMs = 600
    )

    Focus-Window -Handle $Handle
    $rect = Get-ClientRectScreen -Handle $Handle
    $x = [int]($rect.Left + (($rect.Right - $rect.Left) * $XRatio))
    $y = [int]($rect.Top + (($rect.Bottom - $rect.Top) * $YRatio))

    [SmokeWin32]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 120

    if($Button -eq "Left") {
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
    } elseif($Button -eq "Right") {
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, [UIntPtr]::Zero)
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_RIGHTUP, 0, 0, 0, [UIntPtr]::Zero)
    } elseif($Button -eq "Middle") {
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, [UIntPtr]::Zero)
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_MIDDLEUP, 0, 0, 0, [UIntPtr]::Zero)
    } else {
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_XDOWN, 0, 0, [SmokeWin32]::XBUTTON1, [UIntPtr]::Zero)
        [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_XUP, 0, 0, [SmokeWin32]::XBUTTON1, [UIntPtr]::Zero)
    }

    Start-Sleep -Milliseconds $DelayMs
}

function Click-WindowChord {
    param(
        [IntPtr]$Handle,
        [double]$XRatio = 0.5,
        [double]$YRatio = 0.5,
        [int]$DelayMs = 700
    )

    Focus-Window -Handle $Handle
    $rect = Get-ClientRectScreen -Handle $Handle
    $x = [int]($rect.Left + (($rect.Right - $rect.Left) * $XRatio))
    $y = [int]($rect.Top + (($rect.Bottom - $rect.Top) * $YRatio))

    [SmokeWin32]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 120

    [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 180
    [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_RIGHTUP, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [SmokeWin32]::mouse_event([SmokeWin32]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)

    Start-Sleep -Milliseconds $DelayMs
}

function Get-WorldSaveSnapshot {
    param([string]$Root)

    $snapshot = @{}
    Get-ChildItem (Join-Path $Root "saves") -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $level = Join-Path $_.FullName "level.dat"
        if(Test-Path $level) {
            $snapshot[$_.Name] = (Get-Item $level).LastWriteTimeUtc
        }
    }
    return $snapshot
}

function Get-ExistingWorldCount {
    param([string]$Root)

    $count = 0
    Get-ChildItem (Join-Path $Root "saves") -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        if(Test-Path (Join-Path $_.FullName "level.dat")) {
            $count++
        }
    }

    return $count
}

function Get-DebugTracePath {
    param([string]$Root)

    return Join-Path $Root "saves\trace.log"
}

function Get-BenchmarkCsvPath {
    param([string]$Root)

    return Join-Path $Root "saves\benchmark-runtime.csv"
}

function Get-BenchmarkSummaryPath {
    param([string]$Root)

    return Join-Path $Root "saves\bench.txt"
}

function Get-DolphinSdRawPath {
    param([string]$UserRoot)

    if(-not $UserRoot) {
        return $null
    }

    return Join-Path $UserRoot "Load\WiiSD.raw"
}

function Extract-DolphinSdLogs {
    param(
        [string]$RawImagePath,
        [string]$ProjectRoot,
        [string]$ArtifactRoot
    )

    if(-not $RawImagePath -or -not (Test-Path $RawImagePath)) {
        return @()
    }

    $scriptPath = Join-Path $PSScriptRoot "extract_dolphin_sd_logs.py"
    if(-not (Test-Path $scriptPath)) {
        return @()
    }

    $saveDir = Join-Path $ProjectRoot "saves"
    $artifactExtractDir = Join-Path $ArtifactRoot "sd-logs"
    $appName = Split-Path $ProjectRoot -Leaf
    New-Item -ItemType Directory -Force -Path $saveDir | Out-Null
    New-Item -ItemType Directory -Force -Path $artifactExtractDir | Out-Null

    $json = & python $scriptPath --image $RawImagePath --app-name $appName --output-dir $saveDir
    if(-not $json) {
        return @()
    }

    $result = $json | ConvertFrom-Json
    foreach($entry in $result.extracted) {
        if(Test-Path $entry.path) {
            Copy-Item $entry.path $artifactExtractDir -Force
        }
    }

    return @($result.extracted)
}

function Get-BenchmarkSummary {
    param([string]$Path)

    if(-not (Test-Path $Path)) {
        return $null
    }

    $rows = Import-Csv -Path $Path
    if(-not $rows -or $rows.Count -eq 0) {
        return $null
    }

    $fps = @($rows | ForEach-Object { [double]$_.fps })
    $loaded = @($rows | ForEach-Object { [int]$_.loaded })
    $draw = @($rows | ForEach-Object { [int]$_.draw })
    $mesh = @($rows | ForEach-Object { [int]$_.mesh })
    $fluid = @($rows | ForEach-Object { [int]$_.fluid })
    $elapsed = @($rows | ForEach-Object { [double]$_.elapsed_s })

    return [PSCustomObject]@{
        Samples = $rows.Count
        ElapsedSeconds = [Math]::Round(($elapsed | Measure-Object -Maximum).Maximum, 2)
        FpsMin = [Math]::Round(($fps | Measure-Object -Minimum).Minimum, 2)
        FpsAvg = [Math]::Round(($fps | Measure-Object -Average).Average, 2)
        FpsMax = [Math]::Round(($fps | Measure-Object -Maximum).Maximum, 2)
        LoadedMax = ($loaded | Measure-Object -Maximum).Maximum
        DrawMax = ($draw | Measure-Object -Maximum).Maximum
        MeshMax = ($mesh | Measure-Object -Maximum).Maximum
        FluidMax = ($fluid | Measure-Object -Maximum).Maximum
    }
}

function Get-ImageHash {
    param([string]$Path)

    if(-not (Test-Path $Path)) {
        return $null
    }

    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash
}

function Wait-For-FrameChange {
    param(
        [IntPtr]$Handle,
        [string]$ArtifactRoot,
        [string[]]$ReferenceHashes,
        [string]$Prefix,
        [int]$Attempts = 12,
        [int]$SleepSeconds = 6
    )

    $hashes = @($ReferenceHashes | Where-Object { $_ })
    $lastPath = $null

    for($attempt = 1; $attempt -le $Attempts; $attempt++) {
        Start-Sleep -Seconds $SleepSeconds
        $capturePath = Join-Path $ArtifactRoot ("{0}-{1:D2}.png" -f $Prefix, $attempt)
        Save-WindowCapture -Handle $Handle -Path $capturePath
        $captureHash = Get-ImageHash -Path $capturePath
        $lastPath = $capturePath

        if($captureHash -and ($hashes -notcontains $captureHash)) {
            return $capturePath
        }
    }

    return $lastPath
}

function Get-PerfProbeCapture {
    param([string]$Path)

    if(-not (Test-Path $Path)) {
        return $null
    }

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $block = 3
        $gap = 1
        $cols = 8

        $readValue = {
            param([int]$OriginX, [int]$OriginY, [int]$Scale, [int]$Column, [int]$Row)
            $step = ($block + $gap) * $Scale
            $x = $OriginX + ($Column * $step) + [Math]::Floor(($block * $Scale) / 2)
            $y = $OriginY + ($Row * $step) + [Math]::Floor(($block * $Scale) / 2)
            if($x -lt 0 -or $y -lt 0 -or $x -ge $bitmap.Width -or $y -ge $bitmap.Height) {
                return -1
            }
            $pixel = $bitmap.GetPixel($x, $y)
            return ([int]$pixel.R -shl 16) -bor ([int]$pixel.G -shl 8) -bor [int]$pixel.B
        }

        $probeOrigin = $null
        for($scale = 1; $scale -le 8 -and -not $probeOrigin; $scale++) {
            $probeWidth = (($cols * $block) + (($cols - 1) * $gap)) * $scale
            $minX = [Math]::Max(0, $bitmap.Width - $probeWidth - (10 * $scale) - 120)
            $maxX = [Math]::Min($bitmap.Width - $probeWidth, $bitmap.Width - $probeWidth + 40)
            $minY = 0
            $maxY = [Math]::Min(120, $bitmap.Height - (($block + $gap) * 4 * $scale))

            for($originY = $minY; $originY -le $maxY -and -not $probeOrigin; $originY += [Math]::Max(1, $scale)) {
                for($originX = $minX; $originX -le $maxX; $originX += [Math]::Max(1, $scale)) {
                    if((& $readValue $originX $originY $scale 0 0) -eq 0x435552 `
                       -and (& $readValue $originX $originY $scale 0 1) -eq 0x42454E) {
                        $probeOrigin = [PSCustomObject]@{
                            X = $originX
                            Y = $originY
                            Scale = $scale
                        }
                        break
                    }
                }
            }
        }

        if(-not $probeOrigin) {
            return $null
        }

        $probe = [PSCustomObject]@{
            ProbeScale = $probeOrigin.Scale
            CurrentFps = [Math]::Round(((& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 1 0) / 10.0), 1)
            CurrentLoaded = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 2 0)
            CurrentDraw = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 3 0)
            CurrentMesh = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 4 0)
            CurrentFluid = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 5 0)
            CurrentEntities = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 6 0)
            CurrentHeapKiB = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 7 0)
            BenchFpsMin = [Math]::Round(((& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 1 1) / 10.0), 1)
            BenchFpsAvg = [Math]::Round(((& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 2 1) / 10.0), 1)
            BenchFpsMax = [Math]::Round(((& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 3 1) / 10.0), 1)
            BenchLoadedMax = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 4 1)
            BenchDrawMax = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 5 1)
            BenchMeshMax = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 6 1)
            BenchFluidMax = (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 7 1)
        }

        if((& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 0 2) -eq 0x49544D) {
            $probe | Add-Member -NotePropertyName HotbarSlot `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 1 2)
            $probe | Add-Member -NotePropertyName HeldItemId `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 2 2)
            $probe | Add-Member -NotePropertyName HeldItemCount `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 3 2)
            $probe | Add-Member -NotePropertyName StackVerifyActive `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 4 2)
            $probe | Add-Member -NotePropertyName StackVerifyStage `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 5 2)
            $probe | Add-Member -NotePropertyName StackVerifyPlacesSent `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 6 2)
            $probe | Add-Member -NotePropertyName StackVerifyResult `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 7 2)
        }

        if((& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 0 3) -eq 0x434F4E) {
            $probe | Add-Member -NotePropertyName ContainerSmokeInitialized `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 1 3)
            $probe | Add-Member -NotePropertyName ContainerSmokeActive `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 2 3)
            $probe | Add-Member -NotePropertyName ContainerSmokeStage `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 3 3)
            $probe | Add-Member -NotePropertyName ContainerSmokeResult `
                -NotePropertyValue (& $readValue $probeOrigin.X $probeOrigin.Y $probeOrigin.Scale 4 3)
        }

        return $probe
    }
    finally {
        $bitmap.Dispose()
    }
}

if(-not (Test-Path $DolphinExe)) {
    throw "Dolphin executable not found: $DolphinExe"
}

if(-not (Test-Path $DolPath)) {
    throw "Project DOL not found: $DolPath"
}

if($DolphinUserRoot) {
    $syncControlsScript = Join-Path $PSScriptRoot "sync_dolphin_controls.ps1"
    if(Test-Path $syncControlsScript) {
        & powershell -ExecutionPolicy Bypass -File $syncControlsScript -UserRoot $DolphinUserRoot | Out-Null
    }
}

Enable-AutonomousBindings -Root $ProjectRoot
if($RunContainerSmoke) {
    Enable-ContainerSmokeConfig -Root $ProjectRoot
}

$artifactBase = Join-Path $repoRoot "build\test-artifacts\dolphin-smoke"
New-Item -ItemType Directory -Force -Path $artifactBase | Out-Null
$artifactRoot = Join-Path $artifactBase (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

$beforeSaves = Get-WorldSaveSnapshot -Root $ProjectRoot
$existingWorldCount = Get-ExistingWorldCount -Root $ProjectRoot
$tracePath = Get-DebugTracePath -Root $ProjectRoot
$benchmarkPath = Get-BenchmarkCsvPath -Root $ProjectRoot
$benchmarkSummaryPath = Get-BenchmarkSummaryPath -Root $ProjectRoot
$sdRawPath = Get-DolphinSdRawPath -UserRoot $DolphinUserRoot
Remove-Item $tracePath -Force -ErrorAction SilentlyContinue
Remove-Item $benchmarkPath -Force -ErrorAction SilentlyContinue
Remove-Item $benchmarkSummaryPath -Force -ErrorAction SilentlyContinue

$startArgs = @()
if($DolphinUserRoot) {
    $startArgs += @("-u", $DolphinUserRoot)
}
$startArgs += @("-e", $DolPath)

$process = Start-Process -FilePath $DolphinExe -ArgumentList $startArgs -WorkingDirectory $ProjectRoot -PassThru

try {
    Wait-MainWindow -Process $process
    $warningWindow = $null
    Start-Sleep -Seconds 1
    $warningWindow = Find-WindowByTitle -Process $process -Title "Warning"
    if($warningWindow) {
        Save-WindowCapture -Handle $warningWindow.Handle -Path (Join-Path $artifactRoot "00-warning.png")
        Send-KeyToWindow -Handle $warningWindow.Handle -KeyName "ENTER" -DelayMs 600
        Start-Sleep -Milliseconds 300
    }
    $gameWindow = Wait-GameWindow -Process $process
    Start-Sleep -Seconds 1
    Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "00-early.png")
    Start-Sleep -Seconds 5
    Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "01-world-select.png")

    if(-not $SkipMenuInput) {
        if($existingWorldCount -gt 0) {
            Select-ExistingWorld -Handle $gameWindow.Handle
        } else {
            Select-CreateWorld -Handle $gameWindow.Handle
        }
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "01b-after-select.png")
    }
    Start-Sleep -Seconds 4
    $loadingCapturePath = Join-Path $artifactRoot "02-loading.png"
    Save-WindowCapture -Handle $gameWindow.Handle -Path $loadingCapturePath

    $worldSelectHash = Get-ImageHash -Path (Join-Path $artifactRoot "01-world-select.png")
    $loadingHash = Get-ImageHash -Path $loadingCapturePath
    $ingameCapturePath = Join-Path $artifactRoot "03-ingame.png"

    $transitionPath = Wait-For-FrameChange `
        -Handle $gameWindow.Handle `
        -ArtifactRoot $artifactRoot `
        -ReferenceHashes @($worldSelectHash, $loadingHash) `
        -Prefix "02b-transition" `
        -Attempts 12 `
        -SleepSeconds 6

    if($transitionPath -and (Test-Path $transitionPath)) {
        if($SkipMenuInput -or $RunAutonomousExplore) {
            Start-Sleep -Seconds 12
        } else {
            Start-Sleep -Seconds 3
        }
    } else {
        Start-Sleep -Seconds 20
    }

    Save-WindowCapture -Handle $gameWindow.Handle -Path $ingameCapturePath

    if($RunAutonomousExplore -or $RunMovementStress -or $RunStackVerify -or $RunPassiveObserve -or $RunContainerSmoke) {
        $readyTransitionPath = Wait-For-FrameChange `
            -Handle $gameWindow.Handle `
            -ArtifactRoot $artifactRoot `
            -ReferenceHashes @((Get-ImageHash -Path $ingameCapturePath)) `
            -Prefix "03b-ready" `
            -Attempts 8 `
            -SleepSeconds 3

        if($readyTransitionPath -and (Test-Path $readyTransitionPath)) {
            Start-Sleep -Seconds 2
            Save-WindowCapture -Handle $gameWindow.Handle -Path $ingameCapturePath
        }
    }

    if($RunAutonomousExplore) {
        Prime-WindowInput -Handle $gameWindow.Handle
        Run-AutonomousExploreSequence -Handle $gameWindow.Handle `
            -ArtifactRoot $artifactRoot `
            -Segments $ExploreSegments `
            -HoldMs $ExploreHoldMs
        Start-Sleep -Seconds 4
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "05-explore-end.png")
    } elseif($RunPassiveObserve) {
        Start-Sleep -Seconds 8
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "04-observe-01.png")
        Start-Sleep -Seconds 8
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "05-observe-02.png")
    } elseif($RunMovementStress) {
        Run-MovementStressSequence -Handle $gameWindow.Handle -ArtifactRoot $artifactRoot
        Start-Sleep -Seconds 4
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "04-stress-end.png")
    } elseif($RunStackVerify) {
        Start-Sleep -Seconds 6
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "04-stack-verify.png")
    } elseif($RunContainerSmoke) {
        Start-Sleep -Seconds 8
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "04-container-smoke.png")
        Start-Sleep -Seconds 6
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "05-container-smoke-final.png")
    } else {
        # Basic in-game smoke: inventory open/close, action clicks, pause, save+quit.
        Click-Window -Handle $gameWindow.Handle -Button Left -XRatio 0.55 -YRatio 0.55
        Click-Window -Handle $gameWindow.Handle -Button Right -XRatio 0.55 -YRatio 0.55
        Click-Window -Handle $gameWindow.Handle -Button Middle -XRatio 0.55 -YRatio 0.55 -DelayMs 1000
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "04-inventory.png")
        Click-Window -Handle $gameWindow.Handle -Button Middle -XRatio 0.55 -YRatio 0.55 -DelayMs 800
        Click-Window -Handle $gameWindow.Handle -Button X1 -XRatio 0.55 -YRatio 0.55 -DelayMs 1000
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "05-pause.png")
        Click-Window -Handle $gameWindow.Handle -Button Middle -XRatio 0.55 -YRatio 0.55 -DelayMs 2000

        Start-Sleep -Seconds 5
        Save-WindowCapture -Handle $gameWindow.Handle -Path (Join-Path $artifactRoot "06-returned-to-select.png")
    }
}
finally {
    if(-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}

$extractedLogs = Extract-DolphinSdLogs -RawImagePath $sdRawPath -ProjectRoot $ProjectRoot -ArtifactRoot $artifactRoot

$afterSaves = Get-WorldSaveSnapshot -Root $ProjectRoot
$updated = @()
foreach($entry in $afterSaves.GetEnumerator()) {
    if(-not $beforeSaves.ContainsKey($entry.Key) -or $beforeSaves[$entry.Key] -ne $entry.Value) {
        $updated += [PSCustomObject]@{
            World = $entry.Key
            BeforeUtc = if($beforeSaves.ContainsKey($entry.Key)) { $beforeSaves[$entry.Key] } else { $null }
            AfterUtc = $entry.Value
        }
    }
}

$report = [PSCustomObject]@{
    ArtifactRoot = $artifactRoot
    DolphinExe = $DolphinExe
    DolphinUserRoot = $DolphinUserRoot
    DolPath = $DolPath
    SdRawPath = $sdRawPath
    DebugTracePath = $tracePath
    DebugTraceExists = (Test-Path $tracePath)
    BenchmarkCsvPath = $benchmarkPath
    BenchmarkCsvExists = (Test-Path $benchmarkPath)
    BenchmarkSummaryPath = $benchmarkSummaryPath
    BenchmarkSummaryExists = (Test-Path $benchmarkSummaryPath)
    BenchmarkSummary = Get-BenchmarkSummary -Path $benchmarkPath
    ContainerSmokeRequested = [bool]$RunContainerSmoke
    ExtractedLogs = $extractedLogs
    PerfProbeCaptures = Get-ChildItem $artifactRoot -Filter *.png |
        Sort-Object Name |
        ForEach-Object {
            $probe = Get-PerfProbeCapture -Path $_.FullName
            if($probe) {
                [PSCustomObject]@{
                    Capture = $_.Name
                    Metrics = $probe
                }
            }
        }
    WarningDialogSeen = (Test-Path (Join-Path $artifactRoot "00-warning.png"))
    UpdatedWorlds = $updated
    Captures = Get-ChildItem $artifactRoot -Filter *.png | Select-Object Name, Length, LastWriteTime
}

$report | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $artifactRoot "report.json")
$report | ConvertTo-Json -Depth 4
