# ---------------------------------------------------------------------------
# 诊断：任务栏上到底有没有画出 GitRT 图标（按图标主色定位，不依赖 UI Automation）
#
#   ① 取 gitrt.ico 的主色（橙 240,80,51，占图标 ~80% 面积）
#   ② 启动 GitRT → 抓任务栏位图
#   ③ 逐列统计"接近主色"的像素：连续 >=16 列的区块 = 一个实心图标砖块
#      （其它图标只有 1px 级的抗锯齿噪点，不会误判）
#   ④ 把命中区块裁图放大保存，便于肉眼确认
#
# 用法: pwsh -File tools\diag-taskbar-icon.ps1 [-Exe <GitRT.exe>] [-Repo <dir>]
#
# 说明：如果这里 PASS 但你在任务栏看不到图标，多半是下面两种情况之一
#   · 旧进程：图标在进程启动时就固定了，换掉 exe 不会影响**已经在运行**的窗口
#   · 幽灵按钮：Stop-Process -Force 之后任务栏可能留一个没有图标的空按钮，
#     鼠标悬停/点一下就会消失（自动化测试反复强杀 GitRT 时常见）
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$env:LOCALAPPDATA\Programs\GitRT\GitRT.exe",
    [string]$Repo = "$env:TEMP\GitRT-test\repo",
    [string]$Icon = "$PSScriptRoot\..\packaging\Assets\gitrt.ico",
    [string]$OutDir = "$PSScriptRoot\..\docs\images"
)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class Tb {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@

# ① 图标主色 = 出现次数最多的「不透明且非白」颜色
$ico = New-Object System.Drawing.Icon((Resolve-Path $Icon).Path, 32, 32)
$ib = $ico.ToBitmap()
$freq = @{}
for ($x = 0; $x -lt 32; $x++) {
    for ($y = 0; $y -lt 32; $y++) {
        $c = $ib.GetPixel($x, $y)
        if ($c.A -lt 200) { continue }
        if ($c.R -gt 235 -and $c.G -gt 235 -and $c.B -gt 235) { continue }
        $k = "$($c.R),$($c.G),$($c.B)"
        $freq[$k] = 1 + $(if ($freq.ContainsKey($k)) { $freq[$k] } else { 0 })
    }
}
$main = ($freq.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1)
if (-not $main) { Write-Host "❌ 图标里找不到主色"; exit 1 }
$key = $main.Key -split ','
$kr = [int]$key[0]; $kg = [int]$key[1]; $kb = [int]$key[2]
Write-Host "图标主色 = RGB($kr,$kg,$kb)（$($main.Value) 像素）"
$ib.Dispose(); $ico.Dispose()

# ② 启动 + 抓任务栏
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Start-Process $Exe -ArgumentList "`"$Repo`"" | Out-Null
Start-Sleep -Seconds 3
$tb = [Tb]::FindWindow('Shell_TrayWnd', $null)
if ($tb -eq [IntPtr]::Zero) { Write-Host "❌ 找不到任务栏"; exit 1 }
$r = New-Object Tb+RECT; [void][Tb]::GetWindowRect($tb, [ref]$r)
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap -ArgumentList $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$g.Dispose()
New-Item -ItemType Directory $OutDir -Force | Out-Null
$bmp.Save((Join-Path $OutDir 'taskbar-icon.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "任务栏位图: ${w}x${h} -> docs/images/taskbar-icon.png"

# ③ 逐列找实心砖块
$cols = @{}
for ($x = 0; $x -lt $w; $x++) {
    $n = 0
    for ($y = 0; $y -lt $h; $y++) {
        $c = $bmp.GetPixel($x, $y)
        if (([Math]::Abs($c.R - $kr) + [Math]::Abs($c.G - $kg) + [Math]::Abs($c.B - $kb)) -lt 90) { $n++ }
    }
    $cols[$x] = $n
}
$hits = @($cols.Keys | Where-Object { $cols[$_] -ge 12 } | Sort-Object)
$clusters = @()
if ($hits.Count -gt 0) {
    $s = $hits[0]; $p = $hits[0]
    foreach ($xx in $hits) { if ($xx - $p -gt 4) { $clusters += , @($s, $p); $s = $xx }; $p = $xx }
    $clusters += , @($s, $p)
}
$found = @($clusters | Where-Object { ($_[1] - $_[0] + 1) -ge 16 })
Write-Host "命中区块: $(($clusters | ForEach-Object { "x=$($_[0])..$($_[1])(宽$($_[1]-$_[0]+1))" }) -join ' , ')"
if ($found.Count -gt 0) {
    $c0 = $found[0]
    $bx = [Math]::Max(0, $c0[0] - 16)
    $bw = [Math]::Min($w - $bx, ($c0[1] - $c0[0] + 1) + 32)
    $crop = New-Object System.Drawing.Bitmap -ArgumentList $bw, $h
    $gc = [System.Drawing.Graphics]::FromImage($crop)
    $gc.DrawImage($bmp, (New-Object System.Drawing.Rectangle -ArgumentList 0,0,$bw,$h), (New-Object System.Drawing.Rectangle -ArgumentList $bx,0,$bw,$h), [System.Drawing.GraphicsUnit]::Pixel)
    $gc.Dispose()
    $zoom = New-Object System.Drawing.Bitmap -ArgumentList ($bw * 5), ($h * 5)
    $gz = [System.Drawing.Graphics]::FromImage($zoom)
    $gz.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $gz.DrawImage($crop, 0, 0, $bw * 5, $h * 5)
    $gz.Dispose()
    $zoom.Save((Join-Path $OutDir 'taskbar-gitrt-button.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "裁图: docs/images/taskbar-gitrt-button.png（5x）"
    Write-Host "== 结果: PASS —— 任务栏上确实画出了 GitRT 图标（实心块 x=$($c0[0])..$($c0[1])）=="
    $crop.Dispose(); $zoom.Dispose()
} else {
    Write-Host "== 结果: FAIL —— 任务栏位图里找不到 GitRT 图标主色块 =="
}
$bmp.Dispose()
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
