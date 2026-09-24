# Set-NewMenuItems.ps1 —— 精简「右键 → 新建」子菜单（HKCU 免管理员，可完整还原）
#
# 原理：Windows 的「新建」项来自各文件类型在**扩展名键下**的 ShellNew 子键，常见两种位置：
#   HKCU\Software\Classes\.docx\ShellNew                    （扩展名键直接挂）
#   HKCU\Software\Classes\.docx\WPS.Docx.6\ShellNew         （扩展名键 → ProgID → ShellNew，WPS/Office 常用）
# 本脚本把匹配到的 ShellNew 键**重命名为 ShellNew.disabled**（键内数据原样保留，随时可还原），
# 并顺带从 Explorer 的新建缓存列表里去掉对应扩展名。
#
# 用法：
#   pwsh -File tools\Set-NewMenuItems.ps1 -List                 # 只列出探测到的 ShellNew 与当前状态
#   pwsh -File tools\Set-NewMenuItems.ps1                        # 默认禁用办公文档 8 项
#   pwsh -File tools\Set-NewMenuItems.ps1 -Remove .zip,.rtf      # 自定义
#   pwsh -File tools\Set-NewMenuItems.ps1 -AllUsers              # 同时处理 HKLM（需管理员）
#   pwsh -File tools\Set-NewMenuItems.ps1 -Restore               # 全部还原

[CmdletBinding()]
param(
    [string[]]$Remove = @('.doc', '.docx', '.xls', '.xlsx', '.ppt', '.pptx', '.rtf', '.txt'),
    [switch]$List,
    [switch]$Restore,
    [switch]$AllUsers
)

$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
. "$here\..\lib\Common.ps1"
. "$here\..\lib\Menu.ps1"

$cacheKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Discardable\PostSetup\ShellNew'
$backupDir = Get-StateDir

function Get-ExtensionRoots {
    param([string]$Ext)
    $roots = @("HKCU:\Software\Classes\$Ext")
    if ($AllUsers) { $roots += "HKLM:\Software\Classes\$Ext" }
    return $roots
}

function Get-ShellNewKeys {
    <#  返回某个扩展名下所有 ShellNew 键（含"扩展名键 → 子键 → ShellNew"这种嵌套形式） #>
    param([string]$Ext)
    $found = New-Object System.Collections.ArrayList
    foreach ($root in (Get-ExtensionRoots -Ext $Ext)) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $direct = Join-Path $root 'ShellNew'
        if (Test-Path -LiteralPath $direct) {
            [void]$found.Add([pscustomobject]@{ Path = $direct; State = 'active' })
        }
        $disabled = Join-Path $root 'ShellNew.disabled'
        if (Test-Path -LiteralPath $disabled) {
            [void]$found.Add([pscustomobject]@{ Path = $disabled; State = 'disabled' })
        }
        foreach ($child in (Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            if ($child.PSChildName -eq 'ShellNew') { continue }
            foreach ($leaf in @('ShellNew', 'ShellNew.disabled')) {
                $p = Join-Path $child.PSPath $leaf
                if (Test-Path -LiteralPath $p) {
                    [void]$found.Add([pscustomobject]@{ Path = $p; State = $(if ($leaf -eq 'ShellNew') { 'active' } else { 'disabled' }) })
                }
            }
        }
    }
    return $found.ToArray()
}

function Get-ShellNewValues {
    param([string]$KeyPath)
    $out = [ordered]@{}
    try {
        $item = Get-Item -LiteralPath $KeyPath -ErrorAction Stop
        foreach ($n in $item.Property) {
            if ($n -eq '(default)') { continue }
            $out[$n] = [string]$item.GetValue($n)
        }
    } catch { }
    return $out
}

function Get-CacheList {
    if (-not (Test-Path -LiteralPath $cacheKey)) { return @() }
    $v = Get-Item -LiteralPath $cacheKey
    if (-not ($v.Property -contains 'Classes')) { return @() }
    return @([string[]]$v.GetValue('Classes'))
}

function Set-CacheList {
    param([string[]]$Entries)
    if (-not (Test-Path -LiteralPath $cacheKey)) { return }
    Set-ItemProperty -Path $cacheKey -Name 'Classes' -Value ([string[]]$Entries) -Type MultiString
}

$exts = @($Remove | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })

