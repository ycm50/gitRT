// 参数面板：由 CommandSpec 驱动（参数 + flags 复选 + 实时命令行预览）
// （《技术实现设计》§4.3 层级 2 / §9.2）
#include "gui.h"

#include <algorithm>
#include <map>

namespace grt::gui {

const wchar_t* kParamPanelClass = L"GitRT.ParamPanel";

enum : int {
    IDC_PP_EXEC = 1001,
    IDC_PP_COPY = 1002,
    IDC_PP_PARAM_BASE = 4000,
    IDC_PP_FLAG_BASE = 5000,
};
enum : UINT_PTR { kTimerDisarm = 7 };

namespace {

struct CtrlInfo {
    HWND           h = nullptr;
    std::string    key;
    FlagKind       kind = FlagKind::Toggle;
    std::string    radioGroup;
    bool           isParam = false;
    ParamKind      paramKind = ParamKind::None;
};

struct PanelState {
    CommandSpec                         spec{};
    std::vector<std::wstring>           paths;
    std::map<std::string, std::wstring> flags, params;
    std::vector<CtrlInfo>               ctrls;
    std::vector<std::pair<HWND, int>>   stack;   // 顺序布局：控件 + 高度（负 = 拉伸剩余）
    HWND title = nullptr, hint = nullptr, flagsLabel = nullptr, previewLabel = nullptr, preview = nullptr;
    HWND execBtn = nullptr, copyBtn = nullptr;
    bool armed = false;
    BuiltCommand built;
    std::wstring buildMsg;
};

// 创建参数（WM_NCCREATE 时一次性拷进 PanelState）。
// ★ 为什么必须在这里给 paths/flags：WM_CREATE 里就要算预览与默认值，
//   若创建后再赋值（旧实现），首帧预览与"忽略模式默认值"会按空选区计算。
struct PanelCreateParams {
    const CommandSpec*                  spec = nullptr;
    std::vector<std::wstring>           paths;
    std::map<std::string, std::wstring> flags;
};

// flags 初值是否打开（接受 "1"/"true"/"yes"）
bool FlagOn(const std::map<std::string, std::wstring>& flags, const std::string& key, bool def) {
    const auto it = flags.find(key);
    if (it == flags.end()) return def;
    const std::wstring v = ToLowerAscii(it->second);
    if (v == L"1" || v == L"true" || v == L"yes") return true;
    if (v == L"0" || v == L"false" || v == L"no") return false;
    return def;
}

UINT ParamLabelRes(ParamKind k) {
    switch (k) {
        case ParamKind::CommitMessage: return IDS_PARAM_MSG;
        case ParamKind::BranchName:    return IDS_PARAM_BRANCH;
        case ParamKind::ExistingBranch:return IDS_PARAM_BRANCH;
        case ParamKind::RemoteName:    return IDS_PARAM_REMOTE;
        case ParamKind::Revision:      return IDS_PARAM_REVISION;
        case ParamKind::TagName:       return IDS_PARAM_TAG;
        case ParamKind::Url:           return IDS_PARAM_URL;
        case ParamKind::Pattern:       return IDS_PARAM_PATTERN;
        default:                       return IDS_PARAM_MSG;
    }
}

bool IsMultiline(ParamKind k) {
    return k == ParamKind::CommitMessage || k == ParamKind::Pattern;
}

void SyncValues(PanelState* st) {
    for (auto& c : st->ctrls) {
        if (c.isParam) {
            st->params[c.key] = GetText(c.h);
            continue;
        }
        switch (c.kind) {
            case FlagKind::Radio:
            case FlagKind::Toggle:
            case FlagKind::Dangerous: {
                const bool on = ::SendMessageW(c.h, BM_GETCHECK, 0, 0) == BST_CHECKED;
                st->flags[c.key] = on ? L"1" : L"0";
                break;
            }
            case FlagKind::Value:
                st->flags[c.key] = GetText(c.h);
                break;
        }
    }
}

void RebuildPreview(PanelState* st) {
    SyncValues(st);
    if (st->spec.exec == ExecKind::Internal) {
        SetText(st->preview, Str(IDS_MSG_INTERNAL_NOTE));
        st->built = BuiltCommand{};
        st->buildMsg.clear();
        ::EnableWindow(st->execBtn, TRUE);
        return;
    }
    BuildInput in;
    in.spec = &st->spec;
    in.gitExe = App().gitExe;
    in.repoRoot = App().repoRoot;
    in.cwd = App().repoRoot;
    in.paths = st->paths;
    in.flags = st->flags;
    in.params = st->params;

    BuildError err;
    BuiltCommand out;
    if (BuildCommand(in, &out, &err)) {
        st->built = out;
        std::wstring text = out.display;
        for (const auto& n : out.notes) text += L"\r\n-- " + n;
        SetText(st->preview, text);
        st->buildMsg.clear();
        ::EnableWindow(st->execBtn, TRUE);
    } else {
        st->built = BuiltCommand{};
        st->buildMsg = err.message;
        SetText(st->preview, err.message);
        ::EnableWindow(st->execBtn, FALSE);
    }
}

bool NeedsDangerArm(PanelState* st) {
    if (st->spec.danger == Danger::Destructive) return true;
    for (const auto& c : st->ctrls) {
        if (c.kind != FlagKind::Dangerous || c.isParam) continue;
        if (::SendMessageW(c.h, BM_GETCHECK, 0, 0) == BST_CHECKED) return true;
    }
    return false;
}

void Disarm(PanelState* st, HWND hwnd) {
    st->armed = false;
    ::KillTimer(hwnd, kTimerDisarm);
    SetText(st->execBtn, Str(IDS_BTN_EXECUTE));
}

void DoExecute(HWND hwnd, PanelState* st) {
    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    RebuildPreview(st);

    if (st->spec.exec == ExecKind::Internal) {
        ExecuteInternalCommand(root, st->spec, st->paths, &st->flags);
        return;
    }
    if (!st->buildMsg.empty() || st->built.argvList.empty()) return;

    if (NeedsDangerArm(st) && !st->armed) {
        st->armed = true;
        SetText(st->execBtn, Str(IDS_BTN_CONFIRM_DANGER));
        ::SetTimer(hwnd, kTimerDisarm, 10000, nullptr);   // 10 秒内未再点则解除
        return;
    }
    if (st->armed) Disarm(st, hwnd);
    RunBuiltCommand(root, st->spec, st->built);
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void RegisterPanelClassOnce() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kParamPanelClass;
    ::RegisterClassExW(&wc);
    done = true;
}

// ---------------------------------------------------------------- 构建控件
void BuildControls(HWND hwnd, PanelState* st) {
    const int fullHeight = -1;
    st->title = MakeChild(hwnd, WC_STATICW, Str(st->spec.titleRes), WS_CHILD | WS_VISIBLE | SS_LEFT,
                          0, 0, Th().fontBold);
    st->stack.emplace_back(st->title, Scale(24));

    if (st->spec.danger == Danger::Destructive) {
        st->hint = MakeChild(hwnd, WC_STATICW, Str(IDS_MSG_DANGER_HINT), WS_CHILD | WS_VISIBLE | SS_LEFT,
                             0, 0, Th().fontSmall);
        st->stack.emplace_back(st->hint, Scale(20));
    }

    // ---- 参数 ----
    if (st->spec.param != ParamKind::None && st->spec.paramKey) {
        HWND label = MakeChild(hwnd, WC_STATICW, Str(ParamLabelRes(st->spec.param)),
                               WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, Th().fontSmall);
        st->stack.emplace_back(label, Scale(18));

        const int id = IDC_PP_PARAM_BASE;
        HWND ctl = nullptr;
        const bool multiline = IsMultiline(st->spec.param);
        if (st->spec.param == ParamKind::ExistingBranch) {
            ctl = MakeChild(hwnd, WC_COMBOBOXW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN, 0, id,
                            Th().fontUi);
            const auto items = ListBranches(st->spec.paramSource);
            for (const auto& it : items) ::SendMessageW(ctl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it.c_str()));
            if (!items.empty()) ::SendMessageW(ctl, CB_SETCURSEL, 0, 0);
        } else {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
            if (multiline) style |= WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
            ctl = MakeChild(hwnd, WC_EDITW, L"", style, WS_EX_CLIENTEDGE, id, Th().fontUi);
            if (st->spec.param == ParamKind::Pattern) {
                // 用当前选择推导默认忽略模式（每行一个）
                std::wstring def;
                for (const auto& p : st->paths) {
                    if (!def.empty()) def += L"\r\n";
                    def += FileNameOf(p);
                }
                SetText(ctl, def);
            }
        }
        st->stack.emplace_back(ctl, multiline ? Scale(64) : (st->spec.param == ParamKind::ExistingBranch ? Scale(200) : Scale(26)));
        st->ctrls.push_back(CtrlInfo{ctl, st->spec.paramKey, FlagKind::Toggle, {}, true, st->spec.param});
    }

