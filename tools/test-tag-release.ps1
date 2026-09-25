# ---------------------------------------------------------------------------
# 标签 / 发布 端到端（CLI 层，真 git 仓库 + 本地 bare 远端）
#
#   A) 打标签：轻量 / 附注（默认 = HEAD）→ 列表里能看到，附注/轻量标注正确
#   B) 拒绝：非法标签名、未知目标提交、重复标签没 --force、强制覆盖后才允许
#   C) 推送：单个标签 / --all → bare 远端的 ls-remote 能看到
#   D) 列表里的「远端:有」跟随推送变化
#   E) 删除：只删本地保留远端 / 同时删远端；没 --force 必须被拒（破坏性）
#   F) --dry-run 不产生任何副作用
#   G) 发布（gh）：没装 gh → 只断言"如实报告 + 不崩"，并计入 SKIP 不判失败；
#      装了 gh → 只测 --release-list（**绝不**真的创建 Release）
#
# 注意：试验场必须放在**工作区内**（%TEMP% 里 git 会被文件沙箱挡住，实测）
# 用法: pwsh -File tools\test-tag-release.ps1 [-Exe <GitRT.exe>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$LabRoot = "$PSScriptRoot\..\build\tag-release-lab"
)
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0; $skip = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
function Skip($what) {
    $script:skip++; Write-Host "  [SKIP] $what" -ForegroundColor DarkYellow
}
function Git($dir, [string[]]$a) {
    $o = & git.exe -C $dir @a 2>&1
    return ($o | Out-String).Trim()
}
function RemoteTags($bare) {
    # 注意：在 bare 仓库里执行 `ls-remote --tags origin` 是**错的**（bare 里没有 origin），
    # 必须直接把 bare 仓库当远端列（test-remote.ps1 里也是这么做的）
    return (Git $bare @('ls-remote','--tags', $bare))
}

$Exe = [System.IO.Path]::GetFullPath($Exe)
$LabRoot = [System.IO.Path]::GetFullPath($LabRoot)
if (-not (Test-Path $Exe)) {
    Write-Host "找不到 GitRT.exe：$Exe" -ForegroundColor Red
    Write-Host "== 结果: 0 通过 / 1 失败 ==" -ForegroundColor Cyan
    exit 1
}
if (Test-Path $LabRoot) { Remove-Item $LabRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $LabRoot | Out-Null
$origin = Join-Path $LabRoot 'origin.git'
$work = Join-Path $LabRoot 'work'

Write-Host "== 标签 / 发布 端到端 ==" -ForegroundColor Cyan

# ------------------------------------------------------------------ 造仓库
& git.exe init -q --bare $origin | Out-Null
& git.exe -C $origin symbolic-ref HEAD refs/heads/main | Out-Null
& git.exe clone -q $origin $work 2>&1 | Out-Null
& git.exe -C $work config user.name 'GitRT Demo'
& git.exe -C $work config user.email 'demo@e.invalid'
& git.exe -C $work config commit.gpgsign false
Set-Content (Join-Path $work 'r1.txt') 'r1' -Encoding UTF8
& git.exe -C $work add -A | Out-Null
& git.exe -C $work commit -q -m '初始化提交' | Out-Null
& git.exe -C $work push -q -u origin main 2>&1 | Out-Null
$head1 = (Git $work @('rev-parse','HEAD'))
Check "试验场就绪（bare 远端 + 克隆 + 一条提交）" ($head1.Length -eq 40) "head=$head1"

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
    # 进程已退出但报告还没落地时再补读一次（文件写入与进程退出有极小的竞态）
    if ([string]::IsNullOrEmpty($txt) -or $txt -notmatch '(?m)^ok=\d') {
        Start-Sleep -Milliseconds 500
        if (Test-Path $outFile) { $txt = Get-Content $outFile -Raw -ErrorAction SilentlyContinue }
    }
    return [pscustomobject]@{ exit = $p.ExitCode; text = $txt; lines = @($txt -split "`r?`n") }
}
function Field($res, [string]$key) {
    $m = [regex]::Match($res.text, "(?m)^$([regex]::Escape($key))=(.*)$")
    return $m.Groups[1].Value.Trim()
}
function Has($res, [string]$pat) { return [bool]($res.text -match $pat) }
function CmdLines($res) { return [string[]]@($res.lines | Where-Object { $_ -match '^cmd=' } | ForEach-Object { $_ -replace '^cmd=','' }) }

