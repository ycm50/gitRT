// 进度窗口：顺序执行 BuiltCommand 的多条命令，流式回传输出，可取消
// （《技术实现设计》§9.5 / §6.4）
//
// 注意初始化顺序：spec/cmd/cancel 必须在 WM_NCCREATE 中从 lpCreateParams 拷贝完成，
// 因为 WM_CREATE 里就会启动工作线程读取它们（否则是数据竞争）。
#include "gui.h"

#include <atomic>
#include <thread>

namespace grt::gui {

const wchar_t* kProgressClass = L"GitRT.ProgressWindow";

enum : int { IDC_PRG_BAR = 300, IDC_PRG_LOG, IDC_PRG_CANCEL, IDC_PRG_CLOSE };
enum : UINT_PTR { kTimerReveal = 1 };

// 创建参数（与窗口状态分离，避免拷贝 std::thread / std::atomic）
struct ProgInit {
    CommandSpec                        spec;
    BuiltCommand                       cmd;
    std::shared_ptr<CancellationToken> cancel;
};

struct TaskOutcome {
    bool         ok = false;
    bool         cancelled = false;
    int          exitCode = 0;
    uint64_t     ms = 0;
    std::wstring failedCommand;
    std::string  err;
    std::string  out;      // 合并输出（供宿主窗口显示"运行结果"）
};

namespace {

struct ProgState {
    CommandSpec                        spec{};
    BuiltCommand                       cmd;
    HWND                               title = nullptr, phase = nullptr, bar = nullptr, logEdit = nullptr;
    HWND                               cancelBtn = nullptr, closeBtn = nullptr;
    std::shared_ptr<CancellationToken> cancel;
    std::thread                        worker;
    std::atomic<bool>                  finished{false};
    bool                               shown = false;
    int                                lastPercent = -1;
    size_t                             logChars = 0;
};

// 取一行里最后一个百分比（git 进度行形如 "Receiving objects:  43% (13/30)"）
int LastPercent(const std::string& line) {
    int best = -1;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '%') continue;
        size_t j = i;
        while (j > 0 && line[j - 1] >= '0' && line[j - 1] <= '9') --j;
        if (j == i) continue;
        int v = 0;
        for (size_t k = j; k < i; ++k) v = v * 10 + (line[k] - '0');
        if (v >= 0 && v <= 100) best = v;
    }
    return best;
}

void PostLog(HWND hwnd, const std::string& line) {
    ::PostMessageW(hwnd, WM_GRT_TASK_LOG, 0, reinterpret_cast<LPARAM>(new std::string(line)));
}

void WorkerProc(ProgState* st, HWND hwnd) {
    auto runner = MakeProcessGitRunner();
    auto* oc = new TaskOutcome();
    const bool stream = (st->spec.exec == ExecKind::CliStream);

    for (size_t i = 0; i < st->cmd.argvList.size(); ++i) {
        const auto& argv = st->cmd.argvList[i];
        std::wstring disp;
        for (const auto& a : argv) {
            if (!disp.empty()) disp += L' ';
            disp += QuoteArg(a);
        }
        PostLog(hwnd, "> git " + WideToUtf8(disp));

        Invocation inv;
        inv.exe = App().gitExe;
        inv.argv = argv;
        inv.cwd = st->cmd.cwd.empty() ? App().repoRoot : st->cmd.cwd;
        inv.env = BuildGitEnvironment(false, L"");
        inv.timeoutMs = stream ? 0 : 120000;

        std::mutex m;
        std::string pending;
        auto onChunk = [&](const char* p, size_t n) {
            std::lock_guard<std::mutex> lk(m);
            pending.append(p, n);
            for (;;) {
                const size_t nl = pending.find('\n');
                const size_t cr = pending.find('\r');
                size_t cut = std::string::npos;
                if (nl != std::string::npos && cr != std::string::npos) cut = (std::min)(nl, cr);
                else if (nl != std::string::npos) cut = nl;
                else if (cr != std::string::npos) cut = cr;
                if (cut == std::string::npos) break;
                std::string line = pending.substr(0, cut);
                pending.erase(0, cut + 1);
                if (line.empty()) continue;
                const int pct = LastPercent(line);
                if (oc->out.size() < (256u << 10)) {   // 只保留前 256KB，避免无界增长
                    oc->out += line;
                    oc->out += '\n';
                }
                if (pct >= 0) {
                    ::PostMessageW(hwnd, WM_GRT_TASK_PROGRESS, 0, static_cast<LPARAM>(pct));
                } else {
                    PostLog(hwnd, line);
                }
            }
        };

        const RunResult r = runner->RunSync(inv, st->cancel.get(), onChunk, onChunk);
        oc->ms += r.elapsedMs;
        oc->err = r.err;
        if (!pending.empty()) PostLog(hwnd, pending);
        if (r.cancelled) {
            oc->cancelled = true;
            oc->ok = false;
            oc->failedCommand = disp;
            break;
        }
        if (r.spawnFailed || r.exitCode != 0) {
            oc->ok = false;
            oc->exitCode = r.exitCode;
            oc->failedCommand = disp;
            if (!r.err.empty()) PostLog(hwnd, r.err);
            break;
        }
        oc->ok = true;
    }
    ::PostMessageW(hwnd, WM_GRT_TASK_DONE, 0, reinterpret_cast<LPARAM>(oc));
}

