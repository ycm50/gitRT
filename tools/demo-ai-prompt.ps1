# ---------------------------------------------------------------------------
# 系统提示词端到端：首选项里能改，而且**真的送进了请求**
#
#   A) 把 systemPrompt 写进 GitRT.ai.json → 用 mock 服务接请求 →
#      断言请求体里有我们写的标记（证明生效）
#   B) 留空 → 请求体里出现内置默认提示词的特征（去掉了自定义标记）
#   C) 界面：首选项窗口里的「系统提示词」框会载入文件里的值，改完保存能写回文件
#
#   ★ 会临时改写安装目录的 GitRT.ai.json（含用户 Key）：先备份、结束时还原并校验哈希
#
# 用法: pwsh -File tools\demo-ai-prompt.ps1 [-Exe <GitRT.exe>] [-Port 18200]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$env:LOCALAPPDATA\Programs\GitRT\GitRT.exe",
    [int]$Port = 18200
)
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class AP {
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMGetText(IntPtr h, uint m, IntPtr cap, StringBuilder b);
 [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr h, string t);
 [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SMSetText(IntPtr h, uint m, IntPtr w, string l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 public static IntPtr TopByClass(uint pid, string cls) {
   IntPtr f = IntPtr.Zero;
   EnumWindows((h,x) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p==pid) { var c=new StringBuilder(128); GetClassName(h,c,128); if (c.ToString()==cls) { f=h; return false; } } return true; }, IntPtr.Zero);
   return f;
 }
 public static IntPtr ChildByClass(IntPtr parent, string cls) {
   IntPtr f = IntPtr.Zero;
   EnumChildWindows(parent, (h,x) => { var c=new StringBuilder(64); GetClassName(h,c,64);
     if (c.ToString().StartsWith(cls)) { f=h; return false; } return true; }, IntPtr.Zero);
   return f;
 }
 public delegate bool EnumChildProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
}
"@
function TextOf($h) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    $len = [AP]::GetWindowTextLength($h)
    $cap = [Math]::Max(2048, $len + 512)
    $sb = New-Object System.Text.StringBuilder $cap
    [void][AP]::SMGetText($h, 0x000D, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
$keyFile = Join-Path (Split-Path $Exe -Parent) 'GitRT.ai.json'
Check "找到 Key 文件（$keyFile）" (Test-Path $keyFile) ''
if (-not (Test-Path $keyFile)) { exit 1 }
$backup = Get-Content $keyFile -Raw
$hash0 = (Get-FileHash $keyFile -Algorithm SHA256).Hash
$bodyFile = "$PSScriptRoot\..\build\grt-mock-body.json"
$mockLog = "$PSScriptRoot\..\build\grt-mock-log.txt"

try {
    # ---------------------------------------------------------- 造配置文件
    function WriteCfg([string]$sysPrompt) {
        $cfg = [ordered]@{ endpoint = "http://127.0.0.1:$Port/v1/chat/completions"; model = 'mock-model';
                           apiKey = 'sk-demo-plain-key'; apiKeyEnv = 'DEEPSEEK_API_KEY'; timeoutMs = 60000 }
        if ($sysPrompt) { $cfg['systemPrompt'] = $sysPrompt }
        ($cfg | ConvertTo-Json) | Set-Content $keyFile -Encoding UTF8
    }
    WriteCfg 'CUSTOM-SYSTEM-PROMPT-MARKER-A1B2'

    # ---------------------------------------------------------- mock 服务
    $mock = Start-Process pwsh -ArgumentList @(
        '-NoProfile', '-File', "$PSScriptRoot\mock-ai-server.ps1", '-Port', "$Port",
        '-LogFile', $mockLog, '-LastBodyFile', $bodyFile) -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    Check "mock AI 服务就绪（端口 $Port）" (Test-Path $mockLog) ''

    function RunAi([string]$prompt) {
        Remove-Item $bodyFile -Force -ErrorAction SilentlyContinue
        $out = "$PSScriptRoot\..\build\grt-ai-out.txt"
        $p = Start-Process $Exe -ArgumentList @('--ai', $prompt, '--cwd', $PSScriptRoot, '--out', $out) -PassThru -NoNewWindow
        $p.WaitForExit(60000) | Out-Null
        for ($w = 0; $w -lt 40; $w++) {
            Start-Sleep -Milliseconds 200
            if (Test-Path $bodyFile) { break }
        }
        $body = if (Test-Path $bodyFile) { Get-Content $bodyFile -Raw } else { '' }
        $report = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
        return [pscustomobject]@{ body = $body; report = $report }
    }

    # --------- A) 自定义提示词应进请求体
    $r1 = RunAi '测试'
    Check "A 请求体里有自定义提示词标记" ($r1.body -match 'CUSTOM-SYSTEM-PROMPT-MARKER-A1B2') "bodyLen=$($r1.body.Length)"
    Check "A 请求体里**没有**内置命令表（说明确实换掉了默认提示词）" (-not ($r1.body -match 'repo\.clone')) "bodyLen=$($r1.body.Length)"
    Check "A 报告里 system_prompt_bytes 远小于内置默认" ($r1.report -match 'system_prompt_bytes=\d+') ""

    # --------- B) 留空 → 用内置默认
    WriteCfg ''
    $r2 = RunAi '测试'
    Check "B 留空时用内置默认提示词（含只读命令行说明）" (($r2.body -match 'cmdline') -and ($r2.body -match 'git status -sb')) "bodyLen=$($r2.body.Length)"
    Check "B 内置提示词里有全部命令表（repo.clone 在）" ($r2.body -match 'repo\.clone') ''
    Check "B 自定义标记已消失" (-not ($r2.body -match 'CUSTOM-SYSTEM-PROMPT-MARKER-A1B2')) ''

    # --------- C) 首选项界面：载入 + 保存
    WriteCfg 'UI-LOADED-PROMPT-XYZ'
    Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    $app = Start-Process $Exe -ArgumentList "`"$PSScriptRoot\..`"" -PassThru
    Start-Sleep -Seconds 3
    $main = if ($app) { [AP]::TopByClass([uint32]$app.Id, 'GitRT.MainWindow') } else { [IntPtr]::Zero }
    $list = [AP]::GetDlgItem($main, 703)
    $idx = -1
    $n = [int][AP]::SM($list, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($i = 0; $i -lt $n; $i++) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][AP]::SMGetText($list, 0x0189, [IntPtr]$i, $sb)
        if ($sb.ToString() -match '首选项') { $idx = $i; break }
    }
    Check "命令列表里找到「首选项…」" ($idx -ge 0) ''
    [void][AP]::SM($list, 0x0186, [IntPtr]$idx, [IntPtr]::Zero)
    [void][AP]::SM($main, 0x0111, [IntPtr]((1 -shl 16) -bor 703), [IntPtr]::Zero)
    $panel = [IntPtr]::Zero; $exec = [IntPtr]::Zero
    for ($w = 0; $w -lt 20 -and $exec -eq [IntPtr]::Zero; $w++) {
        Start-Sleep -Milliseconds 200
        $panel = [AP]::FindWindowEx($main, [IntPtr]::Zero, 'GitRT.ParamPanel', $null)
        if ($panel -ne [IntPtr]::Zero) { $exec = [AP]::GetDlgItem($panel, 1001) }
        if ($exec -ne [IntPtr]::Zero -and -not [AP]::IsWindowEnabled($exec)) { $exec = [IntPtr]::Zero }
    }
    [void][AP]::SM($exec, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)   # 首选项是 Internal：一次点击即可
    $set = [IntPtr]::Zero
    for ($w = 0; $w -lt 25 -and $set -eq [IntPtr]::Zero; $w++) {
        Start-Sleep -Milliseconds 200
        $set = [AP]::TopByClass([uint32]$app.Id, 'GitRT.SettingsWindow')
    }
    Check "AI 设置窗口打开" ($set -ne [IntPtr]::Zero) ''
    $sys = if ($set -ne [IntPtr]::Zero) { [AP]::GetDlgItem($set, 1030) } else { [IntPtr]::Zero }
    Check "设置窗口里有「系统提示词」输入框" ($sys -ne [IntPtr]::Zero) ''
    $shown = TextOf $sys
    Check "输入框载入了文件里的系统提示词" ($shown -match 'UI-LOADED-PROMPT-XYZ') "shown=[$shown]"

    # 改值 → 保存 → 必须写回 GitRT.ai.json
    # 跨进程设文本要用会 marshal 的 WM_SETTEXT（SetWindowText 对别的进程无效，实测踩过）
    [void][AP]::SMSetText($sys, 0x000C, [IntPtr]::Zero, 'SAVED-FROM-UI-PROMPT-42')
    $saveBtn = [AP]::GetDlgItem($set, 1007)
    [void][AP]::SM($saveBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    $after = Get-Content $keyFile -Raw
    Check "点保存后 systemPrompt 写回了文件" ($after -match 'SAVED-FROM-UI-PROMPT-42') ("file=[" + ($after -replace "`r?`n", " ") + "] 状态行=[" + (TextOf ([AP]::GetDlgItem($set, 1006))) + "]")
    # 截图留证（设置窗口里能看到「系统提示词」）
    try {
        # 截图留证（设置窗口里能看到「系统提示词」）
        Add-Type -AssemblyName System.Drawing
        New-Item -ItemType Directory -Force -Path "$PSScriptRoot\..\docs\images" | Out-Null
        $r = New-Object AP+RECT
        [void][AP]::GetWindowRect($set, [ref]$r)
        $bmp = New-Object System.Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [void][AP]::PrintWindow($set, $dc, 2); $g.ReleaseHdc($dc)
        $bmp.Save("$PSScriptRoot\..\docs\images\ai-system-prompt.png", [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
    } catch { Write-Host "  [注意] 截图失败：$($_.Exception.Message)" -ForegroundColor DarkYellow }

    Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
} finally {
    # 还原用户的 Key 文件（无论如何都还原）
    Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($mock) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
    Set-Content -Path $keyFile -Value $backup -NoNewline -Encoding UTF8
    $hash1 = (Get-FileHash $keyFile -Algorithm SHA256).Hash
    Write-Host "  Key 文件已还原，哈希一致: $($hash1 -eq $hash0)"
    Remove-Item $bodyFile, $mockLog -Force -ErrorAction SilentlyContinue
}
exit ($(if ($fail -eq 0) { 0 } else { 1 }))