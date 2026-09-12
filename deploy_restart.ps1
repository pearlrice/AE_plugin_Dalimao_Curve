# Build + deploy DalimaoCurves, restarting After Effects with the test project.
#
# If After Effects is running with the test project under the project folder,
# close it first (so the plugin file is not locked), deploy, then reopen the
# test project. If AE is running with anything else, abort without touching it.

$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$aeSupportDir = if ($env:AE_SUPPORT_DIR) { $env:AE_SUPPORT_DIR } else {
    Join-Path $env:ProgramFiles 'Adobe\Adobe After Effects 2025\Support Files'
}
$aeExe       = Join-Path $aeSupportDir 'AfterFX.exe'
$testProject = Join-Path $projectRoot 'test project.aep'

if (-not (Test-Path $aeExe)) {
    Write-Host "ERROR: After Effects executable not found: $aeExe"
    exit 1
}
if (-not (Test-Path $testProject)) {
    Write-Host "ERROR: test project not found: $testProject"
    exit 1
}

Add-Type -AssemblyName UIAutomationClient -ErrorAction SilentlyContinue
Add-Type -AssemblyName UIAutomationTypes -ErrorAction SilentlyContinue

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class AeDialog {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    delegate bool EnumProc(IntPtr h, IntPtr lp);

    // The main window title contains the project path; the save prompt is a
    // separate visible top-level window.
    public static string FindDialog(uint target, out IntPtr dlg, out IntPtr main) {
        IntPtr mainFound = IntPtr.Zero;
        IntPtr dlgFound = IntPtr.Zero;
        string dlgTitle = "";
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target && IsWindowVisible(h)) {
                var sb = new StringBuilder(512);
                GetWindowText(h, sb, 512);
                string t = sb.ToString();
                if (t.IndexOf(".aep", StringComparison.OrdinalIgnoreCase) >= 0) {
                    mainFound = h;
                } else if (t.IndexOf("After Effects", StringComparison.OrdinalIgnoreCase) >= 0 ||
                           t.IndexOf("Save", StringComparison.OrdinalIgnoreCase) >= 0 ||
                           t.IndexOf("保存", StringComparison.OrdinalIgnoreCase) >= 0) {
                    dlgFound = h;
                    dlgTitle = t;
                }
            }
            return true;
        }, IntPtr.Zero);
        dlg = dlgFound;
        main = mainFound;
        return dlgTitle;
    }
    public static string TopTitles(uint target) {
        var sb = new System.Collections.Generic.List<string>();
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target && IsWindowVisible(h)) {
                var t = new StringBuilder(512);
                GetWindowText(h, t, 512);
                if (t.Length > 0) sb.Add(t.ToString());
            }
            return true;
        }, IntPtr.Zero);
        return string.Join(" | ", sb.ToArray());
    }
    public static void Activate(IntPtr h) {
        SetForegroundWindow(h);
    }
    public static IntPtr Foreground() {
        return GetForegroundWindow();
    }
}
'@

