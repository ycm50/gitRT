// 主窗口：左命令导航 / 右内容（状态视图或参数面板）/ 顶部仓库选择（《技术实现设计》§9.1）
#include "gui.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

namespace grt::gui {

const wchar_t* kAppWindowClass = L"GitRT.MainWindow";

enum : int {
    IDC_AW_PICK_REPO = 700,
    IDC_AW_REPO_LABEL,
    IDC_AW_REFRESH,
    IDC_AW_LIST,
    IDC_AW_STATUS_BAR,
    IDC_AW_AI,
};
enum : int { kStatusPanelCommand = 1405 };

namespace {

struct AwState {
    HWND repoBtn = nullptr, repoLabel = nullptr, refreshBtn = nullptr, aiBtn = nullptr, list = nullptr,
         statusBar = nullptr;
    HWND view = nullptr, panel = nullptr;
    std::vector<CommandId> itemCmd;   // 列表项 → 命令 ID（0 = 分组标题）
};

std::wstring PickFolder(HWND owner) {
    wchar_t buf[MAX_PATH]{};
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = Str(IDS_TITLE_PICK_REPO).c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST idl = ::SHBrowseForFolderW(&bi);
    if (!idl) return {};
    std::wstring out;
    if (::SHGetPathFromIDListW(idl, buf)) out = NormalizePath(buf);
    ::CoTaskMemFree(idl);
    return out;
}

void UpdateBottomBar(AwState* st) {
    std::wstring text;
    if (!App().gitVersion.empty()) text = App().gitVersion;
    if (!App().repoRoot.empty()) {
        if (!text.empty()) text += L"   |   ";
        text += App().repoRoot;
    }
    if (App().statusLoaded) {
        text += L"   |   " + Str(IDS_SUM_STAGED) + L" " + std::to_wstring(App().status.staged) + L"  " +
                Str(IDS_SUM_MODIFIED) + L" " + std::to_wstring(App().status.modified) + L"  " +
                Str(IDS_SUM_UNTRACKED) + L" " + std::to_wstring(App().status.untracked) + L"  " +
                Str(IDS_SUM_CONFLICTED) + L" " + std::to_wstring(App().status.conflicted);
    } else if (!App().lastError.empty()) {
        text += L"   |   " + App().lastError;
    }
    SetText(st->statusBar, text);
}

void Layout(HWND hwnd, AwState* st) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(10), toolbarH = Scale(44), statusH = Scale(26), leftW = Scale(300);
    const int clientW = static_cast<int>(rc.right);
    const int clientH = static_cast<int>(rc.bottom);
    ::MoveWindow(st->repoBtn, pad, Scale(8), Scale(120), Scale(28), TRUE);
    const int labelW = (std::max)(Scale(120), clientW - pad * 2 - Scale(130) - Scale(220));
    ::MoveWindow(st->repoLabel, pad + Scale(130), Scale(12), labelW, Scale(22), TRUE);
    ::MoveWindow(st->refreshBtn, clientW - pad - Scale(90), Scale(8), Scale(90), Scale(28), TRUE);
    if (st->aiBtn)
        ::MoveWindow(st->aiBtn, clientW - pad - Scale(90) - Scale(8) - Scale(120), Scale(8), Scale(120),
                     Scale(28), TRUE);
    ::MoveWindow(st->list, 0, toolbarH, leftW, clientH - toolbarH - statusH, TRUE);

