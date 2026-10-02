# ---------------------------------------------------------------------------
# 假 gh（测试桩）：让「gh 在场」的代码路径可以被真正执行
#
#   为什么需要它：tools/test-tag-release.ps1 在没装 gh 时只能 [SKIP]，于是
#   「发布列表非空 / 创建发布」这条流程**在任何环境（含 CI）里都没被跑过**。
#   本桩返回与真 gh 相同形状的 JSON，配合 GITRT_GH_EXE 注入。
#
#   入口：tools/fake-gh.bat（CreateProcessW 不能执行 .ps1，所以套一层 .bat）。
#
#   ★ 只认下面这几个子命令，其它一律非零退出 —— 这样“桩没覆盖到”会显式失败，
#     而不是悄悄返回空列表让测试假绿。
# ---------------------------------------------------------------------------
$all = ($args -join ' ')

if ($all -match '^--version') {
    Write-Output 'gh version 2.63.2 (2024-11-20)  [fake]'
    exit 0
}

if ($all -match '^release list') {
    # 形状与真 gh 的 `release list --json tagName,name,publishedAt,isDraft,isPrerelease` 一致
    $json = '[{"tagName":"v1.2.0","name":"Release 1.2.0","publishedAt":"2026-01-15T10:00:00Z","isDraft":false,"isPrerelease":false},' +
            '{"tagName":"v1.1.0","name":"Release 1.1.0","publishedAt":"2025-12-01T09:30:00Z","isDraft":false,"isPrerelease":false},' +
            '{"tagName":"v1.0.0-rc1","name":"RC1","publishedAt":"2025-11-20T08:00:00Z","isDraft":false,"isPrerelease":true}]'
    Write-Output $json
    exit 0
}

if ($all -match '^release create') {
    Write-Output 'https://example.invalid/releases/tag/fake'
    exit 0
}

Write-Error "fake-gh: 未覆盖的子命令: [$all]"
exit 1
