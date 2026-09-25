# ---------------------------------------------------------------------------
# 模拟 OpenAI 兼容服务（用于在没有真实 API Key 的情况下测试 GitRT 的 AI 链路）
#
#   · 只监听 127.0.0.1，不对外
#   · 依据请求体里的关键词决定"模型输出"，从而覆盖各种分支
#   · 所有响应都带同级 reasoning_content，用来验证客户端不会取错字段
#   · 没有 Authorization 头时返回 401（验证错误处理）
#
# 用法: pwsh -File tools\mock-ai-server.ps1 -Port 18080 -LogFile <path>
# ---------------------------------------------------------------------------
param(
    [string]$LastBodyFile = "",
    [int]$Port = 18080,
    [string]$LogFile = "$env:TEMP\GitRT-test\mock-ai.log"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory (Split-Path $LogFile) -Force | Out-Null
"$(Get-Date -Format o) mock-ai starting on 127.0.0.1:$Port" | Set-Content $LogFile -Encoding UTF8

function Write-Log([string]$s) {
    "$(Get-Date -Format o) $s" | Add-Content $LogFile -Encoding UTF8
}

function Get-MockContent([string]$body) {
    # 只匹配 user 消息，且区分大小写：
    #   系统提示词里含小写 "command":"none"，用 -match（不区分大小写）会误命中，导致
    #   所有请求都返回 none —— 这个坑在第一次跑测试时真实踩到过。
    $user = ''
    $m = [regex]::Match($body, '"role"\s*:\s*"user"\s*,\s*"content"\s*:\s*"((?:[^"\\]|\\.)*)"')
    if ($m.Success) { $user = $m.Groups[1].Value }
    if ($user -cmatch 'NOTJSON')   { return 'you should just run git push' }
    if ($user -cmatch 'UNKNOWN')   { return '{"command":"hack.evil","params":{},"flags":{},"explanation":"模型编造的命令"}' }
    if ($user -cmatch 'NONE')      { return '{"command":"none","explanation":"现有命令无法完成该意图"}' }
    if ($user -cmatch 'COMMITALL') { return '{"command":"commit.commit","params":{"msg":"AI 提交：由 mock 模型生成"},"flags":{},"explanation":"暂存并提交全部改动"}' }
    if ($user -cmatch 'FORCE')     { return '{"command":"sync.push","params":{},"flags":{"force-with-lease":"1"},"explanation":"强制推送（安全版）"}' }
    if ($user -cmatch 'MIXEDHARD') { return '{"command":"adv.reset","params":{"rev":"HEAD~1"},"flags":{"hard":"1"},"explanation":"硬重置一个提交"}' }
    return '{"command":"sync.push","params":{},"flags":{"set-upstream":"1"},"explanation":"推送当前分支并设置上游"}'
}

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:$Port/")
$listener.Start()
Write-Log "listening"
try {
    while ($listener.IsListening) {
        $ctx = $listener.GetContext()
        $req = $ctx.Request
        $path = $req.Url.AbsolutePath
        $reader = New-Object System.IO.StreamReader($req.InputStream, [Text.Encoding]::UTF8)
        $body = $reader.ReadToEnd()
    if ($LastBodyFile) { Set-Content -Path $LastBodyFile -Value $body -Encoding UTF8 }   # 测试要断言"系统提示词/白名单"送了什么
        $reader.Close()

        if ($path -eq '/health') {
            $bytes = [Text.Encoding]::UTF8.GetBytes('ok')
            $ctx.Response.StatusCode = 200
            $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            $ctx.Response.Close()
            continue
        }

        $auth = $req.Headers['Authorization']
        $promptHit = ''
        $mu = [regex]::Match($body, '"role"\s*:\s*"user"\s*,\s*"content"\s*:\s*"((?:[^"\\]|\\.)*)"')
        if ($mu.Success) {
            $promptHit = ([regex]::Match($mu.Groups[1].Value, '(NOTJSON|UNKNOWN|NONE|COMMITALL|FORCE|MIXEDHARD|UNAUTHORIZED)')).Value
        }
        Write-Log ("$($req.HttpMethod) $path bodyLen=$($body.Length) auth=$([bool]$auth) promptHit=$promptHit")

        # 模型列表（AI 设置界面「获取列表」用）：GET /v1/models 或 /models
        if ($path -match '/models$') {
            if (-not $auth) {
                $bytes = [Text.Encoding]::UTF8.GetBytes('{"error":{"message":"missing api key"}}')
                $ctx.Response.StatusCode = 401
                $ctx.Response.ContentType = 'application/json'
                $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
                $ctx.Response.Close()
                continue
            }
            $payload = '{"object":"list","data":[{"id":"mock-model","object":"model","owned_by":"gitrt"},' +
                       '{"id":"mock-model-pro","object":"model","owned_by":"gitrt"},' +
                       '{"id":"mock-model-mini","object":"model","owned_by":"gitrt"}]}'
            $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
            $ctx.Response.StatusCode = 200
            $ctx.Response.ContentType = 'application/json'
            $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            $ctx.Response.Close()
            continue
        }

        if (-not $auth) {
            $payload = '{"error":{"message":"missing api key","type":"invalid_request_error","code":"401"}}'
            $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
            $ctx.Response.StatusCode = 401
            $ctx.Response.ContentType = 'application/json'
            $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            $ctx.Response.Close()
            continue
        }
        if ($body -match 'UNAUTHORIZED') {
            $payload = '{"error":{"message":"invalid api key","type":"authentication_error"}}'
            $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
            $ctx.Response.StatusCode = 401
            $ctx.Response.ContentType = 'application/json'
            $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            $ctx.Response.Close()
            continue
        }

        $content = Get-MockContent $body
        $envelope = [ordered]@{
            id      = 'mock-1'
            object  = 'chat.completion'
            model   = 'mock-model'
            choices = @(
                [ordered]@{
                    index         = 0
                    finish_reason = 'stop'
                    message       = [ordered]@{
                        role               = 'assistant'
                        # 故意放在 content 之前：客户端若按"第一个 content"取就会取错
                        reasoning_content  = 'MOCK-REASONING-SHOULD-NOT-BE-USED'
                        content            = $content
                    }
                }
            )
        }
        $payload = $envelope | ConvertTo-Json -Depth 8 -Compress
        $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
        $ctx.Response.StatusCode = 200
        $ctx.Response.ContentType = 'application/json; charset=utf-8'
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
        $ctx.Response.Close()
    }
} finally {
    Write-Log "stopping"
    $listener.Stop()
    $listener.Close()
}
