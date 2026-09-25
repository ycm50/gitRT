// Invoke 转发实现（《技术实现设计》§4.5 / §5.3）
#include "broker_client.h"

#include "request_section.h"

#include <sddl.h>

namespace grt::shell {
namespace {

constexpr DWORD kPipeConnectTimeoutMs = 50;

// 当前用户的 SID 字符串（用于 \\.\pipe\GitRT.<sid>.v1）
std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD need = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    if (need == 0) {
        ::CloseHandle(token);
        return {};
    }
    std::vector<uint8_t> buf(need);
    std::wstring sid;
    if (::GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
        auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        LPWSTR s = nullptr;
        if (::ConvertSidToStringSidW(tu->User.Sid, &s) && s) {
            sid = s;
            ::LocalFree(s);
        }
    }
    ::CloseHandle(token);
    return sid;
}

// 从 HKCU\Software\GitRT 读一个 REG_SZ 值（install.ps1 会写 ExePath / InstallDir）。
// 用途见 GuiExecutableCandidates()：DLL 来自旧位置、或安装目录被移动时，靠它找回真实的 exe。
std::wstring ReadGitRtRegString(const wchar_t* valueName) {
    wchar_t buf[1024]{};
    DWORD cb = sizeof(buf);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\GitRT", valueName, RRF_RT_REG_SZ, nullptr, buf,
                       &cb) != ERROR_SUCCESS)
        return {};
    return std::wstring(buf);
}

bool TryPipe(const std::wstring& sectionName) {
    const std::wstring pipe = BrokerPipeName();
    if (pipe.empty()) return false;
    if (!::WaitNamedPipeW(pipe.c_str(), kPipeConnectTimeoutMs)) return false;   // Broker 未运行
    UniqueHandle h(::CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return false;
    // §5.4：一行 JSON 信封，真正的载荷在共享内存段里
    const std::string line =
        "{\"op\":\"invoke\",\"section\":\"" + WideToUtf8(sectionName) + "\",\"protoVer\":1}\r\n";
    DWORD wrote = 0;
    const BOOL ok = ::WriteFile(static_cast<HANDLE>(h.get()), line.data(),
                                static_cast<DWORD>(line.size()), &wrote, nullptr);
    if (!ok) {
        GRT_LOGW("shell.ipc", "写管道失败 err=" << ::GetLastError());
        return false;
    }
    GRT_LOGI("shell.ipc", "请求已转交常驻 Broker");
    return true;
}

bool SpawnGui(const std::wstring& sectionName, std::wstring* usedExe, int* triedCount) {
    if (triedCount) *triedCount = 0;
    // 开发/CI 诊断钩子：设置该环境变量时把参数透传给 GUI（写请求报告后退出），
    // 用于自动化验证"右键 → GUI 参数面板"这条链路（见 tools/test-all.ps1）。
    std::wstring probeArgs;
    {
        wchar_t buf[1024]{};
        const DWORD n = ::GetEnvironmentVariableW(L"GITRT_SHELL_REQUEST_PROBE", buf, 1024);
        if (n > 0 && n < 1024) {
            probeArgs = std::wstring(L" --request-probe=\"") + buf + L"\" --exit-after-request";
        }
    }
    for (const auto& exe : GuiExecutableCandidates()) {
        // ★ 每个候选都留痕：以前"候选不存在"是静默 continue，出问题时日志里什么都没有，
        //   只能靠"某行没出现"反推（2026-09-25 那次「既没有常驻进程…」就是这么查的）。
        const bool exists = PathExists(exe);
        GRT_LOGI("shell.ipc", "候选 GUI exe=" << U8(exe) << " 存在=" << (exists ? 1 : 0));
        if (triedCount) ++*triedCount;
        if (!exists) continue;
        std::wstring cmd = L"\"" + exe + L"\" --request-section \"" + sectionName + L"\"" + probeArgs;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // CREATE_NO_WINDOW：GUI 是 -mwindows 子系统程序，这里只是不要再弹控制台
        const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (!ok) {
            GRT_LOGW("shell.ipc", "启动 GUI 失败 exe=" << U8(exe) << " err=" << ::GetLastError());
            continue;
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        if (usedExe) *usedExe = exe;
        GRT_LOGI("shell.ipc", "请求已转交 GUI（直启）exe=" << U8(exe));
        return true;
    }
    return false;
}

}  // namespace

std::wstring BrokerPipeName() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    const std::wstring sid = CurrentUserSid();
    if (sid.empty()) return {};
    cached = L"\\\\.\\pipe\\GitRT." + sid + L".v1";
    return cached;
}

std::vector<std::wstring> GuiExecutableCandidates() {
    std::vector<std::wstring> out;
    auto add = [&](const std::wstring& p) {
        if (p.empty()) return;
        for (const auto& e : out)
            if (ToLowerAscii(e) == ToLowerAscii(p)) return;
        out.push_back(p);
    };
    const std::wstring moduleDir = GetModuleDir();
    // ① 发行版布局：GitRT.exe 与 GitRT.Shell.dll 同在安装目录（ExternalLocation）
    add(JoinPath(moduleDir, L"GitRT.exe"));
    // ② 开发目录布局兜底：build/<cfg>/src/shell/ → build/<cfg>/src/gui/GitRT.exe
    add(NormalizePath(moduleDir + L"\\..\\gui\\GitRT.exe"));
    add(NormalizePath(moduleDir + L"\\..\\..\\GitRT.exe"));
    // ③ 注册表（install.ps1 写的）：DLL 来自旧位置 / 安装目录被移动时，靠它找回真正的 exe
    const std::wstring regExe = ReadGitRtRegString(L"ExePath");
    if (!regExe.empty()) add(regExe);
    const std::wstring regDir = ReadGitRtRegString(L"InstallDir");
    if (!regDir.empty()) add(JoinPath(regDir, L"GitRT.exe"));
    // ④ per-user 安装目录（历史布局，§11.5；可能已被卸载删除，留着只作兜底）
    wchar_t local[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        add(JoinPath(std::wstring(local) + L"\\Programs\\GitRT", L"GitRT.exe"));
    return out;
}

bool ForwardInvoke(const InvokeRequest& req, std::wstring* errorText) {
    auto fail = [&](const std::wstring& why) {
        if (errorText) *errorText = why;
        GRT_LOGE("shell.ipc", "转交失败: " << U8(why));
        return false;
    };

    ShellRequest r;
    r.cmdId = req.cmdId;
    r.selMask = req.selMask;
    r.paths = req.paths;
    r.cwd = req.cwd;
    r.optionsJson = req.optionsJson;

    const std::wstring section = MakeRequestSectionName();
    if (!WriteRequestSection(section, r)) return fail(L"无法写入请求共享内存段");
    if (TryPipe(section)) return true;   // ① 常驻 Broker 优先

    std::wstring used;
    int tried = 0;
    if (SpawnGui(section, &used, &tried)) return true;   // ② 直启 GUI（M0 的实际路径）

    // 候选都落空时把"试过几个位置"说清楚：以前那句"请在 GitRT 里运行自检"在这种情况下
    // 完全误导（自检根本跑不起来），真实原因通常是安装目录被移动/删除或 DLL 来自旧位置。
    return fail(L"既没有常驻进程，也无法启动 GitRT.exe（试过 " + std::to_wstring(tried) +
                L" 个位置都不存在；安装目录可能被移动或删除了，请重新安装 GitRT，"
                L"或先在 GitRT 里运行自检）");
}

}  // namespace grt::shell
