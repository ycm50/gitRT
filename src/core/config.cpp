// config.json 访问层实现（《技术实现设计》§12.1）
//
// 文件被建模为"扁平键值表"：每个顶层成员的**原文**都保留下来，
// 因此本进程不认识的键（AI 配置、将来新增的键）不会被写回时丢掉。
#include "config.h"

#include "json_util.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace grt {
namespace {

std::mutex                                        g_mutex;
std::map<std::string, std::string>                g_kv;      // key → 原始 JSON 值
std::vector<std::string>                          g_order;   // 落盘顺序（稳定输出）
uint64_t                                          g_lastLoadMs = 0;
bool                                              g_loadOk = true;
std::string                                       g_note;
bool                                              g_loaded = false;

constexpr uint64_t kReloadThrottleMs = 5000;
constexpr size_t   kMaxConfigBytes   = 256 * 1024;

void SkipWs(const std::string& s, size_t* i) {
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n')) ++*i;
}

// 解析一个 JSON 字符串字面量（含两端引号），返回内容（已反转义）
bool ParseJsonString(const std::string& s, size_t* i, std::string* out) {
    if (*i >= s.size() || s[*i] != '"') return false;
    const size_t start = *i;
    ++*i;
    bool escaped = false;
    while (*i < s.size()) {
        const char c = s[*i];
        if (escaped) {
            escaped = false;
            ++*i;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            ++*i;
            continue;
        }
        if (c == '"') {
            const std::string raw = s.substr(start + 1, *i - start - 1);
            *out = JsonUnescape(raw);
            ++*i;
            return true;
        }
        ++*i;
    }
    return false;
}

// 跳过一段 JSON 值（字符串 / 对象 / 数组 / 字面量），返回其原文
bool CaptureValue(const std::string& s, size_t* i, std::string* raw) {
    SkipWs(s, i);
    if (*i >= s.size()) return false;
    const char c = s[*i];
    if (c == '"') {
        const size_t start = *i;
        size_t j = *i + 1;
        bool escaped = false;
        while (j < s.size()) {
            if (escaped) { escaped = false; ++j; continue; }
            if (s[j] == '\\') { escaped = true; ++j; continue; }
            if (s[j] == '"') break;
            ++j;
        }
        if (j >= s.size()) return false;
        *raw = s.substr(start, j - start + 1);
        *i = j + 1;
        return true;
    }
    if (c == '{' || c == '[') {
        const char open = c;
        const char close = (c == '{') ? '}' : ']';
        int depth = 0;
        const size_t start = *i;
        size_t j = *i;
        bool inStr = false, escaped = false;
        for (; j < s.size(); ++j) {
            const char ch = s[j];
            if (inStr) {
                if (escaped) { escaped = false; continue; }
                if (ch == '\\') { escaped = true; continue; }
                if (ch == '"') inStr = false;
                continue;
            }
            if (ch == '"') { inStr = true; continue; }
            if (ch == open) ++depth;
            else if (ch == close) {
                --depth;
                if (depth == 0) { ++j; break; }
            }
        }
        if (depth != 0) return false;
        *raw = s.substr(start, j - start);
        *i = j;
        return true;
    }
    // 数字 / true / false / null
    const size_t start = *i;
    while (*i < s.size() && s[*i] != ',' && s[*i] != '}' && s[*i] != '\n' && s[*i] != '\r') ++*i;
    std::wstring t = Trim(Utf8ToWide(s.substr(start, *i - start)));
    *raw = WideToUtf8(t);
    return true;
}

bool ParseFlatObject(const std::string& text, std::map<std::string, std::string>* kv,
                     std::vector<std::string>* order) {
    size_t i = text.find('{');
    if (i == std::string::npos) return false;
    ++i;
    while (true) {
        SkipWs(text, &i);
        if (i >= text.size()) return false;
        if (text[i] == '}') return true;
        if (text[i] == ',') { ++i; continue; }
        std::string key;
        if (!ParseJsonString(text, &i, &key)) return false;
        SkipWs(text, &i);
        if (i >= text.size() || text[i] != ':') return false;
        ++i;
        std::string raw;
        if (!CaptureValue(text, &i, &raw)) return false;
        if (!kv->count(key)) order->push_back(key);
        (*kv)[key] = raw;
    }
}

std::wstring BackupPath() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t name[96]{};
    std::swprintf(name, 96, L"config.bad-%04d%02d%02d-%02d%02d%02d.json", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return JoinPath(AppDataDir(), name);
}

std::string ReadWholeFile(const std::wstring& path, bool* ok) {
    *ok = false;
    UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[8192];
    DWORD got = 0;
    while (::ReadFile(static_cast<HANDLE>(h.get()), buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
        if (out.size() >= kMaxConfigBytes) break;
    }
    *ok = !out.empty();
    return out;
}

std::string UnquoteRaw(const std::string& raw) {
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        return JsonUnescape(std::string_view(raw).substr(1, raw.size() - 2));
    return raw;
}

}  // namespace

// --------------------------------------------------------------- 单例与路径
ConfigStore& ConfigStore::Instance() {
    static ConfigStore inst;
    return inst;
}

std::wstring ConfigStore::Path() const { return ConfigFilePath(); }

// --------------------------------------------------------------------- 读盘
void ConfigStore::Reload(bool force) {
    std::lock_guard lock(g_mutex);
    const uint64_t now = ::GetTickCount64();
    if (!force && g_loaded && now - g_lastLoadMs < kReloadThrottleMs) return;
    g_lastLoadMs = now;
    g_loaded = true;

    bool ok = false;
    const std::string text = ReadWholeFile(ConfigFilePath(), &ok);
    if (!ok || Trim(Utf8ToWide(text)).empty()) {
        g_kv.clear();
        g_order.clear();
        g_loadOk = true;
        g_note = "config.json 不存在或为空，使用默认值";
        return;
    }
    std::map<std::string, std::string> kv;
    std::vector<std::string> order;
    if (!ParseFlatObject(text, &kv, &order)) {
        // 损坏 → 备份后以默认值继续（绝不静默丢弃用户文件）
        ::MoveFileExW(ConfigFilePath().c_str(), BackupPath().c_str(), MOVEFILE_REPLACE_EXISTING);
        g_kv.clear();
        g_order.clear();
        g_loadOk = false;
        g_note = "config.json 解析失败，已备份为 config.bad-*.json 并重置";
        return;
    }
    g_kv.swap(kv);
    g_order.swap(order);
    g_loadOk = true;
    g_note = "已从 config.json 读取";
}

// ----------------------------------------------------------------------- 读
std::string ConfigStore::GetString(const std::string& key, const std::string& def) const {
    std::lock_guard lock(g_mutex);
    const auto it = g_kv.find(key);
    if (it == g_kv.end()) return def;
    return UnquoteRaw(it->second);
}

bool ConfigStore::GetBool(const std::string& key, bool def) const {
    const std::string v = WideToUtf8(ToLowerAscii(Utf8ToWide(GetString(key, {}))));
    if (v == "true" || v == "1" || v == "yes") return true;
    if (v == "false" || v == "0" || v == "no") return false;
    return def;
}

int ConfigStore::GetInt(const std::string& key, int def) const {
    const std::string v = GetString(key, {});
    if (v.empty()) return def;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) return def;
    return static_cast<int>(n);
}

