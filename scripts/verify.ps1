# Prove that the shell's windows live on an isolated desktop.
#
# Run this from the DSH shell (the `pwsh` tool). It reports the desktop the
# shell runs on, starts a GUI process, and enumerates windows for that process
# on both the shell's desktop and the user's `Default` desktop. Expected result
# in the isolated modes: the window exists on the shell's desktop and NOT on
# Default.

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$signature = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class DeskProbe
{
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern IntPtr GetThreadDesktop(uint threadId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetUserObjectInformation(IntPtr hObj, int nIndex, StringBuilder info, int length, out int needed);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr OpenDesktop(string name, int flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll")] public static extern bool EnumDesktopWindows(IntPtr desktop, EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    public delegate bool EnumProc(IntPtr window, IntPtr parameter);

    public static string Name(IntPtr handle)
    {
        if (handle == IntPtr.Zero) return "?";
        var buffer = new StringBuilder(512);
        int needed;
        return GetUserObjectInformation(handle, 2, buffer, buffer.Capacity * 2, out needed) ? buffer.ToString() : "?";
    }

    public static string CurrentDesktop()
    {
        return Name(GetThreadDesktop(GetCurrentThreadId()));
    }

    public static List<string> WindowsForProcess(IntPtr desktop, uint processId)
    {
        var found = new List<string>();
        EnumDesktopWindows(desktop, delegate(IntPtr window, IntPtr parameter) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == processId) {
                var text = new StringBuilder(512);
                GetWindowText(window, text, text.Capacity);
                found.Add(window.ToString() + "|" + text.ToString());
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

Add-Type -TypeDefinition $signature -Language CSharp

$shellDesktop = [DeskProbe]::CurrentDesktop()
Write-Output ("SHELL_DESKTOP=" + $shellDesktop)
Write-Output ("DSH_ISOLATED_DESKTOP=" + $env:DSH_ISOLATED_DESKTOP)
Write-Output ("DSH_ISOLATION_MODE=" + $env:DSH_ISOLATION_MODE)

$notepad = $null
try {
    # A classic Win32 GUI app: Windows 11 Notepad is a packaged app and does
    # not create a window on a hidden desktop, so it cannot prove isolation.
    $notepad = Start-Process -FilePath 'winver.exe' -PassThru
    $processId = [uint32]$notepad.Id

    # The shell's own desktop: GetThreadDesktop covers the current one.
    $threadDesktop = [DeskProbe]::GetThreadDesktop([DeskProbe]::GetCurrentThreadId())
    $onShell = @()
    for ($attempt = 0; $attempt -lt 50 -and $onShell.Count -eq 0; $attempt++) {
        Start-Sleep -Milliseconds 100
        $onShell = [DeskProbe]::WindowsForProcess($threadDesktop, $processId)
    }

    # The user's visible desktop. DESKTOP_ENUMERATE = 0x0001.
    $defaultHandle = [DeskProbe]::OpenDesktop('Default', 0, $false, 0x0001)
    if ($defaultHandle -eq [IntPtr]::Zero) {
        Write-Output 'DEFAULT_ENUMERATE=unavailable'
        $onDefault = @()
    } else {
        try { $onDefault = [DeskProbe]::WindowsForProcess($defaultHandle, $processId) }
        finally { [DeskProbe]::CloseDesktop($defaultHandle) | Out-Null }
    }

    Write-Output ("NOTEPAD_PID=" + $processId)
    Write-Output ("WINDOWS_ON_SHELL_DESKTOP=" + $onShell.Count)
    Write-Output ("WINDOWS_ON_DEFAULT=" + $onDefault.Count)
    foreach ($line in $onShell) { Write-Output ("  shell: " + $line) }
    foreach ($line in $onDefault) { Write-Output ("  default: " + $line) }

    if ($shellDesktop -eq 'Default') {
        Write-Output 'VERDICT=NOT_ISOLATED (the shell runs on the user desktop)'
    } elseif ($onShell.Count -gt 0 -and $onDefault.Count -eq 0) {
        Write-Output 'VERDICT=PASS (window only on the isolated desktop)'
    } else {
        Write-Output 'VERDICT=CHECK (see the counts above)'
    }
} finally {
    if ($notepad -ne $null) {
        try { Stop-Process -Id $notepad.Id -Force -ErrorAction SilentlyContinue } catch {}
    }
}
