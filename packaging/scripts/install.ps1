# ---------------------------------------------------------------------------
# install.ps1 —— 面向用户的安装（发布包内自带；不需要源码树、不需要管理员）
#
#   安装位置由**运行时的当前目录**决定（这是刻意的：绿色安装，装到随手放的位置）：
#       pwd = A:\Downloads\GitRT-0.1.0.0-win-x64\   →   安装到  A:\gitRT\
#       pwd = C:\Users\me\Downloads\...\            →   安装到  C:\gitRT\   （系统盘可能需要管理员）
#   即：**取 pwd 的盘符根目录 + \gitRT**。
#
#   做四件事：
#     1) 检查 pwd / 目标盘可写 / 发布包文件齐备（缺文件直接报错，不半装）
#     2) 停掉正在运行的 GitRT → 把 app\ 里的二进制 + package\（稀疏包清单与图标）
#        连同 README/LICENSE 释放到 <盘根>\gitRT
#     3) **写入注册表** HKCU\Software\GitRT：InstallDir / Version / InstalledAt /
#        UninstallString（uninstall.ps1 靠它找到目录并清理；不需要管理员权限）
#     4) 冒烟自检（--list-commands）+ 免签名注册右键菜单（可用 -NoShellMenu 跳过）
#
#   用法：
#     pwsh -File install.ps1                          # 默认：装到 <pwd 盘根>\gitRT
#     pwsh -File install.ps1 -InstallDir D:\tools\GitRT   # 指定目录（仍然写注册表）
#     pwsh -File install.ps1                          # 默认**连右键菜单一起装**（需开发者模式）
#     pwsh -File install.ps1 -WithShellMenu:$false       # 只要程序，不注册右键菜单（默认是 true）
#     pwsh -File install.ps1 -DryRun                  # 只打印将做什么，不动系统
#
#   卸载：pwsh -File uninstall.ps1
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Source = $PSScriptRoot,
    [string]$InstallDir = '',          # 空 = <pwd 盘根>\gitRT
    [string]$RegistryKey = 'HKCU:\Software\GitRT',   # 记录安装信息的位置（自动化测试会改成测试键，绝不动真实安装）
    [bool]$WithShellMenu = $true,    # ★ 默认 **true**：安装时连右键菜单一起注册；跳过用 -WithShellMenu:$false
    [switch]$NoShellMenu,              # 等价写法（更直观）：不注册菜单
    [switch]$NoSmokeTest,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$registerMenu = ($WithShellMenu -and -not $NoShellMenu)   # ★ 默认 $true：装程序时连右键菜单一起注册

