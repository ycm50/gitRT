# ---------------------------------------------------------------------------
# 远端跟踪 + 按提交还原 端到端（CLI 层，真 git 仓库）
#
#   A) 远端基线：远端前进 → 落后 N；本地提提交 → 领先 M；本地新增能列出来
#   B) 上游：取消上游 → has_upstream=0；--set-upstream 设回 → =1
#   C) 远端分支：能列出多个，且标注 [当前上游]/[远端默认分支]
#   D) 还原：只读检出 / 新建分支 / 软·混合·硬重置 —— 工作区与分支状态都要对
#   E) 拒绝：非法哈希、未知提交、分支已存在、脏工作区硬重置没 --force、有未完成 rebase
#   F) 提示：被丢弃的提交已在远端 → 会分叉的告警
#
# 注意：试验场必须放在**工作区内**（%TEMP% 里 git 会被文件沙箱挡住，实测）
# 用法: pwsh -File tools\test-remote.ps1 [-Exe <GitRT.exe>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$LabRoot = "$PSScriptRoot\..\build\remote-lab"
)
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
function Git($dir, [string[]]$a) {
    $o = & git.exe -C $dir @a 2>&1
    return ($o | Out-String).Trim()
}
function Files($dir) { (Get-ChildItem $dir -File | Where-Object { $_.Name -notlike '.*' } | Measure-Object).Count }

