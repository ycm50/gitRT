# ---------------------------------------------------------------------------
# build-msix.ps1 —— 生成稀疏身份包（《技术实现设计》§11.3）
#
#   两种产物：
#     ① -LayoutOnly ：只组装 <OutDir>\sparse\{AppxManifest.xml, Assets\*}
#        → 开发者模式下可用 `Add-AppxPackage -Register` 直接注册，**不需要 Windows SDK**
#          （对应《产品设计》§3.10 的 S2 实验：免签名 loose 注册）
#     ② 默认        ：makeappx pack → signtool 签名（开发期自签名）
#        → 需要 Windows SDK 的 makeappx.exe / signtool.exe（本机按 §1 未安装；
#          这一路是 M0-B 的正式注册路径）
#
#   清单校验是本脚本的核心价值：publisher/packageName/applicationId/CLSID 必须与
#   packaging/identity.json 完全一致，否则资源管理器会**静默不加载**扩展（§3.4）。
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Config   = 'Release',
    [string]$OutDir   = "$PSScriptRoot\..\..\build",
    [string]$StageDir = '',
    [switch]$LayoutOnly,
    [switch]$Sign     = $true,
    [switch]$DevCert  = $true,
    [string]$CertPfx  = '',
    [string]$CertPwd  = 'gitrt-dev'
)
$ErrorActionPreference = 'Stop'

$OutDir  = [System.IO.Path]::GetFullPath($OutDir)
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$identityPath = Join-Path $repoRoot 'packaging\identity.json'
if (-not (Test-Path $identityPath)) { throw "缺少 $identityPath（身份的唯一真相源）" }
$identity = Get-Content $identityPath -Raw | ConvertFrom-Json

# CMake 在 configure 阶段把 AppxManifest.xml 生成到构建目录
$manifestCandidates = @(
    (Join-Path $OutDir "$Config\packaging\AppxManifest.xml"),
    (Join-Path $OutDir 'packaging\AppxManifest.xml')
)
$manifest = $manifestCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $manifest) {
    throw "未找到生成的 AppxManifest.xml；请先运行: cmake --preset ucrt64-$($Config.ToLower())"
}
if (-not $StageDir) { $StageDir = Join-Path $OutDir 'sparse' }

