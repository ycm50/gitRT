// 非交互门面：GitRT.exe --run / --ai（《技术实现设计》§14.3）
//   · 与 GUI 共用同一条 "CommandSpec → BuildCommand → git_runner" 路径，
//     因此对 CLI 的测试等价于对菜单/参数面板逻辑的测试；
//   · 输出既写文件（--out，脚本可靠读取）也尽量写父控制台。
#include "gui.h"

#include "ai_client.h"
#include "json_util.h"

#include <cstdio>

using namespace grt;

namespace grt::gui {

namespace {

std::wstring ReadBranch(const std::wstring& gitExe, const std::wstring& repoRoot) {
    if (gitExe.empty() || repoRoot.empty()) return {};
    const RunResult r = RunGitSync(gitExe, {L"rev-parse", L"--abbrev-ref", L"HEAD"}, repoRoot, 8000);
    if (r.exitCode != 0) return {};
    return Trim(W(r.out));
}

std::vector<std::wstring> SplitSemicolon(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t p = s.find(L';', start);
        const std::wstring one = Trim(s.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start));
        if (!one.empty()) out.push_back(one);
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return out;
}

bool AttachParentConsole() {
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
    return true;
}

struct Sink {
    std::string  text;
    std::wstring path;
    bool         console = false;

    void Line(const std::string& s) {
        text += s;
        text += "\r\n";
    }
    void LineW(const std::wstring& s) { Line(WideToUtf8(s)); }
    bool Flush() {
        bool ok = true;
        if (!path.empty()) {
            UniqueHandle h(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
            if (h.get() == INVALID_HANDLE_VALUE) {
                ok = false;
            } else {
                DWORD wrote = 0;
                ::WriteFile(static_cast<HANDLE>(h.get()), text.data(), static_cast<DWORD>(text.size()),
                            &wrote, nullptr);
            }
        }
        if (console) {
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
        }
        return ok;
    }
};

std::string Argv0Line(const std::vector<std::wstring>& argv) {
    std::string s = "git";
    for (const auto& a : argv) s += " " + WideToUtf8(QuoteArg(a));
    return s;
}

}  // namespace

// ============================================================ --list-commands
int RunCliListCommands(const CliOptions& o) {
    const bool console = AttachParentConsole();
    Sink sink;
    sink.path = o.outPath;
    sink.console = console;
    sink.Line("[GitRT-CMDLIST]");
    sink.Line("count=" + std::to_string(CommandTableSize()));
    for (size_t i = 0; i < CommandTableSize(); ++i) {
        const CommandSpec& c = CommandTable()[i];
        const char* exec = "cli";
        switch (c.exec) {
            case ExecKind::Internal:  exec = "internal"; break;
            case ExecKind::Cli:       exec = "cli"; break;
            case ExecKind::CliPanel:  exec = "panel"; break;
            case ExecKind::CliStream: exec = "stream"; break;
        }
        std::string flags;
        for (uint8_t k = 0; k < c.flagCount; ++k) {
            if (k) flags += ",";
            flags += c.flags[k].key;
        }
        sink.Line(std::string("CMD|") + c.key + "|" + GroupKey(c.group) + "|" +
                  (c.requiresRepo ? "1" : "0") + "|" + exec + "|" + (c.paramKey ? c.paramKey : "") + "|" +
                  flags);
    }
    sink.Line("exit=0");
    sink.Flush();
    return 0;
}

// ============================================================ --run
int RunCliCommand(const CliOptions& o) {
    LogInit();
    const bool console = AttachParentConsole();
    Sink sink;
    sink.path = o.outPath;
    sink.console = console;

    const std::string key = WideToUtf8(o.runKey);
    const CommandSpec* spec = FindCommandByKey(key);
    sink.Line("[GitRT-CLI]");
    sink.Line("key=" + key);
    if (!spec) {
        sink.Line("error=command key not found in the command table");
        sink.Line("exit=3");
        sink.Flush();
        return 3;
    }

    const std::wstring gitExe = FindGitExecutable();
    std::wstring repoRoot = o.cwd;
    if (!repoRoot.empty()) {
        const RepoProbeResult pr = ProbeRepo(repoRoot);
        if (pr.IsRepo()) repoRoot = pr.repoRoot;
    }
    const std::vector<std::wstring> paths = SplitSemicolon(o.pathsSpec);

    sink.LineW(L"cwd=" + o.cwd);
    sink.LineW(L"repo=" + repoRoot);
    sink.Line("git=" + WideToUtf8(gitExe));
    sink.Line("requires_repo=" + std::string(spec->requiresRepo ? "1" : "0"));
    sink.Line("paths=" + std::to_string(paths.size()));

    BuildInput in;
    in.spec = spec;
    in.gitExe = gitExe;
    in.repoRoot = repoRoot;
    in.cwd = repoRoot;
    in.paths = paths;
    in.flags = o.flags;
    in.params = o.params;

    BuildError err;
    BuiltCommand built;
    if (!BuildCommand(in, &built, &err)) {
        sink.Line("error=" + WideToUtf8(err.message));
        sink.Line("exit=1");
        sink.Flush();
        return 1;
    }
    for (size_t i = 0; i < built.argvList.size(); ++i)
        sink.Line("argv" + std::to_string(i) + "=" + Argv0Line(built.argvList[i]));
    for (const auto& n : built.notes) sink.LineW(L"note=" + n);
    sink.Line("cmdline=" + WideToUtf8(built.display));

    if (spec->exec == ExecKind::Internal) {
        sink.Line("note=internal command (needs the GUI); nothing executed");
        sink.Line("exit=1");
        sink.Flush();
        return 1;
    }
    if (o.dryRun) {
        sink.Line("dry_run=1");
        sink.Line("exit=0");
        sink.Flush();
        return 0;
    }

    int finalExit = 0;
    std::string allOut, allErr;
    for (size_t i = 0; i < built.argvList.size(); ++i) {
        Invocation inv;
        inv.exe = gitExe;
        inv.argv = built.argvList[i];
        inv.cwd = built.cwd.empty() ? repoRoot : built.cwd;
        inv.env = BuildGitEnvironment(false, L"");
        inv.timeoutMs = (spec->exec == ExecKind::CliStream) ? 300000 : 120000;
        auto runner = MakeProcessGitRunner();
        const RunResult r = runner->RunSync(inv, nullptr);
        allOut += r.out;
        allErr += r.err;
        sink.Line("step" + std::to_string(i) + "_exit=" + std::to_string(r.exitCode));
        if (r.spawnFailed) {
            sink.Line("error=spawn failed win32=" + std::to_string(r.win32Error));
            finalExit = 2;
            break;
        }
        if (r.exitCode != 0) {
            finalExit = 2;
            break;
        }
    }
    sink.Line("---- stdout ----");
    sink.Line(allOut);
    sink.Line("---- stderr ----");
    sink.Line(allErr);
    sink.Line("exit=" + std::to_string(finalExit));
    sink.Flush();
    GRT_LOGI("cli", "--run " << key << " exit=" << finalExit);
    return finalExit;
}

// ============================================================ --ai
int RunCliAi(const CliOptions& o) {
    LogInit();
    const bool console = AttachParentConsole();
    Sink sink;
    sink.path = o.outPath;
    sink.console = console;

    const AiConfig cfg = LoadAiConfig();
    std::wstring repoRoot = o.cwd;
    if (!repoRoot.empty()) {
        const RepoProbeResult pr = ProbeRepo(repoRoot);
        if (pr.IsRepo()) repoRoot = pr.repoRoot;
    }
    const std::wstring gitExe = FindGitExecutable();
    const std::wstring branch = ReadBranch(gitExe, repoRoot);
    const std::vector<std::wstring> paths = SplitSemicolon(o.pathsSpec);

    sink.Line("[GitRT-AI]");
    sink.LineW(L"endpoint=" + cfg.endpoint);
    sink.LineW(L"model=" + cfg.model);
    sink.Line("key_env=" + cfg.apiKeyEnv);
    sink.Line(std::string("key_set=") + (ResolveApiKey(cfg).empty() ? "0" : "1"));
    sink.LineW(L"prompt=" + o.aiPrompt);
    sink.LineW(L"repo=" + repoRoot);
    sink.LineW(L"branch=" + branch);

    const std::string sys = BuildPlannerSystemPrompt([](uint16_t id) { return WideToUtf8(Str(id)); });
    const std::string user = BuildPlannerUserPrompt(o.aiPrompt, repoRoot, branch, paths);
    sink.Line("system_prompt_bytes=" + std::to_string(sys.size()));

    const AiPlan plan = GeneratePlan(cfg, sys, user);
    sink.Line("http=" + std::to_string(plan.httpStatus));
    sink.Line("elapsed_ms=" + std::to_string(plan.elapsedMs));
    sink.Line(std::string("ok=") + (plan.ok ? "1" : "0"));
    if (!plan.error.empty()) sink.LineW(L"error=" + plan.error);
    if (plan.ok) {
        sink.Line("command=" + plan.commandKey);
        sink.Line(std::string("no_command=") + (plan.noCommand ? "1" : "0"));
        for (const auto& kv : plan.params) sink.Line("param." + kv.first + "=" + kv.second);
        for (const auto& kv : plan.flags) sink.Line("flag." + kv.first + "=" + kv.second);
        sink.LineW(L"explanation=" + plan.explanation);
    }
    sink.Line("---- reply ----");
    sink.Line(WideToUtf8(plan.rawReply));

    if (!plan.ok) {
        sink.Line("exit=4");
        sink.Flush();
        return 4;
    }
    if (plan.noCommand) {
        sink.Line("exit=5");
        sink.Flush();
        return 5;
    }

    // 安全闸门必须**无条件**执行并在报告里体现：
    // 即使只是预览（--ai 不带 --ai-run），也要告诉调用方该方案是否被接受。
    const PlanToCommandResult r = PlanToCommand(plan, repoRoot, paths, gitExe);
    if (!r.ok) {
        sink.LineW(L"plan_error=" + r.error);
        sink.Line("exit=1");
        sink.Flush();
        return 1;
    }
    for (const auto& w : r.warnings) sink.LineW(L"warning=" + w);
    sink.Line("cmdline=" + WideToUtf8(r.built.display));
    for (size_t i = 0; i < r.built.argvList.size(); ++i)
        sink.Line("argv" + std::to_string(i) + "=" + Argv0Line(r.built.argvList[i]));

    if (o.dryRun || !o.aiRun) {
        sink.Line("dry_run=" + std::string(o.dryRun ? "1" : "0"));
        sink.Line("executed=0");
        sink.Line("exit=0");
        sink.Flush();
        return 0;
    }
    int finalExit = 0;
    std::string allOut, allErr;
    for (const auto& argv : r.built.argvList) {
        Invocation inv;
        inv.exe = gitExe;
        inv.argv = argv;
        inv.cwd = r.built.cwd.empty() ? repoRoot : r.built.cwd;
        inv.env = BuildGitEnvironment(false, L"");
        inv.timeoutMs = 300000;
        auto runner = MakeProcessGitRunner();
        const RunResult run = runner->RunSync(inv, nullptr);
        allOut += run.out;
        allErr += run.err;
        if (run.spawnFailed || run.exitCode != 0) {
            finalExit = 2;
            break;
        }
    }
    sink.Line("executed=1");
    sink.Line("---- stdout ----");
    sink.Line(allOut);
    sink.Line("---- stderr ----");
    sink.Line(allErr);
    sink.Line("exit=" + std::to_string(finalExit));
    sink.Flush();
    GRT_LOGI("cli", "--ai -> " << plan.commandKey << " exit=" << finalExit);
    return finalExit;
}

}  // namespace grt::gui
