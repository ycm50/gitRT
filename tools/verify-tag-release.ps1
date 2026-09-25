# ---------------------------------------------------------------------------
# 标签 / 发布 —— 一键验收（把目标里要求的"全部构建/测试绿"一次跑完）
#
#   1) Debug 构建（cmake --build build/debug）
#   2) Release 构建（cmake --build build/release）
#   3) ctest（Debug）
#   4) GUI 自检（含新增的 8g 标签/发布块）→ 必须 `== 结果: N 通过 / 0 失败 ==`
#   5) tools/test-tag-release.ps1（CLI 端到端）
#   6) 回归：test-remote / test-squash / test-clone 失败数必须为 0
#
#   全都只读/只在 build\ 下写，不碰用户真实安装、不碰 %TEMP% 之外的东西。
#   ★ 跑之前请确认没有别的进程在写同一个 build\（ninja 不能并发跑同一目录）。
#
# 用法: pwsh -File tools\verify-tag-release.ps1 [-SkipBuild]
# ---------------------------------------------------------------------------
param([switch]$SkipBuild)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$results = [ordered]@{}
function Line($name, $ok, $detail = '') {
    $mark = if ($ok) { 'OK  ' } else { 'FAIL' }
    $color = if ($ok) { 'Green' } else { 'Red' }
    $results[$name] = $ok
    Write-Host ("  [{0}] {1,-24} {2}" -f $mark, $name, $detail) -ForegroundColor $color
}

if (-not $SkipBuild) {
    Write-Host "== 1/6 Debug 构建 ==" -ForegroundColor Cyan
    $out = cmake --build build/debug 2>&1 | Out-String
    Line 'build/debug' ($LASTEXITCODE -eq 0) (($out -split "`n" | Select-String 'error|FAILED' | Select-Object -First 1).Line)

    Write-Host "== 2/6 Release 构建 ==" -ForegroundColor Cyan
    $out = cmake --build build/release 2>&1 | Out-String
    Line 'build/release' ($LASTEXITCODE -eq 0) (($out -split "`n" | Select-String 'error|FAILED' | Select-Object -First 1).Line)
} else {
    Write-Host "== 1-2/6 构建（已跳过 -SkipBuild）==" -ForegroundColor DarkGray
}

$exe = Join-Path $root 'build\debug\src\gui\GitRT.exe'
Line 'GitRT.exe 存在' (Test-Path $exe) $exe

Write-Host "== 3/6 ctest ==" -ForegroundColor Cyan
$ct = ctest --test-dir build/debug 2>&1 | Out-String
$m = [regex]::Match($ct, '(\d+)% tests passed out of (\d+)')
Line 'ctest' ($m.Success -and $m.Groups[1].Value -eq '100') ($m.Value)

Write-Host "== 4/6 GUI 自检 ==" -ForegroundColor Cyan
$rep = Join-Path $root 'build\verify-selftest-gui.txt'
Remove-Item $rep -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList @("--self-test=$rep") -PassThru
[void]$p.WaitForExit(300000)
$txt = if (Test-Path $rep) { Get-Content $rep -Raw } else { '' }
$sum = [regex]::Match($txt, '== 结果: (\d+) 通过 / (\d+) 失败 ==')
$selftestOk = ($p.ExitCode -eq 0) -and $sum.Success -and ($sum.Groups[2].Value -eq '0')
Line 'GUI 自检' $selftestOk $(if ($sum.Success) { $sum.Value } else { 'exit=' + $p.ExitCode + '（没有结果行）' })
$has8g = [bool](Select-String -Path $rep -Pattern '标签计划：|发布计划：|标签清单：' -Quiet -ErrorAction SilentlyContinue)
Line '标签/发布自检已执行' $has8g ''

Write-Host "== 5/6 标签/发布 CLI 端到端 ==" -ForegroundColor Cyan
$t = pwsh -NoProfile -File (Join-Path $root 'tools\test-tag-release.ps1') -Exe $exe 2>&1 | Out-String
$m2 = [regex]::Match($t, '== 结果: (\d+) 通过 / (\d+) 失败 / (\d+) 跳过 ==')
Line 'test-tag-release' ($m2.Success -and $m2.Groups[2].Value -eq '0') $(if ($m2.Success) { $m2.Value } else { '没有结果行' })

Write-Host "== 6/6 回归三套 ==" -ForegroundColor Cyan
foreach ($s in @('test-remote', 'test-squash', 'test-clone')) {
    $r = pwsh -NoProfile -File (Join-Path $root "tools\$s.ps1") -Exe $exe 2>&1 | Out-String
    $m3 = [regex]::Match($r, '== 结果: (\d+) 通过 / (\d+) 失败 ==')
    Line $s ($m3.Success -and $m3.Groups[2].Value -eq '0') $(if ($m3.Success) { $m3.Value } else { '没有结果行' })
}

$bad = @($results.GetEnumerator() | Where-Object { -not $_.Value })
Write-Host ""
if ($bad.Count -eq 0) {
    Write-Host "== 全部通过（$($results.Count) 项）==" -ForegroundColor Green
    exit 0
} else {
    Write-Host "== 有失败：$($bad.Name -join ', ') ==" -ForegroundColor Red
    exit 1
}