std::vector<std::string> ConfigStore::GetStringArray(const std::string& key) const {
    std::vector<std::string> out;
    const std::string raw = GetString(key, {});
    if (raw.empty()) return out;
    const std::string trimmed = WideToUtf8(Trim(Utf8ToWide(raw)));
    if (!trimmed.empty() && trimmed.front() == '[') {
        size_t i = 1;
        while (i < trimmed.size()) {
            SkipWs(trimmed, &i);
            if (i >= trimmed.size()) break;
            if (trimmed[i] == ']') break;
            if (trimmed[i] == ',') { ++i; continue; }
            if (trimmed[i] == '"') {
                std::string item;
                if (!ParseJsonString(trimmed, &i, &item)) break;
                out.push_back(item);
                continue;
            }
            const size_t start = i;
            while (i < trimmed.size() && trimmed[i] != ',' && trimmed[i] != ']') ++i;
            out.push_back(WideToUtf8(Trim(Utf8ToWide(trimmed.substr(start, i - start)))));
        }
        return out;
    }
    // 也接受 "a;b;c" / "a,b" / 换行分隔
    std::string cur;
    for (const char c : trimmed) {
        if (c == ';' || c == ',' || c == '\n' || c == '\r') {
            const std::string t = WideToUtf8(Trim(Utf8ToWide(cur)));
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    const std::string t = WideToUtf8(Trim(Utf8ToWide(cur)));
    if (!t.empty()) out.push_back(t);
    return out;
}

bool ConfigStore::Has(const std::string& key) const {
    std::lock_guard lock(g_mutex);
    return g_kv.count(key) != 0;
}

// ----------------------------------------------------------------------- 写
void ConfigStore::SetString(const std::string& key, const std::string& value) {
    std::lock_guard lock(g_mutex);
    if (!g_kv.count(key)) g_order.push_back(key);
    g_kv[key] = "\"" + JsonEscape(value) + "\"";
}

void ConfigStore::SetBool(const std::string& key, bool value) {
    std::lock_guard lock(g_mutex);
    if (!g_kv.count(key)) g_order.push_back(key);
    g_kv[key] = value ? "true" : "false";
}

void ConfigStore::SetInt(const std::string& key, int value) {
    std::lock_guard lock(g_mutex);
    if (!g_kv.count(key)) g_order.push_back(key);
    g_kv[key] = std::to_string(value);
}

void ConfigStore::SetStringArray(const std::string& key, const std::vector<std::string>& values) {
    std::string raw = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) raw += ", ";
        raw += "\"" + JsonEscape(values[i]) + "\"";
    }
    raw += "]";
    std::lock_guard lock(g_mutex);
    if (!g_kv.count(key)) g_order.push_back(key);
    g_kv[key] = raw;
}

