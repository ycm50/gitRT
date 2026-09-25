// ---------------------------------------------------------------------------
// 发布窗口（release.create）—— 阶段二专用 GUI 窗口（GitHub Release，走 gh）
//
//   Release **不是** git 的东西：它由 GitHub CLI（gh.exe）创建，所以本窗口
//   比标签窗口多一层"能力探测"：
//     · 顶部一行 gh 状态：可用时显示 `gh --version` 第一行，不可用时把 core 给的
//       中文原因（怎么装）原样显示，并**禁用**创建表单（绝不崩）
//     · 列表：LoadReleases（`gh release list --json …`），不可用时空列表 + 中文原因
//     · 创建表单：标签（下拉已有标签，也可输入）/ 标题 / 说明（多行）/
//       自动生成说明 / 草稿 / 预发布 / 附件（「选择文件…」可加多个）
//     · 校验 / 拒绝理由 / argv 全在 core/release.{h,cpp}（BuildReleasePlan /
//       ApplyReleasePlan），界面只展示 + 确认 + 在工作线程里执行
//
//   ★ 附件多选：GetOpenFileNameW(OFN_ALLOWMULTISELECT)（comdlg32）。
// ---------------------------------------------------------------------------
#include "gui.h"

#include <commctrl.h>
// ★ 本工程定义了 WIN32_LEAN_AND_MEAN → windows.h 不带 commdlg.h，
//   而「选择文件…」用的 GetOpenFileNameW / OPENFILENAMEW 都在这里
#include <commdlg.h>

#include <thread>

#include "core.h"
#include "release.h"
#include "tag.h"   // LoadTags（标签下拉的兜底来源）

