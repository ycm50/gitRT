// ---------------------------------------------------------------------------
// 标签窗口（tag.create / tag.push / tag.delete）—— 阶段二专用 GUI 窗口
//
//   上半：标签列表（名称 / 短哈希 / 类型（附注|轻量）/ 日期 / 主题 / 远端有无）
//         —— 数据只来自 core 的 LoadTags（TagInfo.hash 已经**剥到提交**，
//            shortHash 与它一致，附注标签不会显示成 tag 对象的哈希）
//   下半：三块动作
//     · 新建标签：标签名 + 附注标签(-a -m) + 消息 + 目标 rev + 覆盖同名(-f)
//     · 推送标签：单个名字（可从列表/下拉选，也可手输）或「推送全部」(--tags)
//     · 删除标签：本地 + 远端两个勾选（**破坏性 → 必须二次确认**，默认「否」）
//   规则：校验 / 拒绝理由 / argv 全在 core/tag.{h,cpp}（BuildTagPlan /
//         BuildPushTagPlan / BuildDeleteTagPlan / ApplyTagPlan），
//         界面只负责：把 plan.error 原样（中文）显示、把 commandLines 打进
//         只读预览框、确认后丢到工作线程执行、把日志回显出来。
//
//   ★ 执行一律在工作线程（core 里真的会跑 git），线程里只做 core 调用 + PostMessage，
//     完成后由窗口刷新列表 + 让主界面状态重载（照 remote_window 的写法）。
// ---------------------------------------------------------------------------
#include "gui.h"

#include <commctrl.h>

#include <thread>

#include "core.h"
#include "tag.h"

