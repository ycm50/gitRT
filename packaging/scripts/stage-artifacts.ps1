<#
  .SYNOPSIS
    把"下载下来的构建产物"归位成构建目录布局，供 build-release.ps1 / build-msix.ps1 使用。

  .DESCRIPTION
    CI 的 package job 不再自己重装 MSYS2 + 重建 Release，而是复用 build job 上传的
    `gitrt-binaries-<Config>` 产物（省掉一整段工具链安装与编译）。

    但 actions/download-artifact 落地的目录层级**不保证**与本地构建一致 —— 它取决于
    upload-artifact 计算出的公共父目录，可能平铺、也可能带中间层。而打包脚本期望的是：

        <OutDir>\<Config>\src\gui\GitRT.exe
        <OutDir>\<Config>\src\shell\GitRT.Shell.dll
        <OutDir>\<Config>\src\shellprobe\GitRT.ShellProbe.exe
        <OutDir>\<Config>\packaging\AppxManifest.xml

    本脚本把这 4 个文件在 <SearchRoot> 下递归找到并复制到位；已经就位的原样保留（幂等，
    两种布局都能吃下）。找不到就抛错，并把"看到的文件清单"打出来便于排错。

  .EXAMPLE
    # CI 里（产物已下载到 build\）
    pwsh -File packaging\scripts\stage-artifacts.ps1 -Config Release -OutDir build -SearchRoot build

  .EXAMPLE
    # 本地：手动下载 gitrt-binaries-Release 产物解压到 .\dl\ 后，不重编译直接重打包
    pwsh -File packaging\scripts\stage-artifacts.ps1 -SearchRoot dl -OutDir build
    pwsh -File packaging\scripts\build-release.ps1 -Config Release -OutDir build
#>
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [string]$OutDir = 'build',
    [string]$SearchRoot = '',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
if (-not $SearchRoot) { $SearchRoot = $OutDir } else { $SearchRoot = [System.IO.Path]::GetFullPath($SearchRoot) }
if (-not (Test-Path $SearchRoot)) { throw "找不到搜索根目录：$SearchRoot（产物下载到哪儿了？）" }

function Say([string]$s, [string]$color = 'Gray') { if (-not $Quiet) { Write-Host $s -ForegroundColor $color } }

# 期望相对路径 → 文件名（同一文件名在本工程里是唯一的，可以直接按名字找）
$want = [ordered]@{
    'src\gui\GitRT.exe'                   = 'GitRT.exe'
    'src\shell\GitRT.Shell.dll'           = 'GitRT.Shell.dll'
    'src\shellprobe\GitRT.ShellProbe.exe' = 'GitRT.ShellProbe.exe'
    'packaging\AppxManifest.xml'          = 'AppxManifest.xml'
}

Say "把构建产物归位到 $OutDir\$Config（搜索：$SearchRoot）" 'Cyan'
$staged = 0
foreach ($rel in $want.Keys) {
    $target = Join-Path (Join-Path $OutDir $Config) $rel
    if (Test-Path $target) {
        Say ("  已就位 {0}" -f $rel) 'DarkGray'
        continue
    }
    $hit = Get-ChildItem -Path $SearchRoot -Recurse -File -Filter $want[$rel] -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if (-not $hit) {
        Say "（搜索根下现有的文件）" 'Yellow'
        Get-ChildItem -Path $SearchRoot -Recurse -File -ErrorAction SilentlyContinue |
            Select-Object -First 40 | ForEach-Object { Say ("    " + $_.FullName) 'Yellow' }
        throw "产物里找不到 $($want[$rel])；无法组装发布包"
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $hit.FullName -Destination $target -Force
    Say ("  归位 {0} ← {1}" -f $rel, $hit.FullName) 'Green'
    ++$staged
}

Say ("完成：{0} 个文件新归位，其余已就位" -f $staged) 'Cyan'