void AppendLog(HWND edit, const std::wstring& line, size_t* counter) {
    if (!edit) return;
    if (*counter > 300000) {   // 超长日志保护
        ::SetWindowTextW(edit, (Str(IDS_MSG_LOG_TRUNCATED) + L"\r\n").c_str());
        *counter = 20;
    }
    const int len = ::GetWindowTextLengthW(edit);
    ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    std::wstring text = line;
    text += L"\r\n";
    ::SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    ::SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    *counter += text.size();
}

LRESULT CALLBACK ProgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ProgState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* s = new ProgState();
            if (auto* init = reinterpret_cast<ProgInit*>(cs->lpCreateParams)) {
                s->spec = init->spec;      // ★ 必须早于 WM_CREATE（工作线程会读）
                s->cmd = init->cmd;
                s->cancel = init->cancel;
            }
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            return TRUE;
        }
        case WM_CREATE: {
            st->title = MakeChild(hwnd, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, Th().fontBold);
            st->phase = MakeChild(hwnd, WC_STATICW, Str(IDS_MSG_RUNNING),
                                  WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, Th().fontUi);
            st->bar = MakeChild(hwnd, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0,
                                IDC_PRG_BAR, nullptr);
            ::SendMessageW(st->bar, PBM_SETRANGE32, 0, 100);
            st->logEdit = MakeChild(hwnd, WC_EDITW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL | WS_BORDER,
                                    WS_EX_CLIENTEDGE, IDC_PRG_LOG, Th().fontMono);
            st->cancelBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CANCEL),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_PRG_CANCEL, Th().fontUi);
            st->closeBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED, 0, IDC_PRG_CLOSE,
                                     Th().fontUi);
            SetText(st->title, Str(st->spec.titleRes));
            ThemeApply(hwnd);
            ::SetTimer(hwnd, kTimerReveal, 1500, nullptr);   // 1.5s 后揭示，避免短命令闪烁
            st->worker = std::thread(WorkerProc, st, hwnd);
            return 0;
        }
        case WM_TIMER: {
            if (wp == kTimerReveal && st && !st->finished.load() && !st->shown) {
                st->shown = true;
                ::ShowWindow(hwnd, SW_SHOW);
            }
            return 0;
        }
        case WM_SIZE: {
            if (!st) break;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const int pad = Scale(12), btnH = Scale(30), btnW = Scale(110);
            int y = pad;
            ::MoveWindow(st->title, pad, y, rc.right - pad * 2, Scale(22), TRUE);
            y += Scale(24);
            ::MoveWindow(st->phase, pad, y, rc.right - pad * 2, Scale(20), TRUE);
            y += Scale(22);
            ::MoveWindow(st->bar, pad, y, rc.right - pad * 2, Scale(18), TRUE);
            y += Scale(24);
            const int bottom = rc.bottom - pad - btnH - Scale(8);
            ::MoveWindow(st->logEdit, pad, y, rc.right - pad * 2, (std::max)(Scale(80), bottom - y), TRUE);
            const int by = rc.bottom - pad - btnH;
            ::MoveWindow(st->cancelBtn, rc.right - pad - btnW * 2 - Scale(8), by, btnW, btnH, TRUE);
            ::MoveWindow(st->closeBtn, rc.right - pad - btnW, by, btnW, btnH, TRUE);
            return 0;
        }
        case WM_GRT_TASK_PROGRESS: {
            const int pct = static_cast<int>(lp);
            if (st && st->bar && pct >= 0 && pct != st->lastPercent) {
                st->lastPercent = pct;
                ::SendMessageW(st->bar, PBM_SETPOS, static_cast<WPARAM>(pct), 0);
            }
            return 0;
        }
        case WM_GRT_TASK_LOG: {
            std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lp));
            if (st && line) AppendLog(st->logEdit, W(*line), &st->logChars);
            return 0;
        }
        case WM_GRT_TASK_DONE: {
            std::unique_ptr<TaskOutcome> oc(reinterpret_cast<TaskOutcome*>(lp));
            if (!st || !oc) return 0;
            st->finished.store(true);
            ::KillTimer(hwnd, kTimerReveal);

            // 先把"运行结果"汇总回宿主窗口（AI 助手窗口靠它显示结果）
            HWND owner = ::GetWindow(hwnd, GW_OWNER);
            if (owner) {
                auto* ts = new TaskSummary();
                ts->ok = oc->ok;
                ts->cancelled = oc->cancelled;
                ts->exitCode = oc->exitCode;
                ts->ms = oc->ms;
                ts->command = st->cmd.display;
                std::string body = oc->out;
                if (body.size() > 8000) body = body.substr(body.size() - 8000);   // 只留尾部
                ts->output = W(body);
                ::PostMessageW(owner, WM_GRT_TASK_FINISHED, 0, reinterpret_cast<LPARAM>(ts));
            }

            std::wstring phase;
            if (oc->cancelled) {
                phase = Str(IDS_MSG_CANCELLED);
            } else if (oc->ok) {
                phase = Str(IDS_MSG_DONE) + L"  (" + std::to_wstring(oc->ms) + L" ms)";
                ::SendMessageW(st->bar, PBM_SETPOS, 100, 0);
            } else {
                phase = Str(IDS_MSG_FAILED) + L"  (exit=" + std::to_wstring(oc->exitCode) + L")";
                if (!oc->failedCommand.empty()) phase += L"   " + oc->failedCommand;
            }
            SetText(st->phase, phase);
            AppendLog(st->logEdit, L"--- " + phase, &st->logChars);

            // 成功且从未显示过 → 静默关闭（状态刷新会体现结果）
            if (oc->ok && !st->shown) {
                ::DestroyWindow(hwnd);
                if (owner) ::PostMessageW(owner, WM_GRT_STATUS_RELOAD, 0, 0);
                return 0;
            }
            if (!st->shown) {
                st->shown = true;
                ::ShowWindow(hwnd, SW_SHOW);
            }
            ::EnableWindow(st->cancelBtn, FALSE);
            ::EnableWindow(st->closeBtn, TRUE);
            ::SetForegroundWindow(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_PRG_CANCEL:
                    if (st && st->cancel) {
                        st->cancel->Cancel();
                        SetText(st->phase, Str(IDS_MSG_CANCELLING));
                        ::EnableWindow(st->cancelBtn, FALSE);
                    }
                    return 0;
                case IDC_PRG_CLOSE:
                case IDCANCEL:
                    if (st && !st->finished.load()) return 0;   // 运行中禁止关闭
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
            if (st && !st->finished.load()) return 0;
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

void EnsureProgressClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ProgProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kProgressClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void RunBuiltCommand(HWND owner, const CommandSpec& spec, const BuiltCommand& cmd) {
    if (App().gitExe.empty()) {
        ::MessageBoxW(owner, Str(IDS_MSG_GIT_NOT_FOUND).c_str(), Str(IDS_TITLE_MAIN).c_str(),
                      MB_OK | MB_ICONWARNING);
        return;
    }
    EnsureProgressClass();
    ProgInit init;
    init.spec = spec;
    init.cmd = cmd;
    init.cancel = std::make_shared<CancellationToken>();

    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kProgressClass, Str(IDS_TITLE_PROGRESS).c_str(),
                               (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME), CW_USEDEFAULT,
                               CW_USEDEFAULT, Scale(780), Scale(520), owner, nullptr,
                               ::GetModuleHandleW(nullptr), &init);
    if (!h) return;
    CenterOnOwner(h, owner);
    ::ShowWindow(h, SW_HIDE);   // 由 WM_TIMER 决定是否揭示
}

}  // namespace grt::gui
