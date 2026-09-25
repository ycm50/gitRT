# ---------------------------------------------------------------------------
# 克隆选项端到端：--depth / --single-branch / --branch / --filter
#
#   为什么用 file:// 而不是本地路径：git 对**本地目录**克隆会忽略 --depth
#   （"ignoring --depth"），必须走 file:// 协议才真的浅。
#
# 用法: pwsh -File tools\test-clone.ps1 [-Exe <GitRT.exe>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = "$PSScriptRoot\..\build\debug\src\gui\GitRT.exe",
    [string]$LabRoot = "$PSScriptRoot\..\build\clone-lab"
)
$ErrorActionPreference = 'Continue'
$pass = 0; $fail = 0
function Check($what, $ok, $detail = '') {
    if ($ok) { $script:pass++; Write-Host "  [PASS] $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  [FAIL] $what  $detail" -ForegroundColor Red }
}
function Git($dir, [string[]]$a) { $o = & git.exe -C $dir @a 2>&1; return ($o | Out-String).Trim() }

# 注意：试验场必须在**工作区内**——%TEMP% 里 git 会被文件沙箱挡住写不进去（实测）
$LabRoot = [System.IO.Path]::GetFullPath($LabRoot)
$src = Join-Path $LabRoot 'src'
$work = Join-Path $LabRoot 'work'
if (Test-Path $LabRoot) { Remove-Item $LabRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $src, $work | Out-Null
Git $src @('init','-q','-b','main') | Out-Null
Git $src @('config','user.name','GitRT Test') | Out-Null
Git $src @('config','user.email','t@e.invalid') | Out-Null
Git $src @('config','commit.gpgsign','false') | Out-Null
1..3 | ForEach-Object { Set-Content "$src\f$_.txt" "c$_" -Encoding UTF8; Git $src @('add','-A') | Out-Null; Git $src @('commit','-q','-m',"c$_") | Out-Null }
$srcCount = [int](Git $src @('rev-list','--count','HEAD'))
Write-Host "== 克隆选项端到端 ==" -ForegroundColor Cyan
Write-Host "源仓库: $src（$srcCount 个提交）"
$top = (Git $src @('rev-parse','--show-toplevel')) -replace '\\','/'
Check "源仓库就绪" ($top -eq ($src -replace '\\','/')) "top=$top"

$uri = 'file:///' + ($src -replace '\\','/')

function RunCloneAt([string]$cwdDir, [string[]]$flags, [string]$tag) {
    $outFile = Join-Path $cwdDir "$tag.txt"
    $cliArgs = @('--run','repo.clone','--param',"url=$uri",'--cwd',$cwdDir,'--out',$outFile)
    foreach ($f in $flags) { $cliArgs += @('--flag',$f) }
    $argLine = ($cliArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru -NoNewWindow
    $p.WaitForExit(60000) | Out-Null
    $txt = ''
    for ($w = 0; $w -lt 60; $w++) {
        if (Test-Path $outFile) {
            $txt = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
            if ($txt -match '(?m)^exit=\d+') { break }
        }
        Start-Sleep -Milliseconds 200
    }
    return [pscustomobject]@{ exit = $p.ExitCode; text = $txt }
}
function RunClone([string[]]$flags, [string]$tag) {
    $cliArgs = @('--run','repo.clone','--param',"url=$uri",'--cwd',$work,'--out',"$work\$tag.txt")
    foreach ($f in $flags) { $cliArgs += @('--flag',$f) }
    $argLine = ($cliArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ } }) -join ' '
    # ★ GUI 子系统进程：Start-Process -Wait 不可靠（会早返回），改成显式 WaitForExit，
    #   再等报告文件写出结尾的 "exit=" 行 —— 报告是边跑边写的
    $outFile = "$work\$tag.txt"
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru -NoNewWindow
    $p.WaitForExit(60000) | Out-Null
    $txt = ''
    for ($w = 0; $w -lt 60; $w++) {
        if (Test-Path $outFile) {
            $txt = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
            if ($txt -match '(?m)^exit=\d+') { break }
        }
        Start-Sleep -Milliseconds 200
    }
    return [pscustomobject]@{ exit = $p.ExitCode; text = $txt }
}

# ---------------------------------------------------------------- ① 浅克隆
$r = RunClone @('depth=1','single-branch=1') 'shallow'
$dest = Join-Path $work 'src'
Check "① 浅克隆执行成功（exit=0）" ($r.exit -eq 0) "text=$($r.text)"
Check "① 命令行里有 --depth=1" ($r.text -match '--depth=1') "text=$($r.text)"
if (Test-Path $dest) {
    Check "① .git\shallow 存在（确实是浅克隆）" (Test-Path (Join-Path $dest '.git\shallow')) ''
    Check "① 只有 1 个提交（深度生效）" ((Git $dest @('rev-list','--count','HEAD')) -eq '1') "count=$(Git $dest @('rev-list','--count','HEAD'))"
    Check "① 工作区文件都在" ((Get-ChildItem $dest -Filter 'f*.txt').Count -eq 3) ''
} else {
    Check "① 克隆目录生成" $false "找不到 $dest"
}

# ------------------------------------------------------------ ② 非法深度
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
$r2 = RunClone @('depth=abc') 'baddepth'
Check "② 非法深度被拒绝（exit≠0）" ($r2.exit -ne 0) "exit=$($r2.exit)"
Check "② 报错说清是"正整数"" ($r2.text -match '正整数') "text=$($r2.text)"
Check "② 没有产生克隆目录" (-not (Test-Path $dest)) '竟然克隆出来了'

# ---------------------------------------------------------------- ③ 完整克隆
$r3 = RunClone @() 'full'
Check "③ 完整克隆成功" ($r3.exit -eq 0) "text=$($r3.text)"
if (Test-Path $dest) {
    Check "③ 提交数与源一致（$srcCount）" ((Git $dest @('rev-list','--count','HEAD')) -eq "$srcCount") "count=$(Git $dest @('rev-list','--count','HEAD'))"
    Check "③ 不是浅克隆（无 .git\shallow）" (-not (Test-Path (Join-Path $dest '.git\shallow'))) ''
}

# ------------------------------------------ ④ 仓库内子目录（cwd 回归）
Write-Host ""
Write-Host "-- ④ 在"仓库内的子目录"里克隆（必须落在该子目录，不能落到外层仓库根）--" -ForegroundColor Cyan
$inner = Join-Path ([System.IO.Path]::GetFullPath("$PSScriptRoot\..")) 'build\clone-lab\inner'
Remove-Item $inner -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $inner | Out-Null
# 这个 inner 位于 GitRT 主仓库内 → 旧实现会把 cwd 退化成主仓库根，克隆就会撞上 main 仓库的 src/
$r4 = RunCloneAt $inner @() 'inner'
$dest4 = Join-Path $inner 'src'
Check "④ 克隆落在选中的子目录里" (Test-Path $dest4) "期望 $dest4"
Check "④ 没有落到外层仓库根（不污染主仓库）" (-not (Test-Path (Join-Path ([System.IO.Path]::GetFullPath("$PSScriptRoot\..")) 'src\.git'))) '主仓库 src 下出现了 .git'
if (Test-Path $dest4) {
    $tl = (& git.exe -C $dest4 rev-parse --show-toplevel 2>&1 | Out-String).Trim() -replace '\\','/'
    Check "④ 克隆的仓库根就是它自己" ($tl -eq (($dest4 -replace '\\','/'))) "toplevel=$tl"
}
Remove-Item $inner -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "== 结果: $pass 通过 / $fail 失败 ==" -ForegroundColor Cyan
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
