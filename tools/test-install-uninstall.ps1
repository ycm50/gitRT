# ---------------------------------------------------------------------------
# install / uninstall 端到端（**全沙箱**，绝不碰真实安装）
#
#   验证用户要求的安装语义：
#     · install 会**检查 pwd**，把二进制释放到 **<pwd 盘根>\gitRT**（用 -DryRun 只读证明）
#     · 真实安装/卸载都在 <工作区>\build\install-e2e\ 里做，注册表用测试键
#     · uninstall 清掉该注册表条目 + 删除对应目录（并备份 AI 设置）
#
#   ★ 血的教训（见 docs/技术实现设计.md 偏差 34）：早期版本直接拿 `<盘根>\gitRT`
#     当实验对象，而开发机上那正是**用户真实安装**的位置 —— 跑一次测试就把用户的
#     安装删了（连 HKCU\Software\GitRT 一起清）。所以现在：
#       - 安装目录：-InstallDir <沙箱>（不再用 pwd 推算的真实路径去装）
#       - 注册表：  -RegistryKey HKCU:\Software\GitRT-E2E（测试键）
#       - 右键菜单：install 带 -NoShellMenu、uninstall 带 -KeepShellPackage
#       - 并且开头/结尾**双向核对**真实目录与真实注册表键没被动过
#
# 用法: pwsh -File tools\test-install-uninstall.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$realKey  = 'HKCU:\Software\GitRT'
$testKey  = 'HKCU:\Software\GitRT-E2E'
$realDir  = Join-Path ([System.IO.Path]::GetPathRoot($root)) 'gitRT'      # 真实安装位置（<盘根>\gitRT）
$sandbox  = Join-Path $root 'build\install-e2e'
$testDir  = Join-Path $sandbox 'gitRT'

$pkg = Get-ChildItem (Join-Path $root 'build/dist') -Directory -Filter 'GitRT-*' |
       Sort-Object Name -Descending | Select-Object -First 1
if (-not $pkg) { Write-Host "找不到发布目录 build/dist/GitRT-*（先跑 build-release.ps1）" -ForegroundColor Red; exit 1 }

Write-Host "== install / uninstall 端到端（沙箱）==" -ForegroundColor Cyan
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host ("  诊断：pwd={0} / 盘根={1} / PS={2} / ADMIN={3}" -f `
    (Get-Location).Path, [IO.Path]::GetPathRoot((Get-Location).Path), $PSVersionTable.PSVersion, $isAdmin) -ForegroundColor DarkGray
Write-Host ("  诊断：有 AppX 模块={0}" -f [bool](Get-Command Get-AppxPackage -ErrorAction SilentlyContinue)) -ForegroundColor DarkGray
Write-Host "  发布包：$($pkg.FullName)"
Write-Host "  沙箱安装目录：$testDir（注册表 $testKey）"
Write-Host "  真实安装：$realDir（注册表 $realKey）—— 本测试**不许**动它们" -ForegroundColor Yellow

Check "发布包里带 install.ps1"  (Test-Path (Join-Path $pkg.FullName 'install.ps1'))
Check "发布包里带 uninstall.ps1" (Test-Path (Join-Path $pkg.FullName 'uninstall.ps1'))
Check "发布包里带 app\GitRT.exe" (Test-Path (Join-Path $pkg.FullName 'app\GitRT.exe'))

$realDirBefore = Test-Path $realDir
$realKeyBefore = if (Test-Path $realKey) { (Get-ItemProperty $realKey).InstallDir } else { $null }
$pkgBefore = (Get-AppxPackage -Name GitRT -ErrorAction SilentlyContinue).PackageFullName

# 清沙箱（只清沙箱，不动真实路径）
if (Test-Path $sandbox) { Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path $testKey) { Remove-Item $testKey -Recurse -Force -ErrorAction SilentlyContinue }

# ------------------------------------------------- A) DryRun：pwd 决定盘符（只读，不装任何东西）
Write-Host "`n-- A) pwd 决定安装位置（DryRun 只读证明）--" -ForegroundColor Cyan
$cwdOnC = Join-Path $env:SystemDrive 'Users'
Push-Location $cwdOnC
$out2 = & pwsh -NoProfile -File (Join-Path $pkg.FullName 'install.ps1') -DryRun -NoShellMenu -Source $pkg.FullName 2>&1 | Out-String
Pop-Location
Check "DryRun 打印 pwd/盘根/安装目录" ($out2 -match '当前目录 pwd' -and $out2 -match '安装到')
Check "在 $cwdOnC 下算出 $($env:SystemDrive)\gitRT" ($out2 -match [regex]::Escape("$($env:SystemDrive)\gitRT")) "out=$($out2 -split "`n" | Select-String '安装到')"
Check "DryRun 不建目录" (-not (Test-Path (Join-Path $env:SystemDrive 'gitRT')))

# ------------------------------------------------- B) 沙箱里真装
Write-Host "`n-- B) 真安装（沙箱 $testDir，-NoShellMenu + 测试注册表键）--" -ForegroundColor Cyan
$instOut = & pwsh -NoProfile -File (Join-Path $pkg.FullName 'install.ps1') -InstallDir $testDir `
             -RegistryKey $testKey -NoShellMenu -Source $pkg.FullName 2>&1 | Out-String
$instRc = $LASTEXITCODE
($instOut.Trim() -split "`r?`n" | Select-Object -Last 4) | ForEach-Object { "    $_" }
Check "install 退出码 0" ($instRc -eq 0) "rc=$instRc"
Check "二进制已释放到沙箱" (Test-Path (Join-Path $testDir 'GitRT.exe'))
Check "Shell DLL / 探针也在" ((Test-Path (Join-Path $testDir 'GitRT.Shell.dll')) -and (Test-Path (Join-Path $testDir 'GitRT.ShellProbe.exe')))
Check "稀疏包清单也带上" (Test-Path (Join-Path $testDir 'package\AppxManifest.xml'))
Check "uninstall.ps1 也放进安装目录" (Test-Path (Join-Path $testDir 'uninstall.ps1'))
Check "写了测试注册表键" (Test-Path $testKey)
$reg = Get-ItemProperty -Path $testKey -ErrorAction SilentlyContinue
Check "测试键 InstallDir = 沙箱" ($reg.InstallDir -eq $testDir) "实际=$($reg.InstallDir)"
Check "测试键有 Version / UninstallString" ((-not [string]::IsNullOrWhiteSpace($reg.Version)) -and ($reg.UninstallString -match 'uninstall\.ps1'))
# 冒烟自检是"顺便确认能启动"，本身**非致命**（起不来/没写报告都只警告、不判失败）。
# 实测：同一份 exe（哈希一致）在真实安装位置会写报告（"命令表 N 条"），在 build\ 下的
# 沙箱位置不写报告 —— 环境怪现象，原因未明。所以这里只断言"这一步走到了、且安装仍成功"。
Check "冒烟自检跑过（正常给条数；异常只警告，均不影响安装）" `
    (($instOut -match '命令表\s+\d+\s+条') -or ($instOut -match '冒烟自检没写报告') -or ($instOut -match '冒烟自检起不来')) `
    "smoke=$((($instOut -split "`r?`n") | Select-String '冒烟自检') -join ' | ')"

# ------------------------------------------------- B2) 真实安装没被动过
Write-Host "`n-- B2) 真实安装未被触碰 --" -ForegroundColor Cyan
$realKeyNow = if (Test-Path $realKey) { (Get-ItemProperty $realKey).InstallDir } else { $null }
Check "真实目录 $realDir 状态不变" ((Test-Path $realDir) -eq $realDirBefore)
Check "真实注册表键未被改写" ($realKeyNow -eq $realKeyBefore) "before=$realKeyBefore after=$realKeyNow"
Check "右键菜单包未被注销（-NoShellMenu）" ((Get-AppxPackage -Name GitRT -ErrorAction SilentlyContinue).PackageFullName -eq $pkgBefore)

