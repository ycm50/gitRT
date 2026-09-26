// 按提交还原代码仓库实现 —— 见 restore.h 顶部说明
#include "restore.h"

#include <algorithm>

#include "command_builder.h"
#include "core.h"

namespace grt {

namespace {

bool IsHexHash(std::wstring_view s) {
    if (s.size() < 4 || s.size() > 40) return false;
    for (const wchar_t c : s) {
        const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        if (!hex) return false;
    }
    return true;
}

// 未完成的 rebase / merge / cherry-pick：任何还原动作都别叠在这些状态上
std::wstring PendingOperation(const std::wstring& gitExe, const std::wstring& repoRoot) {
    int rc = 0;
    const std::wstring gitDir = Trim(RunGitOut(gitExe, repoRoot, {L"rev-parse", L"--absolute-git-dir"}, &rc));
    if (rc != 0 || gitDir.empty()) return {};
    for (const wchar_t* name : {L"rebase-merge", L"rebase-apply", L"MERGE_HEAD", L"CHERRY_PICK_HEAD",
                                L"REVERT_HEAD"}) {
        if (PathExists(gitDir + L"\\" + name)) return name;
    }
    return {};
}

std::wstring QuoteCmdArgs(const std::vector<std::wstring>& argv) {
    std::wstring s = L"git";
    for (const auto& a : argv) {
        s += L' ';
        s += QuoteArg(a);
    }
    return s;
}

}  // namespace

std::wstring RestoreModeKey(RestoreMode m) {
    switch (m) {
        case RestoreMode::DetachCheckout: return L"detach";
        case RestoreMode::NewBranch:      return L"branch";
        case RestoreMode::ResetSoft:      return L"soft";
        case RestoreMode::ResetMixed:     return L"mixed";
        case RestoreMode::ResetHard:      return L"hard";
    }
    return L"detach";
}

bool ParseRestoreMode(const std::wstring& s, RestoreMode* out) {
    const std::wstring k = ToLowerAscii(Trim(s));
    if (k == L"detach" || k == L"checkout") { if (out) *out = RestoreMode::DetachCheckout; return true; }
    if (k == L"branch" || k == L"newbranch") { if (out) *out = RestoreMode::NewBranch; return true; }
    if (k == L"soft") { if (out) *out = RestoreMode::ResetSoft; return true; }
    if (k == L"mixed") { if (out) *out = RestoreMode::ResetMixed; return true; }
    if (k == L"hard") { if (out) *out = RestoreMode::ResetHard; return true; }
    return false;
}

std::wstring RestoreModeLabel(RestoreMode m) {
    switch (m) {
        case RestoreMode::DetachCheckout: return L"只读检出（分离头指针，不动分支）";
        case RestoreMode::NewBranch:      return L"从该提交新建分支";
        case RestoreMode::ResetSoft:      return L"重置当前分支（软：改动进暂存区）";
        case RestoreMode::ResetMixed:     return L"重置当前分支（混合：改动留在工作区）";
        case RestoreMode::ResetHard:      return L"重置当前分支（硬：丢弃后续改动）";
    }
    return L"只读检出";
}

RestorePlan BuildRestorePlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                             const std::wstring& hash, RestoreMode mode,
                             const std::wstring& newBranchName, bool forceHard) {
    RestorePlan plan;
    plan.mode = mode;
    plan.newBranchName = Trim(newBranchName);

    if (gitExe.empty() || repoRoot.empty()) {
        plan.error = L"没有可用的 git 或仓库";
        return plan;
    }
    const std::wstring want = Trim(hash);
    if (!IsHexHash(want)) {
        plan.error = L"提交哈希不合法（只接受 4~40 位十六进制）：" + want;
        return plan;
    }

    // 基本仓库信息
    int rc = 0;
    std::wstring err;
    plan.branch = Trim(RunGitOut(gitExe, repoRoot, {L"rev-parse", L"--abbrev-ref", L"HEAD"}, &rc, &err));
    if (rc != 0) {
        plan.error = err.empty() ? L"不是 git 仓库，或仓库里还没有提交" : err;
        return plan;
    }
    plan.detachedNow = (plan.branch == L"HEAD");

    if (const std::wstring pending = PendingOperation(gitExe, repoRoot); !pending.empty()) {
        plan.error = L"仓库里有未完成的操作（" + pending + L"），请先处理完（提交/中止）再还原";
        return plan;
    }

    // 目标提交必须真实存在且是提交对象
    const std::wstring full = Trim(RunGitOut(gitExe, repoRoot,
                                          {L"rev-parse", L"--verify", L"--quiet", want + L"^{commit}"},
                                          &rc));
    if (rc != 0 || full.empty()) {
        plan.error = L"找不到这个提交：" + want;
        return plan;
    }
    plan.hash = full;
    plan.shortHash = RevParseShort(gitExe, repoRoot, full);
    plan.subject = Trim(RunGitOut(gitExe, repoRoot, {L"log", L"-1", L"--format=%s", full}));

    // 上游（用于"被丢掉的提交是否已经推送过"的提示）
    int upRc = 0;
    plan.upstream = Trim(RunGitOut(gitExe, repoRoot,
                                {L"rev-parse", L"--abbrev-ref", L"--symbolic-full-name", L"@{upstream}"},
                                &upRc));
    plan.hasUpstream = (upRc == 0 && !plan.upstream.empty());

    // 工作区是否脏（有未提交改动）
    {
        int dRc = 0;
        const std::wstring st = Trim(RunGitOut(gitExe, repoRoot, {L"status", L"--porcelain"}, &dRc));
        plan.dirty = (dRc == 0 && !st.empty());
    }

    // 会被"删掉"的后续提交（仅重置模式有意义）
    if (plan.isReset()) {
        const std::wstring list = RunGitOut(gitExe, repoRoot,
                                         {L"rev-list", full + L"..HEAD"});
        size_t n = 0;
        for (const wchar_t c : list) {
            if (c == L'\n') ++n;
        }
        if (!list.empty() && list.back() != L'\n') ++n;
        plan.dropCount = n;
        if (plan.hasUpstream && plan.dropCount > 0) {
            const std::wstring pushed = RunGitOut(gitExe, repoRoot,
                                               {L"rev-list", full + L"..HEAD", L"--not", plan.upstream});
            size_t np = 0;
            for (const wchar_t c : pushed) {
                if (c == L'\n') ++np;
            }
            if (!pushed.empty() && pushed.back() != L'\n') ++np;
            plan.droppedPushed = (np <= plan.dropCount) ? plan.dropCount - np : 0;
        }
    }

    // 模式相关的校验与提示
    switch (mode) {
        case RestoreMode::DetachCheckout:
            if (!plan.detachedNow) {
                plan.warnings.push_back(L"检出后处于「分离头指针」状态（不属于任何分支），随时可以切回分支");
            }
            if (plan.dirty) plan.warnings.push_back(L"工作区有未提交改动：能保留的会跟着过去，冲突时 git 会拒绝");
            break;
        case RestoreMode::NewBranch: {
            if (plan.newBranchName.empty()) {
                plan.error = L"请填写新分支名（默认可以用 restore-" + plan.shortHash + L"）";
                return plan;
            }
            if (!IsValidBranchName(plan.newBranchName)) {
                plan.error = L"分支名不合法：" + plan.newBranchName;
                return plan;
            }
            int eRc = 0;
            RunGitOut(gitExe, repoRoot, {L"rev-parse", L"--verify", L"--quiet",
                                      L"refs/heads/" + plan.newBranchName}, &eRc);
            if (eRc == 0) {
                plan.error = L"分支已存在：" + plan.newBranchName;
                return plan;
            }
            if (plan.dirty) plan.warnings.push_back(L"工作区有未提交改动：能保留的会跟着过去");
            break;
        }
        case RestoreMode::ResetSoft:
        case RestoreMode::ResetMixed:
            if (plan.dropCount == 0) {
                plan.warnings.push_back(L"目标提交就是当前 HEAD，这条命令不会改变历史");
            }
            if (plan.droppedPushed > 0) {
                plan.warnings.push_back(L"被丢弃的 " + std::to_wstring(plan.droppedPushed) +
                                        L" 条提交已经在远端上：本地会与远端分叉（需要时用推送 --force-with-lease）");
            }
            break;
        case RestoreMode::ResetHard:
            if (plan.dropCount == 0) {
                plan.warnings.push_back(L"目标提交就是当前 HEAD，只会把工作区强行对齐到它");
            }
            if (plan.droppedPushed > 0) {
                plan.warnings.push_back(L"被丢弃的 " + std::to_wstring(plan.droppedPushed) +
                                        L" 条提交已经在远端上：本地会与远端分叉");
            }
            if (plan.dirty) {
                plan.warnings.push_back(L"硬重置会**丢弃**工作区/暂存区里所有未提交的改动（不可撤销）");
                if (!forceHard) plan.needsExtraConfirm = true;
            }
            if (!plan.dirty) plan.warnings.push_back(L"硬重置会把工作区强行对齐到该提交（未跟踪文件保留）");
            break;
    }

    // 命令行
    switch (mode) {
        case RestoreMode::DetachCheckout:
            plan.commandLines.push_back(QuoteCmdArgs({L"checkout", L"--detach", plan.hash}));
            break;
        case RestoreMode::NewBranch:
            plan.commandLines.push_back(QuoteCmdArgs({L"checkout", L"-b", plan.newBranchName, plan.hash}));
            break;
        case RestoreMode::ResetSoft:
            plan.commandLines.push_back(QuoteCmdArgs({L"reset", L"--soft", plan.hash}));
            break;
        case RestoreMode::ResetMixed:
            plan.commandLines.push_back(QuoteCmdArgs({L"reset", L"--mixed", plan.hash}));
            break;
        case RestoreMode::ResetHard:
            plan.commandLines.push_back(QuoteCmdArgs({L"reset", L"--hard", plan.hash}));
            break;
    }
    plan.ok = true;
    return plan;
}