    const int cx = leftW + Scale(10), cy = toolbarH + Scale(6);
    const int cw = (std::max)(Scale(200), clientW - cx - pad);
    const int ch = (std::max)(Scale(120), clientH - statusH - cy - Scale(6));
    if (st->view) ::MoveWindow(st->view, cx, cy, cw, ch, TRUE);
    if (st->panel) ::MoveWindow(st->panel, cx, cy, cw, ch, TRUE);
    ::MoveWindow(st->statusBar, 0, clientH - statusH, clientW, statusH, TRUE);
}

void ShowStatusView(AwState* st) {
    if (st->panel) {
        ::DestroyWindow(st->panel);
        st->panel = nullptr;
    }
    if (st->view) ::ShowWindow(st->view, SW_SHOW);
}

void SelectCommandEx(HWND hwnd, AwState* st, CommandId id,
                     const std::vector<std::wstring>* pathsOverride,
                     const std::map<std::string, std::wstring>* flags) {
    const CommandSpec* spec = FindCommand(id);
    if (!spec) return;
    if (st->panel) {
        ::DestroyWindow(st->panel);
        st->panel = nullptr;
    }
    std::vector<std::wstring> paths;
    if (pathsOverride && !pathsOverride->empty()) {
        paths = *pathsOverride;   // 右键菜单传来的选区（§4.5）
    } else {
        paths = StatusViewSelectedPaths(st->view);
        if (paths.empty() && !App().repoRoot.empty()) paths.push_back(App().repoRoot);
    }
    st->panel = flags ? CreateParamPanelEx(hwnd, *spec, paths, *flags)
                      : CreateParamPanel(hwnd, *spec, paths);
    if (st->view) ::ShowWindow(st->view, SW_HIDE);
    Layout(hwnd, st);
}

void SelectCommand(HWND hwnd, AwState* st, CommandId id) {
    const CommandSpec* spec = FindCommand(id);
    if (!spec) return;
    if (id == kStatusPanelCommand) {
        ShowStatusView(st);
        Layout(hwnd, st);
        return;
    }
    SelectCommandEx(hwnd, st, id, nullptr, nullptr);
}

void PopulateCommands(AwState* st) {
    for (int g = 0; g < static_cast<int>(GroupId::Count); ++g) {
        const std::wstring head = std::wstring(L"\u25b8 ") + GroupName(static_cast<GroupId>(g));
        ::SendMessageW(st->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(head.c_str()));
        st->itemCmd.push_back(0);
        for (size_t ci = 0; ci < CommandTableSize(); ++ci) {
            const CommandSpec& c = CommandTable()[ci];
            if (static_cast<int>(c.group) != g) continue;
            const std::wstring item = L"      " + Str(c.titleRes);
            ::SendMessageW(st->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
            st->itemCmd.push_back(c.id);
        }
    }
}

LRESULT CALLBACK AppProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<AwState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new AwState()));
            return TRUE;
        case WM_CREATE: {
            App().main = hwnd;
            st->repoBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_PICK_REPO),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_AW_PICK_REPO,
                                    Th().fontUi);
            st->repoLabel = MakeChild(hwnd, WC_STATICW,
                                      App().repoRoot.empty() ? Str(IDS_MSG_NO_REPO) : App().repoRoot,
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS, 0, IDC_AW_REPO_LABEL,
                                      Th().fontUi);
            st->refreshBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_REFRESH),
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_AW_REFRESH, Th().fontUi);
            st->aiBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_CMD_APP_AI),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_AW_AI, Th().fontUi);
            st->list = MakeChild(hwnd, WC_LISTBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY |
                                     LBS_NOINTEGRALHEIGHT,
                                 WS_EX_CLIENTEDGE, IDC_AW_LIST, Th().fontUi);
            st->statusBar = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                      IDC_AW_STATUS_BAR, Th().fontSmall);
            st->view = CreateStatusView(hwnd);
            PopulateCommands(st);
            ThemeApply(hwnd);

            if (!App().repoRoot.empty()) RefreshRepoStatus(st->view);
            UpdateBottomBar(st);
            // 默认进入状态面板：先看到仓库状态，命令列表保持在顶部且不预选
            ShowStatusView(st);
            ::SendMessageW(st->list, LB_SETTOPINDEX, 0, 0);
            Layout(hwnd, st);
            return 0;
        }
        case WM_SIZE:
            Layout(hwnd, st);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = Scale(820);
            mmi->ptMinTrackSize.y = Scale(560);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_AW_PICK_REPO: {
                    const std::wstring dir = PickFolder(hwnd);
                    if (dir.empty()) return 0;
                    App().probe = ProbeRepo(dir);
                    App().repoRoot = App().probe.IsRepo() ? App().probe.repoRoot : dir;
                    SetText(st->repoLabel, App().repoRoot);
                    RefreshRepoStatus(st->view);
                    UpdateBottomBar(st);
                    GRT_LOGI("gui", "选择仓库 " << U8(App().repoRoot) << " flags="
                                               << U8(RepoFlagsToString(App().probe.flags)));
                    return 0;
                }
                case IDC_AW_REFRESH:
                    RefreshRepoStatus(st->view);
                    UpdateBottomBar(st);
                    return 0;
                case IDC_AW_AI: {
                    const CommandSpec* ai = FindCommandByKey("app.ai");
                    if (ai) ExecuteInternalCommand(hwnd, *ai, {}, nullptr);
                    return 0;
                }
                case IDC_AW_LIST:
                    if (HIWORD(wp) == LBN_SELCHANGE) {
                        const int sel = static_cast<int>(::SendMessageW(st->list, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(st->itemCmd.size()) && st->itemCmd[sel])
                            SelectCommand(hwnd, st, st->itemCmd[sel]);
                    }
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_GRT_STATUS_RELOAD:
            StatusViewReload(st->view);
            UpdateBottomBar(st);
            return 0;
        case WM_GRT_TASK_FINISHED:
            // 主窗口不是结果展示方（AI 窗口才是）；仅接管并释放，避免泄漏
            delete reinterpret_cast<TaskSummary*>(lp);
            return 0;
        case WM_GRT_SHOW_STATUS:
            ShowStatusView(st);
            Layout(hwnd, st);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            LRESULT res = 0;
            if (HandleCtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), &res)) return res;
            break;
        }
        case WM_ERASEBKGND:
            if (Th().dark) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                ::FillRect(reinterpret_cast<HDC>(wp), &rc, Th().bgBrush);
                return 1;
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            App().main = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void RegisterAppWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = AppProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kAppWindowClass;
    ::RegisterClassExW(&wc);
}