# ---------------------------------------------------------------- A) 打标签
Write-Host "-- A) 打标签（轻量 / 附注）--" -ForegroundColor Cyan
$r = RunCli @('--tag-list') 'list0'
Check "A0 空仓库标签列表 ok=1 且 count=0" ((Field $r 'ok') -eq '1' -and (Field $r 'count') -eq '0') "text=$($r.text)"

$r = RunCli @('--tag-create','v1.0.0') 'create-light'
Check "A1 轻量标签创建成功" ((Field $r 'ok') -eq '1' -and (Field $r 'created') -eq '1') "text=$($r.text)"
Check "A1 命令是 `git tag <name> <hash>`（没有 -a）" ((Field $r 'cmd') -match '^git tag v1\.0\.0 [0-9a-f]{40}$') "cmd=$(Field $r 'cmd')"
Check "A1 annotated=0" ((Field $r 'annotated') -eq '0') "annotated=$(Field $r 'annotated')"
Check "A1 git 里真的出现了轻量标签" ((Git $work @('cat-file','-t','v1.0.0')) -eq 'commit') "type=$(Git $work @('cat-file','-t','v1.0.0'))"

$r = RunCli @('--tag-create','v1.1.0','--annotated','--message','发布 1.1.0') 'create-annot'
Check "A2 附注标签创建成功" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "A2 命令带 -a -m 与信息" ((Field $r 'cmd') -match '^git tag -a v1\.1\.0 -m "发布 1\.1\.0" [0-9a-f]{40}$') "cmd=$(Field $r 'cmd')"
Check "A2 git 里是 tag 对象（附注）" ((Git $work @('cat-file','-t','v1.1.0')) -eq 'tag') "type=$(Git $work @('cat-file','-t','v1.1.0'))"

# 目标提交（HEAD~ 不需要；这里用一个显式哈希验证 --target）
$r = RunCli @('--tag-create','v0.9.0','--target',$head1) 'create-target'
Check "A3 --target <哈希> 打标签成功且 target 就是它" ((Field $r 'ok') -eq '1' -and (Field $r 'target') -eq $head1) "target=$(Field $r 'target') text=$($r.text)"
Check "A3 git 里 v0.9.0 指向该提交" ((Git $work @('rev-list','-n','1','v0.9.0')) -eq $head1) "tip=$(Git $work @('rev-list','-n','1','v0.9.0'))"

$r = RunCli @('--tag-list') 'list1'
Check "A4 列表 count=3" ((Field $r 'count') -eq '3') "count=$(Field $r 'count')"
Check "A4 列表能看到 v1.1.0" (Has $r '(?m)^tag\d+=v1\.1\.0\r?$') "text=$($r.text)"
$iAnnot = -1; $iLight = -1
for ($i = 0; $i -lt 3; $i++) {
    if ((Field $r "tag$i") -eq 'v1.1.0') { $iAnnot = $i }
    if ((Field $r "tag$i") -eq 'v1.0.0') { $iLight = $i }
}
Check "A4 附注标签标为 annotated=1" ($iAnnot -ge 0 -and (Field $r "annotated$iAnnot") -eq '1') "annotated=$iAnnot"
Check "A4 轻量标签标为 annotated=0" ($iLight -ge 0 -and (Field $r "annotated$iLight") -eq '0') "annotated=$iLight"
Check "A4 未推送时 remote=0" ((Field $r "remote$iLight") -eq '0') "remote=$(Field $r "remote$iLight")"
Check "A4 面板文本标出「附注/轻量」" (Has $r 'describe\| .*附注' -and (Has $r 'describe\| .*轻量')) ''

