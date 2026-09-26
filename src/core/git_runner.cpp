#include "git_runner.h"

#include <algorithm>
#include <map>
#include <thread>

#include "command_builder.h"   // QuoteArg

namespace grt {

// =============================================================== 环境变量
std::vector<std::pair<std::wstring, std::wstring>> BuildGitEnvironment(bool statusOnly,
                                                                      const std::wstring& askPassPath) {
    std::vector<std::pair<std::wstring, std::wstring>> env;
    // 先"做减法"：删除会污染行为的宿主变量（§7.3）
    static const wchar_t* kRemovals[] = {
        L"GIT_DIR", L"GIT_WORK_TREE", L"GIT_INDEX_FILE", L"GIT_OBJECT_DIRECTORY",
        L"GIT_COMMON_DIR", L"GIT_NAMESPACE", L"GIT_PREFIX", L"GIT_ALTERNATE_OBJECT_DIRECTORIES",
        L"GIT_CONFIG_PARAMETERS", L"GIT_TRACE", L"GIT_TRACE2", L"GIT_TRACE2_EVENT",
        L"GIT_TRACE_PACKET", L"GIT_TRACE_PERFORMANCE", L"GIT_TRACE_SETUP",
    };
    for (const wchar_t* k : kRemovals) env.emplace_back(k, L"");

    // 再"做加法"
    env.emplace_back(L"GIT_TERMINAL_PROMPT", L"0");   // 必需：无终端时不得阻塞
    env.emplace_back(L"GIT_PAGER", L"cat");
    env.emplace_back(L"PAGER", L"cat");
    env.emplace_back(L"LC_ALL", L"C");                // 必需：稳定英文输出，便于错误映射
    env.emplace_back(L"LANG", L"C");
    env.emplace_back(L"GIT_ADVICE", L"0");
    if (statusOnly) env.emplace_back(L"GIT_OPTIONAL_LOCKS", L"0");
    if (!askPassPath.empty()) {
        env.emplace_back(L"GIT_ASKPASS", askPassPath);
        env.emplace_back(L"SSH_ASKPASS", askPassPath);
        env.emplace_back(L"SSH_ASKPASS_REQUIRE", L"force");
    }
    return env;
}

// ============================================================ 环境块构建
namespace {

std::wstring UpperKey(const std::wstring& k) {
    std::wstring out = k;
    for (auto& c : out)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    return out;
}

std::vector<wchar_t> BuildEnvironmentBlock(
    const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    std::map<std::wstring, std::wstring> vars;   // 大写 key → "KEY=VALUE"
    std::vector<std::wstring> specials;          // "=C:" 这类以 = 开头的特殊项

    if (LPWCH cur = ::GetEnvironmentStringsW()) {
        for (LPWCH p = cur; *p;) {
            const std::wstring entry(p);
            p += entry.size() + 1;
            if (entry.empty()) continue;
            const size_t eq = entry.find(L'=');
            if (eq == std::wstring::npos) continue;
            if (eq == 0) {
                specials.push_back(entry);
                continue;
            }
            vars[UpperKey(entry.substr(0, eq))] = entry;
        }
        ::FreeEnvironmentStringsW(cur);
    }

    for (const auto& kv : overrides) {
        const std::wstring key = UpperKey(kv.first);
        if (kv.second.empty()) {
            vars.erase(key);
        } else {
            vars[key] = kv.first + L"=" + kv.second;
        }
    }

    std::vector<wchar_t> block;
    auto append = [&block](const std::wstring& s) {
        block.insert(block.end(), s.begin(), s.end());
        block.push_back(L'\0');
    };
    for (const auto& s : specials) append(s);
    for (const auto& kv : vars) append(kv.second);
    block.push_back(L'\0');
    return block;
}

struct ReaderCtx {
    HANDLE                                  pipe = INVALID_HANDLE_VALUE;
    std::string*                            sink = nullptr;
    std::function<void(const char*, size_t)> cb;
    size_t                                  limit = 32u << 20;   // 单次捕获上限 32MB
    bool                                    truncated = false;
};

void ReaderLoop(ReaderCtx* ctx) {
    char buf[8192];
    DWORD got = 0;
    for (;;) {
        if (!::ReadFile(ctx->pipe, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        if (ctx->sink) {
            const size_t room = ctx->sink->size() < ctx->limit ? ctx->limit - ctx->sink->size() : 0;
            const size_t take = (std::min)(room, static_cast<size_t>(got));
            ctx->sink->append(buf, take);
            if (take < got) ctx->truncated = true;
        }
        if (ctx->cb) ctx->cb(buf, got);
    }
}

bool MakeInheritablePipe(HANDLE* readEnd, HANDLE* writeEnd) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    if (!::CreatePipe(readEnd, writeEnd, &sa, 0)) return false;
    // 父进程持有的读端不继承
    ::SetHandleInformation(*readEnd, HANDLE_FLAG_INHERIT, 0);
    return true;
}

}  // namespace

// ================================================================== 运行
class ProcessGitRunner final : public IGitRunner {
public:
    RunResult RunSync(const Invocation& inv, CancellationToken* cancel,
                      std::function<void(const char*, size_t)> onOut,
                      std::function<void(const char*, size_t)> onErr) override {
        RunResult r;
        const uint64_t t0 = ::GetTickCount64();

        // ---- 命令行 ----
        std::wstring cmdline = QuoteArg(inv.exe);
        for (const auto& a : inv.argv) {
            cmdline += L' ';
            cmdline += QuoteArg(a);
        }
        r.display = cmdline;

        // ---- 环境块 ----
        std::vector<wchar_t> envBlock = BuildEnvironmentBlock(inv.env);

        // ---- 管道 ----
        HANDLE outRd = INVALID_HANDLE_VALUE, outWr = INVALID_HANDLE_VALUE;
        HANDLE errRd = INVALID_HANDLE_VALUE, errWr = INVALID_HANDLE_VALUE;
        if (inv.captureStdout && !MakeInheritablePipe(&outRd, &outWr)) {
            r.spawnFailed = true; r.win32Error = ::GetLastError();
            GRT_LOGE("git", "CreatePipe(stdout) 失败 err=" << r.win32Error);
            return r;
        }
        if (inv.captureStderr && !MakeInheritablePipe(&errRd, &errWr)) {
            r.spawnFailed = true; r.win32Error = ::GetLastError();
            if (outRd != INVALID_HANDLE_VALUE) { ::CloseHandle(outRd); ::CloseHandle(outWr); }
            GRT_LOGE("git", "CreatePipe(stderr) 失败 err=" << r.win32Error);
            return r;
        }
        // stdin → NUL（立即 EOF；配合 GIT_TERMINAL_PROMPT=0 不会挂住）
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE nulIn = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &sa, OPEN_EXISTING, 0, nullptr);

