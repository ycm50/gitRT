# ---------------------------------------------------------------------------
# 重建仓库开发知识库（docs/kb + 设计文档 → .kb/kb.sqlite）
#
#   docs/kb/00-索引.md 里承诺的入口就是本脚本：
#     pwsh -File tools/kb-build.ps1            # 重生成 06-模块地图 + 增量入库 + VACUUM
#     pwsh -File tools/kb-build.ps1 -Force     # 全部重新解析与嵌入（引擎升级/切块规则变了才用）
#     pwsh -File tools/kb-build.ps1 -DryRun    # 只重生成 06-模块地图.md，不动库
#
#   语料 = 根目录 *.md（README/CHANGELOG/CONTRIBUTING/SECURITY/THIRD-PARTY-NOTICES）
#        + docs/ 下全部 .md（递归，含 docs/kb 与 docs/research）
#        + packaging/scripts/使用说明.txt
#   产物 .kb/kb.sqlite 随仓库提交：clone 下来即可直接 kb_search，不需要重建。
#
# 用法: pwsh -File tools/kb-build.ps1 [-Force] [-DryRun] [-Python <exe>] [-KbRoot <dir>] [-Engine <py>]
# 退出码: 0 = 成功
# ---------------------------------------------------------------------------
param(
    [switch]$Force,
    [switch]$DryRun,
    [string]$Python = "",
    [string]$KbRoot = "",
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $KbRoot) { $KbRoot = Join-Path $repoRoot ".kb" }

function Say([string]$s) { Write-Host $s }

# ---------------------------------------------------------------- Python 与引擎
# 引擎随 dsh-kb-rag 插件分发；这里只**找**它，不复制一份进仓库（两份实现必然漂移）。
if (-not $Engine) {
    $Engine = Join-Path $env:USERPROFILE ".dsh\profiles\desktop\node_modules\dsh-kb-rag\kb_engine.py"
}
if (-not (Test-Path -LiteralPath $Engine)) {
    Say "找不到 kb-rag 引擎：$Engine"
    Say "用 -Engine <kb_engine.py> 指过去（它由 dsh-kb-rag 插件提供）。"
    exit 2
}
if (-not $Python) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $Python = $cmd.Source } else { $Python = "python" }
}
Say "engine : $Engine"
Say "python : $Python"
Say "kb_root: $KbRoot"

function Invoke-Engine {
    param([string]$Command, [hashtable]$Request)
    $json = $Request | ConvertTo-Json -Compress -Depth 8
    $raw = $json | & $Python $Engine $Command
    if ($LASTEXITCODE -ne 0 -and -not $raw) { throw "engine $Command 失败（exit=$LASTEXITCODE）" }
    return ($raw | ConvertFrom-Json)
}

# ---------------------------------------------------- ① 生成 docs/kb/06-模块地图.md
# 为什么自动生成：手写清单一定会烂（新增文件不会有人记得补），而"某个 .cpp 干什么"
# 恰恰是查库时最常问的。职责文本取自**文件头注释**，所以改注释等于改文档。
# 职责文本 = 文件头第一行"有实质内容"的注释。
# 跳过：空行、#pragma once、#include、纯分隔线（// ---- / // ====）。
# 遇到既不是注释也不是上面几类的行（例如真正的代码）就停 —— 只在文件头找，不扫全文。
function Get-Duty([string[]]$lines) {
    foreach ($ln in $lines) {
        $t = $ln.Trim()
        if ($t -eq "") { continue }
        if ($t.StartsWith("#pragma") -or $t.StartsWith("#include")) { continue }
        if (-not $t.StartsWith("//")) { return "" }
        $body = $t.Substring(2).Trim()
        if ($body -eq "") { continue }
        if ($body -match "^[-=*+_~#\s]+$") { continue }
        return $body
    }
    return ""
}

