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

// 创建参数（与窗口状态分离，避免拷贝 std::thread / std::atomic）
struct ProgInit {
    CommandSpec                        spec;
    BuiltCommand                       cmd;
    std::shared_ptr<CancellationToken> cancel;
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

// 顺序执行 BuiltCommand 的多条命令：日志/进度都投给 hwnd，结果写进 oc。
// 进度窗口与参数面板的"运行结果"框共用这一段。
void RunCommandSequence(HWND hwnd, const CommandSpec& spec, const BuiltCommand& cmd,
                        CancellationToken* cancel, TaskOutcome* oc) {
    auto runner = MakeProcessGitRunner();
    const bool stream = (spec.exec == ExecKind::CliStream);

    for (size_t i = 0; i < cmd.argvList.size(); ++i) {
        const auto& argv = cmd.argvList[i];
        std::wstring disp;
        for (const auto& a : argv) {
            if (!disp.empty()) disp += L' ';
            disp += QuoteArg(a);
        }
        PostLog(hwnd, "> git " + WideToUtf8(disp));

        Invocation inv;
        inv.exe = App().gitExe;
        inv.argv = argv;
        inv.cwd = cmd.cwd.empty() ? App().repoRoot : cmd.cwd;
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

        const RunResult r = runner->RunSync(inv, cancel, onChunk, onChunk);
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
}

void WorkerProc(ProgState* st, HWND hwnd) {
    auto* oc = new TaskOutcome();
    RunCommandSequence(hwnd, st->spec, st->cmd, st->cancel.get(), oc);
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
    std::wstring text = ToCrlf(line);   // git 输出是 LF-only，Edit 不认裸 LF（会把所有行并成一行）
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
            SetText(st->title, Str(st->spec.titleRes) + L"   —   " + st->cmd.display);
            ThemeApply(hwnd);
            // 执行一开始就出现（不再等 1.5s）：用户点"执行"必须马上看到"正在跑什么命令"。
            // 窗口结束后**保留**（不再对短命令静默关闭），日志里第一行就是真实命令行。
            st->shown = true;
            ::SetWindowTextW(hwnd, (Str(st->spec.titleRes) + L" — GitRT").c_str());
            ::ShowWindow(hwnd, SW_SHOW);
            st->worker = std::thread(WorkerProc, st, hwnd);
            return 0;
        }
        case WM_KEYDOWN:
            // Esc：跑完就能关（运行中忽略，避免误关掉正在执行的命令）
            if (wp == VK_ESCAPE && st && st->finished.load()) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            break;
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
            // （揭示定时器已移除：窗口在工作线程启动前就显示）

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

            // ★ 执行结束**一定**刷新状态（无论成功/失败/取消）：命令可能已经改了工作区，
            //   旧实现只在"静默成功的短命令"这一路上刷新，慢命令与失败路径都不刷新。
            //   wp=1 = "先把 git status 重新读一遍再刷新视图"。
            HWND target = App().main ? App().main : owner;
            if (target) ::PostMessageW(target, WM_GRT_STATUS_RELOAD, 1, 0);

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

            // 窗口保留，让人看清"跑了哪条命令 + 输出是什么"；Close 关闭（Esc 亦可）
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
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.lpszClassName = kProgressClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

// 就地执行：不建窗口，把消息投给 target（参数面板的"运行结果"框）
void RunBuiltCommandOn(HWND target, const CommandSpec& spec, const BuiltCommand& cmd,
                       std::shared_ptr<CancellationToken> cancel) {
    if (!target) return;
    struct Job {
        CommandSpec                        spec;
        BuiltCommand                       cmd;
        std::shared_ptr<CancellationToken> cancel;
    };
    auto* job = new Job{spec, cmd, std::move(cancel)};
    std::thread([target, job]() {
        auto* oc = new TaskOutcome();
        RunCommandSequence(target, job->spec, job->cmd, job->cancel.get(), oc);
        ::PostMessageW(target, WM_GRT_TASK_DONE, 0, reinterpret_cast<LPARAM>(oc));
        delete job;
    }).detach();
}
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
    ::ShowWindow(h, SW_HIDE);   // 创建过程先隐藏；WM_CREATE 里控件就绪后立即 SW_SHOW
}

}  // namespace grt::gui