namespace grt::gui {

namespace {

const wchar_t* kReleaseClass = L"GitRT.ReleaseWindow";

enum : int {
    IDC_REL_LIST = 300,
    IDC_REL_STATUS = 301,
    IDC_REL_PREVIEW = 302,
    IDC_REL_LOG = 303,
    IDC_REL_CREATE = 304,
    IDC_REL_REFRESH = 305,
    IDC_REL_CLOSE = 306,
    IDC_REL_GHINFO = 307,
    // 输入
    IDC_REL_TAG = 320,      // CBS_DROPDOWN（可选可输）
    IDC_REL_TITLE = 321,
    IDC_REL_NOTES = 322,    // 多行
    IDC_REL_ASSETS = 323,   // 列表框（已选附件）
    IDC_REL_PICK = 324,
    IDC_REL_REMOVE = 325,
    // 标签
    IDC_REL_LBL_LIST = 330,
    IDC_REL_LBL_CREATE = 331,
    IDC_REL_LBL_TAG = 332,
    IDC_REL_LBL_TITLE = 333,
    IDC_REL_LBL_NOTES = 334,
    IDC_REL_LBL_ASSETS = 335,
    IDC_REL_LBL_PREVIEW = 336,
    IDC_REL_LBL_LOG = 337,
    // 复选
    IDC_REL_GENERATE = 340,
    IDC_REL_DRAFT = 341,
    IDC_REL_PRE = 342,
};

constexpr WPARAM kRelFail = 0x100;
constexpr UINT_PTR kTimerPreview = 1;

struct RelState {
    HWND list = nullptr, status = nullptr, preview = nullptr, log = nullptr, ghInfo = nullptr;
    HWND tagCombo = nullptr, titleEdit = nullptr, notesEdit = nullptr;
    HWND assets = nullptr, pick = nullptr, remove = nullptr;
    HWND generate = nullptr, draft = nullptr, pre = nullptr;
    HWND create = nullptr, refresh = nullptr, close = nullptr;
    std::vector<ReleaseEntry> releases;
    GhInfo gh;
    bool running = false;
    bool filling = false;
    size_t logBytes = 0;
    std::wstring lastLine;
};

RelState* StateOf(HWND h) {
    return reinterpret_cast<RelState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
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

std::wstring JoinLines(const std::vector<std::wstring>& lines, const std::wstring& prefix = {}) {
    std::wstring s;
    for (const auto& l : lines) {
        if (!s.empty()) s += L"\r\n";
        s += prefix + l;
    }
    return s;
}

// 往日志框追加一行（progress_window 的 AppendLog 在匿名命名空间里，这里自带一份）
void AppendLine(RelState* st, const std::wstring& line) {
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

std::wstring ResultText(RelState* st, bool failed, const std::wstring& okText) {
    if (!failed) return okText;
    if (!st->lastLine.empty()) return st->lastLine;
    return ReplaceAll(Str(IDS_REL_FAILED), L"{msg}", L"未知原因");
}

bool Checked(HWND h) {
    return h && ::SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// ------------------------------------------------------------------ 附件选择
std::vector<std::wstring> PickFiles(HWND owner) {
    std::vector<wchar_t> buf(32768, 0);
    const std::wstring title = Str(IDS_REL_BTN_PICK);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = title.c_str();
    // OFN_NOCHANGEDIR：别把 GUI 进程的当前目录改到用户选文件的地方
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT |
                OFN_NOCHANGEDIR;

    std::vector<std::wstring> out;
    if (!::GetOpenFileNameW(&ofn)) return out;
    // 多选：目录\0文件1\0文件2\0\0；只选一个：完整路径\0\0
    const wchar_t* p = buf.data();
    const std::wstring dir = p;
    p += dir.size() + 1;
    if (*p == L'\0') {
        out.push_back(dir);
        return out;
    }
    while (*p != L'\0') {
        const std::wstring f = p;
        p += f.size() + 1;
        out.push_back(dir + L"\\" + f);
    }
    return out;
}

std::vector<std::wstring> SelectedAssets(RelState* st) {
    std::vector<std::wstring> out;
    if (!st->assets) return out;
    const int n = static_cast<int>(::SendMessageW(st->assets, LB_GETCOUNT, 0, 0));
    for (int i = 0; i < n; ++i) {
        const int len = static_cast<int>(::SendMessageW(st->assets, LB_GETTEXTLEN, i, 0));
        if (len <= 0) continue;
        std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');   // LB_GETTEXT 会写结尾的 NUL
        ::SendMessageW(st->assets, LB_GETTEXT, i, reinterpret_cast<LPARAM>(buf.data()));
        out.push_back(std::wstring(buf.data()));
    }
    return out;
}

// ------------------------------------------------------------------ gh 能力
// 不可用时：顶部写中文原因 + 禁用创建表单（列表照旧尝试读，失败也只是空列表 + 原因）
void ApplyGhCapability(RelState* st) {
    st->gh = DetectGh();
    if (st->gh.available) {
        SetText(st->ghInfo, L"GitHub CLI（gh）可用：" +
                                (st->gh.version.empty() ? st->gh.exe : st->gh.version));
    } else {
        SetText(st->ghInfo, st->gh.error.empty() ? Str(IDS_RELEASE_NEED_GH) : st->gh.error);
    }
    const bool on = st->gh.available && !st->running;
    HWND ctrls[] = {st->tagCombo, st->titleEdit, st->notesEdit, st->assets, st->pick, st->remove,
                    st->generate, st->draft,     st->pre,       st->create};
    for (HWND h : ctrls) {
        if (h) ::EnableWindow(h, on ? TRUE : FALSE);
    }
    if (!st->gh.available) SetText(st->status, Str(IDS_RELEASE_NEED_GH));
}

// ------------------------------------------------------------------ 列表/下拉
void FillList(RelState* st) {
    st->filling = true;
    ::SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    std::wstring err;
    // gh 不可用 / 未登录 / 不是 GitHub 仓库 → 空列表 + 中文 error，绝不崩
    const bool ok = LoadReleases(App().gitExe, App().repoRoot, &st->releases, &err);
    for (size_t i = 0; i < st->releases.size(); ++i) {
        const ReleaseEntry& e = st->releases[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        std::wstring tag = e.tag;
        it.pszText = const_cast<wchar_t*>(tag.c_str());
        ::SendMessageW(st->list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        auto setCol = [&](int col, const std::wstring& s) {
            LVITEMW sub{};
            sub.iSubItem = col;
            sub.pszText = const_cast<wchar_t*>(s.c_str());
            ::SendMessageW(st->list, LVM_SETITEMTEXTW, static_cast<WPARAM>(i),
                           reinterpret_cast<LPARAM>(&sub));
        };
        setCol(1, e.name.empty() ? e.tag : e.name);
        setCol(2, e.publishedAt);
        std::wstring state;
        if (e.draft) state = Str(IDS_REL_DRAFT_TAG);
        if (e.prerelease) state += (state.empty() ? L"" : L" / ") + Str(IDS_REL_PRE_TAG);
        setCol(3, state);
    }
    st->filling = false;
    if (!ok) {
        SetText(st->status, st->gh.available ? err : Str(IDS_RELEASE_NEED_GH));
    } else {
        SetText(st->status, st->releases.empty() ? Str(IDS_REL_NO_RELEASES) : Str(IDS_REL_SEL_NONE));
    }
}

// 标签下拉：先问 ListBranches(ParamSource::Tags)（与参数面板同源），为空则退回 LoadTags
void FillTagCombo(RelState* st, const std::wstring& keep) {
    st->filling = true;
    ::SendMessageW(st->tagCombo, CB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> names = ListBranches(ParamSource::Tags);
    if (names.empty()) {
        std::vector<TagInfo> tags;
        std::wstring err;
        if (LoadTags(App().gitExe, App().repoRoot, &tags, &err, L"origin")) {
            for (const auto& t : tags) names.push_back(t.name);
        }
    }
    for (const auto& n : names) {
        ::SendMessageW(st->tagCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n.c_str()));
    }
    ::SendMessageW(st->tagCombo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    if (!keep.empty()) SetText(st->tagCombo, keep);
    st->filling = false;
}

// ------------------------------------------------------------------ 计划/预览
bool ShowPlan(RelState* st, const ReleasePlan& plan) {
    if (!plan.ok) {
        SetText(st->status, plan.error);
        SetText(st->preview, L"");
        return false;
    }
    SetTextMl(st->preview, JoinLines(plan.commandLines));
    std::wstring s;
    for (const auto& w : plan.warnings) {
        if (!s.empty()) s += L"；";
        s += L"⚠ " + w;
    }
    if (s.empty()) {   // 没告警就报「将为 <tag> 创建发布」（占位符一定要替换掉）
        s = ReplaceAll(Str(IDS_REL_SEL_OK), L"{tag}", plan.tag);
    }
    SetText(st->status, s);
    return true;
}

// 用当前表单重新构造计划（只算不跑）；gh 不可用时直接显示中文原因
ReleasePlan BuildFromForm(RelState* st) {
    return BuildReleasePlan(App().gitExe, App().repoRoot, GetText(st->tagCombo),
                            GetText(st->titleEdit), GetText(st->notesEdit), Checked(st->draft),
                            Checked(st->pre), /*pushTag=*/false, SelectedAssets(st),
                            Checked(st->generate));
}

void RefreshPreview(RelState* st) {
    if (!st || st->filling) return;
    if (!st->gh.available) {
        SetText(st->status, Str(IDS_RELEASE_NEED_GH));
        SetText(st->preview, L"");
        return;
    }
    if (!Trim(GetText(st->tagCombo)).empty()) {
        // ShowPlan 内部已处理"拒绝"：把 core 的中文原因写状态行 + 清空预览
        ShowPlan(st, BuildFromForm(st));
        return;
    }
    SetText(st->status, Str(IDS_REL_SEL_NONE));
    SetText(st->preview, L"");
}

std::wstring ConfirmBody(const ReleasePlan& plan) {
    std::wstring body = Str(IDS_REL_CONFIRM_BODY);
    body = ReplaceAll(body, L"{cmds}", JoinLines(plan.commandLines, L"> "));
    std::wstring warns;
    for (const auto& w : plan.warnings) warns += L"⚠ " + w + L"\n";
    if (!warns.empty()) warns += L"\n";
    body = ReplaceAll(body, L"{warns}", warns);
    return ToCrlf(body);
}

// ------------------------------------------------------------------ 布局
void LayOut(HWND hwnd, RelState* st) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(10), gap = Scale(6), btnH = Scale(26), btnW = Scale(120);
    const int labelH = Scale(18), editH = Scale(24), rowGap = Scale(4), checkH = Scale(20);
    const int w = rc.right - 2 * pad;
    auto lbl = [&](int id) { return ::GetDlgItem(hwnd, id); };

    int y = pad;
    ::MoveWindow(st->ghInfo, pad, y, w, labelH, TRUE);
    y += labelH + rowGap;
    ::MoveWindow(lbl(IDC_REL_LBL_LIST), pad, y, w, labelH, TRUE);
    y += labelH;
    const int listH = Scale(150);
    ::MoveWindow(st->list, pad, y, w, listH, TRUE);
    y += listH + gap;
    ::MoveWindow(st->status, pad, y, w, labelH, TRUE);
    y += labelH + gap;

    // ---- 创建发布
    ::MoveWindow(lbl(IDC_REL_LBL_CREATE), pad, y, w, labelH, TRUE);
    y += labelH;
    {
        const int colW = (w - gap) / 2;
        ::MoveWindow(lbl(IDC_REL_LBL_TAG), pad, y, colW, labelH, TRUE);
        ::MoveWindow(lbl(IDC_REL_LBL_TITLE), pad + colW + gap, y, colW, labelH, TRUE);
        y += labelH;
        int itemH = static_cast<int>(::SendMessageW(st->tagCombo, CB_GETITEMHEIGHT, 0, 0));
        if (itemH <= 0) itemH = Scale(18);
        ::SetWindowPos(st->tagCombo, nullptr, pad, y, colW, editH + itemH * 10,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::MoveWindow(st->titleEdit, pad + colW + gap, y, colW, editH, TRUE);
        y += editH + rowGap;
    }
    ::MoveWindow(lbl(IDC_REL_LBL_NOTES), pad, y, w, labelH, TRUE);
    y += labelH;
    const int notesH = Scale(66);
    ::MoveWindow(st->notesEdit, pad, y, w, notesH, TRUE);
    y += notesH + rowGap;

    ::MoveWindow(st->generate, pad, y, Scale(280), checkH, TRUE);
    ::MoveWindow(st->draft, pad + Scale(290), y, Scale(210), checkH, TRUE);
    ::MoveWindow(st->pre, pad + Scale(510), y, Scale(210), checkH, TRUE);
    y += checkH + rowGap;

    // ---- 附件：[标签][选择文件…][移除所选] + 已选列表
    ::MoveWindow(lbl(IDC_REL_LBL_ASSETS), pad, y + Scale(3), Scale(260), labelH, TRUE);
    ::MoveWindow(st->remove, pad + w - btnW, y, btnW, editH, TRUE);
    ::MoveWindow(st->pick, pad + w - btnW * 2 - gap, y, btnW, editH, TRUE);
    y += editH + rowGap;
    const int assetsH = Scale(66);
    ::MoveWindow(st->assets, pad, y, w, assetsH, TRUE);
    y += assetsH + gap;

    // ---- 命令预览
    ::MoveWindow(lbl(IDC_REL_LBL_PREVIEW), pad, y, w, labelH, TRUE);
    y += labelH;
    const int previewH = Scale(48);
    ::MoveWindow(st->preview, pad, y, w, previewH, TRUE);
    y += previewH + gap;

    // ---- 日志 + 按钮
    const int btnY = rc.bottom - pad - btnH;
    ::MoveWindow(lbl(IDC_REL_LBL_LOG), pad, y, w, labelH, TRUE);
    y += labelH;
    int logH = btnY - gap - y;
    if (logH < Scale(50)) logH = Scale(50);
    ::MoveWindow(st->log, pad, y, w, logH, TRUE);

    int x = rc.right - pad - btnW;
    ::MoveWindow(st->close, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->refresh, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->create, x, btnY, btnW + Scale(20), btnH, TRUE);
}

void SetBusy(RelState* st, bool busy) {
    st->running = busy;
    if (busy) {
        HWND ctrls[] = {st->create, st->refresh, st->close, st->tagCombo, st->titleEdit,
                        st->notesEdit, st->assets, st->pick, st->remove, st->generate, st->draft,
                        st->pre};
        for (HWND h : ctrls) {
            if (h) ::EnableWindow(h, FALSE);
        }
        ::EnableWindow(st->close, TRUE);
        return;
    }
    if (st->refresh) ::EnableWindow(st->refresh, TRUE);
    ApplyGhCapability(st);   // 恢复时按"gh 是否可用"决定创建表单能不能用
}

// ------------------------------------------------------------------ 执行
void RunPlan(HWND hwnd, RelState* st, const ReleasePlan& plan) {
    st->running = true;
    SetBusy(st, true);
    SetText(st->status, Str(IDS_REL_RUNNING));
    SetText(st->log, L"");
    st->logBytes = 0;

    const std::wstring repoRoot = App().repoRoot;
    const std::wstring failTpl = Str(IDS_REL_FAILED);
    std::thread([hwnd, plan, repoRoot, failTpl]() {
        const ReleaseResult r = ApplyReleasePlan(
            repoRoot, plan,
            [hwnd](const std::wstring& cmd) {
                auto* s = new std::string(U8(cmd) + "\r\n");
                ::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s));
            },
            [hwnd](const std::string& out) {
                if (out.empty()) return;
                auto* s = new std::string(out);
                ::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s));
            });
        const std::wstring reason = r.error.empty() ? std::wstring(L"未知原因") : r.error;
        const std::wstring line = r.ok ? std::wstring(L"完成：发布已创建") : ReplaceAll(failTpl, L"{msg}", reason);
        auto* s = new std::string(U8(line) + "\r\n");
        ::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s));
        ::PostMessageW(hwnd, WM_GRT_REL_DONE, static_cast<WPARAM>(r.ok ? 0 : kRelFail), 0);
    }).detach();
}

void DoCreate(HWND hwnd, RelState* st) {
    if (st->running) return;
    if (!st->gh.available) {   // 没装 gh：表单本来是禁用的，这里再兜一次底
        ::MessageBoxW(hwnd, Str(IDS_RELEASE_NEED_GH).c_str(), Str(IDS_TITLE_RELEASE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring tag = Trim(GetText(st->tagCombo));
    if (tag.empty()) {
        ::MessageBoxW(hwnd, Str(IDS_TAG_NEED_NAME).c_str(), Str(IDS_TITLE_RELEASE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const ReleasePlan plan = BuildFromForm(st);
    if (!ShowPlan(st, plan)) {
        // core 拒绝（标签不存在 / 附件找不到 / gh 不可用…）：中文原因已进状态行与预览框
        ::MessageBoxW(hwnd, plan.error.c_str(), Str(IDS_TITLE_RELEASE).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    if (::MessageBoxW(hwnd, ConfirmBody(plan).c_str(), Str(IDS_REL_CONFIRM_TITLE).c_str(),
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES) {
        return;
    }
    GRT_LOGI("gui", "创建发布 tag=" << U8(plan.tag) << " assets=" << plan.assets.size());
    RunPlan(hwnd, st, plan);
}

LRESULT CALLBACK RelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RelState* st = StateOf(hwnd);
    switch (msg) {
        case WM_CREATE: {
            auto* s = new RelState();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            st = s;
            App().releases = hwnd;

            st->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                             LVS_SHOWSELALWAYS,
                                         0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(IDC_REL_LIST),
                                         ::GetModuleHandleW(nullptr), nullptr);
            ::SendMessageW(st->list, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi), TRUE);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                                             LVS_EX_DOUBLEBUFFER);
            struct Col { UINT res; int width; };
            const Col cols[] = {{IDS_REL_COL_TAG, Scale(160)},
                                {IDS_REL_COL_NAME, Scale(430)},
                                {IDS_REL_COL_DATE, Scale(180)},
                                {IDS_REL_COL_FLAGS, Scale(120)}};
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

            st->ghInfo = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                   IDC_REL_GHINFO, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_REL_LBL_LIST), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_LIST, Th().fontUi);
            st->status = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                   IDC_REL_STATUS, Th().fontUi);

            // ---- 创建表单
            MakeChild(hwnd, WC_STATICW, Str(IDS_REL_LBL_CREATE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_CREATE, Th().fontBold);
            MakeChild(hwnd, WC_STATICW, Str(IDS_REL_LBL_TAG), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_TAG, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_REL_LBL_TITLE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_TITLE, Th().fontUi);
            {
                const int totalH = Scale(24) + Scale(18) * 10;
                st->tagCombo = ::CreateWindowExW(
                    WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0,
                    0, Scale(300), totalH, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REL_TAG)),
                    ::GetModuleHandleW(nullptr), nullptr);
                if (st->tagCombo && Th().fontUi) {
                    ::SendMessageW(st->tagCombo, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi),
                                   TRUE);
                }
            }
            st->titleEdit = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                                ES_AUTOHSCROLL,
                                      WS_EX_CLIENTEDGE, IDC_REL_TITLE, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_REL_LBL_NOTES), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_NOTES, Th().fontUi);
            st->notesEdit = MakeChild(hwnd, WC_EDITW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                          ES_AUTOVSCROLL | ES_WANTRETURN,
                                      WS_EX_CLIENTEDGE, IDC_REL_NOTES, Th().fontUi);
            st->generate = MakeChild(hwnd, WC_BUTTONW, Str(IDS_REL_GENERATE),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                     IDC_REL_GENERATE, Th().fontUi);
            st->draft = MakeChild(hwnd, WC_BUTTONW, Str(IDS_REL_DRAFT_CHK),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                  IDC_REL_DRAFT, Th().fontUi);
            st->pre = MakeChild(hwnd, WC_BUTTONW, Str(IDS_REL_PRE_CHK),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_REL_PRE,
                                Th().fontUi);

            // ---- 附件
            MakeChild(hwnd, WC_STATICW, Str(IDS_REL_LBL_ASSETS), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_ASSETS, Th().fontUi);
            st->assets = MakeChild(hwnd, WC_LISTBOXW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                       LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL,
                                   WS_EX_CLIENTEDGE, IDC_REL_ASSETS, Th().fontUi);
            st->pick = MakeChild(hwnd, WC_BUTTONW, Str(IDS_REL_BTN_PICK),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_REL_PICK,
                                 Th().fontUi);
            st->remove = MakeChild(hwnd, WC_BUTTONW, Str(IDS_REL_BTN_REMOVE),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                   IDC_REL_REMOVE, Th().fontUi);

            // ---- 预览 + 日志
            MakeChild(hwnd, WC_STATICW, Str(IDS_LABEL_COMMAND_LINE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_PREVIEW, Th().fontUi);
            st->preview = MakeChild(hwnd, WC_EDITW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL,
                                    WS_EX_CLIENTEDGE, IDC_REL_PREVIEW, Th().fontMono);
            MakeChild(hwnd, WC_STATICW, Str(IDS_REL_LBL_LOG), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_REL_LBL_LOG, Th().fontUi);
            st->log = MakeChild(hwnd, WC_EDITW, L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                WS_EX_CLIENTEDGE, IDC_REL_LOG, Th().fontMono);

            // ---- 按钮
            st->create = MakeChild(hwnd, WC_BUTTONW, Str(IDS_REL_BTN_CREATE),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                   IDC_REL_CREATE, Th().fontUi);
            st->refresh = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_REFRESH),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                    IDC_REL_REFRESH, Th().fontUi);
            st->close = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_REL_CLOSE,
                                  Th().fontUi);

