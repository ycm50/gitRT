#include "gui.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <cstring>
#include <unordered_map>

namespace grt::gui {

// ================================================================== 全局状态
AppState& App() {
    static AppState s;
    return s;
}

// ====================================================================== 主题
Theme& Th() {
    static Theme t;
    return t;
}

static int g_scaleSample = 96;

int Scale(int px) { return ::MulDiv(px, static_cast<int>(g_scaleSample), 96); }
int ScaleFont(int pt) { return -::MulDiv(pt, static_cast<int>(g_scaleSample), 72); }

bool IsSystemDark() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                        KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD v = 1, size = sizeof(v), type = 0;
    ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&v), &size);
    ::RegCloseKey(key);
    return v == 0;
}

static void DestroyThemeGdi() {
    Theme& t = Th();
    if (t.bgBrush) ::DeleteObject(t.bgBrush);
    if (t.panelBrush) ::DeleteObject(t.panelBrush);
    if (t.editBrush) ::DeleteObject(t.editBrush);
    if (t.fontUi) ::DeleteObject(t.fontUi);
    if (t.fontBold) ::DeleteObject(t.fontBold);
    if (t.fontMono) ::DeleteObject(t.fontMono);
    if (t.fontSmall) ::DeleteObject(t.fontSmall);
    t.bgBrush = t.panelBrush = t.editBrush = nullptr;
    t.fontUi = t.fontBold = t.fontMono = t.fontSmall = nullptr;
}