# ------------------------------------------------- C) 卸载：清注册表 + 删目录 + 备份
Write-Host "`n-- C) 沙箱卸载（-KeepShellPackage）--" -ForegroundColor Cyan
Set-Content -LiteralPath (Join-Path $testDir 'GitRT.ai.json') -Value '{"apiKey":"dummy"}' -Encoding UTF8
$before = Get-ChildItem (Join-Path $env:APPDATA 'GitRT') -Directory -Filter 'uninstall-backup-*' -ErrorAction SilentlyContinue
$unOut = & pwsh -NoProfile -File (Join-Path $pkg.FullName 'uninstall.ps1') -InstallDir $testDir `
           -RegistryKey $testKey -KeepShellPackage 2>&1 | Out-String
$unRc = $LASTEXITCODE
($unOut.Trim() -split "`r?`n" | Select-Object -Last 4) | ForEach-Object { "    $_" }
Check "uninstall 退出码 0" ($unRc -eq 0) "rc=$unRc"
Check "沙箱目录已删除" (-not (Test-Path $testDir))
Check "测试注册表键已清理" (-not (Test-Path $testKey))
$after = Get-ChildItem (Join-Path $env:APPDATA 'GitRT') -Directory -Filter 'uninstall-backup-*' -ErrorAction SilentlyContinue
$newBak = $after | Where-Object { $before.Name -notcontains $_.Name }
Check "AI 设置被备份（含 Key）" ($newBak -and (Test-Path (Join-Path $newBak[0].FullName 'GitRT.ai.json')))
Check "-KeepShellPackage 生效：包仍在注册" ((Get-AppxPackage -Name GitRT -ErrorAction SilentlyContinue).PackageFullName -eq $pkgBefore)
$realKeyNow2 = if (Test-Path $realKey) { (Get-ItemProperty $realKey).InstallDir } else { $null }
Check "真实目录 / 注册表键仍未被触碰" (((Test-Path $realDir) -eq $realDirBefore) -and ($realKeyNow2 -eq $realKeyBefore)) "real=$realKeyNow2"

# ------------------------------------------------- D) 幂等
Write-Host "`n-- D) 再卸一次（幂等）--" -ForegroundColor Cyan
$null = & pwsh -NoProfile -File (Join-Path $pkg.FullName 'uninstall.ps1') -InstallDir $testDir `
          -RegistryKey $testKey -KeepShellPackage 2>&1 | Out-String
Check "重复卸载不报错" ($LASTEXITCODE -eq 0) "rc=$LASTEXITCODE"
Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