// ------------------------------------------------- 右键菜单请求落地（§4.5）
bool AppWindowApplyRequest(HWND main, const ShellMenuRequest& req) {
    if (!main) return false;
    auto* st = reinterpret_cast<AwState*>(::GetWindowLongPtrW(main, GWLP_USERDATA));
    if (!st) return false;

    // ── cmdId == 0：菜单只提供"打开 GitRT"入口（menu.mode = "app"）────────────
    // 不选中任何命令，只把仓库/目录上下文切到用户右键的位置（他期望"在这里打开"）。
    if (req.cmdId == 0) {
        if (!req.repoHint.empty()) {
            App().probe = ProbeRepo(req.repoHint);
            App().repoRoot = App().probe.IsRepo() ? App().probe.repoRoot : NormalizePath(req.repoHint);
            SetText(st->repoLabel, App().repoRoot);
            RefreshRepoStatus(st->view);
            UpdateBottomBar(st);
        }
        GRT_LOGI("gui", "菜单请求：打开应用（无指定命令） repo=" << U8(App().repoRoot)
                                                             << " paths=" << req.paths.size());
        return true;
    }

    const CommandSpec* spec = FindCommand(req.cmdId);
    if (!spec) {
        GRT_LOGW("gui", "菜单请求的命令 ID 不在命令表内 id=" << req.cmdId);
        return false;
    }
    // ID 与 key 交叉校验：两者不一致说明 DLL 与 GUI 版本脱节，拒绝执行（§4.5）
    if (!req.cmdKey.empty() && req.cmdKey != W(spec->key)) {
        GRT_LOGE("gui", "菜单请求 key/ID 不一致 key=" << U8(req.cmdKey) << " id=" << req.cmdId);
        return false;
    }
    if (!req.repoHint.empty()) {
        App().probe = ProbeRepo(req.repoHint);
        App().repoRoot = App().probe.IsRepo() ? App().probe.repoRoot : NormalizePath(req.repoHint);
        SetText(st->repoLabel, App().repoRoot);
        RefreshRepoStatus(st->view);
        UpdateBottomBar(st);
    }
    // 列表定位到该命令（让人看到"菜单点的是哪一条"）
    for (size_t i = 0; i < st->itemCmd.size(); ++i) {
        if (st->itemCmd[i] == req.cmdId) {
            ::SendMessageW(st->list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
            break;
        }
    }
    GRT_LOGI("gui", "应用菜单请求 key=" << spec->key << " paths=" << req.paths.size()
                                        << " flags=" << req.flags.size());
    if (req.cmdId == kStatusPanelCommand) {
        ShowStatusView(st);
        Layout(main, st);
        return true;
    }
    SelectCommandEx(main, st, req.cmdId, &req.paths, &req.flags);
    return true;
}

}  // namespace grt::gui
