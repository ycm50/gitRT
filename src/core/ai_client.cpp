#include "ai_client.h"

#include <winhttp.h>

#include <cctype>
#include <cstdlib>

#include "json_util.h"
#include "config.h"

namespace grt {

// ================================================================== 配置
// AI 配置是 config.json 的一个子集；读写统一走 ConfigStore（扁平键值表），
// 这样 AI 保存设置时**不会覆盖** Shell 菜单写下的 flags.* 键（反之亦然）。
AiConfig LoadAiConfig() {
    AiConfig cfg;
    cfg.configPath = ConfigFilePath();

    auto& store = ConfigStore::Instance();
    store.Reload(true);
    const bool existed = store.KeyCount() > 0;
    cfg.endpoint = W(store.GetString("aiEndpoint", WideToUtf8(cfg.endpoint)));
    cfg.model = W(store.GetString("aiModel", WideToUtf8(cfg.model)));
    cfg.apiKeyEnv = store.GetString("aiApiKeyEnv", cfg.apiKeyEnv);
    cfg.timeoutMs = static_cast<uint32_t>(store.GetInt("aiTimeoutMs", static_cast<int>(cfg.timeoutMs)));
    if (cfg.timeoutMs < 5000 || cfg.timeoutMs > 600000) cfg.timeoutMs = 60000;
    if (!existed) {
        cfg.loadNote = "config.json 不存在，已写入默认值";
        SaveAiConfig(cfg);
    } else {
        cfg.loadNote = store.LastLoadOk() ? "已从 config.json 读取" : store.LastLoadNote();
    }

    // 环境变量覆盖（CI/临时测试用；不写入文件）。必须最后应用，且任何分支都不能跳过。
    auto envOverride = [](const wchar_t* name) -> std::wstring {
        wchar_t buf[2048]{};
        const DWORD n = ::GetEnvironmentVariableW(name, buf, 2048);
        return (n == 0 || n >= 2048) ? std::wstring() : std::wstring(buf, n);
    };
    bool overridden = false;
    if (const std::wstring e = envOverride(L"GITRT_AI_ENDPOINT"); !e.empty()) {
        cfg.endpoint = e;
        overridden = true;
    }
    if (const std::wstring e = envOverride(L"GITRT_AI_MODEL"); !e.empty()) {
        cfg.model = e;
        overridden = true;
    }
    if (const std::wstring e = envOverride(L"GITRT_AI_API_KEY_ENV"); !e.empty()) {
        cfg.apiKeyEnv = WideToUtf8(e);
        overridden = true;
    }
    if (overridden) cfg.loadNote += "（已应用 GITRT_AI_* 环境变量覆盖）";
    return cfg;
}

bool SaveAiConfig(const AiConfig& cfg) {
    // 只改 AI 自己的 4 个键，其余键（flags.* / menu.* / …）原样保留
    auto& store = ConfigStore::Instance();
    store.Reload();
    store.SetString("aiEndpoint", WideToUtf8(cfg.endpoint));
    store.SetString("aiModel", WideToUtf8(cfg.model));
    store.SetString("aiApiKeyEnv", cfg.apiKeyEnv);
    store.SetInt("aiTimeoutMs", static_cast<int>(cfg.timeoutMs));
    return store.Save();
}

std::wstring ResolveApiKey(const AiConfig& cfg) {
    if (cfg.apiKeyEnv.empty()) return {};
    const std::wstring name = W(cfg.apiKeyEnv);
    wchar_t buf[1024]{};
    const DWORD n = ::GetEnvironmentVariableW(name.c_str(), buf, 1024);
    if (n == 0 || n >= 1024) return {};
    return Trim(std::wstring(buf, n));
}

// ================================================================== HTTP
HttpResponse HttpPostJson(const std::wstring& url,
                          const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                          const std::string& bodyUtf8, uint32_t timeoutMs) {
    HttpResponse r;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        r.error = "URL 解析失败";
        return r;
    }
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    const std::wstring hostStr(host, uc.dwHostNameLength);
    const std::wstring pathStr(path, uc.dwUrlPathLength);