void ThemeInit(HWND sampleWnd) {
    // DPI
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    UINT dpi = 96;
    if (getDpi && sampleWnd) {
        dpi = getDpi(sampleWnd);
    } else {
        HDC dc = ::GetDC(nullptr);
        if (dc) {
            dpi = static_cast<UINT>(::GetDeviceCaps(dc, LOGPIXELSX));
            ::ReleaseDC(nullptr, dc);
        }
    }
    if (dpi == 0) dpi = 96;
    g_scaleSample = static_cast<int>(dpi);
    App().dpi = dpi;

    Theme& t = Th();
    DestroyThemeGdi();
    t.dark = IsSystemDark();
    if (t.dark) {
        t.bg = RGB(32, 32, 32);
        t.panel = RGB(43, 43, 43);
        t.text = RGB(240, 240, 240);
        t.subText = RGB(170, 170, 170);
        t.border = RGB(70, 70, 70);
        t.editBg = RGB(50, 50, 50);
        t.accent = RGB(76, 160, 235);
    } else {
        t.bg = RGB(246, 246, 246);
        t.panel = RGB(255, 255, 255);
        t.text = RGB(24, 24, 24);
        t.subText = RGB(110, 110, 110);
        t.border = RGB(212, 212, 212);
        t.editBg = RGB(255, 255, 255);
        t.accent = RGB(0, 102, 204);
    }
    t.bgBrush = ::CreateSolidBrush(t.bg);
    t.panelBrush = ::CreateSolidBrush(t.panel);
    t.editBrush = ::CreateSolidBrush(t.editBg);

    const int uiPt = 10, smallPt = 9;
    t.fontUi = ::CreateFontW(ScaleFont(uiPt), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    t.fontBold = ::CreateFontW(ScaleFont(uiPt), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    t.fontSmall = ::CreateFontW(ScaleFont(smallPt), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    t.fontMono = ::CreateFontW(ScaleFont(smallPt), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
}

// ================================================================== 应用图标
// 与右键菜单同一张图：packaging/Assets/gitrt.ico 既打进 GitRT.Shell.dll（右键项用
// ",-101"），也打进 GitRT.exe（资源 101）—— 一致性由构建保证，不靠手工同步。
HICON GitRTAppIcon() {
    static HICON cached = nullptr;
    if (!cached) {
        cached = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr),
                                                 MAKEINTRESOURCEW(IDI_GRT_APP), IMAGE_ICON, 0, 0,
                                                 LR_DEFAULTSIZE | LR_SHARED));
        if (!cached) cached = ::LoadIconW(nullptr, IDI_APPLICATION);
        GRT_LOGI("gui", "应用图标加载 " << (cached ? "OK" : "失败"));
    }
    return cached;
}

namespace {
// ThemeApply 用：把同一个主题套到**子控件**上。
// 为什么要做：`SetWindowTheme(父窗口, …)` 不会传给子控件，而深色下推钮若仍走浅色主题，
// 就会出现"深色窗口上贴一排浅色按钮"（用户反馈的"没边框/不协调"）。
// 只处理 Button；ListView 另有 ThemeApplyToTableView（它还要改背景/文字色）。
BOOL CALLBACK ApplyThemeToChild(HWND child, LPARAM param) {
    const bool dark = param != 0;
    if (!dark) return TRUE;   // 浅色模式：按钮保持系统默认外观（别去动它，免得改掉现有观感）
    wchar_t cls[64]{};
    ::GetClassNameW(child, cls, 63);
    if (::lstrcmpiW(cls, L"Button") == 0) {
        ::SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        ::InvalidateRect(child, nullptr, TRUE);
    }
    return TRUE;
}
}  // namespace

void ThemeApply(HWND hwnd) {
    Theme& t = Th();
    BOOL dark = t.dark ? TRUE : FALSE;
    // Windows 11：沉浸式深色标题栏（20 = DWMWA_USE_IMMERSIVE_DARK_MODE）
    ::DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    // Win10 早期版本用 19
    ::DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    if (t.dark) {
        ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    } else {
        ::SetWindowTheme(hwnd, L"Explorer", nullptr);
    }
    // 推钮在深色模式下也要变深（见上面 ApplyThemeToChild 的说明）
    ::EnumChildWindows(hwnd, ApplyThemeToChild, t.dark ? 1 : 0);
    ::InvalidateRect(hwnd, nullptr, TRUE);
}

// 窗口自身的背景擦除（深色模式下用主题刷填满客户区）。
// 为什么要单独抽出来：`ThemeApply()` 只给窗口设主题，**窗口背景仍由窗口类的画刷决定**；
// 而好几个窗口注册时用的是 `COLOR_WINDOW + 1`（系统**浅色**），于是深色模式下会出现
// "深色控件条（WM_CTLCOLOR* 生效了）+ 浅色窗口背景"的割裂 —— 实测反馈就是"色彩不协调"。
bool HandleEraseBkgnd(HWND hwnd, HDC dc) {
    Theme& t = Th();
    if (!t.dark || !t.bgBrush) return false;
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    ::FillRect(dc, &rc, t.bgBrush);
    return true;
}

// 表格类控件（ListView）跟随主题。**必须逐个控件设**：
//   · ThemeApply() 的 SetWindowTheme 只作用于窗口自己，不会传给子控件；
//   · ListView 的背景/文字色要显式发 LVM_SET*COLOR，表头还是另一个控件、要再套一次主题。
// 以前整个代码库都没做这件事（`ListView_SetBkColor` 一次都没出现），于是深色模式下
// 到处都是"深色窗口里嵌一块亮白表格"。
void ThemeApplyToTableView(HWND listView) {
    if (!listView) return;
    Theme& t = Th();
    if (t.dark) {
        ::SetWindowTheme(listView, L"DarkMode_Explorer", nullptr);
        ::SendMessageW(listView, LVM_SETBKCOLOR, 0, static_cast<LPARAM>(t.editBg));
        ::SendMessageW(listView, LVM_SETTEXTBKCOLOR, 0, static_cast<LPARAM>(t.editBg));
        ::SendMessageW(listView, LVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(t.text));
        if (HWND hdr = reinterpret_cast<HWND>(::SendMessageW(listView, LVM_GETHEADER, 0, 0))) {
            ::SetWindowTheme(hdr, L"DarkMode_Explorer", nullptr);
        }
    } else {
        ::SetWindowTheme(listView, L"Explorer", nullptr);
    }
    ::InvalidateRect(listView, nullptr, TRUE);
}

bool HandleCtlColor(UINT msg, HDC dc, HWND child, LRESULT* result) {
    Theme& t = Th();
    if (!t.dark) return false;
    switch (msg) {
        // ★ 只处理**静态文本**：不处理 WM_CTLCOLORBTN。
        //   原因（用户反馈"按钮看上去不对、几乎看不见文字，点一下才正常"）：
        //   主题化的**推钮会忽略这里返回的画刷**（WM_CTLCOLORBTN 对 BS_PUSHBUTTON 不生效），
        //   但我们在 DC 上设的 `SetTextColor(深色模式的浅色)` 可能被它沿用 →
        //   浅色按钮面 + 浅色文字 = 几乎看不见；一按下去按钮改用自己主题重绘就"正常"了。
        //   复选框/单选钮的标题走的是 WM_CTLCOLORSTATIC，不受影响（系统文档如此）。
        case WM_CTLCOLORSTATIC:
            ::SetTextColor(dc, t.text);
            ::SetBkColor(dc, t.bg);
            if (result) *result = reinterpret_cast<LRESULT>(t.bgBrush);
            return true;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            ::SetTextColor(dc, t.text);
            ::SetBkColor(dc, t.editBg);
            if (result) *result = reinterpret_cast<LRESULT>(t.editBrush);
            return true;
        default:
            break;
    }
    (void)child;
    return false;
}

// ====================================================================== 字符串
const std::wstring& Str(UINT id) {
    static std::unordered_map<UINT, std::wstring> cache;
    const auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    wchar_t buf[1024]{};
    const int n = ::LoadStringW(::GetModuleHandleW(nullptr), id, buf, 1023);
    std::wstring s = (n > 0) ? std::wstring(buf, static_cast<size_t>(n)) : L"?" + std::to_wstring(id);
    return cache.emplace(id, std::move(s)).first->second;
}

// ==================================================================== 控件工具
HWND MakeChild(HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style,
               DWORD exStyle, int id, HFONT font) {
    HWND h = ::CreateWindowExW(exStyle, cls, text.c_str(), style, 0, 0, 10, 10, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               ::GetModuleHandleW(nullptr), nullptr);
    if (h && font) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

void SetText(HWND h, const std::wstring& s) { if (h) ::SetWindowTextW(h, s.c_str()); }

// 多行文本必须喂 CRLF：Win32 的 ES_MULTILINE Edit **不认裸 LF**（\n 直接当没有换行），
// 于是 git 的 LF-only 输出（提交历史/差异/文件历史/进度日志）会被并成一行 —— 实机 bug。
// 规则：把裸 \n 和裸 \r 都补成 \r\n，已是 \r\n 的原样保留（幂等）。
std::wstring ToCrlf(std::wstring s) {
    std::wstring out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'\r') {
            out += L"\r\n";
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;   // 已有 \r\n → 跳过 \n
        } else if (c == L'\n') {
            out += L"\r\n";
        } else {
            out += c;
        }
    }
    return out;
}