        // ---- 句柄白名单 ----
        HANDLE inherit[3]{};
        DWORD  inheritCount = 0;
        inherit[inheritCount++] = nulIn;
        if (inv.captureStdout) inherit[inheritCount++] = outWr;
        if (inv.captureStderr) inherit[inheritCount++] = errWr;

        SIZE_T attrSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<BYTE> attrBuf(attrSize);
        auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        bool attrOk = ::InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize) &&
                      ::UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                  inherit, sizeof(HANDLE) * inheritCount, nullptr, nullptr);

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nulIn;
        si.StartupInfo.hStdOutput = inv.captureStdout ? outWr : nullptr;
        si.StartupInfo.hStdError = inv.captureStderr ? errWr : nullptr;
        if (attrOk) si.lpAttributeList = attrList;

        // ---- Job object：保证整棵进程树一起消失 ----
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        }

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
        cmdBuf.push_back(L'\0');
        const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
        const BOOL ok = ::CreateProcessW(inv.exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                                         flags, envBlock.data(),
                                         inv.cwd.empty() ? nullptr : inv.cwd.c_str(), &si.StartupInfo, &pi);
        if (!ok) {
            r.spawnFailed = true;
            r.win32Error = ::GetLastError();
            GRT_LOGE("git", "CreateProcess 失败 err=" << r.win32Error << " exe=" << U8(inv.exe));
        }
        if (attrOk) ::DeleteProcThreadAttributeList(attrList);
        if (nulIn != INVALID_HANDLE_VALUE) ::CloseHandle(nulIn);

        if (ok) {
            if (job) ::AssignProcessToJobObject(job, pi.hProcess);
            ::ResumeThread(pi.hThread);
            ::CloseHandle(pi.hThread);
            // 父进程必须关掉子进程侧写端，否则读线程永远等不到 EOF
            if (outWr != INVALID_HANDLE_VALUE) { ::CloseHandle(outWr); outWr = INVALID_HANDLE_VALUE; }
            if (errWr != INVALID_HANDLE_VALUE) { ::CloseHandle(errWr); errWr = INVALID_HANDLE_VALUE; }

            ReaderCtx outCtx{outRd, &r.out, onOut, 32u << 20, false};
            ReaderCtx errCtx{errRd, &r.err, onErr, 32u << 20, false};
            std::thread outThread, errThread;
            if (inv.captureStdout) outThread = std::thread(ReaderLoop, &outCtx);
            if (inv.captureStderr) errThread = std::thread(ReaderLoop, &errCtx);

            HANDLE waits[2]{pi.hProcess, nullptr};
            DWORD  waitCount = 1;
            if (cancel && cancel->Event()) {
                waits[waitCount++] = cancel->Event();
            }
            const DWORD timeout = inv.timeoutMs ? inv.timeoutMs : INFINITE;
            const DWORD w = ::WaitForMultipleObjects(waitCount, waits, FALSE, timeout);
            if (w == WAIT_TIMEOUT) {
                r.timedOut = true;
                if (job) ::TerminateJobObject(job, 124);
            } else if (waitCount > 1 && w == WAIT_OBJECT_0 + 1) {
                r.cancelled = true;
                if (job) ::TerminateJobObject(job, 130);
            }
            ::WaitForSingleObject(pi.hProcess, 5000);
            DWORD code = 0;
            ::GetExitCodeProcess(pi.hProcess, &code);
            r.exitCode = static_cast<int>(code);
            if (r.cancelled) r.exitCode = 130;
            if (r.timedOut) r.exitCode = 124;

            if (outThread.joinable()) outThread.join();
            if (errThread.joinable()) errThread.join();
            ::CloseHandle(pi.hProcess);
        }

        if (outRd != INVALID_HANDLE_VALUE) ::CloseHandle(outRd);
        if (errRd != INVALID_HANDLE_VALUE) ::CloseHandle(errRd);
        if (outWr != INVALID_HANDLE_VALUE) ::CloseHandle(outWr);
        if (errWr != INVALID_HANDLE_VALUE) ::CloseHandle(errWr);
        if (job) ::CloseHandle(job);

        r.elapsedMs = ::GetTickCount64() - t0;
        return r;
    }
};

