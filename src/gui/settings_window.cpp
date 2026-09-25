// AI 设置窗口（《产品设计》§4.3 层级 4 / Q11；产品决策 2026-09-24）
//
// 「首选项…」原来只弹一句"未实现"。现在它是一个真正能用的设置界面：
//   · 接口地址 / 模型 / API Key / 超时  → 保存到 **exe 同目录的 GitRT.ai.json（明文）**
//   · 「测试连接」用当前输入直接打一次 API（不必先保存），把 HTTP 状态、耗时、错误显示出来
//   · 明确显示 Key 的**来源**（文件 / 环境变量 / 未设置）与目录可写性
//
// 为什么是明文（用户明确选择）：不用设环境变量、不用重启资源管理器、随时可直接查看与修改。
// 代价：任何能读该目录的进程都能读到 Key —— 界面里如实告知（IDS_AI_SET_HINT_KEY）。
#include "gui.h"
#include "config.h"

#include "ai_client.h"
#include "json_util.h"

#include <thread>
#include <vector>

namespace grt::gui {

const wchar_t* kSettingsWindowClass = L"GitRT.SettingsWindow";

enum : int {
    IDC_SET_ENDPOINT = 1000,
    IDC_SET_MODEL,
    IDC_SET_KEY,
    IDC_SET_TIMEOUT,
    IDC_SET_STORE,
    IDC_SET_HINT,
    IDC_SET_STATUS,
    IDC_SET_SAVE,
    IDC_SET_TEST,
    IDC_SET_CLOSE,
    IDC_SET_LBL_ENDPOINT,
    IDC_SET_LBL_MODEL,
    IDC_SET_LBL_KEY,
    IDC_SET_LBL_TIMEOUT,
    IDC_SET_LBL_STORE,
    // ★ 注意别撞号：1010 已经给“接口地址”标签用了（踩过：写 1010 拿到的是标签）
    IDC_SET_SYSPROMPT = 1030,
    IDC_SET_LBL_SYSPROMPT = 1031,
    IDC_SET_RESET_PROMPT = 1032,
    IDC_SET_SYSPROMPT_HINT = 1033,
    IDC_SET_AUTOFETCH = 1040,   // 自动抓取间隔（分钟，0=关）
    IDC_SET_LBL_AUTOFETCH = 1041,
    IDC_SET_AUTOFETCH_HINT_ID = 1042,
    IDC_SET_GETMODELS = 1020,   // 「获取模型列表」
};

constexpr UINT WM_GRT_AI_TEST_DONE = WM_APP + 64;
constexpr UINT WM_GRT_AI_MODELS_DONE = WM_APP + 67;
constexpr UINT WM_GRT_SET_NOTIFY_TARGET = WM_APP + 66;

namespace {

struct SetState {
    AiConfig cfg;
    HWND endpoint = nullptr, model = nullptr, key = nullptr, timeout = nullptr;
    HWND storeLabel = nullptr, hint = nullptr, status = nullptr;
    HWND saveBtn = nullptr, testBtn = nullptr, closeBtn = nullptr;
    HWND getModelsBtn = nullptr;
    HWND sysPrompt = nullptr, sysPromptHint = nullptr, resetPromptBtn = nullptr;
    HWND autoFetch = nullptr, autoFetchHint = nullptr;   // 自动抓取间隔（分钟）
    HWND notify = nullptr;     // 配置变化通知目标（AI 窗口）
    bool testing = false;
    bool fetchingModels = false;
    std::thread worker;
};

void ReadFields(SetState* st);   // 前置声明（StartModelFetch 先用后定义）

void FillFields(SetState* st) {
    SetText(st->endpoint, st->cfg.endpoint);
    SetText(st->model, st->cfg.model);   // 下拉框：既可选，也允许直接手输
    SetText(st->key, st->cfg.apiKey);   // 明文显示：用户选的就是"看得见"
    SetText(st->timeout, std::to_wstring(st->cfg.timeoutMs));
    SetText(st->storeLabel, st->cfg.keyFilePath);
    SetTextMl(st->sysPrompt, st->cfg.systemPrompt);   // 多行：系统提示词
    SetText(st->autoFetch, std::to_wstring(ConfigStore::Instance().GetInt("autoFetchMinutes", 0)));
    SetText(st->sysPromptHint, st->cfg.systemPrompt.empty() ? Str(IDS_AI_SET_PROMPT_DEFAULT_HINT)
                                                              : std::wstring());
}

// 把一个模型列表灌进下拉框：保留用户当前填的值（不在列表里也不丢）
void FillModelCombo(SetState* st, const std::vector<std::wstring>& models) {
    const std::wstring current = Trim(GetText(st->model));
    ::SendMessageW(st->model, CB_RESETCONTENT, 0, 0);
    for (const auto& m : models)
        ::SendMessageW(st->model, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m.c_str()));
    if (!current.empty()) {
        const LRESULT idx = ::SendMessageW(st->model, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                           reinterpret_cast<LPARAM>(current.c_str()));
        if (idx >= 0)
            ::SendMessageW(st->model, CB_SETCURSEL, static_cast<WPARAM>(idx), 0);
        else
            SetText(st->model, current);   // 当前值不在列表里 → 原样保留
    } else if (!models.empty()) {
        ::SendMessageW(st->model, CB_SETCURSEL, 0, 0);
        SetText(st->model, models.front());
    }
}

// 拉模型列表（后台线程；结果经 WM_GRT_AI_MODELS_DONE 回 UI 线程）
void StartModelFetch(HWND hwnd, SetState* st) {
    if (st->fetchingModels) return;
    ReadFields(st);
    st->fetchingModels = true;
    ::EnableWindow(st->getModelsBtn, FALSE);
    SetText(st->status, Str(IDS_MSG_AI_MODELS_LOADING));
    const AiConfig cfg = st->cfg;
    if (st->worker.joinable()) st->worker.join();
    st->worker = std::thread([hwnd, cfg] {
        auto* res = new ModelListResult(FetchModelList(cfg));
        ::PostMessageW(hwnd, WM_GRT_AI_MODELS_DONE, 0, reinterpret_cast<LPARAM>(res));
    });
}

std::wstring KeySourceText(const AiConfig& cfg) {
    if (!cfg.apiKey.empty()) return Str(IDS_MSG_AI_KEY_FROM_FILE);
    wchar_t buf[1024]{};
    const DWORD n = ::GetEnvironmentVariableW(W(cfg.apiKeyEnv).c_str(), buf, 1024);
    if (n > 0 && n < 1024 && Trim(std::wstring(buf, n)).size() > 0)
        return Str(IDS_MSG_AI_KEY_FROM_ENV) + L"  " + W(cfg.apiKeyEnv);
    return Str(IDS_MSG_AI_KEY_NONE2);
}

void RefreshStatus(SetState* st, const std::wstring& extra = {}) {
    std::wstring s = KeySourceText(st->cfg);
    if (!st->cfg.keyFileUsable) s += L"   " + Str(IDS_MSG_AI_STORE_READONLY);
    if (!extra.empty()) s = extra + L"   " + s;
    SetText(st->status, s);
}

// 把界面上的输入读回 cfg（不落盘）
void ReadFields(SetState* st) {
    st->cfg.endpoint = Trim(GetText(st->endpoint));
    st->cfg.model = Trim(GetText(st->model));
    st->cfg.apiKey = Trim(GetText(st->key));
    const std::wstring t = Trim(GetText(st->timeout));
    const int ms = t.empty() ? 60000 : _wtoi(t.c_str());
    st->cfg.timeoutMs = static_cast<uint32_t>((ms < 5000 || ms > 600000) ? 60000 : ms);
    st->cfg.systemPrompt = Trim(GetText(st->sysPrompt));   // 留空 = 用内置默认提示词
    // 自动抓取间隔：0 = 关（写入普通配置键，主窗口的定时器读它）
    {
        const std::wstring v = Trim(GetText(st->autoFetch));
        int m = v.empty() ? 0 : _wtoi(v.c_str());
        if (m < 0) m = 0;
        if (m > 1440) m = 1440;   // 最多一天一次
        ConfigStore::Instance().SetInt("autoFetchMinutes", m);
        ConfigStore::Instance().Save();
    }
}

// 测试连接：最小请求（max_tokens=1），只看 HTTP 是否通、Key 是否被接受
void TestConnection(SetState* st, HWND hwnd) {
    if (st->testing) return;
    ReadFields(st);
    const std::wstring key = ResolveApiKey(st->cfg);
    if (key.empty()) {
        SetText(st->status, Str(IDS_MSG_AI_KEY_NONE2));
        return;
    }
    st->testing = true;
    ::EnableWindow(st->testBtn, FALSE);
    SetText(st->status, Str(IDS_MSG_AI_TESTING));
    const AiConfig cfg = st->cfg;
    const std::wstring url = cfg.endpoint;
    const uint32_t timeout = (std::min)(cfg.timeoutMs, 30000u);
    if (st->worker.joinable()) st->worker.join();
    st->worker = std::thread([hwnd, url, key, cfg, timeout] {
        const std::string body =
            "{\"model\":\"" + JsonEscape(WideToUtf8(cfg.model)) +
            "\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],\"max_tokens\":1}";
        const std::vector<std::pair<std::wstring, std::wstring>> headers = {
            {L"Content-Type", L"application/json"},
            {L"Authorization", L"Bearer " + key},
        };
        const auto a = std::make_unique<HttpResponse>(HttpPostJson(url, headers, body, timeout));
        const auto started = a->status;   // 结果整体回传（status/body/error）
        auto* payload = new std::pair<int, std::string>{started, a->error.empty() ? a->body : a->error};
        ::PostMessageW(hwnd, WM_GRT_AI_TEST_DONE, 0, reinterpret_cast<LPARAM>(payload));
    });
}

void SaveFromUi(SetState* st) {
    ReadFields(st);
    st->cfg.keyFilePath = AiKeyFilePath();
    const bool ok = SaveAiConfig(st->cfg);
    st->cfg.keyFileUsable = ok || st->cfg.keyFileUsable;
    st->cfg.keyFromFile = !st->cfg.apiKey.empty();
    RefreshStatus(st, ok ? Str(IDS_MSG_AI_SAVED) : Str(IDS_MSG_AI_SAVE_FAILED));
    // 让 AI 窗口立刻看到新配置（否则它还按旧配置显示"未设置 Key"）
    if (st->notify) ::PostMessageW(st->notify, WM_GRT_AI_CONFIG_RELOAD, 0, 0);
}

LRESULT CALLBACK SetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SetState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* s = new SetState();
            if (auto* init = reinterpret_cast<AiConfig*>(cs->lpCreateParams)) s->cfg = *init;
            s->cfg.keyFilePath = AiKeyFilePath();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            return TRUE;
        }
        case WM_GRT_SET_NOTIFY_TARGET:   // 由 ShowSettingsWindow 在创建后设置
            if (st) st->notify = reinterpret_cast<HWND>(lp);
            return 0;
        case WM_CREATE: {
            auto label = [&](UINT res, int id, int row) {
                return MakeChild(hwnd, WC_STATICW, Str(res), WS_CHILD | WS_VISIBLE | SS_LEFT, row, id,
                                 Th().fontSmall);
            };
            label(IDS_AI_SET_LABEL_ENDPOINT, IDC_SET_LBL_ENDPOINT, 0);
            st->endpoint = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     WS_EX_CLIENTEDGE, IDC_SET_ENDPOINT, Th().fontUi);
            label(IDS_AI_SET_LABEL_MODEL, IDC_SET_LBL_MODEL, 0);
            // 模型：CBS_DROPDOWN —— 列表来自接口（「获取模型列表」），也允许直接手输。
            // ★★ 组合框的下拉列表高度**只能在创建时指定**：之后 MoveWindow 会被系统
            //    规范化回"编辑框高度"，于是"点开是空的"（实测踩过）。这里一次给足
            //    editH + 8 行，WM_SIZE 里再用同样的总高移动它。
            {
                const int totalH = Scale(26) + Scale(18) * 8;   // 编辑框 + 8 行列表
                st->model = ::CreateWindowExW(
                    WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0,
                    Scale(200), totalH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SET_MODEL)),
                    ::GetModuleHandleW(nullptr), nullptr);
                if (st->model && Th().fontUi)
                    ::SendMessageW(st->model, WM_SETFONT, reinterpret_cast<WPARAM>(Th().fontUi), TRUE);
            }
            st->getModelsBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_AI_MODELS),
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_SET_GETMODELS,
                                         Th().fontUi);
            label(IDS_AI_SET_LABEL_KEY, IDC_SET_LBL_KEY, 0);
            st->key = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                WS_EX_CLIENTEDGE, IDC_SET_KEY, Th().fontUi);
            label(IDS_AI_SET_LABEL_TIMEOUT, IDC_SET_LBL_TIMEOUT, 0);
            st->timeout = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                          ES_NUMBER | ES_AUTOHSCROLL,
                                    WS_EX_CLIENTEDGE, IDC_SET_TIMEOUT, Th().fontUi);
            // ---- 系统提示词（"首选项应该能够设置系统提示词"）----
            label(IDS_AI_SET_LABEL_SYSPROMPT, IDC_SET_LBL_SYSPROMPT, 0);
            st->sysPrompt = MakeChild(hwnd, WC_EDITW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                          ES_WANTRETURN | WS_BORDER,
                                      WS_EX_CLIENTEDGE, IDC_SET_SYSPROMPT, Th().fontMono);
            st->sysPromptHint = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                          IDC_SET_SYSPROMPT_HINT, Th().fontSmall);
            st->resetPromptBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_AI_RESET_PROMPT),
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_SET_RESET_PROMPT,
                                           Th().fontUi);
            // 自动抓取（持续跟踪远端）
            label(IDS_AI_SET_LABEL_AUTOFETCH, IDC_SET_LBL_AUTOFETCH, 0);
            st->autoFetch = MakeChild(hwnd, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                      WS_EX_CLIENTEDGE, IDC_SET_AUTOFETCH, Th().fontUi);
            st->autoFetchHint = MakeChild(hwnd, WC_STATICW, Str(IDS_AI_SET_AUTOFETCH_HINT),
                                          WS_CHILD | WS_VISIBLE | SS_LEFT, 0, IDC_SET_AUTOFETCH_HINT_ID,
                                          Th().fontSmall);
            label(IDS_AI_SET_LABEL_STORE, IDC_SET_LBL_STORE, 0);
            st->storeLabel = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                       IDC_SET_STORE, Th().fontSmall);
            st->hint = MakeChild(hwnd, WC_STATICW, Str(IDS_AI_SET_HINT_KEY),
                                 WS_CHILD | WS_VISIBLE | SS_LEFT, 0, IDC_SET_HINT, Th().fontSmall);
            st->status = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                   IDC_SET_STATUS, Th().fontBold);
            st->saveBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_AI_SAVE),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                                    IDC_SET_SAVE, Th().fontUi);
            st->testBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_AI_TEST),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_SET_TEST, Th().fontUi);
            st->closeBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_SET_CLOSE, Th().fontUi);
            FillFields(st);
            RefreshStatus(st);
            ThemeApply(hwnd);
            return 0;
        }
        case WM_SIZE: {
            if (!st) break;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const int pad = Scale(14), lh = Scale(18), eh = Scale(26), gap = Scale(6);
            const int w = rc.right - pad * 2;
            int y = pad;
            auto row = [&](HWND lbl, HWND edit) {
                ::MoveWindow(lbl, pad, y, w, lh, TRUE);
                y += lh + Scale(2);
                ::MoveWindow(edit, pad, y, w, eh, TRUE);
                y += eh + gap + Scale(4);
            };
            row(::GetDlgItem(hwnd, IDC_SET_LBL_ENDPOINT), st->endpoint);
            // 模型行：下拉框 + 「获取模型列表」按钮同排
            {
                ::MoveWindow(::GetDlgItem(hwnd, IDC_SET_LBL_MODEL), pad, y, w, lh, TRUE);
                y += lh + Scale(2);
                const int btnW2 = Scale(140);
                // 同上：移动时也要带上列表区高度，否则下拉变空
                int itemH = static_cast<int>(::SendMessageW(st->model, CB_GETITEMHEIGHT, 0, 0));
                if (itemH <= 0) itemH = Scale(18);
                ::SetWindowPos(st->model, nullptr, pad, y, w - btnW2 - Scale(8), eh + itemH * 8,
                               SWP_NOZORDER | SWP_NOACTIVATE);
                ::MoveWindow(st->getModelsBtn, pad + w - btnW2, y, btnW2, eh, TRUE);
                y += eh + gap + Scale(4);
            }
            row(::GetDlgItem(hwnd, IDC_SET_LBL_KEY), st->key);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_SET_LBL_TIMEOUT), pad, y, Scale(200), lh, TRUE);
            ::MoveWindow(st->timeout, pad, y + lh + Scale(2), Scale(200), eh, TRUE);
            y += lh + Scale(2) + eh + gap + Scale(4);
            // 系统提示词：标签 + 多行框 + 「恢复默认」按钮 + 默认提示说明
            ::MoveWindow(::GetDlgItem(hwnd, IDC_SET_LBL_SYSPROMPT), pad, y, w - Scale(130), lh, TRUE);
            ::MoveWindow(st->resetPromptBtn, pad + w - Scale(130), y - Scale(2), Scale(130), Scale(22), TRUE);
            y += lh + Scale(2);
            const int promptH = Scale(96);
            ::MoveWindow(st->sysPrompt, pad, y, w, promptH, TRUE);
            y += promptH + Scale(2);
            ::MoveWindow(st->sysPromptHint, pad, y, w, Scale(20), TRUE);
            y += Scale(20) + gap + Scale(4);
            // 自动抓取一行
            ::MoveWindow(::GetDlgItem(hwnd, IDC_SET_LBL_AUTOFETCH), pad, y, Scale(260), lh, TRUE);
            ::MoveWindow(st->autoFetch, pad, y + lh + Scale(2), Scale(120), eh, TRUE);
            ::MoveWindow(st->autoFetchHint, pad + Scale(130), y + lh + Scale(4), w - Scale(130), lh, TRUE);
            y += lh + Scale(2) + eh + gap + Scale(4);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_SET_LBL_STORE), pad, y, w, lh, TRUE);
            y += lh + Scale(2);
            ::MoveWindow(st->storeLabel, pad, y, w, Scale(40), TRUE);
            y += Scale(40) + gap;
            ::MoveWindow(st->hint, pad, y, w, Scale(34), TRUE);
            y += Scale(34) + gap;
            ::MoveWindow(st->status, pad, y, w, Scale(20), TRUE);
            const int btnH = Scale(30), btnW = Scale(120);
            const int by = rc.bottom - pad - btnH;
            ::MoveWindow(st->saveBtn, pad, by, btnW, btnH, TRUE);
            ::MoveWindow(st->testBtn, pad + btnW + Scale(8), by, btnW + Scale(20), btnH, TRUE);
            ::MoveWindow(st->closeBtn, rc.right - pad - btnW, by, btnW, btnH, TRUE);
            return 0;
        }
        case WM_GRT_AI_MODELS_DONE: {
            std::unique_ptr<ModelListResult> res(reinterpret_cast<ModelListResult*>(lp));
            if (!st || !res) return 0;
            st->fetchingModels = false;
            ::EnableWindow(st->getModelsBtn, TRUE);
            std::wstring msg;
            if (res->status == 200 && !res->models.empty()) {
                FillModelCombo(st, res->models);
                msg = Str(IDS_MSG_AI_MODELS_OK) + L"  (" + std::to_wstring(res->models.size()) + L")";
            } else if (res->status == 200) {
                msg = Str(IDS_MSG_AI_MODELS_EMPTY);
            } else {
                msg = Str(IDS_MSG_AI_MODELS_FAIL);
                if (res->status) msg += L"  (HTTP " + std::to_wstring(res->status) + L")";
                std::string detail = res->error;
                if (detail.empty() && res->status && res->status != 200) detail = res->url.empty() ? "" : "";
                if (!detail.empty()) msg += L"  " + W(detail);
            }
            GRT_LOGI("ai", "获取模型列表 url=" << U8(res->url) << " status=" << res->status
                                              << " count=" << res->models.size());
            RefreshStatus(st, msg);
            return 0;
        }
        case WM_GRT_AI_TEST_DONE: {
            std::unique_ptr<std::pair<int, std::string>> res(
                reinterpret_cast<std::pair<int, std::string>*>(lp));
            if (!st || !res) return 0;
            st->testing = false;
            ::EnableWindow(st->testBtn, TRUE);
            std::wstring msg;
            if (res->first == 200) {
                msg = Str(IDS_MSG_AI_TEST_OK) + L"  (HTTP 200)";
            } else {
                msg = Str(IDS_MSG_AI_TEST_FAIL) + L"  (HTTP " + std::to_wstring(res->first) + L")";
                std::string body = res->second;
                if (body.size() > 300) body = body.substr(0, 300) + "...";
                if (!body.empty()) msg += L"  " + W(body);
            }
            GRT_LOGI("ai", "测试连接 status=" << res->first << " bodyLen=" << res->second.size());
            RefreshStatus(st, msg);
            // 连接正常 → 顺手把模型列表也拉回来（用户下一步就是要选模型）
            if (res->first == 200) StartModelFetch(hwnd, st);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_SET_RESET_PROMPT:   // 恢复默认：清空即用内置提示词
                    SetText(st->sysPrompt, L"");
                    SetText(st->sysPromptHint, Str(IDS_AI_SET_PROMPT_DEFAULT_HINT));
                    SetFocus(st->sysPrompt);
                    return 0;
                case IDC_SET_SAVE:
                    SaveFromUi(st);
                    return 0;
                case IDC_SET_TEST:
                    TestConnection(st, hwnd);
                    return 0;
                case IDC_SET_GETMODELS:
                    StartModelFetch(hwnd, st);
                    return 0;
                case IDC_SET_CLOSE:
                case IDCANCEL:
                    ::DestroyWindow(hwnd);
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
            if (HandleCtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), &res))
                return res;
            break;
        }
        case WM_ERASEBKGND: {
            if (Th().dark) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                ::FillRect(reinterpret_cast<HDC>(wp), &rc, Th().bgBrush);
                return 1;
            }
            break;
        }
        case WM_CLOSE:
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

void EnsureSetClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SetProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.lpszClassName = kSettingsWindowClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void ShowSettingsWindow(HWND owner, HWND notify) {
    EnsureSetClass();
    AiConfig init = LoadAiConfig();
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kSettingsWindowClass,
                               Str(IDS_TITLE_AI_SETTINGS).c_str(),
                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, CW_USEDEFAULT,
                               CW_USEDEFAULT, Scale(680), Scale(680), owner, nullptr,
                               ::GetModuleHandleW(nullptr), &init);
    if (!h) return;
    if (notify) ::SendMessageW(h, WM_GRT_SET_NOTIFY_TARGET, 0, reinterpret_cast<LPARAM>(notify));
    CenterOnOwner(h, owner);
    ::ShowWindow(h, SW_SHOW);
    ::UpdateWindow(h);
}

}  // namespace grt::gui
