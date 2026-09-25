// 合并提交（squash）实现 —— 见 squash.h 顶部的算法说明
#include "squash.h"

#include <algorithm>
#include <map>
#include <set>

#include "core.h"

namespace grt {

namespace {

// 只接受十六进制哈希（4..40 位）：既防"选项注入"，也够覆盖界面/CLI 的用法
bool IsHexHash(std::wstring_view s) {
    if (s.size() < 4 || s.size() > 40) return false;
    for (const wchar_t c : s) {
        const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        if (!hex) return false;
    }
    return true;
}

std::wstring RunOut(const std::wstring& gitExe, const std::wstring& repoRoot,
                    const std::vector<std::wstring>& argv, int* exitCode = nullptr,
                    std::wstring* errOut = nullptr) {
    const RunResult r = RunGitSync(gitExe, argv, repoRoot, 30000);
    if (exitCode) *exitCode = r.exitCode;
    if (errOut) *errOut = Trim(W(r.err));
    return W(r.out);
}

// 逐行取字段（用 \x1f 分隔，避免 subject 里的空格/中文干扰解析）
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

// 未完成的 rebase / merge / cherry-pick 一律拒绝（改写历史时最怕叠在这些状态上）
std::wstring PendingOperation(const std::wstring& gitExe, const std::wstring& repoRoot) {
    int rc = 0;
    std::wstring gitDir = Trim(RunOut(gitExe, repoRoot, {L"rev-parse", L"--absolute-git-dir"}, &rc));
    if (rc != 0 || gitDir.empty()) return {};   // 不是仓库：交给后面的命令报错
    for (const wchar_t* name : {L"rebase-merge", L"rebase-apply", L"MERGE_HEAD", L"CHERRY_PICK_HEAD",
                                L"REVERT_HEAD"}) {
        if (PathExists(gitDir + L"\\" + name)) return name;
    }
    return {};
}

}  // namespace

std::vector<CommitEntry> LoadCommitList(const std::wstring& gitExe, const std::wstring& repoRoot,
                                        int limit) {
    std::vector<CommitEntry> out;
    const std::wstring fmt = L"--pretty=format:%H%x1f%h%x1f%an%x1f%ad%x1f%P%x1f%s";
    const std::wstring n = L"-n" + std::to_wstring(limit);
    int rc = 0;
    const std::wstring text = RunOut(gitExe, repoRoot,
                                    {L"log", L"--first-parent", n, fmt, L"--date=short"}, &rc);
    if (rc != 0) return out;
    for (const auto& line : SplitLines(text)) {
        const auto f = SplitFields(line, 0x1f);
        if (f.size() < 6) continue;
        CommitEntry e;
        e.hash = f[0];
        e.shortHash = f[1];
        e.author = f[2];
        e.date = f[3];
        e.subject = f[5];
        size_t parents = 0;
        for (const wchar_t c : Trim(f[4])) {
            if (c == L' ') ++parents;
        }
        e.parentCount = f[4].empty() ? 0 : parents + 1;
        out.push_back(e);
    }
    return out;
}

std::wstring JoinSubjects(const std::vector<CommitEntry>& oldestToNewest) {
    std::wstring msg;
    for (const auto& c : oldestToNewest) {
        if (!msg.empty()) msg += L'\n';
        msg += c.subject;
    }
    return msg;
}

SquashPlan BuildSquashPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                           const std::vector<std::wstring>& hashes,
                           const std::wstring& messageOverride) {
    SquashPlan plan;
    if (hashes.size() < 2) {
        plan.error = L"至少选中 2 条提交才能合并";
        return plan;
    }
    for (const auto& h : hashes) {
        if (!IsHexHash(h)) {
            plan.error = L"提交标识不是合法的哈希：" + h;
            return plan;
        }
    }
    if (gitExe.empty() || repoRoot.empty()) {
        plan.error = L"未找到 git 或仓库";
        return plan;
    }

    if (const std::wstring pending = PendingOperation(gitExe, repoRoot); !pending.empty()) {
        plan.error = L"仓库里有未完成的 " + pending + L"，先结束它（git rebase --abort / git merge --abort）再合并";
        return plan;
    }

    // 沿第一父链取全量列表：既给出顺序，也给出每个提交的真实父数（识别 merge）
    const std::vector<CommitEntry> chain = LoadCommitList(gitExe, repoRoot, 5000);
    if (chain.size() < 2) {
        plan.error = L"当前分支的提交不足 2 条";
        return plan;
    }
    std::map<std::wstring, size_t> index;
    for (size_t i = 0; i < chain.size(); ++i) index[chain[i].hash] = i;

    // 选中项 → 链上序号
    std::vector<size_t> idx;
    for (const auto& h : hashes) {
        auto it = index.find(h);
        if (it == index.end()) {
            // 允许短哈希：在链上按前缀找唯一匹配
            std::vector<size_t> hits;
            for (size_t i = 0; i < chain.size(); ++i) {
                if (chain[i].hash.rfind(h, 0) == 0) hits.push_back(i);
            }
            if (hits.size() != 1) {
                plan.error = L"提交 " + h + L" 不在当前分支的线性历史里（或无法唯一确定）";
                return plan;
            }
            idx.push_back(hits[0]);
        } else {
            idx.push_back(it->second);
        }
    }
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    if (idx.size() < 2) {
        plan.error = L"至少选中 2 条提交才能合并";
        return plan;
    }
    // 必须连续
    for (size_t i = 1; i < idx.size(); ++i) {
        if (idx[i] != idx[i - 1] + 1) {
            plan.error = L"只能合并**连续**的提交：选中的第 " + std::to_wstring(i) + L" 与第 " +
                         std::to_wstring(i + 1) + L" 条之间还有没选中的提交";
            return plan;
        }
    }
    const size_t newest = idx.front();          // 链上序号小的 = 新
    const size_t oldest = idx.back();           // 序号大的 = 旧
    for (const size_t i : idx) {
        if (chain[i].parentCount > 1) {
            plan.error = L"\u6240\u9009\u63d0\u4ea4\u91cc\u5305\u542b**\u5408\u5e76\u63d0\u4ea4**\uff08merge\uff0c\u63d0\u4ea4 " + chain[i].shortHash +
                         L"\uff09\uff0c\u8fd9\u79cd\u63d0\u4ea4\u4e0d\u80fd\u88ab\u8fd9\u6837\u5408\u5e76";
            return plan;
        }
        if (chain[i].parentCount == 0) {   // 第一父链上只有最旧那条可能是根提交
            plan.error = L"\u6240\u9009\u533a\u95f4\u89e6\u5230**\u6839\u63d0\u4ea4**\uff08" + chain[i].shortHash +
                         L"\uff09\uff1a\u6839\u63d0\u4ea4\u6ca1\u6709\u7236\uff0c\u65e0\u6cd5\u4f5c\u4e3a\u5408\u5e76\u57fa\u7ebf\uff0c\u6362\u4e00\u6bb5\u533a\u95f4\u8bd5\u8bd5";
            return plan;
        }
    }
    for (size_t i = 0; i < newest; ++i) {
        if (chain[i].parentCount != 1) {
            plan.error = L"所选提交之上还有**合并提交**（" + chain[i].shortHash +
                         L"），本版本不支持在这种历史里合并，请先处理它";
            return plan;
        }
    }
    // 暂存区必须干净：reset --soft 会连着已暂存的内容一起提交
    {
        int rc = 0;
        RunOut(gitExe, repoRoot, {L"diff-index", L"--cached", L"--quiet", L"HEAD"}, &rc);
        if (rc != 0) {
            plan.error = L"暂存区还有未提交的改动，先提交或取消暂存，否则它们会被一起并进新提交";
            return plan;
        }
    }
    // 仓库根（用于兜底校验）
    {
        int rc = 0;
        const std::wstring top = Trim(RunOut(gitExe, repoRoot, {L"rev-parse", L"--show-toplevel"}, &rc));
        if (rc != 0) {
            plan.error = L"当前目录不是 git 仓库";
            return plan;
        }
        (void)top;
    }

    for (size_t i = oldest; i >= newest && i < chain.size(); --i) {
        plan.commits.push_back(chain[i]);   // 旧 → 新
        if (i == newest) break;
    }
    plan.oldestHash = chain[oldest].hash;
    plan.newestHash = chain[newest].hash;
    plan.headIndex = newest;
    plan.message = messageOverride.empty() ? JoinSubjects(plan.commits) : messageOverride;
    plan.oldestParent = chain[oldest + 1].hash;   // 链上后一个就是父（第一父链）
    {
        int rc = 0;
        plan.newestTree = Trim(RunOut(gitExe, repoRoot, {L"rev-parse", plan.newestHash + L"^{tree}"}, &rc));
        if (rc != 0 || plan.newestTree.empty()) {
            plan.error = L"读取提交 " + chain[newest].shortHash + L" 的 tree 失败";
            return plan;
        }
    }

    const std::wstring msgOneLine = plan.message.substr(0, plan.message.find(L'\n'));
    if (newest == 0) {
        plan.mode = L"reset-soft";
        plan.commandLines = {
            L"git reset --soft " + plan.oldestParent,
            L"git commit -m \"" + msgOneLine + L"\"" +
                (plan.message.find(L'\n') == std::wstring::npos
                     ? L""
                     : L"   (合并信息共 " + std::to_wstring(std::count(plan.message.begin(), plan.message.end(), L'\n') + 1) + L" 行)"),
        };
    } else {
        plan.mode = L"commit-tree+rebase";
        plan.commandLines = {
            L"git commit-tree " + plan.newestTree.substr(0, 12) + L"… -p " + plan.oldestParent.substr(0, 12) +
                L"… -m \"" + msgOneLine + L"\"",
            L"git rebase --onto <新提交> " + chain[newest].shortHash,
        };
    }
    plan.ok = true;
    return plan;
}

