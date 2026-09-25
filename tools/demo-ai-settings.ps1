# ---------------------------------------------------------------------------
# 演示/验证：AI 设置界面（首选项）全流程
#
#   1) 起本地 mock AI 服务（不需要真实 API Key）
#   2) 启动 GitRT → 点「首选项」→ 出现 AI 设置窗口
#   3) 填 接口地址 / Key → 点「测试连接」（应显示 HTTP 200）→ 点「保存」
#   4) 断言：Key 真的写进了 <exe 同目录>\GitRT.ai.json（明文，产品决策）
#   5) 关掉环境变量、只用文件里的配置，打开 AI 助手 → 配置行应显示「已设置 ✓」
#      并真的生成出方案（证明"文件里的 Key/端点"驱动了完整 AI 链路）
#
# 用法: pwsh -File tools\demo-ai-settings.ps1 [-Port 18081] [-Exe <GitRT.exe>] [-Repo <dir>]
# ---------------------------------------------------------------------------
param(
    [int]$Port = 18081,
    [string]$Exe = "$env:TEMP\GitRT-run\GitRT.exe",
    [string]$Repo = "$env:TEMP\GitRT-test\repo",
    [string]$OutDir = "$PSScriptRoot\..\docs\images"
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory $OutDir -Force | Out-Null
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" }
}

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;using System.Collections.Generic;
public class AiSet {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr after, string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
 // 下拉列表是独立弹窗 "ComboLBox"（owner 为 0，不是 combo 的子窗口）→ 按类名 + 进程找
 public static IntPtr FindComboLBox(uint pid) {
   IntPtr found = IntPtr.Zero;
   EnumWindows((h, p) => {
     uint wp; GetWindowThreadProcessId(h, out wp);
     if (wp != pid) return true;
     var cls = new StringBuilder(64); GetClassName(h, cls, 64);
     if (cls.ToString() == "ComboLBox" && IsWindowVisible(h)) { found = h; return false; }
     return true;
   }, IntPtr.Zero);
   return found;
 }
 [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMS(IntPtr h, uint m, IntPtr w, string l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMGetText(IntPtr h, uint m, IntPtr cap, StringBuilder buf);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@

$WM_COMMAND = 0x0111
$WM_SETTEXT = 0x000C
$WM_GETTEXT = 0x000D
$WM_CHAR    = 0x0102
# 控件 ID（与 src/gui/*.cpp 的 enum 一致）
$IDC_AW_SETTINGS = 706
$IDC_AW_AI       = 705
$IDC_SET_ENDPOINT = 1000
$IDC_SET_MODEL    = 1001
$IDC_SET_KEY      = 1002
$IDC_SET_STATUS   = 1006
$IDC_SET_SAVE     = 1007
$IDC_SET_TEST     = 1008
$IDC_SET_GETMODELS = 1020
$IDC_AI_CFG_LABEL = 909
# ComboBox 消息
$CB_GETCOUNT = 0x0146
$CB_GETLBTEXT = 0x0148

function MaskKey($s) {
    if ([string]::IsNullOrEmpty($s)) { return '(空)' }
    if ($s.Length -le 8) { return '***' }
    return $s.Substring(0, 3) + '***(' + $s.Length + ' 字符，已打码)'
}
function TextOf($h) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    # 编辑框：必须用 WM_GETTEXT（跨进程 GetWindowText 读不到 Edit 的真实内容，只读"标题缓存"）
    $sb = New-Object System.Text.StringBuilder 2048
    [void][AiSet]::SMGetText($h, $WM_GETTEXT, [IntPtr]2048, $sb)
    if ($sb.Length -gt 0) { return $sb.ToString() }
    # 静态文本：GetWindowText 可用
    $sb2 = New-Object System.Text.StringBuilder 2048
    [void][AiSet]::GetWindowText($h, $sb2, 2048)
    return $sb2.ToString()
}
function TypeText($h, $text) {
    foreach ($ch in $text.ToCharArray()) {
        [void][AiSet]::SM($h, $WM_CHAR, [IntPtr][int]$ch, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 8
    }
}
function Shot($hwnd, $path) {
    $r = New-Object AiSet+RECT
    [void][AiSet]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][AiSet]::PrintWindow($hwnd, $hdc, 2); $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

$exeDir  = Split-Path -Parent (Resolve-Path $Exe)
$keyFile = Join-Path $exeDir 'GitRT.ai.json'
$keyBak  = "$keyFile.demo-bak"
$hadKey  = Test-Path $keyFile
if ($hadKey) { Copy-Item $keyFile $keyBak -Force }

# ---- mock 服务 ----
$mock = $null
$alive = $false
try { $alive = (Invoke-WebRequest "http://127.0.0.1:$Port/health" -TimeoutSec 2 -UseBasicParsing).StatusCode -eq 200 } catch {}
if (-not $alive) {
    Write-Host "启动 mock AI 服务 (端口 $Port) ..."
    $mock = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList `
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\mock-ai-server.ps1`" -Port $Port -LogFile `"$env:TEMP\GitRT-test\mock-ai-settings.log`""
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        try { if ((Invoke-WebRequest "http://127.0.0.1:$Port/health" -TimeoutSec 2 -UseBasicParsing).StatusCode -eq 200) { $alive = $true; break } } catch {}
    }
}
Check "mock AI 服务就绪（端口 $Port）" $alive ''

# 清掉环境变量覆盖，确保走的是"文件 + 界面"这条路
$env:GITRT_AI_ENDPOINT = $null
$env:GITRT_AI_MODEL = $null
$env:GITRT_AI_API_KEY_ENV = $null
$env:DEEPSEEK_API_KEY = $null

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
$app = Start-Process $Exe -ArgumentList "`"$Repo`"" -PassThru
Start-Sleep -Seconds 3
$main = [AiSet]::FindWindow('GitRT.MainWindow', $null)
Check "主窗口出现" ($main -ne [IntPtr]::Zero) ''
if ($main -eq [IntPtr]::Zero) { if ($mock) { Stop-Process -Id $mock.Id -Force }; exit 1 }

# ---- 打开 AI 设置（首选项）----
[void][AiSet]::SM($main, $WM_COMMAND, [IntPtr]$IDC_AW_SETTINGS, [IntPtr]::Zero)
$set = [IntPtr]::Zero
for ($i = 0; $i -lt 25 -and $set -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 200
    $set = [AiSet]::FindWindow('GitRT.SettingsWindow', $null)
}
Check "AI 设置窗口出现（首选项）" ($set -ne [IntPtr]::Zero) ''
if ($set -eq [IntPtr]::Zero) { Stop-Process -Id $app.Id -Force; if ($mock) { Stop-Process -Id $mock.Id -Force }; exit 1 }

# ---- 填接口地址与 Key（跨进程用 WM_SETTEXT + WM_CHAR）----
$epEdit  = [AiSet]::GetDlgItem($set, $IDC_SET_ENDPOINT)
$keyEdit = [AiSet]::GetDlgItem($set, $IDC_SET_KEY)
[void][AiSet]::SMS($epEdit, $WM_SETTEXT, [IntPtr]::Zero, "http://127.0.0.1:$Port/v1/chat/completions")
# ★ 先清空：设置窗口会载入已有的 GitRT.ai.json，不清空就会把演示 Key 拼到用户真实 Key 后面
[void][AiSet]::SMS($keyEdit, $WM_SETTEXT, [IntPtr]::Zero, '')
TypeText $keyEdit 'sk-demo-plain-key'
Start-Sleep -Milliseconds 200
Check "接口地址已填入" ((TextOf $epEdit) -match "127.0.0.1:$Port") ("got='$(TextOf $epEdit)'")
Check "Key 已填入" ((TextOf $keyEdit) -eq 'sk-demo-plain-key') ("got='$(MaskKey (TextOf $keyEdit))'")

# ---- 测试连接 ----
[void][AiSet]::SM($set, $WM_COMMAND, [IntPtr]$IDC_SET_TEST, [IntPtr]::Zero)
Start-Sleep -Milliseconds 2500
$status = TextOf ([AiSet]::GetDlgItem($set, $IDC_SET_STATUS))
Check "「测试连接」返回 HTTP 200" ($status -match 'HTTP 200|已获取模型列表') ("status='$status'")

# ---- 模型列表：应从接口拉取并填充下拉框 ----
$combo = [AiSet]::GetDlgItem($set, $IDC_SET_MODEL)
# 测试连接成功会自动拉一次；若还没拉到就手动点一次
$count = [int][AiSet]::SM($combo, $CB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
if ($count -lt 1) {
    [void][AiSet]::SM($set, $WM_COMMAND, [IntPtr]$IDC_SET_GETMODELS, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 2500
    $count = [int][AiSet]::SM($combo, $CB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
}
Check "模型下拉框从接口拉到列表（>=2 个）" ($count -ge 2) "count=$count"
$items = @()
for ($i = 0; $i -lt $count; $i++) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][AiSet]::SMGetText($combo, $CB_GETLBTEXT, [IntPtr]$i, $sb)
    if ($sb.Length -gt 0) { $items += $sb.ToString() }
}
Check "列表内容来自 mock 接口（含 mock-model）" (($items -join ',') -match 'mock-model') ("items=$($items -join ',')")

# ---- 下拉真的能"拉出来"：量下拉列表窗口（ComboLBox，独立弹窗）的高度 ----
$CB_SHOWDROPDOWN   = 0x014F
$CB_GETDROPPEDSTATE = 0x0157
$CB_GETITEMHEIGHT  = 0x0154
$itemH = [int][AiSet]::SM($combo, $CB_GETITEMHEIGHT, [IntPtr]0, [IntPtr]::Zero)
[void][AiSet]::SM($combo, $CB_SHOWDROPDOWN, [IntPtr]1, [IntPtr]::Zero)
Start-Sleep -Milliseconds 600
$dropped = [int][AiSet]::SM($combo, $CB_GETDROPPEDSTATE, [IntPtr]::Zero, [IntPtr]::Zero)
Check "点箭头能展开列表（CB_GETDROPPEDSTATE=1）" ($dropped -eq 1) "dropped=$dropped"
# 下拉列表是独立的 "ComboLBox" 弹窗（PrintWindow 抓不到）→ 找它并量高度
$pid2 = 0
[void][AiSet]::GetWindowThreadProcessId($combo, [ref]$pid2)
$lb = [AiSet]::FindComboLBox($pid2)
$lbH = 0
if ($lb -ne [IntPtr]::Zero) {
    $lr = New-Object AiSet+RECT
    [void][AiSet]::GetWindowRect($lb, [ref]$lr)
    $lbH = $lr.B - $lr.T
}
Check "下拉列表窗口有真实高度（>= 2 行，选项画得出来）" `
      ($lbH -ge (2 * $itemH)) "ComboLBox 高=$lbH 行高=$itemH"
# 抓屏（不能用 PrintWindow：弹出列表不属于父窗口）
$cr = New-Object AiSet+RECT
[void][AiSet]::GetWindowRect($combo, [ref]$cr)
$shotW = $cr.R - $cr.L
$shotH = [Math]::Max(40, [Math]::Min(280, $lbH + 8))
if ($shotW -gt 0 -and $shotH -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap($shotW, $shotH)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($cr.L, $cr.B, 0, 0, (New-Object System.Drawing.Size($shotW, $shotH)))
    $bmp.Save((Join-Path $OutDir 'ai-settings-models.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "  抓屏（下拉列表）-> ai-settings-models.png (${shotW}x${shotH})"
}
[void][AiSet]::SM($combo, $CB_SHOWDROPDOWN, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 300
# 选第二个模型并保存，验证"选择"真的落盘
if ($count -ge 2) {
    [void][AiSet]::SM($combo, 0x014E, [IntPtr]1, [IntPtr]::Zero)   # CB_SETCURSEL = 1
}
Start-Sleep -Milliseconds 200
Shot $set (Join-Path $OutDir 'ai-settings.png')

# ---- 保存 ----
[void][AiSet]::SM($set, $WM_COMMAND, [IntPtr]$IDC_SET_SAVE, [IntPtr]::Zero)
Start-Sleep -Milliseconds 600
$status = TextOf ([AiSet]::GetDlgItem($set, $IDC_SET_STATUS))
Check "保存成功（界面提示已保存）" ($status -match '已保存') ("status='$status'")
$fileOk = (Test-Path $keyFile) -and ((Get-Content $keyFile -Raw -Encoding UTF8) -match 'sk-demo-plain-key')
Check "Key 已明文写入 $keyFile" $fileOk ''
if (Test-Path $keyFile) { Write-Host ("    文件内容：apiKey 已打码 → " + ((Get-Content $keyFile -Raw) -replace "`r?`n", ' ' -replace '"apiKey":\s*"[^"]*"', '"apiKey": "<masked>"')) }
# 下拉框里选的模型应该被存下来（选的是列表第 2 项）
$savedText = if (Test-Path $keyFile) { Get-Content $keyFile -Raw -Encoding UTF8 } else { '' }
Check "下拉框选中的模型已保存（列表第 2 项生效）" `
      ($count -ge 2 -and $savedText -match [regex]::Escape($items[1])) `
      ("model 期望='$(if ($count -ge 2) { $items[1] })' 文件 apiKey=<masked>")

# ---- 只用文件配置（环境变量全空）打开 AI 助手，验证配置行 + 真的能生成方案 ----
[void][AiSet]::SM($set, $WM_COMMAND, [IntPtr]1009, [IntPtr]::Zero)   # 关闭设置窗口
[void][AiSet]::SM($main, $WM_COMMAND, [IntPtr]$IDC_AW_AI, [IntPtr]::Zero)
$ai = [IntPtr]::Zero
for ($i = 0; $i -lt 25 -and $ai -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 200
    $ai = [AiSet]::FindWindow('GitRT.AiWindow', $null)
}
Check "AI 助手窗口出现" ($ai -ne [IntPtr]::Zero) ''
if ($ai -ne [IntPtr]::Zero) {
    $cfgLabel = TextOf ([AiSet]::GetDlgItem($ai, $IDC_AI_CFG_LABEL))
    Check "配置行显示 Key 已设置（来源=GitRT.ai.json）" ($cfgLabel -match '已设置') ("label='$cfgLabel'")
    # 在提示框里输入一句话并生成方案（mock 会返回一条合法计划）
    $prompt = [AiSet]::GetDlgItem($ai, 900)
    TypeText $prompt 'COMMITALL 把当前改动提交'
    Start-Sleep -Milliseconds 300
    [void][AiSet]::SM($ai, $WM_COMMAND, [IntPtr]904, [IntPtr]::Zero)   # 生成方案
    Start-Sleep -Milliseconds 2500
    $plan = TextOf ([AiSet]::GetDlgItem($ai, 901))
    Check "只用文件里的配置也能生成方案（AI 链路通）" ($plan -match 'commit' -or $plan -match '方案') ("plan='$($plan.Substring(0,[Math]::Min(80,$plan.Length)))'")
    Shot $ai (Join-Path $OutDir 'ai-settings-verified.png')
}

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
if ($mock) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
# 还原现场
if ($hadKey) { Copy-Item $keyBak $keyFile -Force; Remove-Item $keyBak -Force } else { Remove-Item $keyFile -Force -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 =="
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
