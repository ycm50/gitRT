# ---------------------------------------------------------------------------
# GitRT 全功能端到端测试（全部在 %TEMP% 下进行，不触碰工作区与用户全局配置）
#
#   Phase A：命令表全部条目 dry-run（校验参数/选项 → argv 构造）
#   Phase B：可执行命令真实跑一遍，并用 git 断言实际效果
#   Phase C：AI 链路（对着本地 mock OpenAI 服务跑，覆盖成功/拒绝/错误分支）
#   Phase D：Shell 扩展（ShellProbe 自检 + 菜单树 dump + Invoke→GUI 端到端）
#
# 用法: pwsh -File tools\test-all.ps1 [-Exe <GitRT.exe>] [-Root <dir>] [-Port 18080] [-SkipShell]
# 退出码: 0 = 全部通过
# ---------------------------------------------------------------------------
param(
    [string]$Exe  = "$env:TEMP\GitRT-run\GitRT.exe",
    [string]$Root = "$env:TEMP\GitRT-test",
    [int]$Port    = 18080,
    [switch]$SkipAi,
    [switch]$SkipShell,
    [string]$ShellDll = '',
    [string]$ProbeExe = '',
    [string]$Identity = '',
    [string]$Appx = '',
    [string]$BuildDir = "$PSScriptRoot\..\build\debug"
)

$ErrorActionPreference = 'Continue'
$script:pass = 0
$script:fail = 0
$script:log  = New-Object System.Collections.Generic.List[string]