SquashResult ApplySquash(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const SquashPlan& plan, const std::function<void(const std::wstring&)>& onCommand,
                         const std::function<void(const std::string&)>& onOutput) {
    SquashResult res;
    if (!plan.ok) {
        res.error = plan.error;
        return res;
    }
    auto step = [&](const std::vector<std::wstring>& argv, const std::wstring& display) -> RunResult {
        if (onCommand) onCommand(display);
        const RunResult r = RunGitSync(gitExe, argv, repoRoot, 120000);
        if (onOutput) {
            std::string body = r.out;
            if (!r.err.empty()) {
                if (!body.empty() && body.back() != '\n') body += '\n';
                body += r.err;
            }
            onOutput(body);
        }
        SquashStep s;
        s.commandLine = display;
        s.exitCode = r.exitCode;
        s.out = W(r.out);
        s.err = W(r.err);
        res.steps.push_back(s);
        return r;
    };
    const std::wstring msgOneLine = plan.message.substr(0, plan.message.find(L'\n'));

    if (plan.mode == L"reset-soft") {
        const RunResult a = step({L"reset", L"--soft", plan.oldestParent}, L"git reset --soft " + plan.oldestParent);
        if (a.exitCode != 0) {
            res.error = L"git reset --soft 失败：" + Trim(W(a.err));
            return res;
        }
        const RunResult b = step({L"commit", L"-m", plan.message},
                                 L"git commit -m \"" + msgOneLine + L"\"");
        if (b.exitCode != 0) {
            res.error = L"git commit 失败：" + Trim(W(b.err));
            return res;
        }
        int rc = 0;
        res.newHash = Trim(RunOut(gitExe, repoRoot, {L"rev-parse", L"HEAD"}, &rc));
        res.ok = rc == 0 && !res.newHash.empty();
        if (!res.ok) res.error = L"提交已创建，但读取新提交哈希失败";
        return res;
    }

    // commit-tree + rebase --onto
    const RunResult c = step({L"commit-tree", plan.newestTree, L"-p", plan.oldestParent, L"-m", plan.message},
                             L"git commit-tree " + plan.newestTree.substr(0, 12) + L"… -p " +
                                 plan.oldestParent.substr(0, 12) + L"… -m \"" + msgOneLine + L"\"");
    if (c.exitCode != 0) {
        res.error = L"git commit-tree 失败：" + Trim(W(c.err));
        return res;
    }
    const std::wstring newHash = Trim(W(c.out));
    if (newHash.empty()) {
        res.error = L"git commit-tree 没有返回新提交哈希";
        return res;
    }
    res.newHash = newHash;
    const RunResult d = step({L"rebase", L"--onto", newHash, plan.newestHash},
                             L"git rebase --onto " + newHash.substr(0, 12) + L"… " + plan.newestHash.substr(0, 12) + L"…");
    if (d.exitCode != 0) {
        res.error = L"git rebase --onto 失败（新提交 " + newHash.substr(0, 12) +
                    L" 已创建）：" + Trim(W(d.err)) + L"\n可尝试 git rebase --abort 回到原状";
        return res;
    }
    res.ok = true;
    return res;
}

}  // namespace grt