    HINTERNET session = ::WinHttpOpen(L"GitRT/" GRT_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.error = "WinHttpOpen 失败 err=" + std::to_string(::GetLastError());
        return r;
    }
    const int effectiveTimeout = static_cast<int>(timeoutMs ? timeoutMs : 60000);
    ::WinHttpSetTimeouts(session, effectiveTimeout, effectiveTimeout, effectiveTimeout, effectiveTimeout);

    HINTERNET conn = ::WinHttpConnect(session, hostStr.c_str(), uc.nPort, 0);
    if (!conn) {
        r.error = "WinHttpConnect 失败 err=" + std::to_string(::GetLastError());
        ::WinHttpCloseHandle(session);
        return r;
    }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = ::WinHttpOpenRequest(conn, L"POST", pathStr.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        r.error = "WinHttpOpenRequest 失败 err=" + std::to_string(::GetLastError());
        ::WinHttpCloseHandle(conn);
        ::WinHttpCloseHandle(session);
        return r;
    }
    for (const auto& h : headers) {
        const std::wstring line = h.first + L": " + h.second;
        ::WinHttpAddRequestHeaders(req, line.c_str(), static_cast<DWORD>(-1),
                                   WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    const BOOL sent = ::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                          const_cast<char*>(bodyUtf8.data()),
                                          static_cast<DWORD>(bodyUtf8.size()),
                                          static_cast<DWORD>(bodyUtf8.size()), 0);
    if (!sent) {
        r.error = "发送失败 err=" + std::to_string(::GetLastError());
    } else if (!::WinHttpReceiveResponse(req, nullptr)) {
        r.error = "接收响应失败 err=" + std::to_string(::GetLastError());
    } else {
        DWORD status = 0, len = sizeof(status);
        ::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);
        for (;;) {
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!::WinHttpReadData(req, buf.data(), avail, &read) || read == 0) break;
            r.body.append(buf.data(), read);
            if (r.body.size() > (8u << 20)) break;   // 8MB 上限
        }
    }
    ::WinHttpCloseHandle(req);
    ::WinHttpCloseHandle(conn);
    ::WinHttpCloseHandle(session);
    return r;
}

