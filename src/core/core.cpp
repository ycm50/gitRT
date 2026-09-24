#include "core.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace grt {

// ============================================================ 错误 / HRESULT
std::string HrToString(HRESULT hr) {
    wchar_t* buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out;
    if (n && buf) {
        out = WideToUtf8(std::wstring_view(buf, n));
        ::LocalFree(buf);
        out = Trim(Utf8ToWide(out)).empty() ? out : WideToUtf8(Trim(Utf8ToWide(out)));
    }
    char hex[32]{};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
    if (out.empty()) out = hex;
    return out;
}

// =================================================================== 字符串
std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ToLowerAscii(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

bool StartsWith(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

std::wstring Format(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wchar_t stackBuf[512];
    int n = _vsnwprintf(stackBuf, 511, fmt, ap);
    va_end(ap);
    if (n < 0) {  // 截断 → 用更大的缓冲重试
        va_start(ap, fmt);
        std::vector<wchar_t> big(4096);
        n = _vsnwprintf(big.data(), big.size() - 1, fmt, ap);
        va_end(ap);
        if (n < 0) n = static_cast<int>(big.size() - 1);
        return std::wstring(big.data(), static_cast<size_t>(n));
    }
    return std::wstring(stackBuf, static_cast<size_t>(n));
}

std::vector<std::wstring> SplitWhitespace(std::string_view s) {
    std::vector<std::wstring> out;
    const std::wstring w = Utf8ToWide(s);
    size_t i = 0;
    while (i < w.size()) {
        while (i < w.size() && (w[i] == L' ' || w[i] == L'\t')) ++i;
        const size_t b = i;
        while (i < w.size() && w[i] != L' ' && w[i] != L'\t') ++i;
        if (i > b) out.push_back(w.substr(b, i - b));
    }
    return out;
}

// ====================================================================== 路径
static bool IsSep(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring NormalizePath(std::wstring p) {
    if (p.empty()) return p;
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        p = L"\\\\" + p.substr(8);
    } else if (p.rfind(L"\\\\?\\", 0) == 0) {
        p = p.substr(4);
    }
    wchar_t buf[32768];
    const DWORD n = ::GetFullPathNameW(p.c_str(), 32768, buf, nullptr);
    std::wstring out = (n > 0 && n < 32768) ? std::wstring(buf, n) : p;
    for (auto& c : out)
        if (c == L'/') c = L'\\';
    // 去掉尾部反斜杠，但保留盘符根 "C:\"
    const bool driveRoot = (out.size() == 3 && out[1] == L':' && out[2] == L'\\');
    if (!driveRoot) {
        while (out.size() > 1 && out.back() == L'\\') out.pop_back();
    }
    return out;
}

bool IsNetworkPath(std::wstring_view p) { return p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\'; }

bool PathExists(std::wstring_view p) {
    if (p.empty()) return false;
    return ::GetFileAttributesW(std::wstring(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool PathIsDirectory(std::wstring_view p) {
    if (p.empty()) return false;
    const DWORD a = ::GetFileAttributesW(std::wstring(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring GetModuleDir() {
    wchar_t buf[32768];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, 32768);
    if (n == 0) return {};
    return ParentOf(std::wstring_view(buf, n));
}

std::wstring JoinPath(std::wstring_view a, std::wstring_view b) {
    if (a.empty()) return std::wstring(b);
    if (b.empty()) return std::wstring(a);
    std::wstring out(a);
    if (!IsSep(out.back())) out.push_back(L'\\');
    out.append(b);
    return out;
}

std::wstring FileNameOf(std::wstring_view p) {
    const size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring_view::npos ? std::wstring(p) : std::wstring(p.substr(pos + 1));
}

std::wstring ParentOf(std::wstring_view p) {
    std::wstring s = NormalizePath(std::wstring(p));
    if (s.empty()) return s;
    // 盘符根（"C:"）本身就是父目录，保持原样
    if (s.size() == 3 && s[1] == L':') return s;
    const size_t pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return s;
    if (pos == 0) return s.substr(0, 1);
    // 形如 "C:\a" 时，父目录是盘符根 "C:\"（注意保留反斜杠）
    if (pos == 2 && s[1] == L':') return s.substr(0, 3);
    return s.substr(0, pos);
}

static std::wstring EnvDir(const wchar_t* var, const wchar_t* leaf) {
    wchar_t buf[32768];
    const DWORD n = ::GetEnvironmentVariableW(var, buf, 32768);
    if (n == 0 || n >= 32768) return {};
    return JoinPath(std::wstring_view(buf, n), leaf);
}

std::wstring AppDataDir() { return EnvDir(L"APPDATA", L"GitRT"); }
std::wstring LocalAppDataDir() { return EnvDir(L"LOCALAPPDATA", L"GitRT"); }
std::wstring ConfigFilePath() { return JoinPath(AppDataDir(), L"config.json"); }

uint64_t HashRoot(std::wstring_view root) {
    const std::wstring lower = ToLowerAscii(std::wstring(root));
    uint64_t h = 1469598103934665603ull;  // FNV offset basis
    for (wchar_t c : lower) {
        const uint16_t u = static_cast<uint16_t>(c);
        h ^= static_cast<uint64_t>(u & 0xFFu);
        h *= 1099511628211ull;
        h ^= static_cast<uint64_t>(u >> 8);
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

std::wstring FindGitExecutable(const std::wstring& configured) {
    // ① 用户在配置里显式指定的路径
    if (!configured.empty() && PathExists(configured)) return NormalizePath(configured);

    // ② PATH（含 git.exe 与 git.cmd 所在的 cmd 目录）
    wchar_t found[32768];
    const DWORD n = ::SearchPathW(nullptr, L"git.exe", nullptr, 32768, found, nullptr);
    if (n > 0 && n < 32768) return NormalizePath(std::wstring(found, n));

    // ③ 常见安装位置（不修改任何系统配置，只做只读探测）
    wchar_t pf[MAX_PATH]{}, pfx86[MAX_PATH]{}, local[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
        const std::wstring p = JoinPath(JoinPath(pf, L"Git"), L"cmd\\git.exe");
        if (PathExists(p)) return NormalizePath(p);
    }
    if (::GetEnvironmentVariableW(L"ProgramFiles(x86)", pfx86, MAX_PATH)) {
        const std::wstring p = JoinPath(JoinPath(pfx86, L"Git"), L"cmd\\git.exe");
        if (PathExists(p)) return NormalizePath(p);
    }
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        const std::wstring p = JoinPath(JoinPath(local, L"Programs\\Git"), L"cmd\\git.exe");
        if (PathExists(p)) return NormalizePath(p);
    }
    return {};
}

// ============================================================ 仓库廉价探测
static std::wstring ReadTextFile(const std::wstring& path, size_t maxBytes) {
    UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return {};
    const DWORD want = static_cast<DWORD>(maxBytes > 1u << 20 ? 1u << 20 : maxBytes);
    std::vector<char> buf(want + 1, 0);
    DWORD got = 0;
    if (!::ReadFile(static_cast<HANDLE>(h.get()), buf.data(), want, &got, nullptr)) return {};
    buf[got] = 0;
    return Utf8ToWide(std::string_view(buf.data(), got));
}

static std::wstring FirstLine(const std::wstring& path, size_t maxBytes) {
    std::wstring s = ReadTextFile(path, maxBytes);
    const size_t pos = s.find_first_of(L"\r\n");
    if (pos != std::wstring::npos) s.resize(pos);
    return Trim(s);
}

std::wstring RepoFlagsToString(uint32_t f) {
    struct Item { uint32_t bit; const wchar_t* name; };
    static const Item kItems[] = {
        {RF_IsRepo, L"repo"}, {RF_Worktree, L"worktree"}, {RF_Submodule, L"submodule"},
        {RF_Merging, L"merging"}, {RF_Rebasing, L"rebasing"}, {RF_RebaseInteractive, L"rebase-i"},
        {RF_CherryPicking, L"cherry-pick"}, {RF_Reverting, L"revert"}, {RF_Bisecting, L"bisect"},
        {RF_IndexLocked, L"index.lock"}, {RF_DetachedHead, L"detached"}, {RF_Shallow, L"shallow"},
        {RF_SparseCheckout, L"sparse"}, {RF_PartialClone, L"partialclone"}, {RF_Network, L"network"},
    };
    std::wstring out;
    for (const auto& it : kItems) {
        if (f & it.bit) {
            if (!out.empty()) out += L",";
            out += it.name;
        }
    }
    return out.empty() ? L"(none)" : out;
}

RepoProbeResult ProbeRepo(const std::wstring& anyPath) {
    RepoProbeResult r;
    std::wstring path = NormalizePath(anyPath);
    if (path.empty()) return r;
    if (IsNetworkPath(path)) r.flags |= RF_Network;
    if (!PathIsDirectory(path)) path = ParentOf(path);

    // 向上寻找 .git（限深 12，见 §4.6）
    for (int depth = 0; depth < 12 && !path.empty(); ++depth) {
        const std::wstring dotGit = JoinPath(path, L".git");
        const DWORD attr = ::GetFileAttributesW(dotGit.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            r.repoRoot = path;
            r.flags |= RF_IsRepo;
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                r.gitDir = dotGit;
            } else {
                // .git 文件（worktree / submodule）：内容为 "gitdir: <path>"
                r.flags |= RF_Worktree;
                const std::wstring line = FirstLine(dotGit, 1024);
                const size_t pos = line.find(L"gitdir:");
                if (pos != std::wstring::npos) {
                    std::wstring g = Trim(line.substr(pos + 7));
                    const bool absolute = (!g.empty() && (g[0] == L'\\' || (g.size() > 1 && g[1] == L':')));
                    if (!absolute) g = JoinPath(path, g);
                    r.gitDir = NormalizePath(g);
                } else {
                    r.gitDir = dotGit;
                }
            }
            break;
        }
        const std::wstring parent = ParentOf(path);
        if (parent == path || parent.empty()) break;
        path = parent;
    }
    if (!r.IsRepo()) return r;

    auto has = [&](const wchar_t* rel) { return PathExists(JoinPath(r.gitDir, rel)); };
    if (has(L"MERGE_HEAD")) r.flags |= RF_Merging;
    if (has(L"rebase-merge") || has(L"rebase-apply")) r.flags |= RF_Rebasing;
    if (has(L"rebase-merge\\interactive")) r.flags |= RF_RebaseInteractive;
    if (has(L"CHERRY_PICK_HEAD")) r.flags |= RF_CherryPicking;
    if (has(L"REVERT_HEAD")) r.flags |= RF_Reverting;
    if (has(L"BISECT_LOG")) r.flags |= RF_Bisecting;
    if (has(L"index.lock")) r.flags |= RF_IndexLocked;
    if (has(L"shallow")) r.flags |= RF_Shallow;
    if (has(L"info\\sparse-checkout")) r.flags |= RF_SparseCheckout;

    const std::wstring head = FirstLine(JoinPath(r.gitDir, L"HEAD"), 256);
    if (!head.empty() && head.rfind(L"ref:", 0) != 0) r.flags |= RF_DetachedHead;

    // config 只读一次（上限 64KB），用于部分克隆/稀疏检出判定
    const std::wstring cfg = ReadTextFile(JoinPath(r.gitDir, L"config"), 65536);
    if (cfg.find(L"partialclone") != std::wstring::npos) r.flags |= RF_PartialClone;
    if (cfg.find(L"sparseCheckout = true") != std::wstring::npos) r.flags |= RF_SparseCheckout;

    return r;
}

// ====================================================================== 日志
namespace {
std::mutex  g_logMutex;
HANDLE      g_logHandle = INVALID_HANDLE_VALUE;
std::wstring g_logPath;
std::wstring g_logDir;
std::string  g_logDay;
LogLevel    g_logLevel = LogLevel::Info;

std::string Today() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// CreateDirectoryW 只创建最后一级；这里逐级创建（%LOCALAPPDATA%\GitRT\logs 需要两级）
void EnsureDirectoryExists(const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if ((dir[i] == L'\\' || dir[i] == L'/') && cur.size() > 3) {
            ::CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    ::CreateDirectoryW(dir.c_str(), nullptr);
}

void OpenLogLocked() {
    const std::string day = Today();
    if (g_logHandle != INVALID_HANDLE_VALUE && day == g_logDay) return;
    if (g_logHandle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_logHandle);
        g_logHandle = INVALID_HANDLE_VALUE;
    }
    EnsureDirectoryExists(g_logDir);
    g_logDay = day;
    g_logPath = JoinPath(g_logDir, W("gitrt-" + day + ".log"));
    g_logHandle = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
}
}  // namespace

void LogInit(const std::wstring& dir, LogLevel minLevel) {
    std::lock_guard lock(g_logMutex);
    g_logDir = dir.empty() ? JoinPath(LocalAppDataDir(), L"logs") : dir;
    g_logLevel = minLevel;
    OpenLogLocked();
}

void LogSetLevel(LogLevel lv) {
    std::lock_guard lock(g_logMutex);
    g_logLevel = lv;
}
LogLevel LogGetLevel() {
    std::lock_guard lock(g_logMutex);
    return g_logLevel;
}
std::wstring LogFilePath() {
    std::lock_guard lock(g_logMutex);
    return g_logPath;
}

void LogWrite(LogLevel lv, const char* category, const std::string& msg) {
    std::lock_guard lock(g_logMutex);
    if (static_cast<int>(lv) > static_cast<int>(g_logLevel)) return;
    if (g_logHandle == INVALID_HANDLE_VALUE) {
        if (g_logDir.empty()) g_logDir = JoinPath(LocalAppDataDir(), L"logs");
        OpenLogLocked();
    }
    if (g_logHandle == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    static const char* kNames[] = {"ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};
    const char* lvName = (static_cast<int>(lv) >= 0 && lv <= LogLevel::Trace)
                             ? kNames[static_cast<int>(lv)]
                             : "?????";
    char head[128]{};
    std::snprintf(head, sizeof(head), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ [tid:%5lu] %s %-9s ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  static_cast<unsigned long>(::GetCurrentThreadId()), lvName,
                  category ? category : "-");
    std::string line(head);
    line += Redact(Utf8ToWide(msg));
    line += "\r\n";
    DWORD written = 0;
    ::WriteFile(g_logHandle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void LogFlush() {
    std::lock_guard lock(g_logMutex);
    if (g_logHandle != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_logHandle);
}

std::string Redact(std::wstring_view text) {
    // 目标：https://user:token@host/... → https://***@host/...
    std::wstring s(text);
    size_t i = 0;
    while ((i = s.find(L"://", i)) != std::wstring::npos) {
        const size_t at = s.find(L'@', i + 3);
        if (at == std::wstring::npos) break;
        const size_t boundary = s.find_first_of(L"/ \t\r\n\"'", i + 3);
        if (boundary != std::wstring::npos && boundary < at) {
            i = boundary;
            continue;
        }
        s.replace(i + 3, at - (i + 3), L"***");
        i += 3;
    }
    return WideToUtf8(s);
}

}  // namespace grt
