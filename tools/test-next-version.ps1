# ---------------------------------------------------------------------------
# next-version.ps1 的规则自测：最新 tag +0.0.1、每节上限 9 进位、无 tag 用 1.0.0
# 用法: pwsh -File tools\test-next-version.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
$script = Join-Path (Split-Path -Parent $PSScriptRoot) 'packaging\scripts\next-version.ps1'
$pass = 0; $fail = 0

$cases = @(
    @{ args = @('-NoTag');            want = '1.0.0';  why = '一个 tag 都没有 → 1.0.0' },
    @{ args = @('-Tag', 'v1.0.0');    want = '1.0.1';  why = '标准 tag，+0.0.1' },
    @{ args = @('-Tag', '可用1.0.0'); want = '1.0.1';  why = 'tag 名带中文前缀也能解析' },
    @{ args = @('-Tag', '1.0.9');     want = '1.1.0';  why = '末节封顶 9 → 进位' },
    @{ args = @('-Tag', '1.9.9');     want = '2.0.0';  why = '连续进位' },
    @{ args = @('-Tag', '9.9.9');     want = '10.0.0'; why = '最前一段长到 10' },
    @{ args = @('-Tag', 'release-0.0.1'); want = '0.0.2'; why = '英文前缀' },
    @{ args = @('-Tag', 'v1.2');      want = '1.2.1';  why = '两段自动补齐' },
    @{ args = @('-Tag', 'abc');       want = '1.0.0';  why = 'tag 里没有数字' }
)

Write-Host "== next-version 规则自测 ==" -ForegroundColor Cyan
foreach ($c in $cases) {
    $got = & pwsh -NoProfile -File $script @($c.args) 2>$null | Select-Object -First 1
    $got = if ($null -eq $got) { '(空)' } else { $got.Trim() }
    if ($got -eq $c.want) {
        $pass++; Write-Host ("  [PASS] {0,-22} -> {1}" -f ($c.args -join ' '), $got) -ForegroundColor Green
    } else {
        $fail++; Write-Host ("  [FAIL] {0,-22} -> {1}  期望 {2}（{3}）" -f ($c.args -join ' '), $got, $c.want, $c.why) -ForegroundColor Red
    }
}
Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
