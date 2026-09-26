// ---------------------------------------------------------------------------
// 远端分支与地址（remote.panel）—— 窗口实现
//
//   上半：远端分支列表（refs/remotes/**）+ 抓取 / 检出 / 新建跟踪分支 / 设为上游
//   下半：远端地址（选一个远端改地址，或填新名字「添加远端」）
//   判定全在 core/remote.{h,cpp}（抓取、改地址、加远端、设上游），日志显示真实命令行与输出。
//
//   执行类操作一律丢到工作线程（core 里真的会跑 git），线程里只做 core 调用 + PostMessage。
// ---------------------------------------------------------------------------
#include "gui.h"

#include <commctrl.h>

#include <thread>

#include "core.h"
#include "remote.h"
#include "restore.h"

namespace grt::gui {

namespace {

const wchar_t* kRemoteClass = L"GitRT.RemoteWindow";

enum : int {
    IDC_RM_LIST = 300,
    IDC_RM_STATUS = 301,
    IDC_RM_LOG = 303,
    IDC_RM_FETCH = 304,
    IDC_RM_CHECKOUT = 305,
    IDC_RM_TRACK = 306,
    IDC_RM_UPSTREAM = 307,
    IDC_RM_CLOSE = 308,
    IDC_RM_SAVE_URL = 309,
    IDC_RM_ADD_REMOTE = 310,
    IDC_RM_COMBO = 320,
    IDC_RM_URL = 321,
    // 标签（照 squash 的做法：标签也建子控件，才能进"栈式"布局）
    IDC_RM_LBL_BRANCHES = 330,
    IDC_RM_LBL_REMOTES = 331,
    IDC_RM_LBL_NAME = 332,
    IDC_RM_LBL_URL = 333,
    IDC_RM_LBL_LOG = 334,
};

// WM_GRT_RM_DONE 的 wParam：低 8 位 = 哪个操作跑完了（lParam 按约定恒为 nullptr），
// 高位置 1 = 失败（失败原因由工作线程先写进日志，窗口用 st->lastLine 回填状态行）。
enum : int {
    RMOP_NONE = 0,
    RMOP_FETCH = 1,
    RMOP_SAVE_URL = 2,
    RMOP_ADD_REMOTE = 3,
    RMOP_UPSTREAM = 4,
    RMOP_CHECKOUT = 5,
    RMOP_TRACK = 6,
};
constexpr WPARAM kRmFail = 0x100;

struct RmState {
    HWND list = nullptr, status = nullptr, log = nullptr;
    HWND fetch = nullptr, checkout = nullptr, track = nullptr, upstream = nullptr, close = nullptr;
    HWND combo = nullptr, urlEdit = nullptr, saveUrl = nullptr, addRemote = nullptr;
    std::vector<RemoteBranch> branches;
    std::vector<RemoteEntry> remotes;
    bool running = false;
    bool filling = false;   // 刷新列表/下拉中：抑制选择通知
    size_t logBytes = 0;
    std::wstring lastLine;  // 最近一条日志（失败原因回填状态行用）
};

RmState* StateOf(HWND h) {
    return reinterpret_cast<RmState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
}

std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    if (from.empty() || from == to) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// 往日志框追加一行（progress_window 的 AppendLog 在匿名命名空间里，这里自带一份）
void AppendLine(RmState* st, const std::wstring& line) {
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
    if (!line.empty()) st->lastLine = line;
}

// 失败时状态行显示 core 给的原因（工作线程刚写进日志的那一行就是"失败：原因"）
std::wstring ResultText(RmState* st, bool failed, UINT okRes) {
    if (!failed) return Str(okRes);
    if (!st->lastLine.empty()) return st->lastLine;
    return ReplaceAll(Str(IDS_RM_FAILED), L"{msg}", L"未知原因");
}

void SetBusy(RmState* st, bool busy) {
    st->running = busy;
    HWND ctrls[] = {st->fetch,       st->checkout, st->track,    st->upstream, st->close,
                    st->saveUrl,     st->addRemote, st->combo,   st->urlEdit};
    for (HWND h : ctrls) {
        if (h) ::EnableWindow(h, busy ? FALSE : TRUE);
    }
}

int SelectedRow(RmState* st) {
    return static_cast<int>(::SendMessageW(st->list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

// origin/feature/x → feature/x（"去掉远端前缀"）
std::wstring LocalNameOf(const std::wstring& remoteName) {
    const size_t p = remoteName.find(L'/');
    return (p == std::wstring::npos) ? remoteName : remoteName.substr(p + 1);
}

// ------------------------------------------------------------------ 列表/下拉
void FillBranches(RmState* st) {
    st->filling = true;
    ::SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    st->branches = LoadRemoteBranches(App().gitExe, App().repoRoot);
    for (size_t i = 0; i < st->branches.size(); ++i) {
        const RemoteBranch& b = st->branches[i];
        std::wstring name = b.name;
        if (b.isUpstream) name += L" [当前上游]";
        if (b.isHead) name += L" [远端默认分支]";
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(name.c_str());
        ::SendMessageW(st->list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        auto setCol = [&](int col, const std::wstring& s) {
            LVITEMW sub{};
            sub.iSubItem = col;
            sub.pszText = const_cast<wchar_t*>(s.c_str());
            ::SendMessageW(st->list, LVM_SETITEMTEXTW, static_cast<WPARAM>(i),
                           reinterpret_cast<LPARAM>(&sub));
        };
        setCol(1, b.shortHash);
        setCol(2, b.date);
        setCol(3, b.subject);
    }
    st->filling = false;
    SetText(st->status, st->branches.empty() ? Str(IDS_RM_NO_BRANCHES) : Str(IDS_RM_SEL_NONE));
}

// prefer：刷新后优先选中的远端名（改地址/加远端成功后停在自己操作的那个上）
void FillRemotes(RmState* st, const std::wstring& prefer = {}) {
    st->filling = true;
    st->remotes = LoadRemotes(App().gitExe, App().repoRoot);
    ::SendMessageW(st->combo, CB_RESETCONTENT, 0, 0);
    int want = -1;
    for (size_t i = 0; i < st->remotes.size(); ++i) {
        ::SendMessageW(st->combo, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(st->remotes[i].name.c_str()));
        if (!prefer.empty() && st->remotes[i].name == prefer) want = static_cast<int>(i);
    }
    if (want < 0 && !st->remotes.empty()) want = 0;
    if (want >= 0) {
        ::SendMessageW(st->combo, CB_SETCURSEL, static_cast<WPARAM>(want), 0);
        SetText(st->urlEdit, st->remotes[static_cast<size_t>(want)].fetchUrl);
    } else {
        ::SendMessageW(st->combo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        SetText(st->combo, L"");
        SetText(st->urlEdit, L"");
    }
    st->filling = false;
}

// ------------------------------------------------------------------ 布局
void LayOut(HWND hwnd, RmState* st) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(10), gap = Scale(6), btnH = Scale(26), btnW = Scale(124);
    const int labelH = Scale(18), editH = Scale(26), rowGap = Scale(4);
    const int w = rc.right - 2 * pad;

    int y = pad;
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RM_LBL_BRANCHES), pad, y, w, labelH, TRUE);
    y += labelH;
    const int listH = Scale(180);
    ::MoveWindow(st->list, pad, y, w, listH, TRUE);
    y += listH + gap;
    ::MoveWindow(st->status, pad, y, w, labelH, TRUE);
    y += labelH + gap;

    // 分支操作按钮：右对齐一行 [抓取][检出][新建跟踪分支][设为上游][关闭]
    {
        const int btnY = y;
        int x = rc.right - pad - btnW;
        ::MoveWindow(st->close, x, btnY, btnW, btnH, TRUE);
        x -= btnW + gap;
        ::MoveWindow(st->upstream, x, btnY, btnW, btnH, TRUE);
        x -= btnW + gap;
        ::MoveWindow(st->track, x, btnY, btnW, btnH, TRUE);
        x -= btnW + gap;
        ::MoveWindow(st->checkout, x, btnY, btnW, btnH, TRUE);
        x -= btnW + gap;
        ::MoveWindow(st->fetch, x, btnY, btnW, btnH, TRUE);
        y += btnH + gap;
    }

    // 下半「远端地址」的高度是固定的 → 先算出来，日志框吃掉中间的剩余空间
    const int blockH = labelH + (labelH + Scale(2) + editH) + rowGap + (labelH + Scale(2) + editH);
    const int bottomTop = rc.bottom - pad - blockH;

    const int logLabelY = y;
    const int logY = logLabelY + labelH;
    int logH = bottomTop - gap - logY;
    if (logH < Scale(50)) logH = Scale(50);
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RM_LBL_LOG), pad, logLabelY, w, labelH, TRUE);
    ::MoveWindow(st->log, pad, logY, w, logH, TRUE);

    int by = bottomTop;
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RM_LBL_REMOTES), pad, by, w, labelH, TRUE);
    by += labelH;
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RM_LBL_NAME), pad, by, w, labelH, TRUE);
    by += labelH + Scale(2);
    // ★ 组合框的下拉列表高度只能在"带列表高度"的尺寸里给（MoveWindow 只给编辑框高会被规范化成空列表）
    {
        int itemH = static_cast<int>(::SendMessageW(st->combo, CB_GETITEMHEIGHT, 0, 0));
        if (itemH <= 0) itemH = Scale(18);
        ::SetWindowPos(st->combo, nullptr, pad, by, w - btnW - gap, editH + itemH * 8,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::MoveWindow(st->addRemote, pad + w - btnW, by, btnW, editH, TRUE);
        by += editH + rowGap;
    }
    ::MoveWindow(::GetDlgItem(hwnd, IDC_RM_LBL_URL), pad, by, w, labelH, TRUE);
    by += labelH + Scale(2);
    ::MoveWindow(st->urlEdit, pad, by, w - btnW - gap, editH, TRUE);
    ::MoveWindow(st->saveUrl, pad + w - btnW, by, btnW, editH, TRUE);
}

