// GitRT.Shell.dll 入口（《技术实现设计》§2.5 / §2.8 / §4.8）
//
// 硬约束（§3.7 / §4.8）：
//   · DllMain 内**零逻辑**：不做 COM 注册、不做 I/O、不创建线程、不加锁
//   · DllCanUnloadNow 恒返回 S_FALSE（常驻更稳，避免卸载时序问题）
//   · 崩溃取证只写文件、不弹窗，然后 EXCEPTION_CONTINUE_SEARCH
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "class_factory.h"
#include "clsid.h"
#include "core.h"

namespace {

HINSTANCE g_instance = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

// 崩溃日志（§4.8）：绝不在这里做重活；写不进去就算了
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    wchar_t dir[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
        const std::wstring path =
            std::wstring(dir) + L"\\GitRT\\logs\\shell-crash.log";
        ::CreateDirectoryW((std::wstring(dir) + L"\\GitRT").c_str(), nullptr);
        ::CreateDirectoryW((std::wstring(dir) + L"\\GitRT\\logs").c_str(), nullptr);
        const HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[256]{};
            const int n = std::snprintf(buf, sizeof(buf), "shell crash code=0x%08lX addr=%p\r\n",
                                        info && info->ExceptionRecord
                                            ? static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode)
                                            : 0ul,
                                        info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress
                                                                      : nullptr);
            DWORD wrote = 0;
            ::WriteFile(h, buf, static_cast<DWORD>(n > 0 ? n : 0), &wrote, nullptr);
            ::CloseHandle(h);
        }
    }
    // 只记录，不改动宿主（Explorer）既有的处理链：把决定权交回之前的过滤器
    if (g_prevFilter && g_prevFilter != &CrashFilter) return g_prevFilter(info);
    return EXCEPTION_CONTINUE_SEARCH;   // 交给系统既有的处理链
}

// 只在首次创建 COM 对象时执行：设置日志级别 + 崩溃过滤器。
// 为什么不在 DllMain：那会违反"零逻辑"约束，且可能在持有加载器锁时死锁。
void InitOnce() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        // 菜单构建期不做磁盘 I/O（§3.7）：默认只落 Warn/Error；
        // 开发探针构建提高到 Trace，用于收集真实机器上的耗时分布。
#if defined(GRT_SHELL_PROBE)
        ::grt::LogSetLevel(::grt::LogLevel::Trace);
#else
        ::grt::LogSetLevel(::grt::LogLevel::Warn);
#endif
        // SetUnhandledExceptionFilter 会返回之前的过滤器：保存下来，在崩溃时
        // 记录完日志后把控制权交回去（不破坏宿主进程原有的崩溃处理行为）。
        g_prevFilter = ::SetUnhandledExceptionFilter(&CrashFilter);
    });
}

}  // namespace

// -------------------------------------------------------------------- 入口
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = instance;
        ::DisableThreadLibraryCalls(instance);   // DllMain 里唯一允许的动作
    }
    return TRUE;
}

// ------------------------------------------------------------- COM 导出
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    InitOnce();
    return ::grt::shell::CreateClassFactory(clsid, riid, ppv);
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    // §4.8：常驻更稳。返回 S_FALSE = "不要卸载我"。
    return S_FALSE;
}

#if defined(GRT_DEV_REGISTER)
// 开发期快速迭代用（§2.7 路径 A：走「显示更多选项」的 HKCU 传统注册）。
// ★ 发行版不编译本段：导出表只应有 DllGetClassObject / DllCanUnloadNow。
namespace {
std::wstring ClsidString() {
    wchar_t buf[64]{};
    ::StringFromGUID2(::grt::shell::kClsidGitRTCommand, buf, 64);
    return buf;
}
bool WriteReg(const std::wstring& sub, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                          nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG rc = ::RegSetValueExW(key, name, 0, REG_SZ,
                                     reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}
void DeleteRegTree(const std::wstring& sub) {
    ::RegDeleteTreeW(HKEY_CURRENT_USER, sub.c_str());
}
}  // namespace

extern "C" HRESULT WINAPI DllRegisterServer() {
    wchar_t dll[MAX_PATH]{};
    if (!::GetModuleFileNameW(g_instance, dll, MAX_PATH)) return E_FAIL;
    const std::wstring clsid = ClsidString();
    const std::wstring base = L"Software\\Classes\\CLSID\\" + clsid;
    if (!WriteReg(base, nullptr, L"GitRT Shell 扩展")) return E_FAIL;
    if (!WriteReg(base + L"\\InprocServer32", nullptr, dll)) return E_FAIL;
    if (!WriteReg(base + L"\\InprocServer32", L"ThreadingModel", L"Apartment")) return E_FAIL;

    // 三处 ExplorerCommandHandler（文件夹 / 空白处 / 任意文件）
    const wchar_t* shells[] = {L"Directory", L"Directory\\Background", L"*"};
    for (const wchar_t* sh : shells) {
        const std::wstring sub = std::wstring(L"Software\\Classes\\") + sh + L"\\shell\\GitRTTest";
        if (!WriteReg(sub, L"MUIVerb", L"GitRT（测试）")) return E_FAIL;
        if (!WriteReg(sub, L"ExplorerCommandHandler", clsid)) return E_FAIL;
    }
    return S_OK;
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    const std::wstring clsid = ClsidString();
    DeleteRegTree(L"Software\\Classes\\CLSID\\" + clsid);
    const wchar_t* shells[] = {L"Directory", L"Directory\\Background", L"*"};
    for (const wchar_t* sh : shells)
        DeleteRegTree(std::wstring(L"Software\\Classes\\") + sh + L"\\shell\\GitRTTest");
    return S_OK;
}
#endif  // GRT_DEV_REGISTER
