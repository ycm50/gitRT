# ---------------------------------------------------------------------------
# dev-install.ps1 —— 开发期安装：铺文件 + 注册稀疏身份包 + 重启资源管理器
#   《技术实现设计》§11.4 / §11.5
#
#   默认走 **免签名 loose 注册**（Add-AppxPackage -Register）：
#   开发者模式下不需要 Windows SDK、不需要签名（《产品设计》§3.10 的 S2）。
#   若已用 build-msix.ps1 生成并签名了 msix，用 -Msix 切换。
#
#   注意：注册/升级后**必须重启 explorer.exe**，否则菜单不出现（§3.2 流程 6）。
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\GitRT",
    [string]$OutDir     = "$PSScriptRoot\..\..\build",
    [string]$Config     = 'Release',
    [switch]$Msix,
    [switch]$NoExplorerRestart,
    [switch]$SkipPackage
)
$ErrorActionPreference = 'Stop'
$OutDir   = [System.IO.Path]::GetFullPath($OutDir)
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildDir = Join-Path $OutDir $Config

function Find-Artifact([string]$name) {
    $hit = Get-ChildItem -Path $buildDir -Recurse -Filter $name -File -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' } | Select-Object -First 1
    if (-not $hit) { throw "构建产物里找不到 $name；请先构建（cmake --build build\$Config）" }
    return $hit.FullName
}

$exe  = Find-Artifact 'GitRT.exe'
$dll  = Find-Artifact 'GitRT.Shell.dll'
$probe = Get-ChildItem -Path $buildDir -Recurse -Filter 'GitRT.ShellProbe.exe' -File -ErrorAction SilentlyContinue |
         Select-Object -First 1

Write-Host "安装目录：$InstallDir" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -LiteralPath $exe -Destination $InstallDir -Force
Copy-Item -LiteralPath $dll -Destination $InstallDir -Force
if ($probe) { Copy-Item -LiteralPath $probe.FullName -Destination $InstallDir -Force }
Write-Host "  已铺：GitRT.exe, GitRT.Shell.dll$([bool]$probe ? ', GitRT.ShellProbe.exe' : '')" -ForegroundColor DarkGray

if (-not $SkipPackage) {
    # 同版本重复注册会 0x80073CF9（§3.2）→ 先卸载（必须独立进程，避免 0x80073CFA）
    $existing = Get-AppxPackage -Name (Get-Content (Join-Path $repoRoot 'packaging\identity.json') -Raw |
                                       ConvertFrom-Json).packageName -ErrorAction SilentlyContinue
    if ($existing) {
        Write-Host "发现已注册的 $($existing.PackageFullName) → 先卸载" -ForegroundColor Yellow
        Start-Process powershell -Wait -WindowStyle Hidden -ArgumentList @(
            '-NoProfile', '-Command',
            "Get-AppxPackage -Name $($existing.Name) | Remove-AppxPackage"
        )
    }

    if ($Msix) {
        $msixPath = Join-Path $OutDir 'GitRT.msix'
        if (-not (Test-Path $msixPath)) { throw "找不到 $msixPath；请先运行 build-msix.ps1" }
        Write-Host "注册已签名稀疏包：$msixPath" -ForegroundColor Cyan
        Add-AppxPackage -Path $msixPath -ExternalLocation $InstallDir
    } else {
        $layout = Join-Path $OutDir 'sparse'
        # ★ 每次都重新组装：稀疏包目录里的 AppxManifest.xml 是**拷贝**，
        #   若只在"不存在时"生成，改了清单/identity.json 后会注册到旧清单
        #   （表现是报 0x80080204 之类的"明明已经改了"的怪错误）。
        Write-Host "组装稀疏包目录（含清单校验）" -ForegroundColor Cyan
        & (Join-Path $PSScriptRoot 'build-msix.ps1') -Config $Config -OutDir $OutDir -LayoutOnly
        if (-not (Test-Path (Join-Path $layout 'AppxManifest.xml'))) { throw "稀疏包清单未生成：$layout" }
        Write-Host "免签名 loose 注册：$layout\AppxManifest.xml" -ForegroundColor Cyan
        Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml') -ExternalLocation $InstallDir
    }
    $pkg = Get-AppxPackage -Name (Get-Content (Join-Path $repoRoot 'packaging\identity.json') -Raw |
                                  ConvertFrom-Json).packageName
    if (-not $pkg) { throw "身份包注册失败（Get-AppxPackage 查不到）" }
    Write-Host "已注册：$($pkg.PackageFullName)" -ForegroundColor Green
    Write-Host "  安装位置：$($pkg.InstallLocation)" -ForegroundColor DarkGray
}

if (-not $NoExplorerRestart) {
    Write-Host "重启资源管理器（菜单注册后必须重启才会出现）" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'dev-reload.ps1')
}

Write-Host ""
Write-Host "验证：在任意 Git 仓库目录的空白处右键 → 应能看到「GitRT」子菜单。" -ForegroundColor Yellow
Write-Host "若看不到，先跑自检： & `"$InstallDir\GitRT.ShellProbe.exe`" --dump `"$InstallDir`"" -ForegroundColor Yellow
Write-Host "卸载： pwsh -File packaging\scripts\dev-uninstall.ps1" -ForegroundColor Yellow