# ---------------------------------------------------------------- B) 拒绝
Write-Host "-- B) 拒绝条件 --" -ForegroundColor Cyan
$r = RunCli @('--tag-create','bad name') 'bad-name'
Check "B1 非法标签名被拒（标签名不合法）" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '标签名不合法') "error=$(Field $r 'error')"
Check "B1 拒绝后没有创建任何东西" ((Git $work @('tag','-l','bad name')) -eq '') ''
$r = RunCli @('--tag-create','v2.0.0','--target','no-such-rev') 'bad-target'
Check "B2 未知目标提交被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '找不到这个提交') "error=$(Field $r 'error')"
Check "B2 拒绝后 v2.0.0 不存在" ((Git $work @('tag','-l','v2.0.0')) -eq '') ''
$r = RunCli @('--tag-create','v1.0.0') 'dup'
Check "B3 重复标签没 --force → 拒绝且提示 force" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '已存在' -and (Field $r 'error') -match 'force') "error=$(Field $r 'error')"

# 造一条新提交，把 v1.0.0 覆盖过去（force 才允许）
Set-Content (Join-Path $work 'r2.txt') 'r2' -Encoding UTF8
& git.exe -C $work add -A | Out-Null
& git.exe -C $work commit -q -m '第二条提交' | Out-Null
$head2 = (Git $work @('rev-parse','HEAD'))
$r = RunCli @('--tag-create','v1.0.0','--force') 'force'
Check "B4 --force 覆盖成功" ((Field $r 'ok') -eq '1') "text=$($r.text)"
Check "B4 命令带 -f" ((Field $r 'cmd') -match '^git tag -f v1\.0\.0 [0-9a-f]{40}$') "cmd=$(Field $r 'cmd')"
Check "B4 覆盖前给出「会移动标签」告警" (Has $r 'warn=.*移动') "warns=$(($r.lines | Where-Object { $_ -match '^warn=' }) -join ' ; ')"
Check "B4 v1.0.0 现在指向新提交" ((Git $work @('rev-list','-n','1','v1.0.0')) -eq $head2) "tip=$(Git $work @('rev-list','-n','1','v1.0.0'))"

# ---------------------------------------------------------------- C) 推送
Write-Host "-- C) 推送标签到 bare 远端 --" -ForegroundColor Cyan
$r = RunCli @('--tag-push','v1.1.0','--remote','origin') 'push1'
Check "C1 推送单个标签成功" ((Field $r 'ok') -eq '1' -and (Field $r 'pushed') -eq '1') "text=$($r.text)"
Check "C1 命令是 git push origin v1.1.0" ((Field $r 'cmd') -match '^git push origin v1\.1\.0$') "cmd=$(Field $r 'cmd')"
Check "C1 bare 远端 ls-remote 能看到该标签" ((RemoteTags $origin) -match 'refs/tags/v1\.1\.0') "remote=$(RemoteTags $origin)"
$r = RunCli @('--tag-push','v9.9.9') 'push-missing'
Check "C3 推送不存在的标签被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '本地没有这个标签') "error=$(Field $r 'error')"
$r = RunCli @('--tag-push','v1.1.0','--remote','nosuch') 'push-badremote'
Check "C4 推送到不存在的远端被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '没有这个远端') "error=$(Field $r 'error')"

