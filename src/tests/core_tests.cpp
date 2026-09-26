// ---------------------------------------------------------------------------
// GitRT 核心单元测试：**无窗口、无桌面会话、不依赖真 git 仓库**
//
// 为什么有这个目标：这些断言原先全锁在 GitRT.exe 的 `--self-test` 里，而
// `RunSelfTest(HWND mainWnd, …)` 需要一个真实窗口句柄（还会真的 CreateParamPanel），
// 结果是"想验证 IsValidUrl 的一个边界，也得先启动一个 GUI 程序、等 2 秒、读一份 txt"。
// 搬到这里的都是**纯函数 / 纯构造**的断言：控制台程序、秒级、任何 CI 或容器都能跑，
// 失败直接打在 stdout 上。
//
// 边界（刻意留在 GUI 自检里的）：参数面板能否构建、图标、执行后状态刷新、
// 以及远端/还原/标签那些需要真 git 仓库的用例 —— 它们确实需要窗口或进程。
//
// 覆盖：porcelain v2 固件 / 命令表 dry-run / flags 白名单与参数校验 / JSON 工具 /
//       AI 响应解析与提示词 / AI 计划的安全闸门 / AI 设置往返 / 克隆选项 /
//       AI 直出命令的只读白名单
// ---------------------------------------------------------------------------
#include "check.h"

#include "ai_client.h"
#include "command_builder.h"
#include "command_spec.h"
#include "config.h"
#include "core.h"
#include "json_util.h"
#include "status.h"

using namespace grt;

namespace {

// porcelain v2 的固件要按 NUL 拼接（真实 git 输出就是 NUL 分隔）
std::string NulJoin(const std::vector<std::string>& toks) {
    std::string s;
    for (const auto& t : toks) {
        s += t;
        s.push_back('\0');
    }
    return s;
}

}  // namespace

