# ---------------------------------------------------------------------------
# register-legacy.ps1 —— 《技术实现设计》§2.7 路径 A：传统注册（最快迭代）
#
# 为什么先走这条路：它把"实现错误"与"注册/打包错误"彻底分离（xplorer² 的经验）。
#   · 只写 HKCU\Software\Classes（不需要管理员、不需要打包、不需要签名）
#   · 菜单出现在「显示更多选项」里（传统菜单），而**不是** Win11 现代菜单
#   · 开发构建的 GitRT.Shell.dll 导出了 DllRegisterServer/DllUnregisterServer
#     （GRT_DEV_REGISTER=ON，默认开启），因此用 regsvr32 一键注册/注销
#
# 用法：
#   pwsh -File tools/dev/register-legacy.ps1                 # 注册 + 重启 explorer
#   pwsh -File tools/dev/register-legacy.ps1 -NoRestart      # 只注册
#   pwsh -File tools/dev/register-legacy.ps1 -Unregister     # 注销
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Dll = '',
    [string]$Config = 'Debug',
    [string]$OutDir = "$PSScriptRoot\..\..\build",
    [switch]$Unregister,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

if (-not $Dll) {
    $buildDir = Join-Path $OutDir $Config
    $hit = Get-ChildItem -Path $buildDir -Recurse -Filter 'GitRT.Shell.dll' -File -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if (-not $hit) { throw "找不到 GitRT.Shell.dll；先构建或显式传 -Dll <路径>" }
    $Dll = $hit.FullName
}
if (-not (Test-Path $Dll)) { throw "DLL 不存在：$Dll" }

$objdump = (Get-Command objdump -ErrorAction SilentlyContinue).Source
if (-not $objdump -and (Test-Path 'A:\msys64\ucrt64\bin\objdump.exe')) {
    $objdump = 'A:\msys64\ucrt64\bin\objdump.exe'
}
if ($objdump) {
    $exported = (& $objdump -p $Dll 2>$null) -join "`n"
    if ($exported -notmatch 'DllRegisterServer') {
        throw "该 DLL 未导出 DllRegisterServer：请用 -DGRT_DEV_REGISTER=ON 重新配置构建（默认已是 ON）"
    }
} else {
    Write-Host "未找到 objdump：跳过导出表预检" -ForegroundColor DarkGray
}

$regsvr = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
if ($Unregister) {
    Write-Host "传统注册注销：$Dll" -ForegroundColor Cyan
    & $regsvr /u /s $Dll
} else {
    Write-Host "传统注册：$Dll" -ForegroundColor Cyan
    & $regsvr /s $Dll
}
if ($LASTEXITCODE -ne 0) { throw "regsvr32 失败（$LASTEXITCODE）" }

# 结果核对：CLSID 的 InprocServer32 与 ExplorerCommandHandler 都应存在
$identity = Get-Content (Join-Path $PSScriptRoot '..\..\packaging\identity.json') -Raw | ConvertFrom-Json
$clsid = $identity.clsid
$srv = "HKCU:\Software\Classes\CLSID\$clsid\InprocServer32"
if ($Unregister) {
    Write-Host ("  InprocServer32 已移除：{0}" -f (-not (Test-Path $srv))) -ForegroundColor DarkGray
} else {
    if (-not (Test-Path $srv)) { throw "注册失败：缺少 $srv" }
    $path = (Get-ItemProperty $srv).'(default)'
    Write-Host "  InprocServer32 = $path" -ForegroundColor DarkGray
    foreach ($k in @('Directory', 'Directory\Background', '*')) {
        $sub = "HKCU:\Software\Classes\$k\shell\GitRTTest"
        if (-not (Test-Path $sub)) { throw "注册失败：缺少 $sub" }
    }
    Write-Host "  ExplorerCommandHandler 已写入 Directory / Directory\Background / *" -ForegroundColor DarkGray
}

if (-not $NoRestart) {
    & (Join-Path $PSScriptRoot '..\..\packaging\scripts\dev-reload.ps1')
}
Write-Host ""
Write-Host "验证方式：任意目录右键 → 「显示更多选项」→ 应看到「GitRT（测试）」。" -ForegroundColor Yellow
Write-Host "（传统菜单只是开发期验证实现的手段；现代菜单需要 §3.2 的稀疏包注册。）" -ForegroundColor DarkGray