    // ---- flags ----
    if (st->spec.flags && st->spec.flagCount > 0) {
        st->flagsLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_LABEL_FLAGS), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, Th().fontBold);
        st->stack.emplace_back(st->flagsLabel, Scale(22));

        std::string lastGroup;
        for (uint8_t i = 0; i < st->spec.flagCount; ++i) {
            const FlagSpec& f = st->spec.flags[i];
            const int id = IDC_PP_FLAG_BASE + i;
            const std::wstring label = Str(f.labelRes);
            HWND ctl = nullptr;
            if (f.kind == FlagKind::Value) {
                HWND lab = MakeChild(hwnd, WC_STATICW, label, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0,
                                     Th().fontSmall);
                st->stack.emplace_back(lab, Scale(18));
                ctl = MakeChild(hwnd, WC_EDITW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                WS_EX_CLIENTEDGE, id, Th().fontUi);
                // 右键菜单/配置里的既有值预填（§4.3 层级 2 的"预填智能默认值"）
                const auto it = st->flags.find(f.key);
                if (it != st->flags.end()) SetText(ctl, it->second);
                st->stack.emplace_back(ctl, Scale(26));
            } else if (f.kind == FlagKind::Radio) {
                const std::string grp = f.radioGroup ? f.radioGroup : "";
                DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
                if (grp != lastGroup) style |= WS_GROUP;   // 每组第一个带 WS_GROUP
                lastGroup = grp;
                ctl = MakeChild(hwnd, WC_BUTTONW, label, style, 0, id, Th().fontUi);
                if (FlagOn(st->flags, f.key, f.defaultOn)) ::SendMessageW(ctl, BM_SETCHECK, BST_CHECKED, 0);
                st->stack.emplace_back(ctl, Scale(24));
            } else {
                ctl = MakeChild(hwnd, WC_BUTTONW, label,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, id, Th().fontUi);
                if (FlagOn(st->flags, f.key, f.defaultOn)) ::SendMessageW(ctl, BM_SETCHECK, BST_CHECKED, 0);
                st->stack.emplace_back(ctl, Scale(24));
            }
            st->ctrls.push_back(CtrlInfo{ctl, f.key, f.kind, f.radioGroup ? f.radioGroup : "", false,
                                         ParamKind::None});
        }
    }

    // ---- 预览 ----
    st->previewLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_LABEL_COMMAND_LINE),
                                 WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, Th().fontBold);
    st->stack.emplace_back(st->previewLabel, Scale(22));
    st->preview = MakeChild(hwnd, WC_EDITW, L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | WS_BORDER,
                            WS_EX_CLIENTEDGE, IDC_PP_PARAM_BASE + 500, Th().fontMono);
    st->stack.emplace_back(st->preview, fullHeight);

    // ---- 按钮 ----
    st->execBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_EXECUTE),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_PP_EXEC,
                            Th().fontUi);
    st->copyBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_COPY), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            0, IDC_PP_COPY, Th().fontUi);
}

