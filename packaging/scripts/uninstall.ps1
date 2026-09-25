# ---------------------------------------------------------------------------
# uninstall.ps1 —— 卸载（发布包内自带；不需要管理员）
#
#   做三件事：
#     1) 从注册表 HKCU\Software\GitRT 找到安装目录（找不到就按"当前目录的盘根\gitRT"推断）
#     2) 备份 AI 设置（GitRT.ai.json，含 Key）到 %APPDATA%\GitRT\uninstall-backup-<时间>\
#        → 下次 install.ps1 会自动恢复
#     3) **删除该目录** + **清掉注册表条目**（顺手注销可选注册的右键菜单稀疏包）
#
#   用法：
#     pwsh -File uninstall.ps1                          # 按注册表里的位置卸载
#     pwsh -File uninstall.ps1 -InstallDir A:\gitRT    # 指定目录（同时清注册表）
#     pwsh -File uninstall.ps1 -DryRun                  # 只打印将做什么
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$InstallDir = '',
    [string]$RegistryKey = 'HKCU:\Software\GitRT',   # 与 install.ps1 对应；测试用测试键
    [switch]$KeepBackup,      # 默认会备份 AI 设置；加这个开关就跳过备份
    [switch]$KeepShellPackage, # 不动已注册的右键菜单（自动化测试用，避免注销掉用户真实的菜单）
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

function Say([string]$m, [string]$c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Step([string]$m) { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }

$regPath = $RegistryKey
$regDir = ''
if (Test-Path $regPath) {
    $regDir = (Get-ItemProperty -Path $regPath -Name 'InstallDir' -ErrorAction SilentlyContinue).InstallDir
}

# 目标目录：参数 > 注册表 > <当前目录盘根>\gitRT
if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    if (-not [string]::IsNullOrWhiteSpace($regDir)) {
        $InstallDir = $regDir
    } else {
        $root = [System.IO.Path]::GetPathRoot((Get-Location).Path)
        if ([string]::IsNullOrWhiteSpace($root)) { $root = 'C:\' }
        $InstallDir = Join-Path $root 'gitRT'
        Say "注册表里没有 HKCU\Software\GitRT\InstallDir，按当前目录盘根推断" 'Yellow'
    }
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

Say "GitRT 卸载程序" 'Cyan'
Say "  注册表      ：$(if (Test-Path $regPath) { 'HKCU\Software\GitRT（存在）' } else { 'HKCU\Software\GitRT（不存在）' })"
Say "  安装目录    ：$InstallDir$(if (Test-Path $InstallDir) { '（存在）' } else { '（不存在）' })"
if ($DryRun) { Say "  （DryRun：只打印，不改动系统）" 'Yellow' }

if ($DryRun) {
    Step 'DryRun 结束（未改动任何东西）'
    Say "  将会：备份 AI 设置 → 停 GitRT → 删除 $InstallDir → 删除 $regPath → 注销稀疏包（若有）" 'Yellow'
    exit 0
}

# ------------------------------------------------------------- 1) 停进程
Step '1/3 停掉正在运行的 GitRT'
$running = Get-Process GitRT -ErrorAction SilentlyContinue
if ($running) {
    Say "  结束 $($running.Count) 个进程（否则目录删不掉）" 'Yellow'
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 800
} else {
    Say "  没有在跑的 GitRT" 'DarkGray'
}

# --------------------------------------------------- 2) 备份 AI 设置（含 Key）
$backupNote = '（未备份）'
if (-not $KeepBackup) {
    $keyFile = Join-Path $InstallDir 'GitRT.ai.json'
    if (Test-Path $keyFile) {
        $dstDir = Join-Path (Join-Path $env:APPDATA 'GitRT') ('uninstall-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        Copy-Item -LiteralPath $keyFile -Destination $dstDir -Force
        $backupNote = $dstDir
        Say "  已备份 AI 设置：$dstDir" 'Green'
    } else {
        Say "  安装目录里没有 GitRT.ai.json（没什么可备份）" 'DarkGray'
    }
}

# ------------------------------------------------------------- 3) 删目录 + 清注册表
Step '2/3 删除安装目录'
if (Test-Path $InstallDir) {
    $n = (Get-ChildItem -LiteralPath $InstallDir -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object).Count
    Say "  $InstallDir（$n 个文件）" 'DarkGray'
    $ok = $false
    for ($try = 1; $try -le 5; $try++) {
        try {
            Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction Stop
            $ok = $true
            break
        } catch {
            Say "  第 $try 次删除失败：$($_.Exception.Message)" 'Yellow'
            Start-Sleep -Milliseconds 700
        }
    }
    if (-not $ok) {
        Say "  [警告] 目录没删干净（可能被占用）：$InstallDir" 'Yellow'
        Say "         手动关掉 GitRT/资源管理器后重跑，或手动删除该目录。" 'Yellow'
    } else {
        Say "  [OK] 已删除" 'Green'
    }
} else {
    Say "  目录不存在，跳过" 'DarkGray'
}

# 可选注册过的稀疏包：尽力注销（-KeepShellPackage 时跳过：自动化测试不许动用户真实的菜单）
# ★ 必须 try/catch + 先确认命令存在：非交互会话/没有 AppX 模块时 Get-AppxPackage 会抛
#   （属于"语句终止"错误，-ErrorAction SilentlyContinue 挡不住，会把卸载脚本整个带崩）
if ($KeepShellPackage) {
    Say "  （-KeepShellPackage：保留已注册的右键菜单，不注销）" 'DarkGray'
} else {
try {
    if (-not (Get-Command Get-AppxPackage -ErrorAction SilentlyContinue)) {
        Say "  （没有 AppX 模块，跳过稀疏包注销）" 'DarkGray'
    } else {
        $pkg = Get-AppxPackage -Name 'GitRT' -ErrorAction SilentlyContinue
        if ($pkg) {
            Say "  注销稀疏身份包 $($pkg.PackageFullName)（若注册过）" 'DarkGray'
            Start-Process powershell -Wait -WindowStyle Hidden -ArgumentList @(
                '-NoProfile', '-Command', "Get-AppxPackage -Name GitRT | Remove-AppxPackage"
            ) | Out-Null
            Say "  [OK] 已注销" 'Green'
        }
    }
} catch {
    Say "  [警告] 注销稀疏包失败（可忽略，不影响目录/注册表清理）：$($_.Exception.Message)" 'Yellow'
}
}

Step '3/3 清理注册表'
if (Test-Path $regPath) {
    try {
        Remove-Item -Path $regPath -Recurse -Force -ErrorAction Stop
        Say "  [OK] 已删除 $regPath" 'Green'
    } catch {
        Say "  [警告] 删不掉 $regPath：$($_.Exception.Message)" 'Yellow'
    }
} else {
    Say "  没有这个键，跳过" 'DarkGray'
}

Say ""
Say "卸载完成 ✅" 'Green'
Say "  AI 设置备份：$backupNote" 'DarkGray'
Say "  目录 $(if (Test-Path $InstallDir) { '仍在（见上面的警告）' } else { '已清空' })：$InstallDir"