std::unique_ptr<IGitRunner> MakeProcessGitRunner() { return std::make_unique<ProcessGitRunner>(); }

RunResult RunGitSync(const std::wstring& gitExe, const std::vector<std::wstring>& argv,
                     const std::wstring& cwd, uint32_t timeoutMs) {
    Invocation inv;
    inv.exe = gitExe;
    inv.argv = argv;
    inv.cwd = cwd;
    inv.env = BuildGitEnvironment(true, L"");
    inv.timeoutMs = timeoutMs;
    auto runner = MakeProcessGitRunner();
    return runner->RunSync(inv, nullptr);
}

std::wstring RunGitOut(const std::wstring& gitExe, const std::wstring& repoRoot,
                       const std::vector<std::wstring>& argv, int* exitCode,
                       std::wstring* errOut, uint32_t timeoutMs) {
    const RunResult r = RunGitSync(gitExe, argv, repoRoot, timeoutMs);
    if (exitCode) *exitCode = r.exitCode;
    if (errOut) *errOut = Trim(W(r.err));
    return W(r.out);
}

std::wstring RevParseShort(const std::wstring& gitExe, const std::wstring& repoRoot,
                           const std::wstring& rev, uint32_t timeoutMs) {
    if (gitExe.empty() || repoRoot.empty() || rev.empty()) return {};
    int rc = 0;
    const std::wstring s = Trim(RunGitOut(gitExe, repoRoot, {L"rev-parse", L"--short", rev}, &rc,
                                         nullptr, timeoutMs));
    return rc == 0 ? s : std::wstring();
}

}  // namespace grt