// ============================================================== 提示词
std::string BuildPlannerSystemPrompt(const TitleResolver& titleRes) {
    std::string s;
    s += "\u4f60\u662f GitRT \u7684\u547d\u4ee4\u89c4\u5212\u5668\u3002";   // 你是 GitRT 的命令规划器。
    s += "\u6839\u636e\u7528\u6237\u7684\u81ea\u7136\u8bed\u8a00\u610f\u56fe\uff0c\u4ece\u4e0b\u9762"
         "\u7684\u547d\u4ee4\u8868\u4e2d\u9009\u51fa\u6700\u5408\u9002\u7684**\u4e00\u6761**\u547d\u4ee4\uff0c"
         "\u5e76\u7ed9\u51fa\u53c2\u6570\u4e0e\u9009\u9879\u3002\n";
    s += "\u53ea\u8f93\u51fa\u4e00\u4e2a JSON \u5bf9\u8c61\uff0c\u4e0d\u8981\u8f93\u51fa\u4efb\u4f55"
         "\u89e3\u91ca\u6027\u6587\u5b57\u3001\u4e0d\u8981\u7528 markdown \u4ee3\u7801\u5757\u3002\n";
    s += "JSON \u7ed3\u6784\uff1a\n";
    s += "{\"command\":\"<\u547d\u4ee4 key\uff0c\u65e0\u5408\u9002\u7684\u65f6\u5019\u7528 none>\","
         "\"params\":{\"<\u53c2\u6570\u540d>\":\"<\u503c>\"},"
         "\"flags\":{\"<\u9009\u9879 key>\":\"1 \u6216 0\"},"
         "\"explanation\":\"<\u4e00\u53e5\u4e2d\u6587\u8bf4\u660e>\","
         "\"confidence\":0.0\u52301.0}\n\n";
    s += "\u89c4\u5219\uff1a\n";
    s += "1. \u53ea\u80fd\u4f7f\u7528\u4e0b\u8868\u5217\u51fa\u7684\u547d\u4ee4 key \u4e0e\u9009\u9879 key\uff0c"
         "\u4e0d\u5f97\u7f16\u9020\u3002\n";
    s += "2. \u53c2\u6570\u540d\u7528\u8868\u4e2d\u7684 param \u540d\uff08\u5982\u6709\uff09\uff1b"
         "\u7528\u6237\u672a\u8bf4\u660e\u4f46\u5fc5\u9700\u65f6\uff0c\u9009\u6700\u5408\u7406\u7684"
         "\u9ed8\u8ba4\u503c\u3002\n";
    s += "3. \u4e0d\u8981\u751f\u6210 git \u547d\u4ee4\u884c\uff0c\u53ea\u8fd4\u56de\u4e0a\u9762\u7684 JSON\u3002\n";
    s += "4. \u82e5\u6ca1\u6709\u547d\u4ee4\u80fd\u5b8c\u6210\u7528\u6237\u610f\u56fe\uff0c"
         "\u8fd4\u56de {\"command\":\"none\",\"explanation\":\"\u539f\u56e0\"}\u3002\n";
    s += "5. \u5371\u9669\u9009\u9879\uff08\u6807\u3010\u5371\u9669\u3011\uff09\u53ea\u5728\u7528\u6237"
         "\u660e\u786e\u8981\u6c42\u65f6\u624d\u7f6e 1\u3002\n\n";
    s += "\u547d\u4ee4\u8868\uff1a\n";

    for (size_t i = 0; i < CommandTableSize(); ++i) {
        const CommandSpec& c = CommandTable()[i];
        s += "- ";
        s += c.key;
        s += " | ";
        s += (titleRes ? titleRes(c.titleRes) : std::string(c.key));
        s += c.requiresRepo ? " | \u9700\u4ed3\u5e93" : " | \u65e0\u9700\u4ed3\u5e93";
        if (c.paramKey && c.param != ParamKind::None) {
            s += " | param: ";
            s += c.paramKey;
        }
        if (c.flags && c.flagCount) {
            s += " | \u9009\u9879: ";
            for (uint8_t k = 0; k < c.flagCount; ++k) {
                const FlagSpec& f = c.flags[k];
                if (k) s += "; ";
                s += f.key;
                if (f.danger) s += "(\u5371\u9669)";
                if (f.defaultOn) s += "(\u9ed8\u8ba4\u5f00)";
            }
        }
        if (c.danger == Danger::Destructive) s += " | \u672c\u547d\u4ee4\u5371\u9669";
        s += "\n";
    }
    return s;
}

std::string BuildPlannerUserPrompt(const std::wstring& userText, const std::wstring& repoRoot,
                                   const std::wstring& branch, const std::vector<std::wstring>& paths) {
    std::string s = "\u4e0a\u4e0b\u6587\uff1a\n";
    s += std::string("\u4ed3\u5e93\uff1a") + (repoRoot.empty() ? "\uff08\u672a\u9009\u62e9\uff09" : WideToUtf8(repoRoot)) + "\n";
    s += std::string("\u5f53\u524d\u5206\u652f\uff1a") + (branch.empty() ? "\uff08\u672a\u77e5\uff09" : WideToUtf8(branch)) + "\n";
    if (!paths.empty()) {
        s += "\u5df2\u9009\u6587\u4ef6\uff08" + std::to_string(paths.size()) + " \u9879\uff09\uff1a\n";
        for (size_t i = 0; i < paths.size() && i < 20; ++i)
            s += "  - " + WideToUtf8(paths[i]) + "\n";
    }
    s += "\n\u7528\u6237\u610f\u56fe\uff1a" + WideToUtf8(userText);
    return s;
}