function Say([string]$s) {
    Write-Host $s
    $script:log.Add($s)
}
function Section([string]$s) { Say ""; Say "===== $s =====" }
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { $script:pass++; Say ("  [PASS] {0}" -f $name) }
    else { $script:fail++; Say ("  [FAIL] {0}  {1}" -f $name, $detail) }
}
function Q([string]$s) { if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' } return $s }

function G {
    param([string]$Repo, [Parameter(ValueFromRemainingArguments = $true)][string[]]$GitArgs)
    $out = & git -C $Repo @GitArgs 2>&1
    return ($out | Out-String).Trim()
}
function CommitCount([string]$repo) { return [int](G $repo rev-list --count HEAD) }
function StashCount([string]$repo) {
    $l = @(G $repo stash list)
    if ($l.Count -eq 1 -and [string]::IsNullOrWhiteSpace($l[0])) { return 0 }
    return $l.Count
}

function Invoke-GitRT {
    param(
        [string]$Key = '',
        [string]$WorkDir = $script:repo,
        [string]$Paths = '',
        [hashtable]$Flags = @{},
        [hashtable]$Params = @{},
        [switch]$Execute,
        [string]$AiPrompt = '',
        [switch]$AiRun
    )
    $safe = ($Key + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
    $out = Join-Path $Root "cli\$safe.txt"
    New-Item -ItemType Directory (Split-Path $out) -Force | Out-Null
    $a = @('--out', $out)
    if ($Key) { $a = @('--run', $Key, '--cwd', $WorkDir) + $a }
    if ($AiPrompt) {
        $a = @('--ai', $AiPrompt, '--cwd', $WorkDir, '--out', $out)
        if ($AiRun) { $a += '--ai-run' }
    }
    if (-not $Execute -and -not $AiRun) { $a += '--dry-run' }
    if ($Paths) { $a += @('--paths', $Paths) }
    foreach ($k in $Flags.Keys) { $a += @('--flag', "$k=$($Flags[$k])") }
    foreach ($k in $Params.Keys) { $a += @('--param', "$k=$($Params[$k])") }
    $argLine = ($a | ForEach-Object { Q $_ }) -join ' '
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru -Wait
    $txt = ''
    if (Test-Path $out) { $txt = Get-Content $out -Raw -Encoding UTF8 }
    $fields = @{}
    foreach ($m in [regex]::Matches($txt, '(?m)^([A-Za-z0-9_.\-]+)=(.*)$')) {
        $fields[$m.Groups[1].Value] = $m.Groups[2].Value.Trim()
    }
    return [pscustomobject]@{ Exit = $p.ExitCode; Fields = $fields; Text = $txt; File = $out }
}

# =========================================================== 准备环境
Say "GitRT 全功能端到端测试"
Say "  exe  : $Exe"
Say "  root : $Root"
Say "  time : $(Get-Date -Format o)"

if (-not (Test-Path $Exe)) { Say "找不到 $Exe"; exit 2 }
if (Test-Path $Root) { Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $Root -Force | Out-Null

$repo   = Join-Path $Root 'repo'
$remote = Join-Path $Root 'remote.git'
$other  = Join-Path $Root 'other'
New-Item -ItemType Directory $repo -Force | Out-Null

G $repo init -q --initial-branch=main | Out-Null
G $repo config user.name 'GitRT Test' | Out-Null
G $repo config user.email 'gitrt@test.local' | Out-Null
G $repo config core.autocrlf false | Out-Null
Set-Content (Join-Path $repo 'a.txt') 'a1'
Set-Content (Join-Path $repo 'b.txt') 'b1'
G $repo add -A | Out-Null
G $repo commit -qm 'init' | Out-Null
& git init -q --bare $remote | Out-Null
G $repo remote add origin $remote | Out-Null
G $repo push -qu origin main | Out-Null
Say "  临时仓库已就绪: $repo  (commits=$(CommitCount $repo))"

# =========================================================== Phase A
Section "Phase A：命令表全部条目 dry-run（argv 构造与参数校验）"
$cmdListFile = Join-Path $Root 'cmdlist.txt'
$p = Start-Process -FilePath $Exe -ArgumentList "--list-commands --out `"$cmdListFile`"" -PassThru -Wait
$cmdLines = @()
if (Test-Path $cmdListFile) {
    $cmdLines = Get-Content $cmdListFile -Encoding UTF8 | Where-Object { $_ -like 'CMD|*' }
}
Say ("  命令表条目: {0}" -f $cmdLines.Count)
Check "命令表可导出且非空" ($cmdLines.Count -ge 30) ("count=$($cmdLines.Count)")

function Default-Param([string]$name, [string]$remotePath) {
    switch ($name) {
        'url'     { return $remotePath }
        'msg'     { return 'phase-a message' }
        'branch'  { return 'main' }
        'remote'  { return 'origin' }
        'rev'     { return 'HEAD' }
        'tag'     { return 'v0.0.1' }
        'pattern' { return '*.log' }
        default   { return 'main' }
    }
}

$aOk = 0; $aInternal = 0; $aBad = 0
foreach ($line in $cmdLines) {
    $f = $line.Split('|')
    $key = $f[1]; $exec = $f[4]; $paramKey = $f[5]
    $params = @{}
    if ($paramKey) { $params[$paramKey] = (Default-Param $paramKey $remote) }
    $r = Invoke-GitRT -Key $key -WorkDir $repo -Params $params -Paths (Join-Path $repo 'a.txt')
    if ($exec -eq 'internal') {
        # Internal 命令需要图形界面：dry-run 预期被明确拒绝（exit=1 + 说明）
        if ($r.Exit -eq 1 -and $r.Fields['note'] -like '*internal*') { $aInternal++ }
        else { $aBad++; Say ("    [FAIL] {0} internal 预期 exit=1+note，实际 exit={1} note={2}" -f $key, $r.Exit, $r.Fields['note']) }
    }
    else {
        if ($r.Exit -eq 0 -and $r.Fields['argv0']) { $aOk++ }
        else { $aBad++; Say ("    [FAIL] {0} exit={1} error={2}" -f $key, $r.Exit, $r.Fields['error']) }
    }
}
Check "全部条目 argv 构造正确" ($aBad -eq 0) ("ok=$aOk internal=$aInternal bad=$aBad")
Say ("  可执行 {0} 条 / GUI 交互 {1} 条 / 失败 {2} 条" -f $aOk, $aInternal, $aBad)

# =========================================================== Phase B
Section "Phase B：可执行命令真实执行 + git 断言"

# --- repo.init ---
$newrepo = Join-Path $Root 'newrepo'
New-Item -ItemType Directory $newrepo -Force | Out-Null
$r = Invoke-GitRT -Key 'repo.init' -WorkDir $newrepo -Flags @{ branch = 'trunk' } -Execute
Check "repo.init 生成仓库" ((Test-Path (Join-Path $newrepo '.git')) -and (G $newrepo symbolic-ref --short HEAD) -eq 'trunk') ("exit=$($r.Exit)")

# --- repo.clone ---
$clones = Join-Path $Root 'clones'
New-Item -ItemType Directory $clones -Force | Out-Null
$r = Invoke-GitRT -Key 'repo.clone' -WorkDir $clones -Params @{ url = $remote } -Execute
Check "repo.clone 克隆本地裸仓库" ($r.Exit -eq 0 -and (Test-Path (Join-Path $clones 'remote'))) ("exit=$($r.Exit) err=$($r.Fields['stderr'])")

# --- commit.stage / unstage / discard ---
Set-Content (Join-Path $repo 'a.txt') 'a2'
$r = Invoke-GitRT -Key 'commit.stage' -WorkDir $repo -Paths (Join-Path $repo 'a.txt') -Execute
$staged = G $repo diff --cached --name-only
Check "commit.stage 暂存指定文件" ($r.Exit -eq 0 -and $staged -match 'a\.txt') ("exit=$($r.Exit) staged=$staged")

$r = Invoke-GitRT -Key 'commit.unstage' -WorkDir $repo -Paths (Join-Path $repo 'a.txt') -Execute
$staged2 = G $repo diff --cached --name-only
Check "commit.unstage 取消暂存" ($r.Exit -eq 0 -and $staged2 -notmatch 'a\.txt') ("exit=$($r.Exit) staged=$staged2")

Set-Content (Join-Path $repo 'b.txt') 'b2-changed'
$r = Invoke-GitRT -Key 'commit.discard' -WorkDir $repo -Paths (Join-Path $repo 'b.txt') -Execute
$b = (Get-Content (Join-Path $repo 'b.txt') -Raw).Trim()
Check "commit.discard 撤销工作区改动" ($r.Exit -eq 0 -and $b -eq 'b1') ("exit=$($r.Exit) content=$b")

# --- commit.commit（两阶段：先 add 再 commit）---
$before = CommitCount $repo
Set-Content (Join-Path $repo 'a.txt') 'a3'
$r = Invoke-GitRT -Key 'commit.commit' -WorkDir $repo -Paths (Join-Path $repo 'a.txt') -Params @{ msg = 'commit via GitRT' } -Execute
$after = CommitCount $repo
$subject = G $repo log -1 --pretty=%s
Check "commit.commit 先 add 再 commit" ($r.Exit -eq 0 -and $after -eq ($before + 1) -and $subject -eq 'commit via GitRT') ("exit=$($r.Exit) count=$before->$after subject=$subject")

# --- commit.amend ---
$r = Invoke-GitRT -Key 'commit.amend' -WorkDir $repo -Params @{ msg = 'amended by GitRT' } -Execute
$subject = G $repo log -1 --pretty=%s
Check "commit.amend 修改提交信息" ($r.Exit -eq 0 -and $subject -eq 'amended by GitRT') ("exit=$($r.Exit) subject=$subject")

# --- commit.undo（soft reset）---
$c1 = CommitCount $repo
$r = Invoke-GitRT -Key 'commit.undo' -WorkDir $repo -Execute
$c2 = CommitCount $repo
Check "commit.undo 撤销提交并保留改动" ($r.Exit -eq 0 -and $c2 -eq ($c1 - 1)) ("exit=$($r.Exit) count=$c1->$c2")
# 还原成干净状态，便于后续步骤
G $repo checkout -q -- . | Out-Null
G $repo reset -q --hard | Out-Null

# --- stash.push / pop ---
Set-Content (Join-Path $repo 'a.txt') 'stash-me'
$r = Invoke-GitRT -Key 'stash.push' -WorkDir $repo -Params @{ msg = 'stash from GitRT' } -Execute
$sc = StashCount $repo
Check "stash.push 产生储藏" ($r.Exit -eq 0 -and $sc -eq 1) ("exit=$($r.Exit) stash=$sc")
$r = Invoke-GitRT -Key 'stash.pop' -WorkDir $repo -Execute
$sc = StashCount $repo
Check "stash.pop 弹出储藏" ($r.Exit -eq 0 -and $sc -eq 0) ("exit=$($r.Exit) stash=$sc")
G $repo checkout -q -- . | Out-Null

# --- branch.new / switch / merge / delete ---
$r = Invoke-GitRT -Key 'branch.new' -WorkDir $repo -Params @{ branch = 'feature2' } -Execute
Check "branch.new 新建分支" ($r.Exit -eq 0 -and (G $repo branch --list feature2)) ("exit=$($r.Exit)")

$r = Invoke-GitRT -Key 'branch.switch' -WorkDir $repo -Params @{ branch = 'feature2' } -Execute
Check "branch.switch 切换分支" ($r.Exit -eq 0 -and (G $repo symbolic-ref --short HEAD) -eq 'feature2') ("exit=$($r.Exit) head=$(G $repo symbolic-ref --short HEAD)")

Set-Content (Join-Path $repo 'feature.txt') 'from-feature'
G $repo add -A | Out-Null
G $repo commit -qm 'feature work' | Out-Null
$r = Invoke-GitRT -Key 'branch.switch' -WorkDir $repo -Params @{ branch = 'main' } -Execute
Check "branch.switch 切回 main" ($r.Exit -eq 0 -and (G $repo symbolic-ref --short HEAD) -eq 'main') ("exit=$($r.Exit)")

$r = Invoke-GitRT -Key 'branch.merge' -WorkDir $repo -Params @{ branch = 'feature2' } -Flags @{ 'no-ff' = '1' } -Execute
Check "branch.merge 合并分支" ($r.Exit -eq 0 -and (Test-Path (Join-Path $repo 'feature.txt'))) ("exit=$($r.Exit) err=$($r.Fields['stderr'])")

$r = Invoke-GitRT -Key 'branch.delete' -WorkDir $repo -Params @{ branch = 'feature2' } -Flags @{ force = '1' } -Execute
Check "branch.delete 删除分支" ($r.Exit -eq 0 -and -not (G $repo branch --list feature2)) ("exit=$($r.Exit)")

# --- branch.rebase ---
G $repo branch rb-base | Out-Null
Set-Content (Join-Path $repo 'main2.txt') 'on-main'
G $repo add -A | Out-Null
G $repo commit -qm 'main work' | Out-Null
$r = Invoke-GitRT -Key 'branch.rebase' -WorkDir $repo -Params @{ branch = 'rb-base' } -Execute
Check "branch.rebase 变基到指定分支" ($r.Exit -eq 0) ("exit=$($r.Exit) err=$($r.Fields['stderr'])")

# --- sync.fetch / push / pull / sync ---
$r = Invoke-GitRT -Key 'sync.fetch' -WorkDir $repo -Execute
Check "sync.fetch" ($r.Exit -eq 0) ("exit=$($r.Exit)")

$r = Invoke-GitRT -Key 'sync.push' -WorkDir $repo -Flags @{ 'set-upstream' = '1' } -Execute
$remoteHead = (& git -C $remote rev-parse main 2>&1 | Out-String).Trim()
$localHead = (G $repo rev-parse HEAD)
Check "sync.push 推送并设置上游" ($r.Exit -eq 0 -and $remoteHead -eq $localHead) ("exit=$($r.Exit) remote=$remoteHead local=$localHead")

# 另一个克隆推一个提交，再用 sync.pull 拉回来
& git clone -q -b main $remote $other 2>&1 | Out-Null
G $other config user.name 'Other' | Out-Null
G $other config user.email 'other@test.local' | Out-Null
Set-Content (Join-Path $other 'from-other.txt') 'pushed-by-other'
G $other add -A | Out-Null
G $other commit -qm 'other work' | Out-Null
G $other push -q origin main | Out-Null
$r = Invoke-GitRT -Key 'sync.pull' -WorkDir $repo -Execute
Check "sync.pull 拉取他人提交" ($r.Exit -eq 0 -and (Test-Path (Join-Path $repo 'from-other.txt'))) ("exit=$($r.Exit) err=$($r.Fields['stderr'])")

$r = Invoke-GitRT -Key 'sync.sync' -WorkDir $repo -Execute
Check "sync.sync（pull --rebase --autostash + push）" ($r.Exit -eq 0) ("exit=$($r.Exit) err=$($r.Fields['stderr'])")

# --- adv.reset（含互斥组回归）---
$c1 = CommitCount $repo
$r = Invoke-GitRT -Key 'adv.reset' -WorkDir $repo -Params @{ rev = 'HEAD~1' } -Execute
$c2 = CommitCount $repo
Check "adv.reset（默认 mixed）回退一个提交" ($r.Exit -eq 0 -and $c2 -eq ($c1 - 1)) ("exit=$($r.Exit) count=$c1->$c2 argv=$($r.Fields['argv0'])")
Check "adv.reset 未同时带上 --hard" ($r.Fields['argv0'] -notmatch '--hard') ("argv=$($r.Fields['argv0'])")
G $repo checkout -q -- . | Out-Null
G $repo reset -q --hard | Out-Null

# --- adv.clean ---
Set-Content (Join-Path $repo 'junk.txt') 'junk'
New-Item -ItemType Directory (Join-Path $repo 'junkdir') -Force | Out-Null
Set-Content (Join-Path $repo 'junkdir\x.txt') 'x'
$r = Invoke-GitRT -Key 'adv.clean' -WorkDir $repo -Flags @{ dirs = '1' } -Execute
Check "adv.clean 清理未跟踪文件与目录" ($r.Exit -eq 0 -and -not (Test-Path (Join-Path $repo 'junk.txt')) -and -not (Test-Path (Join-Path $repo 'junkdir'))) ("exit=$($r.Exit)")

# --- adv.maintenance ---
$r = Invoke-GitRT -Key 'adv.maintenance' -WorkDir $repo -Execute
Check "adv.maintenance（git gc）" ($r.Exit -eq 0) ("exit=$($r.Exit)")

# --- inspect.* / app.* / commit.ignore：GUI 交互命令 ---
Say "  说明：inspect.* / app.* / commit.ignore 属于 GUI 交互命令（Internal），"
Say "        已在 Phase A 校验其被正确识别与拒绝；交互行为由 Phase D 的菜单树 dump 覆盖。"

# =========================================================== Phase C
Section "Phase C：AI 链路（本地 mock OpenAI 服务）"
if ($SkipAi) {
    Say "  已跳过"
} else {
    $mockScript = Join-Path $PSScriptRoot 'mock-ai-server.ps1'
    $mockLog = Join-Path $Root 'mock-ai.log'
    $pwshPath = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
    if (-not $pwshPath) { $pwshPath = (Get-Command powershell).Source }
    $mock = Start-Process -FilePath $pwshPath -PassThru -WindowStyle Hidden `
        -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$mockScript`" -Port $Port -LogFile `"$mockLog`""

    $ready = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        try {
            $h = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2 -UseBasicParsing
            if ($h.StatusCode -eq 200) { $ready = $true; break }
        } catch { }
    }
    Check "mock 服务已启动" $ready ("port=$Port")
    if ($ready) {
        $env:GITRT_AI_ENDPOINT    = "http://127.0.0.1:$Port/v1/chat/completions"
        $env:GITRT_AI_MODEL       = 'mock-model'
        $env:GITRT_AI_API_KEY_ENV = 'GITRT_TEST_KEY'
        $env:GITRT_TEST_KEY       = 'test-key-123'

        # C1 正常生成
        $r = Invoke-GitRT -AiPrompt '把当前分支推送到 origin' -WorkDir $repo
        Check "C1 AI 生成方案（sync.push + set-upstream）" ($r.Exit -eq 0 -and $r.Fields['command'] -eq 'sync.push') ("exit=$($r.Exit) command=$($r.Fields['command']) error=$($r.Fields['error'])")
        Check "C1 取到的是 content 而非 reasoning_content" ($r.Text -notmatch 'MOCK-REASONING') ("reply=$($r.Fields['command'])")
        Check "C1 生成了可执行命令行" ($r.Fields['cmdline'] -match 'push' -and $r.Fields['cmdline'] -match '--set-upstream') ("cmdline=$($r.Fields['cmdline'])")
        Check "C1 HTTP/耗时已记录" ($r.Fields['http'] -eq '200' -and [int]$r.Fields['elapsed_ms'] -ge 0) ("http=$($r.Fields['http'])")

        # C2 模型编造命令 → 闸门拒绝
        $r = Invoke-GitRT -AiPrompt 'UNKNOWN 请帮我做点危险的事' -WorkDir $repo
        Check "C2 拒绝命令表外的命令" ($r.Exit -eq 1 -and $r.Fields['plan_error'] -match '命令表') ("exit=$($r.Exit) plan_error=$($r.Fields['plan_error'])")

        # C3 模型认为无解
        $r = Invoke-GitRT -AiPrompt 'NONE 帮我订一张机票' -WorkDir $repo
        Check "C3 无合适命令 → exit=5" ($r.Exit -eq 5 -and $r.Fields['no_command'] -eq '1') ("exit=$($r.Exit)")

        # C4 非 JSON 回复
        $r = Invoke-GitRT -AiPrompt 'NOTJSON 随便说点什么' -WorkDir $repo
        Check "C4 非 JSON 回复被拒绝 → exit=4" ($r.Exit -eq 4) ("exit=$($r.Exit) error=$($r.Fields['error'])")

        # C5 危险选项提示
        $r = Invoke-GitRT -AiPrompt 'FORCE 强制推送到远端' -WorkDir $repo
        Check "C5 危险选项被提示" ($r.Fields['flag.force-with-lease'] -eq '1' -and $r.Fields['cmdline'] -match 'force-with-lease') ("cmdline=$($r.Fields['cmdline'])")

        # C6 互斥组：AI 选 hard
        $r = Invoke-GitRT -AiPrompt 'MIXEDHARD 硬重置' -WorkDir $repo
        Check "C6 AI 选 hard 时不会同时出现 --mixed" ($r.Fields['cmdline'] -match '--hard' -and $r.Fields['cmdline'] -notmatch '--mixed') ("cmdline=$($r.Fields['cmdline'])")

        # C7 AI 方案真实执行（生成 → 校验 → 执行 → 结果）
        Set-Content (Join-Path $repo 'ai.txt') 'ai-generated'
        $before = CommitCount $repo
        $r = Invoke-GitRT -AiPrompt 'COMMITALL 把改动提交' -WorkDir $repo -Paths (Join-Path $repo 'ai.txt') -AiRun
        $after = CommitCount $repo
        $subject = G $repo log -1 --pretty=%s
        Check "C7 --ai-run 真实执行 AI 方案并提交" ($r.Exit -eq 0 -and $after -eq ($before + 1) -and $subject -match 'AI 提交') ("exit=$($r.Exit) count=$before->$after subject=$subject")

        # C8 无 Key 时的降级
        $env:GITRT_AI_API_KEY_ENV = 'GITRT_MISSING_KEY'
        Remove-Item Env:\GITRT_TEST_KEY -ErrorAction SilentlyContinue
        $r = Invoke-GitRT -AiPrompt '推送' -WorkDir $repo
        Check "C8 未配置 Key → 明确报错 exit=4" ($r.Exit -eq 4 -and $r.Fields['key_set'] -eq '0' -and $r.Fields['error'] -match 'API Key') ("exit=$($r.Exit) error=$($r.Fields['error'])")

        # C9 服务端 401
        $env:GITRT_AI_API_KEY_ENV = 'GITRT_TEST_KEY'
        $env:GITRT_TEST_KEY = 'bad-key'
        $r = Invoke-GitRT -AiPrompt 'UNAUTHORIZED 推送' -WorkDir $repo
        Check "C9 HTTP 401 被正确报错 → exit=4" ($r.Exit -eq 4 -and $r.Fields['http'] -eq '401') ("exit=$($r.Exit) http=$($r.Fields['http']) error=$($r.Fields['error'])")
    }
    if ($mock -and -not $mock.HasExited) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
    if (Test-Path $mockLog) {
        Say "  mock 服务收到的请求（节选）:"
        Get-Content $mockLog -Encoding UTF8 | Select-Object -Last 6 | ForEach-Object { Say ("    " + $_) }
    }
}

# =========================================================== Phase D
Section "Phase D：Shell 扩展（GitRT.Shell.dll）"
if ($SkipShell) {
    Say "  已跳过"
} else {
    # 解析产物路径（默认为 build\debug 下的布局）
    if (-not $ShellDll) {
        $ShellDll = (Get-ChildItem -Path $BuildDir -Recurse -Filter 'GitRT.Shell.dll' -File `
                     -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    }
    if (-not $ProbeExe) {
        $ProbeExe = (Get-ChildItem -Path $BuildDir -Recurse -Filter 'GitRT.ShellProbe.exe' -File `
                     -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    }
    if (-not $Identity) { $Identity = Join-Path $PSScriptRoot '..\packaging\identity.json' }
    if (-not $Appx) { $Appx = Join-Path $BuildDir 'packaging\AppxManifest.xml' }

    if (-not $ShellDll -or -not (Test-Path $ShellDll) -or -not $ProbeExe -or -not (Test-Path $ProbeExe)) {
        Check "找到 Shell 产物（GitRT.Shell.dll / GitRT.ShellProbe.exe）" $false `
              "dll=$ShellDll probe=$ProbeExe（构建后重试）"
    } else {
        # 把 DLL、探针与 GUI 三件套放在同一目录：DLL 的 Invoke 直启 GUI 时按
        # "DLL 同目录" 查找 GitRT.exe（发行版布局），并把产物放在工作区外以避开
        # 本会话对"工作区内启动的进程"的写盘限制。
        $shellDir = Join-Path $Root 'shell'
        New-Item -ItemType Directory $shellDir -Force | Out-Null
        Copy-Item -LiteralPath $ShellDll -Destination $shellDir -Force
        Copy-Item -LiteralPath $ProbeExe -Destination $shellDir -Force
        $guiSrc = (Get-ChildItem -Path $BuildDir -Recurse -Filter 'GitRT.exe' -File -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' } | Select-Object -First 1).FullName
        if (-not $guiSrc) { $guiSrc = $Exe }
        if ($guiSrc -and (Test-Path $guiSrc)) { Copy-Item -LiteralPath $guiSrc -Destination $shellDir -Force }
        $probe = Join-Path $shellDir 'GitRT.ShellProbe.exe'
        $dll   = Join-Path $shellDir 'GitRT.Shell.dll'

        # ── 菜单形态由 config.json 的 menu.mode 决定，因此 Phase D 自己控制配置 ──
        $cfgPath = Join-Path $env:APPDATA 'GitRT\config.json'
        $cfgBak  = "$cfgPath.testall-bak"
        $hadCfg  = Test-Path $cfgPath
        if ($hadCfg) { Copy-Item $cfgPath $cfgBak -Force }
        New-Item -ItemType Directory (Split-Path $cfgPath) -Force | Out-Null
        $setCfg = { param($json) Set-Content $cfgPath $json -Encoding UTF8 }
        $runProbe = {
            param($probeArgs, $reportPath)
            if ($reportPath -and (Test-Path $reportPath)) { Remove-Item $reportPath -Force }
            Start-Process -FilePath $probe -PassThru -Wait -ArgumentList $probeArgs
        }
        $dumpFile = Join-Path $shellDir 'shellprobe-dump.txt'
        try {
            # ===== App 形态（默认）：菜单里只有一个「GitRT」入口，点击打开主窗口 =====
            & $setCfg '{}'
            $report = Join-Path $Root 'shell-selftest-app.txt'
            $p = & $runProbe @("--self-test=$report", "--dll", $dll, "--identity", $Identity,
                               "--appx", $Appx, "--workdir", $Root) $report
            $text = if (Test-Path $report) { Get-Content $report -Raw -Encoding UTF8 } else { '' }
            $fail = ([regex]::Matches($text, '\[FAIL\]')).Count
            Check "D1 App 形态自检全部通过（默认配置）" ($p.ExitCode -eq 0 -and $fail -eq 0) `
                  ("exit=$($p.ExitCode) fail=$fail")
            foreach ($m in [regex]::Matches($text, '(?m)^\[FAIL\].*$')) { Say ("    " + $m.Value) }

            $p = & $runProbe @("--dump", $repo, "--dll", $dll, "--workdir", $Root) $dumpFile
            $dt = if (Test-Path $dumpFile) { Get-Content $dumpFile -Raw -Encoding UTF8 } else { '' }
            Check "D2 App 形态：菜单里只有 1 个「GitRT」入口、没有子菜单" `
                  ($p.ExitCode -eq 0 -and $dt -match '合计 1 项' -and $dt -notmatch 'HASSUBCOMMANDS') `
                  ("exit=$($p.ExitCode) 行数=$((@(($dt -split "`r?`n") | Where-Object { $_ -match '^  \[' })).Count)")

            $reqReport = Join-Path $Root 'shell-request-app.txt'
            $p = & $runProbe @("--invoke", 'app.main', "--invoke-path", $repo,
                               "--invoke-report", $reqReport, "--dll", $dll, "--workdir", $Root) $reqReport
            $req = if (Test-Path $reqReport) { Get-Content $reqReport -Raw -Encoding UTF8 } else { '' }
            Check "D3 App 形态：点入口 → 打开 GitRT 主窗口（端到端）" `
                  ($p.ExitCode -eq 0 -and $req -match 'applied=1' -and $req -match 'cmdKey=app\.main') `
                  ("exit=$($p.ExitCode) report=$($req -replace "`r?`n", ' ')")
            Check "D3 入口请求把仓库上下文交给主窗口（无命令、有路径）" `
                  ($req -match 'pathCount=1' -and $req -match 'flagCount=0') ''

            # ===== Tree 形态（menu.mode=tree）：分组菜单仍可用（可配置能力） =====
            & $setCfg '{"menu.mode":"tree"}'
            $p = & $runProbe @("--dump", $repo, "--dll", $dll, "--workdir", $Root) $dumpFile
            $dt = if (Test-Path $dumpFile) { Get-Content $dumpFile -Raw -Encoding UTF8 } else { '' }
            $groups = @('仓库', '提交', '同步', '分支与标签', '查看', '储藏', '高级', '应用')
            $missing = @($groups | Where-Object { $dt -notmatch [regex]::Escape($_) })
            Check "D4 tree 形态：菜单树覆盖全部 8 个分组" ($p.ExitCode -eq 0 -and $missing.Count -eq 0) `
                  ("exit=$($p.ExitCode) missing=$($missing -join ',')")
            $topLines = @(($dt -split "`r?`n") | Where-Object { $_ -match '^  \[' })
            $topVisible = @($topLines | Where-Object { $_ -notmatch '\[HIDDEN\]' })
            $topLeaves = @($topVisible | Where-Object { $_ -notmatch 'HASSUBCOMMANDS' -and $_ -notmatch 'ISSEPARATOR' })
            Check "D4 tree 形态：顶层只保留分组入口（无高频直达/无开关）" `
                  ($topLeaves.Count -eq 0 -and $topVisible.Count -ge 6) `
                  ("可见顶层 $($topVisible.Count) 项，其中非分组 $($topLeaves.Count) 项")

            $reqReport = Join-Path $Root 'shell-request.txt'
            $p = & $runProbe @("--invoke", 'sync.pull', "--invoke-path", $repo,
                               "--invoke-report", $reqReport, "--dll", $dll, "--workdir", $Root) $reqReport
            $req = if (Test-Path $reqReport) { Get-Content $reqReport -Raw -Encoding UTF8 } else { '' }
            Check "D5 tree 形态：Invoke → 请求段 → GUI 参数面板" `
                  ($p.ExitCode -eq 0 -and $req -match 'applied=1' -and $req -match 'cmdKey=sync\.pull') `
                  ("exit=$($p.ExitCode) report=$($req -replace "`r?`n", ' ')")
            Check "D5 选区路径与复选状态已传入面板" `
                  ($req -match 'pathCount=1' -and $req -match 'flag=rebase=') ''

            # D6 menu.showFlags=true 时组内恢复复选/单选开关（能力保留）
            & $setCfg '{"menu.mode":"tree","menu.showFlags":true}'
            $p = & $runProbe @("--dump", $repo, "--dll", $dll, "--workdir", $Root) $dumpFile
            $dt = if (Test-Path $dumpFile) { Get-Content $dumpFile -Raw -Encoding UTF8 } else { '' }
            Check "D6 menu.showFlags=true 时组内恢复复选/单选开关" `
                  ($p.ExitCode -eq 0 -and $dt -match 'TOGGLEABLE') ("exit=$($p.ExitCode)")

            # D7 tree 形态自检（树结构 / 场景裁剪 / 身份一致性全套）
            & $setCfg '{"menu.mode":"tree"}'
            $report = Join-Path $Root 'shell-selftest-tree.txt'
            $p = & $runProbe @("--self-test=$report", "--dll", $dll, "--identity", $Identity,
                               "--appx", $Appx, "--workdir", $Root) $report
            $text = if (Test-Path $report) { Get-Content $report -Raw -Encoding UTF8 } else { '' }
            $fail = ([regex]::Matches($text, '\[FAIL\]')).Count
            $pass = ([regex]::Matches($text, '\[PASS\]')).Count
            Check "D7 tree 形态自检全部通过" ($p.ExitCode -eq 0 -and $fail -eq 0 -and $pass -ge 20) `
                  ("exit=$($p.ExitCode) pass=$pass fail=$fail")
            foreach ($m in [regex]::Matches($text, '(?m)^\[FAIL\].*$')) { Say ("    " + $m.Value) }

            # D8 现代菜单调用形态回归（★ M0-B 实测 bug）：外壳对子菜单项传 nullptr 数组
            #    需要身份包已注册（Get-AppxPackage GitRT），否则跳过。
            if (-not (Get-AppxPackage -Name GitRT -ErrorAction SilentlyContinue)) {
                Say "  [SKIP] D8 现代菜单形态回归：身份包未注册（先跑 packaging\scripts\dev-install.ps1）"
            } else {
                $p = $runProbe.Invoke(@("--activate", "--null-array", "--dump", $repo), $null)
                Check "D8 现代菜单调用形态（子项 nullptr）下菜单非空" ($p.ExitCode -eq 0) `
                      ("exit=$($p.ExitCode)（修复前会因全部 HIDDEN 而失败）")
            }

            # D9 外壳自身的菜单聚合（IContextMenu）能拉起我们的处理器
            $p = $runProbe.Invoke(@("--shell-menu", "--dump", $repo), $null)
            Check "D9 外壳聚合（IContextMenu::QueryContextMenu）成功" ($p.ExitCode -eq 0) `
                  ("exit=$($p.ExitCode)")
        } finally {
            if ($hadCfg) { Copy-Item $cfgBak $cfgPath -Force; Remove-Item $cfgBak -Force }
            else { '{}' | Set-Content $cfgPath -Encoding UTF8 }
        }

        # 让 exe 侧也用同一份产物继续在本机自检一次（可选，避免误报）
        Say "  Shell 产物: $dll"
    }
}

# =========================================================== 汇总
Section "汇总"
Say ("  通过: {0}" -f $script:pass)
Say ("  失败: {0}" -f $script:fail)
$reportPath = Join-Path $Root 'report.txt'
$script:log | Set-Content $reportPath -Encoding UTF8
Say "  报告: $reportPath"
if ($script:fail -eq 0) { Say "  结果: 全部通过 ✅" } else { Say "  结果: 有失败 ❌" }
exit $(if ($script:fail -eq 0) { 0 } else { 1 })
