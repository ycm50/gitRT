# 本地复现 CI 步骤：按顺序跑抽出来的 step 脚本，模拟 GITHUB_ENV / GITHUB_PATH 的传递。
# 用法: pwsh -File tools/ci-run-steps.ps1 [-Job build] [-Only 02,03,05]
param(
    [string]$Job = 'build',
    [string]$Only = ''
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$envFile = Join-Path $root 'build/ci-env.txt'
$pathFile = Join-Path $root 'build/ci-path.txt'
Remove-Item $envFile, $pathFile -Force -ErrorAction SilentlyContinue
New-Item -ItemType File -Path $envFile, $pathFile -Force | Out-Null
$env:GITHUB_ENV = $envFile
$env:GITHUB_PATH = $pathFile
$env:MSYS2_ROOT_HINT = 'A:/msys64'   # 本机 MSYS2 在 A 盘；CI 上这个变量是 C:/msys64
Remove-Item Env:\MSYS2_ROOT -ErrorAction SilentlyContinue

$files = Get-ChildItem "build/ci-steps/$Job-*.ps1" | Sort-Object Name
if ($Only) {
    $keep = $Only -split ','
    $files = $files | Where-Object { $p = $_.Name; ($keep | Where-Object { $p -match "-$_-" }).Count -gt 0 }
}

foreach ($f in $files) {
    $code = ($f.Name -split '-')[1]
    Write-Host "`n=== [$Job $code] $($f.Name) ===" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & pwsh -NoProfile -File $f.FullName *>&1 | ForEach-Object { "  $_" }
    $rc = $LASTEXITCODE
    $sw.Stop()
    $color = if ($rc -eq 0) { 'Green' } else { 'Red' }
    Write-Host ("  → 退出码 {0}  用时 {1}s" -f $rc, [int]$sw.Elapsed.TotalSeconds) -ForegroundColor $color
    # 模拟 runner：把 GITHUB_ENV / GITHUB_PATH 的内容应用到后续步骤
    foreach ($line in (Get-Content $envFile -ErrorAction SilentlyContinue)) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:\" + $Matches[1]) -Value $Matches[2]
            Write-Host "  [env] $($Matches[1])=$($Matches[2])" -ForegroundColor DarkGray
        }
    }
    foreach ($line in (Get-Content $pathFile -ErrorAction SilentlyContinue)) {
        if ($line) { $env:PATH = "$line;$env:PATH"; Write-Host "  [path] $line" -ForegroundColor DarkGray }
    }
}
