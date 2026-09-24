# ---------------------------------------------------------------------------
# make-assets.ps1 —— 生成 shell/打包需要的图形资源（无第三方依赖，用 System.Drawing）
#   packaging/Assets/gitrt.ico        （16/32/48，BMP/DIB 条目，最兼容）
#   packaging/Assets/gitrt-warn.ico   （危险命令图标）
#   packaging/Assets/Square44x44Logo.png / Square150x150Logo.png / storelogo.png
#
# 为什么用脚本而不是"提交一堆二进制"：图标/徽标是**可复现产物**，脚本入库后
# 任何人（含 CI）都能重新生成，避免"这张 png 是谁怎么做的"这类不可审计状态。
# 重新生成：pwsh -File packaging/scripts/make-assets.ps1
# ---------------------------------------------------------------------------
[CmdletBinding()]
param([string]$OutDir = (Join-Path $PSScriptRoot '..\Assets'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function New-IconBitmap {
    param([int]$Size, [ValidateSet('app', 'warn')][string]$Kind)

    $bmp = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    # 圆角方块背景（Git 橙 / 警示琥珀）
    $bg = if ($Kind -eq 'app') { [System.Drawing.Color]::FromArgb(255, 240, 80, 51) }
          else { [System.Drawing.Color]::FromArgb(255, 217, 119, 6) }
    $r = [double]$Size * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($Size - $d, 0, $d, $d, 270, 90)
    $path.AddArc($Size - $d, $Size - $d, $d, $d, 0, 90)
    $path.AddArc(0, $Size - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.SolidBrush($bg)
    $g.FillPath($brush, $path)

    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, [float]($Size * 0.085))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

    if ($Kind -eq 'app') {
        # 分支图形：左上一颗、左下一颗（主干）+ 右中一颗（分支）
        $lt = New-Object System.Drawing.PointF([float]($Size * 0.35), [float]($Size * 0.27))
        $lb = New-Object System.Drawing.PointF([float]($Size * 0.35), [float]($Size * 0.73))
        $rm = New-Object System.Drawing.PointF([float]($Size * 0.68), [float]($Size * 0.50))
        $g.DrawLine($pen, $lt, $lb)
        $g.DrawLine($pen, $lb, $rm)
        $dot = [float]($Size * 0.115)
        foreach ($p in @($lt, $lb, $rm)) {
            $g.FillEllipse($white, [float]($p.X - $dot / 2), [float]($p.Y - $dot / 2), $dot, $dot)
        }
    } else {
        # 感叹号
        $barW = [float]($Size * 0.13)
        $bar = New-Object System.Drawing.RectangleF([float]($Size * 0.435), [float]($Size * 0.22), $barW, [float]($Size * 0.42))
        $g.FillRectangle($white, $bar)
        $dot = [float]($Size * 0.14)
        $g.FillEllipse($white, [float]($Size * 0.43), [float]($Size * 0.72), $dot, $dot)
    }

    $pen.Dispose(); $brush.Dispose(); $white.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# BITMAPINFOHEADER + BGRA（自下而上）+ AND 掩码 —— ICO 里的 DIB 条目
function ConvertTo-DibBytes {
    param([System.Drawing.Bitmap]$Bitmap)
    $s = $Bitmap.Width
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint32]40); $bw.Write([int32]$s); $bw.Write([int32]($s * 2))
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
    $bw.Write([uint32]($s * $s * 4)); $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    for ($y = $s - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $s; $x++) {
            $c = $Bitmap.GetPixel($x, $y)
            $bw.Write([byte]$c.B); $bw.Write([byte]$c.G); $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
        }
    }
    $maskRow = [math]::Floor(($s + 31) / 32) * 4
    $mask = New-Object byte[] ($maskRow * $s)
    $bw.Write($mask)
    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return , $bytes
}

function New-IcoFile {
    param([string]$Path, [int[]]$Sizes, [ValidateSet('app', 'warn')][string]$Kind)
    $entries = @()
    foreach ($s in $Sizes) {
        $bmp = New-IconBitmap -Size $s -Kind $Kind
        $entries += [pscustomobject]@{ Size = $s; Data = (ConvertTo-DibBytes -Bitmap $bmp) }
        $bmp.Dispose()
    }
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$entries.Count)
    $offset = 6 + 16 * $entries.Count
    foreach ($e in $entries) {
        $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
        $bw.Write([byte]$dim); $bw.Write([byte]$dim)
        $bw.Write([byte]0); $bw.Write([byte]0)
        $bw.Write([uint16]1); $bw.Write([uint16]32)
        $bw.Write([uint32]$e.Data.Length); $bw.Write([uint32]$offset)
        $offset += $e.Data.Length
    }
    foreach ($e in $entries) { $bw.Write($e.Data) }
    $bw.Flush()
    [System.IO.File]::WriteAllBytes($Path, $ms.ToArray())
    $bw.Dispose(); $ms.Dispose()
    Write-Host ("  {0}  {1} bytes" -f (Split-Path $Path -Leaf), (Get-Item $Path).Length)
}

function Save-Png {
    param([string]$Path, [int]$Size, [ValidateSet('app', 'warn')][string]$Kind)
    $bmp = New-IconBitmap -Size $Size -Kind $Kind
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("  {0}  {1}x{1}  {2} bytes" -f (Split-Path $Path -Leaf), $Size, (Get-Item $Path).Length)
}

Write-Host "生成图标/徽标 → $OutDir"
New-IcoFile -Path (Join-Path $OutDir 'gitrt.ico') -Sizes @(16, 24, 32, 48) -Kind 'app'
New-IcoFile -Path (Join-Path $OutDir 'gitrt-warn.ico') -Sizes @(16, 24, 32, 48) -Kind 'warn'
Save-Png -Path (Join-Path $OutDir 'Square44x44Logo.png') -Size 44 -Kind 'app'
Save-Png -Path (Join-Path $OutDir 'Square150x150Logo.png') -Size 150 -Kind 'app'
Save-Png -Path (Join-Path $OutDir 'storelogo.png') -Size 50 -Kind 'app'
Write-Host "完成。"
