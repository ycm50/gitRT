// AI 助手窗口：自然语言 →（AI 规划）→ 白名单校验 → 命令预览 → 执行 → 显示运行结果
// 《产品设计》§4.3 层级 4 / 《技术实现设计》§16
//
// 安全边界（务必保持）：
//   1. AI 只产出"命令 key + 参数 + 选项"的结构化计划；
//   2. 计划必须经 PlanToCommand() 走与菜单/参数面板完全相同的校验与 argv 构造；
//   3. 危险命令/危险选项需要用户在模态框中显式确认；
//   4. 绝不把 AI 输出当命令行去执行。
#include "gui.h"

#include "ai_client.h"

#include <thread>

using namespace grt;

namespace grt::gui {

namespace {

enum : int {
    IDC_AI_PROMPT = 900,
    IDC_AI_PLAN,
    IDC_AI_RESULT,
    IDC_AI_DETAILS,
    IDC_AI_GEN,
    IDC_AI_EXEC,
    IDC_AI_COPY,
    IDC_AI_CLOSE,
    IDC_AI_SETTINGS = 908,   // 与 shellprobe/demo 脚本里的 ID 约定保持一致
    IDC_AI_CFG_LABEL = 909,  // 配置行（可被自动化脚本读回，用于验证 Key 状态）
};

const wchar_t* kAiWindowClass = L"GitRT.AiWindow";

struct AiDone {
    AiPlan plan;
};

struct AiState {
    AiConfig cfg;
    HWND cfgLabel = nullptr, promptLabel = nullptr, hintLabel = nullptr, promptEdit = nullptr;
    HWND planLabel = nullptr, planEdit = nullptr, resultLabel = nullptr, resultEdit = nullptr;
    HWND detailsLabel = nullptr, detailsEdit = nullptr;
    HWND genBtn = nullptr, execBtn = nullptr, copyBtn = nullptr, closeBtn = nullptr;
    HWND setBtn = nullptr;   // 「AI 设置」
    bool         busy = false;
    std::thread  worker;
    AiPlan       plan;
    bool         hasPlan = false;
    const CommandSpec* spec = nullptr;
    BuiltCommand built;
};

// 危险判定：命令本身危险，或方案里勾了【危险】选项
bool PlanIsDangerous(const AiState* st) {
    if (!st->spec) return false;
    if (st->spec->danger == Danger::Destructive) return true;
    for (const auto& kv : st->plan.flags) {
        if (kv.second != "1" && kv.second != "true") continue;
        if (const FlagSpec* f = FindFlag(*st->spec, kv.first))
            if (f->danger) return true;
    }
    for (const auto& kv : st->plan.flags) {
        if (kv.second != "1") continue;
        if (st->spec->flags) {
            for (uint8_t i = 0; i < st->spec->flagCount; ++i) {
                const FlagSpec& f = st->spec->flags[i];
                if (f.key == kv.first && f.kind == FlagKind::Dangerous) return true;
            }
        }
    }
    return false;
}

std::wstring DescribePlan(const AiState* st, const PlanToCommandResult& r) {
    std::wstring s;
    const CommandSpec* spec = r.spec ? r.spec : FindCommandByKey(st->plan.commandKey);
    if (!st->plan.cmdline.empty()) {
        // "方案直接输出命令"：模型给的是一条只读 git 命令
        std::wstring t = L"\u6a21\u578b\u76f4\u63a5\u7ed9\u51fa\u7684\u53ea\u8bfb\u547d\u4ee4\uff1a";
        t += W(st->plan.cmdline);
        t += L"\r\n";
        if (!st->plan.explanation.empty()) t += L"\u8bf4\u660e: " + st->plan.explanation + L"\r\n";
        t += L"\r\n\u5373\u5c06\u6267\u884c\u7684\u547d\u4ee4\uff1a\r\n" + r.built.display;
        for (const auto& w : r.warnings) t += L"\r\n\u63d0\u793a: " + w;
        return t;
    }
    s += L"\u547d\u4ee4: " + W(st->plan.commandKey);
    if (spec) s += L"  (" + Str(spec->titleRes) + L")";
    s += L"\r\n";

    s += L"\u53c2\u6570: ";
    if (st->plan.params.empty()) s += L"(\u65e0)";
    for (const auto& kv : st->plan.params) s += W(kv.first) + L"=" + W(kv.second) + L"  ";
    s += L"\r\n";

    s += L"\u9009\u9879: ";
    {
        bool any = false;
        for (const auto& kv : st->plan.flags) {
            if (kv.second != "1" && kv.second != "0") continue;
            if (kv.second != "1") continue;   // 只列出开启的
            s += W(kv.first) + L"  ";
            any = true;
        }
        if (!any) s += L"(\u65e0)";
    }
    s += L"\r\n";

    if (!st->plan.explanation.empty()) s += L"\u8bf4\u660e: " + st->plan.explanation + L"\r\n";
    for (const auto& w : r.warnings) s += L"\u63d0\u793a: " + w + L"\r\n";

    s += L"\r\n\u5373\u5c06\u6267\u884c\u7684\u547d\u4ee4\uff1a\r\n";
    s += r.built.display;
    if (!r.built.notes.empty()) {
        for (const auto& n : r.built.notes) s += L"\r\n-- " + n;
    }
    s += L"\r\n\r\n(\u8017\u65f6 " + std::to_wstring(st->plan.elapsedMs) + L" ms, HTTP " +
         std::to_wstring(st->plan.httpStatus) + L")";
    return s;
}

void SetPlanText(AiState* st, const std::wstring& text) { SetTextMl(st->planEdit, text); }

void AppendResult(AiState* st, const std::wstring& text) {
    const std::wstring cur = GetText(st->resultEdit);
    SetTextMl(st->resultEdit, cur.empty() ? text : (cur + L"\r\n" + text));
    // 滚到底部
    ::SendMessageW(st->resultEdit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    ::SendMessageW(st->resultEdit, EM_SCROLLCARET, 0, 0);
}

void UpdateConfigLabel(AiState* st) {
    const std::wstring key = ResolveApiKey(st->cfg);
    const bool loopback = IsLoopbackEndpoint(st->cfg.endpoint);
    std::wstring keyText;
    if (!key.empty()) {
        keyText = L"\u5df2\u8bbe\u7f6e \u2713";
    } else if (loopback) {
        keyText = L"\u672a\u8bbe\u7f6e\uff08\u672c\u673a\u670d\u52a1\u514d Key\uff09";   // Ollama 等
    } else {
        keyText = L"\u672a\u8bbe\u7f6e \u2717";
    }
    std::wstring s = Str(IDS_AI_LABEL_CONFIG) + L": " + st->cfg.endpoint + L"  \u00b7  " + st->cfg.model +
                     L"  \u00b7  Key(" + W(st->cfg.apiKeyEnv) + L"): " + keyText;
    // 明文过网（http + 非本机）：这里常驻提醒；设置窗口保存/测试时另有一次确认框
    if (IsInsecureRemoteEndpoint(st->cfg.endpoint))
        s += L"  \u00b7  \u26a0 \u660e\u6587 HTTP \u4e14\u975e\u672c\u673a\uff0cKey \u4f1a\u660e\u6587\u8fc7\u7f51";
    SetText(st->cfgLabel, s);
}

void StartGenerate(HWND hwnd, AiState* st) {
    const std::wstring raw = GetText(st->promptEdit);
    const std::wstring text = Trim(raw);
    GRT_LOGI("ai", "生成方案 promptLen=" << text.size());
    if (text.empty()) {
        SetPlanText(st, Str(IDS_MSG_AI_EMPTY));
        return;
    }
    // 本机服务（Ollama / LM Studio…）不鉴权时不拦；远端才需要 Key
    if (ResolveApiKey(st->cfg).empty() && !IsLoopbackEndpoint(st->cfg.endpoint)) {
        SetPlanText(st, Str(IDS_MSG_AI_NO_KEY) + L"\r\n\r\n" +
                            Str(IDS_AI_LABEL_CONFIG) + L":\r\n  " + st->cfg.configPath + L"\r\n" +
                            L"  aiEndpoint = " + st->cfg.endpoint + L"\r\n  aiModel = " + st->cfg.model +
                            L"\r\n  aiApiKeyEnv = " + W(st->cfg.apiKeyEnv));
        UpdateConfigLabel(st);
        return;
    }
    SetPlanText(st, Str(IDS_MSG_AI_GENERATING));
    st->busy = true;
    ::EnableWindow(st->genBtn, FALSE);
    ::EnableWindow(st->execBtn, FALSE);

    // 工作线程只持有自己的上下文，不触碰窗口状态（避免与 WM_NCDESTROY 竞争）
    struct Ctx {
        AiConfig     cfg;
        std::string  sys, user;
        HWND         hwnd = nullptr;
    };
    auto* ctx = new Ctx();
    ctx->cfg = st->cfg;
    ctx->hwnd = hwnd;
    ctx->sys = EffectiveSystemPrompt(ctx->cfg, [](uint16_t id) { return WideToUtf8(Str(id)); });
    ctx->user = BuildPlannerUserPrompt(text, App().repoRoot, W(App().status.head), {});
    GRT_LOGI("ai", "生成方案 endpoint=" << U8(ctx->cfg.endpoint) << " model=" << U8(ctx->cfg.model)
                                        << " prompt=" << U8(text) << " sysBytes=" << ctx->sys.size());

    st->worker = std::thread([ctx]() {
        auto* done = new AiDone();
        done->plan = GeneratePlan(ctx->cfg, ctx->sys, ctx->user);
        if (!::PostMessageW(ctx->hwnd, WM_GRT_AI_DONE, 0, reinterpret_cast<LPARAM>(done))) delete done;
        delete ctx;
    });
}

void ExecutePlan(HWND hwnd, AiState* st) {
    if (!st->hasPlan || !st->spec) return;
    if (PlanIsDangerous(st)) {
        const std::wstring msg = std::wstring(L"\u5371\u9669\u64cd\u4f5c\u786e\u8ba4\r\n\r\n") +
                                 W(st->spec->key) + L"\r\n\r\n" + st->built.display + L"\r\n\r\n" +
                                 Str(IDS_MSG_DANGER_HINT);
        if (::MessageBoxW(hwnd, msg.c_str(), Str(IDS_TITLE_AI).c_str(),
                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    AppendResult(st, L"\u2500\u2500 " + Str(IDS_MSG_AI_EXECUTING) + L" \u2500\u2500\r\n" +
                         st->built.display);
    RunBuiltCommand(hwnd, *st->spec, st->built);
}

void LayoutAi(HWND hwnd, AiState* st) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int pad = Scale(12);
    const int w = (std::max)(Scale(300), static_cast<int>(rc.right) - pad * 2);
    const int btnH = Scale(32), labelH = Scale(18);
    int y = pad;

    ::MoveWindow(st->cfgLabel, pad, y, w, Scale(18), TRUE);
    y += Scale(20) + Scale(4);
    ::MoveWindow(st->promptLabel, pad, y, w, labelH, TRUE);
    y += labelH + Scale(2);
    ::MoveWindow(st->hintLabel, pad, y, w, Scale(16), TRUE);
    y += Scale(16) + Scale(4);
    const int promptH = Scale(64);
    ::MoveWindow(st->promptEdit, pad, y, w, promptH, TRUE);
    y += promptH + Scale(8);
    ::MoveWindow(st->genBtn, pad, y, Scale(120), btnH, TRUE);
    ::MoveWindow(st->execBtn, pad + Scale(128), y, Scale(120), btnH, TRUE);
    ::MoveWindow(st->copyBtn, pad + Scale(256), y, Scale(120), btnH, TRUE);
    ::MoveWindow(st->closeBtn, pad + Scale(384), y, Scale(110), btnH, TRUE);
    if (st->setBtn) ::MoveWindow(st->setBtn, pad + Scale(502), y, Scale(120), btnH, TRUE);
    y += btnH + Scale(10);

    ::MoveWindow(st->planLabel, pad, y, w, labelH, TRUE);
    y += labelH + Scale(2);
    const int planH = Scale(140);
    ::MoveWindow(st->planEdit, pad, y, w, planH, TRUE);
    y += planH + Scale(8);

    ::MoveWindow(st->resultLabel, pad, y, w, labelH, TRUE);
    y += labelH + Scale(2);
    // 底部保留"详情"区
    const int detailsH = Scale(70);
    const int bottomLimit = static_cast<int>(rc.bottom) - pad - labelH - Scale(2) - detailsH - Scale(8);
    const int resultH = (std::max)(Scale(80), bottomLimit - y);
    ::MoveWindow(st->resultEdit, pad, y, w, resultH, TRUE);
    y += resultH + Scale(8);
    ::MoveWindow(st->detailsLabel, pad, y, w, labelH, TRUE);
    y += labelH + Scale(2);
    ::MoveWindow(st->detailsEdit, pad, y, w, detailsH, TRUE);
}

LRESULT CALLBACK AiProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<AiState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new AiState()));
            return TRUE;
        case WM_CREATE: {
            st->cfg = LoadAiConfig();
            st->cfgLabel = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                     IDC_AI_CFG_LABEL, Th().fontSmall);
            st->promptLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_AI_LABEL_PROMPT),
                                        WS_CHILD | WS_VISIBLE, 0, 0, Th().fontBold);
            st->hintLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_AI_PROMPT_HINT),
                                      WS_CHILD | WS_VISIBLE, 0, 0, Th().fontSmall);
            st->promptEdit = MakeChild(hwnd, WC_EDITW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER |
                                           ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                       WS_EX_CLIENTEDGE, IDC_AI_PROMPT, Th().fontUi);
            st->genBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_AI_GENERATE),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_AI_GEN,
                                   Th().fontUi);
            st->execBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_AI_EXECUTE),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED, 0, IDC_AI_EXEC,
                                    Th().fontUi);
            st->copyBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_COPY), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    0, IDC_AI_COPY, Th().fontUi);
            st->closeBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     0, IDC_AI_CLOSE, Th().fontUi);
            // 直达设置：用户看到"未设置 Key"时，下一步就该点这里
            st->setBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_TITLE_AI_SETTINGS),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_AI_SETTINGS, Th().fontUi);

            st->planLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_AI_LABEL_PLAN), WS_CHILD | WS_VISIBLE, 0,
                                      0, Th().fontBold);
            st->planEdit = MakeChild(hwnd, WC_EDITW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                         ES_READONLY | ES_AUTOVSCROLL | WS_BORDER,
                                     WS_EX_CLIENTEDGE, IDC_AI_PLAN, Th().fontMono);
            st->resultLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_AI_LABEL_RESULT), WS_CHILD | WS_VISIBLE,
                                        0, 0, Th().fontBold);
            st->resultEdit = MakeChild(hwnd, WC_EDITW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                           ES_READONLY | ES_AUTOVSCROLL | WS_BORDER,
                                       WS_EX_CLIENTEDGE, IDC_AI_RESULT, Th().fontMono);
            st->detailsLabel = MakeChild(hwnd, WC_STATICW, Str(IDS_AI_LABEL_DETAILS),
                                         WS_CHILD | WS_VISIBLE, 0, 0, Th().fontSmall);
            st->detailsEdit = MakeChild(hwnd, WC_EDITW, L"",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                            ES_AUTOVSCROLL | WS_BORDER,
                                        WS_EX_CLIENTEDGE, IDC_AI_DETAILS, Th().fontMono);
            ThemeApply(hwnd);
            UpdateConfigLabel(st);
            SetFocus(st->promptEdit);
            return 0;
        }
        case WM_SIZE:
            LayoutAi(hwnd, st);
            return 0;
        case WM_GRT_AI_CONFIG_RELOAD: {
            // AI 设置窗口保存后：重新读配置，配置行立刻从"未设置 ✗"变成"已设置 ✓"
            if (!st) return 0;
            st->cfg = LoadAiConfig();
            UpdateConfigLabel(st);
            return 0;
        }
        case WM_GRT_AI_DONE: {
            std::unique_ptr<AiDone> done(reinterpret_cast<AiDone*>(lp));
            if (!st || !done) return 0;
            if (st->worker.joinable()) st->worker.join();
            st->busy = false;
            ::EnableWindow(st->genBtn, TRUE);
            st->plan = done->plan;
            st->hasPlan = false;
            st->spec = nullptr;

            if (!st->plan.ok) {
                SetPlanText(st, Str(IDS_MSG_AI_REJECTED) + L"\r\n\r\n" + st->plan.error +
                                    L"\r\n\r\n(HTTP " + std::to_wstring(st->plan.httpStatus) + L", " +
                                    std::to_wstring(st->plan.elapsedMs) + L" ms)");
                SetTextMl(st->detailsEdit, st->plan.rawReply);
                return 0;
            }
            if (st->plan.noCommand) {
                SetPlanText(st, Str(IDS_MSG_AI_NONE) + L"\r\n\r\n" + st->plan.explanation);
                SetTextMl(st->detailsEdit, st->plan.rawReply);
                return 0;
            }
            const PlanToCommandResult r =
                PlanToCommand(st->plan, App().repoRoot, {}, App().gitExe);
            if (!r.ok) {
                SetPlanText(st, Str(IDS_MSG_AI_REJECTED) + L"\r\n\r\n" + r.error + L"\r\n\r\n" +
                                    L"\u6a21\u578b\u8fd4\u56de: " + W(st->plan.commandKey));
                SetTextMl(st->detailsEdit, st->plan.rawReply);
                return 0;
            }
            st->hasPlan = true;
            st->spec = r.spec;
            st->built = r.built;
            SetPlanText(st, Str(IDS_MSG_AI_READY) + L"\r\n\r\n" + DescribePlan(st, r));
            SetTextMl(st->detailsEdit, st->plan.rawReply);
            ::EnableWindow(st->execBtn, TRUE);
            GRT_LOGI("ai", "方案已生成 command=" << st->plan.commandKey
                                                 << " argv=" << U8(r.built.display));
            return 0;
        }
        case WM_GRT_TASK_FINISHED: {
            std::unique_ptr<TaskSummary> ts(reinterpret_cast<TaskSummary*>(lp));
            if (!st || !ts) return 0;
            std::wstring head = ts->ok ? Str(IDS_MSG_DONE)
                                       : (ts->cancelled ? Str(IDS_MSG_CANCELLED) : Str(IDS_MSG_FAILED));
            std::wstring s = L"\u2500\u2500 " + head + L"  (exit=" + std::to_wstring(ts->exitCode) +
                             L", " + std::to_wstring(ts->ms) + L" ms) \u2500\u2500";
            if (!ts->output.empty()) s += L"\r\n" + ts->output;
            AppendResult(st, s);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_AI_GEN:
                    if (!st->busy) StartGenerate(hwnd, st);
                    return 0;
                case IDC_AI_EXEC:
                    ExecutePlan(hwnd, st);
                    return 0;
                case IDC_AI_COPY:
                    CopyTextToClipboard(hwnd, st->hasPlan ? st->built.display : GetText(st->planEdit));
                    return 0;
                case IDC_AI_CLOSE:
                case IDCANCEL:
                    if (st->busy) return 0;   // 请求中不允许关闭（避免线程悬空）
                    ::DestroyWindow(hwnd);
                    return 0;
                case IDC_AI_SETTINGS:
                    // 打开 AI 设置；保存后本窗口会收到 WM_GRT_AI_CONFIG_RELOAD 并刷新配置行
                    ShowSettingsWindow(hwnd, hwnd);
                    return 0;
                default:
                    break;
            }
            break;
        }
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
                ::FillRect(reinterpret_cast<HDC>(wp), &rc, Th().bgBrush);
                return 1;
            }
            break;
        case WM_CLOSE:
            if (st && st->busy) return 0;
            ::DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (st) {
                if (st->worker.joinable()) st->worker.join();
                delete st;
            }
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureAiClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = AiProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.lpszClassName = kAiWindowClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void ShowAiWindow(HWND owner) {
    EnsureAiClass();
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kAiWindowClass, Str(IDS_TITLE_AI).c_str(),
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, Scale(940), Scale(780),
                               owner, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!h) return;
    CenterOnOwner(h, owner);
    ::ShowWindow(h, SW_SHOW);
    ::UpdateWindow(h);
}

}  // namespace grt::gui