function Click-SaveButton([IntPtr]$hwnd, [string]$title) {
    # Try UI Automation first (handles both English "Save" and Chinese "保存").
    try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
        $cond = New-Object System.Windows.Automation.AndCondition(
            (New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::Button)),
            (New-Object System.Windows.Automation.OrCondition(
                (New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::NameProperty, '保存')),
                (New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::NameProperty, 'Save')))))
        $btn = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
        if ($btn) {
            $inv = $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
            $inv.Invoke()
            return $true
        }
        $names = Get-ButtonNames $hwnd
        Write-Host "  (no Save button found; dialog buttons: $names)"
    } catch {}

    # Fallback: bring the dialog to the foreground and VERIFY it is foreground
    # before sending any keys; otherwise abort (never send keys elsewhere).
    $fg = [AeDialog]::Foreground()
    if ($fg -ne $hwnd) {
        [AeDialog]::Activate($hwnd)
        Start-Sleep -Milliseconds 600
        $fg = [AeDialog]::Foreground()
    }
    if ($fg -ne $hwnd) {
        try {
            $wshell = New-Object -ComObject WScript.Shell
            $null = $wshell.AppActivate($title)
            Start-Sleep -Milliseconds 600
            $fg = [AeDialog]::Foreground()
        } catch {}
    }
    if ($fg -ne $hwnd) {
        Write-Host '  Save dialog is not in the foreground; not sending any keys.'
        Write-Host '  Please close After Effects manually and choose Save, then re-run the script.'
        return $false
    }

    # The dialog is confirmed foreground: send a real Alt+S (Save mnemonic).
    try {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public class SaveKeys {
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public int type; public InputUnion U; }
    [StructLayout(LayoutKind.Explicit)] public struct InputUnion {
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    public static void AltS() {
        INPUT[] seq = new INPUT[4];
        seq[0].type = 1; seq[0].U.ki = new KEYBDINPUT(); seq[0].U.ki.wVk = 0x12;                 // Alt down
        seq[1].type = 1; seq[1].U.ki = new KEYBDINPUT(); seq[1].U.ki.wVk = 0x53;                 // S down
        seq[2].type = 1; seq[2].U.ki = new KEYBDINPUT(); seq[2].U.ki.wVk = 0x53; seq[2].U.ki.dwFlags = 2; // S up
        seq[3].type = 1; seq[3].U.ki = new KEYBDINPUT(); seq[3].U.ki.wVk = 0x12; seq[3].U.ki.dwFlags = 2; // Alt up
        SendInput(4, seq, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
        [SaveKeys]::AltS()
        return $true
    } catch {
        Write-Host "  (Alt+S failed: $($_.Exception.Message))"
        return $false
    }
}

function Get-ButtonNames([IntPtr]$hwnd) {
    try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)
        $btns = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
        $names = @()
        foreach ($b in $btns) { $names += $b.Current.Name }
        return ($names -join ' | ')
    } catch { return '' }
}

function Close-AfterEffects($proc) {
    $null = $proc.CloseMainWindow()
    $deadline = (Get-Date).AddSeconds(25)
    $handledDialog = $false
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        Start-Sleep -Milliseconds 400
        $proc.Refresh()
        if ($proc.HasExited) { break }
        if (-not $handledDialog) {
            $dummyMain = [IntPtr]::Zero
            $dlg = [IntPtr]::Zero
            $dlgTitle = [AeDialog]::FindDialog([uint32]$proc.Id, [ref]$dlg, [ref]$dummyMain)
            if ($dlg -ne [IntPtr]::Zero) {
                Write-Host "Save prompt detected ('$dlgTitle'); clicking Save..."
                $handledDialog = $true
                $null = Click-SaveButton $dlg $dlgTitle
            }
        }
    }
    if (-not $proc.HasExited) {
        if ($handledDialog) {
            Write-Host 'The save prompt was handled but After Effects is still running; leaving it open.'
        } else {
            Write-Host 'No save prompt was found; leaving After Effects open to protect your work.'
        }
        Write-Host 'Please close After Effects manually (choose Save), then re-run the script.'
        exit 3
    }
}

$proc = Get-Process -Name 'AfterFX' -ErrorAction SilentlyContinue | Select-Object -First 1

if ($proc) {
    $titles = [AeDialog]::TopTitles([uint32]$proc.Id)
    if ($titles -and $titles -notlike '*test project.aep*' -and $titles -like '*.aep*') {
        Write-Host "After Effects is running with a different project, leaving it alone:`n  $titles"
        exit 2
    }
    if (-not $titles -or $titles -notlike '*test project.aep*') {
        Write-Host "After Effects is running but the test project is not open; leaving it alone.`n  $titles"
        exit 2
    }
    Write-Host 'Closing After Effects (test project)...'
    Close-AfterEffects $proc
    Write-Host 'After Effects closed.'
} else {
    Write-Host 'After Effects is not running.'
}

Push-Location $projectRoot
try {
    py -3 deploy.py --build
    if ($LASTEXITCODE -ne 0) {
        throw 'deploy.py failed'
    }
} finally {
    Pop-Location
}

Write-Host 'Reopening the test project in After Effects...'
Start-Process -FilePath $aeExe -ArgumentList "`"$testProject`""
Write-Host 'Done.'
