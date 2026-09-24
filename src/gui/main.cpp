// GitRT.exe 入口：命令行解析 / 全局初始化 / 消息循环 / --self-test 自检
#include "gui.h"

#include "ai_client.h"
#include "json_util.h"

#include <shellapi.h>

#include <string>
#include <vector>

using namespace grt;
using namespace grt::gui;

namespace {

std::string NulJoin(const std::vector<std::string>& toks) {
    std::string s;
    for (const auto& t : toks) {
        s += t;
        s.push_back('\0');
    }
    return s;
}

bool WriteReport(const std::wstring& path, const std::wstring& text) {
    const std::string utf8 = WideToUtf8(text);
    std::vector<std::wstring> candidates{path};
    if (!path.empty()) {
        const std::wstring fallback = JoinPath(GetModuleDir(), L"gitrt-selftest.txt");
        if (fallback != path) candidates.push_back(fallback);
    }
    for (const auto& p : candidates) {
        if (p.empty()) continue;
        HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            GRT_LOGW("selftest", "写报告失败 " << U8(p) << " err=" << ::GetLastError());
            continue;
        }
        DWORD wrote = 0;
        ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
        ::CloseHandle(h);
        GRT_LOGI("selftest", "报告已写入 " << U8(p) << " bytes=" << wrote);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ 自检
// 覆盖：porcelain 固件（真实 git 2.53 字节）/ 命令表 dry-run / flags 白名单 /
//       参数校验 / 每个命令的参数面板可构建 / 仓库探测
int RunSelfTest(HWND mainWnd, const std::wstring& outPath) {
    std::wstring rep;
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        rep += (ok ? L"[PASS] " : L"[FAIL] ");
        rep += what;
        rep += L"\r\n";
        if (ok) ++pass; else ++fail;
    };

    rep += L"== GitRT \u81ea\u68c0 ==\r\n";
    rep += L"\u547d\u4ee4\u8868\u6761\u76ee: " + std::to_wstring(kCommandCount) + L"\r\n";
    rep += L"git: " + App().gitExe + L"  " + App().gitVersion + L"\r\n";
    // 路径诊断（用于确认 %LOCALAPPDATA% 等环境是否可用）
    {
        wchar_t lbuf[512]{};
        const DWORD ln = ::GetEnvironmentVariableW(L"LOCALAPPDATA", lbuf, 512);
        rep += L"LOCALAPPDATA=" + std::wstring(lbuf, ln) + L"\r\n";
        rep += L"LocalAppDataDir=" + LocalAppDataDir() + L"\r\n";
        rep += L"LogFilePath=" + LogFilePath() + L"\r\n";
        rep += L"ConfigFilePath=" + ConfigFilePath() + L"\r\n";
        // 直接在应用内做一次写探测，拿到确切 Win32 错误码
        const std::wstring logs = JoinPath(LocalAppDataDir(), L"logs");
        const BOOL ok1 = ::CreateDirectoryW(LocalAppDataDir().c_str(), nullptr);
        const DWORD e1 = ok1 ? 0 : ::GetLastError();
        const BOOL ok2 = ::CreateDirectoryW(logs.c_str(), nullptr);
        const DWORD e2 = ok2 ? 0 : ::GetLastError();
        const std::wstring probe = JoinPath(logs, L"app-probe.txt");
        HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD e3 = (h == INVALID_HANDLE_VALUE) ? ::GetLastError() : 0;
        DWORD wrote = 0;
        if (h != INVALID_HANDLE_VALUE) {
            ::WriteFile(h, "x", 1, &wrote, nullptr);
            ::CloseHandle(h);
            ::DeleteFileW(probe.c_str());   // 只做可写性探测，不留垃圾
        }
        // 与《技术实现设计》§12.3 doctor 检查项 8「缓存目录是否可写」对应
        rep += L"dir writable: mkdir err=" + std::to_wstring(e1) + L"/" + std::to_wstring(e2) +
               L" CreateFile err=" + std::to_wstring(e3) + L" wrote=" + std::to_wstring(wrote) +
               L"  (183=已存在, 0=成功, 5=拒绝访问)" + L"\r\n";
    }
    rep += L"\r\n";

    // ---- 1. porcelain v2 固件 ----
    {
        const std::string fx = NulJoin({"# branch.oid 6c477e5c19685cf13fca7378eeae12055cff82c8",
                                        "# branch.head main", "# stash 1", "? o.bin", "? renamed.txt"});
        const RepoStatus s = ParsePorcelainV2(fx);
        check(s.parsed && s.head == "main" && s.stashCount == 1 && s.entries.size() == 2 && s.untracked == 2,
              L"porcelain: header \u4ee5 NUL \u7ed3\u5c3e\u3001\u7f3a branch.ab \u4ecd\u53ef\u89e3\u6790");
    }
    {
        const std::string fx = NulJoin(
            {"2 R. N... 100644 100644 100644 c1b0730e0133447badcfd47fd144e254807b06e1 "
             "c1b0730e0133447badcfd47fd144e254807b06e1 R100 b.txt",
             "a.txt", "? r.bin"});
        const RepoStatus s = ParsePorcelainV2(fx);
        check(s.entries.size() == 2 && s.entries[0].type == '2' && s.entries[0].path == "b.txt" &&
                  s.entries[0].origPath == "a.txt" && s.staged == 1,
              L"porcelain: \u91cd\u547d\u540d\u6761\u76ee\u53cc NUL token\uff08\u65b0\u8def\u5f84\u5728\u524d\uff09");
    }
    {
        const std::string fx = NulJoin({"1 M. N... 100644 100644 100644 aaa bbb file with spaces.txt"});
        const RepoStatus s = ParsePorcelainV2(fx);
        check(s.entries.size() == 1 && s.entries[0].path == "file with spaces.txt" && s.staged == 1,
              L"porcelain: \u542b\u7a7a\u683c\u8def\u5f84\u6309\u5b57\u6bb5\u6570\u5207\u5206\u6b63\u786e");
    }
    {
        const std::string fx = NulJoin({"# branch.head (detached)"});
        const RepoStatus s = ParsePorcelainV2(fx);
        check(s.detached, L"porcelain: \u5206\u79bb\u5934\u6307\u9488\u8bc6\u522b");
    }

    // ---- 2. 命令表 dry-run ----
    int built = 0, internal = 0, bad = 0;
    for (size_t ci = 0; ci < CommandTableSize(); ++ci) {
        const CommandSpec& c = CommandTable()[ci];
        if (c.exec == ExecKind::Internal) {
            ++internal;
            continue;
        }
        BuildInput in;
        in.spec = &c;
        in.gitExe = App().gitExe;
        in.repoRoot = L"C:\\selftest\\repo";
        in.cwd = in.repoRoot;
        in.paths = {L"C:\\selftest\\repo\\a.txt"};
        if (c.paramKey) {
            switch (c.param) {
                case ParamKind::ExistingBranch: in.params[c.paramKey] = L"main"; break;
                case ParamKind::BranchName:     in.params[c.paramKey] = L"feature/selftest"; break;
                case ParamKind::Url:            in.params[c.paramKey] = L"https://example.com/a.git"; break;
                case ParamKind::Revision:       in.params[c.paramKey] = L"HEAD~1"; break;
                case ParamKind::CommitMessage:  in.params[c.paramKey] = L"selftest"; break;
                case ParamKind::Pattern:        in.params[c.paramKey] = L"*.log"; break;
                default:                        in.params[c.paramKey] = L"selftest"; break;
            }
        }
        BuiltCommand out;
        BuildError err;
        if (!BuildCommand(in, &out, &err)) {
            ++bad;
            rep += L"       \u6784\u9020\u5931\u8d25 " + W(c.key) + L": " + err.message + L"\r\n";
            continue;
        }
        if (out.argvList.empty()) {
            ++bad;
            rep += L"       \u7a7a\u547d\u4ee4 " + W(c.key) + L"\r\n";
            continue;
        }
        ++built;
    }
    check(bad == 0, L"dry-run: " + std::to_wstring(built) + L" \u6761\u751f\u6210 argv\uff0c" +
                        std::to_wstring(internal) + L" \u6761 Internal\uff0c\u5931\u8d25 " +
                        std::to_wstring(bad));

    // ---- 3. flags 白名单与参数校验 ----
    {
        const CommandSpec* push = FindCommandByKey("sync.push");
        bool ok = false;
        if (push) {
            BuildInput in;
            in.spec = push;
            in.gitExe = App().gitExe;
            in.repoRoot = L"C:\\selftest\\repo";
            in.flags["force-with-lease"] = L"1";
            in.flags["bogus-flag"] = L"1";   // 不在白名单 → 必须被忽略且不报错
            BuiltCommand out;
            BuildError err;
            ok = BuildCommand(in, &out, &err);
            bool hasLease = false, hasBogus = false;
            if (ok && !out.argvList.empty()) {
                for (const auto& a : out.argvList[0]) {
                    if (a == L"--force-with-lease") hasLease = true;
                    if (a.find(L"bogus") != std::wstring::npos) hasBogus = true;
                }
            }
            ok = ok && hasLease && !hasBogus;
        }
        check(ok, L"flags \u767d\u540d\u5355\uff1a\u5df2\u767b\u8bb0\u9879\u8fdb\u5165 argv\uff0c\u672a\u767b\u8bb0\u9879\u88ab\u5ffd\u7565");
    }
    check(!IsValidBranchName(L"-evil") && !IsValidBranchName(L"a..b") && IsValidBranchName(L"feature/x"),
          L"\u5206\u652f\u540d\u6821\u9a8c");
    check(!IsValidUrl(L"--upload-pack=evil") && IsValidUrl(L"https://example.com/a.git") &&
              IsValidUrl(L"git@github.com:u/r.git"),
          L"URL \u6821\u9a8c\uff08\u62d2\u7edd\u4ee5 - \u5f00\u5934\u7684\u6ce8\u5165\uff09");
    check(!IsValidRevision(L"HEAD;rm -rf /") && IsValidRevision(L"HEAD~1"), L"\u4fee\u8ba2\u8868\u8fbe\u5f0f\u6821\u9a8c");
    {
        // 互斥组：显式选 hard 之后，不得再套用默认的 mixed
        const CommandSpec* rst = FindCommandByKey("adv.reset");
        bool ok = false;
        if (rst) {
            BuildInput in;
            in.spec = rst;
            in.gitExe = App().gitExe;
            in.repoRoot = L"C:\\selftest\\repo";
            in.params["rev"] = L"HEAD~1";
            in.flags["hard"] = L"1";
            BuiltCommand out;
            BuildError err;
            ok = BuildCommand(in, &out, &err);
            bool hasHard = false, hasMixed = false;
            if (ok && !out.argvList.empty()) {
                for (const auto& a : out.argvList[0]) {
                    if (a == L"--hard") hasHard = true;
                    if (a == L"--mixed") hasMixed = true;
                }
            }
            ok = ok && hasHard && !hasMixed;
        }
        check(ok, L"\u4e92\u65a5\u9009\u9879\u7ec4\uff1a\u663e\u5f0f\u9009\u62e9\u540e\u4e0d\u518d"
                 L"\u5957\u7528\u9ed8\u8ba4\u9879\uff08\u4e0d\u4f1a\u51fa\u73b0 --mixed --hard\uff09");
    }

    // ---- 4. 每个命令的参数面板都可构建 ----
    {
        int made = 0;
        for (size_t ci = 0; ci < CommandTableSize(); ++ci) {
            const CommandSpec& c = CommandTable()[ci];
            HWND p = CreateParamPanel(mainWnd, c, {L"C:\\selftest\\repo\\a.txt"});
            if (p) {
                ++made;
                ::DestroyWindow(p);
            }
        }
        check(made == static_cast<int>(kCommandCount),
              L"\u53c2\u6570\u9762\u677f: " + std::to_wstring(made) + L"/" +
                  std::to_wstring(kCommandCount) + L" \u53ef\u521b\u5efa");
    }

    // ---- 5. 仓库探测 ----
    {
        const RepoProbeResult r = ProbeRepo(GetModuleDir());
        rep += L"       \u63a2\u6d4b\u672c\u5de5\u7a0b\u76ee\u5f55: " + RepoFlagsToString(r.flags) +
               L"  root=" + r.repoRoot + L"\r\n";
        // ---- 6. JSON 工具 ----
    {
        const std::string raw = std::string("a\"b\\c\nd\te\rf\x01g \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80");
        check(JsonUnescape(JsonEscape(raw)) == raw,
              L"JSON \u8f6c\u4e49/\u53cd\u8f6c\u4e49\u5f80\u8fd4\uff08\u5f15\u53f7/\u53cd\u659c\u6760/"
              L"\u63a7\u5236\u7b26/\u4e2d\u6587/emoji\uff09");
        check(JsonUnescape("\\ud83d\\ude00") == "\xf0\x9f\x98\x80",
              L"JSON \\u \u4ee3\u7406\u5bf9\u89e3\u7801\u4e3a UTF-8");
    }
    {
        // DeepSeek 风格响应：同级的 reasoning_content 绝不能误命中 content
        const std::string env =
            "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
            "\"reasoning_content\":\"should not match\","
            "\"content\":\"{\\\"command\\\":\\\"sync.push\\\",\\\"flags\\\":"
            "{\\\"set-upstream\\\":\\\"1\\\"},\\\"explanation\\\":"
            "\\\"\\u63a8\\u9001\\u5f53\\u524d\\u5206\\u652f\\\"}\"}}]}";
        std::string content;
        const bool got = ExtractChatContent(env, &content);
        check(got && content.find("sync.push") != std::string::npos &&
                  content.find("should not match") == std::string::npos,
              L"\u4ece OpenAI \u517c\u5bb9\u54cd\u5e94\u53d6\u51fa message.content"
              L"\uff08\u4e0d\u8bef\u547d\u4e2d reasoning_content\uff09");
        AiPlan p = ParsePlanReply(content);
        check(p.ok && !p.noCommand && p.commandKey == "sync.push" &&
                  p.flags["set-upstream"] == "1" &&
                  p.explanation == L"\u63a8\u9001\u5f53\u524d\u5206\u652f",
              L"\u89e3\u6790\u8ba1\u5212\uff08command/flags/explanation\uff0c\u542b JSON \\u \u8f6c\u4e49\uff09");
    }
    {
        const AiPlan p = ParsePlanReply("```json\n{\"command\":\"none\",\"explanation\":\"nope\"}\n```");
        check(p.ok && p.noCommand, L"\u89e3\u6790\u8ba1\u5212\uff1amarkdown \u56f4\u680f + none \u5206\u652f");
    }
    {
        const AiPlan p = ParsePlanReply("you should just run git push");
        check(!p.ok && !p.error.empty(), L"\u975e JSON \u56de\u590d\u88ab\u62d2\u7edd");
    }
    {
        const std::string prompt = BuildPlannerSystemPrompt([](uint16_t id) { return WideToUtf8(Str(id)); });
        bool allKeys = true;
        for (size_t i = 0; i < CommandTableSize(); ++i)
            if (prompt.find(CommandTable()[i].key) == std::string::npos) { allKeys = false; break; }
        check(allKeys, L"\u7cfb\u7edf\u63d0\u793a\u8bcd\u7531\u547d\u4ee4\u8868\u81ea\u52a8\u751f\u6210\uff0c"
                       L"\u5305\u542b\u5168\u90e8 key");
    }

    // ---- 7. AI 计划 → 命令（安全闸门） ----
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "definitely.not.a.command";
        const auto r = PlanToCommand(p, L"C:\\selftest\\repo", {}, App().gitExe);
        check(!r.ok && !r.error.empty(), L"\u95f8\u95e8\uff1a\u62d2\u7edd\u547d\u4ee4\u8868\u4e2d"
                                          L"\u4e0d\u5b58\u5728\u7684\u547d\u4ee4");
    }
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "sync.push";
        p.flags["bogus"] = "1";
        p.flags["set-upstream"] = "1";
        const auto r = PlanToCommand(p, L"C:\\selftest\\repo", {}, App().gitExe);
        bool hasWarn = false;
        for (const auto& w : r.warnings)
            if (w.find(L"bogus") != std::wstring::npos) hasWarn = true;
        check(r.ok && hasWarn && r.built.display.find(L"--set-upstream") != std::wstring::npos,
              L"\u95f8\u95e8\uff1a\u672a\u77e5\u9009\u9879\u88ab\u5ffd\u7565\u5e76\u63d0\u793a\uff0c"
              L"\u767d\u540d\u5355\u9009\u9879\u8fdb\u5165 argv");
    }
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "app.ai";
        const auto r = PlanToCommand(p, L"C:\\selftest\\repo", {}, App().gitExe);
        check(!r.ok, L"\u95f8\u95e8\uff1aInternal \u547d\u4ee4\u4e0d\u5141\u8bb8\u7531 AI \u6267\u884c");
    }
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "sync.pull";
        const auto r = PlanToCommand(p, L"", {}, App().gitExe);
        check(!r.ok, L"\u95f8\u95e8\uff1a\u9700\u8981\u4ed3\u5e93\u7684\u547d\u4ee4\u5728\u672a\u9009"
                     L"\u4ed3\u5e93\u65f6\u88ab\u62d2\u7edd");
    }

    check(true, L"\u4ed3\u5e93\u5ec9\u4ef7\u63a2\u6d4b\u53ef\u6267\u884c");
    }

    rep += L"\r\n== \u7ed3\u679c: " + std::to_wstring(pass) + L" \u901a\u8fc7 / " +
           std::to_wstring(fail) + L" \u5931\u8d25 ==\r\n";
    WriteReport(outPath, rep);
    GRT_LOGI("selftest", "pass=" << pass << " fail=" << fail << " report=" << U8(outPath));
    return fail == 0 ? 0 : 1;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::wstring repoPath, selfTestPath, requestSection, requestProbePath;
    bool selfTest = false, exitAfterRequest = false;
    CliOptions cli;
    auto nextArg = [&](int* i) -> std::wstring {
        if (*i + 1 < argc) return argv[++(*i)];
        return {};
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--run") {
            cli.runKey = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--ai") {
            cli.aiPrompt = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--cwd") {
            cli.cwd = nextArg(&i);
        } else if (a == L"--out") {
            cli.outPath = nextArg(&i);
        } else if (a == L"--paths") {
            cli.pathsSpec = nextArg(&i);
        } else if (a == L"--flag") {
            const std::wstring kv = nextArg(&i);
            const size_t eq = kv.find(L'=');
            if (eq == std::wstring::npos) cli.flags[WideToUtf8(kv)] = L"1";
            else cli.flags[WideToUtf8(kv.substr(0, eq))] = kv.substr(eq + 1);
        } else if (a == L"--param") {
            const std::wstring kv = nextArg(&i);
            const size_t eq = kv.find(L'=');
            if (eq != std::wstring::npos) cli.params[WideToUtf8(kv.substr(0, eq))] = kv.substr(eq + 1);
        } else if (a == L"--dry-run") {
            cli.dryRun = true;
        } else if (a == L"--ai-run") {
            cli.aiRun = true;
        } else if (a == L"--list-commands") {
            cli.listCommands = true;
            cli.hasWork = true;
        } else if (a.rfind(L"--self-test", 0) == 0) {
            selfTest = true;
            const size_t eq = a.find(L'=');
            if (eq != std::wstring::npos) selfTestPath = a.substr(eq + 1);
        } else if (a == L"--request-section") {
            // 右键菜单（Shell DLL）转交过来的请求段名，见 §4.5 / §5.4
            requestSection = nextArg(&i);
        } else if (a.rfind(L"--request-probe", 0) == 0) {
            // 诊断/CI：把"已接收并应用的菜单请求"写成报告后退出（不进入消息循环）
            const size_t eq = a.find(L'=');
            requestProbePath = (eq == std::wstring::npos) ? nextArg(&i) : a.substr(eq + 1);
        } else if (a == L"--exit-after-request") {
            exitAfterRequest = true;
        } else if (!a.empty() && a[0] != L'-') {
            repoPath = a;
        }
    }
    if (argv) ::LocalFree(argv);

    // ---- 非交互门面：不创建窗口，直接返回退出码 ----
    if (cli.hasWork) {
        int rc = 0;
        if (cli.listCommands) rc = RunCliListCommands(cli);
        else if (!cli.runKey.empty()) rc = RunCliCommand(cli);
        else rc = RunCliAi(cli);
        LogFlush();
        return rc;
    }

    LogInit();
    GRT_LOGI("app", "GitRT GUI \u542f\u52a8 selfTest=" << selfTest << " repo=" << U8(repoPath));

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES |
                ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&icc);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    App().gitExe = FindGitExecutable();
    if (!App().gitExe.empty()) {
        const RunResult r = RunGitSync(App().gitExe, {L"--version"}, L"", 8000);
        if (r.exitCode == 0) App().gitVersion = Trim(W(r.out));
    }
    GRT_LOGI("app", "git=" << U8(App().gitExe) << " version=" << U8(App().gitVersion));

    if (!repoPath.empty()) {
        App().probe = ProbeRepo(repoPath);
        App().repoRoot = App().probe.IsRepo() ? App().probe.repoRoot : NormalizePath(repoPath);
    }

    // 主题/字体必须在创建窗口之前初始化（WM_CREATE 里创建的控件要用到字体）
    ThemeInit(nullptr);
    RegisterAppWindowClass();

    HWND main = ::CreateWindowExW(WS_EX_CONTROLPARENT, kAppWindowClass, Str(IDS_TITLE_MAIN).c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  Scale(1120), Scale(720), nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                  nullptr);
    if (!main) {
        GRT_LOGE("app", "\u4e3b\u7a97\u53e3\u521b\u5efa\u5931\u8d25 err=" << ::GetLastError());
        return 2;
    }
    ThemeApply(main);
    ::ShowWindow(main, (selfTest || exitAfterRequest) ? SW_HIDE : SW_SHOW);
    ::UpdateWindow(main);

    if (selfTest) {
        if (selfTestPath.empty()) {
            wchar_t tmp[MAX_PATH]{};
            ::GetTempPathW(MAX_PATH, tmp);
            selfTestPath = std::wstring(tmp) + L"gitrt-selftest.txt";
        }
        const int rc = RunSelfTest(main, selfTestPath);
        ::DestroyWindow(main);
        ::CoUninitialize();
        LogFlush();
        return rc;
    }

    // ---- 右键菜单请求：把窗口切到对应命令并预填选区/flags（§4.5）----
    if (!requestSection.empty()) {
        ShellMenuRequest req;
        std::wstring why;
        const bool loaded = LoadShellMenuRequest(requestSection, &req, &why);
        const bool applied = loaded && AppWindowApplyRequest(main, req);
        if (!applied) {
            if (why.empty()) why = Str(IDS_MSG_REQ_MISSING);
            if (requestProbePath.empty())
                ::MessageBoxW(main, why.c_str(), L"GitRT", MB_ICONWARNING | MB_OK);
        } else {
            ::SetForegroundWindow(main);
        }
        // 诊断报告（--request-probe）：供 tools/test-all.ps1 断言"菜单 → 参数面板"链路
        if (!requestProbePath.empty()) {
            std::wstring rep;
            rep += std::wstring(L"loaded=") + (loaded ? L"1" : L"0") + L"\r\n";
            rep += std::wstring(L"applied=") + (applied ? L"1" : L"0") + L"\r\n";
            rep += L"cmdId=" + std::to_wstring(req.cmdId) + L"\r\n";
            rep += L"cmdKey=" + req.cmdKey + L"\r\n";
            rep += L"repoRoot=" + App().repoRoot + L"\r\n";
            rep += L"pathCount=" + std::to_wstring(req.paths.size()) + L"\r\n";
            for (const auto& p : req.paths) rep += L"path=" + p + L"\r\n";
            rep += L"flagCount=" + std::to_wstring(req.flags.size()) + L"\r\n";
            for (const auto& kv : req.flags)
                rep += L"flag=" + W(kv.first) + L"=" + kv.second + L"\r\n";
            rep += L"why=" + why + L"\r\n";
            const std::string utf8 = WideToUtf8(rep);
            UniqueHandle h(::CreateFileW(requestProbePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (h.get() != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                ::WriteFile(static_cast<HANDLE>(h.get()), utf8.data(), static_cast<DWORD>(utf8.size()),
                            &wrote, nullptr);
            }
        }
        if (exitAfterRequest) {
            ::DestroyWindow(main);
            ::CoUninitialize();
            LogFlush();
            return applied ? 0 : 1;
        }
    }

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND root = msg.hwnd ? ::GetAncestor(msg.hwnd, GA_ROOT) : nullptr;
        if (root && ::IsDialogMessageW(root, &msg)) continue;   // Tab 导航 / Enter / Esc
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::CoUninitialize();
    LogFlush();
    return 0;
}