function Say([string]$m, [string]$c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Fail([string]$m) { Write-Host "错误：$m" -ForegroundColor Red; exit 1 }
function Step([string]$m) { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }

$Source = [System.IO.Path]::GetFullPath($Source)

# ---------------------------------------------------- 0) 检查 pwd 与目标目录
$pwdPath = (Get-Location).Path
$pwdRoot = [System.IO.Path]::GetPathRoot($pwdPath)          # 例如 A:\
if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    if ([string]::IsNullOrWhiteSpace($pwdRoot)) { Fail "拿不到当前目录的盘符根目录（pwd=$pwdPath）" }
    $InstallDir = Join-Path $pwdRoot 'gitRT'
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
$installRoot = [System.IO.Path]::GetPathRoot($InstallDir)

Say "GitRT 安装程序" 'Cyan'
Say "  当前目录 pwd ：$pwdPath"
Say "  pwd 盘根      ：$pwdRoot"
Say "  安装到        ：$InstallDir"
if ($DryRun) { Say "  （DryRun：只打印，不改动系统）" 'Yellow' }

# 发布包文件齐备？
$exe   = Join-Path $Source 'app\GitRT.exe'
$dll   = Join-Path $Source 'app\GitRT.Shell.dll'
$probe = Join-Path $Source 'app\GitRT.ShellProbe.exe'
foreach ($f in @($exe, $dll)) {
    if (-not (Test-Path $f)) {
        Fail "发布包不完整，缺少：$f`n（请在解压后的发布目录里运行本脚本：<包>\install.ps1）"
    }
}
Say "  [OK] 发布包文件齐备（$Source\app）" 'DarkGray'

# 目标盘/目录可写？（系统盘根目录写 <盘>:gitRT 可能要管理员）
if (-not $DryRun) {
    try {
        if (-not (Test-Path $installRoot)) { Fail "盘符不存在：$installRoot" }
        if (Test-Path $InstallDir) {
            $probeFile = Join-Path $InstallDir '.gitrt-write-test'
            Set-Content -LiteralPath $probeFile -Value 'x' -ErrorAction Stop
            Remove-Item -LiteralPath $probeFile -Force
        } else {
            New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
        }
    } catch {
        Fail "写不进 $InstallDir（$($_.Exception.Message)）`n提示：系统盘根目录通常需要管理员 PowerShell；也可以 -InstallDir 指定别处。"
    }
    Say "  [OK] 目标目录可写" 'DarkGray'
}

# 版本（从 exe 的文件版本拿，避免依赖清单）
$ver = (Get-Item $exe).VersionInfo.FileVersion
if ([string]::IsNullOrWhiteSpace($ver)) { $ver = '0.0.0.0' }
Say "  版本          ：$ver" 'DarkGray'

if ($DryRun) {
    Step 'DryRun 结束（未改动任何东西）'
    Say "  将会：停 GitRT → 复制 app\* 与 package\* → 写 HKCU\Software\GitRT → 冒烟自检" 'Yellow'
    if ($registerMenu) { Say "  还会：免签名注册稀疏身份包（Add-AppxPackage -Register）" 'Yellow' }
    exit 0
}

# ------------------------------------------------------------- 1) 停进程
Step '1/4 停掉正在运行的 GitRT'
$running = Get-Process GitRT -ErrorAction SilentlyContinue
if ($running) {
    Say "  结束 $($running.Count) 个进程（否则文件被占用）" 'Yellow'
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 600
} else {
    Say "  没有在跑的 GitRT" 'DarkGray'
}

# ------------------------------------------------------------- 2) 释放文件
Step "2/4 释放二进制到 $InstallDir"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Path (Join-Path $Source 'app\*') -Destination $InstallDir -Force
$pkgSrc = Join-Path $Source 'package'
if (Test-Path $pkgSrc) {
    # 稀疏包（清单 + 图标）留在安装目录，方便以后再次注册
    Copy-Item -Path $pkgSrc -Destination $InstallDir -Recurse -Force
}
foreach ($n in @('README.md', 'LICENSE', 'README.txt')) {
    $s = Join-Path $Source $n
    if (Test-Path $s) { Copy-Item -LiteralPath $s -Destination $InstallDir -Force }
}
foreach ($n in @('GitRT.exe', 'GitRT.Shell.dll', 'GitRT.ShellProbe.exe')) {
    $f = Join-Path $InstallDir $n
    if (Test-Path $f) { Say ("  {0,-24} {1,10:N0} bytes" -f $n, (Get-Item $f).Length) 'DarkGray' }
}

# 上次卸载备份的 AI 设置（含 Key）：只在当前没有时恢复
$keyFile = Join-Path $InstallDir 'GitRT.ai.json'
if (-not (Test-Path $keyFile)) {
    $bak = Get-ChildItem (Join-Path $env:APPDATA 'GitRT') -Directory -Filter 'uninstall-backup-*' `
             -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if ($bak -and (Test-Path (Join-Path $bak.FullName 'GitRT.ai.json'))) {
        Copy-Item -LiteralPath (Join-Path $bak.FullName 'GitRT.ai.json') -Destination $keyFile -Force
        Say "  [OK] 已恢复上次卸载备份的 AI 设置（$($bak.Name)）" 'Green'
    }
}

# ------------------------------------------------------------- 3) 写注册表
Step '3/4 写入注册表 HKCU\Software\GitRT'
$regPath = $RegistryKey
if (-not (Test-Path $regPath)) { New-Item -Path $regPath -Force | Out-Null }
$uninstallScriptInInstall = Join-Path $InstallDir 'uninstall.ps1'
# 把 uninstall.ps1 也放进安装目录：以后从任何地方都能卸载
$unSrc = Join-Path $Source 'uninstall.ps1'
if (Test-Path $unSrc) { Copy-Item -LiteralPath $unSrc -Destination $uninstallScriptInInstall -Force }

$uninstallCmd = 'pwsh -NoProfile -File "' + $uninstallScriptInInstall + '"'
New-ItemProperty -Path $regPath -Name 'InstallDir'      -Value $InstallDir            -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name 'Version'         -Value $ver                   -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name 'InstalledAt'     -Value (Get-Date -Format o)    -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name 'Source'          -Value $Source                 -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name 'ExePath'         -Value (Join-Path $InstallDir 'GitRT.exe') -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name 'UninstallString' -Value $uninstallCmd           -PropertyType String -Force | Out-Null
foreach ($n in @('InstallDir', 'Version', 'UninstallString')) {
    Say ("  {0,-16} = {1}" -f $n, (Get-ItemProperty -Path $regPath -Name $n).$n) 'DarkGray'
}
Say "  [OK] 注册表已写入（卸载时清掉）" 'Green'

# ------------------------------------------------------------- 4) 冒烟自检
Step '4/4 冒烟自检'
$exeInInstall = Join-Path $InstallDir 'GitRT.exe'
if (-not $NoSmokeTest) {
    $out = Join-Path ([System.IO.Path]::GetTempPath()) ("gitrt-install-smoke-{0}.txt" -f (Get-Random))
    # ★ 冒烟自检只是"顺便确认能启动"：起不来/超时都只警告，**不能**把安装判失败
    try {
        $p = Start-Process -FilePath $exeInInstall -ArgumentList @('--list-commands', '--out', $out) -PassThru
        if (-not $p.WaitForExit(60000)) { $p.Kill(); Say "  [警告] 冒烟自检超时（不影响安装）" 'Yellow' }
        elseif ($p.ExitCode -ne 0) { Say "  [警告] 冒烟自检返回 exit=$($p.ExitCode)（不影响安装）" 'Yellow' }
        elseif (Test-Path $out) {
            $n = (Select-String -Path $out -Pattern '^CMD\|' | Measure-Object).Count
            Say "  [OK] 能启动，命令表 $n 条" 'Green'
        } else {
            Say "  [警告] 冒烟自检没写报告 $out" 'Yellow'
        }
    } catch {
        Say "  [警告] 冒烟自检起不来（不影响安装）：$($_.Exception.Message)" 'Yellow'
    }
    Remove-Item $out -Force -ErrorAction SilentlyContinue
} else {
    Say "  （已跳过）" 'DarkGray'
}

# 可选：现代右键菜单（免签名稀疏包，需要开发者模式）
if ($registerMenu) {
    Step '注册资源管理器右键菜单（稀疏身份包，免签名）'
    $manifest = Join-Path $InstallDir 'package\AppxManifest.xml'
    if (-not (Test-Path $manifest)) {
        Say "  [跳过] 安装目录里没有 package\AppxManifest.xml" 'Yellow'
    } else {
        $unlock = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
        $dev = (Get-ItemProperty -Path $unlock -Name AllowDevelopmentWithoutDevLicense -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
        if ($dev -ne 1) {
            Say "  [跳过] 未开启「开发者模式」，免签名注册做不了 → 右键菜单不会出现。$([Environment]::NewLine)         开启方式：设置 → 系统 → 开发者选项 → 开发人员模式（管理员也可：New-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -Name AllowDevelopmentWithoutDevLicense -Value 1 -PropertyType DWORD -Force），然后重跑本脚本即可。" 'Yellow'
        } elseif (-not (Get-Command Get-AppxPackage -ErrorAction SilentlyContinue)) {
            Say "  [跳过] 没有 AppX 模块，右键菜单注册跳过（其它功能不受影响）" 'Yellow'
        } else {
            [xml]$xml = Get-Content -LiteralPath $manifest -Raw
            $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
            $ns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
            $pkgName = $xml.SelectSingleNode('/f:Package/f:Identity', $ns).Name
            try {
                $existing = Get-AppxPackage -Name $pkgName -ErrorAction SilentlyContinue
                if ($existing) {
                    Start-Process powershell -Wait -WindowStyle Hidden -ArgumentList @(
                        '-NoProfile', '-Command', "Get-AppxPackage -Name $pkgName | Remove-AppxPackage"
                    ) | Out-Null
                    Start-Sleep -Milliseconds 500
                }
                Add-AppxPackage -Register $manifest -ExternalLocation $InstallDir
                Say "  [OK] 已注册；重启资源管理器后右键可见" 'Green'
                Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 1200
                if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer }
            } catch {
                Say "  [警告] 注册失败（右键菜单不可用，其它功能不受影响）：$($_.Exception.Message)" 'Yellow'
            }
        }
    }
}

Say ""
Say "安装完成 ✅" 'Green'
Say "  程序：$exeInInstall"
Say "  注册表：HKCU\Software\GitRT（InstallDir=$InstallDir）" 'DarkGray'
Say "  卸载：pwsh -File `"$InstallDir\uninstall.ps1`"（或跑发布包里的 uninstall.ps1）" 'Yellow'
if (-not $registerMenu) {
    Say "  这次没注册右键菜单（-WithShellMenu:$false / -NoShellMenu）；想去掉这个开关重跑即可" 'Yellow'
}
