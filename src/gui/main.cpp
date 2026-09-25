// GitRT.exe 入口：命令行解析 / 全局初始化 / 消息循环 / --self-test 自检
#include "gui.h"
#include "config.h"

#include "ai_client.h"
#include "remote.h"
#include "release.h"
#include "restore.h"
#include "tag.h"
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
        AiConfig tcfg;   // 空 systemPrompt = 用内置默认，自检"提示词含全部 key"
        const std::string prompt = EffectiveSystemPrompt(tcfg, [](uint16_t id) { return WideToUtf8(Str(id)); });
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

    // ---- 8b. AI 设置：Key 明文存 exe 同目录（GitRT.ai.json）的往返与优先级 ----------
    {
        const std::wstring path = AiKeyFilePath();
        const bool existed = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        std::string backup;
        if (existed) {
            UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (h.get() != INVALID_HANDLE_VALUE) {
                DWORD size = ::GetFileSize(static_cast<HANDLE>(h.get()), nullptr);
                if (size > 0 && size < (1 << 20)) {
                    backup.resize(size);
                    DWORD got = 0;
                    ::ReadFile(static_cast<HANDLE>(h.get()), backup.data(), size, &got, nullptr);
                    backup.resize(got);
                }
            }
        }
        // 记录 config.json 里 AI 键的原值，测完还原（SaveAiConfig 会同步这几个键）
        auto& store = ConfigStore::Instance();
        store.Reload();
        const std::string e0 = store.GetString("aiEndpoint"), m0 = store.GetString("aiModel");
        const std::string k0 = store.GetString("aiApiKeyEnv");
        const int t0 = store.GetInt("aiTimeoutMs", 60000);
        bool roundTrip = false, keyWins = false, fileExists = false;
        {
            AiConfig cfg = LoadAiConfig();
            cfg.endpoint = L"http://127.0.0.1:1/selftest";
            cfg.model = L"selftest-model";
            cfg.apiKey = L"sk-selftest-plain-key";
            cfg.timeoutMs = 12345;
            cfg.keyFilePath = path;
            const bool saved = SaveAiConfig(cfg);
            fileExists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
            const AiConfig back = LoadAiConfig();
            roundTrip = saved && back.endpoint == cfg.endpoint && back.model == cfg.model &&
                        back.apiKey == cfg.apiKey && back.timeoutMs == 12345 && back.keyFromFile;
            keyWins = ResolveApiKey(back) == L"sk-selftest-plain-key";
        }
        check(roundTrip, L"AI \u8bbe\u7f6e\u5f80\u8fd4\uff1a\u4fdd\u5b58\u5230 " + path + L" \u540e\u80fd\u8bfb\u56de");
        check(fileExists && keyWins,
              L"AI Key \u5c31\u4f4d\uff1a\u6587\u4ef6\u91cc\u7684\u660e\u6587 Key \u4f18\u5148\u4e8e\u73af\u5883\u53d8\u91cf\u751f\u6548");
        // 模型列表地址推导（AI 设置里「获取模型列表」用的 GET 地址）
        const bool urlOk =
            AiModelsUrlFromEndpoint(L"https://api.deepseek.com/chat/completions") ==
                L"https://api.deepseek.com/models" &&
            AiModelsUrlFromEndpoint(L"https://api.deepseek.com/v1/chat/completions") ==
                L"https://api.deepseek.com/v1/models" &&
            AiModelsUrlFromEndpoint(L"http://127.0.0.1:8080/v1/") == L"http://127.0.0.1:8080/v1/models" &&
            AiModelsUrlFromEndpoint(L"https://api.deepseek.com") == L"https://api.deepseek.com/models" &&
            AiModelsUrlFromEndpoint(L"").empty();
        check(urlOk, L"\u6a21\u578b\u5217\u8868\u5730\u5740\u63a8\u5bfc\uff1a/chat/completions \u2192 /models\uff08\u542b /v1\u3001\u5c3e\u90e8\u659c\u6760\u3001\u7a7a\u503c\uff09");

        // 图标一致性：右键菜单用 DLL 的 101，任务栏用 exe 的 101 —— 两张必须是同一张图
        const HICON appIcon = GitRTAppIcon();
        const HICON genericIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
        check(appIcon != nullptr && appIcon != genericIcon,
              L"GitRT.exe \u5185\u5d4c\u5e94\u7528\u56fe\u6807\uff08\u975e\u901a\u7528\u56fe\u6807\uff09");
        if (mainWnd) {
            const HICON wmBig = reinterpret_cast<HICON>(::SendMessageW(mainWnd, WM_GETICON, ICON_BIG, 0));
            const HICON wmSmall = reinterpret_cast<HICON>(::SendMessageW(mainWnd, WM_GETICON, ICON_SMALL, 0));
            check(wmBig == appIcon && wmSmall == appIcon,
                  L"\u4e3b\u7a97\u53e3\uff08\u4efb\u52a1\u680f\uff09\u56fe\u6807 == GitRT \u56fe\u6807\uff08WM_SETICON \u5df2\u751f\u6548\uff09");
        }

        // 还原现场
        if (existed && !backup.empty()) {
            UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
            if (h.get() != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                ::WriteFile(static_cast<HANDLE>(h.get()), backup.data(),
                            static_cast<DWORD>(backup.size()), &wrote, nullptr);
            }
        } else {
            ::DeleteFileW(path.c_str());
        }
        store.Reload();
        store.SetString("aiEndpoint", e0);
        store.SetString("aiModel", m0);
        store.SetString("aiApiKeyEnv", k0);
        store.SetInt("aiTimeoutMs", t0);
        store.Save();
    }

    // ---- 8c. 多行文本换行：Win32 Edit 不认裸 LF --------------------------------
    // git 输出是 LF-only；直接塞进 ES_MULTILINE 会把所有行并成一行
    // （实机 bug：提交历史三条提交挤在一行）。所有多行文本必须过 ToCrlf/SetTextMl。
    {
        const bool crlfOk = ToCrlf(L"a\nb") == L"a\r\nb" &&
                            ToCrlf(L"a\r\nb") == L"a\r\nb" &&   // 幂等
                            ToCrlf(L"a\rb") == L"a\r\nb" &&
                            ToCrlf(L"\n\n") == L"\r\n\r\n" &&
                            ToCrlf(L"") == L"" && ToCrlf(L"x") == L"x";
        check(crlfOk, L"\u6362\u884c\u5f52\u4e00\u5316\uff1aLF \u2192 CRLF\uff08Edit \u63a7\u4ef6\u4e0d\u8ba4\u88f8 LF\uff09");
    }
    // ---- 8d. 克隆选项（深度/单分支/分支/无标签/部分克隆）--------------------------
    {
        auto joinArgv = [](const std::vector<std::wstring>& v) {
            std::wstring s;
            for (const auto& x : v) {
                if (!s.empty()) s += L' ';
                s += x;
            }
            return s;
        };
        const CommandSpec* clone = FindCommandByKey("repo.clone");
        check(clone != nullptr && clone->flagCount == 6,
              L"\u514b\u9686\u9009\u9879\uff1a\u547d\u4ee4\u8868\u91cc\u6709 6 \u4e2a\u9009\u9879\uff08\u542b\u6df1\u5ea6\uff09");
        BuildInput in;
        in.spec = clone;
        in.cwd = L"C:\\selftest";
        in.params["url"] = L"https://example.invalid/x.git";
        in.flags["depth"] = L"1";
        in.flags["single-branch"] = L"1";
        in.flags["branch"] = L"main";
        in.flags["no-tags"] = L"1";
        in.flags["filter"] = L"blob:none";
        BuiltCommand b;
        BuildError e;
        const bool built = clone && BuildCommand(in, &b, &e);
        const std::wstring line = built ? joinArgv(b.argvList.front()) : std::wstring();
        check(built && line.find(L"--depth=1") != std::wstring::npos &&
                  line.find(L"--single-branch") != std::wstring::npos &&
                  line.find(L"--branch=main") != std::wstring::npos &&
                  line.find(L"--no-tags") != std::wstring::npos &&
                  line.find(L"--filter=blob:none") != std::wstring::npos,
              L"\u514b\u9686\u9009\u9879\uff1a\u6df1\u5ea6\u7b49\u90fd\u8fdb\u4e86 argv\uff08" + line + L"\uff09");
        // 深度必须是正整数：拦在构造期，别让用户看 git 的报错
        BuildInput bad = in;
        bad.flags["depth"] = L"abc";
        BuiltCommand b2;
        BuildError e2;
        check(!BuildCommand(bad, &b2, &e2) && e2.message.find(L"\u6b63\u6574\u6570") != std::wstring::npos,
              L"\u514b\u9686\u9009\u9879\uff1a\u6df1\u5ea6\u975e\u6b63\u6574\u6570\u88ab\u62e6\u4e0b\uff08" + e2.message + L"\uff09");
    }
    // ---- 8e. AI 直出命令的只读白名单（"方案直接输出命令"的安全边界）----
    {
        std::vector<std::wstring> a;
        std::wstring why;
        auto ro = [&](const wchar_t* line) {
            a.clear();
            why.clear();
            return ParseReadOnlyGitCommand(line, &a, &why);
        };
        check(ro(L"git status -sb") && a.size() == 2 && a[0] == L"status",
              L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit status -sb \u653e\u884c\uff08\u62c6\u6210 argv\uff09");
        check(ro(L"git log --oneline -5"), L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit log \u653e\u884c");
        check(ro(L"git branch -a"), L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit branch -a \u653e\u884c");
        check(!ro(L"git branch newbranch"),
              L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit branch newbranch \u62d2\u7edd\uff08\u5efa\u5206\u652f\u662f\u5199\u64cd\u4f5c\uff09");
        check(ro(L"git tag -l") && !ro(L"git tag v1.0"),
              L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit tag -l \u653e\u884c\u3001git tag v1.0 \u62d2\u7edd");
        check(!ro(L"git push origin main"), L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit push \u62d2\u7edd");
        check(!ro(L"git reset --hard HEAD~1"), L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit reset --hard \u62d2\u7edd");
        check(ro(L"git remote -v") && !ro(L"git remote add o u"),
              L"AI \u53ea\u8bfb\u547d\u4ee4\uff1agit remote -v \u653e\u884c\u3001remote add \u62d2\u7edd");
        check(!ro(L"rm -rf /"), L"AI \u53ea\u8bfb\u547d\u4ee4\uff1a\u975e git \u547d\u4ee4\u62d2\u7edd");
        check(ro(L"git config --get user.name") && !ro(L"git config user.name x"),
              L"AI \u53ea\u8bfb\u547d\u4ee4\uff1aconfig --get \u653e\u884c\u3001config \u5199\u5165\u62d2\u7edd");
    }
    // ---- 9. 执行 → 状态刷新（★ 用户可见行为）----------------------------------
    // 点"执行"后必须看到两件事：① 真实命令行出现在执行窗口里（UI 行为，见
    // progress_window.cpp）；② 命令改了工作区后状态跟着变。这里用临时仓库验证 ②：
    // 真的执行 git add，再走 RefreshRepoStatus，看计数是否随之变化。
    {
        // 夹具放在**模块目录**下（不写用户目录）：既是工作区内可写位置，也不污染仓库
        const std::wstring repo = GetModuleDir() + L"\\status-selftest-repo";
        const std::wstring git = FindGitExecutable();
        wchar_t comspec[MAX_PATH]{};
        if (!::GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH))
            ::wcscpy_s(comspec, L"C:\\Windows\\System32\\cmd.exe");
        auto rmdirRepo = [&] {
            RunGitSync(comspec, {L"/c", L"rmdir", L"/s", L"/q", repo}, GetModuleDir(), 20000);
        };
        if (git.empty()) {
            check(true, L"\u72b6\u6001\u5237\u65b0\uff1a\u672a\u627e\u5230 git.exe\uff0c\u8df3\u8fc7");
        } else {
            rmdirRepo();   // 从干净状态开始（保证可重复运行）
            ::CreateDirectoryW(repo.c_str(), nullptr);
            RunGitSync(git, {L"init", L"-q"}, repo, 30000);
            {
                const std::wstring f = repo + L"\\new.txt";
                HANDLE h = ::CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            }
            const std::wstring savedRepo = App().repoRoot;
            const std::wstring savedGit = App().gitExe;
            App().repoRoot = repo;
            App().gitExe = git;
            RefreshRepoStatus(nullptr);
            const int untrackedBefore = App().status.untracked;
            const int stagedBefore = App().status.staged;
            // 与参数面板"执行"（进度窗口里的 WorkerProc）走同一条 git 通道
            RunGitSync(git, {L"add", L"."}, repo, 30000);
            const bool readAgain = RefreshRepoStatus(nullptr);
            check(readAgain && untrackedBefore == 1 && stagedBefore == 0 &&
                      App().status.staged == 1 && App().status.untracked == 0,
                  L"\u6267\u884c\u540e\u72b6\u6001\u5237\u65b0\uff1a\u6682\u5b58/\u672a\u8ddf\u8e2a\u8ba1\u6570"
                  L"\u8ddf\u7740\u547d\u4ee4\u53d8\uff08untracked " + std::to_wstring(untrackedBefore) +
                      L"\u2192" + std::to_wstring(App().status.untracked) + L"\uff0cstaged " +
                      std::to_wstring(stagedBefore) + L"\u2192" + std::to_wstring(App().status.staged) + L"\uff09");
            // 远端/还原用例需要 HEAD 可解析：给自检仓库补一条提交
            RunGitSync(git, {L"-c", L"user.email=t@e.invalid", L"-c", L"user.name=GitRT", L"commit",
                            L"-qm", L"selftest"}, repo, 30000);
    // ---- 8f. 远端跟踪 / 按提交还原（core 判定）----
    {
        // 自检用的临时仓库没有远端 → 上游读出来应当是"没有"，不是报错
        const RemoteInfo ri = LoadRemoteInfo(App().gitExe, App().repoRoot);
        check(ri.ok && !ri.hasUpstream && ri.ahead == 0 && ri.behind == 0,
              L"\u8fdc\u7aef\u4fe1\u606f\uff1a\u65e0\u4e0a\u6e38\u65f6 ok=1 \u4e14 hasUpstream=0");
        const RemoteBaseline rb = LoadRemoteBaseline(App().gitExe, App().repoRoot, 20);
        check(rb.info.hasUpstream == ri.hasUpstream, L"\u8fdc\u7aef\u57fa\u7ebf\uff1a\u4e0e\u4e0a\u6e38\u4fe1\u606f\u4e00\u81f4");
        check(DescribeRemoteBaseline(rb).find(L"\u672a\u8bbe\u7f6e\u4e0a\u6e38") != std::wstring::npos,
              L"\u8fdc\u7aef\u57fa\u7ebf\uff1a\u65e0\u4e0a\u6e38\u65f6\u5982\u5b9e\u8bf4\u660e\uff08\u4e0d\u662f\u62a5\u9519\uff09");

        // 远端地址/名字校验：既要拦住危险输入，也不能误杀合法写法
        check(ValidateRemoteUrl(L"https://example.com/a.git").empty() &&
                  ValidateRemoteUrl(L"git@github.com:u/r.git").empty() &&
                  ValidateRemoteUrl(L"C:\\my repos\\x.git").empty(),
              L"\u8fdc\u7aef\u5730\u5740\u6821\u9a8c\uff1ahttps/ssh/\u5e26\u7a7a\u683c\u672c\u5730\u8def\u5f84\u90fd\u653e\u884c");
        check(!ValidateRemoteUrl(L"-bad").empty() && !ValidateRemoteUrl(L"").empty() &&
                  !ValidateRemoteName(L"bad name").empty() && ValidateRemoteName(L"origin2").empty(),
              L"\u8fdc\u7aef\u6821\u9a8c\uff1a\u9009\u9879\u6ce8\u5165\u4e0e\u975e\u6cd5\u540d\u5b57\u88ab\u62e6");
        const std::vector<RemoteEntry> remotes = LoadRemotes(App().gitExe, App().repoRoot);
        check(remotes.empty(), L"\u8fdc\u7aef\u5217\u8868\uff1a\u6ca1\u914d\u7f6e\u8fdc\u7aef\u65f6\u4e3a\u7a7a");

        // 还原方式解析
        RestoreMode m = RestoreMode::DetachCheckout;
        check(ParseRestoreMode(L"hard", &m) && m == RestoreMode::ResetHard &&
                  ParseRestoreMode(L"MIXED", &m) && m == RestoreMode::ResetMixed &&
                  !ParseRestoreMode(L"nope", &m),
              L"\u8fd8\u539f\u65b9\u5f0f\uff1ahard/mixed \u80fd\u89e3\u6790\uff0c\u672a\u77e5\u503c\u62d2\u7edd");
        check(RestoreModeKey(RestoreMode::DetachCheckout) == L"detach" &&
                  RestoreModeKey(RestoreMode::NewBranch) == L"branch" &&
                  !RestoreModeLabel(RestoreMode::ResetHard).empty(),
              L"\u8fd8\u539f\u65b9\u5f0f\uff1akey \u4e0e\u4e2d\u6587\u6807\u7b7e\u90fd\u6b63\u5e38");

        // 还原计划：非法哈希 / 未知提交必须被拒；合法哈希要给出 checkout --detach
        const RestorePlan bad1 = BuildRestorePlan(App().gitExe, App().repoRoot, L"zzz",
                                                  RestoreMode::DetachCheckout);
        check(!bad1.ok && bad1.error.find(L"\u4e0d\u5408\u6cd5") != std::wstring::npos,
              L"\u8fd8\u539f\u8ba1\u5212\uff1a\u975e\u6cd5\u54c8\u5e0c\u88ab\u62d2");
        const RestorePlan bad2 = BuildRestorePlan(App().gitExe, App().repoRoot,
                                                  L"0123456789abcdef0123456789abcdef01234567",
                                                  RestoreMode::DetachCheckout);
        check(!bad2.ok, L"\u8fd8\u539f\u8ba1\u5212\uff1a\u4e0d\u5b58\u5728\u7684\u63d0\u4ea4\u88ab\u62d2");
        const std::wstring headHash =
            Trim(W(RunGitSync(git, {L"rev-parse", L"HEAD"}, repo, 30000).out));
        const RestorePlan okPlan = BuildRestorePlan(App().gitExe, App().repoRoot, headHash,
                                                    RestoreMode::DetachCheckout);
        check(okPlan.ok && !okPlan.commandLines.empty() &&
                  okPlan.commandLines[0].find(L"checkout --detach") != std::wstring::npos,
              L"\u8fd8\u539f\u8ba1\u5212\uff1aHEAD \u4e0a\u7684\u53ea\u8bfb\u68c0\u51fa\u547d\u4ee4\u6b63\u786e");
        const RestorePlan brPlan = BuildRestorePlan(App().gitExe, App().repoRoot, headHash,
                                                    RestoreMode::NewBranch, L"");
        check(!brPlan.ok, L"\u8fd8\u539f\u8ba1\u5212\uff1a\u65b0\u5efa\u5206\u652f\u6ca1\u7ed9\u540d\u5b57\u88ab\u62d2");
    }
    // ---- 8g. 标签 / 发布（core 判定）----
    {
        const std::wstring git = App().gitExe;
        const std::wstring repo = App().repoRoot;
        // 轻量标签：git tag <name> <hash>
        const TagPlan light = BuildTagPlan(git, repo, L"selftest-lw", L"", false, L"HEAD", false);
        check(light.ok && !light.annotated && light.commandLines.size() == 1 &&
                  light.commandLines[0].find(L"git tag selftest-lw ") == 0 &&
                  light.commandLines[0].find(L"-a") == std::wstring::npos,
              L"\u6807\u7b7e\u8ba1\u5212\uff1a\u8f7b\u91cf\u6807\u7b7e\u7684\u547d\u4ee4\u5c31\u662f git tag <name> <hash>");
        // 附注标签：git tag -a <name> -m <msg> <hash>
        const TagPlan annot = BuildTagPlan(git, repo, L"selftest-an", L"selftest message", true, L"HEAD", false);
        check(annot.ok && annot.annotated && annot.commandLines.size() == 1 &&
                  annot.commandLines[0].find(L"git tag -a selftest-an -m ") == 0,
              L"\u6807\u7b7e\u8ba1\u5212\uff1a\u9644\u6ce8\u6807\u7b7e\u5e26 -a -m");
        check(annot.ok && annot.commandLines[0].find(L"selftest message") != std::wstring::npos,
              L"标签计划：附注标签的信息真的进了命令行（-m 后面就是它）");
        // 非法标签名 / 未知提交必须被拒
        const TagPlan badName = BuildTagPlan(git, repo, L"bad name", L"", false, L"HEAD", false);
        check(!badName.ok && badName.error.find(L"\u4e0d\u5408\u6cd5") != std::wstring::npos,
              L"\u6807\u7b7e\u8ba1\u5212\uff1a\u975e\u6cd5\u6807\u7b7e\u540d\u88ab\u62d2\uff08" + badName.error + L"\uff09");
        // 阶段二补强：非法写法（含 ..）与空名都必须被拒，且理由必须是中文
        const TagPlan badDots = BuildTagPlan(git, repo, L"v1..2", L"", false, L"HEAD", false);
        check(!badDots.ok && badDots.error.find(L"不合法") != std::wstring::npos,
              L"标签计划：含 .. 的标签名被拒（" + badDots.error + L"）");
        const TagPlan noName = BuildTagPlan(git, repo, L"", L"", false, L"HEAD", false);
        check(!noName.ok && noName.error.find(L"请填写标签名") != std::wstring::npos,
              L"标签计划：空标签名被拒且原因是中文（" + noName.error + L"）");
        const TagPlan badRev = BuildTagPlan(git, repo, L"selftest-x", L"", false, L"no-such-rev-xyz", false);
        check(!badRev.ok, L"\u6807\u7b7e\u8ba1\u5212\uff1a\u4e0d\u5b58\u5728\u7684\u76ee\u6807\u63d0\u4ea4\u88ab\u62d2");
        // 真的执行一次：打完能在清单里看到（附注/轻量都要对）
        if (light.ok) {
            const TagResult tr = ApplyTagPlan(git, repo, light, {}, {});
            check(tr.ok, L"\u6807\u7b7e\u6267\u884c\uff1a\u8f7b\u91cf\u6807\u7b7e\u771f\u7684\u6253\u4e0a\u4e86");
        }
        if (annot.ok) {
            const TagResult tr = ApplyTagPlan(git, repo, annot, {}, {});
            check(tr.ok, L"\u6807\u7b7e\u6267\u884c\uff1a\u9644\u6ce8\u6807\u7b7e\u771f\u7684\u6253\u4e0a\u4e86");
        }
        std::vector<TagInfo> tags;
        std::wstring terr;
        const bool loaded = LoadTags(git, repo, &tags, &terr, L"origin");
        bool sawLight = false, sawAnnot = false;
        for (const auto& t : tags) {
            if (t.name == L"selftest-lw") sawLight = !t.annotated;
            if (t.name == L"selftest-an") sawAnnot = t.annotated;
            check(!t.onRemote, L"\u6807\u7b7e\u6e05\u5355\uff1a\u6ca1\u6709\u8fdc\u7aef\u65f6 onRemote \u5168\u4e3a false");
            if (t.onRemote) break;   // 只报一次就够，别把自检报告刷满
        }
        check(loaded && sawLight && sawAnnot,
              L"\u6807\u7b7e\u6e05\u5355\uff1a\u80fd\u8bfb\u5230\u521a\u6253\u7684\u8f7b\u91cf\u4e0e\u9644\u6ce8\u6807\u7b7e");
        // 附注标签的 hash 必须是**提交**（不是 tag 对象）：与 git rev-parse <tag>^{commit} 一致
        std::wstring annotHash;
        for (const auto& t : tags) {
            if (t.name == L"selftest-an") annotHash = t.hash;
        }
        const std::wstring realHash =
            Trim(W(RunGitSync(git, {L"rev-parse", L"selftest-an^{commit}"}, repo, 30000).out));
        check(!annotHash.empty() && !realHash.empty() && annotHash == realHash,
              L"标签清单：附注标签的 hash 已剥到提交（= git rev-parse <tag>^{commit}）");
        check(sawAnnot, L"标签清单：附注标签 annotated==true（界面「类型」列显示附注）");
        check(DescribeTags(tags).find(L"\u9644\u6ce8") != std::wstring::npos,
              L"\u6807\u7b7e\u6e05\u5355\uff1a\u9762\u677f\u6587\u672c\u6807\u51fa\u4e86\u300c\u9644\u6ce8\u300d");
        // 重复标签：没 force 拒绝、有 force 放行并带 -f
        const TagPlan dup = BuildTagPlan(git, repo, L"selftest-lw", L"", false, L"HEAD", false);
        check(!dup.ok && dup.forceNeeded && dup.error.find(L"force") != std::wstring::npos,
              L"\u6807\u7b7e\u8ba1\u5212\uff1a\u91cd\u590d\u6807\u7b7e\u6ca1 force \u88ab\u62d2");
        const TagPlan forced = BuildTagPlan(git, repo, L"selftest-lw", L"", false, L"HEAD", true);
        check(forced.ok && !forced.commandLines.empty() &&
                  forced.commandLines[0].find(L"git tag -f selftest-lw") == 0,
              L"\u6807\u7b7e\u8ba1\u5212\uff1a\u52a0\u4e86 force \u5c31\u5e26 -f");
        check(forced.ok && !forced.warnings.empty(),
              L"标签计划：force=true 会给出「这个标签会被移动」的告警");
        // 推送 / 删除（删除是破坏性：destructive=true 且两条命令）
        // ★ 自检仓库默认没有远端，而"推到哪个远端"必须校验；这里临时加一个**本地路径**远端，
        //   只为满足校验（自检不做任何网络操作；仓库随后整个删掉）。
        const std::wstring fakeRemote = GetModuleDir() + L"\\selftest-tag-fake-origin.git";
        RunGitSync(git, {L"remote", L"add", L"origin", fakeRemote}, repo, 30000);
        const TagPlan pushOne = BuildPushTagPlan(git, repo, L"selftest-lw", false, L"");
        check(pushOne.ok && !pushOne.commandLines.empty() &&
                  pushOne.commandLines[0] == L"git push origin selftest-lw",
              L"\u6807\u7b7e\u8ba1\u5212\uff1a\u63a8\u9001\u5355\u4e2a\u6807\u7b7e\u7684\u547d\u4ee4\u5bf9");
        const TagPlan pushAll = BuildPushTagPlan(git, repo, L"", true, L"");
        check(pushAll.ok && !pushAll.warnings.empty() &&
                  pushAll.commandLines[0] == L"git push origin --tags",
              L"\u6807\u7b7e\u8ba1\u5212\uff1a--all \u63a8\u5168\u90e8\u4e14\u6709\u544a\u8b66");
        const TagPlan del = BuildDeleteTagPlan(git, repo, L"selftest-lw", true, L"");
        check(del.ok && del.destructive && del.commandLines.size() == 2 &&
                  del.commandLines[0] == L"git tag -d selftest-lw" &&
                  del.commandLines[1] == L"git push origin :refs/tags/selftest-lw",
              L"\u6807\u7b7e\u8ba1\u5212\uff1a\u5220\u9664\u662f\u7834\u574f\u6027\u4e14\u672c\u5730+\u8fdc\u7aef\u4e24\u6761\u547d\u4ee4");
        const TagPlan delMissing = BuildDeleteTagPlan(git, repo, L"no-such-tag-xyz", false, L"");
        check(!delMissing.ok, L"\u6807\u7b7e\u8ba1\u5212\uff1a\u5220\u4e0d\u5b58\u5728\u7684\u6807\u7b7e\u88ab\u62d2");

        // 发布：走 gh（本机通常没装）——没装就必须拒绝，且提示怎么装
        const GhInfo gh = DetectGh();
        std::vector<ReleaseEntry> rels;
        std::wstring rerr;
        const bool relOk = LoadReleases(git, repo, &rels, &rerr);
        check(relOk || !rerr.empty(),
              L"\u53d1\u5e03\u6e05\u5355\uff1a\u4e0d\u53ef\u7528\u65f6\u8fd4\u56de\u7a7a\u5217\u8868 + \u4e2d\u6587\u539f\u56e0\uff08\u4e0d\u5d29\uff09");
        const ReleasePlan rel = BuildReleasePlan(git, repo, L"selftest-lw", L"", L"", false, false,
                                                 false, {}, false);
        if (!gh.available) {
            check(!rel.ok && (rel.error.find(L"gh") != std::wstring::npos ||
                              rel.error.find(L"GitHub CLI") != std::wstring::npos),
                  L"\u53d1\u5e03\u8ba1\u5212\uff1a\u6ca1\u88c5 gh \u65f6\u62d2\u7edd\u5e76\u8bf4\u660e\u600e\u4e48\u88c5\uff08" +
                      rel.error + L"\uff09");
        } else {
            check(rel.ok, L"\u53d1\u5e03\u8ba1\u5212\uff1agh \u53ef\u7528\u65f6\u80fd\u6784\u9020\uff08" + rel.error + L"\uff09");
        }
        // 标签不存在 → 无论有没有 gh 都必须拒绝（这里用当前仓库必然不存在的名字）
        const ReleasePlan relBad = BuildReleasePlan(git, repo, L"no-such-tag-xyz", L"", L"", false,
                                                    false, false, {}, false);
        check(!relBad.ok && !relBad.error.empty(),
              L"\u53d1\u5e03\u8ba1\u5212\uff1a\u6807\u7b7e\u4e0d\u5b58\u5728\u65f6\u62d2\u7edd\u5e76\u7ed9\u4e2d\u6587\u539f\u56e0\uff08" +
                  relBad.error + L"\uff09");
    }
            App().repoRoot = savedRepo;
            App().gitExe = savedGit;
            rmdirRepo();
        }
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
        } else if (a == L"--squash") {
            cli.squashHashes = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--message") {
            cli.squashMessage = nextArg(&i);
            cli.tagMessage = cli.squashMessage;   // 附注标签的 -m 复用同一个开关
        } else if (a == L"--remote-info") {
            cli.remoteInfoMode = L"info";
            cli.hasWork = true;
        } else if (a == L"--remote-fetch") {
            cli.remoteInfoMode = L"fetch";   // --remote-info --remote-fetch = 先抓取再报
            cli.hasWork = true;
        } else if (a == L"--set-upstream") {
            cli.setUpstream = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--remote-add") {
            cli.remoteAdd = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--remote-set-url") {
            cli.remoteSetUrl = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--remote-remove") {
            cli.remoteRemove = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--restore") {
            cli.restoreHash = nextArg(&i);
            cli.hasWork = true;
        } else if (a == L"--mode") {
            cli.restoreMode = nextArg(&i);
        } else if (a == L"--branch") {
            cli.restoreBranch = nextArg(&i);
        } else if (a == L"--force") {
            cli.forceRestore = true;
            cli.tagForce = true;   // 同一个开关：还原用 --force，标签覆盖/删除也用 --force
        } else if (a == L"--tag-list") {
            cli.tagList = true;
            cli.workKind = CliOptions::CliWork::TagList;
            cli.hasWork = true;
        } else if (a == L"--tag-create") {
            cli.tagCreateName = nextArg(&i);
            cli.workKind = CliOptions::CliWork::TagCreate;
            cli.hasWork = true;
        } else if (a == L"--annotated") {
            cli.tagAnnotated = true;
        } else if (a == L"--target") {
            cli.tagTarget = nextArg(&i);
        } else if (a == L"--tag-push") {
            cli.tagPushName = nextArg(&i);
            cli.workKind = CliOptions::CliWork::TagPush;
            cli.hasWork = true;
        } else if (a == L"--all") {
            cli.tagPushAll = true;
        } else if (a == L"--tag-delete") {
            cli.tagDeleteName = nextArg(&i);
            cli.workKind = CliOptions::CliWork::TagDelete;
            cli.hasWork = true;
        } else if (a == L"--remote") {
            // 两种用法：--tag-delete <name> --remote（远端也删，用默认远端）
            //            --tag-push/--tag-delete … --remote <r>（指定远端名）
            const std::wstring v = (i + 1 < argc) ? std::wstring(argv[i + 1]) : std::wstring();
            if (!v.empty() && v[0] != L'-') {
                cli.tagRemoteName = nextArg(&i);
            } else {
                cli.tagDeleteRemote = true;
            }
        } else if (a == L"--release-list") {
            cli.releaseList = true;
            cli.workKind = CliOptions::CliWork::ReleaseList;
            cli.hasWork = true;
        } else if (a == L"--release-create") {
            cli.releaseTag = nextArg(&i);
            cli.workKind = CliOptions::CliWork::ReleaseCreate;
            cli.hasWork = true;
        } else if (a == L"--title") {
            cli.releaseTitle = nextArg(&i);
        } else if (a == L"--notes") {
            cli.releaseNotes = nextArg(&i);
        } else if (a == L"--generate-notes") {
            cli.releaseGenerateNotes = true;
        } else if (a == L"--draft") {
            cli.releaseDraft = true;
        } else if (a == L"--prerelease") {
            cli.releasePrerelease = true;
        } else if (a == L"--push-tag") {
            cli.releasePushTag = true;
        } else if (a == L"--asset") {
            const std::wstring v = nextArg(&i);
            if (!v.empty()) cli.releaseAssets.push_back(v);
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
        else if (!cli.squashHashes.empty()) rc = RunCliSquash(cli);
        else if (!cli.remoteInfoMode.empty() || !cli.setUpstream.empty() || !cli.remoteAdd.empty() ||
                 !cli.remoteSetUrl.empty() || !cli.remoteRemove.empty()) rc = RunCliRemote(cli);
        else if (!cli.restoreHash.empty()) rc = RunCliRestore(cli);
        else if (cli.workKind == CliOptions::CliWork::TagList ||
                 cli.workKind == CliOptions::CliWork::TagCreate ||
                 cli.workKind == CliOptions::CliWork::TagPush ||
                 cli.workKind == CliOptions::CliWork::TagDelete) rc = RunCliTag(cli);
        else if (cli.workKind == CliOptions::CliWork::ReleaseList ||
                 cli.workKind == CliOptions::CliWork::ReleaseCreate) rc = RunCliRelease(cli);
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
    // 任务栏/Alt-Tab 的图标：显式设到窗口上（与右键菜单同一张 gitrt.ico）
    ::SendMessageW(main, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(GitRTAppIcon()));
    ::SendMessageW(main, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(GitRTAppIcon()));
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