RestoreResult ApplyRestore(const std::wstring& gitExe, const std::wstring& repoRoot,
                           const RestorePlan& plan,
                           const std::function<void(const std::wstring&)>& onCommand,
                           const std::function<void(const std::string&)>& onOutput) {
    RestoreResult res;
    if (!plan.ok) {
        res.error = plan.error.empty() ? L"还原计划不可执行" : plan.error;
        return res;
    }
    std::vector<std::wstring> argv;
    switch (plan.mode) {
        case RestoreMode::DetachCheckout: argv = {L"checkout", L"--detach", plan.hash}; break;
        case RestoreMode::NewBranch:      argv = {L"checkout", L"-b", plan.newBranchName, plan.hash}; break;
        case RestoreMode::ResetSoft:      argv = {L"reset", L"--soft", plan.hash}; break;
        case RestoreMode::ResetMixed:     argv = {L"reset", L"--mixed", plan.hash}; break;
        case RestoreMode::ResetHard:      argv = {L"reset", L"--hard", plan.hash}; break;
    }
    const std::wstring line = QuoteCmdArgs(argv);
    if (onCommand) onCommand(L"> " + line);
    const RunResult r = RunGitSync(gitExe, argv, repoRoot, 120000);
    RestoreStep step;
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
        res.error = step.err.empty() ? (L"命令失败（exit=" + std::to_wstring(r.exitCode) + L"）") : step.err;
        return res;
    }
    res.ok = true;
    return res;
}

}  // namespace grt