void LayoutPanel(HWND hwnd, PanelState* st) {
    if (!st) return;
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(12);
    const int btnH = Scale(32), btnW = Scale(130);
    const int contentBottom = rc.bottom - pad - btnH - Scale(10);
    int y = pad;
    for (auto& item : st->stack) {
        if (item.second < 0) {
            const int h = (std::max)(Scale(60), contentBottom - y);
            ::MoveWindow(item.first, pad, y, rc.right - pad * 2, h, TRUE);
            y = contentBottom;
        } else {
            ::MoveWindow(item.first, pad, y, rc.right - pad * 2, item.second, TRUE);
            y += item.second + Scale(4);
        }
    }
    const int by = rc.bottom - pad - btnH;
    ::MoveWindow(st->execBtn, pad, by, btnW, btnH, TRUE);
    ::MoveWindow(st->copyBtn, pad + btnW + Scale(8), by, btnW, btnH, TRUE);
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<PanelState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* s = new PanelState();
            if (auto* init = reinterpret_cast<const PanelCreateParams*>(cs->lpCreateParams)) {
                if (init->spec) s->spec = *init->spec;
                s->paths = init->paths;
                s->flags = init->flags;
            }
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            return TRUE;
        }
        case WM_CREATE:
            BuildControls(hwnd, st);
            ThemeApply(hwnd);
            RebuildPreview(st);
            return 0;
        case WM_SIZE:
            LayoutPanel(hwnd, st);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_PP_EXEC) {
                DoExecute(hwnd, st);
                return 0;
            }
            if (id == IDC_PP_COPY) {
                CopyTextToClipboard(hwnd, GetText(st->preview));
                return 0;
            }
            // 任何输入变化 → 重算预览
            if ((code == EN_CHANGE || code == CBN_SELCHANGE || code == CBN_EDITCHANGE || code == BN_CLICKED) &&
                id >= IDC_PP_PARAM_BASE) {
                if (st->armed) Disarm(st, hwnd);   // 改动参数后重新确认
                RebuildPreview(st);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerDisarm && st) Disarm(st, hwnd);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
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

HWND CreateParamPanelEx(HWND parent, const CommandSpec& spec, const std::vector<std::wstring>& paths,
                        const std::map<std::string, std::wstring>& flags) {
    RegisterPanelClassOnce();
    PanelCreateParams init;
    init.spec = &spec;
    init.paths = paths;
    init.flags = flags;
    return ::CreateWindowExW(0, kParamPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10,
                             10, parent, nullptr, ::GetModuleHandleW(nullptr), &init);
}

HWND CreateParamPanel(HWND parent, const CommandSpec& spec, const std::vector<std::wstring>& paths) {
    return CreateParamPanelEx(parent, spec, paths, {});
}

void ParamPanelRelayout(HWND panel, int x, int y, int w, int h) {
    if (panel) ::MoveWindow(panel, x, y, w, h, TRUE);
}

void ParamPanelSetPaths(HWND panel, const std::vector<std::wstring>& paths) {
    if (!panel) return;
    if (auto* st = reinterpret_cast<PanelState*>(::GetWindowLongPtrW(panel, GWLP_USERDATA))) {
        st->paths = paths;
        RebuildPreview(st);
    }
}

}  // namespace grt::gui
