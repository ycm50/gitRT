# ---------------------------------------------------------------------------
# 演示/验证：AI 助手窗口全流程（生成方案 → 执行 → 显示运行结果）
#
#   对着 tools\mock-ai-server.ps1（本地 OpenAI 兼容服务）跑，不需要真实 API Key；
#   全过程通过 UIA 消息驱动，并抓取窗口截图作为证据。
#
# 用法: pwsh -File tools\demo-ai-gui.ps1 [-Repo <dir>] [-Port 18080] [-OutDir <dir>]
# ---------------------------------------------------------------------------
param(
    [string]$Repo = "$env:TEMP\GitRT-test\repo",
    [int]$Port = 18080,
    [string]$OutDir = "$PSScriptRoot\..\docs\images",
    [string]$Exe = "$env:TEMP\GitRT-run\GitRT.exe"
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory $OutDir -Force | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class AiDemo {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr h, string s);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@

$WM_COMMAND = 0x0111
# 控件 ID（必须与 src/gui/app_window.cpp、ai_window.cpp 的 enum 保持一致）
$IDC_AW_AI    = 705
$IDC_AI_PROMPT = 900
$IDC_AI_GEN   = 904
$IDC_AI_EXEC  = 905

function Shot($hwnd, $path) {
    $r = New-Object AiDemo+RECT
    [void][AiDemo]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { Write-Host "  [warn] 窗口句柄无效: $hwnd"; return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][AiDemo]::PrintWindow($hwnd, $hdc, 2); $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "  截图 -> $path ($((Get-Item $path).Length) bytes)"
}

# ---- mock 服务 ----
$mock = $null
$alive = $false
try { $alive = (Invoke-WebRequest "http://127.0.0.1:$Port/health" -TimeoutSec 2 -UseBasicParsing).StatusCode -eq 200 } catch {}
if (-not $alive) {
    Write-Host "启动 mock AI 服务 (端口 $Port) ..."
    $mock = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList `
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\mock-ai-server.ps1`" -Port $Port -LogFile `"$env:TEMP\GitRT-test\mock-ai-demo.log`""
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        try { if ((Invoke-WebRequest "http://127.0.0.1:$Port/health" -TimeoutSec 2 -UseBasicParsing).StatusCode -eq 200) { $alive = $true; break } } catch {}
    }
}
Write-Host "mock 就绪: $alive"

# ---- 准备一个已暂存的改动，保证 commit.commit 会成功 ----
Set-Content (Join-Path $Repo 'ai-demo.txt') "ai demo $(Get-Random)"
& git -C $Repo add -A 2>&1 | Out-Null
$before = [int](& git -C $Repo rev-list --count HEAD)
Write-Host "仓库已就绪: $Repo  HEAD commits=$before"

$env:GITRT_AI_ENDPOINT = "http://127.0.0.1:$Port/v1/chat/completions"
$env:GITRT_AI_MODEL = 'mock-model'
$env:GITRT_AI_API_KEY_ENV = 'GITRT_TEST_KEY'
$env:GITRT_TEST_KEY = 'test-key'

Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
$app = Start-Process $Exe -ArgumentList "`"$Repo`"" -PassThru
Start-Sleep -Seconds 3

$main = [AiDemo]::FindWindow('GitRT.MainWindow', $null)
if ($main -eq [IntPtr]::Zero) { Write-Host "找不到主窗口"; exit 1 }
[void][AiDemo]::SM($main, $WM_COMMAND, [IntPtr]$IDC_AW_AI, [IntPtr]::Zero)

$ai = [IntPtr]::Zero
for ($i = 0; $i -lt 25 -and $ai -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 300
    $ai = [AiDemo]::FindWindow('GitRT.AiWindow', $null)
}
if ($ai -eq [IntPtr]::Zero) { Write-Host "AI 窗口未出现"; Stop-Process -Id $app.Id -Force; exit 1 }
Write-Host "AI 窗口 = $ai"

$edit = [AiDemo]::GetDlgItem($ai, $IDC_AI_PROMPT)
# 注意：跨进程 SetWindowText 只会写"窗口标题缓存"，编辑框的真实内容不会变
#       （实测：GUI 进程内 GetWindowTextLength 仍为 0）。改用 WM_CHAR 逐字符输入。
$prompt = 'COMMITALL 把当前改动提交'
foreach ($ch in $prompt.ToCharArray()) {
    [void][AiDemo]::SM($edit, 0x0102, [IntPtr][int]$ch, [IntPtr]::Zero)   # WM_CHAR
    Start-Sleep -Milliseconds 15
}
Start-Sleep -Milliseconds 400
$sb = New-Object System.Text.StringBuilder 256
[void][AiDemo]::GetWindowText($edit, $sb, 256)
Write-Host "提示词回读 = '$($sb.ToString())'"

Write-Host "点击[生成方案] ..."
[void][AiDemo]::SM($ai, $WM_COMMAND, [IntPtr]$IDC_AI_GEN, [IntPtr]::Zero)
Start-Sleep -Milliseconds 2500
Shot $ai (Join-Path $OutDir 'ai-plan.png')

Write-Host "点击[执行方案] ..."
[void][AiDemo]::SM($ai, $WM_COMMAND, [IntPtr]$IDC_AI_EXEC, [IntPtr]::Zero)
Start-Sleep -Milliseconds 3500
Shot $ai (Join-Path $OutDir 'ai-result.png')

$after = [int](& git -C $Repo rev-list --count HEAD)
Write-Host "提交数: $before -> $after"
Write-Host "HEAD: $(& git -C $Repo log -1 --pretty=%s)"
Write-Host "--- 应用日志（AI 相关）---"
$log = Get-ChildItem "$env:LOCALAPPDATA\GitRT\logs" -Filter '*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) { Get-Content $log.FullName | Select-String -Pattern 'ai|AI' | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" } }

Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
if ($mock) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