// ============================================================== 解析
bool ExtractChatContent(const std::string& envelope, std::string* content) {
    // 先定位 "message"，再从其后找 "content"，避免命中其他字段
    const size_t msg = envelope.find("\"message\"");
    const size_t from = (msg == std::string::npos) ? 0 : msg;
    return JsonFindString(envelope, "content", content, from);
}

AiPlan ParsePlanReply(const std::string& replyContent) {
    AiPlan plan;
    plan.rawReply = W(replyContent);
    const std::string json = StripCodeFence(replyContent);

    std::string cmd;
    if (!JsonFindString(json, "command", &cmd)) {
        plan.error = L"模型回复里没有 command 字段（可能不是 JSON）";
        return plan;
    }
    if (cmd == "none" || cmd.empty()) {
        plan.ok = true;
        plan.noCommand = true;
        JsonFindString(json, "explanation", &cmd);
        plan.explanation = W(cmd);
        return plan;
    }
    plan.commandKey = cmd;
    JsonFindStringMap(json, "params", &plan.params);
    JsonFindStringMap(json, "flags", &plan.flags);
    std::string expl;
    if (JsonFindString(json, "explanation", &expl)) plan.explanation = W(expl);
    plan.ok = true;
    return plan;
}

AiPlan GeneratePlan(const AiConfig& cfg, const std::string& systemPrompt, const std::string& userPrompt) {
    AiPlan plan;
    const uint64_t t0 = ::GetTickCount64();
    const std::wstring key = ResolveApiKey(cfg);
    if (key.empty()) {
        plan.error = L"\u672a\u914d\u7f6e API Key\uff1a\u8bf7\u8bbe\u7f6e\u73af\u5883\u53d8\u91cf " +
                     W(cfg.apiKeyEnv) + L"\uff08\u7136\u540e\u91cd\u542f GitRT\uff09";
        return plan;
    }
    std::string body;
    body += "{";
    body += "\"model\":\"" + JsonEscape(WideToUtf8(cfg.model)) + "\",";
    body += "\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"" + JsonEscape(systemPrompt) + "\"},";
    body += "{\"role\":\"user\",\"content\":\"" + JsonEscape(userPrompt) + "\"}";
    body += "],";
    body += "\"temperature\":0,";
    body += "\"stream\":false";
    body += "}";
    plan.requestBody = body;

    const std::vector<std::pair<std::wstring, std::wstring>> headers = {
        {L"Content-Type", L"application/json; charset=utf-8"},
        {L"Authorization", L"Bearer " + key},
    };
    const HttpResponse resp = HttpPostJson(cfg.endpoint, headers, body, cfg.timeoutMs);
    plan.elapsedMs = ::GetTickCount64() - t0;
    plan.httpStatus = resp.status;

    if (!resp.error.empty()) {
        plan.error = L"\u7f51\u7edc\u8bf7\u6c42\u5931\u8d25\uff1a" + W(resp.error);
        return plan;
    }
    if (resp.status < 200 || resp.status >= 300) {
        std::string msg;
        JsonFindString(resp.body, "message", &msg);   // OpenAI 风格错误体
        plan.error = L"HTTP " + std::to_wstring(resp.status) +
                     (msg.empty() ? L"" : (L"\uff1a" + W(msg)));
        plan.rawReply = W(resp.body);
        return plan;
    }
    std::string content;
    if (!ExtractChatContent(resp.body, &content)) {
        plan.error = L"\u54cd\u5e94\u91cc\u627e\u4e0d\u5230 choices[0].message.content";
        plan.rawReply = W(resp.body);
        return plan;
    }
    AiPlan parsed = ParsePlanReply(content);
    parsed.httpStatus = resp.status;
    parsed.elapsedMs = plan.elapsedMs;
    parsed.requestBody = plan.requestBody;
    return parsed;
}