namespace grt::gui {

namespace {

const wchar_t* kTagClass = L"GitRT.TagWindow";

enum : int {
    IDC_TG_LIST = 300,
    IDC_TG_STATUS = 301,
    IDC_TG_PREVIEW = 302,
    IDC_TG_LOG = 303,
    IDC_TG_CREATE = 304,
    IDC_TG_PUSH = 305,
    IDC_TG_DELETE = 306,
    IDC_TG_REFRESH = 307,
    IDC_TG_CLOSE = 308,
    // 输入
    IDC_TG_NAME = 320,
    IDC_TG_TARGET = 321,
    IDC_TG_MESSAGE = 322,
    IDC_TG_REMOTE = 323,
    IDC_TG_PUSHNAME = 324,   // 组合框：CBS_DROPDOWN（可下拉选，也可直接输入）
    // 标签（子控件也建出来，才能进"栈式"布局 —— 照 remote_window 的做法）
    IDC_TG_LBL_LIST = 330,
    IDC_TG_LBL_CREATE = 331,
    IDC_TG_LBL_NAME = 332,
    IDC_TG_LBL_TARGET = 333,
    IDC_TG_LBL_MESSAGE = 334,
    IDC_TG_LBL_PUSH = 335,
    IDC_TG_LBL_DELETE = 336,
    IDC_TG_LBL_REMOTE = 337,
    IDC_TG_LBL_PREVIEW = 338,
    IDC_TG_LBL_LOG = 339,
    // 复选
    IDC_TG_ANNOTATED = 340,
    IDC_TG_FORCE = 341,
    IDC_TG_PUSH_ALL = 342,
    IDC_TG_DEL_LOCAL = 343,
    IDC_TG_DEL_REMOTE = 344,
};

// WM_GRT_TAG_DONE 的 wParam：低 8 位 = 哪个动作跑完了（lParam 按约定恒为 nullptr），
// 高位置 1 = 失败（失败原因由工作线程先写进日志，窗口用 st->lastLine 回填状态行）
enum : int {
    TGOP_NONE = 0,
    TGOP_CREATE = 1,
    TGOP_PUSH = 2,
    TGOP_DELETE = 3,
};
constexpr WPARAM kTgFail = 0x100;

constexpr UINT_PTR kTimerPreview = 1;   // 输入防抖：每个键都重算计划太贵（要跑好几个 git）

// 命令预览框当前显示哪一块的计划（用户最后碰过哪个表单就显示哪个）
enum class PreviewOp { Create, Push, Delete };

struct TgState {
    HWND list = nullptr, status = nullptr, preview = nullptr, log = nullptr;
    HWND create = nullptr, push = nullptr, del = nullptr, refresh = nullptr, close = nullptr;
    HWND nameEdit = nullptr, targetEdit = nullptr, msgEdit = nullptr, remoteEdit = nullptr;
    HWND pushCombo = nullptr;
    HWND annotated = nullptr, force = nullptr, pushAll = nullptr;
    HWND delLocal = nullptr, delRemote = nullptr;
    std::vector<TagInfo> tags;
    bool running = false;
    bool filling = false;    // 刷新列表/下拉中：抑制通知与预览重算
    size_t logBytes = 0;
    std::wstring lastLine;   // 最近一条日志（失败原因回填状态行用）
    PreviewOp previewOp = PreviewOp::Create;
    std::wstring pendingName;   // 正在执行的动作对应的标签名（写"完成"日志用）
};

TgState* StateOf(HWND h) {
    return reinterpret_cast<TgState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
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
void AppendLine(TgState* st, const std::wstring& line) {
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
std::wstring ResultText(TgState* st, bool failed, const std::wstring& okText) {
    if (!failed) return okText;
    if (!st->lastLine.empty()) return st->lastLine;
    return ReplaceAll(Str(IDS_TG_FAILED), L"{msg}", L"未知原因");
}

bool Checked(HWND h) {
    return h && ::SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

int SelectedRow(TgState* st) {
    return static_cast<int>(::SendMessageW(st->list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                                           LVNI_SELECTED));
}

std::wstring KindText(const TagInfo& t) {
    return t.annotated ? Str(IDS_TG_TYPE_ANNOTATED) : Str(IDS_TG_TYPE_LIGHT);
}

// ------------------------------------------------------------------ 列表/下拉
// 拉标签清单。withRemote=true 会多做一次 `git ls-remote` —— 那是**网络调用**，
// 仓库远端慢的时候要等好几秒。所以只有"用户显式点【刷新】"和"写操作完成后"才带上它；
// **开窗时与复用窗口时用本地清单**（毫秒级），远端那一列显示"待核对"而不是假的"否"。
void FillList(TgState* st, bool withRemote = false) {
    st->filling = true;
    ::SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    std::wstring err;
    // 远端列：ls-remote 失败（离线/没有远端）不算致命，core 会把 onRemote 全置 false
    const bool ok = LoadTags(App().gitExe, App().repoRoot, &st->tags, &err,
                             withRemote ? L"origin" : L"-");   // ★ L"-" 才是"只读本地"的哨兵
                                                              //   （传空串会被 PickRemoteName 当成 origin）
    for (size_t i = 0; i < st->tags.size(); ++i) {
        const TagInfo& t = st->tags[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(t.name.c_str());
        ::SendMessageW(st->list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        auto setCol = [&](int col, const std::wstring& s) {
            LVITEMW sub{};
            sub.iSubItem = col;
            sub.pszText = const_cast<wchar_t*>(s.c_str());
            ::SendMessageW(st->list, LVM_SETITEMTEXTW, static_cast<WPARAM>(i),
                           reinterpret_cast<LPARAM>(&sub));
        };
        setCol(1, t.shortHash);
        setCol(2, KindText(t));
        setCol(3, t.date);
        setCol(4, t.subject);
        setCol(5, withRemote ? (t.onRemote ? Str(IDS_TG_REMOTE_YES) : Str(IDS_TG_REMOTE_NO))
                             : L"待核对");
    }
    st->filling = false;
    // 本地清单先显示出来；远端那列还没核对时说清怎么核对（避免把"没查"误读成"没推送"）
    if (st->tags.empty()) {
        SetText(st->status, Str(IDS_TG_NO_TAGS));
    } else if (!ok) {
        SetText(st->status, err);
    } else if (withRemote) {
        SetText(st->status, Str(IDS_TG_SEL_NONE));
    } else {
        SetText(st->status, Str(IDS_TG_SEL_NONE) + L"（远端列显示「待核对」，点「刷新」核对）");
    }
}

// 下拉里的标签名（刷新时**保留用户手输/已选的名字**，别把选择清掉）
void FillCombo(TgState* st, const std::wstring& keep) {
    st->filling = true;
    ::SendMessageW(st->pushCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& t : st->tags) {
        ::SendMessageW(st->pushCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.name.c_str()));
    }
    ::SendMessageW(st->pushCombo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    if (!keep.empty()) SetText(st->pushCombo, keep);
    st->filling = false;
}

// ------------------------------------------------------------------ 计划/预览
// 把计划里的命令与告警写进预览框 + 状态行；失败时把 core 的中文原因原样显示。
// 返回 false = 计划被 core 拒绝（调用方不要再往下执行）。
bool ShowPlan(TgState* st, const TagPlan& plan) {
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
    SetText(st->status, s.empty() ? Str(IDS_TG_PLAN_OK) : s);
    return true;
}

// 依据"用户最后碰过哪一块表单"重算预览（绝不执行任何东西）
void RefreshPreview(TgState* st) {
    if (!st || st->filling) return;

    if (st->previewOp == PreviewOp::Create) {
        const std::wstring name = GetText(st->nameEdit);
        if (name.empty()) {
            SetText(st->status, Str(IDS_TAG_NEED_NAME));
            SetText(st->preview, L"");
            return;
        }
        const TagPlan plan = BuildTagPlan(App().gitExe, App().repoRoot, name, GetText(st->msgEdit),
                                          Checked(st->annotated), GetText(st->targetEdit),
                                          Checked(st->force));
        ShowPlan(st, plan);
        return;
    }

    const std::wstring remote = GetText(st->remoteEdit);
    if (st->previewOp == PreviewOp::Push) {
        const bool all = Checked(st->pushAll);
        const TagPlan plan = BuildPushTagPlan(App().gitExe, App().repoRoot,
                                             all ? std::wstring() : GetText(st->pushCombo), all,
                                             remote);
        ShowPlan(st, plan);
        return;
    }

    // 删除：core 的删除计划**总是**先删本地，所以「只删远端」这种组合必须挡在界面层
    if (!Checked(st->delLocal)) {
        SetText(st->status, Checked(st->delRemote) ? Str(IDS_TG_NEED_LOCAL) : Str(IDS_TG_NEED_ONE));
        SetText(st->preview, L"");
        return;
    }
    const TagPlan plan = BuildDeleteTagPlan(App().gitExe, App().repoRoot, GetText(st->pushCombo),
                                            Checked(st->delRemote), remote);
    ShowPlan(st, plan);
}

// 确认框正文：命令 + 告警（{warns} 自带换行，模板里紧跟"确定继续？"）
std::wstring ConfirmBody(UINT bodyRes, const TagPlan& plan) {
    std::wstring body = Str(bodyRes);
    body = ReplaceAll(body, L"{name}", plan.name);
    body = ReplaceAll(body, L"{cmds}", JoinLines(plan.commandLines, L"> "));
    std::wstring warns;
    for (const auto& w : plan.warnings) warns += L"⚠ " + w + L"\n";
    if (!warns.empty()) warns += L"\n";
    body = ReplaceAll(body, L"{warns}", warns);
    return ToCrlf(body);
}

// ------------------------------------------------------------------ 布局
void LayOut(HWND hwnd, TgState* st) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(10), gap = Scale(6), btnH = Scale(26), btnW = Scale(120);
    const int labelH = Scale(18), editH = Scale(24), rowGap = Scale(4), checkH = Scale(20);
    const int w = rc.right - 2 * pad;
    auto lbl = [&](int id) { return ::GetDlgItem(hwnd, id); };

    int y = pad;
    ::MoveWindow(lbl(IDC_TG_LBL_LIST), pad, y, w, labelH, TRUE);
    y += labelH;
    const int listH = Scale(150);
    ::MoveWindow(st->list, pad, y, w, listH, TRUE);
    y += listH + gap;
    ::MoveWindow(st->status, pad, y, w, labelH, TRUE);
    y += labelH + gap;

    // ---- 新建标签
    ::MoveWindow(lbl(IDC_TG_LBL_CREATE), pad, y, w, labelH, TRUE);
    y += labelH;
    {
        const int colW = (w - gap) / 2;
        ::MoveWindow(lbl(IDC_TG_LBL_NAME), pad, y, colW, labelH, TRUE);
        ::MoveWindow(lbl(IDC_TG_LBL_TARGET), pad + colW + gap, y, colW, labelH, TRUE);
        y += labelH;
        ::MoveWindow(st->nameEdit, pad, y, colW, editH, TRUE);
        ::MoveWindow(st->targetEdit, pad + colW + gap, y, colW, editH, TRUE);
        y += editH + rowGap;
    }
    ::MoveWindow(st->annotated, pad, y, Scale(210), checkH, TRUE);
    ::MoveWindow(st->force, pad + Scale(220), y, Scale(220), checkH, TRUE);
    y += checkH + rowGap;
    ::MoveWindow(lbl(IDC_TG_LBL_MESSAGE), pad, y, w, labelH, TRUE);
    y += labelH;
    ::MoveWindow(st->msgEdit, pad, y, w, editH, TRUE);
    y += editH + gap;

    // ---- 推送标签（名字下拉 + 「推送全部」）
    ::MoveWindow(lbl(IDC_TG_LBL_PUSH), pad, y, Scale(220), labelH, TRUE);
    ::MoveWindow(st->pushAll, pad + w - Scale(230), y - Scale(2), Scale(230), checkH, TRUE);
    y += labelH;
    {
        // 组合框的下拉列表高度只能在"带列表高度"的尺寸里给（MoveWindow 给编辑框高会被规范化成空列表）
        int itemH = static_cast<int>(::SendMessageW(st->pushCombo, CB_GETITEMHEIGHT, 0, 0));
        if (itemH <= 0) itemH = Scale(18);
        ::SetWindowPos(st->pushCombo, nullptr, pad, y, w, editH + itemH * 10,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        y += editH + gap;
    }

    // ---- 删除标签
    ::MoveWindow(lbl(IDC_TG_LBL_DELETE), pad, y, w, labelH, TRUE);
    y += labelH;
    ::MoveWindow(st->delLocal, pad, y, Scale(200), checkH, TRUE);
    ::MoveWindow(st->delRemote, pad + Scale(210), y, Scale(300), checkH, TRUE);
    y += checkH + rowGap;
    ::MoveWindow(lbl(IDC_TG_LBL_REMOTE), pad, y + Scale(3), Scale(180), labelH, TRUE);
    ::MoveWindow(st->remoteEdit, pad + Scale(186), y, Scale(220), editH, TRUE);
    y += editH + gap;

    // ---- 命令预览（只读多行、等宽）
    ::MoveWindow(lbl(IDC_TG_LBL_PREVIEW), pad, y, w, labelH, TRUE);
    y += labelH;
    const int previewH = Scale(48);
    ::MoveWindow(st->preview, pad, y, w, previewH, TRUE);
    y += previewH + gap;

    // ---- 日志（吃掉剩余高度）+ 底部按钮
    const int btnY = rc.bottom - pad - btnH;
    ::MoveWindow(lbl(IDC_TG_LBL_LOG), pad, y, w, labelH, TRUE);
    y += labelH;
    int logH = btnY - gap - y;
    if (logH < Scale(50)) logH = Scale(50);
    ::MoveWindow(st->log, pad, y, w, logH, TRUE);

    int x = rc.right - pad - btnW;
    ::MoveWindow(st->close, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->refresh, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->del, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->push, x, btnY, btnW, btnH, TRUE);
    x -= btnW + gap;
    ::MoveWindow(st->create, x, btnY, btnW, btnH, TRUE);
}

void SetBusy(TgState* st, bool busy) {
    st->running = busy;
    HWND ctrls[] = {st->create,     st->push,      st->del,      st->refresh,   st->close,
                    st->nameEdit,   st->targetEdit, st->msgEdit,  st->remoteEdit, st->pushCombo,
                    st->annotated,  st->force,     st->pushAll,  st->delLocal,  st->delRemote};
    for (HWND h : ctrls) {
        if (h) ::EnableWindow(h, busy ? FALSE : TRUE);
    }
    if (!busy) {
        // 「推送全部」勾上时名字框没意义（--tags 不看名字）
        if (st->pushCombo) ::EnableWindow(st->pushCombo, Checked(st->pushAll) ? FALSE : TRUE);
    }
}

// ------------------------------------------------------------------ 执行
// 计划已定 → 工作线程执行（ApplyTagPlan 是"同一套校验/日志"的执行入口，
// onCommand 已带 "> " 前缀，日志里不能再加一次）
void RunPlan(HWND hwnd, TgState* st, const TagPlan& plan, int op, const std::wstring& doneText) {
    st->running = true;
    SetBusy(st, true);
    SetText(st->status, Str(IDS_TG_RUNNING));
    SetText(st->log, L"");
    st->logBytes = 0;
    st->pendingName = plan.name;

    const std::wstring gitExe = App().gitExe, repoRoot = App().repoRoot;
    const std::wstring failTpl = Str(IDS_TG_FAILED);
    std::thread([hwnd, plan, op, gitExe, repoRoot, doneText, failTpl]() {
        const TagResult r = ApplyTagPlan(
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
        const std::wstring reason = r.error.empty() ? std::wstring(L"未知原因") : r.error;
        const std::wstring line = r.ok ? doneText : ReplaceAll(failTpl, L"{msg}", reason);
        auto* s = new std::string(U8(line) + "\r\n");
        if (!::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
        ::PostMessageW(hwnd, WM_GRT_TAG_DONE, static_cast<WPARAM>(op) | (r.ok ? 0 : kTgFail), 0);
    }).detach();
}

// 「打标签」：core 校验 → 中文原因显示在状态行/预览框 → 确认 → 执行
void DoCreate(HWND hwnd, TgState* st) {
    if (st->running) return;
    st->previewOp = PreviewOp::Create;
    const std::wstring name = GetText(st->nameEdit);
    if (name.empty()) {
        SetText(st->status, Str(IDS_TAG_NEED_NAME));
        ::MessageBoxW(hwnd, Str(IDS_TAG_NEED_NAME).c_str(), Str(IDS_TITLE_TAG).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    const TagPlan plan = BuildTagPlan(App().gitExe, App().repoRoot, name, GetText(st->msgEdit),
                                      Checked(st->annotated), GetText(st->targetEdit),
                                      Checked(st->force));
    if (!ShowPlan(st, plan)) {
        // core 拒绝：原因已经（中文）在状态行与预览框里，再弹一次让用户不会漏看
        ::MessageBoxW(hwnd, plan.error.c_str(), Str(IDS_TITLE_TAG).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    if (::MessageBoxW(hwnd, ConfirmBody(IDS_TG_CONFIRM_BODY, plan).c_str(),
                      Str(IDS_TG_CONFIRM_TITLE).c_str(),
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES) {
        return;
    }
    std::wstring done = Str(IDS_TG_CREATED_OK);
    done = ReplaceAll(done, L"{name}", plan.name);
    RunPlan(hwnd, st, plan, TGOP_CREATE, done);
}

// 「推送」：单个名字 或 --tags 推全部（推全部必须把"会推所有本地标签"的告警醒目显示）
void DoPush(HWND hwnd, TgState* st) {
    if (st->running) return;
    st->previewOp = PreviewOp::Push;
    const bool all = Checked(st->pushAll);
    const TagPlan plan =
        BuildPushTagPlan(App().gitExe, App().repoRoot, all ? std::wstring() : GetText(st->pushCombo),
                         all, GetText(st->remoteEdit));
    if (!ShowPlan(st, plan)) {
        ::MessageBoxW(hwnd, plan.error.c_str(), Str(IDS_TITLE_TAG).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    // 推全部时告警（会把所有本地标签都推上去）在确认框里逐条列出（⚠ 前缀）
    if (::MessageBoxW(hwnd, ConfirmBody(IDS_TG_CONFIRM_BODY, plan).c_str(),
                      Str(IDS_TG_CONFIRM_TITLE).c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    const std::wstring done = all ? std::wstring(L"完成：已推送全部本地标签")
                                  : (L"完成：已推送 " + plan.name);
    RunPlan(hwnd, st, plan, TGOP_PUSH, done);
}

// 「删除」：**必须**二次确认（默认「否」）—— 破坏性动作
void DoDelete(HWND hwnd, TgState* st) {
    if (st->running) return;
    st->previewOp = PreviewOp::Delete;
    if (!Checked(st->delLocal) && !Checked(st->delRemote)) {
        ::MessageBoxW(hwnd, Str(IDS_TG_NEED_ONE).c_str(), Str(IDS_TITLE_TAG).c_str(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!Checked(st->delLocal)) {   // core 的删除计划总是先删本地 → 只删远端表达不出来
        SetText(st->status, Str(IDS_TG_NEED_LOCAL));
        ::MessageBoxW(hwnd, Str(IDS_TG_NEED_LOCAL).c_str(), Str(IDS_TITLE_TAG).c_str(),
                      MB_OK | MB_ICONWARNING);
        return;
    }
    const TagPlan plan = BuildDeleteTagPlan(App().gitExe, App().repoRoot, GetText(st->pushCombo),
                                            Checked(st->delRemote), GetText(st->remoteEdit));
    if (!ShowPlan(st, plan)) {
        ::MessageBoxW(hwnd, plan.error.c_str(), Str(IDS_TITLE_TAG).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    // ★ 破坏性：MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2（默认「否」）
    if (::MessageBoxW(hwnd, ConfirmBody(IDS_TG_DEL_CONFIRM_BODY, plan).c_str(),
                      Str(IDS_TG_DEL_CONFIRM_TITLE).c_str(),
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    const std::wstring done = L"完成：已删除标签 " + plan.name;
    RunPlan(hwnd, st, plan, TGOP_DELETE, done);
}

LRESULT CALLBACK TagProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TgState* st = StateOf(hwnd);
    switch (msg) {
        case WM_CREATE: {
            auto* s = new TgState();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            st = s;
            App().tags = hwnd;

            st->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                             LVS_SHOWSELALWAYS,
                                         0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(IDC_TG_LIST),
                                         ::GetModuleHandleW(nullptr), nullptr);
            ::SendMessageW(st->list, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi), TRUE);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                                             LVS_EX_DOUBLEBUFFER);
            ThemeApplyToTableView(st->list);   // 深色模式下让列表与表头跟着变深（否则是亮白表格）
            struct Col { UINT res; int width; };
            const Col cols[] = {{IDS_TG_COL_NAME, Scale(190)},
                                {IDS_TG_COL_HASH, Scale(80)},
                                {IDS_TG_COL_TYPE, Scale(70)},
                                {IDS_TG_COL_DATE, Scale(90)},
                                {IDS_TG_COL_SUBJECT, Scale(400)},
                                {IDS_TG_COL_REMOTE, Scale(60)}};
            for (int i = 0; i < 6; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                const std::wstring& t = Str(cols[i].res);
                c.pszText = const_cast<wchar_t*>(t.c_str());
                c.cx = cols[i].width;
                c.iSubItem = i;
                ::SendMessageW(st->list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                               reinterpret_cast<LPARAM>(&c));
            }

            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_LIST), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_LIST, Th().fontUi);
            st->status = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                   IDC_TG_STATUS, Th().fontUi);

            // ---- 新建标签
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_CREATE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_CREATE, Th().fontBold);
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_NAME), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_NAME, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_TARGET), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_TARGET, Th().fontUi);
            st->nameEdit = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                              ES_AUTOHSCROLL,
                                     WS_EX_CLIENTEDGE, IDC_TG_NAME, Th().fontUi);
            st->targetEdit = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                                ES_AUTOHSCROLL,
                                       WS_EX_CLIENTEDGE, IDC_TG_TARGET, Th().fontUi);
            st->annotated = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_ANNOTATED),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                      IDC_TG_ANNOTATED, Th().fontUi);
            st->force = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_FORCE),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                  IDC_TG_FORCE, Th().fontUi);
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_MESSAGE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_MESSAGE, Th().fontUi);
            st->msgEdit = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                            ES_AUTOHSCROLL,
                                    WS_EX_CLIENTEDGE, IDC_TG_MESSAGE, Th().fontUi);

            // ---- 推送标签（下拉可选可输）
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_PUSH), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_PUSH, Th().fontBold);
            st->pushAll = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_PUSH_ALL),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                    IDC_TG_PUSH_ALL, Th().fontUi);
            {
                const int totalH = Scale(24) + Scale(18) * 10;   // 编辑框 + 10 行下拉列表
                st->pushCombo = ::CreateWindowExW(
                    WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0,
                    0, Scale(300), totalH, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TG_PUSHNAME)),
                    ::GetModuleHandleW(nullptr), nullptr);
                if (st->pushCombo && Th().fontUi) {
                    ::SendMessageW(st->pushCombo, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi),
                                   TRUE);
                }
            }