function New-ModuleMap {
    $srcRoot = Join-Path $repoRoot "src"
    $rows = New-Object System.Collections.Generic.List[string]
    $files = Get-ChildItem -LiteralPath $srcRoot -Recurse -File -Include *.cpp, *.h | Sort-Object FullName
    # 先扫一遍同名 .h 的职责：好几个 .cpp 是"#include 自己的头就开写"（没有文件头注释），
    # 它们的职责其实写在配对的头里 —— 不借用的话模块地图上会出现一堆"（无文件头注释）"，
    # 而"这个 .cpp 干什么"正是查库时最常问的。借用时明确标注来源（见 xxx.h）。
    $headerDuty = @{}
    foreach ($f in $files) {
        if ($f.Extension -ne ".h") { continue }
        $d = Get-Duty (Get-Content -LiteralPath $f.FullName)
        if ($d -ne "") { $headerDuty[$f.FullName] = $d }
    }
    $curDir = "";
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($repoRoot.Length + 1).Replace([char]92, [char]47)
        $dir = $rel.Substring(0, $rel.LastIndexOf("/"))
        if ($dir -ne $curDir) {
            $curDir = $dir
            $rows.Add("")
            $rows.Add("### " + $dir)
            $rows.Add("")
            $rows.Add("| 文件 | 行数 | 职责（取自文件头注释） |")
            $rows.Add("| --- | --- | --- |")
        }
        $lines = Get-Content -LiteralPath $f.FullName
        $duty = Get-Duty $lines
        if ($duty -eq "" -and $f.Extension -eq ".cpp") {
            $hdr = [System.IO.Path]::ChangeExtension($f.FullName, ".h")
            if ($headerDuty.ContainsKey($hdr)) {
                $duty = "见 " + [System.IO.Path]::GetFileName($hdr) + "：" + $headerDuty[$hdr]
            }
        }
        if ($duty -eq "") { $duty = "（无文件头注释）" }
        if ($duty.Length -gt 110) { $duty = $duty.Substring(0, 110) + "…" }
        $duty = $duty.Replace("|", "\|")
        $name = $rel.Substring($dir.Length + 1)
        $rows.Add("| " + $name + " | " + $lines.Count + " | " + $duty + " |")
    }

    $doc = New-Object System.Collections.Generic.List[string]
    $doc.Add("# 模块地图")
    $doc.Add("")
    $doc.Add('> **本文件由 tools/kb-build.ps1 自动生成，不要手改**（改源码里的文件头注释，再跑一次脚本）。')
    $doc.Add('> 职责文本就是每个文件开头的第一行有意义的注释 —— 所以"注释写清楚"等于"查得到"。')
    $doc.Add("")
    $doc.Add('行数是**物理行**（含注释与空行）：用来看规模与"这个文件是不是已经太大"，不是复杂度指标。')
    $doc.Add("")
    $doc.Add("## 目录")
    $doc.Add("")
    $doc.Add("| 目录 | 产物 / 角色 |")
    $doc.Add("| --- | --- |")
    $doc.Add("| src/core | 静态库 gitrt_core + gitrt_core_tiny（命令表、argv 构造、git 进程、状态解析、业务 core） |")
    $doc.Add("| src/gui | GitRT.exe：主窗口 / 参数面板 / 各专用窗口 / **全部 CLI 门面** |")
    $doc.Add("| src/shell | GitRT.Shell.dll：IExplorerCommand 菜单扩展（只链 tiny） |")
    $doc.Add("| src/shellprobe | GitRT.ShellProbe.exe：不依赖资源管理器的菜单 / COM 验证工具 |")
    $doc.Add("| src/tests | GitRT.CoreTests.exe：无窗口单元测试（CTest 用例 core_selftest） |")
    foreach ($r in $rows) { $doc.Add($r) }
    $doc.Add("")
    $out = Join-Path $repoRoot "docs\kb\06-模块地图.md"
    $text = [string]::Join([char]10, $doc) + [char]10
    [System.IO.File]::WriteAllText($out, $text, (New-Object System.Text.UTF8Encoding($false)))
    Say "已生成 $out（$($files.Count) 个源文件）"
}

New-ModuleMap
if ($DryRun) { Say "-DryRun：只生成了模块地图，未动库"; exit 0 }

# ------------------------------------------------------------ ② 收集语料
$paths = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $repoRoot -File -Filter *.md | Sort-Object Name | ForEach-Object { $paths.Add($_.FullName) }
$paths.Add((Join-Path $repoRoot "docs"))
$usage = Join-Path $repoRoot "packaging\scripts\使用说明.txt"
if (Test-Path -LiteralPath $usage) { $paths.Add($usage) }
Say "语料根：$($paths.Count) 项（根 *.md + docs/ 递归 + 使用说明.txt）"

# ------------------------------------------------------------------ ③ 入库
# 增量：引擎按 sha256 跳过未变文件，重复跑是安全的；-Force 才全部重解析重嵌入。
$req = @{ kb_root = $KbRoot; paths = @($paths) }
if ($Force) { $req["force"] = $true }
$t0 = Get-Date
$resp = Invoke-Engine -Command "ingest" -Request $req
if (-not $resp.ok) { Say ("入库失败：" + $resp.error); exit 1 }
$tt = $resp.totals
$secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
Say ("入库完成：" + $secs + " s  新增 " + $tt.added + " / 更新 " + $tt.updated + " / 跳过 " + $tt.skipped + " / 删除 " + $tt.removed + " / 元数据 " + $tt.meta_changed)
if ($resp.embedding_error) { Say ("[!] 向量链路不可用：" + $resp.embedding_error + "（检索会降级成关键词）") }
Say ("indexed_with=" + $resp.indexed_with + "  embedding=" + $resp.embedding + "  vectors_missing=" + $resp.vectors_missing)

# ----------------------------------------------------------------- ④ VACUUM
# 入库会留下大量空闲页；库是**要提交进 git** 的，体积直接进历史，所以每次压实。
$db = Join-Path $KbRoot "kb.sqlite"
$vac = 'import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.execute("VACUUM"); c.close()'
& $Python -c $vac $db
if ($LASTEXITCODE -ne 0) { Say "VACUUM 失败（库仍可用，只是体积偏大）" }

# ------------------------------------------------------------------ ⑤ 汇报
$resp2 = Invoke-Engine -Command "stats" -Request @{ kb_root = $KbRoot }
Say ("库：" + $resp2.db)
Say ("篇数 " + $resp2.docs + " / 块 " + $resp2.chunks + " / 向量 " + $resp2.vectors + " / 字符 " + $resp2.chars + "  health=" + $resp2.health.ok + "  stale=" + $resp2.stale_docs)
$size = (Get-Item -LiteralPath $db).Length
Say ("体积：" + [math]::Round($size / 1MB, 2) + " MB  → 记得把 .kb/kb.sqlite 一起提交")
if (-not $resp2.health.ok) { Say "[!] health 不为 ok，检索可能漏；用 -Force 重灌"; exit 1 }
exit 0
