# ---------------------------------------------------------------------------
# dev-uninstall.ps1 —— 卸载身份包 + 清理开发期痕迹
#   《技术实现设计》§11.4 / §11.5 的"卸载残留清单"
#
#   ★ 必须在**独立进程**里卸载：在已加载该包的会话内直接 Remove-AppxPackage
#     容易留下 0x80073CFA 半卸载残状态（社区经验，§3.2）。
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\GitRT",
    [switch]$RemoveData,
    [switch]$NoExplorerRestart
)
$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$identity = Get-Content (Join-Path $repoRoot 'packaging\identity.json') -Raw | ConvertFrom-Json

$pkg = Get-AppxPackage -Name $identity.packageName -ErrorAction SilentlyContinue
if ($pkg) {
    Write-Host "卸载身份包 $($pkg.PackageFullName)（独立进程）" -ForegroundColor Cyan
    Start-Process powershell -Wait -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-Command', "Get-AppxPackage -Name $($identity.packageName) | Remove-AppxPackage"
    )
    Start-Sleep -Milliseconds 500
    $still = Get-AppxPackage -Name $identity.packageName -ErrorAction SilentlyContinue
    if ($still) {
        Write-Host "警告：身份包仍在（$($still.PackageFullName)）——可能需要注销/重启后再试" -ForegroundColor Yellow
    } else {
        Write-Host "身份包已移除" -ForegroundColor Green
    }
} else {
    Write-Host "未发现已注册的身份包（跳过）" -ForegroundColor DarkGray
}

if (Test-Path $InstallDir) {
    Write-Host "删除安装目录 $InstallDir" -ForegroundColor Cyan
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
}

# 开发期传统注册（tools/dev 用过 regsvr32 时留下的键）
$legacy = @(
    'HKCU:\Software\Classes\Directory\shell\GitRTTest',
    'HKCU:\Software\Classes\Directory\Background\shell\GitRTTest',
    'HKCU:\Software\Classes\*\shell\GitRTTest'
)
foreach ($k in $legacy) {
    if (Test-Path $k) {
        Remove-Item $k -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "  已清理传统注册：$k" -ForegroundColor DarkGray
    }
}

if ($RemoveData) {
    foreach ($d in @("$env:APPDATA\GitRT", "$env:LOCALAPPDATA\GitRT")) {
        if (Test-Path $d) {
            Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "  已删除数据目录：$d" -ForegroundColor DarkGray
        }
    }
} else {
    Write-Host "保留配置与日志（加 -RemoveData 可一并删除）：$env:APPDATA\GitRT、$env:LOCALAPPDATA\GitRT" -ForegroundColor DarkGray
}

if (-not $NoExplorerRestart) {
    & (Join-Path $PSScriptRoot 'dev-reload.ps1')
}
Write-Host "卸载完成。右键菜单里的 GitRT 应已消失。" -ForegroundColor Green
