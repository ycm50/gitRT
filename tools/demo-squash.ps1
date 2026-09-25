# ---------------------------------------------------------------------------
# 合并提交（squash）界面端到端：真的开窗口、真的勾选、真的合并
#
#   流程：
#     1) 造一个 4 提交的仓库（工作区内，避免 git 向上找到主仓库）
#     2) 启动 GUI → 命令列表选「合并所选提交…」→ 面板点「执行」→ 合并窗口打开
#     3) 断言列表行数 = 提交数
#     4) 用键盘勾选**连续的两条**（LVM_SETSELECTIONMARK 聚焦 + 空格切换勾选）
#     5) 点「合并所选提交」→ 弹出确认框（#32770）→ 回 IDYES
#     6) 断言：仓库提交数 -1、tree 不变、合并信息 = 两条 subject 拼接、
#              执行日志里出现真实命令行（> git reset --soft / > git commit）
#
# 用法: pwsh -File tools\demo-squash.ps1 [-Exe <GitRT.exe>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$RepoDir = "$PSScriptRoot\..\build\squash-lab\gui",
    [string]$OutDir = "$PSScriptRoot\..\docs\images"
)
$ErrorActionPreference = 'Continue'
$RepoDir = [System.IO.Path]::GetFullPath($RepoDir)
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class SQ {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMGetText(IntPtr h, uint m, IntPtr cap, StringBuilder b);
 [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
 public static void ClickAt(int x, int y) {
   SetCursorPos(x, y);
   mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);   // LEFTDOWN
   mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);   // LEFTUP
 }
 public static void Key(byte vk) {
   keybd_event(vk, 0, 0, IntPtr.Zero);
   keybd_event(vk, 0, 2, IntPtr.Zero);
 }
 public static IntPtr FindTopByClass(uint pid, string cls) {
   IntPtr found = IntPtr.Zero;
   EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p == pid) { var c = new StringBuilder(128); GetClassName(h, c, 128);
       if (c.ToString() == cls) { found = h; return false; } } return true; }, IntPtr.Zero);
   return found;
 }
 public static System.Collections.Generic.List<string> AllWindows(uint pid) {
   var res = new System.Collections.Generic.List<string>();
   EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p == pid) { var c = new StringBuilder(128); GetClassName(h, c, 128);
                     var t = new StringBuilder(128); GetWindowText(h, t, 128);
                     res.Add(h + " cls=" + c + " title=" + t); } return true; }, IntPtr.Zero);
   return res;
 }
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@
$LVM_GETITEMCOUNT = 0x1004
$LVM_GETITEMSTATE = 0x102C
$LVM_SETSELECTIONMARK = 0x1043
$LVIS_STATEIMAGEMASK = 0xF000
$LB_GETCOUNT = 0x018B
$LB_GETTEXT = 0x0189
$LB_SETCURSEL = 0x0186
$WM_COMMAND = 0x0111
$WM_KEYDOWN = 0x0100
$WM_KEYUP = 0x0101
$VK_SPACE = 0x20
$BM_CLICK = 0x00F5
$IDYES = 6
$IDC_AW_LIST = 703
$IDC_PP_EXEC = 1001
$IDC_SQ_LIST = 300
$IDC_SQ_STATUS = 301
$IDC_SQ_LOG = 303
$IDC_SQ_MERGE = 304