# ------------------------------------------------------------------ 1) 校验
Write-Host "校验 $manifest" -ForegroundColor Cyan
[xml]$xml = Get-Content -LiteralPath $manifest -Raw
$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
$ns.AddNamespace('uap10', 'http://schemas.microsoft.com/appx/manifest/uap/windows10/10')
$ns.AddNamespace('desktop4', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/4')
$ns.AddNamespace('desktop5', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/5')
$ns.AddNamespace('com', 'http://schemas.microsoft.com/appx/manifest/com/windows10')

$raw = Get-Content -LiteralPath $manifest -Raw
if ($raw -match '@GRT_') { throw "AppxManifest 里还有未替换的 @...@ 占位符：$manifest" }

$ident = $xml.SelectSingleNode('/f:Package/f:Identity', $ns)
if (-not $ident) { throw "清单缺少 Identity 元素" }
$checks = @(
    @{ Name = 'Identity/@Name';        Want = $identity.packageName;   Got = $ident.Name },
    @{ Name = 'Identity/@Publisher';   Want = $identity.publisher;     Got = $ident.Publisher },
    @{ Name = 'Identity/@Version';     Want = $identity.version;       Got = $ident.Version }
)
$app = $xml.SelectSingleNode('/f:Package/f:Applications/f:Application', $ns)
if (-not $app) { throw "清单缺少 Application 元素" }
$checks += @{ Name = 'Application/@Id';         Want = $identity.applicationId; Got = $app.Id }
$checks += @{ Name = 'Application/@Executable'; Want = $identity.executable;    Got = $app.Executable }

$cls = $xml.SelectSingleNode('//com:Class', $ns)
if (-not $cls) { throw "清单缺少 com:Class（COM 服务器注册）" }
# ★ M0-B 实测：清单里的 CLSID 必须**不带花括号**（schema pattern 不允许 {}），
#   否则 Add-AppxPackage 报 0x80080204 / 0xC00CE169。identity.json 允许两种写法。
function Normalize-Clsid([string]$s) { return ($s -replace '[{}]', '').ToLowerInvariant() }
if ($cls.Id -match '[{}]') {
    throw "清单的 com:Class/@Id 不能带花括号（Appx schema 要求）：$($cls.Id)"
}
if ($identity.clsid -match '[{}]') {
    throw "identity.json 的 clsid 请写成不带花括号的形式（${($identity.clsid)}）"
}
$checks += @{ Name = 'com:Class/@Id';   Want = Normalize-Clsid $identity.clsid; Got = Normalize-Clsid $cls.Id }
$checks += @{ Name = 'com:Class/@Path'; Want = $identity.shellDll;               Got = $cls.Path }

foreach ($c in $checks) {
    if ($c.Got -ne $c.Want) {
        throw "清单与 identity.json 不一致：$($c.Name) = '$($c.Got)'，期望 '$($c.Want)'（§3.4 的静默失败点）"
    }
    Write-Host ("  [OK] {0} = {1}" -f $c.Name, $c.Got) -ForegroundColor DarkGray
}
if ($ident.Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "版本号必须是 4 段数字：$($ident.Version)" }
$itemTypes = $xml.SelectNodes('//desktop5:ItemType', $ns) | ForEach-Object { $_.Type }
if ($itemTypes.Count -lt 3) { throw "contextMenu 目标类型不足（应有 * / Directory / Directory\Background）" }
Write-Host "  [OK] desktop5:ItemType = $($itemTypes -join ', ')" -ForegroundColor DarkGray

# ------------------------------------------------------------------ 2) 组装
Write-Host "组装稀疏包目录 → $StageDir" -ForegroundColor Cyan
if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir 'Assets') | Out-Null
Copy-Item -LiteralPath $manifest -Destination (Join-Path $StageDir 'AppxManifest.xml') -Force
Copy-Item -Path (Join-Path $repoRoot 'packaging\Assets\*') -Destination (Join-Path $StageDir 'Assets') -Force
Get-ChildItem -Recurse -File $StageDir | ForEach-Object {
    Write-Host ("  {0}  {1} bytes" -f $_.FullName.Substring($StageDir.Length + 1), $_.Length) -ForegroundColor DarkGray
}

if ($LayoutOnly) {
    Write-Host ""
    Write-Host "LayoutOnly：可用开发者模式免签名注册（不需要 Windows SDK）：" -ForegroundColor Green
    Write-Host "  Add-AppxPackage -Register `"$StageDir\AppxManifest.xml`" -ExternalLocation <安装目录>" -ForegroundColor Yellow
    Write-Host "或直接运行： pwsh -File packaging\scripts\dev-install.ps1 -Config $Config" -ForegroundColor Yellow
    return
}

# ------------------------------------------------------------- 3) 打包+签名
function Find-SdkTool([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem $r -Recurse -Filter $name -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$makeappx = Find-SdkTool 'makeappx.exe'
$signtool = Find-SdkTool 'signtool.exe'
if (-not $makeappx) {
    throw "未找到 makeappx.exe：请安装 Windows SDK（仅打包需要），或改用 -LayoutOnly 走免签名 loose 注册"
}
if ($Sign -and -not $signtool) {
    throw "未找到 signtool.exe：请安装 Windows SDK，或改用 -Sign:`$false 生成未签名包"
}

$msix = Join-Path $OutDir 'GitRT.msix'
Write-Host "makeappx pack → $msix" -ForegroundColor Cyan
# /nv 必需：跳过对包外文件路径（ExternalLocation）的校验
& $makeappx pack /o /d $StageDir /nv /p $msix
if ($LASTEXITCODE -ne 0) { throw "makeappx 失败（$LASTEXITCODE）" }

if ($Sign) {
    if (-not $CertPfx) { $CertPfx = Join-Path $OutDir 'dev.pfx' }
    if ($DevCert -and -not (Test-Path $CertPfx)) {
        Write-Host "创建开发期自签名证书（导入 TrustedPeople，否则注册报 0x800B0109）" -ForegroundColor Cyan
        $cert = New-SelfSignedCertificate -Type Custom -Subject $identity.publisher `
            -KeyUsage DigitalSignature -FriendlyName 'GitRT Dev' `
            -CertStoreLocation 'Cert:\CurrentUser\My' `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
        $pwd = ConvertTo-SecureString -String $CertPwd -Force -AsPlainText
        Export-PfxCertificate -Cert $cert -FilePath $CertPfx -Password $pwd | Out-Null
        $cer = Join-Path $OutDir 'dev.cer'
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
    }
    & $signtool sign /fd SHA256 /f $CertPfx /p $CertPwd /tr http://timestamp.digicert.com /td SHA256 $msix
    if ($LASTEXITCODE -ne 0) { throw "signtool 失败（$LASTEXITCODE）" }
    & $signtool verify /pa /v $msix | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "签名校验失败（$LASTEXITCODE）" }
}

Write-Host "OK → $msix" -ForegroundColor Green
