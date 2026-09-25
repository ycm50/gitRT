# ---------------------------------------------------------------------------
# 验证：任务栏图标 == 右键菜单图标（同一张 gitrt.ico）
#
#   ① 从 GitRT.exe 与 GitRT.Shell.dll 各取第一个图标资源（都是 101 = gitrt.ico），
#      转成位图后做**像素级哈希比对** —— 相等才算"一致"
#   ② 同一个哈希必须区别于系统通用图标（防止"两边都是通用图标"这种假通过）
#   ③ 启动 GitRT，读主窗口图标（WM_GETICON）并**抓任务栏**作为肉眼证据
#
# 用法: pwsh -File tools\demo-taskbar-icon.ps1 [-Exe <GitRT.exe>] [-Dll <GitRT.Shell.dll>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$Dll = "$PSScriptRoot\..\build\debug\src\shell\GitRT.Shell.dll",
    [string]$OutDir = "$PSScriptRoot\..\docs\images",
    [string]$Repo = "$env:TEMP\GitRT-test\repo"
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory $OutDir -Force | Out-Null
Add-Type -AssemblyName System.Drawing
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" }
}

Add-Type @"
using System;using System.Runtime.InteropServices;
public class Ico {
 [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
 public static extern uint ExtractIconEx(string file, int index, out IntPtr big, out IntPtr small, uint count);
 [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@

function Hash-Icon($hIcon) {
    $bmp = [System.Drawing.Icon]::FromHandle($hIcon).ToBitmap()
    $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($data.Stride * $data.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $hash = [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
    $size = "$($bmp.Width)x$($bmp.Height)"
    $bmp.Dispose()
    return [pscustomobject]@{ Hash = ($hash | ForEach-Object { $_.ToString('x2') }) -join ''; Size = $size }
}

function First-Icon($path) {
    $big = [IntPtr]::Zero; $small = [IntPtr]::Zero
    $n = [Ico]::ExtractIconEx((Resolve-Path $path).Path, 0, [ref]$big, [ref]$small, 1)
    return [pscustomobject]@{ Count = $n; Big = $big; Small = $small }
}

# ---- ① 两个二进制里的第一个图标资源必须像素一致 ----
$exeIcon = First-Icon $Exe
$dllIcon = First-Icon $Dll
Check "GitRT.exe 含图标资源" ($exeIcon.Count -ge 1 -and $exeIcon.Big -ne [IntPtr]::Zero) "count=$($exeIcon.Count)"
Check "GitRT.Shell.dll 含图标资源（右键菜单用的那张）" `
      ($dllIcon.Count -ge 1 -and $dllIcon.Big -ne [IntPtr]::Zero) "count=$($dllIcon.Count)"
if ($exeIcon.Big -ne [IntPtr]::Zero -and $dllIcon.Big -ne [IntPtr]::Zero) {
    $hExe = Hash-Icon $exeIcon.Big
    $hDll = Hash-Icon $dllIcon.Big
    $hGeneric = Hash-Icon ([System.Drawing.SystemIcons]::Application.Handle)
    Write-Host "    exe  : $($hExe.Size) $($hExe.Hash.Substring(0,16))…"
    Write-Host "    shell: $($hDll.Size) $($hDll.Hash.Substring(0,16))…"
    Write-Host "    通用 : $($hGeneric.Size) $($hGeneric.Hash.Substring(0,16))…"
    Check "任务栏图标 == 右键菜单图标（像素级一致）" ($hExe.Hash -eq $hDll.Hash) "exe=$($hExe.Hash) dll=$($hDll.Hash)"
    Check "两者都不是系统通用图标（排除假通过）" `
          ($hExe.Hash -ne $hGeneric.Hash -and $hDll.Hash -ne $hGeneric.Hash) "generic=$($hGeneric.Hash)"
}

# ---- ② 运行中的主窗口图标 + 抓任务栏 ----
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
$app = Start-Process (Resolve-Path $Exe).Path -ArgumentList "`"$Repo`"" -PassThru
Start-Sleep -Seconds 3
$main = [Ico]::FindWindow('GitRT.MainWindow', $null)
Check "主窗口出现" ($main -ne [IntPtr]::Zero) ''
if ($main -ne [IntPtr]::Zero) {
    $ICON_SMALL = 0; $ICON_BIG = 1; $WM_GETICON = 0x007F
    $small = [Ico]::SendMessage($main, $WM_GETICON, [IntPtr]$ICON_SMALL, [IntPtr]::Zero)
    $big = [Ico]::SendMessage($main, $WM_GETICON, [IntPtr]$ICON_BIG, [IntPtr]::Zero)
    Check "窗口报告了应用图标（任务栏取的就是它）" `
          ($small -ne [IntPtr]::Zero -and $big -ne [IntPtr]::Zero) "small=$small big=$big"
    $tb = [Ico]::FindWindow('Shell_TrayWnd', $null)
    if ($tb -ne [IntPtr]::Zero) {
        $r = New-Object Ico+RECT
        [void][Ico]::GetWindowRect($tb, [ref]$r)
        $w = $r.R - $r.L; $h = $r.B - $r.T
        if ($w -gt 0 -and $h -gt 0) {
            $bmp = New-Object System.Drawing.Bitmap($w, $h)
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $h)))
            $bmp.Save((Join-Path $OutDir 'taskbar-icon.png'), [System.Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
            Write-Host "    抓屏（任务栏）-> docs/images/taskbar-icon.png (${w}x${h})"
        }
    } else {
        Write-Host "    （找不到 Shell_TrayWnd，跳过任务栏抓屏）"
    }
}
Get-Process GitRT -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($h in @($exeIcon.Big, $dllIcon.Big)) { if ($h -ne [IntPtr]::Zero) { [void][Ico]::DestroyIcon($h) } }

Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 =="
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
