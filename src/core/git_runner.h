#pragma once
// ---------------------------------------------------------------------------
// git 进程封装（《技术实现设计》§7.1 / §7.2）
//   · Job object + KILL_ON_JOB_CLOSE：保证不留下孤儿 ssh / credential helper
//   · PROC_THREAD_ATTRIBUTE_HANDLE_LIST：只把 3 个 stdio 句柄交给子进程
//   · stdout/stderr 各自独立线程读取：避免管道写满导致死锁
// ---------------------------------------------------------------------------

#include <functional>

#include "core.h"

namespace grt {

struct Invocation {
    std::wstring                            exe;
    std::vector<std::wstring>               argv;   // 不含 exe
    std::wstring                            cwd;
    std::vector<std::pair<std::wstring, std::wstring>> env;  // 值空 = 从环境中删除
    bool                                    captureStdout = true;
    bool                                    captureStderr = true;
    uint32_t                                timeoutMs = 0;   // 0 = 不超时
};

struct RunResult {
    int          exitCode = -1;
    std::string  out;
    std::string  err;
    uint64_t     elapsedMs = 0;
    bool         spawnFailed = false;
    bool         cancelled = false;
    bool         timedOut = false;
    uint32_t     win32Error = 0;
    std::wstring display;
};

// 线程安全；Cancel() 可从任意线程调用
class CancellationToken {
public:
    CancellationToken() : m_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~CancellationToken() { if (m_event) ::CloseHandle(m_event); }
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;

    void Cancel() {
        m_cancelled.store(true);
        if (m_event) ::SetEvent(m_event);
    }
    bool   IsCancelled() const { return m_cancelled.load(); }
    HANDLE Event() const { return m_event; }

private:
    std::atomic<bool> m_cancelled{false};
    HANDLE            m_event = nullptr;
};

class IGitRunner {
public:
    virtual ~IGitRunner() = default;
    // onOut/onErr 在读取线程上被调用，实现必须线程安全且快速返回
    virtual RunResult RunSync(const Invocation& inv,
                              CancellationToken* cancel = nullptr,
                              std::function<void(const char*, size_t)> onOut = {},
                              std::function<void(const char*, size_t)> onErr = {}) = 0;
};

std::unique_ptr<IGitRunner> MakeProcessGitRunner();

// git 统一环境（§7.3）：先删除宿主泄漏变量，再注入必要变量
std::vector<std::pair<std::wstring, std::wstring>> BuildGitEnvironment(
    bool statusOnly, const std::wstring& askPassPath);

// 便捷入口：git + argv
RunResult RunGitSync(const std::wstring& gitExe, const std::vector<std::wstring>& argv,
                     const std::wstring& cwd, uint32_t timeoutMs = 15000);

}  // namespace grt