# ---------------------------------------------------------------- D) 列表的远端标注
Write-Host "-- D) 列表里的「远端:有/无」--" -ForegroundColor Cyan
# 造一个**从没推送过**的标签，验证它是「远端:无」（放在 --all 之前，否则会被一起推上去）
& git.exe -C $work tag v0.8.0 v1.0.0 2>&1 | Out-Null
$r = RunCli @('--tag-list') 'list2'
$iPushed = -1; $iLocal = -1
for ($i = 0; $i -lt 4; $i++) {
    if ((Field $r "tag$i") -eq 'v1.1.0') { $iPushed = $i }
    if ((Field $r "tag$i") -eq 'v0.8.0') { $iLocal = $i }
}
Check "D1 推过的 v1.1.0 标为远端已有" ($iPushed -ge 0 -and (Field $r "remote$iPushed") -eq '1') "remote=$(Field $r "remote$iPushed")"
Check "D2 没推过的 v0.8.0 标为远端没有" ($iLocal -ge 0 -and (Field $r "remote$iLocal") -eq '0') "remote=$(Field $r "remote$iLocal")"

$r = RunCli @('--tag-push','v1.1.0','--all') 'pushall'
Check "C2 --all 推送全部标签" ((Field $r 'ok') -eq '1' -and (Field $r 'pushed') -eq '1') "text=$($r.text)"
Check "C2 命令是 git push origin --tags" ((Field $r 'cmd') -match '^git push origin --tags$') "cmd=$(Field $r 'cmd')"
Check "C2 提示会把所有标签都推上去" (Has $r 'warn=.*所有') "warns=$(($r.lines | Where-Object { $_ -match '^warn=' }) -join ' ; ')"
Check "C2 三个之前的标签都到了远端" (((RemoteTags $origin) -match 'refs/tags/v0\.8\.0') -and ((RemoteTags $origin) -match 'refs/tags/v0\.9\.0') -and ((RemoteTags $origin) -match 'refs/tags/v1\.1\.0')) "remote=$(RemoteTags $origin)"

# ---------------------------------------------------------------- E) 删除
Write-Host "-- E) 删除标签（破坏性）--" -ForegroundColor Cyan
& git.exe -C $work tag del-remote v1.0.0 2>&1 | Out-Null
& git.exe -C $work push -q origin del-remote 2>&1 | Out-Null
Check "E0 夹具：del-remote 本地与远端都存在" (((Git $work @('tag','-l','del-remote')) -eq 'del-remote') -and ((RemoteTags $origin) -match 'refs/tags/del-remote')) "remote=$(RemoteTags $origin)"

$r = RunCli @('--tag-delete','del-remote') 'del-noforce'
Check "E1 删除没 --force → 拒绝" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '--force') "error=$(Field $r 'error')"
Check "E1 计划标出 destructive=1" ((Field $r 'destructive') -eq '1') "destructive=$(Field $r 'destructive')"
Check "E1 拒绝时标签还在（没删）" ((Git $work @('tag','-l','del-remote')) -eq 'del-remote') ''

$r = RunCli @('--tag-delete','del-remote','--force') 'del-local'
Check "E2 只删本地成功" ((Field $r 'ok') -eq '1' -and (Field $r 'deleted_local') -eq '1') "text=$($r.text)"
$cmdsE2 = @(CmdLines $r)
Check "E2 命令只有 git tag -d 一条" ($cmdsE2.Count -eq 1) "cmds=$($cmdsE2 -join ' | ')"
Check "E2 那条命令就是 git tag -d del-remote" ($cmdsE2.Count -ge 1 -and $cmdsE2[0] -match '^git tag -d del-remote$') "cmds=$($cmdsE2 -join ' | ')"
Check "E2 本地没了、远端还在" (((Git $work @('tag','-l','del-remote')) -eq '') -and ((RemoteTags $origin) -match 'refs/tags/del-remote')) "remote=$(RemoteTags $origin)"

& git.exe -C $work tag del-both v1.0.0 2>&1 | Out-Null
& git.exe -C $work push -q origin del-both 2>&1 | Out-Null
$r = RunCli @('--tag-delete','del-both','--force','--remote') 'del-remote'
Check "E3 同时删本地与远端成功" ((Field $r 'ok') -eq '1' -and (Field $r 'deleted_local') -eq '1' -and (Field $r 'deleted_remote') -eq '1') "text=$($r.text)"
Check "E3 两条命令（tag -d + push :refs/tags）" ((CmdLines $r).Count -eq 2 -and (CmdLines $r)[1] -match '^git push origin :refs/tags/del-both$') "cmds=$((CmdLines $r) -join ' | ')"
Check "E3 本地与远端都没有了" (((Git $work @('tag','-l','del-both')) -eq '') -and ((RemoteTags $origin) -notmatch 'refs/tags/del-both')) "remote=$(RemoteTags $origin)"