# ---------- 还原 ----------
if ($Restore) {
    $restored = New-Object System.Collections.ArrayList
    foreach ($ext in $exts) {
        foreach ($k in (Get-ShellNewKeys -Ext $ext)) {
            if ($k.State -eq 'disabled') {
                $target = $k.Path -replace '\.disabled$', ''
                try {
                    Rename-Item -LiteralPath $k.Path -NewName (Split-Path -Leaf $target) -ErrorAction Stop
                    [void]$restored.Add($target)
                } catch {
                    Write-Host ('  还原失败: {0} → {1}' -f $k.Path, $_.Exception.Message)
                }
            }
        }
    }
    # 缓存列表也还原（若列表里已无这些扩展名，由 Explorer 自行重建）
    [void](Invoke-ShellRefresh)
    Write-Host ('已还原 {0} 个 ShellNew 键：' -f $restored.Count)
    foreach ($r in $restored) { Write-Host ('  {0}' -f $r) }
    if ($restored.Count -eq 0) { Write-Host '  （没有处于禁用状态的项）' }
    exit 0
}

# ---------- 探测 ----------
$plan = New-Object System.Collections.ArrayList
foreach ($ext in $exts) {
    foreach ($k in (Get-ShellNewKeys -Ext $ext)) {
        [void]$plan.Add([pscustomobject]@{ Ext = $ext; Path = $k.Path; State = $k.State; Values = (Get-ShellNewValues -KeyPath $k.Path) })
    }
}

$cacheNow = Get-CacheList
$cacheHas = @($exts | Where-Object { $cacheNow -contains $_ })

if ($List -or $plan.Count -eq 0) {
    Write-Host '== 目标扩展名 =='
    Write-Host ('  {0}' -f ($exts -join ', '))
    Write-Host '== 探测到的 ShellNew 键 =='
    if ($plan.Count -eq 0) { Write-Host '  （未找到）' }
    foreach ($p in $plan) {
        Write-Host ('  [{0,-8}] {1}  {2}' -f $p.State, $p.Path, (($p.Values.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join '; '))
    }
    Write-Host '== Explorer 新建缓存列表 =='
    Write-Host ('  {0}' -f ($cacheNow -join ' | '))
    if ($List) { exit 0 }
}

if ($plan.Count -eq 0 -and $cacheHas.Count -eq 0) {
    Write-Host '没有可处理的项。'
    exit 0
}

# ---------- 备份 ----------
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupPath = Join-Path $backupDir ("shellnew-backup-{0}.json" -f $stamp)
Write-JsonFile -Path $backupPath -Object ([ordered]@{
    backedUpAt    = (Get-Date).ToString('o')
    extensions    = $exts
    shellNewKeys  = @($plan | ForEach-Object { [ordered]@{ ext = $_.Ext; path = $_.Path; state = $_.State; values = $_.Values } })
    cacheClasses  = @($cacheNow)
})
Write-Host ('已备份 -> {0}' -f $backupPath)

# ---------- 禁用 ----------
$done = New-Object System.Collections.ArrayList
foreach ($p in $plan) {
    if ($p.State -eq 'disabled') { continue }
    try {
        Rename-Item -LiteralPath $p.Path -NewName 'ShellNew.disabled' -ErrorAction Stop
        [void]$done.Add($p.Path)
    } catch {
        Write-Host ('  禁用失败: {0} → {1}' -f $p.Path, $_.Exception.Message)
    }
}
Write-Host ('已禁用 {0} 个 ShellNew 键：' -f $done.Count)
foreach ($d in $done) { Write-Host ('  {0}' -f $d) }

# ---------- 顺带清理缓存列表（Explorer 会按真实注册重建，这里只是加速生效） ----------
if ($cacheHas.Count -gt 0) {
    $kept = @($cacheNow | Where-Object { $exts -notcontains $_.Trim().ToLowerInvariant() })
    Set-CacheList -Entries $kept
    Write-Host ('缓存列表已更新：{0}' -f ($kept -join ' | '))
}
[void](Invoke-ShellRefresh)

Write-Host ''
Write-Host '说明：'
Write-Host '  · 生效时机：Explorer 重建「新建」菜单缓存后（通常下次右键即可，最迟注销/重启资源管理器）。'
Write-Host '  · WPS/Office 更新或修复安装时可能重新写回这些键，届时重跑本脚本即可。'
Write-Host ('  · 还原：pwsh -File tools\Set-NewMenuItems.ps1 -Restore')