$LabRoot = [System.IO.Path]::GetFullPath($LabRoot)
if (Test-Path $LabRoot) { Remove-Item $LabRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $LabRoot | Out-Null
$origin = Join-Path $LabRoot 'origin.git'
$work = Join-Path $LabRoot 'work'
$other = Join-Path $LabRoot 'other'

Write-Host "== 远端跟踪 / 按提交还原 端到端 ==" -ForegroundColor Cyan

# ------------------------------------------------------------------ 造远端
& git.exe init -q --bare $origin | Out-Null
& git.exe -C $origin symbolic-ref HEAD refs/heads/main | Out-Null
& git.exe clone -q $origin $work 2>&1 | Out-Null
& git.exe -C $work config user.name 'GitRT Demo'; & git.exe -C $work config user.email 'demo@e.invalid'
& git.exe -C $work config commit.gpgsign false
1..2 | ForEach-Object {
    Set-Content (Join-Path $work "f$_.txt") "c$_" -Encoding UTF8
    & git.exe -C $work add -A | Out-Null
    & git.exe -C $work commit -q -m "提交$_" | Out-Null
}
& git.exe -C $work push -q -u origin main 2>&1 | Out-Null
Check "试验场就绪（bare 远端 + 克隆 + 上游）" ((Git $work @('rev-parse','--abbrev-ref','@ {u}' -replace ' ','' )) -eq 'origin/main' -or (Git $work @('rev-parse','--abbrev-ref','--symbolic-full-name','@{u}')) -eq 'origin/main') "upstream=$(Git $work @('rev-parse','--abbrev-ref','--symbolic-full-name','@{u}'))"

function RunCli([string[]]$extra, [string]$tag) {
    $outFile = Join-Path $LabRoot "$tag.txt"
    Remove-Item $outFile -Force -ErrorAction SilentlyContinue
    $cliArgs = @() + $extra + @('--cwd', $work, '--out', $outFile)
    $argLine = ($cliArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru -NoNewWindow
    $p.WaitForExit(120000) | Out-Null
    $txt = ''
    for ($w = 0; $w -lt 60; $w++) {
        if (Test-Path $outFile) {
            $txt = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
            if ($txt -match '(?m)^ok=\d') { break }
        }
        Start-Sleep -Milliseconds 200
    }
    return [pscustomobject]@{ exit = $p.ExitCode; text = $txt; lines = @($txt -split "`r?`n") }
}
function Field($res, [string]$key) {
    $m = [regex]::Match($res.text, "(?m)^$([regex]::Escape($key))=(.*)$")
    return $m.Groups[1].Value.Trim()
}
function Has($res, [string]$pat) { return [bool]($res.text -match $pat) }

# ---------------------------------------------------------------- A) 远端前进
Write-Host "-- A) 远端前进 → 落后 --" -ForegroundColor Cyan
& git.exe clone -q $origin $other 2>&1 | Out-Null
& git.exe -C $other config user.name 'Other'; & git.exe -C $other config user.email 'o@e.invalid'
Set-Content (Join-Path $other 'r1.txt') 'remote1' -Encoding UTF8
& git.exe -C $other add -A | Out-Null; & git.exe -C $other commit -q -m '远端新增1' | Out-Null
& git.exe -C $other push -q origin main 2>&1 | Out-Null

$r = RunCli @('--remote-info','--remote-fetch') 'remote1'
Check "A 报告 ok=1" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "A 有上游 origin/main" ((Field $r 'upstream') -eq 'origin/main') "upstream=$(Field $r 'upstream')"
Check "A 落后 1（远端有本地没有）" ((Field $r 'behind') -eq '1') "behind=$(Field $r 'behind')"
Check "A 领先 0" ((Field $r 'ahead') -eq '0') "ahead=$(Field $r 'ahead')"
Check "A 基线文本里有「远端基线 origin/main」" (Has $r 'base\| == 远端基线 origin/main') ''
Check "A 列表里出现远端新增的提交" (Has $r '\[远端\] 远端新增1') ""

# ---------------------------------------------------------------- B) 本地领先
Write-Host "-- B) 本地提交 → 领先（以远端为基往上累加）--" -ForegroundColor Cyan
Set-Content (Join-Path $work 'l1.txt') 'local1' -Encoding UTF8
& git.exe -C $work add -A | Out-Null; & git.exe -C $work commit -q -m '本地新增1' | Out-Null
Set-Content (Join-Path $work 'l2.txt') 'local2' -Encoding UTF8
& git.exe -C $work add -A | Out-Null; & git.exe -C $work commit -q -m '本地新增2' | Out-Null
$hash2 = (Git $work @('rev-parse','HEAD~1'))
$hashOldest = (Git $work @('rev-parse','HEAD~3'))

$r = RunCli @('--remote-info') 'remote2'
Check "B 领先 2" ((Field $r 'ahead') -eq '2') "ahead=$(Field $r 'ahead')"
Check "B 落后 1" ((Field $r 'behind') -eq '1') "behind=$(Field $r 'behind')"
Check "B local_only=2" ((Field $r 'local_only') -eq '2') "local_only=$(Field $r 'local_only')"
$localMarks = ([regex]::Matches($r.text, '\[本地\]')).Count
Check "B 基线里 2 条标为【本地】" ($localMarks -eq 2) "marks=$localMarks"
Check "B 领先/落后写进了基线说明" (Has $r '本地新增（在远端之上累加）2 条，远端新增（本地还没有）1 条') ''

# ---------------------------------------------------------------- C) 上游 + 远端分支
Write-Host "-- C) 远端分支与上游 --" -ForegroundColor Cyan
& git.exe -C $work push -q origin HEAD:feature 2>&1 | Out-Null
& git.exe -C $work fetch -q origin 2>&1 | Out-Null
$r = RunCli @('--remote-info') 'remote3'
Check "C 列出 >=2 个远端分支" ([int](Field $r 'branches') -ge 2) "branches=$(Field $r 'branches')"
Check "C 标出当前上游" (Has $r '\| upstream') ''
Check "C 出现 origin/feature" (Has $r 'branch\d+=origin/feature') "text=$($r.text)"

& git.exe -C $work branch --unset-upstream | Out-Null
$r = RunCli @('--remote-info') 'remote4'
Check "C 取消上游后 has_upstream=0" ((Field $r 'has_upstream') -eq '0') "has=$(Field $r 'has_upstream')"
Check "C 无上游时基线文本如实说明" (Has $r '未设置上游') ''
$r = RunCli @('--set-upstream','origin/main') 'setu'
Check "C 设置上游成功" ((Field $r 'ok') -eq '1' -and (Field $r 'set_upstream_ok') -eq '1') "text=$($r.text)"
Check "C 设完后 has_upstream=1" ((Field (RunCli @('--remote-info') 'remote5') 'has_upstream') -eq '1') ''
$r = RunCli @('--set-upstream','nope/xxx') 'setbad'
Check "C 设不存在的上游 → 失败" ((Field $r 'ok') -eq '0') "text=$($r.text)"

# ---------------------------------------------------------------- D) 还原
Write-Host "-- D) 还原到提交（5 种）--" -ForegroundColor Cyan
# D1 只读检出
$r = RunCli @('--restore', $hash2, '--mode','detach','--dry-run') 'detach-dry'
Check "D1 detach 计划里的命令对" ((Field $r 'cmd') -match '^git checkout --detach ' + $hash2.Substring(0,7)) "cmd=$(Field $r 'cmd')"
$r = RunCli @('--restore', $hash2, '--mode','detach') 'detach'
Check "D1 detach 执行成功" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "D1 现在是分离头指针" ((Git $work @('rev-parse','--abbrev-ref','HEAD')) -eq 'HEAD') "head=$(Git $work @('rev-parse','--abbrev-ref','HEAD'))"
Check "D1 工作区内容 = 目标提交（l2.txt 不在）" (-not (Test-Path (Join-Path $work 'l2.txt'))) ''
Check "D1 分支没被移动（main 仍在原处）" ((Git $work @('rev-parse','main')) -ne $hash2) "main=$(Git $work @('rev-parse','main'))"

# D2 新建分支
$r = RunCli @('--restore', $hash2, '--mode','branch','--branch','restore-test') 'branch'
Check "D2 新建分支成功" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "D2 已在 restore-test 上" ((Git $work @('rev-parse','--abbrev-ref','HEAD')) -eq 'restore-test') "head=$(Git $work @('rev-parse','--abbrev-ref','HEAD'))"
Check "D2 指向目标提交" ((Git $work @('rev-parse','HEAD')) -eq $hash2) ''
Check "D2 内容 = 目标提交（有 l1.txt 无 l2.txt）" ((Test-Path (Join-Path $work 'l1.txt')) -and -not (Test-Path (Join-Path $work 'l2.txt'))) ''

# D3 软重置：HEAD 回到目标，改动进暂存区，工作区文件不动
& git.exe -C $work checkout -q main | Out-Null
& git.exe -C $work branch -D restore-test -q | Out-Null
$r = RunCli @('--restore', $hash2, '--mode','soft') 'soft'
Check "D3 soft 重置成功" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "D3 HEAD = 目标" ((Git $work @('rev-parse','HEAD')) -eq $hash2) ''
Check "D3 丢弃计数 = 1" ((Field $r 'drop') -eq '1') "drop=$(Field $r 'drop')"
Check "D3 改动进了暂存区" ((Git $work @('diff','--cached','--name-only')) -match 'l2.txt') "staged=$(Git $work @('diff','--cached','--name-only'))"
Check "D3 工作区文件还在（soft 不碰文件）" (Test-Path (Join-Path $work 'l2.txt')) ''

# D4 混合重置：改动退回工作区（未暂存）
$r = RunCli @('--restore', $hash2, '--mode','mixed') 'mixed'
Check "D4 mixed 重置成功" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "D4 暂存区已空" ((Git $work @('diff','--cached','--name-only')) -eq '') "staged=$(Git $work @('diff','--cached','--name-only'))"
Check "D4 被重置掉的文件变成未跟踪（内容还在）" ((Git $work @('status','--porcelain')) -match '\?\? l2.txt') "status=$(Git $work @('status','--porcelain'))"

# D5 硬重置：改动被丢弃
$r = RunCli @('--restore', $hash2, '--mode','hard','--force') 'hard'
Check "D5 hard 重置成功" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "D5 跟踪文件已对齐（只剩未跟踪项）" ((Git $work @('status','--porcelain')) -notmatch '^ M|^M ') "status=$(Git $work @('status','--porcelain'))"
Check "D5 未跟踪文件保留（git 硬重置不删未跟踪）" (Test-Path (Join-Path $work 'l2.txt')) ''
Check "D5 明确提示会丢弃未提交改动" (Has $r 'warn=.*丢弃') "warns=$(($r.lines | Where-Object { $_ -match '^warn=' }) -join ' ; ')"

# ---------------------------------------------------------------- E) 拒绝
Write-Host "-- E) 拒绝条件 --" -ForegroundColor Cyan
$r = RunCli @('--restore','zzz') 'bad-hash'
Check "E1 非法哈希被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '不合法') "error=$(Field $r 'error')"
$r = RunCli @('--restore','0123456789abcdef0123456789abcdef01234567') 'bad-missing'
Check "E2 未知提交被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '找不到') "error=$(Field $r 'error')"
$r = RunCli @('--restore', $hash2, '--mode','branch','--branch','main') 'bad-branch'
Check "E3 分支已存在被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '已存在') "error=$(Field $r 'error')"
$r = RunCli @('--restore', $hash2, '--mode','branch','--branch','bad name') 'bad-branch2'
Check "E4 非法分支名被拒" ((Field $r 'ok') -eq '0') "error=$(Field $r 'error')"
$r = RunCli @('--restore', $hash2, '--mode','nonsense') 'bad-mode'
Check "E5 未知模式被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '未知的还原方式') "error=$(Field $r 'error')"

# 脏工作区 + hard：没有 --force 要拒绝；给了才放行
Set-Content (Join-Path $work 'dirty.txt') 'dirty' -Encoding UTF8
$r = RunCli @('--restore', $hash2, '--mode','hard') 'dirty-no-force'
Check "E6 脏工作区 hard 没有 --force → 拒绝" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '--force') "error=$(Field $r 'error')"
Check "E6 拒绝时工作区没被动" (Test-Path (Join-Path $work 'dirty.txt')) ''
$r = RunCli @('--restore', $hash2, '--mode','hard','--force') 'dirty-force'
Check "E7 带 --force 才执行" ((Field $r 'ok') -eq '1') "text=$($r.text)"

# 未完成的 rebase：造一个 .git/rebase-merge 目录
New-Item -ItemType Directory -Force -Path (Join-Path $work '.git\rebase-merge') | Out-Null
$r = RunCli @('--restore', $hash2, '--mode','detach') 'pending'
Check "E8 有未完成 rebase → 拒绝" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '未完成') "error=$(Field $r 'error')"
Remove-Item (Join-Path $work '.git\rebase-merge') -Recurse -Force

