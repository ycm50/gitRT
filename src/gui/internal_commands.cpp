// Internal 命令的 GUI 实现（打开终端 / 写 .gitignore / 差异 / 历史 / 自检）
#include "gui.h"

#include <shellapi.h>

#include <algorithm>
#include <fstream>

#include "remote.h"   // 远端基线（提交历史里显示"以远端为基、本地新增在上"）
#include "tag.h"
#include "release.h"

namespace grt::gui {

namespace {

std::wstring RunGitOut(const std::vector<std::wstring>& argv, uint32_t timeoutMs = 30000) {
    if (App().gitExe.empty() || App().repoRoot.empty()) return {};
    const RunResult r = RunGitSync(App().gitExe, argv, App().repoRoot, timeoutMs);
    std::wstring out = W(r.out);
    if (!r.err.empty()) {
        if (!out.empty()) out += L"\r\n";
        out += W(r.err);
    }
    if (out.empty() && r.exitCode != 0) out = Str(IDS_MSG_FAILED) + L" (exit=" + std::to_wstring(r.exitCode) + L")";
    return out;
}

// 依次尝试 Windows Terminal → PowerShell → cmd（git-bash 见下）
void LaunchTerminal(HWND owner, const std::map<std::string, std::wstring>* flags) {
    std::wstring dir = App().repoRoot.empty() ? GetModuleDir() : App().repoRoot;
    std::string choice = "windows-terminal";
    if (flags) {
        for (const auto& kv : *flags) {
            if (kv.first == "git-bash" && kv.second == L"1") choice = "git-bash";
            else if (kv.first == "powershell" && kv.second == L"1") choice = "powershell";
            else if (kv.first == "cmd" && kv.second == L"1") choice = "cmd";
            else if (kv.first == "windows-terminal" && kv.second == L"1") choice = "windows-terminal";
        }
    }
    auto exists = [](const std::wstring& p) { return PathExists(p); };

    if (choice == "git-bash") {
        const std::wstring root = GitRootOf(App().gitExe);
        const std::wstring gitBash = root + L"\\git-bash.exe";
        if (exists(gitBash)) {
            const std::wstring args = L"--cd=\"" + dir + L"\"";
            if (reinterpret_cast<INT_PTR>(::ShellExecuteW(owner, L"open", gitBash.c_str(), args.c_str(),
                                                         dir.c_str(), SW_SHOWNORMAL)) > 32)
                return;
        }
    }
    if (choice == "windows-terminal" || choice == "git-bash") {
        const std::wstring args = L"-d \"" + dir + L"\"";
        if (reinterpret_cast<INT_PTR>(::ShellExecuteW(owner, L"open", L"wt.exe", args.c_str(), dir.c_str(),
                                                     SW_SHOWNORMAL)) > 32)
            return;
    }
    if (choice == "powershell") {
        const std::wstring args = L"-NoExit -Command Set-Location -LiteralPath '" + dir + L"'";
        if (reinterpret_cast<INT_PTR>(::ShellExecuteW(owner, L"open", L"powershell.exe", args.c_str(),
                                                     dir.c_str(), SW_SHOWNORMAL)) > 32)
            return;
    }
    const std::wstring args = L"/K cd /d \"" + dir + L"\"";
    ::ShellExecuteW(owner, L"open", L"cmd.exe", args.c_str(), dir.c_str(), SW_SHOWNORMAL);
}

// 追加忽略模式到仓库根的 .gitignore
void AddToGitignore(HWND owner, const std::vector<std::wstring>& paths) {
    if (App().repoRoot.empty()) {
        ::MessageBoxW(owner, Str(IDS_MSG_NEED_REPO).c_str(), Str(IDS_TITLE_MAIN).c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring file = App().repoRoot + L"\\.gitignore";
    std::string existing;
    {
        UniqueHandle h(::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h.get() != INVALID_HANDLE_VALUE) {
            char buf[65536];
            DWORD got = 0;
            if (::ReadFile(static_cast<HANDLE>(h.get()), buf, sizeof(buf), &got, nullptr))
                existing.assign(buf, got);
        }
    }
    int added = 0, skipped = 0;
    std::string append;
    for (const auto& p : paths) {
        const std::string name = WideToUtf8(FileNameOf(p));
        if (name.empty()) continue;
        if (existing.find(name) != std::string::npos) {
            ++skipped;
            continue;
        }
        if (append.find(name) != std::string::npos) continue;
        append += name;
        append += "\n";
        ++added;
    }
    if (added > 0) {
        UniqueHandle h(::CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h.get() != INVALID_HANDLE_VALUE) {
            std::string text = existing.empty() ? ("# GitRT\n" + append) : append;
            DWORD wrote = 0;
            ::WriteFile(static_cast<HANDLE>(h.get()), text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
            GRT_LOGI("gui", "写入 .gitignore 增加 " << added << " 条");
        }
    }
    std::wstring msg = Str(added > 0 ? IDS_MSG_IGNORE_ADDED : IDS_MSG_IGNORE_EXISTS);
    msg += L"\r\n\r\n" + file + L"\r\n+" + std::to_wstring(added) + L" / \u5df2\u5b58\u5728 " +
           std::to_wstring(skipped);
    ::MessageBoxW(owner, msg.c_str(), Str(IDS_TITLE_MAIN).c_str(), MB_OK | MB_ICONINFORMATION);
    RefreshRepoStatus(nullptr);
}

std::wstring BuildDoctorReport() {
    std::wstring r;
    auto line = [&](const std::wstring& k, const std::wstring& v) { r += k + L": " + v + L"\r\n"; };
    r += L"== GitRT \u81ea\u68c0 ==\r\n";
    line(L"GitRT \u7248\u672c", W(GRT_VERSION));
    line(L"git.exe", App().gitExe.empty() ? Str(IDS_MSG_GIT_NOT_FOUND) : App().gitExe);
    line(L"git \u7248\u672c", App().gitVersion);
    line(L"\u4ed3\u5e93\u6839", App().repoRoot.empty() ? Str(IDS_MSG_NO_REPO) : App().repoRoot);
    line(L"\u4ed3\u5e93\u6807\u5fd7", RepoFlagsToString(App().probe.flags));
    line(L"git \u76ee\u5f55", App().probe.gitDir);
    line(L"\u914d\u7f6e\u6587\u4ef6", ConfigFilePath());
    line(L"\u65e5\u5fd7\u6587\u4ef6", LogFilePath());
    line(L"DPI", std::to_wstring(App().dpi));
    line(L"\u6df1\u8272\u6a21\u5f0f", Th().dark ? L"\u662f" : L"\u5426");
    line(L"\u547d\u4ee4\u8868\u6761\u76ee", std::to_wstring(kCommandCount));
    r += L"\r\n";
    r += L"== \u5df2\u5b9e\u73b0\u8303\u56f4 ==\r\n";
    r += L"\u2022 GUI\uff08\u672c\u6b21\u589e\u91cf\uff09\uff1a\u53c2\u6570\u9762\u677f + \u5b9e\u65f6\u547d\u4ee4\u884c\u9884\u89c8 + \u72b6\u6001\u9762\u677f + \u8fdb\u5ea6/\u53d6\u6d88\r\n";
    r += L"\u2022 \u672a\u5b9e\u73b0\uff1aWin11 \u73b0\u4ee3\u53f3\u952e\u83dc\u5355\u6269\u5c55\uff08IExplorerCommand\uff09\u3001MSIX \u6253\u5305\u4e0e\u7b7e\u540d\r\n";
    r += L"  \u2014\u2014 \u89c1\u300a\u6280\u672f\u5b9e\u73b0\u8bbe\u8ba1\u300b\u00a714.4 \u5b9e\u65bd\u987a\u5e8f\u7b2c 1-2 \u6b65\r\n";
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// 查看类命令的内容：**只在这里产出一份**。
//   · 点「执行」→ 用 ShowTextWindow 打开完整内容
//   · 选中命令后参数面板会自动调用同一个函数，把内容直接显示在面板里并定时刷新
// ---------------------------------------------------------------------------
bool IsViewCommand(CommandId id) {
    return id == 1401 || id == 1402 || id == 1403;   // 提交历史 / 查看差异 / 文件历史
}

bool BuildViewText(CommandId id, const std::vector<std::wstring>& paths,
                   const std::map<std::string, std::wstring>& flags, uint16_t* titleRes,
                   std::wstring* body) {
    if (!body) return false;
    if (titleRes) *titleRes = IDS_TITLE_LOG;
    auto on = [&](const char* key) {
        const auto it = flags.find(key);
        return it != flags.end() && it->second == L"1";
    };
    switch (id) {
        case 1401: {   // 提交历史：先给"远端基线"的两段（本地新增在上、远端基线在下），再给提交图
            const RemoteBaseline base = LoadRemoteBaseline(App().gitExe, App().repoRoot, 100);
            std::wstring text = DescribeRemoteBaseline(base);
            for (const auto& c : base.commits) {
                text += c.localOnly ? L"【本地】 " : (c.remoteOnly ? L"【远端新增】 " : L"【远端】 ");
                text += c.shortHash + L"  " + c.date + L"  " + c.subject + L"\r\n";
            }
            text += L"\r\n---- 提交图（--all --graph --decorate）----\r\n";
            std::wstring graph = RunGitOut({L"log", L"--all", L"--graph", L"--decorate", L"--oneline",
                                                  L"-n", L"300"});
            if (graph.empty()) graph = Str(IDS_MSG_NO_CHANGES);
            *body = text + graph;
            return true;
        }
        case 1402: {  // 查看差异
            std::vector<std::wstring> argv{L"diff"};
            if (on("staged")) argv.push_back(L"--staged");
            if (on("stat")) argv.push_back(L"--stat");
            if (on("name-only")) argv.push_back(L"--name-only");
            if (!paths.empty()) {
                argv.push_back(L"--");
                argv.insert(argv.end(), paths.begin(), paths.end());
            }
            *body = RunGitOut(argv);
            if (body->empty()) *body = Str(IDS_MSG_NO_CHANGES);
            if (paths.size() == 1) *body = paths.front() + L"\r\n\r\n" + *body;
            if (titleRes) *titleRes = IDS_TITLE_DIFF;
            return true;
        }
        case 1403: {  // 文件历史
            if (paths.empty()) {
                *body = Str(IDS_MSG_NEED_SELECTION);
                return true;
            }
            std::vector<std::wstring> argv{L"log", L"--follow", L"--stat", L"-n", L"100", L"--"};
            argv.insert(argv.end(), paths.begin(), paths.end());
            *body = RunGitOut(argv);
            return true;
        }
        case 2307: {   // tag.list —— 标签清单（本地/远端、轻量/附注）
            std::vector<TagInfo> tags;
            std::wstring err;
            LoadTags(App().gitExe, App().repoRoot, &tags, &err);
            if (titleRes) *titleRes = IDS_CMD_TAG_LIST;
            *body = err.empty() ? DescribeTags(tags) : (L"读取标签失败：" + err);
            return true;
        }
        case 2311: {   // release.list —— GitHub Release 列表（依赖 gh，缺失时给中文原因）
            std::vector<ReleaseEntry> rels;
            std::wstring err;
            LoadReleases(App().gitExe, App().repoRoot, &rels, &err);
            if (titleRes) *titleRes = IDS_CMD_RELEASE_LIST;
            if (!err.empty()) { *body = err; return true; }
            if (rels.empty()) { *body = Str(IDS_TAG_EMPTY); return true; }
            std::wstring text;
            for (const auto& r : rels) {
                text += r.tag + L"  " + (r.name.empty() ? r.tag : r.name) + L"  " + r.publishedAt;
                if (r.draft)       text += L"  [草稿]";
                if (r.prerelease)  text += L"  [预发布]";
                text += L"\r\n";
            }
            *body = text;
            return true;
        }        default:
            return false;
    }
}
// ------------------------------------------------------------ 内部命令的"去向"
// 唯一一份映射：ExecuteInternalCommand 用它分派，GUI 自检也用它遍历命令表断言
// "每个内部命令都有去处"——「标签列表/发布列表」就是漏在这里才点了弹「尚未实现」。
namespace {
// 自动化（自检/CI）期间抑制模态框：模态框会阻塞调用线程，没人点就永远不返回。
// 实测：分派缺映射时，自检不是"变红"而是挂死（报告文件都没生成），CI 会拖到超时。
bool gSuppressModal = false;
}  // namespace

void SuppressModalDialogs(bool suppress) { gSuppressModal = suppress; }
bool ModalDialogsSuppressed() { return gSuppressModal; }

InternalAction ActionOfInternal(CommandId id) {
    switch (id) {
        case 1003: return InternalAction::Console;
        case 1004: return InternalAction::Terminal;
        case 1104: return InternalAction::Gitignore;
        case 1108: return InternalAction::SquashWindow;
        case 1401:   // inspect.log
        case 1402:   // inspect.diff
        case 1403: return InternalAction::TextWindow;
        case 1405: return InternalAction::StatusView;
        case 1701: return InternalAction::Settings;
        case 1702: return InternalAction::Doctor;
        case 1703: return InternalAction::Ai;
        case 2109: return InternalAction::RestoreWindow;
        case 2110: return InternalAction::RemoteWindow;
        // ★ 标签/发布：**列表与写操作开同一个窗口**（那个窗口本身就是"列表 + 表单 + 预览 + 日志"，
        //   选中列表行还会自动填进表单）。tag.list(2307) / release.list(2311) 以前漏在这里，
        //   于是菜单里看得见、点下去弹「该功能尚未实现」。
        case 2307:   // tag.list —— 标签列表
        case 2308:   // tag.create —— 打标签
        case 2309:   // tag.push —— 推送标签
        case 2310: return InternalAction::TagWindow;      // tag.delete —— 删除标签（窗口里二次确认）
        case 2311:   // release.list —— 发布列表
        case 2312: return InternalAction::ReleaseWindow;  // release.create —— 创建发布（gh）
        default:   return InternalAction::Unimplemented;
    }
}

bool OpensDedicatedWindow(CommandId id) {
    switch (ActionOfInternal(id)) {
        case InternalAction::TagWindow:
        case InternalAction::ReleaseWindow:
        case InternalAction::RestoreWindow:
        case InternalAction::RemoteWindow:
        case InternalAction::SquashWindow:
            return true;
        default:
            return false;
    }
}

void ExecuteInternalCommand(HWND owner, const CommandSpec& spec, const std::vector<std::wstring>& paths,
                            const std::map<std::string, std::wstring>* flags) {
    switch (ActionOfInternal(spec.id)) {
        case InternalAction::Console:   // app.console：把主窗口带到前台
            if (App().main) {
                ::ShowWindow(App().main, SW_RESTORE);
                ::SetForegroundWindow(App().main);
            }
            return;
        case InternalAction::Terminal:   // app.terminal
            LaunchTerminal(owner, flags);
            return;
        case InternalAction::Gitignore:   // commit.ignore
            AddToGitignore(owner, paths);
            return;
        case InternalAction::SquashWindow:   // commit.squash —— 合并所选提交（复选列表窗口）
            ShowSquashWindow(owner);
            return;
        case InternalAction::TextWindow: {   // inspect.log / diff / filelog
            // 内容与面板自动预览同源（BuildViewText）
            uint16_t title = IDS_TITLE_LOG;
            std::wstring body;
            const std::map<std::string, std::wstring> emptyFlags;
            BuildViewText(spec.id, paths, flags ? *flags : emptyFlags, &title, &body);
            const std::wstring subtitle = (spec.id == 1403 && !paths.empty()) ? paths.front() : App().repoRoot;
            ShowTextWindow(owner, title, subtitle, body);
            return;
        }
        case InternalAction::StatusView:   // inspect.status：切回状态视图
            if (App().main) ::PostMessageW(App().main, WM_GRT_SHOW_STATUS, 0, 0);
            return;
        case InternalAction::Settings:   // app.settings
            // 真正能用的设置界面：AI（端点/模型/Key/超时 + 测试连接）。
            // Key 按产品决策**明文**存 exe 同目录的 GitRT.ai.json（见 settings_window.cpp 顶部说明）。
            ShowSettingsWindow(owner);
            return;
        case InternalAction::Doctor:   // app.doctor
            ShowTextWindow(owner, IDS_TITLE_DOCTOR, App().repoRoot, BuildDoctorReport());
            return;
        case InternalAction::Ai:   // app.ai
            ShowAiWindow(owner);
            return;
        case InternalAction::RestoreWindow:   // history.restore —— 还原到提交
            ShowRestoreWindow(owner);
            return;
        case InternalAction::RemoteWindow:   // remote.panel —— 远端分支与地址
            ShowRemoteWindow(owner);
            return;
        // 标签 / 发布（阶段二）：专用 GUI 窗口（列表 + 表单 + 命令预览 + 日志）。
        // 列表命令（tag.list / release.list）与写命令用的是同一个窗口。
        case InternalAction::TagWindow:
            ShowTagWindow(owner);
            return;
        case InternalAction::ReleaseWindow:
            ShowReleaseWindow(owner);
            return;
        // ★ 这里**故意不写 `default:`**：switch 的目标类型是枚举 InternalAction，缺任何一个枚举值都会
        //   被 -Wswitch 抓出来（本项目 -Werror）→ **编译期**就挡住"新增一种去向却忘了分派"。
        //   另一个原因来自实测：一旦真落到这里，模态框会**阻塞调用线程** —— 自检/CI 里没人去点它，
        //   于是"回归"表现为**挂死**（我反向验证时实测到：报告文件根本没生成），比失败更糟。
        //   所以自检侧也加了"先确认映射实现了再调用"的前置判断（见 main.cpp）。
        case InternalAction::Unimplemented:
            // 内部命令漏映射时会走到这里（自检的"内部命令都有去向"断言会在 CI 里先把它挡下来）。
            // 除了弹框，也写一条错误日志：日志里能看到是**哪个**命令漏了，便于定位。
            GRT_LOGE("gui", "内部命令没有实现去向 id=" << spec.id << " key=" << spec.key);
            if (!gSuppressModal) {
                ::MessageBoxW(owner, Str(IDS_MSG_NOT_IMPLEMENTED).c_str(), Str(spec.titleRes).c_str(),
                              MB_OK | MB_ICONINFORMATION);
            }
            return;
    }
}

}  // namespace grt::gui
