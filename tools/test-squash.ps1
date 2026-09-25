# ---------------------------------------------------------------------------
# 合并提交（squash）端到端测试 —— 走 CLI（与 GUI 复选框同一套 core 逻辑）
#
#   覆盖：
#     A) 含 HEAD 的连续区间 3 条  → reset --soft + commit（提交数 -2、tree 不变、信息=拼接）
#     B) 不含 HEAD 的中间 2 条    → commit-tree + rebase --onto（提交数 -1、顶部 tree 不变）
#     C) 不连续的选择             → 拒绝，历史不变
#     D) 选择里含 merge 提交      → 拒绝
#     E) 只选 1 条                → 拒绝
#     F) 暂存区有改动             → 拒绝
#     G) 区间触到根提交           → 拒绝
#     H) 自定义合并信息           → 生效
#
# 用法: pwsh -File tools\test-squash.ps1 [-Exe <GitRT.exe>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$LabRoot = "$PSScriptRoot\..\build\squash-lab"
)
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
function Invoke-Git([string]$repo, [string[]]$argv) { $o = & git.exe -C $repo @argv 2>&1; return ($o | Out-String).Trim() }
function MsgOf($repo, $rev) {
    # 用 %B（完整正文）：%s 会把多行折成一行，断言会失真
    # git 在 Windows 上把日志输出成 CRLF（存储是 LF），比较前归一化
    return ((Invoke-Git $repo @('log', '-1', '--format=%B', $rev)).Trim() -replace "`r`n", "`n")
}
function CountOf($repo) { return [int](Invoke-Git $repo @('rev-list', '--count', 'HEAD')) }
function TreeOf($repo) { return (Invoke-Git $repo @('rev-parse', 'HEAD^{tree}')) }
function HashList($repo) {
    # ★ 必须显式分两步：`Invoke-Git ... -split "`n"` 会被当成给函数传 -split 参数（坑）
    $txt = Invoke-Git $repo @('log', '--format=%H')
    return @($txt -split "`n" | Where-Object { $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
function RunSquash($repo, [string]$hashes, [string]$msg = '', [switch]$DryRun) {
    # ★ 不能叫 $args（PowerShell 保留变量，赋值无效 —— 踩过）
    $cliArgs = @('--squash', $hashes, '--cwd', $repo, '--out', (Join-Path $repo '.squash-out.txt'))
    if ($msg) { $cliArgs += @('--message', $msg) }
    if ($DryRun) { $cliArgs += '--dry-run' }
    # ★ Start-Process 的 ArgumentList 不会自动给含空格的参数加引号（会把提交信息截断）
    $argLine = ($cliArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -Wait -PassThru -NoNewWindow
    $outFile = Join-Path $repo '.squash-out.txt'
    for ($w = 0; $w -lt 20 -and -not (Test-Path $outFile); $w++) { Start-Sleep -Milliseconds 100 }
    $txt = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
    if (-not $txt) { $txt = '' }
    $r = [ordered]@{ exit = $p.ExitCode; raw = $txt }
    foreach ($k in @('ok', 'mode', 'error', 'new_hash', 'message_first', 'commits', 'message_lines')) {
        $m = [regex]::Match($txt, "(?m)^$k=(.*)$")
        if ($m.Success) { $r[$k] = $m.Groups[1].Value.Trim() }
    }
    Remove-Item (Join-Path $repo '.squash-out.txt') -Force -ErrorAction SilentlyContinue
    return [pscustomobject]$r
}
function New-Lab([string]$name, [int]$commits) {
    $repo = Join-Path $LabRoot $name
    if (Test-Path $repo) { Remove-Item $repo -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $repo | Out-Null
    Invoke-Git $repo @('init', '-q', '-b', 'main') | Out-Null
    Invoke-Git $repo @('config', 'user.name', 'GitRT Test') | Out-Null
    Invoke-Git $repo @('config', 'user.email', 'test@example.invalid') | Out-Null
    Invoke-Git $repo @('config', 'commit.gpgsign', 'false') | Out-Null
    for ($i = 1; $i -le $commits; $i++) {
        Set-Content (Join-Path $repo "f$i.txt") "content $i" -Encoding UTF8
        Invoke-Git $repo @('add', '-A') | Out-Null
        Invoke-Git $repo @('commit', '-q', '-m', "c$i") | Out-Null
    }
    # ★ 防呆：确认这个 lab 目录**自己**就是仓库根，避免 git 向上找到主仓库（踩过这个坑）
    $top = (Invoke-Git $repo @('rev-parse', '--show-toplevel')) -replace '\\', '/'
    $want = $repo -replace '\\', '/'
    #   注意：%TEMP% 可能是 8.3 短名（ADMINI~1），所以不比较整串，只比较"倒数第二段/末段"
    $topLeaf = Split-Path ($top -replace '/', '\') -Leaf
    $topParent = Split-Path (Split-Path ($top -replace '/', '\') -Parent) -Leaf
    $wantLeaf = Split-Path $repo -Leaf
    $wantParent = Split-Path ([System.IO.Path]::GetFullPath($LabRoot)) -Leaf
    if ($topLeaf -ne $wantLeaf -or $topParent -ne $wantParent) {
        throw "lab 仓库位置不对（git 可能向上找到了别的仓库）：$top"
    }

    return $repo
}
function Clean($repo) { return ((Invoke-Git $repo @('status', '--porcelain')) -eq '') }

New-Item -ItemType Directory -Force -Path $LabRoot | Out-Null
Write-Host "== 合并提交（squash）端到端 ==" -ForegroundColor Cyan
Write-Host "exe = $Exe"

# ---------------------------------------------------------------- A) 含 HEAD
Write-Host "`n-- A) 含 HEAD 的连续 3 条 → reset-soft --" -ForegroundColor Cyan
$repo = New-Lab 'a-head' 4
$tree0 = TreeOf $repo
$plan = RunSquash $repo ((HashList $repo)[0..2] -join ',') -DryRun
Check "A 计划成功（dry-run）" ($plan.ok -eq '1') "raw=$($plan.raw)"
Check "A 选了 3 条、模式 reset-soft" ($plan.commits -eq '3' -and $plan.mode -eq 'reset-soft') "mode=$($plan.mode) commits=$($plan.commits)"
$res = RunSquash $repo ((HashList $repo)[0..2] -join ',')
Check "A 执行成功" ($res.ok -eq '1' -and $res.exit -eq 0) "raw=$($res.raw)"
Check "A 提交数 4 → 2" ((CountOf $repo) -eq 2) "count=$(CountOf $repo)"
Check "A HEAD 的 tree 不变（改动 = 三条之和）" ((TreeOf $repo) -eq $tree0) 'tree 变了'
Check "A 合并信息 = c2/c3/c4 拼接" ((MsgOf $repo 'HEAD') -eq "c2`nc3`nc4") "msg=[$(MsgOf $repo 'HEAD')]"
Check "A 工作区干净" (Clean $repo) (Invoke-Git $repo @('status', '--porcelain'))
Check "A 新提交 = new_hash" ((Invoke-Git $repo @('rev-parse', 'HEAD')) -eq $res.new_hash) "expected=$($res.new_hash)"

# ------------------------------------------------------------ B) 不含 HEAD
Write-Host "`n-- B) 中间 2 条（不含 HEAD）→ commit-tree + rebase --onto --" -ForegroundColor Cyan
$repo = New-Lab 'b-mid' 5
$hashes = HashList $repo          # [0]=c5 … [4]=c1
$tree0 = TreeOf $repo
$topSubject = MsgOf $repo 'HEAD'
$res = RunSquash $repo (@($hashes[1], $hashes[2]) -join ',')   # c4,c3（中间两条）
Check "B 执行成功" ($res.ok -eq '1') "raw=$($res.raw)"
Check "B 模式是 commit-tree+rebase" ($res.mode -eq 'commit-tree+rebase') "mode=$($res.mode)"
Check "B 提交数 5 → 4" ((CountOf $repo) -eq 4) "count=$(CountOf $repo)"
Check "B HEAD 的 tree 不变" ((TreeOf $repo) -eq $tree0) 'tree 变了'
Check "B 顶部提交信息保留（$topSubject）" ((MsgOf $repo 'HEAD') -eq $topSubject) "msg=$(MsgOf $repo 'HEAD')"
Check "B 合并后的提交是被合并的那两条之和（c3+c4）" ((MsgOf $repo 'HEAD~1') -eq "c3`nc4") "msg=$(MsgOf $repo 'HEAD~1')"
Check "B 工作区干净" (Clean $repo) (Invoke-Git $repo @('status', '--porcelain'))

# ---------------------------------------------------------------- C) 不连续
Write-Host "`n-- C) 不连续的选择 → 拒绝 --" -ForegroundColor Cyan
$repo = New-Lab 'c-gap' 4
$before = CountOf $repo
$hashes = HashList $repo
$res = RunSquash $repo (@($hashes[0], $hashes[2]) -join ',')   # c4 与 c2（跳过 c3）
Check "C 被拒绝" ($res.ok -eq '0') "ok=$($res.ok)"
Check "C 提示'连续'" ($res.error -match '连续') "error=$($res.error)"
Check "C 历史未变" ((CountOf $repo) -eq $before) 'count 变了'

# ------------------------------------------------------------ D) 含 merge
Write-Host "`n-- D) 选择里含 merge 提交 → 拒绝 --" -ForegroundColor Cyan
$repo = New-Lab 'd-merge' 3
Invoke-Git $repo @('checkout', '-q', '-b', 'side') | Out-Null
Set-Content (Join-Path $repo 's.txt') 'side' -Encoding UTF8
Invoke-Git $repo @('add', '-A') | Out-Null
Invoke-Git $repo @('commit', '-q', '-m', 'side work') | Out-Null
Invoke-Git $repo @('checkout', '-q', 'main') | Out-Null
Invoke-Git $repo @('merge', '--no-ff', '-q', '-m', 'merge side', 'side') | Out-Null
$before = CountOf $repo
$hashes = HashList $repo
    # ★ 用 ^1 显式取"第一父"：git log 的第 2 条可能是第二个父（同秒时间戳下顺序不定）——踩过
    $mergeFirstParent = (Invoke-Git $repo @('rev-parse', '--verify', "$($hashes[0])^1")).Trim()
    $res = RunSquash $repo (@($hashes[0], $mergeFirstParent) -join ',')   # merge + 它的第一父
Check "D 被拒绝" ($res.ok -eq '0') "ok=$($res.ok)"
Check "D 提示'合并提交'" ($res.error -match '合并提交') "error=$($res.error)"
Check "D 历史未变" ((CountOf $repo) -eq $before) 'count 变了'

# ---------------------------------------------------------------- E) 单条
Write-Host "`n-- E) 只选 1 条 → 拒绝 --" -ForegroundColor Cyan
$repo = New-Lab 'e-single' 3
$res = RunSquash $repo (HashList $repo)[0]
Check "E 被拒绝" ($res.ok -eq '0') "ok=$($res.ok)"
Check "E 提示'至少…2 条'" ($res.error -match '2 条') "error=$($res.error)"

# ------------------------------------------------------------ F) 暂存区脏
Write-Host "`n-- F) 暂存区有改动 → 拒绝 --" -ForegroundColor Cyan
$repo = New-Lab 'f-staged' 3
Set-Content (Join-Path $repo 'staged.txt') 'x' -Encoding UTF8
Invoke-Git $repo @('add', '-A') | Out-Null
$hashes = HashList $repo
$res = RunSquash $repo (@($hashes[0], $hashes[1]) -join ',')
Check "F 被拒绝" ($res.ok -eq '0') "ok=$($res.ok)"
Check "F 提示'暂存区'" ($res.error -match '暂存区') "error=$($res.error)"

# ---------------------------------------------------------------- G) 根提交
Write-Host "`n-- G) 区间触到根提交 → 拒绝 --" -ForegroundColor Cyan
$repo = New-Lab 'g-root' 3
$hashes = HashList $repo
$res = RunSquash $repo (@($hashes[1], $hashes[2]) -join ',')   # c2 + 根 c1
Check "G 被拒绝" ($res.ok -eq '0') "ok=$($res.ok)"
Check "G 提示'根提交'" ($res.error -match '根提交') "error=$($res.error)"

# ------------------------------------------------------- H) 自定义合并信息
Write-Host "`n-- H) 自定义合并信息 --" -ForegroundColor Cyan
$repo = New-Lab 'h-msg' 3
$res = RunSquash $repo ((HashList $repo)[0..1] -join ',') -msg 'squashed: c2+c3'
Check "H 执行成功" ($res.ok -eq '1') "raw=$($res.raw)"
Check "H 信息被替换为自定义" ((MsgOf $repo 'HEAD') -eq 'squashed: c2+c3') "msg=[$(MsgOf $repo 'HEAD')]"

Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
