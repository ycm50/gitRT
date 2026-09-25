# 一次性诊断：AI 设置窗口的「模型」下拉框展开后，下拉列表窗口到底存不存在、多大
param([int]$Port = 18090, [string]$Exe = "$env:TEMP\GitRT-run\GitRT.exe")
$ErrorActionPreference = 'Continue'
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;using System.Collections.Generic;
public class Diag {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMS(IntPtr h, uint m, IntPtr w, string l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMGetText(IntPtr h, uint m, IntPtr cap, StringBuilder b);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 public static List<string> Dump(IntPtr combo, uint comboPid) {
   var list = new List<string>();
   EnumWindows((h, p) => {
     uint pid; GetWindowThreadProcessId(h, out pid);
     if (pid != comboPid) return true;
     var cls = new StringBuilder(128); GetClassName(h, cls, 128);
     var r = new RECT(); GetWindowRect(h, out r);
     IntPtr owner = GetWindow(h, 4);
     bool isChild = GetWindow(h, 3) != IntPtr.Zero;
     list.Add(string.Format("{0}|owner={1}|vis={2}|rect={3},{4},{5},{6}|child={7}",
        cls.ToString(), owner == combo ? "combo" : owner.ToInt64().ToString(), IsWindowVisible(h),
        r.L, r.T, r.R, r.B, isChild));
     return true;
   }, IntPtr.Zero);
   return list;
 }
}
"@
if (-not (Test-Path "$PSScriptRoot\mock-ai-server.ps1")) { Write-Host "缺少 mock 服务脚本"; exit 1 }
$mock = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList `
    "-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\mock-ai-server.ps1`" -Port $Port -LogFile `"$env:TEMP\GitRT-test\mock-diag.log`""
for ($i = 0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 250; try { if ((Invoke-WebRequest "http://127.0.0.1:$Port/health" -TimeoutSec 2 -UseBasicParsing).StatusCode -eq 200) { break } } catch {} }

$env:GITRT_AI_ENDPOINT = "http://127.0.0.1:$Port/v1/chat/completions"
$env:GITRT_AI_MODEL = 'mock-model'
$env:GITRT_AI_API_KEY_ENV = 'GITRT_DIAG_KEY'
$env:GITRT_DIAG_KEY = 'diag-key'
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
$app = Start-Process $Exe -ArgumentList "`"$env:TEMP\GitRT-test\repo`"" -PassThru
Start-Sleep -Seconds 3
$main = [Diag]::FindWindow('GitRT.MainWindow', $null)
[void][Diag]::SM($main, 0x0111, [IntPtr]706, [IntPtr]::Zero)
Start-Sleep -Seconds 1
$set = [Diag]::FindWindow('GitRT.SettingsWindow', $null)
if ($set -eq [IntPtr]::Zero) { Write-Host "设置窗口没出现"; exit 1 }
$combo = [Diag]::GetDlgItem($set, 1001)
$r = New-Object Diag+RECT; [void][Diag]::GetWindowRect($combo, [ref]$r)
Write-Host "组合框句柄=$combo  rect=$($r.L),$($r.T),$($r.R),$($r.B)  高=$($r.B - $r.T)"
Write-Host "行高 CB_GETITEMHEIGHT=" ([int][Diag]::SM($combo, 0x0154, [IntPtr]0, [IntPtr]::Zero))
Write-Host "条目数（展开前）=" ([int][Diag]::SM($combo, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero))
Write-Host "--- 展开 ---"
[void][Diag]::SM($combo, 0x014F, [IntPtr]1, [IntPtr]::Zero)
Start-Sleep -Milliseconds 800
$pid2 = 0; [void][Diag]::GetWindowThreadProcessId($combo, [ref]$pid2)
Write-Host "CB_GETDROPPEDSTATE=" ([int][Diag]::SM($combo, 0x0157, [IntPtr]::Zero, [IntPtr]::Zero))
Write-Host "CB_GETCOUNT=" ([int][Diag]::SM($combo, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero))
$r2 = New-Object Diag+RECT; [void][Diag]::GetWindowRect($combo, [ref]$r2)
Write-Host "展开后组合框 rect=$($r2.L),$($r2.T),$($r2.R),$($r2.B)  高=$($r2.B - $r2.T)"
Write-Host "--- 同进程窗口清单（含 owner=combo 的下拉列表）---"
foreach ($line in [Diag]::Dump($combo, $pid2)) { Write-Host "  $line" }
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue
