// ---------------------------------------------------------------------------
// GitRT.ShellProbe.exe —— Shell 扩展的"无资源管理器"验证工具
//
// 为什么需要它（《技术实现设计》§2.7 路径 A 的思路）：
//   "实现错误"与"注册错误"必须彻底分离。现代菜单注册（稀疏包 + 签名 + 身份清单）
//   是最容易**静默失败**的一环；先直接加载 DLL 并把 IExplorerCommand 走一遍，
//   就能证明"实现本身是对的"，再单独去查注册。
//
// 两种模式：
//   --dump <path>          打印该路径（目录或文件）上的完整菜单树 + 状态位 + flags
//   --self-test=<file>     断言集（含内嵌 msix 身份的 §3.4 校验），退出码 0/1
// 其它参数：
//   --dll <path>           GitRT.Shell.dll 路径（默认与本体同目录）
//   --identity <json>      packaging/identity.json（用于校验身份一致性）
//   --appx <xml>           生成的 AppxManifest.xml（同上）
// ---------------------------------------------------------------------------

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "clsid.h"
#include "command_spec.h"
#include "config.h"
#include "core.h"

using namespace grt;

namespace {

int          g_pass = 0;
int          g_fail = 0;
int          g_skip = 0;
std::wstring g_report;

void Report(const std::wstring& line) {
    g_report += line + L"\r\n";
    // 控制台按 UTF-8 输出：fwprintf 在管道/重定向下会把宽字符写成 UTF-16 字节流，
    // 脚本侧读起来就是乱码，因此这里显式转 UTF-8。
    const std::string utf8 = WideToUtf8(line) + "\n";
    fwrite(utf8.data(), 1, utf8.size(), stdout);
    fflush(stdout);
}

void Check(bool ok, const std::wstring& what) {
    Report((ok ? L"[PASS] " : L"[FAIL] ") + what);
    if (ok) ++g_pass;
    else ++g_fail;
}

// 环境受限（例如本会话的文件沙箱不允许向工作区外写盘）时用 SKIP 而不是 FAIL：
// 这类失败与被测代码无关，报成 FAIL 会掩盖真实问题。
void Skip(const std::wstring& what) {
    Report(L"[SKIP] " + what);
    ++g_skip;
}

// 目录是否可写（探测用：写一个临时文件再删）
bool DirWritable(const std::wstring& dir) {
    if (dir.empty()) return false;
    const std::wstring probe = JoinPath(dir, L"gitrt-write-probe.tmp");
    HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    ::DeleteFileW(probe.c_str());
    return true;
}

std::wstring GuidText(REFGUID g) {
    wchar_t buf[64]{};
    ::StringFromGUID2(g, buf, 64);
    return buf;
}

bool ReadFileText(const std::wstring& path, std::string* out) {
    UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return false;
    char buf[8192];
    DWORD got = 0;
    out->clear();
    while (::ReadFile(static_cast<HANDLE>(h.get()), buf, sizeof(buf), &got, nullptr) && got > 0)
        out->append(buf, got);
    return true;
}

// identity.json 是本工程自有的扁平结构，取出一个字符串字段即可
bool JsonField(const std::string& json, const std::string& key, std::string* out) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    const size_t b = json.find('"', p);
    if (b == std::string::npos) return false;
    const size_t e = json.find('"', b + 1);
    if (e == std::string::npos) return false;
    *out = json.substr(b + 1, e - b - 1);
    return true;
}

IShellItemArray* MakeItemArray(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem,
                                             reinterpret_cast<void**>(&item))) ||
        !item)
        return nullptr;
    IShellItemArray* arr = nullptr;
    const HRESULT hr =
        ::SHCreateShellItemArrayFromShellItem(item, IID_IShellItemArray, reinterpret_cast<void**>(&arr));
    item->Release();
    if (FAILED(hr)) return nullptr;
    return arr;
}

std::wstring StateText(EXPCMDSTATE s) {
    std::wstring t;
    const int base = static_cast<int>(s) & 0x3;
    if (base == ECS_ENABLED) t = L"ENABLED";
    else if (base == ECS_DISABLED) t = L"DISABLED";
    else if (base == ECS_HIDDEN) t = L"HIDDEN";
    else t = L"MASK";
    if (s & ECS_CHECKBOX) t += L"|CHECKBOX";
    if (s & ECS_CHECKED) t += L"|CHECKED";
    if (s & ECS_RADIOCHECK) t += L"|RADIOCHECK";
    return t;
}

std::wstring FlagsText(EXPCMDFLAGS f) {
    if (f == ECF_DEFAULT) return L"DEFAULT";
    std::wstring t;
    auto add = [&](bool on, const wchar_t* n) {
        if (!on) return;
        if (!t.empty()) t += L"|";
        t += n;
    };
    add((f & ECF_HASSUBCOMMANDS) != 0, L"HASSUBCOMMANDS");
    add((f & ECF_ISSEPARATOR) != 0, L"ISSEPARATOR");
    add((f & ECF_TOGGLEABLE) != 0, L"TOGGLEABLE");
    return t.empty() ? L"DEFAULT" : t;
}

std::wstring TitleOf(IExplorerCommand* cmd, IShellItemArray* items) {
    LPWSTR t = nullptr;
    const HRESULT hr = cmd->GetTitle(items, &t);
    std::wstring title = (SUCCEEDED(hr) && t) ? t : L"";
    if (t) ::CoTaskMemFree(t);
    return title;
}

enum class FindMode { ByExactTitle, ByTitlePart, FirstToggleable };

// 递归查找：命中返回**已持有引用**的节点（调用方负责 Release），未命中返回 nullptr
IExplorerCommand* FindNode(IExplorerCommand* cmd, IShellItemArray* items, FindMode mode,
                           const std::wstring& needle) {
    IEnumExplorerCommand* en = nullptr;
    if (FAILED(cmd->EnumSubCommands(&en)) || !en) return nullptr;
    IExplorerCommand* child = nullptr;
    ULONG fetched = 0;
    IExplorerCommand* hit = nullptr;
    while (!hit && en->Next(1, &child, &fetched) == S_OK && fetched == 1 && child) {
        const std::wstring title = TitleOf(child, items);
        EXPCMDFLAGS flags = ECF_DEFAULT;
        child->GetFlags(&flags);
        const bool match =
            (mode == FindMode::ByExactTitle && title == needle) ||
            (mode == FindMode::ByTitlePart && !needle.empty() && title.find(needle) != std::wstring::npos) ||
            (mode == FindMode::FirstToggleable && (flags & ECF_TOGGLEABLE) != 0);
        if (match) {
            hit = child;   // 保留引用
        } else {
            hit = FindNode(child, items, mode, needle);
            child->Release();
        }
        child = nullptr;
    }
    en->Release();
    return hit;
}

// 枚举根的直接子项（调用方负责 Release 每项）
std::vector<IExplorerCommand*> ChildrenOf(IExplorerCommand* cmd) {
    std::vector<IExplorerCommand*> out;
    IEnumExplorerCommand* en = nullptr;
    if (FAILED(cmd->EnumSubCommands(&en)) || !en) return out;
    IExplorerCommand* child = nullptr;
    ULONG fetched = 0;
    while (en->Next(1, &child, &fetched) == S_OK && fetched == 1 && child) {
        out.push_back(child);
        child = nullptr;
    }
    en->Release();
    return out;
}

struct DumpStats {
    int total = 0, separators = 0, emptyTitle = 0, subs = 0, maxDepth = 0;
};

void DumpCommand(IExplorerCommand* cmd, IShellItemArray* items, int depth, DumpStats* st) {
    const std::wstring title = TitleOf(cmd, items);
    EXPCMDFLAGS flags = ECF_DEFAULT;
    cmd->GetFlags(&flags);
    EXPCMDSTATE state = ECS_ENABLED;
    cmd->GetState(items, FALSE, &state);

    ++st->total;
    if (flags & ECF_ISSEPARATOR) ++st->separators;
    if (title.empty()) ++st->emptyTitle;
    if (depth > st->maxDepth) st->maxDepth = depth;

    std::wstring line(static_cast<size_t>(depth) * 2, L' ');
    line += L"[" + StateText(state) + L"] {" + FlagsText(flags) + L"} ";
    line += title.empty() ? L"(分隔符)" : title;
    Report(line);

    IEnumExplorerCommand* en = nullptr;
    if (SUCCEEDED(cmd->EnumSubCommands(&en)) && en) {
        ++st->subs;
        IExplorerCommand* child = nullptr;
        ULONG fetched = 0;
        while (en->Next(1, &child, &fetched) == S_OK && fetched == 1 && child) {
            DumpCommand(child, items, depth + 1, st);
            child->Release();
            child = nullptr;
        }
        en->Release();
    }
}

// 静默遍历（与 DumpCommand 调用的方法完全相同，只是不打印）：用于性能度量
void SilentWalk(IExplorerCommand* cmd, IShellItemArray* items, int* count) {
    LPWSTR t = nullptr;
    if (SUCCEEDED(cmd->GetTitle(items, &t)) && t) ::CoTaskMemFree(t);
    EXPCMDFLAGS flags = ECF_DEFAULT;
    cmd->GetFlags(&flags);
    EXPCMDSTATE state = ECS_ENABLED;
    cmd->GetState(items, FALSE, &state);
    ++*count;
    IEnumExplorerCommand* en = nullptr;
    if (SUCCEEDED(cmd->EnumSubCommands(&en)) && en) {
        IExplorerCommand* child = nullptr;
        ULONG fetched = 0;
        while (en->Next(1, &child, &fetched) == S_OK && fetched == 1 && child) {
            SilentWalk(child, items, count);
            child->Release();
            child = nullptr;
        }
        en->Release();
    }
}

double QpcMs(LARGE_INTEGER a, LARGE_INTEGER b, long long freq) {
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(freq);
}

// ------------------------------------------------------- 系统 COM 激活（跨进程）
// 与资源管理器**完全相同**的取用路径：CoCreateInstance(CLSID) → 包注册 → DllHost 代理
// → 返回的 IExplorerCommand 是**跨进程封送的代理**。进程内 DllGetClassObject 直连
// 会掩盖封送层的问题，所以这一路必须单独验证。
IExplorerCommand* ActivateViaSystemCom(std::wstring* why) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IExplorerCommand* cmd = nullptr;
    const HRESULT hr = ::CoCreateInstance(shell::kClsidGitRTCommand, nullptr, CLSCTX_ALL,
                                          shell::kIidIExplorerCommand,
                                          reinterpret_cast<void**>(&cmd));
    if (FAILED(hr) || !cmd) {
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"0x%08lX", static_cast<unsigned long>(hr));
        if (why) *why = std::wstring(L"CoCreateInstance 失败 hr=") + buf;
        return nullptr;
    }
    return cmd;
}

// --------------------------------------------- ② 外壳自身的菜单聚合（可脚本化复现）
// 用 IShellFolder::GetUIObjectOf + IContextMenu::QueryContextMenu 让**外壳自己**
// 把我们的扩展挂进菜单（与资源管理器构建菜单用的是同一套处理器聚合），然后把
// HMENU 结构打印出来。价值：不用人肉右键就能复现"菜单里到底有什么"。
void DumpHMenu(HMENU menu, int depth, int* total) {
    const int count = ::GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        wchar_t text[512]{};
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE;
        mii.dwTypeData = text;
        mii.cch = 511;
        if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) continue;
        ++*total;
        std::wstring line(static_cast<size_t>(depth) * 2, L' ');
        const bool sep = (mii.fType & MFT_SEPARATOR) != 0;
        const bool sub = mii.hSubMenu != nullptr;
        line += sep ? L"---- 分隔符 ----" : std::wstring(text);
        if (!sep) {
            line += L"  [id=" + std::to_wstring(mii.wID) + L"]";
            if (sub) line += L" {有子菜单}";
            if (mii.fState & MFS_CHECKED) line += L" {已勾选}";
            if (mii.fState & MFS_DISABLED) line += L" {禁用}";
            if (mii.fState & MFS_GRAYED) line += L" {灰显}";
        }
        Report(line);
        if (sub) DumpHMenu(mii.hSubMenu, depth + 1, total);
    }
}

int DumpShellMenu(const std::wstring& path) {
    Report(L"== 外壳自身的菜单聚合（IContextMenu::QueryContextMenu）==");
    Report(L"路径：" + NormalizePath(path));
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT hrParse = ::SHParseDisplayName(NormalizePath(path).c_str(), nullptr, &pidl, 0, nullptr);
    Check(SUCCEEDED(hrParse) && pidl != nullptr, L"SHParseDisplayName");
    if (!pidl) return 1;

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    const HRESULT hrBind =
        ::SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child);
    Check(SUCCEEDED(hrBind) && parent != nullptr, L"SHBindToParent → IShellFolder");
    if (!parent) {
        ::CoTaskMemFree(pidl);
        return 1;
    }

    IContextMenu* cm = nullptr;
    const HRESULT hrCtx = parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr,
                                                reinterpret_cast<void**>(&cm));
    Check(SUCCEEDED(hrCtx) && cm != nullptr, L"IShellFolder::GetUIObjectOf(IID_IContextMenu)");
    if (cm) {
        HMENU menu = ::CreatePopupMenu();
        const HRESULT hrQ = cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
        Check(SUCCEEDED(hrQ), L"IContextMenu::QueryContextMenu");
        int total = 0;
        Report(L"---- 外壳构建出的菜单 ----");
        DumpHMenu(menu, 0, &total);
        Report(L"---- 合计 " + std::to_wstring(total) + L" 项 ----");
        Check(total > 0, L"外壳菜单非空");
        ::DestroyMenu(menu);
        cm->Release();
    }
    parent->Release();
    ::CoTaskMemFree(pidl);
    ::CoUninitialize();
    Report(L"");
    Report(L"== 结果: " + std::to_wstring(g_pass) + L" 通过 / " + std::to_wstring(g_fail) + L" 失败 ==");
    return g_fail == 0 ? 0 : 1;
}

