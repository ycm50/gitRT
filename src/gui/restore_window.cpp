// ---------------------------------------------------------------------------
// 还原到提交（history.restore）—— 窗口实现
//
//   界面：提交列表（第一父链，最新在最上）
//         + 还原方式单选（只读检出 / 新建分支 / 重置 soft·mixed·hard）
//         + 新分支名（仅"新建分支"时可用）
//         + 状态行（选中摘要 + 校验失败原因 + 警告）
//         + 命令预览（**不落盘、不执行**，随选择/方式实时重算）
//         + 执行日志（每步真实命令行 + 输出）
//   规则：校验 / 拒绝理由 / argv 全在 core/restore.{h,cpp}，界面只负责展示与确认；
//         「脏工作区 + 硬重置」要二次确认，之后再走一次总确认。
// ---------------------------------------------------------------------------
#include "gui.h"

#include <commctrl.h>

#include <thread>

#include "core.h"
#include "restore.h"
#include "squash.h"   // CommitEntry / LoadCommitList（提交列表与 squash 窗口同源）

namespace grt::gui {

namespace {

const wchar_t* kRestoreClass = L"GitRT.RestoreWindow";

enum : int {
    IDC_RST_LIST = 300,
    IDC_RST_STATUS = 301,
    IDC_RST_PREVIEW = 302,
    IDC_RST_LOG = 303,
    IDC_RST_DO = 304,
    IDC_RST_REFRESH = 305,
    IDC_RST_CLOSE = 306,
    IDC_RST_LBL_COMMITS = 307,
    IDC_RST_LBL_MODE = 308,
    IDC_RST_LBL_BRANCH = 309,
    IDC_RST_MODE_BASE = 310,   // 310..314：5 个还原方式单选项（第一个带 WS_GROUP）
    IDC_RST_MODE_END = 314,
    IDC_RST_LBL_PREVIEW = 315,
    IDC_RST_LBL_LOG = 316,
    IDC_RST_BRANCH = 320,
};

constexpr UINT_PTR kTimerBranch = 1;   // 「新分支名」输入的防抖定时器（每个键都重算计划太贵）

struct RstState {
    HWND list = nullptr, status = nullptr, preview = nullptr, log = nullptr;
    HWND doBtn = nullptr, refresh = nullptr, close = nullptr, branchEdit = nullptr;
    HWND modes[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};   // 310..314
    std::vector<CommitEntry> commits;
    bool running = false;
    bool filling = false;        // 列表重建中：抑制 WM_NOTIFY 里的重算
    bool branchTouched = false;  // 用户手改过分支名 → 不再自动预填
    size_t logBytes = 0;         // AppendLine 的计数器（传 nullptr 会崩）
    // 正在执行的计划摘要：core 的 RestoreResult 不带 mode/hash，写"完成"日志要用它
    std::wstring pendingShort;
    RestoreMode pendingMode = RestoreMode::DetachCheckout;
};

RstState* StateOf(HWND h) {
    return reinterpret_cast<RstState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
}

// 占位符替换：squash 窗口里也是这么拼的（资源串里带 {x} 的都在这里填）
std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    if (from.empty() || from == to) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::wstring JoinLines(const std::vector<std::wstring>& lines, const std::wstring& prefix = {}) {
    std::wstring s;
    for (const auto& l : lines) {
        if (!s.empty()) s += L"\r\n";
        s += prefix + l;
    }
    return s;
}

// 往日志框追加一行（progress_window 的 AppendLog 在匿名命名空间里，这里自带一份）
void AppendLine(RstState* st, const std::wstring& line) {
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

// -------------------------------------------------------------- 还原方式单选
RestoreMode ModeFromId(int id) {
    switch (id) {
        case IDC_RST_MODE_BASE + 1: return RestoreMode::NewBranch;
        case IDC_RST_MODE_BASE + 2: return RestoreMode::ResetSoft;
        case IDC_RST_MODE_BASE + 3: return RestoreMode::ResetMixed;
        case IDC_RST_MODE_BASE + 4: return RestoreMode::ResetHard;
        default:                    return RestoreMode::DetachCheckout;
    }
}

RestoreMode CurrentMode(RstState* st) {
    for (int i = 0; i < 5; ++i) {
        if (st->modes[i] && ::SendMessageW(st->modes[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            return ModeFromId(IDC_RST_MODE_BASE + i);
        }
    }
    return RestoreMode::DetachCheckout;
}

int SelectedRow(RstState* st) {
    return static_cast<int>(::SendMessageW(st->list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

// ------------------------------------------------------------------ 计划预览
// 重新用当前"选中提交 + 还原方式 + 分支名"生成计划：成功显示命令行与警告，
// 失败（校验拒绝）把 core 给的原因原样写在状态行上，并禁用「还原」。
void RefreshPlan(RstState* st, bool autoFillBranch) {
    if (!st || st->filling) return;
    const RestoreMode mode = CurrentMode(st);
    const int sel = SelectedRow(st);

    if (st->branchEdit) {
        ::EnableWindow(st->branchEdit, mode == RestoreMode::NewBranch && !st->running && sel >= 0);
    }
    if (sel < 0 || static_cast<size_t>(sel) >= st->commits.size()) {
        SetText(st->status, Str(IDS_RST_SEL_NONE));
        SetText(st->preview, L"");
        if (st->doBtn) ::EnableWindow(st->doBtn, FALSE);
        return;
    }

    const CommitEntry& c = st->commits[static_cast<size_t>(sel)];
    if (autoFillBranch && st->branchEdit && !st->branchTouched) {
        SetText(st->branchEdit, L"restore-" + c.shortHash);
        st->branchTouched = false;   // SetText 会触发 EN_CHANGE，把 touched 又置真
    }
    const std::wstring branchName = st->branchEdit ? GetText(st->branchEdit) : std::wstring();

    // 预览**只算不跑**：forceHard=false（forceHard 只影响 needsExtraConfirm）
    const RestorePlan plan =
        BuildRestorePlan(App().gitExe, App().repoRoot, c.hash, mode, branchName, false);
    if (!plan.ok) {
        SetText(st->status, plan.error);
        SetText(st->preview, L"");
        if (st->doBtn) ::EnableWindow(st->doBtn, FALSE);
        return;
    }

    std::wstring text = Str(IDS_RST_SEL_OK);
    text = ReplaceAll(text, L"{mode}", RestoreModeLabel(mode));
    text = ReplaceAll(text, L"{hash}", plan.shortHash);
    text = ReplaceAll(text, L"{subject}", plan.subject);
    for (const auto& w : plan.warnings) text += L"；" + w;   // 状态行是单行静态框 → 用分号串起来
    SetText(st->status, text);
    SetTextMl(st->preview, JoinLines(plan.commandLines));    // 多行必须 ToCrlf（否则挤一行）
    if (st->doBtn) ::EnableWindow(st->doBtn, !st->running);
}

// 重新读提交列表；列表为空时在状态行提示「还没有提交」
void FillList(HWND hwnd, RstState* st) {
    st->filling = true;
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
            ::SendMessageW(st->list, LVM_SETITEMTEXTW, static_cast<WPARAM>(i),
                           reinterpret_cast<LPARAM>(&sub));
        };
        setCol(1, c.shortHash);
        setCol(2, c.date);
        setCol(3, c.author);
    }
    st->filling = false;
    st->branchTouched = false;
    SetText(st->preview, L"");
    SetText(st->status, st->commits.empty() ? Str(IDS_RST_NO_COMMITS) : Str(IDS_RST_SEL_NONE));
    if (st->branchEdit) ::EnableWindow(st->branchEdit, FALSE);
    if (st->doBtn) ::EnableWindow(st->doBtn, FALSE);
    (void)hwnd;
}

// ------------------------------------------------------------------ 布局
void LayOut(HWND hwnd, RstState* st) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(10), gap = Scale(6), btnH = Scale(26), btnW = Scale(130);
    const int labelH = Scale(18), editH = Scale(24), radioH = Scale(22);
    const int w = rc.right - 2 * pad;

    int y = pad;
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RST_LBL_COMMITS), pad, y, w, labelH, TRUE);
    y += labelH;
    const int listH = Scale(180);
    ::MoveWindow(st->list, pad, y, w, listH, TRUE);
    y += listH + gap;
    ::MoveWindow(st->status, pad, y, w, labelH, TRUE);
    y += labelH + gap;

    // 还原方式：一组单选（第一个在创建时带 WS_GROUP）
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RST_LBL_MODE), pad, y, w, labelH, TRUE);
    y += labelH;
    for (int id = IDC_RST_MODE_BASE; id <= IDC_RST_MODE_END; ++id) {
        ::MoveWindow(::GetDlgItem(hwnd, id), pad + Scale(8), y, w - Scale(8), radioH, TRUE);
        y += radioH;
    }
    y += gap;

