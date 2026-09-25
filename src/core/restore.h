#pragma once
// ---------------------------------------------------------------------------
// 按提交还原代码仓库 —— 《产品设计》新增需求
//
//   用户选了「只读检出到该提交」，但不排除后续要改/删后续提交、或从这里新建分支，
//   所以这里把三件事一次规划好，由界面给成几个单选项：
//
//     ① 只读检出（分离头指针）  git checkout --detach <提交>
//        工作区变成那条提交的样子，**不动任何分支**，随时切回来。
//     ② 从该提交新建分支      git checkout -b <新分支名> <提交>
//        以它为新起点继续干活，原分支不受影响。
//     ③ 把当前分支重置到该提交 git reset --soft|--mixed|--hard <提交>
//        这就是"删除后续提交记录"：soft 保留改动在暂存区、mixed 保留改动不暂存、
//        hard **丢弃**改动（危险，脏工作区时还要二次确认）。
//
//   校验/拒绝理由全在这里给（不是仓库 / 哈希非法 / 未知提交 / 有未完成的 rebase /
//   新分支名非法或已存在 / hard 重置弄脏改动），UI 与 CLI 直接展示。
// ---------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

#include "core.h"
#include "git_runner.h"

namespace grt {

enum class RestoreMode {
    DetachCheckout,   // 只读检出
    NewBranch,        // 新建分支
    ResetSoft,        // 重置：软（改动进暂存区）
    ResetMixed,       // 重置：混合（改动留在工作区）
    ResetHard,        // 重置：硬（丢弃改动）
};

std::wstring RestoreModeKey(RestoreMode m);          // detach | branch | soft | mixed | hard
bool         ParseRestoreMode(const std::wstring& s, RestoreMode* out);
std::wstring RestoreModeLabel(RestoreMode m);        // 中文说明（界面/日志用）

struct RestorePlan {
    bool                      ok = false;
    std::wstring              error;          // ok=false 时直接给用户看
    std::wstring              hash, shortHash, subject;   // 目标提交
    std::wstring              branch;         // 当前分支（detached 时为 HEAD 短哈希）
    RestoreMode               mode = RestoreMode::DetachCheckout;
    std::wstring              newBranchName;  // 仅 NewBranch
    bool                      detachedNow = false;   // 现在就已经是分离头指针
    bool                      dirty = false;         // 工作区有未提交改动
    size_t                    dropCount = 0;         // 会"被删掉"的后续提交数（重置模式）
    size_t                    droppedPushed = 0;     // 其中已推送到远端的条数（>0 会分叉）
    bool                      hasUpstream = false;
    std::wstring              upstream;
    bool                      needsExtraConfirm = false;   // hard + 脏工作区 → 再确认一次
    std::vector<std::wstring> warnings;       // 给用户看的提示（不算拒绝）
    std::vector<std::wstring> commandLines;   // 将执行/已执行的命令

    bool isReset() const {
        return mode == RestoreMode::ResetSoft || mode == RestoreMode::ResetMixed ||
               mode == RestoreMode::ResetHard;
    }
};

// hash 可以是完整/短哈希；newBranchName 仅 NewBranch 用。
// forceHard=true（CLI 的 --force）才能对"脏工作区"做 hard 重置。
RestorePlan BuildRestorePlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                             const std::wstring& hash, RestoreMode mode,
                             const std::wstring& newBranchName = {}, bool forceHard = false);

struct RestoreStep {
    std::wstring commandLine;
    int          exitCode = 0;
    std::wstring out, err;
};
struct RestoreResult {
    bool                    ok = false;
    std::wstring            error;
    std::vector<RestoreStep> steps;
};

RestoreResult ApplyRestore(const std::wstring& gitExe, const std::wstring& repoRoot,
                           const RestorePlan& plan,
                           const std::function<void(const std::wstring&)>& onCommand = {},
                           const std::function<void(const std::string&)>& onOutput = {});

}  // namespace grt