std::wstring ResString(HMODULE mod, UINT id) {
    wchar_t buf[512]{};
    const int n = ::LoadStringW(mod, id, buf, 511);
    return n > 0 ? std::wstring(buf, static_cast<size_t>(n)) : std::wstring();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring dllPath, dumpPath, selfTestPath, identityPath, appxPath, workDirArg;
    std::wstring invokeKey, invokePath, invokeReport;
    bool         activateMode = false;
    bool         shellMenuMode = false;
    bool         nullArrayMode = false;
    auto next = [&](int* i) -> std::wstring { return (*i + 1 < argc) ? argv[++(*i)] : std::wstring(); };
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--dll") dllPath = next(&i);
        else if (a == L"--dump") dumpPath = next(&i);
        else if (a == L"--workdir") workDirArg = next(&i);
        else if (a == L"--identity") identityPath = next(&i);
        else if (a == L"--appx") appxPath = next(&i);
        else if (a == L"--invoke") invokeKey = next(&i);
        else if (a == L"--activate") activateMode = true;
        else if (a == L"--shell-menu") shellMenuMode = true;
        else if (a == L"--null-array") nullArrayMode = true;
        else if (a == L"--invoke-path") invokePath = next(&i);
        else if (a == L"--invoke-report") invokeReport = next(&i);
        else if (a.rfind(L"--self-test", 0) == 0) {
            const size_t eq = a.find(L'=');
            selfTestPath = (eq == std::wstring::npos) ? std::wstring(L"shellprobe.txt") : a.substr(eq + 1);
        }
    }

    // --------------------------------- ② 外壳自身的菜单聚合（自动复现，先于 LoadLibrary）
    if (shellMenuMode) {
        return DumpShellMenu(dumpPath.empty() ? GetModuleDir() : dumpPath);
    }

    // --------------------------------------------- ① 系统 COM 激活模式（跨进程）
    // 这一路**不需要** LoadLibrary：完全走系统 COM（包注册 → dllhost 代理 → 封送），
    // 与资源管理器的取用方式一致。放在最前面，避免"进程内直连"掩盖封送层问题。
    if (activateMode) {
        const std::wstring path = dumpPath.empty() ? GetModuleDir() : NormalizePath(dumpPath);
        Report(L"== 系统 COM 激活（跨进程封送）==");
        std::wstring why;
        IExplorerCommand* root = ActivateViaSystemCom(&why);
        Check(root != nullptr, L"CoCreateInstance({CLSID}) 成功" + (root ? std::wstring() : L" —— " + why));
        if (root) {
            IShellItemArray* items = MakeItemArray(path);
            Check(items != nullptr, L"构造 IShellItemArray（" + path + L"）");
            if (items && nullArrayMode) {
                // ★ 模拟资源管理器现代菜单的真实调用形态：
                //   外壳先用**真实数组**问一次根项（我们借此记住选区），随后对整棵树
                //   的所有节点都以 nullptr 调用 GetTitle/GetState。修复前这种形态会让
                //   全部子项变成 HIDDEN（子菜单空白）。
                LPWSTR t = nullptr;
                root->GetTitle(items, &t);
                if (t) ::CoTaskMemFree(t);
                EXPCMDSTATE s = ECS_ENABLED;
                root->GetState(items, FALSE, &s);
                Report(L"（已用真实数组预热根项，随后全部以 nullptr 调用）");
            }
            if (items) {
                IShellItemArray* walkItems = nullArrayMode ? nullptr : items;
                DumpStats st;
                Report(L"---- 经受封送的菜单树（" + path + L"）----");
                DumpCommand(root, walkItems, 0, &st);
                Report(L"---- 合计 " + std::to_wstring(st.total) + L" 项；分隔符 " +
                       std::to_wstring(st.separators) + L"；最大深度 " + std::to_wstring(st.maxDepth) +
                       L" ----");
                if (nullArrayMode) {
                    // 逐项统计可见性：修复后应看到大量 ENABLED（而不是全 HIDDEN）
                    int hidden = 0, enabled = 0;
                    std::wstring report = g_report;
                    size_t at = 0;
                    while ((at = report.find(L"[HIDDEN]", at)) != std::wstring::npos) {
                        ++hidden;
                        at += 8;
                    }
                    at = 0;
                    while ((at = report.find(L"[ENABLED]", at)) != std::wstring::npos) {
                        ++enabled;
                        at += 9;
                    }
                    Report(L"以 nullptr 遍历：ENABLED=" + std::to_wstring(enabled) + L" HIDDEN=" +
                           std::to_wstring(hidden));
                    Check(enabled > 20, L"子项在 nullptr 形态下仍可见（粘性选区/乐观显示生效）");
                }
                const auto kids = ChildrenOf(root);
                Report(L"L1 直接子项数=" + std::to_wstring(kids.size()));
                int l2Total = 0;
                for (auto* k : kids) {
                    LPWSTR t = nullptr;
                    k->GetTitle(items, &t);
                    const std::wstring title = t ? t : L"";
                    if (t) ::CoTaskMemFree(t);
                    IEnumExplorerCommand* en = nullptr;
                    const HRESULT hr = k->EnumSubCommands(&en);
                    int n = 0;
                    if (SUCCEEDED(hr) && en) {
                        IExplorerCommand* c = nullptr;
                        ULONG f = 0;
                        while (en->Next(1, &c, &f) == S_OK && f == 1 && c) {
                            ++n;
                            c->Release();
                            c = nullptr;
                        }
                        en->Release();
                    }
                    l2Total += n;
                    if (n > 0 || FAILED(hr)) {
                        wchar_t buf[32]{};
                        std::swprintf(buf, 32, L"0x%08lX", static_cast<unsigned long>(hr));
                        Report(L"  「" + title + L"」子项=" + std::to_wstring(n) +
                               L" (EnumSubCommands hr=" + buf + L")");
                    }
                    k->Release();
                }
                Check(l2Total > 0,
                      L"经受封送后 L2 仍能枚举到子项（合计 " + std::to_wstring(l2Total) + L"）");
                items->Release();
            }
            root->Release();
        }
        ::CoUninitialize();
        Report(L"");
        Report(L"== 结果: " + std::to_wstring(g_pass) + L" 通过 / " + std::to_wstring(g_fail) + L" 失败 ==");
        if (!selfTestPath.empty()) {
            const std::string utf8 = WideToUtf8(g_report);
            UniqueHandle h(::CreateFileW(selfTestPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
            if (h.get() != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                ::WriteFile(static_cast<HANDLE>(h.get()), utf8.data(), static_cast<DWORD>(utf8.size()),
                            &wrote, nullptr);
            }
        }
        return g_fail == 0 ? 0 : 1;
    }

    if (dllPath.empty()) dllPath = JoinPath(GetModuleDir(), L"GitRT.Shell.dll");
    HMODULE mod = ::LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        Report(L"[FATAL] 无法加载 " + dllPath + L" err=" + std::to_wstring(::GetLastError()));
        return 2;
    }
    auto getClassObject = reinterpret_cast<decltype(&DllGetClassObject)>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "DllGetClassObject")));
    if (!getClassObject) {
        Report(L"[FATAL] 找不到 DllGetClassObject（导出表被裁剪？应该只有 2 个导出）");
        ::FreeLibrary(mod);
        return 2;
    }
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Report(L"GitRT.ShellProbe —— " + dllPath);

    auto makeRoot = [&](IExplorerCommand** out) -> bool {
        IClassFactory* f = nullptr;
        if (FAILED(getClassObject(shell::kClsidGitRTCommand, IID_IClassFactory,
                                  reinterpret_cast<void**>(&f))) ||
            !f)
            return false;
        const HRESULT hr = f->CreateInstance(nullptr, IID_IExplorerCommand, reinterpret_cast<void**>(out));
        f->Release();
        return SUCCEEDED(hr) && *out != nullptr;
    };

    // ------------------------------------------------------------- dump 模式
    if (!dumpPath.empty()) {
        IShellItemArray* items = MakeItemArray(NormalizePath(dumpPath));
        if (!items) {
            Report(L"[FATAL] 无法把路径转成 IShellItemArray: " + dumpPath);
            return 3;
        }
        IExplorerCommand* root = nullptr;
        if (!makeRoot(&root)) {
            Report(L"[FATAL] 无法创建根命令对象");
            items->Release();
            return 3;
        }
        DumpStats st;
        Report(L"---- 菜单树（" + NormalizePath(dumpPath) + L"）----");
        DumpCommand(root, items, 0, &st);
        Report(L"---- 合计 " + std::to_wstring(st.total) + L" 项；分隔符 " +
               std::to_wstring(st.separators) + L"；空标题 " + std::to_wstring(st.emptyTitle) +
               L"；最大深度 " + std::to_wstring(st.maxDepth) + L" ----");
        root->Release();
        items->Release();
        if (selfTestPath.empty()) {
            const std::string utf8 = WideToUtf8(g_report);
            UniqueHandle h(::CreateFileW((GetModuleDir() + L"\\shellprobe-dump.txt").c_str(), GENERIC_WRITE,
                                         0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (h.get() != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                ::WriteFile(static_cast<HANDLE>(h.get()), utf8.data(), static_cast<DWORD>(utf8.size()),
                            &wrote, nullptr);
            }
            ::CoUninitialize();
            ::FreeLibrary(mod);
            return 0;
        }
        Report(L"");
    }

    // ------------------------------------------------- ③ invoke 端到端模式
    // 验证"右键菜单 → GUI 参数面板"整条链路（§4.5）：
    //   ① 通过 IExplorerCommand::Invoke 触发（和资源管理器点菜单完全相同）
    //   ② DLL 写出请求段并直启 GitRT.exe --request-section
    //   ③ GUI 应用请求后（由 GITRT_SHELL_REQUEST_PROBE 钩子传参）写报告并退出
    //   ④ 本工具读报告断言命令 key / 选区 / flags 都正确落到参数面板
    if (!invokeKey.empty()) {
        const std::wstring path = invokePath.empty() ? (workDirArg.empty() ? GetModuleDir() : workDirArg)
                                                     : invokePath;
        const std::wstring report = invokeReport.empty() ? JoinPath(GetModuleDir(), L"shellrequest-report.txt")
                                                         : invokeReport;
        IShellItemArray* items = MakeItemArray(NormalizePath(path));
        IExplorerCommand* r = nullptr;
        if (!items || !makeRoot(&r)) {
            Report(L"[FATAL] invoke 模式无法准备上下文（path=" + path + L"）");
            return 3;
        }
        // --invoke app.main|root ：直接点**根节点自身**（App 形态下菜单只有这一个入口）
        const bool rootInvoke = (invokeKey == L"app.main" || invokeKey == L"root");
        const CommandSpec* spec = rootInvoke ? nullptr : FindCommandByKey(WideToUtf8(invokeKey));
        if (!rootInvoke && !spec) {
            Report(L"[FATAL] 命令表里没有 key=" + invokeKey);
            return 3;
        }
        const std::wstring wantTitle = rootInvoke ? std::wstring(L"(根节点 GitRT)") : ResString(mod, spec->titleRes);
        IExplorerCommand* node = rootInvoke ? r : FindNode(r, items, FindMode::ByExactTitle, wantTitle);
        if (!node) {
            Report(L"[FATAL] 菜单树里没有标题为「" + wantTitle + L"」的命令项");
            return 3;
        }
        ::DeleteFileW(report.c_str());
        ::SetEnvironmentVariableW(L"GITRT_SHELL_REQUEST_PROBE", report.c_str());
        Report(L"invoke：" + invokeKey + L"（" + wantTitle + L"）→ 选区 " + NormalizePath(path));
        const HRESULT hr = node->Invoke(items, nullptr);
        ::SetEnvironmentVariableW(L"GITRT_SHELL_REQUEST_PROBE", nullptr);

        std::string text;
        bool got = false;
        for (int i = 0; i < 150 && !got; ++i) {   // 最多等 15 s（GUI 冷启动 + 落报告）
            ::Sleep(100);
            got = ::ReadFileText(report, &text) && !text.empty();
        }
        Check(SUCCEEDED(hr), L"Invoke 返回 S_OK");
        Check(got, L"GUI 写回了请求报告（" + report + L"）");
        if (got) {
            Check(text.find("applied=1") != std::string::npos, L"GUI 已应用请求（applied=1）");
            Check(text.find("cmdKey=" + WideToUtf8(rootInvoke ? std::wstring(L"app.main") : invokeKey)) !=
                      std::string::npos,
                  L"报告里的命令 key 与菜单点击一致（cmdKey=" +
                      (rootInvoke ? std::wstring(L"app.main") : invokeKey) + L"）");
            Check(text.find("pathCount=0") == std::string::npos, L"选区路径已传入参数面板");
            if (!rootInvoke)
                Check(text.find("flag=") != std::string::npos,
                      L"flags 覆盖已传入参数面板（config.json 的复选状态随菜单一起走）");
            else
                Check(text.find("flagCount=0") != std::string::npos,
                      L"入口请求不带命令，只把仓库上下文交给主窗口（flagCount=0）");
            Report(L"---- 报告 ----");
            {
                std::string line;
                for (const char c : text) {
                    if (c == '\n') {
                        Report(W(line));
                        line.clear();
                    } else if (c != '\r') {
                        line.push_back(c);
                    }
                }
                if (!line.empty()) Report(W(line));
            }
        }
        node->Release();
        r->Release();
        items->Release();
        Report(L"");
        Report(L"== invoke 结果: " + std::to_wstring(g_pass) + L" 通过 / " + std::to_wstring(g_fail) +
               L" 失败 ==");
        ::CoUninitialize();
        ::FreeLibrary(mod);
        return g_fail == 0 ? 0 : 1;
    }

    // --------------------------------------------------------- self-test 模式
    Report(L"== GitRT Shell 扩展自检 ==");
    Report(L"命令表条目: " + std::to_wstring(CommandTableSize()));

    // ---- ① 入口与工厂 ----
    {
        IClassFactory* f = nullptr;
        const GUID bogus = {0x11111111, 0x2222, 0x3333, {0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44}};
        const HRESULT hr = getClassObject(bogus, IID_IClassFactory, reinterpret_cast<void**>(&f));
        Check(hr == CLASS_E_CLASSNOTAVAILABLE && !f,
              L"DllGetClassObject：未知 CLSID → CLASS_E_CLASSNOTAVAILABLE");
    }
    {
        IClassFactory* f = nullptr;
        Check(SUCCEEDED(getClassObject(shell::kClsidGitRTCommand, IID_IClassFactory,
                                       reinterpret_cast<void**>(&f))) &&
                  f != nullptr,
              L"DllGetClassObject：本产品 CLSID → IClassFactory");
        if (f) f->Release();
    }
    IExplorerCommand* root = nullptr;
    Check(makeRoot(&root), L"ClassFactory::CreateInstance → IExplorerCommand");
    if (root) {
        IUnknown* unk = nullptr;
        const HRESULT hr = root->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unk));
        Check(SUCCEEDED(hr) && unk != nullptr, L"QueryInterface(IID_IUnknown) 成功");
        if (unk) unk->Release();
        IUnknown* none = nullptr;
        const GUID bogus = {0x11111111, 0x2222, 0x3333, {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55}};
        const HRESULT hr2 = root->QueryInterface(bogus, reinterpret_cast<void**>(&none));
        Check(hr2 == E_NOINTERFACE && !none, L"QueryInterface：未知 IID → E_NOINTERFACE");

        GUID canonical{};
        Check(SUCCEEDED(root->GetCanonicalName(&canonical)) &&
                  ::IsEqualGUID(canonical, shell::kGuidGitRTCommandCanonical),
              L"GetCanonicalName == 规范 GUID（" + GuidText(shell::kGuidGitRTCommandCanonical) + L"）");

        EXPCMDFLAGS flags = ECF_DEFAULT;
        root->GetFlags(&flags);
        // 形态一半一半：App = 单一入口（无子菜单）；Tree = 有子菜单。
        // 这里只断言"两者自洽"，具体形态由 config 的 menu.mode 决定。
        const bool hasSub = (flags & ECF_HASSUBCOMMANDS) != 0;
        IEnumExplorerCommand* probeEnum = nullptr;
        const HRESULT hrEnum = root->EnumSubCommands(&probeEnum);
        if (probeEnum) probeEnum->Release();
        Report(L"菜单形态自检：ECF_HASSUBCOMMANDS=" + std::to_wstring(hasSub ? 1 : 0) +
               L"，EnumSubCommands hr=0x" + [&] {
                   wchar_t b[16]{};
                   std::swprintf(b, 16, L"%08lX", static_cast<unsigned long>(hrEnum));
                   return std::wstring(b);
               }());
        Check(hasSub == SUCCEEDED(hrEnum) || (!hasSub && hrEnum == E_NOTIMPL),
              L"根节点标志与子菜单能力自洽（有 HASSUBCOMMANDS ⇔ EnumSubCommands 成功）");

        const std::wstring title = TitleOf(root, nullptr);
        Check(!title.empty() && title == ResString(mod, IDS_MENU_ROOT),
              L"根节点 GetTitle == 资源串 IDS_MENU_ROOT（" + title + L"）");
    }

    // ---- ② 真实目录 / 文件的选择上下文 ----
    // 探针夹具放在工作目录（--workdir，默认 = 本程序同目录）下：
    //   某些受限会话不允许向工作区外写盘（%TEMP% 在区外），夹具必须落在可写位置。
    // 夹具目录一律规范化：--workdir 可能来自 CMake 变量（正斜杠），而 Shell 的
    // IShellItem 解析器（SHCreateItemFromParsingName）对混合分隔符很敏感。
    const std::wstring workDir =
        NormalizePath(workDirArg.empty() ? GetModuleDir() : workDirArg);
    const std::wstring fixture = JoinPath(workDir, L"shellprobe-tmp");
    const std::wstring fakeRepo = fixture + L"\\repo";
    const std::wstring filePath = fixture + L"\\a.txt";
    ::CreateDirectoryW(fixture.c_str(), nullptr);

    // "非仓库"夹具必须真的不在任何仓库里（否则 requiresRepo 裁剪无从验证）。
    // 优先用 %TEMP% / %USERPROFILE%（只读取路径，不需要写权限），最后才用工作目录。
    auto pickNonRepoDir = [&]() -> std::wstring {
        std::vector<std::wstring> candidates;
        wchar_t buf[MAX_PATH]{};
        if (::GetTempPathW(MAX_PATH, buf)) candidates.push_back(NormalizePath(buf));
        if (::GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) candidates.push_back(NormalizePath(buf));
        candidates.push_back(fixture);
        for (const auto& c : candidates)
            if (!c.empty() && PathIsDirectory(c) && !ProbeRepo(c).IsRepo()) return c;
        return {};
    };
    const std::wstring plainDir = pickNonRepoDir();
    const std::wstring repoDir = ProbeRepo(workDir).IsRepo() ? NormalizePath(workDir) : fakeRepo;
    if (repoDir == fakeRepo) {
        ::CreateDirectoryW(fakeRepo.c_str(), nullptr);
        ::CreateDirectoryW((fakeRepo + L"\\.git").c_str(), nullptr);   // 只伪造 .git 目录（零 git 调用）
    }
    {
        HANDLE h = ::CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
    Report(L"夹具：仓库目录=" + repoDir + L"；非仓库目录=" +
           (plainDir.empty() ? std::wstring(L"(未找到)") : plainDir) + L"；文件=" + filePath);

    auto titleOfCommand = [&](const char* key) -> std::wstring {
        const CommandSpec* spec = FindCommandByKey(key);
        return spec ? ResString(mod, spec->titleRes) : std::wstring();
    };
    auto stateOfTitle = [&](IShellItemArray* items, const std::wstring& title, bool* found) -> EXPCMDSTATE {
        IExplorerCommand* r = nullptr;
        if (!makeRoot(&r)) {
            if (found) *found = false;
            return ECS_HIDDEN;
        }
        IExplorerCommand* node = FindNode(r, items, FindMode::ByExactTitle, title);
        EXPCMDSTATE st = ECS_HIDDEN;
        if (found) *found = node != nullptr;
        if (node) {
            node->GetState(items, FALSE, &st);
            node->Release();
        }
        r->Release();
        return st;
    };

    IShellItemArray* repoItems = MakeItemArray(repoDir);
    IShellItemArray* plainItems = plainDir.empty() ? nullptr : MakeItemArray(plainDir);
    IShellItemArray* fileItems = MakeItemArray(filePath);
    Check(repoItems != nullptr && fileItems != nullptr,
          L"构造 IShellItemArray（仓库目录" + std::wstring(repoItems ? L" OK" : L" 失败") + L"；文件" +
              std::wstring(fileItems ? L" OK" : L" 失败") + L"；文件存在=" +
              std::wstring(PathExists(filePath) ? L"1" : L"0") + L"）");

    // ---- 菜单形态判定（《产品设计》v0.5）-------------------------------------
    //   app （默认）：根节点没有子菜单，点击 = 打开 GitRT 主窗口
    //   tree：分层菜单（menu.mode=tree），下面的树结构/裁剪断言才有意义
    bool treeMode = false;
    if (repoItems) {
        IExplorerCommand* rr = nullptr;
        if (makeRoot(&rr)) {
            EXPCMDFLAGS f = ECF_DEFAULT;
            rr->GetFlags(&f);
            treeMode = (f & ECF_HASSUBCOMMANDS) != 0;
            rr->Release();
        }
    }
    Report(L"菜单形态：" + std::wstring(treeMode ? L"tree（分层菜单）"
                                                 : L"app（单一入口 = 打开主窗口；menu.mode=tree 可切回）"));
    // 树结构类断言在 app 形态下不适用 → 记为 SKIP 而不是 FAIL（避免误报）
    auto checkTree = [&](bool ok, const std::wstring& what) {
        if (treeMode) {
            Check(ok, what);
        } else {
            Skip(what + L"（仅 menu.mode=tree 时验证）");
        }
    };

    if (repoItems) {
        bool found = false;
        const EXPCMDSTATE st = stateOfTitle(repoItems, titleOfCommand("commit.stage"), &found);
        checkTree(found && st == ECS_ENABLED,
                  L"仓库内：暂存命令可见且 ENABLED（" + StateText(st) + L"）");
    }
    if (plainItems) {
        bool found = false;
        const EXPCMDSTATE st = stateOfTitle(plainItems, titleOfCommand("sync.pull"), &found);
        checkTree(found && st == ECS_HIDDEN,
                  L"非仓库：拉取命令 HIDDEN（requiresRepo 裁剪，当前 " + StateText(st) + L"）");
    } else {
        Skip(L"非仓库裁剪校验：本机找不到任何「不在仓库内」的可读目录");
    }
    if (plainItems) {
        bool found = false;
        const EXPCMDSTATE st = stateOfTitle(plainItems, titleOfCommand("repo.init"), &found);
        checkTree(found && st == ECS_ENABLED,
                  L"非仓库：在此处初始化仓库 ENABLED（" + StateText(st) + L"）");
    }
    if (fileItems) {
        bool found = false;
        const EXPCMDSTATE st = stateOfTitle(fileItems, titleOfCommand("sync.pull"), &found);
        checkTree(!found || st == ECS_HIDDEN,
                  L"文件选区：拉取命令 HIDDEN（只对目录/空白处可见，当前 " + StateText(st) + L"）");
    }

    // ---- ②b App 形态（默认）：根节点就是"打开 GitRT"入口 --------------------
    if (repoItems) {
        IExplorerCommand* r = nullptr;
        if (makeRoot(&r)) {
            EXPCMDFLAGS f = ECF_DEFAULT;
            r->GetFlags(&f);
            if (!treeMode) {
                Check((f & ECF_HASSUBCOMMANDS) == 0, L"App 形态：根节点不含 ECF_HASSUBCOMMANDS（无子菜单）");
                IEnumExplorerCommand* en = nullptr;
                const HRESULT hrEnum = r->EnumSubCommands(&en);
                Check(hrEnum == E_NOTIMPL && en == nullptr,
                      L"App 形态：EnumSubCommands 返回 E_NOTIMPL（不会挂空子菜单）");
                LPWSTR tip = nullptr;
                const HRESULT hrTip = r->GetToolTip(repoItems, &tip);
                Check(SUCCEEDED(hrTip) && tip && *tip,
                      L"App 形态：根节点有工具提示（说明这一项是打开 GitRT 主窗口）");
                if (tip) ::CoTaskMemFree(tip);
                EXPCMDSTATE st = ECS_HIDDEN;
                r->GetState(repoItems, FALSE, &st);
                Check(st == ECS_ENABLED, L"App 形态：入口在所有场景下都可点（" + StateText(st) + L"）");
                Report(L"      （点击入口 → 打开主窗口的端到端验证：--invoke app.main）");
            }
            r->Release();
        }
    }

    // ---- ③ 开关项：默认**不上菜单**（产品决策）；存在时必须是复选/单选且 Invoke 只改配置 ----
    if (repoItems && treeMode) {
        IExplorerCommand* r = nullptr;
        if (makeRoot(&r)) {
            IExplorerCommand* flagNode = FindNode(r, repoItems, FindMode::FirstToggleable, L"");
            if (!flagNode) {
                // 默认配置：menu.showFlags=false → 菜单里不应有任何开关项
                Check(true, L"默认菜单里没有复选/单选开关项（menu.showFlags=false，开关只在参数面板）");
                Report(L"      （要验证菜单内联开关：在 config.json 设 \"menu.showFlags\": true 后重跑）");
            } else {
                EXPCMDSTATE st = ECS_ENABLED;
                flagNode->GetState(repoItems, FALSE, &st);
                const std::wstring flagTitle = TitleOf(flagNode, repoItems);
                Check((st & ECS_CHECKBOX) != 0 || (st & ECS_RADIOCHECK) != 0,
                      L"开关项返回复选状态（" + flagTitle + L" → " + StateText(st) + L"）");
                const bool wasOn = (st & ECS_CHECKED) != 0;
                flagNode->Invoke(repoItems, nullptr);   // 只切配置
                EXPCMDSTATE after = ECS_ENABLED;
                flagNode->GetState(repoItems, FALSE, &after);
                Check(((after & ECS_CHECKED) != 0) != wasOn,
                      L"Invoke 后复选态翻转（" + std::to_wstring(wasOn) + L" → " +
                          std::to_wstring((after & ECS_CHECKED) != 0) + L"）");
                if (DirWritable(AppDataDir())) {
                    std::string cfg;
                    ::ReadFileText(ConfigFilePath(), &cfg);
                    Check(cfg.find("flags.") != std::string::npos,
                          L"开关状态已落盘到 config.json（" + ConfigFilePath() + L"）");
                } else {
                    Skip(L"开关落盘校验：本会话不允许写 " + AppDataDir() +
                         L"（环境限制，与被测代码无关；在真实用户会话里应通过）");
                }
                flagNode->Invoke(repoItems, nullptr);   // 还原
                flagNode->Release();
            }
            r->Release();
        }
    }

    // ---- ④ 菜单结构：顶层只应有分组入口（+ 可选直达项）、无游离开关、≤16 项 ----
    if (repoItems && root) {
        const auto kids = ChildrenOf(root);
        int separators = 0, emptyTitles = 0, toggleables = 0, groups = 0, leaves = 0;
        for (auto* k : kids) {
            EXPCMDFLAGS f = ECF_DEFAULT;
            k->GetFlags(&f);
            if (f & ECF_ISSEPARATOR) ++separators;
            if (f & ECF_TOGGLEABLE) ++toggleables;
            if (f & ECF_HASSUBCOMMANDS) ++groups;
            else if (!(f & ECF_ISSEPARATOR)) ++leaves;
            if (TitleOf(k, repoItems).empty() && !(f & ECF_ISSEPARATOR)) ++emptyTitles;
            k->Release();
        }
        // 「分组即入口」的产品决策：顶层必须由分组构成，默认没有直达命令项
        checkTree(groups >= 6, L"顶层由分组入口构成（" + std::to_wstring(groups) + L" 个分组）");
        checkTree(emptyTitles == 0 && toggleables == 0,
                  L"顶层无空标题项、无开关项（开关只在 GUI 参数面板）");
        checkTree(separators <= 1,
                  L"顶层最多 1 个分隔符（仅当配置了 menu.directTop 时出现；当前 " +
                      std::to_wstring(separators) + L" 个，直达命令 " + std::to_wstring(leaves) + L" 条）");
        Check(kids.size() <= 16,
              L"顶层条目 " + std::to_wstring(kids.size()) + L" 项 ≤ 16（§3.6 的 16 项上限）");
    }

    // ---- ⑤ 菜单构建期性能（§3.7 的 10 ms 预算）----
    if (repoItems && root) {
        LARGE_INTEGER freq{}, t0{}, t1{};
        ::QueryPerformanceFrequency(&freq);
        int warm = 0;
        SilentWalk(root, repoItems, &warm);          // 预热：LoadString/探测缓存
        double best = 1e9, worst = 0;
        for (int round = 0; round < 5; ++round) {
            int n = 0;
            ::QueryPerformanceCounter(&t0);
            SilentWalk(root, repoItems, &n);
            ::QueryPerformanceCounter(&t1);
            const double ms = QpcMs(t0, t1, freq.QuadPart);
            if (ms < best) best = ms;
            if (ms > worst) worst = ms;
        }
        auto fmtMs = [](double v) {
            wchar_t b[32]{};
            std::swprintf(b, 32, L"%.3f", v);
            return std::wstring(b);
        };
        Report(L"菜单构建期：完整遍历 " + std::to_wstring(warm) + L" 项（GetTitle+GetFlags+GetState）最好 " +
               fmtMs(best) + L" ms / 最差 " + fmtMs(worst) + L" ms（预算 10 ms，§3.7）");
        Check(best < 10.0, L"菜单构建期在预算内（零 git 调用）");
    }

    // ---- ⑥ 内嵌 msix 身份与清单（§3.4 最致命的静默失败点）----
    {
        std::string identity;
        if (!identityPath.empty() && ::ReadFileText(identityPath, &identity)) {
            std::string publisher, packageName, appId;
            JsonField(identity, "publisher", &publisher);
            JsonField(identity, "packageName", &packageName);
            JsonField(identity, "applicationId", &appId);

            HRSRC res = ::FindResourceW(mod, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
            std::string manifest;
            if (res) {
                HGLOBAL g = ::LoadResource(mod, res);
                const DWORD sz = ::SizeofResource(mod, res);
                if (g && sz) manifest.assign(static_cast<const char*>(::LockResource(g)), sz);
            }
            Check(!manifest.empty(), L"DLL 内嵌清单资源存在（RT_MANIFEST id=1）");
            Check(!publisher.empty() && manifest.find(publisher) != std::string::npos,
                  L"内嵌清单 publisher 与 identity.json 一致（" + W(publisher) + L"）");
            Check(!packageName.empty() &&
                      manifest.find("packageName=\"" + packageName + "\"") != std::string::npos,
                  L"内嵌清单 packageName 与 identity.json 一致（" + W(packageName) + L"）");
            Check(!appId.empty() && manifest.find("applicationId=\"" + appId + "\"") != std::string::npos,
                  L"内嵌清单 applicationId 与 identity.json 一致（" + W(appId) + L"）");
        } else {
            Check(false, L"未能读取 identity.json（--identity 未给或文件不存在）");
        }

        std::string appx;
        if (!appxPath.empty() && ::ReadFileText(appxPath, &appx)) {
            Check(appx.find("windows.fileExplorerContextMenus") != std::string::npos,
                  L"AppxManifest 含 windows.fileExplorerContextMenus 扩展");
            Check(appx.find("com:SurrogateServer") != std::string::npos,
                  L"AppxManifest 含 com:SurrogateServer（COM 注册）");
            Check(appx.find("Directory\\Background") != std::string::npos,
                  L"AppxManifest 注册了 Directory\\Background 目标类型");
            Check(appx.find("@GRT_") == std::string::npos, L"AppxManifest 无未替换的 @...@ 占位符");
            Check(appx.find("AllowExternalContent") != std::string::npos,
                  L"AppxManifest 声明 AllowExternalContent（Add-AppxPackage -ExternalLocation 的前提）");
        } else {
            Check(false, L"未能读取生成的 AppxManifest.xml（--appx 未给或文件不存在）");
        }
    }

    // ---- 清理 ----
    if (repoItems) repoItems->Release();
    if (plainItems) plainItems->Release();
    if (fileItems) fileItems->Release();
    if (root) root->Release();
    // 只清理探针自己造的夹具；仓库目录若用的是真实工作目录则不动（§13.4 的洁癖）
    ::DeleteFileW(filePath.c_str());
    ::RemoveDirectoryW((fakeRepo + L"\\.git").c_str());
    ::RemoveDirectoryW(fakeRepo.c_str());
    ::RemoveDirectoryW(fixture.c_str());

    Report(L"");
    Report(L"== 结果: " + std::to_wstring(g_pass) + L" 通过 / " + std::to_wstring(g_fail) + L" 失败 / " +
           std::to_wstring(g_skip) + L" 跳过 ==");
    if (!selfTestPath.empty()) {
        const std::string utf8 = WideToUtf8(g_report);
        UniqueHandle h(::CreateFileW(selfTestPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h.get() != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            ::WriteFile(static_cast<HANDLE>(h.get()), utf8.data(), static_cast<DWORD>(utf8.size()), &wrote,
                        nullptr);
        }
    }
    ::CoUninitialize();
    ::FreeLibrary(mod);
    return g_fail == 0 ? 0 : 1;
}