// ------------------------------------------------------------------ 操作
void StartFetch(HWND hwnd, RmState* st) {
    if (st->running) return;
    SetBusy(st, true);
    SetText(st->status, Str(IDS_RM_RUNNING));
    AppendLine(st, L"> git fetch --prune");
    const std::wstring done = Str(IDS_RM_DONE), failTpl = Str(IDS_RM_FAILED);
    const std::wstring gitExe = App().gitExe, repoRoot = App().repoRoot;
    std::thread([hwnd, gitExe, repoRoot, done, failTpl]() {
        const std::wstring err = FetchRemote(gitExe, repoRoot);
        const std::wstring line = err.empty() ? done : ReplaceAll(failTpl, L"{msg}", err);
        auto* s = new std::string(U8(line) + "\r\n");
        if (!::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
        ::PostMessageW(hwnd, WM_GRT_RM_DONE,
                       static_cast<WPARAM>(RMOP_FETCH) | (err.empty() ? 0 : kRmFail), 0);
    }).detach();
}

// 计划已定 → 工作线程执行（ApplyRestore 是"同一套校验/日志"的执行入口）
// trackUpstream 非空时（"新建跟踪分支"）跑完再补一次 git branch --set-upstream-to
void RunPlan(HWND hwnd, RmState* st, const RestorePlan& plan, int op,
             const std::wstring& trackUpstream = {}) {
    st->running = true;
    SetText(st->status, Str(IDS_RM_RUNNING));
    SetBusy(st, true);
    // 命令行不在这里预写：ApplyRestore 的 onCommand 会带真实命令行回显；
    // 「新建跟踪分支」补的那条 set-upstream 在 checkout 跑完后由工作线程写日志（保证先后顺序）

    const std::wstring gitExe = App().gitExe, repoRoot = App().repoRoot;
    const std::wstring done = Str(IDS_RM_DONE), failTpl = Str(IDS_RM_FAILED);
    const std::wstring upLine =
        trackUpstream.empty() ? std::wstring() : L"> git branch --set-upstream-to=" + trackUpstream;
    std::thread([hwnd, plan, op, trackUpstream, gitExe, repoRoot, done, failTpl, upLine]() {
        auto postLine = [hwnd](const std::wstring& line) {
            auto* s = new std::string(U8(line) + "\r\n");
            if (!::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
        };
        // ApplyRestore 的 onCommand 已带 "> " 前缀，日志里不能再加一次
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
        std::wstring err = r.error;
        if (r.ok && !trackUpstream.empty()) {
            postLine(upLine);                                     // 建跟踪关系（checkout -b 不会自动建）
            err = SetUpstream(gitExe, repoRoot, trackUpstream);
            if (!err.empty()) r.ok = false;
        }
        postLine(r.ok ? done : ReplaceAll(failTpl, L"{msg}", err));
        ::PostMessageW(hwnd, WM_GRT_RM_DONE, static_cast<WPARAM>(op) | (r.ok ? 0 : kRmFail), 0);
    }).detach();
}

// 检出：把远端分支顶端**只读检出**（分离头指针）—— 不动本地分支、不丢提交，无需二次确认
void StartCheckout(HWND hwnd, RmState* st) {
    if (st->running) return;
    const int sel = SelectedRow(st);
    if (sel < 0 || static_cast<size_t>(sel) >= st->branches.size()) {
        ::MessageBoxW(hwnd, Str(IDS_RM_NEED_BRANCH).c_str(), Str(IDS_TITLE_REMOTE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const RemoteBranch b = st->branches[static_cast<size_t>(sel)];
    const RestorePlan plan =
        BuildRestorePlan(App().gitExe, App().repoRoot, b.shortHash, RestoreMode::DetachCheckout, {}, false);
    if (!plan.ok) {
        SetText(st->status, plan.error);
        return;
    }
    RunPlan(hwnd, st, plan, RMOP_CHECKOUT);
}

// 新建跟踪分支：origin/xxx → 本地分支 xxx，并把它设为上游
void StartTrack(HWND hwnd, RmState* st) {
    if (st->running) return;
    const int sel = SelectedRow(st);
    if (sel < 0 || static_cast<size_t>(sel) >= st->branches.size()) {
        ::MessageBoxW(hwnd, Str(IDS_RM_NEED_BRANCH).c_str(), Str(IDS_TITLE_REMOTE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const RemoteBranch b = st->branches[static_cast<size_t>(sel)];
    const std::wstring local = LocalNameOf(b.name);
    if (local.empty()) return;
    std::wstring body = ReplaceAll(ReplaceAll(Str(IDS_RM_CONFIRM_TRACK), L"{remote}", b.name),
                                  L"{branch}", local);
    if (::MessageBoxW(hwnd, ToCrlf(body).c_str(), Str(IDS_RM_CONFIRM_TITLE).c_str(),
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES) {
        return;
    }
    // ★ BuildRestorePlan 只接受 4~40 位十六进制哈希（core/restore.cpp 的 IsHexHash），
    //   所以起点给**远端分支的短哈希**（给分支名会被判"哈希不合法"），
    //   建完本地分支后再 git branch --set-upstream-to=<origin/xxx>，
    //   效果与 `git checkout -b <local> origin/xxx`（DWIM 自动跟踪）一致。
    const RestorePlan plan =
        BuildRestorePlan(App().gitExe, App().repoRoot, b.shortHash, RestoreMode::NewBranch, local, false);
    if (!plan.ok) {
        SetText(st->status, plan.error);   // 分支名冲突 / 名字非法等：core 的理由直接显示
        return;
    }
    RunPlan(hwnd, st, plan, RMOP_TRACK, b.name);
}

void StartSetUpstream(HWND hwnd, RmState* st) {
    if (st->running) return;
    const int sel = SelectedRow(st);
    if (sel < 0 || static_cast<size_t>(sel) >= st->branches.size()) {
        ::MessageBoxW(hwnd, Str(IDS_RM_NEED_BRANCH).c_str(), Str(IDS_TITLE_REMOTE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring name = st->branches[static_cast<size_t>(sel)].name;
    SetBusy(st, true);
    SetText(st->status, Str(IDS_RM_RUNNING));
    AppendLine(st, L"> git branch --set-upstream-to=" + name);
    const std::wstring done = Str(IDS_RM_DONE), failTpl = Str(IDS_RM_FAILED);
    const std::wstring gitExe = App().gitExe, repoRoot = App().repoRoot;
    std::thread([hwnd, gitExe, repoRoot, name, done, failTpl]() {
        const std::wstring err = SetUpstream(gitExe, repoRoot, name);
        const std::wstring line = err.empty() ? done : ReplaceAll(failTpl, L"{msg}", err);
        auto* s = new std::string(U8(line) + "\r\n");
        if (!::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
        ::PostMessageW(hwnd, WM_GRT_RM_DONE,
                       static_cast<WPARAM>(RMOP_UPSTREAM) | (err.empty() ? 0 : kRmFail), 0);
    }).detach();
}

// 远端地址类操作：git remote set-url / git remote add（core 里会跑 git → 走工作线程）
void StartRemoteOp(HWND hwnd, RmState* st, bool add) {
    if (st->running) return;
    const std::wstring name = GetText(st->combo);
    const std::wstring url = GetText(st->urlEdit);
    const std::wstring shown = W(Redact(url));   // 日志里抹掉 user:token@ 这类凭据
    SetBusy(st, true);
    SetText(st->status, Str(IDS_RM_RUNNING));
    AppendLine(st, (add ? L"> git remote add " : L"> git remote set-url ") + name + L" " + shown);
    const std::wstring okText = Str(add ? IDS_RM_ADD_OK : IDS_RM_URL_OK);
    const std::wstring failTpl = Str(IDS_RM_FAILED);
    const int op = add ? RMOP_ADD_REMOTE : RMOP_SAVE_URL;
    const std::wstring gitExe = App().gitExe, repoRoot = App().repoRoot;
    std::thread([hwnd, add, op, name, url, gitExe, repoRoot, okText, failTpl]() {
        const std::wstring err = add ? AddRemote(gitExe, repoRoot, name, url)
                                     : SetRemoteUrl(gitExe, repoRoot, name, url);
        const std::wstring line = err.empty() ? okText : ReplaceAll(failTpl, L"{msg}", err);
        auto* s = new std::string(U8(line) + "\r\n");
        if (!::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
        ::PostMessageW(hwnd, WM_GRT_RM_DONE, static_cast<WPARAM>(op) | (err.empty() ? 0 : kRmFail), 0);
    }).detach();
}

LRESULT CALLBACK RmProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RmState* st = StateOf(hwnd);
    switch (msg) {
        case WM_CREATE: {
            auto* s = new RmState();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            st = s;
            App().remote = hwnd;

            st->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                             LVS_SHOWSELALWAYS,
                                         0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(IDC_RM_LIST),
                                         ::GetModuleHandleW(nullptr), nullptr);
            ::SendMessageW(st->list, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi), TRUE);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                                             LVS_EX_DOUBLEBUFFER);
            struct Col { UINT res; int width; };
            const Col cols[] = {{IDS_LABEL_BRANCH, Scale(260)},
                                {IDS_SQ_COL_HASH, Scale(90)},
                                {IDS_SQ_COL_DATE, Scale(90)},
                                {IDS_SQ_COL_SUBJECT, Scale(420)}};
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

            MakeChild(hwnd, WC_STATICW, Str(IDS_RM_LBL_BRANCHES), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RM_LBL_BRANCHES, Th().fontUi);
            st->status = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                   IDC_RM_STATUS, Th().fontUi);
            st->fetch = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RM_FETCH),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_RM_FETCH,
                                  Th().fontUi);
            st->checkout = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RM_CHECKOUT),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                     IDC_RM_CHECKOUT, Th().fontUi);
            st->track = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RM_TRACK),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_RM_TRACK,
                                  Th().fontUi);
            st->upstream = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RM_UPSTREAM),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                     IDC_RM_UPSTREAM, Th().fontUi);
            st->close = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_RM_CLOSE,
                                  Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_RM_LBL_LOG), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RM_LBL_LOG, Th().fontUi);
            st->log = MakeChild(hwnd, WC_EDITW, L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                WS_EX_CLIENTEDGE, IDC_RM_LOG, Th().fontMono);
            MakeChild(hwnd, WC_STATICW, Str(IDS_RM_LBL_REMOTES), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RM_LBL_REMOTES, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_RM_LBL_NAME), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RM_LBL_NAME, Th().fontUi);
            // 远端名字：CBS_DROPDOWN —— 列表来自 git remote，也允许直接手输新名字（添加用）
            {
                const int totalH = Scale(26) + Scale(18) * 8;   // 编辑框 + 8 行下拉列表
                st->combo = ::CreateWindowExW(
                    WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0,
                    0, Scale(320), totalH, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RM_COMBO)),
                    ::GetModuleHandleW(nullptr), nullptr);
                if (st->combo && Th().fontUi) {
                    ::SendMessageW(st->combo, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi), TRUE);
                }
            }
            MakeChild(hwnd, WC_STATICW, Str(IDS_RM_LBL_URL), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_RM_LBL_URL, Th().fontUi);
            st->urlEdit = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                             ES_AUTOHSCROLL,
                                    WS_EX_CLIENTEDGE, IDC_RM_URL, Th().fontUi);
            st->saveUrl = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RM_SAVE_URL),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                    IDC_RM_SAVE_URL, Th().fontUi);
            st->addRemote = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_RM_ADD_REMOTE),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                      IDC_RM_ADD_REMOTE, Th().fontUi);

            LayOut(hwnd, st);
            FillBranches(st);
            FillRemotes(st);
            GRT_LOGI("gui", "远端窗口已打开，分支数=" << st->branches.size() << " 远端数="
                                                       << st->remotes.size());
            return 0;
        }
        case WM_SIZE:
            if (st) LayOut(hwnd, st);
            return 0;
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_RM_FETCH && code == BN_CLICKED) {
                StartFetch(hwnd, st);
                return 0;
            }
            if (id == IDC_RM_CHECKOUT && code == BN_CLICKED) {
                StartCheckout(hwnd, st);
                return 0;
            }
            if (id == IDC_RM_TRACK && code == BN_CLICKED) {
                StartTrack(hwnd, st);
                return 0;
            }
            if (id == IDC_RM_UPSTREAM && code == BN_CLICKED) {
                StartSetUpstream(hwnd, st);
                return 0;
            }
            if (id == IDC_RM_SAVE_URL && code == BN_CLICKED) {
                StartRemoteOp(hwnd, st, false);
                return 0;
            }
            if (id == IDC_RM_ADD_REMOTE && code == BN_CLICKED) {
                StartRemoteOp(hwnd, st, true);
                return 0;
            }
            if (id == IDC_RM_CLOSE && code == BN_CLICKED) {
                if (st->running) {
                    ::MessageBoxW(hwnd, Str(IDS_RM_RUNNING).c_str(), Str(IDS_TITLE_REMOTE).c_str(),
                                  MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_RM_COMBO && code == CBN_SELCHANGE) {
                if (st->filling) return 0;
                const int idx = static_cast<int>(::SendMessageW(st->combo, CB_GETCURSEL, 0, 0));
                if (idx >= 0 && static_cast<size_t>(idx) < st->remotes.size()) {
                    SetText(st->urlEdit, st->remotes[static_cast<size_t>(idx)].fetchUrl);
                }
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            // ★ 选中变化 → 状态行（漏了这段 UI 就不会跟着选）
            if (!st) break;
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->idFrom == IDC_RM_LIST && hdr->code == LVN_ITEMCHANGED && !st->filling) {
                auto* nv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nv->uChanged & LVIF_STATE) &&
                    ((nv->uOldState ^ nv->uNewState) & LVIS_SELECTED)) {
                    const int sel = SelectedRow(st);
                    if (sel >= 0 && static_cast<size_t>(sel) < st->branches.size()) {
                        const RemoteBranch& b = st->branches[static_cast<size_t>(sel)];
                        std::wstring text = Str(IDS_RM_SEL_OK);
                        text = ReplaceAll(text, L"{name}", b.name);
                        text = ReplaceAll(text, L"{hash}", b.shortHash);
                        SetText(st->status, text);
                    } else {
                        SetText(st->status, Str(IDS_RM_SEL_NONE));
                    }
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
        case WM_GRT_RM_DONE: {
            if (!st) return 0;
            const int op = static_cast<int>(wp) & 0xFF;
            const bool failed = (wp & kRmFail) != 0;
            SetBusy(st, false);   // running=false + 恢复按钮
            // 先刷新再写状态行：刷新过程里的选择通知会把状态行盖掉
            switch (op) {
                case RMOP_FETCH:
                    FillBranches(st);
                    SetText(st->status, ResultText(st, failed, IDS_RM_DONE));
                    if (HWND main = App().main) ::PostMessageW(main, WM_GRT_STATUS_RELOAD, 1, 0);
                    break;
                case RMOP_SAVE_URL:
                    FillRemotes(st, GetText(st->combo));
                    SetText(st->status, ResultText(st, failed, IDS_RM_URL_OK));
                    break;
                case RMOP_ADD_REMOTE:
                    FillRemotes(st, GetText(st->combo));
                    SetText(st->status, ResultText(st, failed, IDS_RM_ADD_OK));
                    break;
                case RMOP_UPSTREAM:
                    FillBranches(st);
                    SetText(st->status, ResultText(st, failed, IDS_RM_DONE));
                    if (!failed) {
                        if (HWND main = App().main) ::PostMessageW(main, WM_GRT_STATUS_RELOAD, 1, 0);
                    }
                    break;
                case RMOP_CHECKOUT:
                case RMOP_TRACK:
                    FillBranches(st);
                    SetText(st->status, ResultText(st, failed, IDS_RM_DONE));
                    if (!failed) {
                        if (HWND main = App().main) ::PostMessageW(main, WM_GRT_STATUS_RELOAD, 1, 0);
                    }
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            if (st && st->running) {
                ::MessageBoxW(hwnd, Str(IDS_RM_RUNNING).c_str(), Str(IDS_TITLE_REMOTE).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (App().remote == hwnd) App().remote = nullptr;
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterRemoteClass() {
    static bool once = false;
    if (once) return;
    once = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = RmProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kRemoteClass;
    ::RegisterClassExW(&wc);
}

}  // namespace

void ShowRemoteWindow(HWND owner) {
    RegisterRemoteClass();
    if (App().remote && ::IsWindow(App().remote)) {
        ::ShowWindow(App().remote, SW_SHOW);
        ::SetForegroundWindow(App().remote);
        return;
    }
    if (App().repoRoot.empty()) {
        ::MessageBoxW(owner, Str(IDS_RM_NO_BRANCHES).c_str(), Str(IDS_TITLE_REMOTE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kRemoteClass, Str(IDS_TITLE_REMOTE).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                               Scale(1000), Scale(780), owner, nullptr, ::GetModuleHandleW(nullptr),
                               nullptr);
    if (h) {
        ThemeApply(h);
        CenterOnOwner(h, owner);
        ::ShowWindow(h, SW_SHOW);
        ::UpdateWindow(h);
    }
}

}  // namespace grt::gui