bool ConfigStore::Save() {
    std::string body = "{\n";
    {
        std::lock_guard lock(g_mutex);
        for (size_t i = 0; i < g_order.size(); ++i) {
            const auto it = g_kv.find(g_order[i]);
            if (it == g_kv.end()) continue;
            body += "  \"" + JsonEscape(it->first) + "\": " + it->second;
            body += (i + 1 < g_order.size()) ? ",\n" : "\n";
        }
    }
    body += "}\n";

    const std::wstring dir = AppDataDir();
    // CreateDirectoryW 不建父目录 → 逐级创建
    {
        std::wstring cur;
        for (size_t i = 0; i < dir.size(); ++i) {
            cur.push_back(dir[i]);
            if ((dir[i] == L'\\' || dir[i] == L'/') && cur.size() > 3) ::CreateDirectoryW(cur.c_str(), nullptr);
        }
        ::CreateDirectoryW(dir.c_str(), nullptr);
    }
    const std::wstring tmp = ConfigFilePath() + L".tmp";
    {
        UniqueHandle h(::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h.get() == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        if (!::WriteFile(static_cast<HANDLE>(h.get()), body.data(), static_cast<DWORD>(body.size()), &wrote,
                         nullptr) ||
            wrote != static_cast<DWORD>(body.size()))
            return false;
    }
    if (!::MoveFileExW(tmp.c_str(), ConfigFilePath().c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    {
        std::lock_guard lock(g_mutex);
        g_lastLoadMs = ::GetTickCount64();   // 刚刚落盘的内容就是内存内容，避免立刻回读
        g_loaded = true;
    }
    return true;
}

size_t ConfigStore::KeyCount() const {
    std::lock_guard lock(g_mutex);
    return g_kv.size();
}
bool ConfigStore::LastLoadOk() const {
    std::lock_guard lock(g_mutex);
    return g_loadOk;
}
std::string ConfigStore::LastLoadNote() const {
    std::lock_guard lock(g_mutex);
    return g_note;
}
void ConfigStore::ResetForTest() {
    std::lock_guard lock(g_mutex);
    g_kv.clear();
    g_order.clear();
    g_loadOk = true;
    g_note = "reset";
    g_loaded = true;
    g_lastLoadMs = ::GetTickCount64();
}

// --------------------------------------------------------------- 语义化封装
std::string FlagConfigKey(const std::string& commandKey, const std::string& flagKey) {
    return "flags." + commandKey + "." + flagKey;
}

bool ReadFlagOverride(const std::string& commandKey, const std::string& flagKey, bool defaultOn) {
    auto& cfg = ConfigStore::Instance();
    cfg.Reload();
    return cfg.GetBool(FlagConfigKey(commandKey, flagKey), defaultOn);
}

void WriteFlagOverride(const std::string& commandKey, const std::string& flagKey, bool value) {
    auto& cfg = ConfigStore::Instance();
    cfg.Reload();
    cfg.SetBool(FlagConfigKey(commandKey, flagKey), value);
    cfg.Save();
}

}  // namespace grt
