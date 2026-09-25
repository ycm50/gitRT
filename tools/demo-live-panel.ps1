# ---------------------------------------------------------------------------
# 参数面板"自动显示 + 就地输出"端到端（真开窗口、真点按钮）
#
#   A) 查看类命令（提交历史）：选中后**不用点执行**，面板里就直接显示内容；
#      随后仓库发生变化，面板在 ~2.5 秒内**自动刷新**出来（新提交出现）。
#   B) 执行类命令（撤销上次提交）：点执行 → 输出直接显示在"将执行的命令"下面的
#      「运行结果」框里（> git … + 输出 + 完成汇总），**不再另开进度窗口**；
#      并且主窗口状态栏跟着刷新。
#
# 用法: pwsh -File tools\demo-live-panel.ps1 [-Exe <GitRT.exe>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$RepoDir = "$PSScriptRoot\..\build\pipe-lab\repo",
    [string]$OutDir = "$PSScriptRoot\..\docs\images"
)
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public class LP {
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMGetText(IntPtr h, uint m, IntPtr cap, StringBuilder b);
 [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 public static IntPtr TopByClass(uint pid, string cls) {
   IntPtr f = IntPtr.Zero;
   EnumWindows((h,x) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p==pid) { var c=new StringBuilder(128); GetClassName(h,c,128); if (c.ToString()==cls) { f=h; return false; } } return true; }, IntPtr.Zero);
   return f;
 }
 public static List<string> TopClasses(uint pid) {
   var l = new List<string>();
   EnumWindows((h,x) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p==pid) { var c=new StringBuilder(128); GetClassName(h,c,128); l.Add(c.ToString()); } return true; }, IntPtr.Zero);
   return l;
 }
 public static string ChildTexts(IntPtr parent) {
   var sb = new StringBuilder();
   EnumChildWindows(parent, (h,x) => { var c=new StringBuilder(64); GetClassName(h,c,64);
     if (c.ToString().StartsWith("Static")) { var t=new StringBuilder(200); GetWindowText(h,t,200);
       sb.Append(t.ToString()); sb.Append(" | "); } return true; }, IntPtr.Zero);
   return sb.ToString();
 }
}
"@
function TextOf($h) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    $len = [LP]::GetWindowTextLength($h)
    $cap = [Math]::Max(4096, $len + 512)
    $sb = New-Object System.Text.StringBuilder $cap
    [void][LP]::SMGetText($h, 0x000D, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Git($dir, [string[]]$a) { $o = & git.exe -C $dir @a 2>&1; return ($o | Out-String).Trim() }
function Shot($hwnd, $path) {
    $r = New-Object LP+RECT; [void][LP]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][LP]::PrintWindow($hwnd, $dc, 2); $g.ReleaseHdc($dc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}
function SelectCommand($main, $list, $pattern) {
    $n = [int][LP]::SM($list, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($i = 0; $i -lt $n; $i++) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][LP]::SMGetText($list, 0x0189, [IntPtr]$i, $sb)
        if ($sb.ToString() -match $pattern) {
            [void][LP]::SM($list, 0x0186, [IntPtr]$i, [IntPtr]::Zero)
            [void][LP]::SM($main, 0x0111, [IntPtr]((1 -shl 16) -bor 703), [IntPtr]::Zero)
            return $i
        }
    }
    return -1
}
function WaitPanel($main) {
    for ($w = 0; $w -lt 20; $w++) {
        Start-Sleep -Milliseconds 200
        $p = [LP]::FindWindowEx($main, [IntPtr]::Zero, 'GitRT.ParamPanel', $null)
        if ($p -ne [IntPtr]::Zero) { return $p }
    }
    return [IntPtr]::Zero
}
$IDC_PP_EXEC = 1001
$IDC_PP_OUTPUT = 1003
$IDC_PP_PREVIEW = 4000 + 500
$BM_CLICK = 0x00F5