            // ---- 删除标签（本地 + 远端；破坏性 → 二次确认）
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_DELETE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_DELETE, Th().fontBold);
            st->delLocal = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_DEL_LOCAL),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                     IDC_TG_DEL_LOCAL, Th().fontUi);
            st->delRemote = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_DEL_REMOTE),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                      IDC_TG_DEL_REMOTE, Th().fontUi);
            ::SendMessageW(st->delLocal, BM_SETCHECK, BST_CHECKED, 0);   // 默认删本地
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_REMOTE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_REMOTE, Th().fontUi);
            st->remoteEdit = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                                ES_AUTOHSCROLL,
                                       WS_EX_CLIENTEDGE, IDC_TG_REMOTE, Th().fontUi);

            // ---- 预览 + 日志
            MakeChild(hwnd, WC_STATICW, Str(IDS_LABEL_COMMAND_LINE), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_PREVIEW, Th().fontUi);
            st->preview = MakeChild(hwnd, WC_EDITW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL,
                                    WS_EX_CLIENTEDGE, IDC_TG_PREVIEW, Th().fontMono);
            MakeChild(hwnd, WC_STATICW, Str(IDS_TG_LBL_LOG), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                      IDC_TG_LBL_LOG, Th().fontUi);
            st->log = MakeChild(hwnd, WC_EDITW, L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                WS_EX_CLIENTEDGE, IDC_TG_LOG, Th().fontMono);

            // ---- 按钮
            st->create = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_BTN_CREATE),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_TG_CREATE,
                                   Th().fontUi);
            st->push = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_BTN_PUSH),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_TG_PUSH,
                                 Th().fontUi);
            st->del = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TG_BTN_DELETE),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_TG_DELETE,
                                Th().fontUi);
            st->refresh = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_REFRESH),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                    IDC_TG_REFRESH, Th().fontUi);
            st->close = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_TG_CLOSE,
                                  Th().fontUi);

            LayOut(hwnd, st);
            FillList(st, false);   // 开窗只读本地清单：`ls-remote` 是网络调用，会让窗口开得很慢
            FillCombo(st, {});
            GRT_LOGI("gui", "标签窗口已打开，标签数=" << st->tags.size());
            return 0;
        }
        case WM_SIZE:
            if (st) LayOut(hwnd, st);
            return 0;
        case WM_ERASEBKGND:
            // 窗口背景用主题刷填（本窗口类注册时用的是 COLOR_WINDOW 浅色画刷，
            // 而深色模式下控件已被 WM_CTLCOLOR* 变深 → 不擦会"深浅割裂"）
            if (HandleEraseBkgnd(hwnd, reinterpret_cast<HDC>(wp))) return 1;
            break;
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
            if (id == IDC_TG_CREATE && code == BN_CLICKED) {
                DoCreate(hwnd, st);
                return 0;
            }
            if (id == IDC_TG_PUSH && code == BN_CLICKED) {
                DoPush(hwnd, st);
                return 0;
            }
            if (id == IDC_TG_DELETE && code == BN_CLICKED) {
                DoDelete(hwnd, st);
                return 0;
            }
            if (id == IDC_TG_REFRESH && code == BN_CLICKED) {
                FillList(st, true);   // 显式刷新：这里做一次远端核对（用户主动等这几秒）
                FillCombo(st, GetText(st->pushCombo));
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_TG_CLOSE && code == BN_CLICKED) {
                if (st->running) {
                    ::MessageBoxW(hwnd, Str(IDS_TG_RUNNING).c_str(), Str(IDS_TITLE_TAG).c_str(),
                                  MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                ::DestroyWindow(hwnd);
                return 0;
            }
            // ---- 输入变化 → 重算预览（防抖；SetText 触发的通知用 filling 抑制）
            if (st->filling) return 0;
            if ((id == IDC_TG_NAME || id == IDC_TG_MESSAGE) && code == EN_CHANGE) {
                st->previewOp = PreviewOp::Create;
                ::SetTimer(hwnd, kTimerPreview, 350, nullptr);
                return 0;
            }
            if (id == IDC_TG_TARGET && code == EN_CHANGE) {
                st->previewOp = PreviewOp::Create;
                ::SetTimer(hwnd, kTimerPreview, 350, nullptr);
                return 0;
            }
            if (id == IDC_TG_REMOTE && code == EN_CHANGE) {
                ::SetTimer(hwnd, kTimerPreview, 350, nullptr);
                return 0;
            }
            if (id == IDC_TG_NAME && code == EN_SETFOCUS) {
                st->previewOp = PreviewOp::Create;
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_TG_PUSHNAME && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) {
                st->previewOp = PreviewOp::Push;
                ::SetTimer(hwnd, kTimerPreview, 350, nullptr);
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_TG_PUSHNAME && code == CBN_SETFOCUS) {
                st->previewOp = PreviewOp::Push;
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_TG_ANNOTATED && code == BN_CLICKED) {
                st->previewOp = PreviewOp::Create;
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_TG_FORCE && code == BN_CLICKED) {
                st->previewOp = PreviewOp::Create;
                RefreshPreview(st);
                return 0;
            }
            if (id == IDC_TG_PUSH_ALL && code == BN_CLICKED) {
                st->previewOp = PreviewOp::Push;
                if (st->pushCombo) {
                    ::EnableWindow(st->pushCombo, Checked(st->pushAll) ? FALSE : TRUE);
                }
                RefreshPreview(st);
                return 0;
            }
            if ((id == IDC_TG_DEL_LOCAL || id == IDC_TG_DEL_REMOTE) && code == BN_CLICKED) {
                st->previewOp = PreviewOp::Delete;
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
            // ★ 选中变化 → 填名字下拉 + 状态行（漏了这段 UI 就不会跟着选）
            if (!st) break;
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->idFrom == IDC_TG_LIST && hdr->code == LVN_ITEMCHANGED && !st->filling) {
                auto* nv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nv->uChanged & LVIF_STATE) &&
                    ((nv->uOldState ^ nv->uNewState) & LVIS_SELECTED)) {
                    const int sel = SelectedRow(st);
                    if (sel >= 0 && static_cast<size_t>(sel) < st->tags.size()) {
                        const TagInfo& t = st->tags[static_cast<size_t>(sel)];
                        // 选中即填"推送/删除"的名字（SetText 的通知用 filling 抑制，
                        // 否则会把预览切成另一块表单）
                        st->filling = true;
                        SetText(st->pushCombo, t.name);
                        st->filling = false;
                        std::wstring text = Str(IDS_TG_SEL_OK);
                        text = ReplaceAll(text, L"{name}", t.name);
                        text = ReplaceAll(text, L"{hash}", t.shortHash);
                        text = ReplaceAll(text, L"{kind}", KindText(t));
                        SetText(st->status, text);
                    } else {
                        SetText(st->status, st->tags.empty() ? Str(IDS_TG_NO_TAGS)
                                                             : Str(IDS_TG_SEL_NONE));
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
        case WM_GRT_TAG_DONE: {
            if (!st) return 0;
            const int op = static_cast<int>(wp) & 0xFF;
            const bool failed = (wp & kTgFail) != 0;
            SetBusy(st, false);   // running=false + 恢复按钮
            const std::wstring keep = GetText(st->pushCombo);
            FillList(st, true);   // 写操作刚改过标签 → 顺手核对一次远端（先刷新再写状态行：刷新过程里的选择通知会把状态行盖掉）
            FillCombo(st, keep);
            std::wstring okText = Str(IDS_TG_DONE);
            if (!failed) {
                if (op == TGOP_CREATE) {
                    okText = ReplaceAll(Str(IDS_TG_CREATED_OK), L"{name}", st->pendingName);
                } else if (op == TGOP_DELETE) {
                    okText = L"完成：已删除标签 " + st->pendingName;
                } else if (op == TGOP_PUSH) {
                    okText = L"完成：已推送 " + st->pendingName;
                }
                GRT_LOGI("gui", "标签动作完成 op=" << op << " name=" << U8(st->pendingName));
                // 打完标签/推完/删完，主界面的状态与提历史都可能变 → 让主窗口重读
                if (HWND main = App().main) ::PostMessageW(main, WM_GRT_STATUS_RELOAD, 1, 0);
            }
            RefreshPreview(st);
            // ★ 顺序：RefreshPreview 会按当前表单重写状态行，完成/失败的话必须**最后**写，
            //   否则用户看不到"完成：…"（remote_window 里踩过同一个坑）
            SetText(st->status, ResultText(st, failed, okText));
            return 0;
        }
        case WM_CLOSE:
            if (st && st->running) {
                ::MessageBoxW(hwnd, Str(IDS_TG_RUNNING).c_str(), Str(IDS_TITLE_TAG).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) ::KillTimer(hwnd, kTimerPreview);
            if (App().tags == hwnd) App().tags = nullptr;
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterTagClass() {
    static bool once = false;
    if (once) return;
    once = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = TagProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kTagClass;
    ::RegisterClassExW(&wc);
}

}  // namespace

void ShowTagWindow(HWND owner) {
    RegisterTagClass();
    if (App().tags && ::IsWindow(App().tags)) {
        ::ShowWindow(App().tags, SW_SHOW);
        ::SetForegroundWindow(App().tags);
        // ★ 复用已有窗口时**必须刷新列表**：这个窗口现在同时是「标签列表」命令的落点，
        //   而那个命令的语义就是"看当前有哪些标签"。期间别人（CLI / 另一处 GUI 操作）可能
        //   刚打过标签，只把旧窗口提到前台会显示**过期数据**。
        if (TgState* st = StateOf(App().tags)) {
            const std::wstring keep = GetText(st->pushCombo);
            FillList(st, false);   // 复用开窗也走本地清单（毫秒级）；要核对远端点窗口里的「刷新」
            FillCombo(st, keep);
        }
        return;
    }
    if (App().repoRoot.empty()) {
        // 自动化期间不弹模态框（会阻塞线程 → 自检挂死）；正常使用时会弹。
        if (!ModalDialogsSuppressed()) {
            ::MessageBoxW(owner, Str(IDS_MSG_NEED_REPO).c_str(), Str(IDS_TITLE_TAG).c_str(),
                          MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kTagClass, Str(IDS_TITLE_TAG).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                               Scale(1040), Scale(920), owner, nullptr, ::GetModuleHandleW(nullptr),
                               nullptr);
    if (h) {
        ThemeApply(h);
        CenterOnOwner(h, owner);
        ::ShowWindow(h, SW_SHOW);
        ::UpdateWindow(h);
    }
}

// 自检用：标签窗口当前列表的行数（窗口没开 / 还没有列表 → -1）。
// 为什么需要它：断言"点开标签列表能看到东西"，光判断"窗口开了"是不够的 ——
// 空壳窗口同样"开了"。这比截图断言稳定，也能在 CI 的 gui_selftest 里跑。
int TagWindowRowCount() {
    if (!App().tags || !::IsWindow(App().tags)) return -1;
    HWND list = ::GetDlgItem(App().tags, IDC_TG_LIST);
    if (!list) return -1;
    return static_cast<int>(::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
}

}  // namespace grt::gui
