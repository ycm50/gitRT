# ---------------------------------------------------------------------------
# 验证：多行文本的换行（提交历史 / 差异 / 文件历史 …）
#
#   实机 bug：git 输出是 LF-only，Win32 的 ES_MULTILINE Edit **不认裸 LF**，
#   于是"提交历史"三条提交被并成一行（* c1 …* c2 …* c3 …）。
#
#   本脚本真的驱动 GUI：
#     打开主窗口（带仓库）→ 命令列表选「提交历史」→ 参数面板点「执行」
#     → 抓文本窗口的 Edit 正文 → 断言：
#       ① 有多行（CRLF 数量 ≥ 2）
#       ② 出现 ≥ 2 条提交（用 git log 的真实 subject 数比对）
#       ③ 没有任何一行里挤着两个图线标记（bug 的特征）
#
# 用法: pwsh -File tools\demo-history.ps1 [-Exe <GitRT.exe>] [-Repo <dir>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$env:LOCALAPPDATA\Programs\GitRT\GitRT.exe",
    [string]$Repo = "$env:TEMP\GitRT-test\repo",
    [string]$OutDir = "$PSScriptRoot\..\docs\images"
)
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" }
}
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class H {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMGetText(IntPtr h, uint m, IntPtr cap, StringBuilder b);
 [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@
$WM_COMMAND = 0x0111
$LBN_SELCHANGE = 1
$LB_GETCOUNT = 0x018B
$LB_GETTEXT = 0x0189
$LB_SETCURSEL = 0x0186
$BM_CLICK = 0x00F5
$IDC_AW_LIST = 703
$IDC_PP_EXEC = 1001
$IDC_TXT_BODY = 200

function TextOf($h) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    # 跨进程 WM_GETTEXT 必须按实际长度分配缓冲区：给 256KB 会读回空串（实测踩过）
    $len = [H]::GetWindowTextLength($h)
    $cap = [Math]::Max(4096, $len + 256)
    $sb = New-Object System.Text.StringBuilder $cap
    [void][H]::SMGetText($h, 0x000D, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Shot($hwnd, $path) {
    $r = New-Object H+RECT; [void][H]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][H]::PrintWindow($hwnd, $hdc, 2); $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}

# 期望的提交条数（真实 git 数据，避免"看起来对"）
$expect = [int](& git -C $Repo rev-list --count HEAD)
Write-Host "仓库 $Repo 提交数 = $expect"

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$app = Start-Process $Exe -ArgumentList "`"$Repo`"" -PassThru
Start-Sleep -Seconds 3
$main = [H]::FindWindow('GitRT.MainWindow', $null)
Check "主窗口出现" ($main -ne [IntPtr]::Zero) ''
if ($main -eq [IntPtr]::Zero) { exit 1 }

# 命令列表里找「提交历史」
$list = [H]::GetDlgItem($main, $IDC_AW_LIST)
$count = [int][H]::SM($list, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
$idx = -1
for ($i = 0; $i -lt $count; $i++) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][H]::SMGetText($list, $LB_GETTEXT, [IntPtr]$i, $sb)
    if ($sb.ToString() -match '提交历史|历史') { $idx = $i; break }
}
Check "命令列表里找到「提交历史」（列表共 $count 项，命中 #$idx）" ($idx -ge 0) ''
if ($idx -lt 0) { Get-Process GitRT | Stop-Process -Force; exit 1 }

# 选中 → 参数面板 → 点执行
[void][H]::SM($list, $LB_SETCURSEL, [IntPtr]$idx, [IntPtr]::Zero)
[void][H]::SM($main, $WM_COMMAND, [IntPtr](($LBN_SELCHANGE -shl 16) -bor $IDC_AW_LIST), [IntPtr]::Zero)
Start-Sleep -Milliseconds 800
$panel = [H]::FindWindowEx($main, [IntPtr]::Zero, 'GitRT.ParamPanel', $null)
Check "参数面板出现" ($panel -ne [IntPtr]::Zero) ''
$exec = if ($panel -ne [IntPtr]::Zero) { [H]::GetDlgItem($panel, $IDC_PP_EXEC) } else { [IntPtr]::Zero }
Check "参数面板有「执行」按钮" ($exec -ne [IntPtr]::Zero) ''
if ($exec -ne [IntPtr]::Zero) { [void][H]::SM($exec, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) }

# 文本窗口
$tw = [IntPtr]::Zero
for ($i = 0; $i -lt 20 -and $tw -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    $tw = [H]::FindWindow('GitRT.TextWindow', $null)
}
Check "文本窗口（提交历史）出现" ($tw -ne [IntPtr]::Zero) ''
if ($tw -ne [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 500
    $body = TextOf ([H]::GetDlgItem($tw, $IDC_TXT_BODY))
    New-Item -ItemType Directory $OutDir -Force | Out-Null
    Shot $tw (Join-Path $OutDir 'history-window.png')
    $crlf = ([regex]::Matches($body, "`r`n")).Count
    $bareLf = ([regex]::Matches($body, "(?<!`r)`n")).Count
    Write-Host "  正文长度=$($body.Length)  CRLF=$crlf  裸LF=$bareLf"
    Write-Host "  前两行："
    ($body -split "`r`n" | Select-Object -First 2) | ForEach-Object { "    $_" }
    Check "正文是多行（CRLF >= 2）" ($crlf -ge 2) "crlf=$crlf"
    Check "没有裸 LF 残留" ($bareLf -eq 0) "bareLf=$bareLf"
    # 每条提交各占一行：行数应 >= 提交数
    $lines = @($body -split "`r`n" | Where-Object { $_.Trim() -ne '' })
    Check "行数 >= 提交数（$($lines.Count) >= $expect）" ($lines.Count -ge $expect) "lines=$($lines.Count)"
    # bug 特征：某一行里挤着两个图线标记
    $mashed = @($lines | Where-Object { ([regex]::Matches($_, '\*\s')).Count -ge 2 })
    Check "没有任何一行挤着两条提交（bug 特征）" ($mashed.Count -eq 0) ("mashed=" + ($mashed -join ' | '))
}
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 =="
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