$r = RunCli @('--tag-delete','v9.9.9','--force') 'del-missing'
Check "E4 删除不存在的标签被拒" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '本地没有这个标签') "error=$(Field $r 'error')"

# ---------------------------------------------------------------- F) dry-run
Write-Host "-- F) --dry-run 不产生副作用 --" -ForegroundColor Cyan
$tagsBefore = (Git $work @('tag','-l'))
$r = RunCli @('--tag-create','v3.0.0','--dry-run') 'dry-create'
Check "F1 dry-run 报 ok=1 且 dry_run=1" ((Field $r 'ok') -eq '1' -and (Field $r 'dry_run') -eq '1') "text=$($r.text)"
Check "F1 dry-run 后 v3.0.0 不存在" ((Git $work @('tag','-l','v3.0.0')) -eq '') ''
$r = RunCli @('--tag-push','v1.1.0','--dry-run') 'dry-push'
Check "F2 dry-run 推送 ok=1 且 pushed=0" ((Field $r 'ok') -eq '1' -and (Field $r 'pushed') -eq '0') "text=$($r.text)"
$r = RunCli @('--tag-delete','v1.1.0','--force','--dry-run') 'dry-del'
Check "F3 dry-run 删除 ok=1 且 deleted_local=0" ((Field $r 'ok') -eq '1' -and (Field $r 'deleted_local') -eq '0') "text=$($r.text)"
Check "F3 dry-run 后标签一个都没少" ((Git $work @('tag','-l')) -eq $tagsBefore) "before=$tagsBefore now=$(Git $work @('tag','-l'))"

# ---------------------------------------------------------------- G) 发布（gh）
Write-Host "-- G) 发布列表（gh）--" -ForegroundColor Cyan
$ghCmd = Get-Command gh -ErrorAction SilentlyContinue
$r = RunCli @('--release-list') 'rel'
if ($null -eq $ghCmd) {
    Check "G0 没装 gh → 如实报告 gh=0 且中文提示怎么装（不崩）" ((Field $r 'ok') -eq '0' -and (Field $r 'gh') -eq '0' -and (Has $r 'winget install GitHub\.cli')) "text=$($r.text)"
    Skip "gh 未安装：跳过真实 Release 列表断言"
} else {
    Check "G1 gh 已安装 → gh=1" ((Field $r 'gh') -eq '1') "text=$($r.text)"
    if ((Field $r 'ok') -eq '1') {
        Check "G1 release-list 有 count= 字段" ((Field $r 'count') -ne '') "count=$(Field $r 'count')"
    } else {
        Check "G1 release-list 失败时也有 error=（未登录/非 GitHub 仓库不算崩）" ((Field $r 'error') -ne '') "error=$(Field $r 'error')"
        Skip "gh 未登录或该仓库不是 GitHub 仓库：跳过 count 断言"
    }
    # 附件缺失必须被拒（不发任何网络请求）
    $r = RunCli @('--release-create','v1.1.0','--asset',(Join-Path $LabRoot 'no-such-asset.bin'),'--dry-run') 'rel-asset'
    Check "G2 附件不存在 → 拒绝并指名" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '找不到附件文件') "error=$(Field $r 'error')"
    $r = RunCli @('--release-create','no-such-tag-xyz','--dry-run') 'rel-notag'
    Check "G3 标签不存在 → 拒绝" ((Field $r 'ok') -eq '0' -and (Field $r 'error') -match '标签不存在') "error=$(Field $r 'error')"
}

Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 / $skip 跳过 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
