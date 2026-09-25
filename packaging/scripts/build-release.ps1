# ---------------------------------------------------------------------------
# build-release.ps1 —— 组装可发布的 Release 目录 + zip（CI 与本地共用）
#
#   产物（默认 build/dist/）：
#     GitRT-<version>-win-x64\
#       ├─ app\        GitRT.exe / GitRT.Shell.dll / GitRT.ShellProbe.exe   ← ExternalLocation
#       ├─ package\    AppxManifest.xml + Assets\*                          ← 稀疏身份包
#       ├─ install.ps1 / uninstall.ps1
#       ├─ README.md / LICENSE / THIRD-PARTY-NOTICES.md
#       └─ 版本信息.txt（构建时间、git 提交、文件大小/哈希）
#     GitRT-<version>-win-x64.zip
#
#   用法： pwsh -File packaging/scripts/build-release.ps1 [-Config Release] [-OutDir build] [-NoZip]
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [string]$OutDir = "$PSScriptRoot\..\..\build",
    # ★ 不能把参数叫 $Version：PowerShell 变量名**大小写不敏感**，会和下面内部的 $version 撞成同一个，
    #   于是 $version = $identity.version 会把参数值覆盖掉、覆盖静默失效（踩过）。保留 -Version 别名照样能传。
    [Alias('Version')]
    [string]$PackageVersion = '',   # 空 = 用 packaging/identity.json 的；CI 传【最新 tag +0.0.1】
    [string]$DistDir = '',
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
$OutDir   = [System.IO.Path]::GetFullPath($OutDir)
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildDir = Join-Path $OutDir $Config

function Say([string]$m, [string]$c = 'Gray') { Write-Host $m -ForegroundColor $c }

# ------------------------------------------------------------ 1) 找构建产物
function Find-Artifact([string]$name) {
    $hit = Get-ChildItem -Path $buildDir -Recurse -Filter $name -File -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' } | Select-Object -First 1
    if (-not $hit) { throw "构建产物里找不到 $name；请先构建：cmake --preset ucrt64-$($Config.ToLower()) && cmake --build build\$($Config.ToLower())" }
    return $hit.FullName
}
$exe   = Find-Artifact 'GitRT.exe'
$dll   = Find-Artifact 'GitRT.Shell.dll'
$probe = Get-ChildItem -Path $buildDir -Recurse -Filter 'GitRT.ShellProbe.exe' -File -ErrorAction SilentlyContinue |
         Select-Object -First 1

# --------------------------------------------------------------- 2) 版本/身份
$identity = Get-Content (Join-Path $repoRoot 'packaging\identity.json') -Raw | ConvertFrom-Json
$version  = $identity.version
if (-not $PackageVersion -and $env:GRT_VERSION) { $PackageVersion = $env:GRT_VERSION }   # CI 通过环境变量传
if ($PackageVersion) { $version = $PackageVersion }   # ★ 覆盖（-Version 是它的别名）
if (-not $version) { throw "packaging/identity.json 里没有 version" }
Say "  版本：$version（identity.json=$($identity.version)；-PackageVersion=[$PackageVersion]；env=[$env:GRT_VERSION]）" 'DarkGray'
$name = "GitRT-$version-win-x64"
if (-not $DistDir) { $DistDir = Join-Path $OutDir 'dist' }
$stage = Join-Path ([System.IO.Path]::GetFullPath($DistDir)) $name

# ------------------------------------------------------------ 3) 组装稀疏包
Say "组装稀疏包（含清单校验）→ $OutDir\sparse" 'Cyan'
& (Join-Path $PSScriptRoot 'build-msix.ps1') -Config $Config -OutDir $OutDir -LayoutOnly | Out-Host
$layout = Join-Path $OutDir 'sparse'
if (-not (Test-Path (Join-Path $layout 'AppxManifest.xml'))) { throw "稀疏包清单未生成：$layout" }

# ---------------------------------------------------------------- 4) 铺发布目录
Say "组装发布目录 → $stage" 'Cyan'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'app'), (Join-Path $stage 'package') | Out-Null

Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'app') -Force
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'app') -Force
if ($probe) { Copy-Item -LiteralPath $probe.FullName -Destination (Join-Path $stage 'app') -Force }
Copy-Item -Path (Join-Path $layout '*') -Destination (Join-Path $stage 'package') -Recurse -Force

foreach ($s in @('install.ps1', 'uninstall.ps1', '使用说明.txt')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $s) -Destination $stage -Force
}
foreach ($d in @('README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md')) {
    $src = Join-Path $repoRoot $d
    if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination $stage -Force }
}

# 版本信息（便于排错：用户报问题时要的就是这几行）
$commit = (& git -C $repoRoot rev-parse --short HEAD 2>$null)
if (-not $commit) { $commit = '(unknown)' }
$lines = @(
    "GitRT $version  ($Config)",
    "构建时间 : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
    "git 提交 : $commit",
    "构建机器 : $([System.Environment]::OSVersion.VersionString)",
    "",
    "文件："
)
Get-ChildItem -Recurse -File (Join-Path $stage 'app') | ForEach-Object {
    $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.Substring(0, 16).ToLower()
    $lines += ("  {0,-24} {1,10:N0} bytes  sha256:{2}…" -f $_.Name, $_.Length, $hash)
}
Set-Content -LiteralPath (Join-Path $stage '版本信息.txt') -Value ($lines -join "`r`n") -Encoding UTF8

Get-ChildItem -Recurse -File $stage | ForEach-Object {
    Say ("  {0}  {1:N0} bytes" -f $_.FullName.Substring($stage.Length + 1), $_.Length) 'DarkGray'
}

# ------------------------------------------------- 4.5) 强校验：这些文件缺一个就是发坏包
$mustHave = @('install.ps1', 'uninstall.ps1', '使用说明.txt', 'app\GitRT.exe', 'README.md')
$missing = @()
foreach ($m in $mustHave) { if (-not (Test-Path (Join-Path $stage $m))) { $missing += $m } }
if ($missing.Count -gt 0) {
    throw "发布目录缺少必需文件：$($missing -join ', ')（发布包必须带安装/卸载脚本和使用说明）"
}
Say "  [OK] 必需文件齐备：$($mustHave -join ' / ')" 'Green'
# --------------------------------------------------------------------- 5) 打包
if (-not $NoZip) {
    $zip = "$stage.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Say "压缩 → $zip" 'Cyan'
    Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
    Say ("  {0:N0} bytes" -f (Get-Item $zip).Length) 'DarkGray'
    # ★ 直接读 zip 条目核对：zips 里必须能看到安装/卸载脚本和使用说明（用户踩过"下的包里没有"）
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $za = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $names = $za.Entries | ForEach-Object { $_.FullName }
        $need = @('install.ps1', 'uninstall.ps1', '使用说明.txt', 'app/GitRT.exe')
        $gone = @()
        foreach ($n in $need) { if (-not ($names | Where-Object { $_ -like "*$n" })) { $gone += $n } }
        if ($gone.Count -gt 0) { throw "zip 里缺少：$($gone -join ', ')" }
        Say "  [OK] zip 内含：$($need -join ' / ')（共 $($names.Count) 项）" 'Green'
    } finally { $za.Dispose() }
}

Say ""
Say "完成 ✅" 'Green'
Say "  发布目录：$stage" 'Yellow'
Say "  用户安装：在解压出来的目录里跑 pwsh -File install.ps1" 'Yellow'
Say "            → 会装到 <该目录所在盘符的根>\gitRT，并写注册表 HKCU\Software\GitRT" 'Yellow'
