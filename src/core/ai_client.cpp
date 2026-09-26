#include "ai_client.h"

#include <winhttp.h>
#include <wincrypt.h>   // DPAPI：可选地把 Key 加密后落盘（CryptProtectData / CryptUnprotectData）

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "json_util.h"
#include "config.h"
#include "command_builder.h"

namespace grt {

// ================================================================== 配置
// AI 设置主存储：exe 同目录的 GitRT.ai.json（明文；产品决策，见 ai_client.h）
std::wstring AiKeyFilePath() {
    wchar_t exe[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(exe, n) : std::wstring();
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash + 1);
    return dir + L"GitRT.ai.json";
}

namespace {

std::string ReadWholeFile(const std::wstring& path) {
    UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(static_cast<HANDLE>(h.get()), &size) || size.QuadPart <= 0 ||
        size.QuadPart > (1 << 20))
        return {};
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    if (!::ReadFile(static_cast<HANDLE>(h.get()), out.data(), static_cast<DWORD>(out.size()), &got,
                    nullptr))
        return {};
    out.resize(got);
    return out;
}

bool WriteWholeFile(const std::wstring& path, const std::string& text) {
    UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = ::WriteFile(static_cast<HANDLE>(h.get()), text.data(),
                                static_cast<DWORD>(text.size()), &wrote, nullptr) != FALSE;
    return ok && wrote == text.size();
}

// 目录可写探测（界面要明确告诉用户"Key 存到了哪里/能不能存"）
bool DirWritable(const std::wstring& filePath) {
    const std::wstring probe = filePath + L".probe";
    UniqueHandle h(::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return false;
    h.reset();
    ::DeleteFileW(probe.c_str());
    return true;
}

// ------------------------------------------------ Key 的可选加密（DPAPI + Base64）
// 落盘形式二选一：
//   · 明文（默认，产品决策：方便直接查看/修改）
//   · "dpapi:<base64(CryptProtectData(utf8(key)))>"（按当前用户加密，换机/换用户解不开）
constexpr const char* kDpapiPrefix = "dpapi:";

const char kBase64Tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<BYTE>& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                           (static_cast<uint32_t>(in[i + 1]) << 8) | static_cast<uint32_t>(in[i + 2]);
        out.push_back(kBase64Tab[(v >> 18) & 63]);
        out.push_back(kBase64Tab[(v >> 12) & 63]);
        out.push_back(kBase64Tab[(v >> 6) & 63]);
        out.push_back(kBase64Tab[v & 63]);
    }
    const size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
        out.push_back(kBase64Tab[(v >> 18) & 63]);
        out.push_back(kBase64Tab[(v >> 12) & 63]);
        out += "==";
    } else if (rem == 2) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                           (static_cast<uint32_t>(in[i + 1]) << 8);
        out.push_back(kBase64Tab[(v >> 18) & 63]);
        out.push_back(kBase64Tab[(v >> 12) & 63]);
        out.push_back(kBase64Tab[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

int Base64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool Base64Decode(const std::string& s, std::vector<BYTE>* out) {
    out->clear();
    uint32_t buf = 0;
    int bits = 0;
    for (const char c : s) {
        if (c == '=' || c == '\r' || c == '\n') continue;
        const int v = Base64Val(c);
        if (v < 0) return false;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<BYTE>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

std::vector<BYTE> DpapiProtect(const std::string& plain) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    if (!::CryptProtectData(&in, L"GitRT AI key", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    const std::vector<BYTE> v(out.pbData, out.pbData + out.cbData);
    ::LocalFree(out.pbData);
    return v;
}

bool DpapiUnprotect(const std::vector<BYTE>& blob, std::string* plain) {
    if (blob.empty()) return false;
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(blob.data());
    in.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plain->assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::LocalFree(out.pbData);
    return true;
}

// Key 是否以密文形式存储（在 LoadAiConfig 里直接比对前缀即可，这里不另留函数）

}  // namespace

AiConfig LoadAiConfig() {
    AiConfig cfg;
    cfg.configPath = ConfigFilePath();
    cfg.keyFilePath = AiKeyFilePath();
    cfg.keyFileUsable = DirWritable(cfg.keyFilePath);

    auto& store = ConfigStore::Instance();
    store.Reload(true);
    const bool existed = store.KeyCount() > 0;
    cfg.endpoint = W(store.GetString("aiEndpoint", WideToUtf8(cfg.endpoint)));
    cfg.model = W(store.GetString("aiModel", WideToUtf8(cfg.model)));
    cfg.apiKeyEnv = store.GetString("aiApiKeyEnv", cfg.apiKeyEnv);
    cfg.timeoutMs = static_cast<uint32_t>(store.GetInt("aiTimeoutMs", static_cast<int>(cfg.timeoutMs)));
    if (!existed) {
        cfg.loadNote = "config.json 不存在，已写入默认值";
        SaveAiConfig(cfg);
    } else {
        cfg.loadNote = store.LastLoadOk() ? "已从 config.json 读取" : store.LastLoadNote();
    }

    // ② exe 同目录的 GitRT.ai.json 覆盖 config.json（它才是"AI 设置"界面的落点）
    const std::string fileText = ReadWholeFile(cfg.keyFilePath);
    if (!fileText.empty()) {
        std::string v;
        if (JsonFindString(fileText, "endpoint", &v) && !v.empty()) cfg.endpoint = W(v);
        if (JsonFindString(fileText, "model", &v) && !v.empty()) cfg.model = W(v);
        if (JsonFindString(fileText, "apiKeyEnv", &v) && !v.empty()) cfg.apiKeyEnv = v;
        const std::string rawTimeout = [&] {
            std::string r;
            return JsonFindRaw(fileText, "timeoutMs", &r) ? r : std::string();
        }();
        if (!rawTimeout.empty()) {
            const int t = std::atoi(rawTimeout.c_str());
            if (t >= 5000 && t <= 600000) cfg.timeoutMs = static_cast<uint32_t>(t);
        }
        cfg.systemPrompt = JsonFindString(fileText, "systemPrompt", &v) ? W(v) : std::wstring();
        // Key 有两种落盘形式：明文，或 DPAPI 密文（dpapi:<base64>）
        std::string protectRaw;
        const bool fileSaysProtect =
            JsonFindRaw(fileText, "protectKey", &protectRaw) && protectRaw == "true";
        bool storedProtected = false;
        if (JsonFindString(fileText, "apiKey", &v)) {
            const std::wstring stored = Trim(W(v));
            const std::wstring prefix = W(kDpapiPrefix);
            if (stored.rfind(prefix, 0) == 0) {
                storedProtected = true;
                std::vector<BYTE> blob;
                std::string plain;
                if (Base64Decode(WideToUtf8(stored.substr(prefix.size())), &blob) &&
                    DpapiUnprotect(blob, &plain)) {
                    cfg.apiKey = Trim(W(plain));
                } else {
                    // 换用户/换机器就会走到这里：**不清文件**，只报明原因让用户重填
                    cfg.apiKey.clear();
                    cfg.loadNote += "（Key 是 DPAPI 密文，当前用户/机器解不开，请重新填写）";
                }
            } else {
                cfg.apiKey = stored;
            }
            cfg.keyFromFile = !cfg.apiKey.empty();
        }
        // 复选框状态：文件里写了 protectKey，或者里面的 Key 本来就是密文
        cfg.protectKey = fileSaysProtect || storedProtected;
        cfg.loadNote += "（已应用 " + WideToUtf8(cfg.keyFilePath) + "）";
    }

    // ③ 环境变量覆盖（CI/临时测试用；不写入文件）。必须最后应用，任何分支都不能跳过。
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
    if (cfg.timeoutMs < 5000 || cfg.timeoutMs > 600000) cfg.timeoutMs = 60000;
    return cfg;
}

bool SaveAiConfig(const AiConfig& cfg) {
    // ① 主存储：exe 同目录的 GitRT.ai.json（默认明文含 Key；可选 DPAPI 密文）
    std::string keyField = WideToUtf8(cfg.apiKey);
    bool protectFailed = false;
    if (cfg.protectKey && !cfg.apiKey.empty()) {
        const std::vector<BYTE> blob = DpapiProtect(keyField);
        if (blob.empty()) {
            protectFailed = true;        // 加密失败 → 宁可丢 Key 也不明文落盘（见下）
            keyField.clear();
        } else {
            keyField = std::string(kDpapiPrefix) + Base64Encode(blob);
        }
    }
    std::string json = "{\n";
    json += "  \"endpoint\": \"" + JsonEscape(WideToUtf8(cfg.endpoint)) + "\",\n";
    json += "  \"model\": \"" + JsonEscape(WideToUtf8(cfg.model)) + "\",\n";
    json += "  \"apiKey\": \"" + JsonEscape(keyField) + "\",\n";
    json += "  \"protectKey\": " + std::string(cfg.protectKey ? "true" : "false") + ",\n";
    json += "  \"apiKeyEnv\": \"" + JsonEscape(cfg.apiKeyEnv) + "\",\n";
    json += "  \"timeoutMs\": " + std::to_string(cfg.timeoutMs) + ",\n";
    json += "  \"systemPrompt\": \"" + JsonEscape(WideToUtf8(cfg.systemPrompt)) + "\"\n";
    json += "}\n";
    const std::wstring path = cfg.keyFilePath.empty() ? AiKeyFilePath() : cfg.keyFilePath;
    const bool ok = WriteWholeFile(path, json);

    // ② 兼容：config.json 里的 ai* 键仍保持同步（不含 Key），老流程/文档仍然成立
    auto& store = ConfigStore::Instance();
    store.Reload();
    store.SetString("aiEndpoint", WideToUtf8(cfg.endpoint));
    store.SetString("aiModel", WideToUtf8(cfg.model));
    store.SetString("aiApiKeyEnv", cfg.apiKeyEnv);
    store.SetInt("aiTimeoutMs", static_cast<int>(cfg.timeoutMs));
    store.Save();
    // 勾了加密却加不上（DPAPI 失败）→ 报"保存失败"：此时文件里没有 Key，
    // 宁可让用户看到失败并重试，也不把明文写进去（fail-closed）。
    return ok && !protectFailed;
}

std::wstring ResolveApiKey(const AiConfig& cfg) {
    // ① 界面里填的明文 Key（exe 同目录的 GitRT.ai.json）
    if (!cfg.apiKey.empty()) return cfg.apiKey;
    // ② 环境变量（名字可配置）：适合"不想让 Key 落盘"的用户与 CI
    if (cfg.apiKeyEnv.empty()) return {};
    const std::wstring name = W(cfg.apiKeyEnv);
    wchar_t buf[1024]{};
    const DWORD n = ::GetEnvironmentVariableW(name.c_str(), buf, 1024);
    if (n == 0 || n >= 1024) return {};
    return Trim(std::wstring(buf, n));
}

// ================================================================== HTTP
namespace {

// POST/GET 共用实现（原 HttpPostJson 直接内联，这里抽出来给 GET 复用）
HttpResponse HttpRequest(const wchar_t* method, const std::wstring& url,
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
    HINTERNET req = ::WinHttpOpenRequest(conn, method, pathStr.c_str(), nullptr,
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

}  // namespace

HttpResponse HttpPostJson(const std::wstring& url,
                          const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                          const std::string& bodyUtf8, uint32_t timeoutMs) {
    return HttpRequest(L"POST", url, headers, bodyUtf8, timeoutMs);
}

HttpResponse HttpGetJson(const std::wstring& url,
                         const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                         uint32_t timeoutMs) {
    // GET 不带请求体（WinHttpSendRequest 传 0 长度即为 GET）
    return HttpRequest(L"GET", url, headers, std::string(), timeoutMs);
}

// ---------------------------------------------------------------- 模型列表
std::wstring AiModelsUrlFromEndpoint(const std::wstring& endpointIn) {
    std::wstring u = Trim(endpointIn);
    while (!u.empty() && u.back() == L'/') u.pop_back();
    for (const wchar_t* tail : {L"/chat/completions", L"/completions"}) {
        const size_t n = std::wcslen(tail);
        if (u.size() >= n && ::_wcsicmp(u.c_str() + u.size() - n, tail) == 0) {
            u.resize(u.size() - n);
            while (!u.empty() && u.back() == L'/') u.pop_back();
            break;
        }
    }
    if (u.empty()) return {};
    return u + L"/models";
}

// ------------------------------------------------------------ 端点安全判定
namespace {

// 取 endpoint 的主机名（小写）：跳过 scheme 与 userinfo，截到第一个 / ? #，去掉端口。
std::wstring EndpointHost(const std::wstring& url) {
    const std::wstring s = Trim(url);
    const size_t scheme = s.find(L"://");
    const size_t start = (scheme == std::wstring::npos) ? 0 : scheme + 3;
    size_t end = s.size();
    for (size_t i = start; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'/' || c == L'?' || c == L'#') {
            end = i;
            break;
        }
    }
    std::wstring auth = s.substr(start, end - start);
    const size_t at = auth.find(L'@');             // user:pass@host → host
    if (at != std::wstring::npos) auth = auth.substr(at + 1);
    if (!auth.empty() && auth[0] == L'[') {        // [::1]:11434 → [::1]
        const size_t rb = auth.find(L']');
        if (rb != std::wstring::npos) auth = auth.substr(0, rb + 1);
    } else {
        const size_t colon = auth.find(L':');
        if (colon != std::wstring::npos) auth = auth.substr(0, colon);
    }
    return ToLowerAscii(auth);
}

}  // namespace

bool IsLoopbackEndpoint(const std::wstring& url) {
    const std::wstring host = EndpointHost(url);
    if (host == L"localhost" || host == L"::1" || host == L"[::1]") return true;
    // 127.0.0.0/8：必须整段都是数字与点，避免 "127.evil.com" 被当成回环
    if (host.rfind(L"127.", 0) == 0) {
        for (const wchar_t c : host)
            if (!((c >= L'0' && c <= L'9') || c == L'.')) return false;
        return true;
    }
    return false;
}

bool IsInsecureRemoteEndpoint(const std::wstring& url) {
    const std::wstring s = Trim(url);
    if (s.size() < 7 || _wcsnicmp(s.c_str(), L"http://", 7) != 0) return false;   // https / 其它 scheme
    return !IsLoopbackEndpoint(s);
}

namespace {

// 从 pos 处读一个 JSON 字符串字面量（含转义），成功返回真并把结束位置写回 end
bool ReadJsonStringAt(const std::string& json, size_t pos, std::string* out, size_t* end) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' ||
                                 json[pos] == '\n'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    std::string raw;
    bool esc = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (esc) {
            raw += '\\';
            raw += c;
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '"') {
            if (out) *out = JsonUnescape(raw);
            if (end) *end = pos + 1;
            return true;
        }
        raw += c;
    }
    return false;
}

}  // namespace

ModelListResult FetchModelList(const AiConfig& cfg) {
    ModelListResult out;
    out.url = AiModelsUrlFromEndpoint(cfg.endpoint);
    const std::wstring key = ResolveApiKey(cfg);
    if (out.url.empty()) {
        out.error = "接口地址为空";
        return out;
    }
    // 本机服务（Ollama / LM Studio / vLLM …）通常不鉴权：Key 为空不是错误，照常发请求
    if (key.empty() && !IsLoopbackEndpoint(out.url)) {
        out.error = "未配置 API Key（远端服务需要；本机服务如 Ollama 可以不填）";
        return out;
    }
    std::vector<std::pair<std::wstring, std::wstring>> headers = {
        {L"Accept", L"application/json"},
    };
    if (!key.empty()) headers.push_back({L"Authorization", L"Bearer " + key});
    const HttpResponse r = HttpGetJson(out.url, headers, (std::min)(cfg.timeoutMs, 30000u));
    out.status = r.status;
    out.error = r.error;
    if (r.status != 200) return out;

    // {"data":[{"id":"…", …}, …]}（也兼容 {"models":[…]} / 裸数组）
    size_t pos = JsonFindKeyPos(r.body, "data");
    if (pos == std::string::npos) pos = JsonFindKeyPos(r.body, "models");
    if (pos == std::string::npos) pos = 0;
    for (;;) {
        const size_t kp = JsonFindKeyPos(r.body, "id", pos);
        if (kp == std::string::npos) break;
        std::string val;
        size_t end = 0;
        if (!ReadJsonStringAt(r.body, kp, &val, &end)) {
            pos = kp + 1;
            continue;
        }
        const std::wstring w = W(val);
        if (!w.empty() && std::find(out.models.begin(), out.models.end(), w) == out.models.end())
            out.models.push_back(w);
        pos = end;
        if (out.models.size() >= 500) break;   // 防御：异常大的列表
    }
    return out;
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
    s += "\u4e8c\u9009\u4e00\uff1a\u67e5\u770b\u7c7b\u7528 {\"cmdline\":\"git ...\",...}\uff0c"
         "\u5199\u64cd\u4f5c\u7528 {\"command\":\"<\u547d\u4ee4 key>\",...}\u3002\u5b57\u6bb5\uff1a\n";
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
    s += "3. \u67e5\u770b\u7c7b\u9700\u6c42\uff08\u5f53\u524d\u5206\u652f/\u72b6\u6001/\u63d0\u4ea4\u5386\u53f2/\u5dee\u5f02/\u6587\u4ef6\u5386\u53f2\u7b49\uff09"
         "\u4f18\u5148\u76f4\u63a5\u7ed9\u4e00\u6761**\u53ea\u8bfb git \u547d\u4ee4\u884c**\uff1a"
         "{\"cmdline\":\"git status -sb\",\"explanation\":\"\u4e00\u53e5\u8bf4\u660e\",\"confidence\":0.9}\u3002"
         "\u53ea\u5141\u8bb8\u53ea\u8bfb\u5b50\u547d\u4ee4\uff08status/log/show/diff/branch/remote/tag -l/"
         "rev-parse/describe/shortlog/blame/for-each-ref/ls-files/config --get*/stash list/reflog\uff09\uff0c"
         "\u4e0d\u8981\u5e26\u4efb\u4f55\u5199\u64cd\u4f5c\u9009\u9879\u3002\n";
    s += "4. \u9700\u8981\u5199\u64cd\u4f5c\uff08\u63d0\u4ea4/\u63a8\u9001/\u5408\u5e76/\u91cd\u7f6e\u7b49\uff09\u65f6\uff0c"
         "\u7528\u4e0b\u8868\u7684\u547d\u4ee4 key\uff08\u5b57\u6bb5 command\uff09\uff0c\u4e0d\u8981\u81ea\u5df1\u7f16 git \u547d\u4ee4\u884c\u3002\n";
    s += "5. **\u4e0d\u8981**\u9009\u9700\u8981\u56fe\u5f62\u754c\u9762\u4ea4\u4e92\u7684\u5185\u90e8\u547d\u4ee4"
         "\uff08key \u4ee5 app. / commit.squash / inspect.status \u7b49\u9700\u7a97\u53e3\u7684\uff09\uff1b"
         "\u5b83\u4eec\u4e0d\u4f1a\u88ab\u81ea\u52a8\u6267\u884c\u3002\n";
    s += "6. \u82e5\u6ca1\u6709\u547d\u4ee4\u80fd\u5b8c\u6210\u7528\u6237\u610f\u56fe\uff0c"
         "\u8fd4\u56de {\"command\":\"none\",\"explanation\":\"\u539f\u56e0\"}\u3002\n";
    s += "7. \u5371\u9669\u9009\u9879\uff08\u6807\u3010\u5371\u9669\u3011\uff09\u53ea\u5728\u7528\u6237"
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

// 生效的系统提示词：AI 设置里填了就用它（用户可以完全改写行为），否则用内置默认
std::string EffectiveSystemPrompt(const AiConfig& cfg, const TitleResolver& titleRes) {
    if (!cfg.systemPrompt.empty()) return WideToUtf8(cfg.systemPrompt);
    return BuildPlannerSystemPrompt(titleRes);
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

    // 二选一：{"cmdline":"git status -sb"}（直接给命令）或 {"command":"<命令表 key>"}
    std::string rawCmd;
    std::string cmd;
    const bool hasCmdline = JsonFindString(json, "cmdline", &rawCmd) && !Trim(W(rawCmd)).empty();
    if (!hasCmdline && !JsonFindString(json, "command", &cmd)) {
        plan.error = L"模型回复里既没有 cmdline 也没有 command 字段（可能不是 JSON）";
        return plan;
    }
    if (hasCmdline) {
        plan.cmdline = Trim(W(rawCmd)) == L"none" ? std::string() : rawCmd;
        JsonFindStringMap(json, "params", &plan.params);
        std::string expl;
        if (JsonFindString(json, "explanation", &expl)) plan.explanation = W(expl);
        plan.ok = true;
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
    // 本机服务不鉴权时允许没有 Key（否则 Ollama / LM Studio 这类端点根本用不了）
    if (key.empty() && !IsLoopbackEndpoint(cfg.endpoint)) {
        plan.error = L"\u672a\u914d\u7f6e API Key\uff1a\u8bf7\u8bbe\u7f6e\u73af\u5883\u53d8\u91cf " +
                     W(cfg.apiKeyEnv) +
                     L"\uff08\u7136\u540e\u91cd\u542f GitRT\uff09\u3002\u82e5\u7528\u7684\u662f\u672c\u673a"
                     L"\u670d\u52a1\uff08Ollama / LM Studio / vLLM\uff09\uff0c\u63a5\u53e3\u5730\u5740"
                     L"\u5199\u6210 http://127.0.0.1:<\u7aef\u53e3>/v1/chat/completions \u5c31\u53ef\u4ee5"
                     L"\u4e0d\u586b Key\u3002";
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

    std::vector<std::pair<std::wstring, std::wstring>> headers = {
        {L"Content-Type", L"application/json; charset=utf-8"},
    };
    if (!key.empty()) headers.push_back({L"Authorization", L"Bearer " + key});
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
// ---------------------------------------------- AI 直出命令（只读白名单）
// "方案应该直接输出命令"：允许模型给一条**只读** git 命令并直接执行。
// 写操作（提交/推送/合并/重置…）一律仍走命令表，避免模型一句话就改写仓库。
const CommandSpec& ReadOnlySpec() {
    static const CommandSpec kSpec{0, "ai.cmdline", GroupId::Inspect, IDS_TITLE_AI, 0, kSelAny, true,
                                   Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
                                   ExecKind::CliPanel, nullptr, 0, nullptr};
    return kSpec;
}

std::wstring JoinArgvForDisplay(const std::vector<std::wstring>& argv) {
    std::wstring s = L"git";
    for (const auto& a : argv) {
        s += L' ';
        s += QuoteArg(a);
    }
    return s;
}

bool SplitCommandLine(const std::wstring& line, std::vector<std::wstring>* out, std::wstring* why) {
    std::wstring cur;
    wchar_t quote = 0;
    bool any = false;
    for (const wchar_t c : line) {
        if (c == L'\r' || c == L'\n' || c == L'\0') {
            if (why) *why = L"命令里不能有换行";
            return false;
        }
        if (quote) {
            if (c == quote) { quote = 0; continue; }
            cur += c;
            continue;
        }
        if (c == L'"' || c == L'\'') { quote = c; any = true; continue; }
        if (c == L' ' || c == L'\t') {
            if (!cur.empty() || any) { out->push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur += c;
    }
    if (quote) {
        if (why) *why = L"引号没有闭合";
        return false;
    }
    if (!cur.empty() || any) out->push_back(cur);
    return true;
}

bool IsPureReadOnlySubcommand(const std::wstring& sub) {
    static const wchar_t* kOk[] = {L"status",   L"log",        L"show",       L"diff",      L"rev-parse",
                                   L"rev-list", L"describe",   L"shortlog",   L"blame",     L"for-each-ref",
                                   L"ls-files", L"ls-tree",    L"cat-file",   L"reflog",    L"name-rev",
                                   L"merge-base", L"count-objects", L"whatchanged", L"grep", L"diff-tree",
                                   L"diff-index", L"diff-files", L"version", L"help", L"var"};
    for (const wchar_t* k : kOk) {
        if (sub == k) return true;
    }
    return false;
}

bool IsReadOnlyWithGuard(const std::vector<std::wstring>& args) {
    if (args.size() < 2) return false;
    const std::wstring& sub = args[1];
    auto has = [&](std::initializer_list<const wchar_t*> names) {
        for (size_t i = 2; i < args.size(); ++i) {
            for (const wchar_t* n : names) {
                if (args[i] == n) return true;
            }
        }
        return false;
    };
    auto firstIs = [&](std::initializer_list<const wchar_t*> names) {
        if (args.size() < 3) return false;
        for (const wchar_t* n : names) {
            if (args[2] == n) return true;
        }
        return false;
    };
    if (sub == L"branch") {
        for (size_t i = 2; i < args.size(); ++i) {
            if (!args[i].empty() && args[i][0] != L'-') return false;
        }
        return true;
    }
    if (sub == L"tag") return has({L"-l", L"--list"});
    if (sub == L"remote") return firstIs({L"-v", L"--verbose", L"show", L"get-url"});
    if (sub == L"stash") return firstIs({L"list", L"show"});
    if (sub == L"config") return firstIs({L"--get", L"--get-all", L"--get-regexp", L"--list", L"-l"});
    if (sub == L"worktree") return firstIs({L"list"});
    if (sub == L"submodule") return firstIs({L"status", L"summary"});
    return false;
}

bool ParseReadOnlyGitCommand(const std::wstring& line, std::vector<std::wstring>* argv, std::wstring* why) {
    std::vector<std::wstring> parts;
    if (!SplitCommandLine(Trim(line), &parts, why)) return false;
    if (parts.size() < 2) {
        if (why) *why = L"至少要给出 git 子命令（例如 git status）";
        return false;
    }
    const std::wstring exe = ToLowerAscii(parts[0]);
    if (exe != L"git" && exe != L"git.exe") {
        if (why) *why = L"只允许 git 命令，收到的是：" + parts[0];
        return false;
    }
    const std::wstring sub = ToLowerAscii(parts[1]);
    if (!IsPureReadOnlySubcommand(sub) && !IsReadOnlyWithGuard(parts)) {
        if (why) *why = L"不是允许的只读子命令（写操作请用命令表）：" + parts[1];
        return false;
    }
    argv->assign(parts.begin() + 1, parts.end());
    return true;
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
    // ---- 路线 A：模型直接给了命令（cmdline）----
    // 只放行**只读** git 命令：这是"方案直接输出命令"的安全边界，写操作仍必须走命令表。
    if (!plan.cmdline.empty()) {
        std::vector<std::wstring> argv;
        std::wstring why;
        if (!ParseReadOnlyGitCommand(W(plan.cmdline), &argv, &why)) {
            res.error = L"模型想直接执行命令，但没通过只读检查：" + why + L"\n（原始命令：" + W(plan.cmdline) + L"）";
            return res;
        }
        res.ok = true;
        res.spec = &ReadOnlySpec();
        res.built.argvList = {argv};
        res.built.cwd = repoRoot;
        res.built.display = JoinArgvForDisplay(argv);
        res.built.notes.push_back(L"由 AI 直接给出的只读命令");
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