    // 新分支名（标签与编辑框同一行）
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RST_LBL_BRANCH), pad, y + Scale(3), Scale(90), labelH, TRUE);
    ::MoveWindow(st->branchEdit, pad + Scale(96), y, Scale(260), editH, TRUE);
    y += editH + gap;

    // 命令预览（只读多行）
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RST_LBL_PREVIEW), pad, y, w, labelH, TRUE);
    y += labelH;
    const int previewH = editH * 2;
    ::MoveWindow(st->preview, pad, y, w, previewH, TRUE);
    y += previewH + gap;

    // 执行日志：占满剩余高度
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RST_LBL_LOG), pad, y, w, labelH, TRUE);
    y += labelH;
    const int btnY = rc.bottom - pad - btnH;
    int logH = btnY - gap - y;
    if (logH < Scale(50)) logH = Scale(50);
    ::MoveWindow(st->log, pad, y, w, logH, TRUE);

    // 按钮：右下角 [还原][刷新][关闭]
    int x = rc.right - pad - btnW;
    ::MoveWindow(st->close, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->refresh, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->doBtn, x, btnY, btnW, btnH, TRUE);
}

// ------------------------------------------------------------------ 确认正文
std::wstring ConfirmBody(const RestorePlan& plan) {
    std::wstring body = Str(IDS_RST_CONFIRM_BODY);
    body = ReplaceAll(body, L"{mode}", RestoreModeLabel(plan.mode));
    body = ReplaceAll(body, L"{hash}", plan.shortHash);
    body = ReplaceAll(body, L"{subject}", plan.subject);
    body = ReplaceAll(body, L"{cmds}", JoinLines(plan.commandLines, L"> "));
    std::wstring warns;
    for (const auto& w : plan.warnings) {
        if (!warns.empty()) warns += L"\r\n";
        warns += L"⚠ " + w;
    }
    if (!warns.empty()) warns += L"\r\n\r\n";
    body = ReplaceAll(body, L"{warns}", warns);
    return ToCrlf(body);
}