// ================================================= 计划 → 可执行命令（安全闸门）
PlanToCommandResult PlanToCommand(const AiPlan& plan, const std::wstring& repoRoot,
                                  const std::vector<std::wstring>& paths, const std::wstring& gitExe) {
    PlanToCommandResult res;
    if (plan.noCommand) {
        res.error = plan.explanation.empty() ? L"\u6a21\u578b\u8ba4\u4e3a\u6ca1\u6709\u5408\u9002\u7684\u547d\u4ee4"
                                             : plan.explanation;
        return res;
    }
    const CommandSpec* spec = FindCommandByKey(plan.commandKey);
    if (!spec) {
        res.error = L"\u6a21\u578b\u8fd4\u56de\u4e86\u547d\u4ee4\u8868\u4e2d\u4e0d\u5b58\u5728\u7684\u547d\u4ee4\uff1a" +
                    W(plan.commandKey) + L"\uff08\u5df2\u62d2\u7edd\u6267\u884c\uff09";
        return res;
    }
    if (spec->exec == ExecKind::Internal) {
        res.error = L"\u547d\u4ee4 " + W(spec->key) +
                    L" \u9700\u8981\u56fe\u5f62\u754c\u9762\u4ea4\u4e92\uff0c\u8bf7\u5728\u5de6\u4fa7"
                    L"\u547d\u4ee4\u5217\u8868\u4e2d\u624b\u52a8\u6267\u884c";
        return res;
    }
    if (spec->requiresRepo && repoRoot.empty()) {
        res.error = L"\u8be5\u547d\u4ee4\u9700\u8981\u5148\u9009\u62e9\u4ed3\u5e93";
        return res;
    }

    BuildInput in;
    in.spec = spec;
    in.gitExe = gitExe;
    in.repoRoot = repoRoot;
    in.cwd = repoRoot;
    in.paths = paths;

    // flags：只接受该命令 FlagSpec 白名单内的 key，未知项一律忽略并提示
    if (spec->flags) {
        for (uint8_t i = 0; i < spec->flagCount; ++i) {
            const FlagSpec& f = spec->flags[i];
            const auto it = plan.flags.find(f.key);
            if (it == plan.flags.end()) continue;
            std::string v = it->second;
            for (auto& ch : v) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
            in.flags[f.key] = (v == "1" || v == "true" || v == "yes") ? L"1" : L"0";
            if (f.danger && in.flags[f.key] == L"1")
                res.warnings.push_back(L"\u5df2\u542f\u7528\u5371\u9669\u9009\u9879\uff1a" + W(f.key));
        }
    }
    for (const auto& kv : plan.flags) {
        if (!FindFlag(*spec, kv.first))
            res.warnings.push_back(L"\u5ffd\u7565\u4e86\u547d\u4ee4\u8868\u4e2d\u4e0d\u5b58\u5728\u7684\u9009\u9879\uff1a" +
                                   W(kv.first));
    }
    // params：只接受该命令声明的 paramKey
    for (const auto& kv : plan.params) {
        if (spec->paramKey && kv.first == spec->paramKey) {
            in.params[kv.first] = W(kv.second);
        } else {
            res.warnings.push_back(L"\u5ffd\u7565\u4e86\u672a\u58f0\u660e\u7684\u53c2\u6570\uff1a" + W(kv.first));
        }
    }

    BuildError err;
    BuiltCommand out;
    if (!BuildCommand(in, &out, &err)) {
        res.error = err.message.empty() ? L"\u547d\u4ee4\u6784\u9020\u5931\u8d25" : err.message;
        return res;
    }
    res.ok = true;
    res.spec = spec;
    res.built = out;
    return res;
}

}  // namespace grt