void SetTextMl(HWND h, const std::wstring& s) { if (h) SetText(h, ToCrlf(s)); }

std::wstring GetText(HWND h) {
    if (!h) return {};
    const int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    ::GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

void CenterOnOwner(HWND hwnd, HWND owner) {    RECT r{}, ro{};
    ::GetWindowRect(hwnd, &r);
    if (owner && ::IsWindow(owner)) {
        ::GetWindowRect(owner, &ro);
    } else {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &ro, 0);
    }
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = ro.left + ((ro.right - ro.left) - w) / 2;
    const int y = ro.top + ((ro.bottom - ro.top) - h) / 2;
    ::SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) return;
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = ::GlobalLock(mem)) {
            std::memcpy(p, text.c_str(), bytes);
            ::GlobalUnlock(mem);
            ::SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    ::CloseClipboard();
}

// ==================================================================== 业务逻辑
bool RefreshRepoStatus(HWND notify) {    AppState& app = App();
    app.statusLoaded = false;
    app.lastError.clear();
    if (app.repoRoot.empty()) return false;
    if (app.gitExe.empty()) {
        app.gitExe = FindGitExecutable();
        if (app.gitExe.empty()) {
            app.lastError = Str(IDS_MSG_GIT_NOT_FOUND);
            return false;
        }
    }
    const std::vector<std::wstring> argv = {
        L"--no-optional-locks", L"-c", L"core.quotepath=false", L"status",
        L"--porcelain=v2", L"-z", L"--branch", L"--show-stash", L"--untracked-files=normal"};
    const RunResult r = RunGitSync(app.gitExe, argv, app.repoRoot, 15000);
    if (r.spawnFailed) {
        app.lastError = Str(IDS_MSG_GIT_NOT_FOUND);
        GRT_LOGE("gui", "status spawn 失败 err=" << r.win32Error);
        return false;
    }
    if (r.exitCode != 0) {
        app.lastError = W(r.err);
        GRT_LOGW("gui", "status 退出码 " << r.exitCode << " err=" << Redact(W(r.err)));
        return false;
    }
    app.status = ParsePorcelainV2(r.out);
    app.statusLoaded = true;
    GRT_LOGI("gui", "status ok entries=" << app.status.entries.size() << " ms=" << r.elapsedMs
                                         << " staged=" << app.status.staged
                                         << " modified=" << app.status.modified
                                         << " untracked=" << app.status.untracked);
    if (notify) ::PostMessageW(notify, WM_GRT_STATUS_RELOAD, 0, 0);
    return true;
}

std::vector<std::wstring> ListBranches(ParamSource src) {
    AppState& app = App();
    std::vector<std::wstring> out;
    if (app.gitExe.empty() || app.repoRoot.empty()) return out;

    auto add = [&](const std::wstring& fmt) {
        const RunResult r = RunGitSync(app.gitExe, {L"for-each-ref", L"--format=" + fmt, L"refs/heads", L"refs/remotes"},
                                       app.repoRoot, 8000);
        if (r.exitCode != 0) return;
        std::string line;
        for (size_t i = 0; i <= r.out.size(); ++i) {
            if (i == r.out.size() || r.out[i] == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) out.push_back(W(line));
                line.clear();
            } else {
                line.push_back(r.out[i]);
            }
        }
    };

    switch (src) {
        case ParamSource::LocalBranches:
            add(L"%(refname:short)");
            break;
        case ParamSource::AllBranches:
            add(L"%(refname:short)");
            break;
        case ParamSource::RemoteBranches:
            add(L"%(refname:short)");
            break;
        default:
            break;
    }
    if (out.empty() && src == ParamSource::LocalBranches) out.push_back(L"main");
    return out;
}

std::wstring GitRootOf(const std::wstring& gitExe) {
    std::wstring dir = ParentOf(gitExe);            // ...\Git\bin 或 ...\Git\cmd
    std::wstring root = ParentOf(dir);              // ...\Git
    return root;
}

}  // namespace grt::gui
