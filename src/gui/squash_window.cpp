// ---------------------------------------------------------------------------
// 合并提交窗口（squash）——《产品设计》"复选连续的提交记录，然后合并"
//
//   界面：带复选框的提交列表（第一父链，最新在最上）
//         + 合并后的提交信息（可编辑，勾选变化时自动预填）
//         + 执行日志（每步真实命令行 + 输出）
//   规则：只能勾**连续**的若干条；核心校验全部走 core/squash.{h,cpp}
//         （UI 只做即时提示，真正的判定与拒绝理由由 BuildSquashPlan 给出）
// ---------------------------------------------------------------------------
#include "gui.h"

#include <commctrl.h>

#include <algorithm>

#include <thread>

#include "squash.h"

namespace grt::gui {

namespace {

const wchar_t* kSquashClass = L"GitRT.SquashWindow";

enum : int {
    IDC_SQ_LIST = 300,
    IDC_SQ_STATUS,
    IDC_SQ_MSG,
    IDC_SQ_LOG,
    IDC_SQ_MERGE,
    IDC_SQ_REFRESH,
    IDC_SQ_CLOSE,
    IDC_SQ_LBL_MSG,
    IDC_SQ_LBL_LOG,
};

struct SqState {
    HWND list = nullptr, status = nullptr, msg = nullptr, log = nullptr;
    HWND merge = nullptr, refresh = nullptr, close = nullptr;
    std::vector<CommitEntry> commits;
    bool msgTouched = false;   // 用户手改过信息 → 不再自动覆盖
    bool running = false;
    size_t logBytes = 0;   // AppendLog 的计数器（传 nullptr 会崩）
};

SqState* StateOf(HWND h) { return reinterpret_cast<SqState*>(::GetWindowLongPtrW(h, GWLP_USERDATA)); }

// 勾选状态 → 索引集合
std::vector<size_t> CheckedIndexes(HWND list) {
    std::vector<size_t> out;
    const int n = ::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        LVITEMW item{};
        item.mask = LVIF_STATE;
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.iItem = i;
        const UINT st = static_cast<UINT>(::SendMessageW(list, LVM_GETITEMSTATE, i, LVIS_STATEIMAGEMASK));
        (void)item;
        // 状态图 2 = 勾选（1 = 未勾选）
        if (((st >> 12) & 0xF) == 2) out.push_back(static_cast<size_t>(i));
    }
    return out;
}

void SetStatus(SqState* st) {
    const auto sel = CheckedIndexes(st->list);
    std::wstring text;
    if (sel.empty()) {
        text = Str(IDS_SQ_SEL_NONE);
    } else {
        const size_t n = sel.size();
        bool contig = true;
        for (size_t i = 1; i < n; ++i) {
            if (sel[i] != sel[i - 1] + 1) contig = false;
        }
        std::wstring hasMerge;
        for (const size_t i : sel) {
            if (i < st->commits.size() && st->commits[i].parentCount > 1) {
                hasMerge = st->commits[i].shortHash;
                break;
            }
        }
        if (!hasMerge.empty()) {
            text = Str(IDS_SQ_SEL_MERGE);
        } else if (!contig) {
            text = Str(IDS_SQ_SEL_GAP);
        } else {
            text = Str(IDS_SQ_SEL_OK);
        }
        const std::wstring nStr = std::to_wstring(n);
        if (const size_t pos = text.find(L"{n}"); pos != std::wstring::npos) text.replace(pos, 3, nStr);
    }
    SetText(st->status, text);
    if (st->merge) ::EnableWindow(st->merge, sel.size() >= 2 && !st->running);
    // 自动预填合并信息（用户没手改过才覆盖）
    if (!st->msgTouched) {
        std::vector<CommitEntry> picked;
        for (const size_t i : sel) {
            if (i < st->commits.size()) picked.push_back(st->commits[i]);
        }
        // 列表是新→旧，JoinSubjects 要旧→新
        std::reverse(picked.begin(), picked.end());
        SetTextMl(st->msg, JoinSubjects(picked));
    }
}

// 往日志框追加一行（progress_window 的 AppendLog 在匿名命名空间里，这里自带一份）
void AppendLine(SqState* st, const std::wstring& line) {
    if (!st || !st->log) return;
    if (st->logBytes > 300000) {   // 超长保护
        SetText(st->log, Str(IDS_MSG_LOG_TRUNCATED) + L"\r\n");
        st->logBytes = 0;
    }
    const int len = ::GetWindowTextLengthW(st->log);
    ::SendMessageW(st->log, EM_SETSEL, len, len);
    const std::wstring text = ToCrlf(line) + L"\r\n";
    ::SendMessageW(st->log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    ::SendMessageW(st->log, EM_SCROLLCARET, 0, 0);
    st->logBytes += text.size();
}
void FillList(HWND hwnd, SqState* st) {
    ::SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    st->commits = LoadCommitList(App().gitExe, App().repoRoot, 500);
    for (size_t i = 0; i < st->commits.size(); ++i) {
        const CommitEntry& c = st->commits[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(c.subject.c_str());
        ::SendMessageW(st->list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        auto setCol = [&](int col, const std::wstring& s) {
            LVITEMW sub{};
            sub.iSubItem = col;
            sub.pszText = const_cast<wchar_t*>(s.c_str());
            ::SendMessageW(st->list, LVM_SETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&sub));
        };
        setCol(1, c.shortHash);
        setCol(2, c.date);
        setCol(3, c.author);
    }
    st->msgTouched = false;
    SetStatus(st);
    // 注意：**不清日志** —— 合并完成后要保留"真实命令行 + 输出"（刷新按钮会清）
    if (st->commits.empty()) SetText(st->status, Str(IDS_SQ_NO_COMMITS));
}

void LayOut(HWND hwnd, SqState* st) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(10), gap = Scale(6), btnH = Scale(26), btnW = Scale(130);
    const int labelH = Scale(18);
    const int listH = (rc.bottom - rc.top) / 2;
    int y = pad;
    ::MoveWindow(st->list, pad, y, rc.right - 2 * pad, listH, TRUE);
    y += listH + gap;
    ::MoveWindow(st->status, pad, y, rc.right - 2 * pad, labelH, TRUE);
    y += labelH + gap;
    ::MoveWindow(::GetDlgItem(hwnd, IDC_SQ_LBL_MSG), pad, y, rc.right - 2 * pad, labelH, TRUE);
    y += labelH;
    const int msgH = Scale(24) * 3;
    ::MoveWindow(st->msg, pad, y, rc.right - 2 * pad, msgH, TRUE);
    y += msgH + gap;
    const int logLabelH = labelH;
    ::MoveWindow(::GetDlgItem(hwnd, IDC_SQ_LBL_LOG), pad, y, rc.right - 2 * pad, logLabelH, TRUE);
    y += logLabelH;
    const int btnY = rc.bottom - pad - btnH;
    const int logH = btnY - gap - y;
    ::MoveWindow(st->log, pad, y, rc.right - 2 * pad, logH > Scale(40) ? logH : Scale(40), TRUE);
    int x = rc.right - pad - btnW;
    ::MoveWindow(st->close, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->refresh, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->merge, x, btnY, btnW, btnH, TRUE);
}

// 拼确认框正文
std::wstring ConfirmBody(const SquashPlan& plan) {
    std::wstring body = Str(IDS_SQ_CONFIRM_BODY);
    const std::wstring n = std::to_wstring(plan.commits.size());
    if (const size_t p = body.find(L"{n}"); p != std::wstring::npos) body.replace(p, 3, n);
    if (const size_t p = body.find(L"{msg}"); p != std::wstring::npos) body.replace(p, 5, plan.message);
    if (const size_t p = body.find(L"{mode}"); p != std::wstring::npos) body.replace(p, 6, plan.mode);
    std::wstring cmds;
    for (const auto& c : plan.commandLines) {
        if (!cmds.empty()) cmds += L"\r\n";
        cmds += L"> " + c;
    }
    if (const size_t p = body.find(L"{cmds}"); p != std::wstring::npos) body.replace(p, 6, cmds);
    return ToCrlf(body);
}

void StartMerge(HWND hwnd, SqState* st) {
    const auto sel = CheckedIndexes(st->list);
    if (sel.size() < 2) {
        ::MessageBoxW(hwnd, Str(IDS_SQ_NEED2).c_str(), Str(IDS_TITLE_SQUASH).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::vector<std::wstring> hashes;
    for (const size_t i : sel) {
        if (i < st->commits.size()) hashes.push_back(st->commits[i].hash);
    }
    const std::wstring msg = GetText(st->msg);
    const SquashPlan plan = BuildSquashPlan(App().gitExe, App().repoRoot, hashes, msg);
    if (!plan.ok) {
        ::MessageBoxW(hwnd, plan.error.c_str(), Str(IDS_TITLE_SQUASH).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring body = ConfirmBody(plan);
    if (::MessageBoxW(hwnd, body.c_str(), Str(IDS_SQ_CONFIRM_TITLE).c_str(),
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    st->running = true;
    SetText(st->status, Str(IDS_SQ_RUNNING));
    if (st->merge) ::EnableWindow(st->merge, FALSE);
    SetText(st->log, L"");
    const std::wstring gitExe = App().gitExe;
    const std::wstring repoRoot = App().repoRoot;
    std::thread([hwnd, plan, gitExe, repoRoot]() {
        SquashResult r = ApplySquash(
            gitExe, repoRoot, plan,
            [hwnd](const std::wstring& cmd) {
                auto* s = new std::string(U8(L"> " + cmd) + "\r\n");
                ::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s));
            },
            [hwnd](const std::string& out) {
                if (out.empty()) return;
                auto* s = new std::string(out);
                ::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s));
            });
        ::PostMessageW(hwnd, WM_GRT_SQ_DONE, 0, reinterpret_cast<LPARAM>(new SquashResult(r)));
    }).detach();
}

LRESULT CALLBACK SqProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SqState* st = StateOf(hwnd);
    switch (msg) {
        case WM_CREATE: {
            auto* s = new SqState();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            st = s;
            App().squash = hwnd;

            st->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                         0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(IDC_SQ_LIST),
                                         ::GetModuleHandleW(nullptr), nullptr);
            ::SendMessageW(st->list, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi), TRUE);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                                             LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            struct Col { const wchar_t* text; int width; };
            const Col cols[] = {{Str(IDS_SQ_COL_SUBJECT).c_str(), Scale(420)},
                                {Str(IDS_SQ_COL_HASH).c_str(), Scale(90)},
                                {Str(IDS_SQ_COL_DATE).c_str(), Scale(90)},
                                {Str(IDS_SQ_COL_AUTHOR).c_str(), Scale(120)}};
            for (int i = 0; i < 4; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                c.pszText = const_cast<wchar_t*>(cols[i].text);
                c.cx = cols[i].width;
                c.iSubItem = i;
                ::SendMessageW(st->list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                               reinterpret_cast<LPARAM>(&c));
            }
            st->status = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, IDC_SQ_STATUS,
                                   Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_SQ_LABEL_MSG), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_SQ_LBL_MSG, Th().fontUi);
            st->msg = MakeChild(hwnd, WC_EDITW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN,
                                WS_EX_CLIENTEDGE, IDC_SQ_MSG, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_SQ_LABEL_LOG), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_SQ_LBL_LOG, Th().fontUi);
            st->log = MakeChild(hwnd, WC_EDITW, L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                WS_EX_CLIENTEDGE, IDC_SQ_LOG, Th().fontMono);
            st->merge = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_SQ_MERGE), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     BS_PUSHBUTTON, 0, IDC_SQ_MERGE, Th().fontUi);
            st->refresh = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_REFRESH), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      BS_PUSHBUTTON, 0, IDC_SQ_REFRESH, Th().fontUi);
            st->close = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_PUSHBUTTON, 0, IDC_SQ_CLOSE, Th().fontUi);            LayOut(hwnd, st);
            FillList(hwnd, st);
            GRT_LOGI("gui", "合并提交窗口已打开，提交数=" << st->commits.size());
            return 0;
        }
        case WM_SIZE:
            if (st) LayOut(hwnd, st);
            return 0;
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_SQ_MERGE && code == BN_CLICKED) {
                StartMerge(hwnd, st);
                return 0;
            }
            if (id == IDC_SQ_REFRESH && code == BN_CLICKED) {
                if (st->log) SetText(st->log, L"");   // 手动刷新顺便清日志
                st->logBytes = 0;
                FillList(hwnd, st);
                return 0;
            }
            if (id == IDC_SQ_CLOSE && code == BN_CLICKED) {
                if (st->running) {
                    ::MessageBoxW(hwnd, Str(IDS_SQ_RUNNING).c_str(), Str(IDS_TITLE_SQUASH).c_str(),
                                  MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_SQ_MSG && code == EN_CHANGE) {
                st->msgTouched = true;
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            // ★ 勾选变化 → 刷新状态行 / 预填合并信息 / 启用按钮（漏了这段 UI 就不会响应勾选）
            if (!st) break;
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->idFrom == IDC_SQ_LIST && hdr->code == LVN_ITEMCHANGED) {
                auto* nv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nv->uChanged & LVIF_STATE) &&
                    ((nv->uOldState ^ nv->uNewState) & LVIS_STATEIMAGEMASK)) {
                    SetStatus(st);
                }
            }
            break;
        }        case WM_GRT_TASK_LOG: {
            auto* s = reinterpret_cast<std::string*>(lp);
            if (s && st && st->log) {
                AppendLine(st, W(*s));
            }
            delete s;
            return 0;
        }
        case WM_GRT_SQ_DONE: {
            auto* r = reinterpret_cast<SquashResult*>(lp);
            if (st) {
                st->running = false;
                if (r && r->ok) {
                    std::wstring s = Str(IDS_SQ_DONE);
                    if (const size_t p = s.find(L"{hash}"); p != std::wstring::npos) {
                        s.replace(p, 6, r->newHash.substr(0, 10));
                    }
                    GRT_LOGI("gui", "合并提交完成 new=" << U8(r->newHash));
                    // 列表刷新 + 通知主窗口重读 git status
                    FillList(hwnd, st);
                    SetText(st->status, s);   // 列表刷新后再写"完成"（FillList 会重置状态行）
                    if (HWND main = App().main) {
                        ::PostMessageW(main, WM_GRT_STATUS_RELOAD, 1, 0);
                    }
                } else {
                    const std::wstring err = r ? r->error : L"unknown";
                    SetText(st->status, Str(IDS_SQ_FAILED));
                    AppendLine(st, Str(IDS_SQ_FAILED) + L"：" + err);
                    ::MessageBoxW(hwnd, err.c_str(), Str(IDS_SQ_FAILED).c_str(), MB_OK | MB_ICONERROR);
                    SetStatus(st);
                }
            }
            delete r;
            return 0;
        }
        case WM_CLOSE:
            if (st && st->running) {
                ::MessageBoxW(hwnd, Str(IDS_SQ_RUNNING).c_str(), Str(IDS_TITLE_SQUASH).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (App().squash == hwnd) App().squash = nullptr;
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterSquashClass() {
    static bool once = false;
    if (once) return;
    once = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SqProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSquashClass;
    ::RegisterClassExW(&wc);
}

}  // namespace

void ShowSquashWindow(HWND owner) {
    RegisterSquashClass();
    if (App().squash && ::IsWindow(App().squash)) {
        ::ShowWindow(App().squash, SW_SHOW);
        ::SetForegroundWindow(App().squash);
        return;
    }
    if (App().repoRoot.empty()) {
        ::MessageBoxW(owner, Str(IDS_SQ_NO_COMMITS).c_str(), Str(IDS_TITLE_SQUASH).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kSquashClass, Str(IDS_TITLE_SQUASH).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                               Scale(900), Scale(680), owner, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (h) {
        ThemeApply(h);
        ::ShowWindow(h, SW_SHOW);
        ::UpdateWindow(h);
    }
}

}  // namespace grt::gui
