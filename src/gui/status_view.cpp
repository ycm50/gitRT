// 状态视图：分支/领先落后 + 改动列表 + 行操作（《技术实现设计》§9 状态面板）
#include "gui.h"

#include <shellapi.h>

#include <algorithm>

namespace grt::gui {

const wchar_t* kStatusViewClass = L"GitRT.StatusView";

enum : int {
    IDC_SV_LIST = 600,
    IDC_SV_REFRESH,
    IDC_SV_STAGE,
    IDC_SV_UNSTAGE,
    IDC_SV_DISCARD,
    IDC_SV_DIFF,
    IDC_SV_OPEN,
};

namespace {

struct SvState {
    HWND branchLabel = nullptr, branchVal = nullptr, abLabel = nullptr, abVal = nullptr, sumLabel = nullptr;
    HWND list = nullptr, refreshBtn = nullptr, stageBtn = nullptr, unstageBtn = nullptr;
    HWND discardBtn = nullptr, diffBtn = nullptr, openBtn = nullptr;
    HWND hintLabel = nullptr;
    std::vector<std::wstring> rowPaths;
};

void RunGitAndRefresh(HWND view, std::vector<std::wstring> argv) {
    if (App().gitExe.empty() || App().repoRoot.empty()) return;
    const RunResult r = RunGitSync(App().gitExe, argv, App().repoRoot, 60000);
    if (r.exitCode != 0) {
        GRT_LOGW("gui", "行操作失败 rc=" << r.exitCode << " err=" << Redact(W(r.err)));
        ::MessageBoxW(view, (Str(IDS_MSG_FAILED) + L"\r\n" + W(r.err)).c_str(),
                      Str(IDS_TITLE_MAIN).c_str(), MB_OK | MB_ICONWARNING);
    }
    RefreshRepoStatus(view);
}

std::vector<std::wstring> Selected(SvState* st) {
    std::vector<std::wstring> out;
    if (!st->list) return out;
    const int count = ::SendMessageW(st->list, LVM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        if (::SendMessageW(st->list, LVM_GETITEMSTATE, i, LVIS_SELECTED) & LVIS_SELECTED) {
            if (i < static_cast<int>(st->rowPaths.size())) out.push_back(st->rowPaths[i]);
        }
    }
    return out;
}

void RefreshList(HWND hwnd, SvState* st) {
    if (!st->list) return;
    ::SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    st->rowPaths.clear();
    const AppState& app = App();

    // 顶部摘要
    std::wstring branch = W(app.status.head);
    if (branch.empty()) branch = app.status.detached ? Str(IDS_MSG_DETACHED) : L"-";
    SetText(st->branchVal, branch);

    // 领先/落后：带上游名（"origin/main  +2 / -1"），没设上游就如实说
    std::wstring ab;
    if (app.status.upstream.empty()) {
        ab = Str(IDS_MSG_NO_UPSTREAM);
    } else {
        ab = W(app.status.upstream) + L"   ";
        if (app.status.ahead == 0 && app.status.behind == 0) {
            ab += Str(IDS_MSG_UP_TO_DATE);
        } else {
            ab += L"+" + std::to_wstring(app.status.ahead) + L" / -" + std::to_wstring(app.status.behind);
        }
    }
    SetText(st->abVal, ab);

    if (!app.statusLoaded) {
        SetText(st->sumLabel, app.lastError.empty() ? Str(IDS_MSG_LOADING) : app.lastError);
    } else if (app.status.IsClean()) {
        SetText(st->sumLabel, Str(IDS_MSG_CLEAN));
    } else {
        SetText(st->sumLabel,
                std::to_wstring(app.status.entries.size()) + L" " + Str(IDS_MSG_ENTRIES) +
                    L"   [" + Str(IDS_SUM_STAGED) + L" " + std::to_wstring(app.status.staged) + L"  " +
                    Str(IDS_SUM_MODIFIED) + L" " + std::to_wstring(app.status.modified) + L"  " +
                    Str(IDS_SUM_UNTRACKED) + L" " + std::to_wstring(app.status.untracked) + L"  " +
                    Str(IDS_SUM_CONFLICTED) + L" " + std::to_wstring(app.status.conflicted) + L"]");
    }

    int row = 0;
    for (const auto& e : app.status.entries) {
        LVITEMW item{};
        const std::wstring status = DescribeEntry(e);
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(status.c_str());
        const int idx = static_cast<int>(::SendMessageW(st->list, LVM_INSERTITEMW, 0,
                                                       reinterpret_cast<LPARAM>(&item)));
        std::wstring path = W(e.path);
        if (!e.origPath.empty()) path = W(e.origPath) + L" \u2192 " + path;
        LVITEMW sub{};
        sub.mask = LVIF_TEXT;
        sub.iItem = idx;
        sub.iSubItem = 1;
        sub.pszText = const_cast<LPWSTR>(path.c_str());
        ::SendMessageW(st->list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));
        st->rowPaths.push_back(app.repoRoot + L"\\" + W(e.path));
        ++row;
    }
    ::EnableWindow(st->stageBtn, row > 0);
    ::EnableWindow(st->unstageBtn, row > 0);
    ::EnableWindow(st->discardBtn, row > 0);
    ::EnableWindow(st->diffBtn, app.repoRoot.empty() ? FALSE : TRUE);
    ::EnableWindow(st->openBtn, row > 0);
    SetText(st->hintLabel, app.repoRoot.empty() ? Str(IDS_LABEL_NO_REPO_HINT) : app.repoRoot);
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SvState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new SvState()));
            return TRUE;
        case WM_CREATE: {
            st->branchLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_LABEL_BRANCH), WS_CHILD | WS_VISIBLE, 0, 0,
                                        Th().fontSmall);
            st->branchVal = MakeChild(hwnd, WC_STATICW, L"-", WS_CHILD | WS_VISIBLE, 0, 0, Th().fontBold);
            st->abLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_LABEL_AHEAD_BEHIND), WS_CHILD | WS_VISIBLE,
                                    0, 0, Th().fontSmall);
            st->abVal = MakeChild(hwnd, WC_STATICW, L"-", WS_CHILD | WS_VISIBLE, 0, 0, Th().fontBold);
            st->sumLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_MSG_LOADING), WS_CHILD | WS_VISIBLE, 0, 0,
                                     Th().fontUi);
            st->hintLabel = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 0, 0,
                                      Th().fontSmall);
            st->list = MakeChild(hwnd, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                     LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                 WS_EX_CLIENTEDGE, IDC_SV_LIST, Th().fontUi);
            ::SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = Scale(120);
            col.iSubItem = 0;
            col.pszText = const_cast<LPWSTR>(Str(IDS_COL_STATUS).c_str());
            ::SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
            col.cx = Scale(520);
            col.iSubItem = 1;
            col.pszText = const_cast<LPWSTR>(Str(IDS_COL_FILE).c_str());
            ::SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

            st->refreshBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_REFRESH), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, IDC_SV_REFRESH, Th().fontUi);
            st->stageBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_STAGE), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,
                                     IDC_SV_STAGE, Th().fontUi);
            st->unstageBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_UNSTAGE), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, IDC_SV_UNSTAGE, Th().fontUi);
            st->discardBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_DISCARD), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, IDC_SV_DISCARD, Th().fontUi);
            st->diffBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_DIFF), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,
                                    IDC_SV_DIFF, Th().fontUi);
            st->openBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_OPEN_FOLDER), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    0, IDC_SV_OPEN, Th().fontUi);
            ThemeApply(hwnd);
            RefreshList(hwnd, st);
            return 0;
        }
        case WM_SIZE: {
            if (!st) break;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const int pad = Scale(10), btnH = Scale(30);
            int y = pad;
            const int halfW = (rc.right - pad * 3) / 2;
            ::MoveWindow(st->branchLabel, pad, y, halfW, Scale(16), TRUE);
            ::MoveWindow(st->abLabel, pad * 2 + halfW, y, halfW, Scale(16), TRUE);
            y += Scale(17);
            ::MoveWindow(st->branchVal, pad, y, halfW, Scale(22), TRUE);
            ::MoveWindow(st->abVal, pad * 2 + halfW, y, halfW, Scale(22), TRUE);
            y += Scale(24);
            ::MoveWindow(st->sumLabel, pad, y, rc.right - pad * 2, Scale(20), TRUE);
            y += Scale(22);
            ::MoveWindow(st->hintLabel, pad, y, rc.right - pad * 2, Scale(18), TRUE);
            y += Scale(20);

            const int btnRow = rc.bottom - pad - btnH;
            const int listBottom = btnRow - Scale(8);
            ::MoveWindow(st->list, pad, y, rc.right - pad * 2, (std::max)(Scale(60), listBottom - y), TRUE);
            // 列宽自适应
            const int total = rc.right - pad * 2 - Scale(4);
            ::SendMessageW(st->list, LVM_SETCOLUMNWIDTH, 0, Scale(130));
            ::SendMessageW(st->list, LVM_SETCOLUMNWIDTH, 1, (std::max)(Scale(200), total - Scale(140)));

            int bx = pad;
            auto place = [&](HWND h, int w) {
                ::MoveWindow(h, bx, btnRow, w, btnH, TRUE);
                bx += w + Scale(6);
            };
            place(st->refreshBtn, Scale(80));
            place(st->stageBtn, Scale(80));
            place(st->unstageBtn, Scale(96));
            place(st->discardBtn, Scale(96));
            place(st->diffBtn, Scale(96));
            place(st->openBtn, Scale(140));
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_SV_REFRESH:
                    RefreshRepoStatus(hwnd);
                    return 0;
                case IDC_SV_STAGE: {
                    auto paths = Selected(st);
                    if (paths.empty()) return 0;
                    std::vector<std::wstring> argv{L"add", L"-A", L"--"};
                    argv.insert(argv.end(), paths.begin(), paths.end());
                    RunGitAndRefresh(hwnd, argv);
                    return 0;
                }
                case IDC_SV_UNSTAGE: {
                    auto paths = Selected(st);
                    if (paths.empty()) return 0;
                    std::vector<std::wstring> argv{L"restore", L"--staged", L"--"};
                    argv.insert(argv.end(), paths.begin(), paths.end());
                    RunGitAndRefresh(hwnd, argv);
                    return 0;
                }
                case IDC_SV_DISCARD: {
                    auto paths = Selected(st);
                    if (paths.empty()) return 0;
                    const std::wstring msg = Str(IDS_BTN_DISCARD) + L"\r\n\r\n" +
                                             std::to_wstring(paths.size()) + L" " + Str(IDS_MSG_ENTRIES) +
                                             L"\r\n" + Str(IDS_MSG_DANGER_HINT);
                    if (::MessageBoxW(hwnd, msg.c_str(), Str(IDS_TITLE_MAIN).c_str(),
                                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                        return 0;
                    std::vector<std::wstring> argv{L"restore", L"--worktree", L"--"};
                    argv.insert(argv.end(), paths.begin(), paths.end());
                    RunGitAndRefresh(hwnd, argv);
                    return 0;
                }
                case IDC_SV_DIFF: {
                    if (App().gitExe.empty() || App().repoRoot.empty()) return 0;
                    std::vector<std::wstring> argv{L"diff"};
                    auto paths = Selected(st);
                    if (!paths.empty()) {
                        argv.push_back(L"--");
                        argv.insert(argv.end(), paths.begin(), paths.end());
                    }
                    const RunResult r = RunGitSync(App().gitExe, argv, App().repoRoot, 30000);
                    std::wstring body = W(r.out);
                    if (body.empty()) body = r.exitCode == 0 ? Str(IDS_MSG_NO_CHANGES) : W(r.err);
                    ShowTextWindow(hwnd, IDS_TITLE_DIFF, App().repoRoot, body);
                    return 0;
                }
                case IDC_SV_OPEN: {
                    auto paths = Selected(st);
                    if (paths.empty()) return 0;
                    const std::wstring arg = L"/select,\"" + paths.front() + L"\"";
                    ::ShellExecuteW(hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }
        case WM_GRT_STATUS_RELOAD:
            RefreshList(hwnd, st);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            LRESULT res = 0;
            if (HandleCtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), &res)) return res;
            break;
        }
        case WM_ERASEBKGND:
            if (Th().dark) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                ::FillRect(reinterpret_cast<HDC>(wp), &rc, Th().panelBrush);
                return 1;
            }
            break;
        case WM_NCDESTROY:
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateStatusView(HWND parent) {
    static bool done = false;
    if (!done) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kStatusViewClass;
        ::RegisterClassExW(&wc);
        done = true;
    }
    return ::CreateWindowExW(0, kStatusViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10,
                             parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

void StatusViewRelayout(HWND view, int x, int y, int w, int h) {
    if (view) ::MoveWindow(view, x, y, w, h, TRUE);
}

void StatusViewReload(HWND view) {
    if (view) ::SendMessageW(view, WM_GRT_STATUS_RELOAD, 0, 0);
}

std::vector<std::wstring> StatusViewSelectedPaths(HWND view) {
    if (!view) return {};
    auto* st = reinterpret_cast<SvState*>(::GetWindowLongPtrW(view, GWLP_USERDATA));
    return st ? Selected(st) : std::vector<std::wstring>{};
}

}  // namespace grt::gui