int main() {
    grt::test::Harness t;

    // ---- 1. porcelain v2 固件（真实 git 2.53 的字节） ----
    {
        const std::string fx = NulJoin({"# branch.oid 6c477e5c19685cf13fca7378eeae12055cff82c8",
                                        "# branch.head main", "# stash 1", "? o.bin", "? renamed.txt"});
        const RepoStatus s = ParsePorcelainV2(fx);
        t.Check(s.parsed && s.head == "main" && s.stashCount == 1 && s.entries.size() == 2 &&
                    s.untracked == 2,
                L"porcelain: header 以 NUL 结尾、缺 branch.ab 仍可解析");
    }
    {
        const std::string fx = NulJoin(
            {"2 R. N... 100644 100644 100644 c1b0730e0133447badcfd47fd144e254807b06e1 "
             "c1b0730e0133447badcfd47fd144e254807b06e1 R100 b.txt",
             "a.txt", "? r.bin"});
        const RepoStatus s = ParsePorcelainV2(fx);
        t.Check(s.entries.size() == 2 && s.entries[0].type == '2' && s.entries[0].path == "b.txt" &&
                    s.entries[0].origPath == "a.txt" && s.staged == 1,
                L"porcelain: 重命名条目双 NUL token（新路径在前）");
    }
    {
        const std::string fx = NulJoin({"1 M. N... 100644 100644 100644 aaa bbb file with spaces.txt"});
        const RepoStatus s = ParsePorcelainV2(fx);
        t.Check(s.entries.size() == 1 && s.entries[0].path == "file with spaces.txt" && s.staged == 1,
                L"porcelain: 含空格路径按字段数切分正确");
    }
    {
        const std::string fx = NulJoin({"# branch.head (detached)"});
        const RepoStatus s = ParsePorcelainV2(fx);
        t.Check(s.detached, L"porcelain: 分离头指针识别");
    }

    // ---- 2. 命令表 dry-run（每条非 Internal 命令都要能生成 argv） ----
    // 注：BuildCommand 不启动 git（只构造 argv），所以这里给一个占位 exe 名即可。
    int built = 0, internal = 0, bad = 0;
    for (size_t ci = 0; ci < CommandTableSize(); ++ci) {
        const CommandSpec& c = CommandTable()[ci];
        if (c.exec == ExecKind::Internal) {
            ++internal;
            continue;
        }
        BuildInput in;
        in.spec = &c;
        in.gitExe = L"git.exe";
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
            std::printf("       构造失败 %s: %s\n", c.key, WideToUtf8(err.message).c_str());
            continue;
        }
        if (out.argvList.empty()) {
            ++bad;
            std::printf("       空命令 %s\n", c.key);
            continue;
        }
        ++built;
    }
    t.Check(bad == 0, L"dry-run: " + std::to_wstring(built) + L" 条生成 argv，" +
                          std::to_wstring(internal) + L" 条 Internal，失败 " + std::to_wstring(bad));

    // ---- 3. flags 白名单与参数校验 ----
    {
        const CommandSpec* push = FindCommandByKey("sync.push");
        bool ok = false;
        if (push) {
            BuildInput in;
            in.spec = push;
            in.gitExe = L"git.exe";
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
        t.Check(ok, L"flags 白名单：已登记项进入 argv，未登记项被忽略");
    }
    t.Check(!IsValidBranchName(L"-evil") && !IsValidBranchName(L"a..b") && IsValidBranchName(L"feature/x"),
            L"分支名校验");
    t.Check(!IsValidUrl(L"--upload-pack=evil") && IsValidUrl(L"https://example.com/a.git") &&
                IsValidUrl(L"git@github.com:u/r.git"),
            L"URL 校验（拒绝以 - 开头的注入）");
    t.Check(!IsValidRevision(L"HEAD;rm -rf /") && IsValidRevision(L"HEAD~1"), L"修订表达式校验");
    {
        // 互斥组：显式选 hard 之后，不得再套用默认的 mixed
        const CommandSpec* rst = FindCommandByKey("adv.reset");
        bool ok = false;
        if (rst) {
            BuildInput in;
            in.spec = rst;
            in.gitExe = L"git.exe";
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
        t.Check(ok, L"互斥选项组：显式选择后不再套用默认项（不会出现 --mixed --hard）");
    }

    // ---- 4. JSON 工具 ----
    {
        const std::string raw = std::string("a\"b\\c\nd\te\rf\x01g \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80");
        t.Check(JsonUnescape(JsonEscape(raw)) == raw,
                L"JSON 转义/反转义往返（引号/反斜杠/控制符/中文/emoji）");
        t.Check(JsonUnescape("\\ud83d\\ude00") == "\xf0\x9f\x98\x80",
                L"JSON \\u 代理对解码为 UTF-8");
    }

    // ---- 5. AI 响应解析与系统提示词 ----
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
        t.Check(got && content.find("sync.push") != std::string::npos &&
                    content.find("should not match") == std::string::npos,
                L"从 OpenAI 兼容响应取出 message.content（不误命中 reasoning_content）");
        AiPlan p = ParsePlanReply(content);
        t.Check(p.ok && !p.noCommand && p.commandKey == "sync.push" &&
                    p.flags["set-upstream"] == "1" && p.explanation == L"推送当前分支",
                L"解析计划（command/flags/explanation，含 JSON \\u 转义）");
    }
    {
        const AiPlan p = ParsePlanReply("```json\n{\"command\":\"none\",\"explanation\":\"nope\"}\n```");
        t.Check(p.ok && p.noCommand, L"解析计划：markdown 围栏 + none 分支");
    }
    {
        const AiPlan p = ParsePlanReply("you should just run git push");
        t.Check(!p.ok && !p.error.empty(), L"非 JSON 回复被拒绝");
    }
    {
        AiConfig tcfg;   // 空 systemPrompt = 用内置默认，检查"提示词含全部 key"
        // 标题解析器在 GUI 里读资源串；这里只关心"命令表 key 是否都在提示词里"，
        // 所以给一个占位实现（TitleResolver = std::function<std::string(uint16_t)>）。
        const std::string prompt = EffectiveSystemPrompt(tcfg, [](uint16_t) { return std::string("—"); });
        bool allKeys = true;
        for (size_t i = 0; i < CommandTableSize(); ++i)
            if (prompt.find(CommandTable()[i].key) == std::string::npos) {
                allKeys = false;
                break;
            }
        t.Check(allKeys, L"系统提示词由命令表自动生成，包含全部 key");
    }

    // ---- 6. AI 计划 → 命令（安全闸门） ----
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "definitely.not.a.command";
        const auto r = PlanToCommand(p, L"C:\\selftest\\repo", {}, L"git.exe");
        t.Check(!r.ok && !r.error.empty(), L"闸门：拒绝命令表中不存在的命令");
    }
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "sync.push";
        p.flags["bogus"] = "1";
        p.flags["set-upstream"] = "1";
        const auto r = PlanToCommand(p, L"C:\\selftest\\repo", {}, L"git.exe");
        bool hasWarn = false;
        for (const auto& w : r.warnings)
            if (w.find(L"bogus") != std::wstring::npos) hasWarn = true;
        t.Check(r.ok && hasWarn && r.built.display.find(L"--set-upstream") != std::wstring::npos,
                L"闸门：未知选项被忽略并提示，白名单选项进入 argv");
    }
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "app.ai";
        const auto r = PlanToCommand(p, L"C:\\selftest\\repo", {}, L"git.exe");
        t.Check(!r.ok, L"闸门：Internal 命令不允许由 AI 执行");
    }
    {
        AiPlan p;
        p.ok = true;
        p.commandKey = "sync.pull";
        const auto r = PlanToCommand(p, L"", {}, L"git.exe");
        t.Check(!r.ok, L"闸门：需要仓库的命令在未选仓库时被拒绝");
    }

    // ---- 7. AI 设置：Key 明文落盘（exe 同目录 GitRT.ai.json）的往返与优先级 ----
    //   注：AiKeyFilePath() 按**当前进程的 exe 目录**解析，所以这里落在测试程序旁边
    //   （build/<cfg>/src/tests/），不会碰用户已安装的 GitRT.ai.json —— 比放在
    //   GUI 自检里更安全（从安装目录跑 --self-test 时曾经会动到用户那份）。
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
        t.Check(roundTrip, L"AI 设置往返：保存到 " + path + L" 后能读回");
        t.Check(fileExists && keyWins,
                L"AI Key 就位：文件里的明文 Key 优先于环境变量生效");

        // 模型列表地址推导（AI 设置里「获取模型列表」用的 GET 地址）
        const bool urlOk =
            AiModelsUrlFromEndpoint(L"https://api.deepseek.com/chat/completions") ==
                L"https://api.deepseek.com/models" &&
            AiModelsUrlFromEndpoint(L"https://api.deepseek.com/v1/chat/completions") ==
                L"https://api.deepseek.com/v1/models" &&
            AiModelsUrlFromEndpoint(L"http://127.0.0.1:8080/v1/") == L"http://127.0.0.1:8080/v1/models" &&
            AiModelsUrlFromEndpoint(L"https://api.deepseek.com") == L"https://api.deepseek.com/models" &&
            AiModelsUrlFromEndpoint(L"").empty();
        t.Check(urlOk,
                L"模型列表地址推导：/chat/completions → /models（含 /v1、尾部斜杠、空值）");

        // 可选加密（DPAPI）：勾上之后文件里**不能**出现明文 Key，但仍要能读回原文
        {
            const std::wstring secret = L"sk-dpapi-plaintext-must-not-leak";
            AiConfig enc = LoadAiConfig();
            enc.keyFilePath = path;
            enc.protectKey = true;
            enc.apiKey = secret;
            const bool savedEnc = SaveAiConfig(enc);
            std::string raw;
            {
                UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (h.get() != INVALID_HANDLE_VALUE) {
                    DWORD size = ::GetFileSize(static_cast<HANDLE>(h.get()), nullptr);
                    if (size > 0 && size < (1 << 20)) {
                        raw.resize(size);
                        DWORD got = 0;
                        ::ReadFile(static_cast<HANDLE>(h.get()), raw.data(), size, &got, nullptr);
                        raw.resize(got);
                    }
                }
            }
            const std::string secretUtf8 = WideToUtf8(secret);
            const bool noPlain = !raw.empty() && raw.find(secretUtf8) == std::string::npos;
            const bool hasMarker = raw.find("\"dpapi:") != std::string::npos;
            const AiConfig back2 = LoadAiConfig();
            t.Check(savedEnc && noPlain && hasMarker && back2.apiKey == secret && back2.protectKey,
                    L"Key 可选加密（DPAPI）：文件里只有密文、读回仍是原文、protectKey 状态保持");
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

    // ---- 8. 克隆选项（深度/单分支/分支/无标签/部分克隆） ----
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
        t.Check(clone != nullptr && clone->flagCount == 6,
                L"克隆选项：命令表里有 6 个选项（含深度）");
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
        const bool builtOk = clone && BuildCommand(in, &b, &e);
        const std::wstring line = builtOk ? joinArgv(b.argvList.front()) : std::wstring();
        t.Check(builtOk && line.find(L"--depth=1") != std::wstring::npos &&
                    line.find(L"--single-branch") != std::wstring::npos &&
                    line.find(L"--branch=main") != std::wstring::npos &&
                    line.find(L"--no-tags") != std::wstring::npos &&
                    line.find(L"--filter=blob:none") != std::wstring::npos,
                L"克隆选项：深度等都进了 argv（" + line + L"）");
        // 深度必须是正整数：拦在构造期，别让用户看 git 的报错
        BuildInput bad = in;
        bad.flags["depth"] = L"abc";
        BuiltCommand b2;
        BuildError e2;
        // ★ 先执行再取 message：函数实参求值顺序未指定，若把 BuildCommand 写在
        //   Check() 的第一个实参里，第二个实参（消息）可能先被求值 → 打印出空 message
        //   （这个坑在原来 GUI 自检的同一行里就存在，输出一直是"拦下（）"）。
        const bool rejected = !BuildCommand(bad, &b2, &e2);
        t.Check(rejected && e2.message.find(L"正整数") != std::wstring::npos,
                L"克隆选项：深度非正整数被拦下（" + e2.message + L"）");
    }

    // ---- 9. AI 直出命令的只读白名单（"方案直接输出命令"的安全边界） ----
    {
        std::vector<std::wstring> a;
        std::wstring why;
        auto ro = [&](const wchar_t* line) {
            a.clear();
            why.clear();
            return ParseReadOnlyGitCommand(line, &a, &why);
        };
        t.Check(ro(L"git status -sb") && a.size() == 2 && a[0] == L"status",
                L"AI 只读命令：git status -sb 放行（拆成 argv）");
        t.Check(ro(L"git log --oneline -5"), L"AI 只读命令：git log 放行");
        t.Check(ro(L"git branch -a"), L"AI 只读命令：git branch -a 放行");
        t.Check(!ro(L"git branch newbranch"),
                L"AI 只读命令：git branch newbranch 拒绝（建分支是写操作）");
        t.Check(ro(L"git tag -l") && !ro(L"git tag v1.0"),
                L"AI 只读命令：git tag -l 放行、git tag v1.0 拒绝");
        t.Check(!ro(L"git push origin main"), L"AI 只读命令：git push 拒绝");
        t.Check(!ro(L"git reset --hard HEAD~1"), L"AI 只读命令：git reset --hard 拒绝");
        t.Check(ro(L"git remote -v") && !ro(L"git remote add o u"),
                L"AI 只读命令：git remote -v 放行、remote add 拒绝");
        t.Check(!ro(L"rm -rf /"), L"AI 只读命令：非 git 命令拒绝");
        t.Check(ro(L"git config --get user.name") && !ro(L"git config user.name x"),
                L"AI 只读命令：config --get 放行、config 写入拒绝");
    }

    // ---- 10. 端点安全判定（"本机免 Key"与"明文过网告警"的判定基础）----
    {
        // 本机：Key 为空也允许直连（本地 OpenAI 兼容服务通常不鉴权）
        t.Check(IsLoopbackEndpoint(L"http://127.0.0.1:11434/v1/chat/completions") &&
                    IsLoopbackEndpoint(L"http://localhost:8080/v1") &&
                    IsLoopbackEndpoint(L"http://[::1]:11434/v1") &&
                    IsLoopbackEndpoint(L"https://127.0.0.1/v1") &&
                    IsLoopbackEndpoint(L"http://user:pass@127.0.0.1:1234/v1") &&
                    IsLoopbackEndpoint(L"127.0.0.1:1234/v1"),   // 没写 scheme 也算
                L"端点判定：localhost / 127.x / [::1] / 带 userinfo / 无 scheme 都识别为本机");
        t.Check(!IsLoopbackEndpoint(L"https://api.deepseek.com/chat/completions") &&
                    !IsLoopbackEndpoint(L"http://192.168.1.5:8080/v1") &&
                    !IsLoopbackEndpoint(L"http://127.evil.com/v1") &&        // 只有数字与点才算 127/8
                    !IsLoopbackEndpoint(L"http://localhost.evil.com/v1") &&
                    !IsLoopbackEndpoint(L""),
                L"端点判定：远端 / 局域网 / 伪装成 127. 或 localhost 的域名都不算本机");
        // 明文过网 = http 且非本机；https、http+本机 都不算
        t.Check(IsInsecureRemoteEndpoint(L"http://192.168.1.5:8080/v1") &&
                    IsInsecureRemoteEndpoint(L"HTTP://api.example.com/v1"),   // scheme 大小写不敏感
                L"明文过网判定：http + 非本机（含大写 scheme）为真");
        t.Check(!IsInsecureRemoteEndpoint(L"http://127.0.0.1:11434/v1") &&
                    !IsInsecureRemoteEndpoint(L"https://api.deepseek.com/v1") &&
                    !IsInsecureRemoteEndpoint(L""),
                L"明文过网判定：http+本机、https、空值都为假");
    }

    return t.Summary("GitRT 核心单元测试：");
}
