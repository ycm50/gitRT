# ---------------------------------------------------------------------------
# dev-reload.ps1 —— 只重启资源管理器（改了 DLL 后用；身份包无需重装）
#   《技术实现设计》§2.7 路径 B / §11.4
# ---------------------------------------------------------------------------
[CmdletBinding()]
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'

$before = (Get-Process explorer -ErrorAction SilentlyContinue | Measure-Object).Count
if (-not $Quiet) { Write-Host "重启 explorer.exe（当前 $before 个实例）…" -ForegroundColor Cyan }

Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) {
    Start-Process explorer
    Start-Sleep -Milliseconds 800
}
$after = (Get-Process explorer -ErrorAction SilentlyContinue | Measure-Object).Count
if (-not $Quiet) {
    Write-Host "explorer 已在运行（$after 个实例）。现在可以右键验证菜单。" -ForegroundColor Green
    Write-Host "提示：只改了 DLL 的代码不需要重新注册身份包（身份未变，§3.2）。" -ForegroundColor DarkGray
}
