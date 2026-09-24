#pragma once
// ---------------------------------------------------------------------------
// GitRT 核心基础层
//   · 对应《技术实现设计》§14.1 的 gitrt_core / gitrt_core_tiny 公共部分
//   · 只依赖 Win32 + C++ 标准库；不链接 libgit2、不发起网络请求
//   · 这里的每个函数都允许被 Shell DLL（Explorer UI 线程）调用，
//     因此实现必须遵守 §3.7 的"菜单构建期零 git 调用"约束（没有例外）
// ---------------------------------------------------------------------------

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace grt {

// --------------------------------------------------------------------- 错误
struct HrError {
    HRESULT     hr = E_FAIL;
    std::string where;
};
std::string HrToString(HRESULT hr);
inline void ThrowIfFailed(HRESULT hr, const char* where = "") {
    if (FAILED(hr)) throw HrError{hr, where};
}

// --------------------------------------------------------------- RAII 句柄
struct HandleDeleter {
    void operator()(void* h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(static_cast<HANDLE>(h));
    }
};
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

// ------------------------------------------------------------------- 字符串
std::wstring Utf8ToWide(std::string_view s);
std::string  WideToUtf8(std::wstring_view s);
inline std::string  U8(std::wstring_view s) { return WideToUtf8(s); }
inline std::wstring W(std::string_view s) { return Utf8ToWide(s); }
std::wstring ToLowerAscii(std::wstring s);
bool         StartsWith(std::wstring_view s, std::wstring_view prefix);
bool         EndsWith(std::wstring_view s, std::wstring_view suffix);
std::wstring Trim(std::wstring_view s);
std::wstring Format(const wchar_t* fmt, ...);
std::vector<std::wstring> SplitWhitespace(std::string_view s);

// ------------------------------------------------------------------- 路径
// NormalizePath: 去掉 \\?\ 前缀、折叠 . 与 ..、去掉尾部反斜杠（盘符根除外）
std::wstring NormalizePath(std::wstring p);
bool         IsNetworkPath(std::wstring_view p);
bool         PathExists(std::wstring_view p);
bool         PathIsDirectory(std::wstring_view p);
std::wstring GetModuleDir();
std::wstring JoinPath(std::wstring_view a, std::wstring_view b);
std::wstring FileNameOf(std::wstring_view p);
std::wstring ParentOf(std::wstring_view p);
std::wstring AppDataDir();       // %APPDATA%\GitRT
std::wstring LocalAppDataDir();  // %LOCALAPPDATA%\GitRT
std::wstring ConfigFilePath();   // %APPDATA%\GitRT\config.json
// HashRoot: FNV-1a 64，输入必须是"小写化、无尾部反斜杠"的仓库根路径（DLL/Broker 一致性契约）
uint64_t     HashRoot(std::wstring_view root);
std::wstring FindGitExecutable(const std::wstring& configured = {});

// ------------------------------------------- 仓库廉价探测（菜单期零 git 调用）
enum RepoFlag : uint32_t {
    RF_None              = 0,
    RF_IsRepo            = 1u << 0,
    RF_Worktree          = 1u << 1,   // .git 是文件（gitdir: ...）
    RF_Submodule         = 1u << 2,
    RF_Merging           = 1u << 3,
    RF_Rebasing          = 1u << 4,
    RF_RebaseInteractive = 1u << 5,
    RF_CherryPicking     = 1u << 6,
    RF_Reverting         = 1u << 7,
    RF_Bisecting         = 1u << 8,
    RF_IndexLocked       = 1u << 9,
    RF_DetachedHead      = 1u << 10,
    RF_Shallow           = 1u << 11,
    RF_SparseCheckout    = 1u << 12,
    RF_PartialClone      = 1u << 13,
    RF_Network           = 1u << 14,
};
std::wstring RepoFlagsToString(uint32_t flags);

struct RepoProbeResult {
    uint32_t     flags = RF_None;
    std::wstring repoRoot;
    std::wstring gitDir;
    bool IsRepo() const { return (flags & RF_IsRepo) != 0; }
    bool InProgress() const {
        return (flags & (RF_Merging | RF_Rebasing | RF_CherryPicking | RF_Reverting | RF_Bisecting)) != 0;
    }
};
RepoProbeResult ProbeRepo(const std::wstring& anyPath);

// --------------------------------------------------------------------- 日志
enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };
void         LogInit(const std::wstring& dir = {}, LogLevel minLevel = LogLevel::Info);
void         LogSetLevel(LogLevel lv);
LogLevel     LogGetLevel();
void         LogWrite(LogLevel lv, const char* category, const std::string& msg);
void         LogFlush();
std::wstring LogFilePath();
// Redact: 抹掉 URL 中的 userinfo（user:token@host → ***@host）
std::string  Redact(std::wstring_view text);

#define GRT_LOG_AT(lv, cat, expr)                        \
    do {                                                 \
        std::ostringstream grt_log_os_;                  \
        grt_log_os_ << expr;                             \
        ::grt::LogWrite(lv, cat, grt_log_os_.str());     \
    } while (0)

#define GRT_LOGE(cat, expr) GRT_LOG_AT(::grt::LogLevel::Error, cat, expr)
#define GRT_LOGW(cat, expr) GRT_LOG_AT(::grt::LogLevel::Warn, cat, expr)
#define GRT_LOGI(cat, expr) GRT_LOG_AT(::grt::LogLevel::Info, cat, expr)
#define GRT_LOGD(cat, expr) GRT_LOG_AT(::grt::LogLevel::Debug, cat, expr)
#define GRT_LOGT(cat, expr) GRT_LOG_AT(::grt::LogLevel::Trace, cat, expr)

}  // namespace grt