function TextOf($h) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    $len = [SQ]::GetWindowTextLength($h)
    $cap = [Math]::Max(2048, $len + 256)
    $sb = New-Object System.Text.StringBuilder $cap
    [void][SQ]::SMGetText($h, 0x000D, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Git($dir, [string[]]$a) { $o = & git.exe -C $dir @a 2>&1; return ($o | Out-String).Trim() }
function GetCheckState($lv, $i) { return (([int][SQ]::SM($lv, $LVM_GETITEMSTATE, [IntPtr]$i, [IntPtr]$LVIS_STATEIMAGEMASK)) -shr 12) -band 0xF }
function CheckedState($list, $i) { return (([int][SQ]::SM($list, $LVM_GETITEMSTATE, [IntPtr]$i, [IntPtr]$LVIS_STATEIMAGEMASK) -shr 12) -band 0xF) }
# 用**真实输入**勾选：先在列表里点一下拿焦点（点中间，避开复选框列），
# 再 Home 回到第一条，然后「空格」切换勾选、「Down」下移 —— 比跨进程发 LVM_* 稳
# 勾选前 N 行：直接点"状态图标"那一列（鼠标不需要窗口有前台焦点），并自行校准行位置 ——
# 先记录全部行状态，再沿 y 向下扫描点，看哪一行翻转，从而定位 0/1 行的复选框。
function CheckFirstRows($sqw, $lv, $want) {
    # ★ 合成鼠标点击只会落到"最上面"的窗口：先把自己的窗口置顶，
    #   否则点到的可能是盖在上面的别人的窗口（实测踩过）
    [void][SQ]::SetWindowPos($sqw, [IntPtr](-1), 0, 0, 0, 0, 0x0043)   # HWND_TOPMOST + NOMOVE|NOSIZE|SHOW
    Start-Sleep -Milliseconds 300
    $r = New-Object SQ+RECT
    [void][SQ]::GetWindowRect($lv, [ref]$r)
    $x = $r.L + 14
    $n = [int][SQ]::SM($lv, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    $done = @{}
    for ($y = $r.T + 18; $y -lt $r.T + ($n * 30) -and $done.Count -lt $want; $y += 3) {
        [SQ]::ClickAt($x, $y)
        Start-Sleep -Milliseconds 120
        for ($i = 0; $i -lt $n; $i++) {
            $st = GetCheckState $lv $i
            if ($st -eq 2 -and -not $done.ContainsKey($i)) { $done[$i] = $y }
        }
        # 只要前 want 行
        $extra = @($done.Keys | Where-Object { $_ -ge $want })
        foreach ($k in $extra) {
            [SQ]::ClickAt($x, $done[$k])   # 多余的点回去
            Start-Sleep -Milliseconds 120
            $done.Remove($k)
        }
    }
    return $done
}
function Shot($hwnd, $path) {
    $r = New-Object SQ+RECT; [void][SQ]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][SQ]::PrintWindow($hwnd, $hdc, 2); $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}

# ------------------------------------------------------------------ 造仓库
if (Test-Path $RepoDir) { Remove-Item $RepoDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $RepoDir | Out-Null
Git $RepoDir @('init', '-q', '-b', 'main') | Out-Null
Git $RepoDir @('config', 'user.name', 'GitRT Demo') | Out-Null
Git $RepoDir @('config', 'user.email', 'demo@example.invalid') | Out-Null
Git $RepoDir @('config', 'commit.gpgsign', 'false') | Out-Null
1..4 | ForEach-Object { Set-Content "$RepoDir\f$_.txt" "c$_" -Encoding UTF8; Git $RepoDir @('add','-A') | Out-Null; Git $RepoDir @('commit','-q','-m',"c$_") | Out-Null }
$top = (Git $RepoDir @('rev-parse', '--show-toplevel')) -replace '\\', '/'
$want = $RepoDir -replace '\\', '/'
Check "测试仓库就绪（$top）" ($top -eq $want) "top=$top want=$want"
$count0 = [int](Git $RepoDir @('rev-list','--count','HEAD'))
$tree0 = Git $RepoDir @('rev-parse','HEAD^{tree}')
$subjText = Git $RepoDir @('log', '--format=%s')
$subjects = @($subjText -split "`n" | Where-Object { $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
Write-Host "  提交数=$count0  tree=$($tree0.Substring(0,8))…  最新两条=$($subjects[0]),$($subjects[1])"

# -------------------------------------------------------------- 打开合并窗口
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$script:appProc = Start-Process $Exe -ArgumentList "`"$RepoDir`"" -PassThru
Start-Sleep -Seconds 3
$main = if ($appProc) { [SQ]::FindTopByClass([uint32]$appProc.Id, 'GitRT.MainWindow') } else { [IntPtr]::Zero }
Check "主窗口出现" ($main -ne [IntPtr]::Zero) ''
$list = [SQ]::GetDlgItem($main, $IDC_AW_LIST)
$n = [int][SQ]::SM($list, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
$idx = -1
for ($i = 0; $i -lt $n; $i++) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][SQ]::SMGetText($list, $LB_GETTEXT, [IntPtr]$i, $sb)
    if ($sb.ToString() -match '合并所选提交|Squash selected') { $idx = $i; break }
}
Check "命令列表里有「合并所选提交…」（#$idx/$n）" ($idx -ge 0) ''
if ($idx -ge 0) {
    [void][SQ]::SM($list, $LB_SETCURSEL, [IntPtr]$idx, [IntPtr]::Zero)
    [void][SQ]::SM($main, $WM_COMMAND, [IntPtr]((1 -shl 16) -bor $IDC_AW_LIST), [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    # 等面板和「执行」按钮真正出现（跨进程点按钮，早一步就点空）
    $panel = [IntPtr]::Zero; $exec = [IntPtr]::Zero
    for ($w = 0; $w -lt 20 -and $exec -eq [IntPtr]::Zero; $w++) {
        Start-Sleep -Milliseconds 200
        $panel = [SQ]::FindWindowEx($main, [IntPtr]::Zero, 'GitRT.ParamPanel', $null)
        if ($panel -ne [IntPtr]::Zero) { $exec = [SQ]::GetDlgItem($panel, $IDC_PP_EXEC) }
        # 按钮可能刚创建还没启用 —— 对禁用按钮 BM_CLICK 是空操作
        if ($exec -ne [IntPtr]::Zero -and -not [SQ]::IsWindowEnabled($exec)) { $exec = [IntPtr]::Zero }
    }
    Check "参数面板与「执行」按钮就位" ($exec -ne [IntPtr]::Zero) "panel=$panel exec=$exec"
    # ★ Danger::Destructive 的命令：第一次点只是"arm"，第二次才真执行 —— 循环点到窗口出现
    $sqw = [IntPtr]::Zero
    for ($t = 0; $t -lt 3 -and $sqw -eq [IntPtr]::Zero; $t++) {
        if ($exec -ne [IntPtr]::Zero) { [void][SQ]::SM($exec, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) }
        Start-Sleep -Milliseconds 700
        $sqw = [SQ]::FindTopByClass([uint32]$appProc.Id, 'GitRT.SquashWindow')
    }
}
if ($sqw -eq [IntPtr]::Zero) {
    for ($i = 0; $i -lt 20 -and $sqw -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $sqw = [SQ]::FindTopByClass([uint32]$appProc.Id, 'GitRT.SquashWindow') }
}
if ($sqw -eq [IntPtr]::Zero) { Get-Process GitRT | Stop-Process -Force; exit 1 }
Start-Sleep -Milliseconds 600

$lv = [SQ]::GetDlgItem($sqw, $IDC_SQ_LIST)
$rows = [int][SQ]::SM($lv, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
Check "复选列表列出全部提交（$rows 行）" ($rows -eq $count0) "rows=$rows count=$count0"

# ------------------------------------------------------------ 勾选连续两条
$checked = CheckFirstRows $sqw $lv 2
$selRow = [int][SQ]::SM($lv, 0x100C, [IntPtr](-1), [IntPtr]2)   # LVM_GETNEXTITEM/LVNI_SELECTED
Write-Host "  debug: 被点中的行=$selRow  勾选到的行=$($checked.Keys -join ',')"
$c0 = GetCheckState $lv 0
$c1 = GetCheckState $lv 1
$c2 = GetCheckState $lv 2
Check "前两条被勾选（state=$c0/$c1）" ($c0 -eq 2 -and $c1 -eq 2) "0=$c0 1=$c1"
Check "第三条未勾选（state=$c2）" ($c2 -eq 1) "2=$c2"
$status = TextOf ([SQ]::GetDlgItem($sqw, $IDC_SQ_STATUS))
Write-Host "  状态行：$status"
Check "状态行提示'已选 2 条、连续'" ($status -match '已选 2 条' -and $status -match '连续') "status=$status"
Check "「合并所选提交」按钮已启用" ([SQ]::IsWindowEnabled([SQ]::GetDlgItem($sqw, $IDC_SQ_MERGE))) ''
$msgText = TextOf ([SQ]::GetDlgItem($sqw, 302))
Check "合并信息已自动预填（含两条 subject）" ($msgText -match [regex]::Escape($subjects[0]) -and $msgText -match [regex]::Escape($subjects[1])) "msg=[$msgText]"

# ------------------------------------------------------------------ 点合并
[void][SQ]::SetForegroundWindow($sqw)
$mergeBtn = [SQ]::GetDlgItem($sqw, $IDC_SQ_MERGE)
[void][SQ]::SM($mergeBtn, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
# 确认框是模态的：立刻轮询并回 YES（人如果在旁边先点了也无妨）
$dlg = [IntPtr]::Zero
for ($d = 0; $d -lt 30 -and $dlg -eq [IntPtr]::Zero; $d++) {
    Start-Sleep -Milliseconds 100
    $dlg = [SQ]::FindTopByClass([uint32]$appProc.Id, '#32770')
}
if ($dlg -ne [IntPtr]::Zero) {
    Check "弹出确认框（危险操作）" $true
    [void][SQ]::SM($dlg, $WM_COMMAND, [IntPtr]$IDYES, [IntPtr]::Zero)
} else {
    Write-Host "  [注意] 没抓到确认框（可能已被人工点掉）；继续断言结果" -ForegroundColor DarkYellow
}

# 等合并完成（提交数下降）
$done = $false
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 300
    if ((& git.exe -C $RepoDir rev-list --count HEAD 2>$null) -ne "$count0") { $done = $true; break }
}
Check "合并已执行（提交数变了）" $done "count=$(& git.exe -C $RepoDir rev-list --count HEAD 2>$null)"
Start-Sleep -Milliseconds 800
New-Item -ItemType Directory $OutDir -Force | Out-Null
Shot $sqw (Join-Path $OutDir 'squash-window.png')

# ------------------------------------------------------------------ 断言结果
$count1 = [int](Git $RepoDir @('rev-list','--count','HEAD'))
$tree1 = Git $RepoDir @('rev-parse','HEAD^{tree}')
$msg1 = (Git $RepoDir @('log','-1','--format=%B','HEAD')).Trim() -replace "`r`n", "`n"
$log = TextOf ([SQ]::GetDlgItem($sqw, $IDC_SQ_LOG))
Check "提交数 $count0 → $($count0-1)" ($count1 -eq $count0 - 1) "count=$count1"
Check "HEAD 的 tree 不变（改动 = 两条之和）" ($tree1 -eq $tree0) 'tree 变了'
Check "合并信息 = 两条 subject 拼接" ($msg1 -eq "$($subjects[1])`n$($subjects[0])") "msg=[$msg1]"
Check "日志里有真实命令行（reset --soft）" ($log -match 'git reset --soft') "log=[$log]"
Check "日志里有真实命令行（git commit）" ($log -match 'git commit') "log=[$log]"
Check "工作区干净" ((Git $RepoDir @('status','--porcelain')) -eq '') (Git $RepoDir @('status','--porcelain'))
# 合并后：信息框清空（没有勾选）、列表刷新出合并后的提交
$msgAfter = TextOf ([SQ]::GetDlgItem($sqw, 302))
Check "合并后信息框已清空" ($msgAfter.Trim() -eq '') "msg=[$msgAfter]"
$rowsAfter = [int][SQ]::SM($lv, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
Check "合并后列表刷新为 $($count0-1) 行" ($rowsAfter -eq $count0 - 1) "rows=$rowsAfter"
$statusAfter = TextOf ([SQ]::GetDlgItem($sqw, $IDC_SQ_STATUS))
Check "状态行显示'完成'" ($statusAfter -match '完成') "status=$statusAfter"

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))