// 计划已定 → 工作线程里执行；线程里只做 core 调用 + PostMessage
void RunPlan(HWND hwnd, RstState* st, const RestorePlan& plan) {
    st->running = true;
    st->pendingShort = plan.shortHash;
    st->pendingMode = plan.mode;
    SetText(st->status, Str(IDS_RST_RUNNING));
    if (st->doBtn) ::EnableWindow(st->doBtn, FALSE);
    if (st->refresh) ::EnableWindow(st->refresh, FALSE);
    if (st->branchEdit) ::EnableWindow(st->branchEdit, FALSE);
    SetText(st->log, L"");
    st->logBytes = 0;
    // 命令行不在这里预写：ApplyRestore 的 onCommand 会带真实命令行回显（写两遍会重复）

    const std::wstring gitExe = App().gitExe;
    const std::wstring repoRoot = App().repoRoot;
    std::thread([hwnd, plan, gitExe, repoRoot]() {
        // 注意：ApplyRestore 的 onCommand 已经带了 "> " 前缀，这里不能再加一次
        RestoreResult r = ApplyRestore(
            gitExe, repoRoot, plan,
            [hwnd](const std::wstring& cmd) {
                auto* s = new std::string(U8(cmd) + "\r\n");
                if (!::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
            },
            [hwnd](const std::string& out) {
                if (out.empty()) return;
                auto* s = new std::string(out);
                if (!::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
            });
        // 窗口已销毁时 PostMessageW 失败 → 自己回收结果对象，避免泄漏
        auto* done = new RestoreResult(std::move(r));
        if (!::PostMessageW(hwnd, WM_GRT_RST_DONE, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

// 「还原」：重新算计划 → (脏工作区+硬重置) 二次确认 → 总确认 → 执行
void StartRestore(HWND hwnd, RstState* st) {
    if (st->running) return;
    const int sel = SelectedRow(st);
    if (sel < 0 || static_cast<size_t>(sel) >= st->commits.size()) {
        ::MessageBoxW(hwnd, Str(IDS_RST_NEED_COMMIT).c_str(), Str(IDS_TITLE_RESTORE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const CommitEntry c = st->commits[static_cast<size_t>(sel)];
    const RestoreMode mode = CurrentMode(st);
    const std::wstring branchName = st->branchEdit ? GetText(st->branchEdit) : std::wstring();
    const std::wstring title = Str(IDS_TITLE_RESTORE);

    RestorePlan plan = BuildRestorePlan(App().gitExe, App().repoRoot, c.hash, mode, branchName, false);
    if (!plan.ok) {
        SetText(st->status, plan.error);
        ::MessageBoxW(hwnd, plan.error.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    // 脏工作区 + 硬重置：先二次确认，确认后才 forceHard=true 生成可执行的计划
    if (plan.needsExtraConfirm) {
        if (::MessageBoxW(hwnd, ToCrlf(Str(IDS_RST_HARD_BODY)).c_str(),
                          Str(IDS_RST_HARD_TITLE).c_str(),
                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        plan = BuildRestorePlan(App().gitExe, App().repoRoot, c.hash, mode, branchName, true);
        if (!plan.ok) {
            SetText(st->status, plan.error);
            return;
        }
    }
    // 总确认
    if (::MessageBoxW(hwnd, ConfirmBody(plan).c_str(), Str(IDS_RST_CONFIRM_TITLE).c_str(),
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    GRT_LOGI("gui", "还原到提交 mode=" << U8(RestoreModeKey(mode)) << " target=" << U8(plan.shortHash));
    RunPlan(hwnd, st, plan);
}

LRESULT CALLBACK RstProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RstState* st = StateOf(hwnd);
    switch (msg) {
        case WM_CREATE: {
            auto* s = new RstState();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            st = s;
            App().restore = hwnd;

            st->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                             LVS_SHOWSELALWAYS,
                                         0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(IDC_RST_LIST),
                                         ::GetModuleHandleW(nullptr), nullptr);
            ::SendMessageW(st->list, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi), TRUE);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                                             LVS_EX_DOUBLEBUFFER);
            struct Col { UINT res; int width; };
            const Col cols[] = {{IDS_SQ_COL_SUBJECT, Scale(420)},
                                {IDS_SQ_COL_HASH, Scale(90)},
                                {IDS_SQ_COL_DATE, Scale(90)},
                                {IDS_SQ_COL_AUTHOR, Scale(120)}};
            for (int i = 0; i < 4; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                const std::wstring& t = Str(cols[i].res);
                c.pszText = const_cast<wchar_t*>(t.c_str());
                c.cx = cols[i].width;
                c.iSubItem = i;
                ::SendMessageW(st->list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                               reinterpret_cast<LPARAM>(&c));
            }

            MakeChild(hwnd, WC_STATICW, Str(IDS_RST_LBL_COMMITS), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RST_LBL_COMMITS, Th().fontUi);
            st->status = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                   IDC_RST_STATUS, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_RST_LABEL_MODE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RST_LBL_MODE, Th().fontUi);
            // 5 个还原方式单选项：第一个带 WS_GROUP（否则整窗只有一组，行为不可预期）
            const UINT modeRes[5] = {IDS_RST_MODE_DETACH, IDS_RST_MODE_BRANCH, IDS_RST_MODE_SOFT,
                                     IDS_RST_MODE_MIXED, IDS_RST_MODE_HARD};
            for (int i = 0; i < 5; ++i) {
                DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
                if (i == 0) style |= WS_GROUP;
                HWND r = MakeChild(hwnd, WC_BUTTONW, Str(modeRes[i]), style, 0,
                                   IDC_RST_MODE_BASE + i, Th().fontUi);
                st->modes[i] = r;
                if (i == 0) ::SendMessageW(r, BM_SETCHECK, BST_CHECKED, 0);   // 默认只读检出
            }
            MakeChild(hwnd, WC_STATICW, Str(IDS_RST_LBL_BRANCH), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RST_LBL_BRANCH, Th().fontUi);
            st->branchEdit = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                               ES_AUTOHSCROLL,
                                       WS_EX_CLIENTEDGE, IDC_RST_BRANCH, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_LABEL_COMMAND_LINE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RST_LBL_PREVIEW, Th().fontUi);
            st->preview = MakeChild(hwnd, WC_EDITW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                        ES_READONLY | ES_AUTOVSCROLL,
                                    WS_EX_CLIENTEDGE, IDC_RST_PREVIEW, Th().fontMono);
            MakeChild(hwnd, WC_STATICW, Str(IDS_RST_LBL_LOG), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RST_LBL_LOG, Th().fontUi);
            st->log = MakeChild(hwnd, WC_EDITW, L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                WS_EX_CLIENTEDGE, IDC_RST_LOG, Th().fontMono);
            st->doBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RST_DO),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_RST_DO,
                                  Th().fontUi);
            st->refresh = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RST_REFRESH),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                    IDC_RST_REFRESH, Th().fontUi);
            st->close = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_RST_CLOSE,
                                  Th().fontUi);
            LayOut(hwnd, st);
            FillList(hwnd, st);
            GRT_LOGI("gui", "还原到提交窗口已打开，提交数=" << st->commits.size());
            return 0;
        }
        case WM_SIZE:
            if (st) LayOut(hwnd, st);
            return 0;
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_RST_DO && code == BN_CLICKED) {
                StartRestore(hwnd, st);
                return 0;
            }
            if (id == IDC_RST_REFRESH && code == BN_CLICKED) {
                FillList(hwnd, st);   // 日志保留：里面是"跑过什么"的记录
                return 0;
            }
            if (id == IDC_RST_CLOSE && code == BN_CLICKED) {
                if (st->running) {
                    ::MessageBoxW(hwnd, Str(IDS_RST_RUNNING).c_str(), Str(IDS_TITLE_RESTORE).c_str(),
                                  MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id >= IDC_RST_MODE_BASE && id <= IDC_RST_MODE_END && code == BN_CLICKED) {
                RefreshPlan(st, false);   // 换方式 → 重算预览与校验
                return 0;
            }
            if (id == IDC_RST_BRANCH && code == EN_CHANGE) {
                st->branchTouched = true;
                // 每敲一个键就算一次计划太贵（BuildRestorePlan 要跑好几个 git）→ 防抖
                ::SetTimer(hwnd, kTimerBranch, 300, nullptr);
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (wp == kTimerBranch && st) {
                ::KillTimer(hwnd, kTimerBranch);
                RefreshPlan(st, false);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            // ★ 选中变化 → 立即重算计划（漏了这段 UI 就不会响应选择）
            if (!st) break;
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->idFrom == IDC_RST_LIST && hdr->code == LVN_ITEMCHANGED) {
                auto* nv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nv->uChanged & LVIF_STATE) &&
                    ((nv->uOldState ^ nv->uNewState) & LVIS_SELECTED)) {
                    RefreshPlan(st, true);
                }
            }
            break;
        }
        case WM_GRT_TASK_LOG: {
            auto* s = reinterpret_cast<std::string*>(lp);
            if (s && st && st->log) AppendLine(st, W(*s));
            delete s;
            return 0;
        }
        case WM_GRT_RST_DONE: {
            auto* r = reinterpret_cast<RestoreResult*>(lp);
            if (st) {
                st->running = false;
                const std::wstring title = Str(IDS_TITLE_RESTORE);
                if (r && r->ok) {
                    std::wstring s = Str(IDS_RST_DONE);
                    s = ReplaceAll(s, L"{mode}", RestoreModeLabel(st->pendingMode));
                    s = ReplaceAll(s, L"{hash}", st->pendingShort);
                    AppendLine(st, s);
                    GRT_LOGI("gui", "还原完成 mode=" << U8(RestoreModeKey(st->pendingMode)));
                    // 提交列表可能已经变了（重置/检出）→ 刷新，并让主窗口重读 git status
                    FillList(hwnd, st);
                    RefreshPlan(st, false);   // 清空预览/重算按钮可用性（刷新后通常已无选中）
                    SetText(st->status, s);   // 列表刷新会重置状态行 → 再写"完成"
                    if (HWND main = App().main) ::PostMessageW(main, WM_GRT_STATUS_RELOAD, 1, 0);
                } else {
                    const std::wstring err = r ? r->error : std::wstring(L"unknown");
                    const std::wstring s = ReplaceAll(Str(IDS_RST_FAILED), L"{msg}", err);
                    AppendLine(st, s);
                    ::MessageBoxW(hwnd, err.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
                    // 计划本身仍可能有效（比如 git 拒绝执行）→ 按当前选择恢复按钮状态
                    SetText(st->status, s);
                    RefreshPlan(st, false);
                    SetText(st->status, s);
                }
                if (st->refresh) ::EnableWindow(st->refresh, TRUE);
            }
            delete r;
            return 0;
        }
        case WM_CLOSE:
            if (st && st->running) {
                ::MessageBoxW(hwnd, Str(IDS_RST_RUNNING).c_str(), Str(IDS_TITLE_RESTORE).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) ::KillTimer(hwnd, kTimerBranch);
            if (App().restore == hwnd) App().restore = nullptr;
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterRestoreClass() {
    static bool once = false;
    if (once) return;
    once = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = RstProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kRestoreClass;
    ::RegisterClassExW(&wc);
}

}  // namespace

void ShowRestoreWindow(HWND owner) {
    RegisterRestoreClass();
    if (App().restore && ::IsWindow(App().restore)) {
        ::ShowWindow(App().restore, SW_SHOW);
        ::SetForegroundWindow(App().restore);
        return;
    }
    if (App().repoRoot.empty()) {
        ::MessageBoxW(owner, Str(IDS_RST_NO_COMMITS).c_str(), Str(IDS_TITLE_RESTORE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kRestoreClass, Str(IDS_TITLE_RESTORE).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                               Scale(980), Scale(780), owner, nullptr, ::GetModuleHandleW(nullptr),
                               nullptr);
    if (h) {
        ThemeApply(h);
        CenterOnOwner(h, owner);
        ::ShowWindow(h, SW_SHOW);
        ::UpdateWindow(h);
    }
}

}  // namespace grt::gui
