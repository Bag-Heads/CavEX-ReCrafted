param(
    [string]$ProcessName = "Dolphin",
    [int]$StartupGraceSeconds = 60
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class DolphinWheelHotbar
{
    public delegate IntPtr LowLevelMouseProc(int nCode, IntPtr wParam, IntPtr lParam);

    public const int WH_MOUSE_LL = 14;
    public const int WM_LBUTTONDOWN = 0x0201;
    public const int WM_LBUTTONUP = 0x0202;
    public const int WM_RBUTTONDOWN = 0x0204;
    public const int WM_RBUTTONUP = 0x0205;
    public const int WM_MOUSEWHEEL = 0x020A;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const byte VK_1 = 0x31;
    public const byte VK_2 = 0x32;
    public const byte VK_F = 0x46;
    public const byte VK_G = 0x47;

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT
    {
        public int x;
        public int y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MSLLHOOKSTRUCT
    {
        public POINT pt;
        public uint mouseData;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SetWindowsHookEx(int idHook, LowLevelMouseProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    public static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern IntPtr GetModuleHandle(string lpModuleName);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);

    private static IntPtr _hook = IntPtr.Zero;
    private static readonly LowLevelMouseProc _proc = HookCallback;
    public static string TargetProcessName = "Dolphin";

    public static void Install()
    {
        using (Process curProcess = Process.GetCurrentProcess())
        using (ProcessModule curModule = curProcess.MainModule)
        {
            _hook = SetWindowsHookEx(WH_MOUSE_LL, _proc, GetModuleHandle(curModule.ModuleName), 0);
            if (_hook == IntPtr.Zero)
                throw new System.ComponentModel.Win32Exception();
        }
    }

    public static void Uninstall()
    {
        if (_hook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }
    }

    private static bool IsTargetForeground()
    {
        IntPtr hWnd = GetForegroundWindow();
        if (hWnd == IntPtr.Zero)
            return false;

        uint pid;
        GetWindowThreadProcessId(hWnd, out pid);
        if (pid == 0)
            return false;

        try
        {
            Process process = Process.GetProcessById((int)pid);
            return string.Equals(process.ProcessName, TargetProcessName, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private static void SendVirtualKeyTap(byte vk)
    {
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        keybd_event(vk, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    private static void SendVirtualKeyState(byte vk, bool down)
    {
        keybd_event(vk, 0, down ? 0 : KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    private static IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0 && IsTargetForeground())
        {
            if (wParam == (IntPtr)WM_MOUSEWHEEL)
            {
                MSLLHOOKSTRUCT hookStruct = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
                short delta = unchecked((short)((hookStruct.mouseData >> 16) & 0xffff));
                if (delta > 0)
                    SendVirtualKeyTap(VK_1);
                else if (delta < 0)
                    SendVirtualKeyTap(VK_2);
            }
            else if (wParam == (IntPtr)WM_LBUTTONDOWN)
            {
                SendVirtualKeyState(VK_F, true);
            }
            else if (wParam == (IntPtr)WM_LBUTTONUP)
            {
                SendVirtualKeyState(VK_F, false);
            }
            else if (wParam == (IntPtr)WM_RBUTTONDOWN)
            {
                SendVirtualKeyState(VK_G, true);
            }
            else if (wParam == (IntPtr)WM_RBUTTONUP)
            {
                SendVirtualKeyState(VK_G, false);
            }
        }

        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }
}
"@

[DolphinWheelHotbar]::TargetProcessName = $ProcessName
[DolphinWheelHotbar]::Install()

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 500
$startedAt = Get-Date
$hasSeenProcess = $false
$timer.Add_Tick({
    $processExists = [bool](Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    if($processExists) {
        $script:hasSeenProcess = $true
        return
    }

    $startupExpired = ((Get-Date) - $script:startedAt).TotalSeconds -ge $StartupGraceSeconds
    if($script:hasSeenProcess -or $startupExpired) {
        $timer.Stop()
        [System.Windows.Forms.Application]::ExitThread()
    }
})
$timer.Start()

try {
    [System.Windows.Forms.Application]::Run()
} finally {
    [DolphinWheelHotbar]::Uninstall()
}
