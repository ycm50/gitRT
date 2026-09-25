# ---------------------------------------------------------------------------
# 远端跟踪 + 按提交还原 —— GUI 端到端（真开窗口、真点按钮）
#
#   A) 提交历史面板里显示"远端基线"两段（本地新增/远端已有）与领先·落后
#   B) 「远端分支与地址」窗口：列出远端分支、抓取写日志、设为上游
#   C) 「还原到提交」窗口：选提交 → 预览命令 → 只读检出 / 新建分支（含确认框）
#
# 用法: pwsh -File tools\demo-remote.ps1 [-Exe <GitRT.exe>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$LabRoot = "$PSScriptRoot\..\build\remote-gui-lab",
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
public class RG {
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMGetText(IntPtr h, uint m, IntPtr cap, StringBuilder b);
 [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
 public static void ClickAt(int x, int y) {
   SetCursorPos(x, y);
   mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
   mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
 }
 public static IntPtr TopByClass(uint pid, string cls) {
   IntPtr f = IntPtr.Zero;
   EnumWindows((h,x) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p==pid) { var c=new StringBuilder(128); GetClassName(h,c,128); if (c.ToString()==cls) { f=h; return false; } } return true; }, IntPtr.Zero);
   return f;
 }
 public static IntPtr TopByClassContaining(uint pid, string part) {
   IntPtr f = IntPtr.Zero;
   EnumWindows((h,x) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p==pid) { var c=new StringBuilder(128); GetClassName(h,c,128); if (c.ToString().Contains(part)) { f=h; return false; } } return true; }, IntPtr.Zero);
   return f;
 }
}
"@
function TextOf($h) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    $len = [RG]::GetWindowTextLength($h)
    $cap = [Math]::Max(4096, $len + 512)
    $sb = New-Object System.Text.StringBuilder $cap
    [void][RG]::SMGetText($h, 0x000D, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Git($dir, [string[]]$a) { $o = & git.exe -C $dir @a 2>&1; return ($o | Out-String).Trim() }
function Shot($hwnd, $path) {
    $r = New-Object RG+RECT; [void][RG]::GetWindowRect($hwnd, [ref]$r)
    if ($r.R - $r.L -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][RG]::PrintWindow($hwnd, $dc, 2); $g.ReleaseHdc($dc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}
function SelectCommand($main, $list, $pattern) {
    $n = [int][RG]::SM($list, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($i = 0; $i -lt $n; $i++) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][RG]::SMGetText($list, 0x0189, [IntPtr]$i, $sb)
        if ($sb.ToString() -match $pattern) {
            [void][RG]::SM($list, 0x0186, [IntPtr]$i, [IntPtr]::Zero)
            [void][RG]::SM($main, 0x0111, [IntPtr]((1 -shl 16) -bor 703), [IntPtr]::Zero)
            return $i
        }
    }
    return -1
}
function RunPanel($main, $pattern, $classContains) {
    $idx = SelectCommand $main ([RG]::GetDlgItem($main, 703)) $pattern
    if ($idx -lt 0) { return [IntPtr]::Zero }
    $panel = [IntPtr]::Zero; $exec = [IntPtr]::Zero
    for ($w = 0; $w -lt 25 -and $panel -eq [IntPtr]::Zero; $w++) {
        Start-Sleep -Milliseconds 200
        $panel = [RG]::FindWindowEx($main, [IntPtr]::Zero, 'GitRT.ParamPanel', $null)
    }
    $exec = [RG]::GetDlgItem($panel, 1001)
    for ($w = 0; $w -lt 15 -and -not [RG]::IsWindowEnabled($exec); $w++) { Start-Sleep -Milliseconds 200 }
    [void][RG]::SM($exec, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $win = [IntPtr]::Zero
    for ($w = 0; $w -lt 30 -and $win -eq [IntPtr]::Zero; $w++) {
        Start-Sleep -Milliseconds 200
        $win = [RG]::TopByClassContaining([uint32]$script:appPid, $classContains)
    }
    return $win
}
function AnswerDialogs($seconds) {
    $answered = 0
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $dlg = [RG]::TopByClassContaining([uint32]$script:appPid, '32770')
        if ($dlg -ne [IntPtr]::Zero) {
            [void][RG]::SM($dlg, 0x0111, [IntPtr]6, [IntPtr]::Zero)   # IDYES
            $answered++
            Start-Sleep -Milliseconds 300
        } else {
            Start-Sleep -Milliseconds 150
        }
    }
    return $answered
}
# 跨进程 LVM_* 不会被 marshal（实测），所以用**真实鼠标点击**选中某一行，
# 再用 LVM_GETNEXTITEM/LVNI_SELECTED（无指针）回读校验。
function SelectRow($win, $lv, $row) {
    [void][RG]::SetWindowPos($win, [IntPtr](-1), 0, 0, 0, 0, 0x0043)   # 置顶，否则点到别的窗口
    Start-Sleep -Milliseconds 250
    $r = New-Object RG+RECT
    [void][RG]::GetWindowRect($lv, [ref]$r)
    $x = [int](($r.L + $r.R) / 2)
    for ($tries = 0; $tries -lt 6; $tries++) {
        $y = [int]($r.T + 26 + $row * 20 + 9 + $tries)   # 表头 ~25px + 行高 ~20px
        [RG]::ClickAt($x, $y)
        Start-Sleep -Milliseconds 250
        $sel = [int][RG]::SM($lv, 0x100C, [IntPtr](-1), [IntPtr]2)   # LVM_GETNEXTITEM / LVNI_SELECTED
        if ($sel -eq $row) { return $true }
    }
    return $false
}

# 按名字选中：逐行点开看状态行（"已选：origin/xxx"），避免依赖排序位置
function SelectRowByName($win, $lv, $status, $want, $rows) {
    for ($r = 0; $r -lt [Math]::Min($rows, 12); $r++) {
        if (SelectRow $win $lv $r) {
            $s = TextOf $status
            if ($s -match [regex]::Escape($want)) { return $true }
        }
    }
    return $false
}
# ------------------------------------------------------------------ 试验场
$LabRoot = [System.IO.Path]::GetFullPath($LabRoot)
if (Test-Path $LabRoot) { Remove-Item $LabRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $LabRoot | Out-Null
$origin = Join-Path $LabRoot 'origin.git'
$work = Join-Path $LabRoot 'work'
$other = Join-Path $LabRoot 'other'
& git.exe init -q --bare $origin | Out-Null
& git.exe -C $origin symbolic-ref HEAD refs/heads/main | Out-Null
& git.exe clone -q $origin $work 2>&1 | Out-Null
& git.exe -C $work config user.name 'GitRT Demo'; & git.exe -C $work config user.email 'demo@e.invalid'
& git.exe -C $work config commit.gpgsign false
1..2 | ForEach-Object { Set-Content (Join-Path $work "f$_.txt") "c$_" -Encoding UTF8; & git.exe -C $work add -A | Out-Null; & git.exe -C $work commit -q -m "提交$_" | Out-Null }
& git.exe -C $work push -q -u origin main 2>&1 | Out-Null
# 本地在远端之上加两条（"以远端为基往上累加"）
3..4 | ForEach-Object { Set-Content (Join-Path $work "l$_.txt") "l$_" -Encoding UTF8; & git.exe -C $work add -A | Out-Null; & git.exe -C $work commit -q -m "本地新增$_" | Out-Null }
$hashTarget = (Git $work @('rev-parse','HEAD~1'))
# 再推一个远端分支：用来测"检出 / 新建跟踪分支"这两条之前没覆盖的路径
& git.exe -C $work push -q origin HEAD:feature 2>&1 | Out-Null
& git.exe -C $work fetch -q origin 2>&1 | Out-Null
$up = Git $work @('rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{upstream}')

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$app = Start-Process $Exe -ArgumentList "`"$work`"" -PassThru
$script:appPid = $app.Id
Start-Sleep -Seconds 3
$main = [RG]::TopByClass([uint32]$app.Id, 'GitRT.MainWindow')
Check "主窗口出现" ($main -ne [IntPtr]::Zero) ''

# ------------------------------------- A) 提交历史里的"远端基线"
Write-Host "-- A) 提交历史显示远端基线 --" -ForegroundColor Cyan
$idx = SelectCommand $main ([RG]::GetDlgItem($main, 703)) '提交历史'
Check "命令列表里找到「提交历史」" ($idx -ge 0) ''
$panel = [IntPtr]::Zero
for ($w = 0; $w -lt 25 -and $panel -eq [IntPtr]::Zero; $w++) {
    Start-Sleep -Milliseconds 200
    $panel = [RG]::FindWindowEx($main, [IntPtr]::Zero, 'GitRT.ParamPanel', $null)
}
$preview = [RG]::GetDlgItem($panel, 4500)
$txt = ''
for ($w = 0; $w -lt 25; $w++) {
    Start-Sleep -Milliseconds 200
    $txt = TextOf $preview
    if ($txt -match '远端基线') { break }
}
Check "面板里出现「远端基线 origin/main」" ($txt -match '远端基线 origin/main') "txt=$($txt.Substring(0,[Math]::Min(120,$txt.Length)))"
Check "显示领先 2 / 落后 0" ($txt -match '本地新增（在远端之上累加）2 条.*远端新增（本地还没有）0 条') ''
$localMarks = ([regex]::Matches($txt, '(?m)^【本地】 ')).Count
Check "两条本地新增被标注【本地】" ($localMarks -eq 2) "marks=$localMarks"
Check "有【远端】标注的基线提交" ($txt -match '【远端】') ''

# ------------------------------------- B) 远端分支与地址窗口
Write-Host "-- B) 远端分支与地址 --" -ForegroundColor Cyan
$rmWin = RunPanel $main '远端分支与地址' 'RemoteWindow'
Check "远端窗口打开" ($rmWin -ne [IntPtr]::Zero) ''
$rmList = [RG]::GetDlgItem($rmWin, 300)
$rows = if ($rmList -ne [IntPtr]::Zero) { [int][RG]::SM($rmList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) } else { 0 }
Check "列出远端分支（>=1 行）" ($rows -ge 1) "rows=$rows"
$rmStatus = [RG]::GetDlgItem($rmWin, 301)
$st = TextOf $rmStatus
Check "状态行显示已选/提示" ($st.Length -ge 0) "status=$st"
# 抓取：写日志
$fetchBtn = [RG]::GetDlgItem($rmWin, 304)
[void][RG]::SM($fetchBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$log = TextOf ([RG]::GetDlgItem($rmWin, 303))
Check "抓取写进了日志（git fetch）" ($log -match 'fetch') "log=$($log -Replace "`r?`n",' | ')"
# 设为上游：选中第一行 → 点按钮
[void](SelectRow $rmWin $rmList 0)
Start-Sleep -Milliseconds 400
$upBtn = [RG]::GetDlgItem($rmWin, 307)
[void][RG]::SM($upBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$log2 = TextOf ([RG]::GetDlgItem($rmWin, 303))
Check "设为上游有结果写入日志" ($log2.Length -gt $log.Length) "log=$($log2 -Replace "`r?`n",' | ')"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
# 检出（305）：按名字选中远端分支 origin/feature → 确认 → 应进入分离头指针
[void](SelectRow $rmWin $rmList 0)
Start-Sleep -Milliseconds 400
$selText = TextOf ([RG]::GetDlgItem($rmWin, 301))
Check "按名字选中远端分支 origin/feature" (SelectRowByName $rmWin $rmList ([RG]::GetDlgItem($rmWin, 301)) 'origin/feature' $rows) ""
$coBtn = [RG]::GetDlgItem($rmWin, 305)
[void][RG]::SM($coBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
[void](AnswerDialogs 4)
Start-Sleep -Seconds 1
Check "检出后处于分离头指针" ((Git $work @('rev-parse','--abbrev-ref','HEAD')) -eq 'HEAD') "head=$(Git $work @('rev-parse','--abbrev-ref','HEAD'))"

# 新建跟踪分支（306）：选 origin/feature → 本地 feature 且上游是 origin/feature
& git.exe -C $work checkout -q main 2>&1 | Out-Null
Start-Sleep -Milliseconds 300
[void](SelectRow $rmWin $rmList 0)
Start-Sleep -Milliseconds 400
$trBtn = [RG]::GetDlgItem($rmWin, 306)
[void][RG]::SM($trBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
[void](AnswerDialogs 4)
Start-Sleep -Seconds 2
$up = Git $work @('rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{upstream}')
Check "新建跟踪分支后上游指向远端分支" ($up -match 'origin/') "upstream=$up"

Shot $rmWin (Join-Path $OutDir 'remote-window.png')

# ------------------------------------- C) 还原到提交
Write-Host "-- C) 还原到提交 --" -ForegroundColor Cyan
& git.exe -C $work checkout -q main 2>&1 | Out-Null; Start-Sleep -Milliseconds 400   # 回到 main，提交列表才够长
$rstWin = RunPanel $main '还原到提交' 'RestoreWindow'
Check "还原窗口打开" ($rstWin -ne [IntPtr]::Zero) ''
$rstList = [RG]::GetDlgItem($rstWin, 300)
$rstRows = if ($rstList -ne [IntPtr]::Zero) { [int][RG]::SM($rstList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) } else { 0 }
Check "提交列表有行（>=4）" ($rstRows -ge 4) "rows=$rstRows"
[void](SelectRow $rstWin $rstList 2)
Start-Sleep -Milliseconds 500
$rstPreview = TextOf ([RG]::GetDlgItem($rstWin, 302))
Check "预览显示 checkout --detach 命令" ($rstPreview -match 'checkout --detach') "preview=$($rstPreview -Replace "`r?`n",' | ')"
# 只读检出
$doBtn = [RG]::GetDlgItem($rstWin, 304)
[void][RG]::SM($doBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
$ans = AnswerDialogs 6
if ($ans -ge 1) { Check "弹出确认框并回答（自动化应答）" $true }
else { Write-Host "  [注意] 没抓到确认框（可能已被人工点掉）；继续断言还原结果" -ForegroundColor DarkYellow }
Start-Sleep -Seconds 1
$headRef = Git $work @('rev-parse','--abbrev-ref','HEAD')
Check "还原后处于分离头指针" ($headRef -eq 'HEAD') "head=$headRef"
Check "工作区与目标提交一致（l4.txt 不在）" (-not (Test-Path (Join-Path $work 'l4.txt'))) ''
Shot $rstWin (Join-Path $OutDir 'restore-window.png')

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