Write-Host ""
# 已推送到 origin/main 的提交被重置掉 → 必须提示会分叉
& git.exe -C $work push -q origin main 2>&1 | Out-Null
& git.exe -C $work fetch -q origin 2>&1 | Out-Null
$r = RunCli @('--restore', $hashOldest, '--mode','mixed') 'pushed-reset'
Check "E9 丢弃已推送提交时给出分叉告警" (([int](Field $r 'dropped_pushed')) -ge 1 -and (Has $r 'warn=.*分叉')) "dropped=$(Field $r 'dropped_pushed') warns=$(($r.lines | Where-Object { $_ -match '^warn=' }) -join ' ; ')"

# ---------------------------------------------------------------- F) 远端地址
Write-Host "-- F) 远端地址管理 --" -ForegroundColor Cyan
$r = RunCli @('--remote-info') 'remotes'
Check "F1 报告里列出了远端 origin 及其地址" ((Field $r 'remotes') -ge 1 -and (Has $r 'remote0=origin \| ')) "remotes=$(Field $r 'remotes')"
$altUrl = (Join-Path $LabRoot 'alt.git')
& git.exe init -q --bare $altUrl | Out-Null
$r = RunCli @('--remote-set-url', "origin=$altUrl") 'seturl'
Check "F2 改远端地址成功" ((Field $r 'remote_set_url_ok') -eq '1') "text=$($r.text)"
Check "F2 git 里的地址确实变了" ((Git $work @('remote','get-url','origin')).Replace('\\','/').EndsWith('alt.git')) "url=$(Git $work @('remote','get-url','origin'))"
$r = RunCli @('--remote-set-url', "nosuch=http://x/y.git") 'seturl-bad'
Check "F3 改不存在的远端被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '没有这个远端') "error=$(Field $r 'error')"
$r = RunCli @('--remote-set-url', 'origin=-bad') 'seturl-bad2'   # 以 - 开头 = 选项注入
Check "F4 以 - 开头的地址被拒（防选项注入）" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '不能以 - 开头') "error=$(Field $r 'error')"
# 带空格的本地路径是**合法**地址，不能被误拒（校验要克制）
$spaceDir = Join-Path $LabRoot 'alt space.git'
& git.exe init -q --bare $spaceDir | Out-Null
$r = RunCli @('--remote-set-url', "origin=$spaceDir") 'seturl-space'
Check "F4b 带空格的本地路径被接受" ((Field $r 'remote_set_url_ok') -eq '1') "text=$($r.text)"
$r = RunCli @('--remote-set-url', "origin=$altUrl") 'seturl-back'
Check "F4b 地址改回 alt.git" ((Field $r 'remote_set_url_ok') -eq '1') ""
$r = RunCli @('--remote-add', "backup=$altUrl") 'add-remote'
Check "F5 新增远端成功" ((Field $r 'remote_add_ok') -eq '1') "text=$($r.text)"
$r = RunCli @('--remote-info') 'remotes2'
Check "F5 报告里现在有两个远端" ((Field $r 'remotes') -eq '2') "remotes=$(Field $r 'remotes')"
$r = RunCli @('--remote-add', "backup=$altUrl") 'add-dup'
Check "F6 重复新增被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '已存在') "error=$(Field $r 'error')"
$r = RunCli @('--remote-remove', 'backup') 'rm-remote'
Check "F7 删除远端成功" ((Field $r 'remote_remove_ok') -eq '1') "text=$($r.text)"

Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