            LayOut(hwnd, st);
            ApplyGhCapability(st);   // 先探测 gh（不可用 → 禁用表单 + 中文原因）
            FillList(st);
            FillTagCombo(st, {});
            GRT_LOGI("gui", "发布窗口已打开，gh=" << (st->gh.available ? "可用" : "不可用")
                                                  << " 发布数=" << st->releases.size());
            return 0;
        }
        case WM_SIZE:
            if (st) LayOut(hwnd, st);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            LRESULT res = 0;
            if (HandleCtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), &res)) {
                return res;
            }
            break;
        }
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_REL_CREATE && code == BN_CLICKED) {
                DoCreate(hwnd, st);
                return 0;
            }
            if (id == IDC_REL_REFRESH && code == BN_CLICKED) {
                ApplyGhCapability(st);
                FillList(st);
                FillTagCombo(st, GetText(st->tagCombo));
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_REL_CLOSE && code == BN_CLICKED) {
                if (st->running) {
                    ::MessageBoxW(hwnd, Str(IDS_REL_RUNNING).c_str(), Str(IDS_TITLE_RELEASE).c_str(),
                                  MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_REL_PICK && code == BN_CLICKED) {
                if (!st->gh.available) return 0;
                for (const auto& f : PickFiles(hwnd)) {
                    if (::SendMessageW(st->assets, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                       reinterpret_cast<LPARAM>(f.c_str())) == LB_ERR) {
                        ::SendMessageW(st->assets, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(f.c_str()));
                    }
                }
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_REL_REMOVE && code == BN_CLICKED) {
                // 从后往前删，避免索引位移
                for (int i = static_cast<int>(::SendMessageW(st->assets, LB_GETCOUNT, 0, 0)) - 1;
                     i >= 0; --i) {
                    if (::SendMessageW(st->assets, LB_GETSEL, static_cast<WPARAM>(i), 0) > 0) {
                        ::SendMessageW(st->assets, LB_DELETESTRING, static_cast<WPARAM>(i), 0);
                    }
                }
                RefreshPreview(st);
                return 0;
            }
            if (st->filling) return 0;
            if ((id == IDC_REL_GENERATE && code == BN_CLICKED) ||
                (id == IDC_REL_DRAFT && code == BN_CLICKED) ||
                (id == IDC_REL_PRE && code == BN_CLICKED)) {
                RefreshPreview(st);
                return 0;
            }
            if ((id == IDC_REL_TAG && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) ||
                (id == IDC_REL_TITLE && code == EN_CHANGE) ||
                (id == IDC_REL_NOTES && code == EN_CHANGE)) {
                ::SetTimer(hwnd, kTimerPreview, 350, nullptr);
                return 0;
            }
            if (id == IDC_REL_TAG && code == CBN_SETFOCUS) {
                RefreshPreview(st);
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (wp == kTimerPreview && st) {
                ::KillTimer(hwnd, kTimerPreview);
                RefreshPreview(st);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            // 列表选中 → 把标签填进表单（方便"给这条发布再加点东西"或看命令）
            if (!st || st->filling) break;
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->idFrom == IDC_REL_LIST && hdr->code == LVN_ITEMCHANGED) {
                auto* nv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nv->uChanged & LVIF_STATE) &&
                    ((nv->uOldState ^ nv->uNewState) & LVIS_SELECTED)) {
                    const int sel = static_cast<int>(::SendMessageW(
                        st->list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
                    if (sel >= 0 && static_cast<size_t>(sel) < st->releases.size()) {
                        const ReleaseEntry& e = st->releases[static_cast<size_t>(sel)];
                        st->filling = true;
                        SetText(st->tagCombo, e.tag);
                        st->filling = false;
                        std::wstring ok = ReplaceAll(Str(IDS_REL_SEL_OK), L"{tag}", e.tag);
                        SetText(st->status, ok);
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
        case WM_GRT_REL_DONE: {
            if (!st) return 0;
            const bool failed = (wp & kRelFail) != 0;
            SetBusy(st, false);   // 恢复按钮 + 按 gh 可用性恢复表单
            FillList(st);         // 先刷新再写状态行（刷新会把状态行盖掉）
            if (!failed) {
                GRT_LOGI("gui", "发布创建完成");
                if (HWND main = App().main) ::PostMessageW(main, WM_GRT_STATUS_RELOAD, 1, 0);
            }
            SetText(st->preview, L"");
            RefreshPreview(st);
            SetText(st->status, ResultText(st, failed, Str(IDS_REL_DONE)));
            return 0;
        }
        case WM_CLOSE:
            if (st && st->running) {
                ::MessageBoxW(hwnd, Str(IDS_REL_RUNNING).c_str(), Str(IDS_TITLE_RELEASE).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) ::KillTimer(hwnd, kTimerPreview);
            if (App().releases == hwnd) App().releases = nullptr;
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterReleaseClass() {
    static bool once = false;
    if (once) return;
    once = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = RelProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kReleaseClass;
    ::RegisterClassExW(&wc);
}

}  // namespace

void ShowReleaseWindow(HWND owner) {
    RegisterReleaseClass();
    if (App().releases && ::IsWindow(App().releases)) {
        ::ShowWindow(App().releases, SW_SHOW);
        ::SetForegroundWindow(App().releases);
        return;
    }
    if (App().repoRoot.empty()) {
        ::MessageBoxW(owner, Str(IDS_MSG_NEED_REPO).c_str(), Str(IDS_TITLE_RELEASE).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kReleaseClass, Str(IDS_TITLE_RELEASE).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                               Scale(1040), Scale(890), owner, nullptr, ::GetModuleHandleW(nullptr),
                               nullptr);
    if (h) {
        ThemeApply(h);
        CenterOnOwner(h, owner);
        ::ShowWindow(h, SW_SHOW);
        ::UpdateWindow(h);
    }
}

}  // namespace grt::gui
