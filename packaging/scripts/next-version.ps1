# ---------------------------------------------------------------------------
# next-version.ps1 —— 由「最新 tag」推这次构建的版本号
#
#   规则（按用户要求）：
#     · 取最新 tag（本地没有就问 origin；tag 名里可以带前缀，如 v1.2.3 / 可用1.0.0）
#     · 在最后一段 **+1**（即 "加 0.0.1"）
#     · **每个分节上限 9**：该节变成 10 就进位（1.0.9 → 1.1.0；1.9.9 → 2.0.0；9.9.9 → 10.0.0）
#     · 一个 tag 都没有（或 tag 里没有数字）→ 用 1.0.0
#
#   用法：
#     pwsh -File next-version.ps1                       # 打印版本号（stdout，只有版本号一行）
#     pwsh -File next-version.ps1 -OutFile ver.txt      # 同时写文件（CI 里给 GITHUB_ENV 用）
#     pwsh -File next-version.ps1 -Tag 可用1.0.0        # 指定 tag（自测/复现用，不查 git）
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$Tag = '',          # 指定 tag：跳过"找最新 tag"
    [switch]$NoTag,             # 强制当成"一个 tag 都没有"（自测用）
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Stop'

function Get-LatestTag([string]$repo) {
    # 1) 本地 tag（按版本倒序，尽量拿语义上最大的）
    $local = & git -C $repo tag --sort=-v:refname 2>$null
    if ($local) { return ($local | Where-Object { $_ } | Select-Object -First 1) }
    # 2) 本地没有（CI 浅克隆常见）→ 问远端
    $remote = & git -C $repo ls-remote --tags origin 2>$null
    if ($remote) {
        $names = $remote | ForEach-Object {
            $p = ($_ -split '\s+')[1]
            if ($p -and $p.StartsWith('refs/tags/')) { $p.Substring(10) -replace '\^\{\}$', '' }
        } | Where-Object { $_ } | Sort-Object -Descending -Unique
        if ($names) { return ($names | Select-Object -First 1) }
    }
    return ''
}

function ConvertTo-VersionParts([string]$text) {
    if (-not $text) { return $null }
    $m = [regex]::Matches($text, '\d+(?:\.\d+)*')
    if ($m.Count -eq 0) { return $null }
    $nums = $m[$m.Count - 1].Value -split '\.' | ForEach-Object { [int]$_ }
    return , $nums
}

# ---------------------------------------------------------------- 解析
$rawTag = if ($NoTag) { '' } elseif ($Tag) { $Tag } else { Get-LatestTag $RepoRoot }
$parts = ConvertTo-VersionParts $rawTag

if (-not $parts) {
    $version = '1.0.0'          # 没有 tag（或 tag 里没数字）
    $from = if ($rawTag) { "tag '$rawTag' 里没有数字" } else { '没有任何 tag' }
} else {
    # 补齐到三段，方便显示；再"最后一段 +1、逐级进位，每节上限 9"
    while ($parts.Count -lt 3) { $parts += 0 }
    # 最后一段 +1，然后逐级进位；**每节上限 9**
    # 最前一段本身允许长到 10（9.9.9 → 10.0.0）：不然没法表达"再大一点"
    $i = $parts.Count - 1
    $parts[$i]++
    while ($i -gt 0 -and $parts[$i] -gt 9) {
        $parts[$i] = 0
        $parts[$i - 1]++
        $i--
    }
    $version = ($parts -join '.')
    $from = "最新 tag '$rawTag'"
}

if ($OutFile) { Set-Content -LiteralPath $OutFile -Value $version -Encoding UTF8 -NoNewline }
Write-Output $version
Write-Host "  （$from → $version）" -ForegroundColor DarkGray
