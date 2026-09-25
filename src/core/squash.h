#pragma once
// ---------------------------------------------------------------------------
// 合并提交（squash）——《产品设计》新增需求
//
//   在「提交历史」里**复选连续的提交**，把它们合并成一条：
//     · 代码改动 = 所有选中提交的改动之和（等价于最新那条选中提交的 tree，
//       因为提交链上后面的 tree 已经包含前面全部改动）
//     · 提交信息 = 各选中提交 subject 的简单拼接（界面上可以改）
//
//   两种实现路径（由选中区间是否触到 HEAD 决定，BuildSquashPlan 自动选）：
//     A) 含 HEAD： `git reset --soft <最旧选中的父>` + `git commit -m <合并信息>`
//        索引不变（= 原 HEAD 的 tree），于是新提交的 tree 正好是合并后的改动。
//     B) 不含 HEAD：`git commit-tree <最新选中的 tree> -p <最旧选中的父> -m <信息>`
//        + `git rebase --onto <新提交> <最新选中>` 把上面的提交重放到新提交上。
//        因为新提交的 tree 与原最新选中的 tree 完全相同，重放**不会冲突**。
//
//   所有拒绝理由（不连续 / 含合并提交 / 触到根提交 / 暂存区脏 / 有未完成的
//   rebase 等）都由 BuildSquashPlan 一次性给出，UI 与 CLI 直接展示，不各自判断。
// ---------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

#include "core.h"
#include "git_runner.h"

namespace grt {

// ------------------------------------------------------------------ 提交条目
struct CommitEntry {
    std::wstring hash;        // 完整哈希
    std::wstring shortHash;   // 短哈希
    std::wstring author;
    std::wstring date;        // yyyy-MM-dd
    std::wstring subject;
    size_t       parentCount = 0;   // 真实父提交数（>1 = merge）
};

// 读提交列表（沿第一父链，最新在前）。失败返回空。
std::vector<CommitEntry> LoadCommitList(const std::wstring& gitExe, const std::wstring& repoRoot,
                                        int limit = 500);

// ------------------------------------------------------------------ 合并计划
struct SquashPlan {
    bool                      ok = false;
    std::wstring              error;         // ok=false 时直接给用户看
    std::vector<CommitEntry>  commits;       // 选中的提交，**从旧到新**
    std::wstring              message;       // 合并后的提交信息
    std::wstring              mode;          // "reset-soft" | "commit-tree+rebase"
    std::wstring              oldestParent;  // 新提交的父
    std::wstring              newestHash, newestTree, oldestHash;
    size_t                    headIndex = 0; // 最新选中提交在第一父链上的序号（0 = HEAD）
    std::vector<std::wstring> commandLines;  // 将执行/已执行的命令行（展示与日志）

    bool includesHead() const { return headIndex == 0; }
};

// hashes：选中提交（顺序不重要，内部按历史顺序归一）。
// messageOverride 非空则用它作为合并后的提交信息。
SquashPlan BuildSquashPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                           const std::vector<std::wstring>& hashes,
                           const std::wstring& messageOverride = {});

// ------------------------------------------------------------------ 执行
struct SquashStep {
    std::wstring commandLine;
    int          exitCode = 0;
    std::wstring out, err;
};
struct SquashResult {
    bool                      ok = false;
    std::wstring              error;
    std::wstring              newHash;
    std::vector<SquashStep>   steps;
};

// onCommand：每条命令开跑前回调（UI 用它把 "> git …" 写进日志）
// onOutput ：每条命令结束后回调该命令的原始输出（可为空）
SquashResult ApplySquash(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const SquashPlan& plan, const std::function<void(const std::wstring&)>& onCommand = {},
                         const std::function<void(const std::string&)>& onOutput = {});

// 把一组 subject 拼成"简单合并"的提交信息（从旧到新，每个一行）
std::wstring JoinSubjects(const std::vector<CommitEntry>& oldestToNewest);

}  // namespace grt
