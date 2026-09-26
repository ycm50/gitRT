// 标签（tag）实现 —— 见 tag.h 顶部说明
#include "tag.h"

#include <algorithm>
#include <set>

#include "command_builder.h"
#include "core.h"
#include "remote.h"   // ValidateRemoteName（漏了它构建会报 was not declared）

namespace grt {

namespace {

std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (const wchar_t c : s) {
        if (c == L'\n') {
            if (!cur.empty() && cur.back() == L'\r') cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::wstring> SplitFields(const std::wstring& line, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (const wchar_t c : line) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::wstring QuoteCmdArgs(const std::vector<std::wstring>& argv) {
    std::wstring s = L"git";
    for (const auto& a : argv) {
        s += L' ';
        s += QuoteArg(a);
    }
    return s;
}

// 记下一条命令：展示用 "git …" + 执行用 argv（两者一一对应，绝不反解析字符串）
void AddCommand(TagPlan* plan, const std::vector<std::wstring>& argv) {
    plan->argvList.push_back(argv);
    plan->commandLines.push_back(QuoteCmdArgs(argv));
}

// 远端名：空 → origin；只允许 IsValidRemoteName 认的字符（防选项注入）
std::wstring PickRemoteName(const std::wstring& remote) {
    const std::wstring r = Trim(remote);
    if (r.empty()) return L"origin";
    return r;
}

bool TagExistsLocally(const std::wstring& gitExe, const std::wstring& repoRoot,
                      const std::wstring& name) {
    int rc = 0;
    RunGitOut(gitExe, repoRoot, {L"rev-parse", L"--verify", L"--quiet", L"refs/tags/" + name}, &rc);
    return rc == 0;
}

// 目标修订：空 → HEAD。必须解析到一个**提交**（^{commit}），否则拒绝。
bool ResolveCommit(const std::wstring& gitExe, const std::wstring& repoRoot, const std::wstring& rev,
                   std::wstring* hash, std::wstring* error) {
    int rc = 0;
    const std::wstring full = Trim(RunGitOut(gitExe, repoRoot,
                                          {L"rev-parse", L"--verify", L"--quiet",
                                           rev + L"^{commit}"},
                                          &rc));
    if (rc != 0 || full.empty()) {
        *error = L"找不到这个提交：" + rev;
        return false;
    }
    *hash = full;
    return true;
}

}  // namespace

// ------------------------------------------------------------------ 标签清单
bool LoadTags(const std::wstring& gitExe, const std::wstring& repoRoot, std::vector<TagInfo>* out,
              std::wstring* error, const std::wstring& remoteName) {
    if (out) out->clear();
    if (error) error->clear();
    if (!out) return false;
    if (gitExe.empty() || repoRoot.empty()) {
        if (error) *error = L"没有可用的 git 或仓库";
        return false;
    }

    // ★ 分隔符 = %09(Tab)：format 不支持 %x1f。subject 里可能再有 Tab，见下面的拼回。
    const std::wstring txt = RunGitOut(
        gitExe, repoRoot,
        {L"for-each-ref", L"--sort=-creatordate",
         L"--format=%(refname:short)%09%(objecttype)%09%(objectname)%09%(objectname:short)"
         L"%09%(creatordate:short)%09%(*objectname)%09%(subject)",
         L"refs/tags"});

    for (const auto& line : SplitLines(txt)) {
        const auto f = SplitFields(line, L'\t');
        if (f.size() < 7) continue;
        TagInfo t;
        t.name = f[0];
        if (t.name.empty()) continue;
        t.annotated = (f[1] == L"tag");
        // 附注标签的 %(objectname) 是 tag 对象，%(*objectname) 才是它指向的提交；轻量标签后者为空
        t.hash = f[5].empty() ? f[2] : f[5];
        t.shortHash = f[3];
        // 附注标签：%(objectname:short) 是 tag 对象的短哈希，会跟上面剥过提交的 hash 对不上 → 统一按 hash 截
        if (!f[5].empty() && t.hash.size() >= 7) t.shortHash = t.hash.substr(0, 7);
        t.date = f[4];
        t.subject = f[6];
        for (size_t k = 7; k < f.size(); ++k) t.subject += L"\t" + f[k];   // subject 里的 Tab 拼回去
        out->push_back(std::move(t));
    }

    // 远端同名标签：离线/没有远端**不算致命**（onRemote 全 false）
    const std::wstring remote = PickRemoteName(remoteName);
    if (remote != L"-" && !remote.empty()) {
        int rc = 0;
        const std::wstring ls = RunGitOut(gitExe, repoRoot, {L"ls-remote", L"--tags", remote}, &rc);
        if (rc == 0) {
            std::set<std::wstring> remoteNames;
            for (const auto& line : SplitLines(ls)) {
                // "<hash>\trefs/tags/<name>"（附注标签还会多一行 refs/tags/<name>^{}）
                const size_t tab = line.find(L'\t');
                if (tab == std::wstring::npos) continue;
                std::wstring ref = Trim(line.substr(tab + 1));
                if (ref.rfind(L"refs/tags/", 0) != 0) continue;
                std::wstring n = ref.substr(10);
                if (EndsWith(n, L"^{}")) n = n.substr(0, n.size() - 3);
                remoteNames.insert(n);
            }
            for (auto& t : *out) {
                if (remoteNames.count(t.name) != 0) t.onRemote = true;
            }
        }
    }
    return true;
}

std::wstring DescribeTags(const std::vector<TagInfo>& tags) {
    if (tags.empty()) {
        return L"（没有标签：用「打标签…」新建一个，标签是给某次提交起的固定名字）\r\n";
    }
    std::wstring s;
    for (const auto& t : tags) {
        s += t.name;
        s += L"  ";
        s += t.shortHash;
        s += L"  ";
        s += t.date;
        s += L"  ";
        s += t.annotated ? L"附注" : L"轻量";
        s += t.onRemote ? L"  远端:有" : L"  远端:无";
        s += L"  ";
        s += t.subject;
        s += L"\r\n";
    }
    return s;
}

// ------------------------------------------------------------------ 新建标签
TagPlan BuildTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                     const std::wstring& name, const std::wstring& message, bool annotated,
                     const std::wstring& target, bool force) {
    TagPlan plan;
    plan.op = L"create";
    plan.annotated = annotated;
    plan.force = force;
    plan.name = Trim(name);
    plan.message = message;

    if (gitExe.empty() || repoRoot.empty()) {
        plan.error = L"没有可用的 git 或仓库";
        return plan;
    }
    if (plan.name.empty()) {
        plan.error = L"请填写标签名（例如 v1.0.0）";
        return plan;
    }

    // 名字合法性交给 git 自己判定（refs/tags/<name> 必须能通过 check-ref-format）
    {
        int rc = 0;
        std::wstring err;
        RunGitOut(gitExe, repoRoot, {L"check-ref-format", L"refs/tags/" + plan.name}, &rc, &err);
        if (rc != 0) {
            plan.error = L"标签名不合法：" + plan.name +
                         L"（不能有空格 / ~ ^ : ? * [ \\ / 开头结尾的斜杠或点，也不能以 - 开头）";
            return plan;
        }
    }

    // 同名已存在：不给 force 就拒绝，给了才加 -f
    if (TagExistsLocally(gitExe, repoRoot, plan.name)) {
        plan.forceNeeded = true;
        if (!force) {
            plan.error = L"标签已存在：" + plan.name + L"（要覆盖它请勾选「强制覆盖」（CLI 加 --force））";
            return plan;
        }
        plan.warnings.push_back(L"标签 " + plan.name + L" 已存在：这条命令会**移动**它到新提交（-f）");
    }

    // 目标修订（空 = HEAD）必须解析到提交
    const std::wstring rev = Trim(target).empty() ? std::wstring(L"HEAD") : Trim(target);
    std::wstring hash;
    if (std::wstring err; !ResolveCommit(gitExe, repoRoot, rev, &hash, &err)) {
        plan.error = err;
        return plan;
    }
    plan.target = hash;

    // 命令行：轻量 / 附注（+ 可选 -f）
    std::vector<std::wstring> argv{L"tag"};
    if (annotated) {
        argv.push_back(L"-a");
        argv.push_back(plan.name);
        argv.push_back(L"-m");
        argv.push_back(plan.message.empty() ? plan.name : plan.message);
    } else {
        argv.push_back(plan.name);
    }
    if (force) {
        // -f 紧跟在 tag 后面，读起来最顺：git tag -f <name> <target>
        std::vector<std::wstring> withF{L"tag", L"-f"};
        withF.insert(withF.end(), argv.begin() + 1, argv.end());
        argv = std::move(withF);
    }
    argv.push_back(hash);
    AddCommand(&plan, argv);

    if (!message.empty() && !annotated) {
        plan.warnings.push_back(L"给了信息但没选「附注标签」：轻量标签不保存信息，只有附注标签才有");
    }
    if (annotated && plan.message.empty()) {
        plan.warnings.push_back(L"附注标签没给信息：会用标签名当信息");
    }
    plan.ok = true;
    return plan;
}

// ------------------------------------------------------------------ 推送标签
TagPlan BuildPushTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const std::wstring& name, bool all, const std::wstring& remote) {
    TagPlan plan;
    plan.op = L"push";
    plan.name = Trim(name);
    plan.force = false;

    if (gitExe.empty() || repoRoot.empty()) {
        plan.error = L"没有可用的 git 或仓库";
        return plan;
    }
    const std::wstring r = PickRemoteName(remote);
    if (!IsValidRemoteName(r)) {
        plan.error = L"远端名不合法：" + r;
        return plan;
    }
    // 远端必须存在（否则 git push 会给出难懂的报错）
    {
        int rc = 0;
        RunGitOut(gitExe, repoRoot, {L"remote", L"get-url", r}, &rc);
        if (rc != 0) {
            plan.error = L"没有这个远端：" + r + L"（先在「远端分支与地址…」里加一个）";
            return plan;
        }
    }

    if (all) {
        plan.warnings.push_back(L"--tags 会把**所有**本地标签都推上去（不只是这一个）");
        AddCommand(&plan, {L"push", r, L"--tags"});
        plan.ok = true;
        return plan;
    }

    if (plan.name.empty()) {
        plan.error = L"请填写要推送的标签名（或加 --all 推送全部）";
        return plan;
    }
    if (!TagExistsLocally(gitExe, repoRoot, plan.name)) {
        plan.error = L"本地没有这个标签：" + plan.name;
        return plan;
    }
    plan.target = Trim(RunGitOut(gitExe, repoRoot, {L"rev-parse", L"--verify", L"--quiet",
                                                 L"refs/tags/" + plan.name}));
    AddCommand(&plan, {L"push", r, plan.name});
    plan.ok = true;
    return plan;
}

// ------------------------------------------------------------------ 删除标签
TagPlan BuildDeleteTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                           const std::wstring& name, bool remote, const std::wstring& remoteName) {
    TagPlan plan;
    plan.op = L"delete";
    plan.destructive = true;   // 破坏性：CLI 必须 --force 才真的执行
    plan.name = Trim(name);

    if (gitExe.empty() || repoRoot.empty()) {
        plan.error = L"没有可用的 git 或仓库";
        return plan;
    }
    if (plan.name.empty()) {
        plan.error = L"请填写要删除的标签名";
        return plan;
    }
    if (!TagExistsLocally(gitExe, repoRoot, plan.name)) {
        plan.error = L"本地没有这个标签：" + plan.name;
        return plan;
    }
    plan.target = Trim(RunGitOut(gitExe, repoRoot, {L"rev-parse", L"--verify", L"--quiet",
                                                 L"refs/tags/" + plan.name}));

    plan.commandLines.push_back(QuoteCmdArgs({L"tag", L"-d", plan.name}));
    plan.argvList.push_back({L"tag", L"-d", plan.name});
    plan.warnings.push_back(L"删除标签只删掉这个名字，不会删掉任何提交");

    if (remote) {
        const std::wstring r = PickRemoteName(remoteName);
        if (!IsValidRemoteName(r)) {
            plan.error = L"远端名不合法：" + r;
            return plan;
        }
        int rc = 0;
        RunGitOut(gitExe, repoRoot, {L"remote", L"get-url", r}, &rc);
        if (rc != 0) {
            plan.error = L"没有这个远端：" + r;
            return plan;
        }
        AddCommand(&plan, {L"push", r, L":refs/tags/" + plan.name});
        plan.warnings.push_back(L"远端 " + r + L" 上的同名标签也会被删掉（别人下次抓取就看不到了）");
    }
    plan.ok = true;
    return plan;
}

// ------------------------------------------------------------------ 执行
TagResult ApplyTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot, const TagPlan& plan,
                       const std::function<void(const std::wstring&)>& onCommand,
                       const std::function<void(const std::string&)>& onOutput) {
    TagResult res;
    if (!plan.ok) {
        res.error = plan.error.empty() ? L"标签计划不可执行" : plan.error;
        return res;
    }
    if (gitExe.empty() || repoRoot.empty()) {
        res.error = L"没有可用的 git 或仓库";
        return res;
    }

    const size_t n = std::min(plan.argvList.size(), plan.commandLines.size());
    for (size_t i = 0; i < n; ++i) {
        const std::wstring& line = plan.commandLines[i];
        const std::vector<std::wstring>& argv = plan.argvList[i];

        if (onCommand) onCommand(L"> " + line);
        const RunResult r = RunGitSync(gitExe, argv, repoRoot, 120000);
        TagStep step;
        step.commandLine = line;
        step.exitCode = r.exitCode;
        step.out = W(r.out);
        step.err = W(r.err);
        res.steps.push_back(step);
        if (onOutput) {
            if (!r.out.empty()) onOutput(r.out);
            if (!r.err.empty()) onOutput(r.err);
        }
        if (r.spawnFailed) {
            res.error = L"启动 git 失败";
            return res;
        }
        if (r.exitCode != 0) {
            res.error = step.err.empty() ? (L"命令失败（exit=" + std::to_wstring(r.exitCode) + L"）")
                                         : step.err;
            return res;
        }
    }
    res.ok = true;
    return res;
}

}  // namespace grt