# ------------------------------------------------------------------ 仓库
$RepoDir = [System.IO.Path]::GetFullPath($RepoDir)
if (Test-Path (Split-Path $RepoDir -Parent)) { Remove-Item (Split-Path $RepoDir -Parent) -Recurse -Force }
New-Item -ItemType Directory -Force -Path $RepoDir | Out-Null
Git $RepoDir @('init','-q','-b','main') | Out-Null
Git $RepoDir @('config','user.name','GitRT Demo') | Out-Null
Git $RepoDir @('config','user.email','demo@e.invalid') | Out-Null
Git $RepoDir @('config','commit.gpgsign','false') | Out-Null
1..2 | ForEach-Object { Set-Content "$RepoDir\f$_.txt" "c$_" -Encoding UTF8; Git $RepoDir @('add','-A') | Out-Null; Git $RepoDir @('commit','-q','-m',"初始提交$_") | Out-Null }
$top = (Git $RepoDir @('rev-parse','--show-toplevel')) -replace '\\','/'
Check "测试仓库就绪" ($top -eq ($RepoDir -replace '\\','/')) "top=$top"

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$app = Start-Process $Exe -ArgumentList "`"$RepoDir`"" -PassThru
Start-Sleep -Seconds 3
$main = if ($app) { [LP]::TopByClass([uint32]$app.Id, 'GitRT.MainWindow') } else { [IntPtr]::Zero }
Check "主窗口出现" ($main -ne [IntPtr]::Zero) ''
$list = [LP]::GetDlgItem($main, 703)

# ------------------------------------------- A) 查看类命令：自动显示
Write-Host "-- A) 提交历史：选中即显示（不点执行）--" -ForegroundColor Cyan
$idx = SelectCommand $main $list '提交历史'
Check "命令列表里找到「提交历史」" ($idx -ge 0) ''
$panel = WaitPanel $main
Check "参数面板出现" ($panel -ne [IntPtr]::Zero) ''
$preview = [LP]::GetDlgItem($panel, $IDC_PP_PREVIEW)
$shown = ''
for ($w = 0; $w -lt 25; $w++) {
    Start-Sleep -Milliseconds 200
    $shown = TextOf $preview
    if ($shown -match '初始提交1') { break }
}
Check "面板自动显示了提交内容（未点执行）" ($shown -match '初始提交1' -and $shown -match '初始提交2') "preview=[$shown]"
Check "预览标签改成「内容（自动刷新）」" (([LP]::ChildTexts($panel)) -match '内容（自动刷新）') "labels=$([LP]::ChildTexts($panel))"

Write-Host "-- A2) 仓库变化 → 面板自动刷新 --" -ForegroundColor Cyan
Set-Content "$RepoDir\f3.txt" 'c3' -Encoding UTF8
Git $RepoDir @('add','-A') | Out-Null
Git $RepoDir @('commit','-q','-m','第三个提交') | Out-Null
$refreshed = $false
for ($w = 0; $w -lt 30; $w++) {
    Start-Sleep -Milliseconds 300
    if ((TextOf $preview) -match '第三个提交') { $refreshed = $true; break }
}
Check "新提交在 ~2.5 秒内自动出现" $refreshed "preview=[$(TextOf $preview)]"

# --------------------------------- B) 执行类命令：就地输出
Write-Host "-- B) 撤销上次提交：输出就地显示 --" -ForegroundColor Cyan
$before = [int](Git $RepoDir @('rev-list','--count','HEAD'))
$idx2 = SelectCommand $main $list '撤销上次提交'
Check "命令列表里找到「撤销上次提交」" ($idx2 -ge 0) ''
$panel2 = WaitPanel $main
$exec = [LP]::GetDlgItem($panel2, $IDC_PP_EXEC)
for ($w = 0; $w -lt 15 -and -not [LP]::IsWindowEnabled($exec); $w++) { Start-Sleep -Milliseconds 200 }
Check "「执行」按钮就位" ($exec -ne [IntPtr]::Zero -and [LP]::IsWindowEnabled($exec)) ''
$output = [LP]::GetDlgItem($panel2, $IDC_PP_OUTPUT)
Check "面板里有「运行结果」框" ($output -ne [IntPtr]::Zero) ''
# 危险命令：第一次点只是 arm，第二次才真执行
[void][LP]::SM($exec, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 400
[void][LP]::SM($exec, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
$out = ''
for ($w = 0; $w -lt 40; $w++) {
    Start-Sleep -Milliseconds 300
    $out = TextOf $output
    if ($out -match '---') { break }
}
Write-Host "  运行结果：`n$(($out -split "`n" | Select-Object -First 6) -join "`n")"
Check "运行结果里有真实命令行（git reset --soft）" ($out -match 'git reset --soft') "out=[$out]"
Check "运行结果里有完成汇总（---）" ($out -match '---') "out=[$out]"
Check "提交数 $before → $($before-1)（命令真的跑了）" ([int](Git $RepoDir @('rev-list','--count','HEAD')) -eq $before - 1) "count=$([int](Git $RepoDir @('rev-list','--count','HEAD')))"
$classes = [LP]::TopClasses([uint32]$app.Id)
Check "没有另开进度窗口（就地显示）" (-not ($classes -match 'ProgressWindow')) "windows=$($classes -join ',')"
New-Item -ItemType Directory $OutDir -Force | Out-Null
Shot $panel2 (Join-Path $OutDir 'panel-run-output.png')

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
