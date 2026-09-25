// 非交互门面：GitRT.exe --run / --ai（《技术实现设计》§14.3）
//   · 与 GUI 共用同一条 "CommandSpec → BuildCommand → git_runner" 路径，
//     因此对 CLI 的测试等价于对菜单/参数面板逻辑的测试；
//   · 输出既写文件（--out，脚本可靠读取）也尽量写父控制台。
#include <algorithm>
#include "squash.h"
#include "remote.h"
#include "restore.h"
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
    // --cwd 对"不需要仓库"的命令就是**字面**工作目录（克隆时它决定目的地）；
    // 需要仓库的命令仍用探测到的仓库根，保证相对路径语义稳定
    in.cwd = spec->requiresRepo ? repoRoot : (o.cwd.empty() ? repoRoot : o.cwd);
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
// --------------------------------------------------------------- --squash
// 把"合并连续提交"做成可脚本化/可测试的入口：与 GUI 的复选框走**同一套** core 逻辑
//   GitRT.exe --squash <h1,h2,h3> [--message "合并信息"] [--cwd <repo>] [--dry-run] [--out <file>]
int RunCliSquash(const CliOptions& o) {
    LogInit();
    const bool console = AttachParentConsole();
    Sink sink;
    sink.path = o.outPath;
    sink.console = console;

    const std::wstring gitExe = FindGitExecutable();
    std::wstring repoRoot = o.cwd;
    if (!repoRoot.empty()) {
        const RepoProbeResult pr = ProbeRepo(repoRoot);
        if (pr.IsRepo()) repoRoot = pr.repoRoot;
    }
    std::vector<std::wstring> hashes;
    {
        std::wstring cur;
        for (const wchar_t c : o.squashHashes) {
            if (c == L',' || c == L';') {
                if (const std::wstring t = Trim(cur); !t.empty()) hashes.push_back(t);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (const std::wstring t = Trim(cur); !t.empty()) hashes.push_back(t);
    }

    sink.Line("[GitRT-SQUASH]");
    sink.LineW(L"repo=" + repoRoot);
    sink.Line("git=" + WideToUtf8(gitExe));
    sink.Line("selected=" + std::to_string(hashes.size()));
    for (size_t i = 0; i < hashes.size(); ++i) sink.LineW(L"pick" + std::to_wstring(i) + L"=" + hashes[i]);

    const SquashPlan plan = BuildSquashPlan(gitExe, repoRoot, hashes, o.squashMessage);
    if (!plan.ok) {
        sink.LineW(L"error=" + plan.error);
        sink.Line("ok=0");
        sink.Flush();
        return 1;
    }
    sink.LineW(L"oldest=" + plan.oldestHash);
    sink.LineW(L"newest=" + plan.newestHash);
    sink.LineW(L"base=" + plan.oldestParent);
    sink.LineW(L"mode=" + plan.mode);
    sink.Line("commits=" + std::to_string(plan.commits.size()));
    sink.Line("message_lines=" +
              std::to_string(std::count(plan.message.begin(), plan.message.end(), L'\n') + 1));
    {
        std::wstring first = plan.message;
        if (const size_t nl = first.find(L'\n'); nl != std::wstring::npos) first = first.substr(0, nl);
        sink.LineW(L"message_first=" + first);
    }
    for (const auto& c : plan.commandLines) sink.LineW(L"cmd=" + c);

    if (o.dryRun) {
        sink.Line("dry_run=1");
        sink.Line("ok=1");
        sink.Flush();
        return 0;
    }

    const SquashResult r = ApplySquash(
        gitExe, repoRoot, plan,
        [&](const std::wstring& cmd) { sink.LineW(L"> " + cmd); },
        [&](const std::string& out) {
            std::string cur;
            for (const char c : out) {
                if (c == '\n') {
                    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                    if (!cur.empty()) sink.Line("  " + cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) sink.Line("  " + cur);
        });
    for (const auto& s : r.steps) {
        sink.Line("step_exit=" + std::to_string(s.exitCode) + " cmd=" + WideToUtf8(s.commandLine));
    }
    if (!r.ok) {
        sink.LineW(L"error=" + r.error);
        sink.Line("ok=0");
        sink.Flush();
        return 1;
    }
    sink.LineW(L"new_hash=" + r.newHash);
    sink.Line("ok=1");
    sink.Flush();
    GRT_LOGI("cli", "--squash ok new=" << U8(r.newHash) << " selected=" << hashes.size());
    return 0;
}
// ---------------------------------------------------------------------------
// 远端基线（--remote-info [--remote-fetch]）与上游设置（--set-upstream <名字>）
// ---------------------------------------------------------------------------
namespace {

// 把多行文本逐行写进报告（带前缀），保持 CLI 报告好断言
void DumpLines(Sink& sink, const std::string& prefix, const std::wstring& text) {
    std::wstring cur;
    for (const wchar_t c : text) {
        if (c == L'\n') {
            if (!cur.empty() && cur.back() == L'\r') cur.pop_back();
            if (!cur.empty()) sink.Line(prefix + WideToUtf8(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) sink.Line(prefix + WideToUtf8(cur));
}

}  // namespace

int RunCliRemote(const CliOptions& o) {
    LogInit();
    const bool console = AttachParentConsole();
    Sink sink;
    sink.path = o.outPath;
    sink.console = console;

    const std::wstring gitExe = FindGitExecutable();
    std::wstring repoRoot = o.cwd;
    if (!repoRoot.empty()) {
        const RepoProbeResult pr = ProbeRepo(repoRoot);
        if (pr.IsRepo()) repoRoot = pr.repoRoot;
    }
    sink.Line("[GitRT-REMOTE]");
    sink.LineW(L"repo=" + repoRoot);
    sink.Line("git=" + WideToUtf8(gitExe));

    if (!o.setUpstream.empty()) {
        sink.LineW(L"set_upstream=" + o.setUpstream);
        const std::wstring err = SetUpstream(gitExe, repoRoot, o.setUpstream);
        if (!err.empty()) {
            sink.LineW(L"error=" + err);
            sink.Line("ok=0");
            sink.Flush();
            return 1;
        }
        sink.Line("set_upstream_ok=1");
    }

    // 远端地址管理（--remote-add / --remote-set-url / --remote-remove）
    {
        auto splitKv = [](const std::wstring& kv, std::wstring* k, std::wstring* v) {
            const size_t eq = kv.find(L'=');
            if (eq == std::wstring::npos) return false;
            *k = Trim(kv.substr(0, eq));
            *v = Trim(kv.substr(eq + 1));
            return true;
        };
        std::wstring k, v, err;
        if (!o.remoteAdd.empty()) {
            if (!splitKv(o.remoteAdd, &k, &v)) {
                sink.LineW(L"error=--remote-add 要写成 name=url");
                sink.Line("ok=0");
                sink.Flush();
                return 1;
            }
            sink.LineW(L"remote_add=" + k + L"=" + v);
            err = AddRemote(gitExe, repoRoot, k, v);
            if (!err.empty()) { sink.LineW(L"error=" + err); sink.Line("ok=0"); sink.Flush(); return 1; }
            sink.Line("remote_add_ok=1");
        }
        if (!o.remoteSetUrl.empty()) {
            if (!splitKv(o.remoteSetUrl, &k, &v)) {
                sink.LineW(L"error=--remote-set-url 要写成 name=url");
                sink.Line("ok=0");
                sink.Flush();
                return 1;
            }
            sink.LineW(L"remote_set_url=" + k + L"=" + v);
            err = SetRemoteUrl(gitExe, repoRoot, k, v);
            if (!err.empty()) { sink.LineW(L"error=" + err); sink.Line("ok=0"); sink.Flush(); return 1; }
            sink.Line("remote_set_url_ok=1");
        }
        if (!o.remoteRemove.empty()) {
            sink.LineW(L"remote_remove=" + o.remoteRemove);
            err = RemoveRemote(gitExe, repoRoot, o.remoteRemove);
            if (!err.empty()) { sink.LineW(L"error=" + err); sink.Line("ok=0"); sink.Flush(); return 1; }
            sink.Line("remote_remove_ok=1");
        }
    }

    if (o.remoteInfoMode == L"fetch") {
        sink.Line("fetching=1");
        const std::wstring err = FetchRemote(gitExe, repoRoot);
        if (!err.empty()) {
            sink.LineW(L"fetch_error=" + err);
        } else {
            sink.Line("fetched=1");
        }
    }

    const RemoteInfo info = LoadRemoteInfo(gitExe, repoRoot);
    if (!info.ok) {
        sink.LineW(L"error=" + info.error);
        sink.Line("ok=0");
        sink.Flush();
        return 1;
    }
    sink.LineW(L"branch=" + info.branch);
    sink.Line(std::string("has_upstream=") + (info.hasUpstream ? "1" : "0"));
    sink.LineW(L"upstream=" + info.upstream);
    sink.LineW(L"upstream_hash=" + info.upstreamHash);
    sink.LineW(L"upstream_subject=" + info.upstreamSubject);
    sink.Line("ahead=" + std::to_string(info.ahead));
    sink.Line("behind=" + std::to_string(info.behind));
    sink.LineW(L"remote_url=" + info.remoteUrl);

    // 本地新增的提交（远端还没有）：以远端为基，往上累加的那部分
    const std::vector<std::wstring> local = LocalOnlyCommits(gitExe, repoRoot, info);
    sink.Line("local_only=" + std::to_string(local.size()));

    const std::vector<RemoteEntry> remotes = LoadRemotes(gitExe, repoRoot);
    sink.Line("remotes=" + std::to_string(remotes.size()));
    for (size_t i = 0; i < remotes.size(); ++i) {
        std::wstring line = remotes[i].name + L" | " + remotes[i].fetchUrl;
        if (!remotes[i].pushUrl.empty() && remotes[i].pushUrl != remotes[i].fetchUrl) {
            line += L" | push: " + remotes[i].pushUrl;
        }
        sink.LineW(L"remote" + std::to_wstring(i) + L"=" + line);
    }
    const std::vector<RemoteBranch> branches = LoadRemoteBranches(gitExe, repoRoot);
    sink.Line("branches=" + std::to_string(branches.size()));
    for (size_t i = 0; i < branches.size(); ++i) {
        std::wstring line = branches[i].name + L" | " + branches[i].shortHash + L" | " +
                            branches[i].date + L" | " + branches[i].subject;
        if (branches[i].isUpstream) line += L" | upstream";
        if (branches[i].isHead) line += L" | remote-head";
        sink.LineW(L"branch" + std::to_wstring(i) + L"=" + line);
    }
    // 基线视图（给界面用的同一份文本，顺带让脚本能直接看）
    const RemoteBaseline base = LoadRemoteBaseline(gitExe, repoRoot, 20);
    DumpLines(sink, "base| ", DescribeRemoteBaseline(base));
    for (const auto& c : base.commits) {
        sink.LineW(L"commit=" + c.shortHash + (c.localOnly ? L" [本地] " : L" [远端] ") + c.subject);
    }
    sink.Line("ok=1");
    sink.Flush();
    GRT_LOGI("cli", "--remote-info branch=" << U8(info.branch) << " ahead=" << info.ahead
                                            << " behind=" << info.behind);
    return 0;
}

// ---------------------------------------------------------------------------
// 按提交还原（--restore <hash> [--mode detach|branch|soft|mixed|hard] [--branch <名字>] [--force]）
// ---------------------------------------------------------------------------
int RunCliRestore(const CliOptions& o) {
    LogInit();
    const bool console = AttachParentConsole();
    Sink sink;
    sink.path = o.outPath;
    sink.console = console;

    const std::wstring gitExe = FindGitExecutable();
    std::wstring repoRoot = o.cwd;
    if (!repoRoot.empty()) {
        const RepoProbeResult pr = ProbeRepo(repoRoot);
        if (pr.IsRepo()) repoRoot = pr.repoRoot;
    }

    RestoreMode mode = RestoreMode::DetachCheckout;
    if (!o.restoreMode.empty() && !ParseRestoreMode(o.restoreMode, &mode)) {
        sink.Line("[GitRT-RESTORE]");
        sink.LineW(L"error=未知的还原方式（detach|branch|soft|mixed|hard）：" + o.restoreMode);
        sink.Line("ok=0");
        sink.Flush();
        return 1;
    }

    sink.Line("[GitRT-RESTORE]");
    sink.LineW(L"repo=" + repoRoot);
    sink.Line("git=" + WideToUtf8(gitExe));
    sink.LineW(L"want=" + o.restoreHash);
    sink.LineW(L"mode=" + RestoreModeKey(mode));

    const RestorePlan plan =
        BuildRestorePlan(gitExe, repoRoot, o.restoreHash, mode, o.restoreBranch, o.forceRestore);
    if (!plan.ok) {
        sink.LineW(L"error=" + plan.error);
        sink.Line("ok=0");
        sink.Flush();
        return 1;
    }
    sink.LineW(L"target=" + plan.hash);
    sink.LineW(L"short=" + plan.shortHash);
    sink.LineW(L"subject=" + plan.subject);
    sink.LineW(L"branch=" + plan.branch);
    sink.LineW(L"new_branch=" + plan.newBranchName);
    sink.Line(std::string("dirty=") + (plan.dirty ? "1" : "0"));
    sink.Line("drop=" + std::to_string(plan.dropCount));
    sink.Line("dropped_pushed=" + std::to_string(plan.droppedPushed));
    sink.Line(std::string("has_upstream=") + (plan.hasUpstream ? "1" : "0"));
    sink.LineW(L"upstream=" + plan.upstream);
    sink.Line(std::string("needs_extra_confirm=") + (plan.needsExtraConfirm ? "1" : "0"));
    for (const auto& w : plan.warnings) sink.LineW(L"warn=" + w);
    for (const auto& c : plan.commandLines) sink.LineW(L"cmd=" + c);

    // 脏工作区上的硬重置：脚本必须显式 --force（等价于界面上的二次确认）
    if (plan.needsExtraConfirm && !o.forceRestore) {
        sink.LineW(L"error=硬重置会丢弃未提交的改动：确认无误请加 --force");
        sink.Line("ok=0");
        sink.Flush();
        return 1;
    }

    if (o.dryRun) {
        sink.Line("dry_run=1");
        sink.Line("ok=1");
        sink.Flush();
        return 0;
    }

    const RestoreResult r = ApplyRestore(
        gitExe, repoRoot, plan,
        [&](const std::wstring& cmd) { sink.LineW(cmd); },
        [&](const std::string& out) {
            std::string cur;
            for (const char c : out) {
                if (c == '\n') {
                    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                    if (!cur.empty()) sink.Line("  " + cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) sink.Line("  " + cur);
        });
    for (const auto& s : r.steps) {
        sink.Line("step_exit=" + std::to_string(s.exitCode) + " cmd=" + WideToUtf8(s.commandLine));
    }
    if (!r.ok) {
        sink.LineW(L"error=" + r.error);
        sink.Line("ok=0");
        sink.Flush();
        return 1;
    }
    sink.Line("ok=1");
    sink.Flush();
    GRT_LOGI("cli", "--restore ok mode=" << U8(RestoreModeKey(mode)) << " target=" << U8(plan.shortHash));
    return 0;
}
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

    const std::string sys = EffectiveSystemPrompt(cfg, [](uint16_t id) { return WideToUtf8(Str(id)); });
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
