#pragma once
// ---------------------------------------------------------------------------
// 标签（tag）—— 标签/发布阶段的核心
//
//   本文件只管"标签"这一层：
//     · 读：`git for-each-ref refs/tags` 的清单（轻量/附注、本地/远端）
//     · 计划：新建 / 推送 / 删除 三种动作都先在这里校验并生成命令行
//     · 执行：ApplyTagPlan 逐条跑 git，UI/CLI 只展示（与 squash/restore 同一套路）
//
//   ★ 分隔符必须用 %09(Tab)：for-each-ref 的 format **不支持** %x1f
//     （会原样输出字面量 "%x1f"，remote.cpp 里已踩过一次，见那里的注释）。
//     subject 里还可能再有 Tab，所以只取前 N 个字段、其余拼回 subject。
//
//   写操作分危险级别：
//     tag.create / tag.push = Careful（改了仓库或远端）
//     tag.delete            = Destructive（tagPlan.destructive = true，CLI 必须 --force）
// ---------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

#include "core.h"
#include "git_runner.h"

namespace grt {

// ------------------------------------------------------------------ 标签清单
struct TagInfo {
    std::wstring name;             // v1.0（refs/tags/ 之后的部分）
    std::wstring hash;             // 标签指向的提交（附注标签已剥到提交）
    std::wstring shortHash;
    std::wstring subject;          // 提交标题
    std::wstring date;             // yyyy-MM-dd（打标签的时间）
    bool         annotated = false;   // 附注标签（有 tag 对象）vs 轻量标签
    bool         onRemote = false;    // origin 上也有同名标签
};

// 读本地标签清单。
// ★ 想"不连远端"必须传 **L"-"**：传空串会被 PickRemoteName() 当成 origin
//   （踩过：开窗口时想省掉网络调用而传了空串/默认值，结果照样 `ls-remote`，开窗卡 6 秒）。
// `git ls-remote --tags <remote>` 失败（离线/没远端）**不算致命**：
// 此时 onRemote 全 false，函数仍返回 true。
bool LoadTags(const std::wstring& gitExe, const std::wstring& repoRoot, std::vector<TagInfo>* out,
              std::wstring* error, const std::wstring& remoteName = L"origin");

// 给面板用的多行中文文本（标出「附注/轻量」「本地/远端」）
std::wstring DescribeTags(const std::vector<TagInfo>& tags);

// ------------------------------------------------------------------ 计划
struct TagPlan {
    bool                      ok = false;
    std::wstring              error;          // ok=false 时直接给用户看（中文）
    std::wstring              name;           // 标签名
    std::wstring              target;         // 指向的提交（完整哈希；空 target 时 = HEAD）
    std::wstring              message;        // 附注标签的信息
    bool                      annotated = false;
    bool                      force = false;
    bool                      forceNeeded = false;   // 同名已存在，必须 force 才能覆盖
    const wchar_t*            op = L"create";        // create | push | delete（给界面区分用）
    bool                      destructive = false;   // 破坏性动作（删除）
    std::vector<std::wstring> commandLines;   // 将执行/已执行的命令（"git …"，展示用）
    std::vector<std::vector<std::wstring>> argvList;   // 与 commandLines 一一对应的 argv（执行用）
    std::vector<std::wstring> warnings;       // 给用户看的提示（不算拒绝）
};

// 新建标签：轻量 `git tag <name> <target>`；附注 `git tag -a <name> -m <message> <target>`
TagPlan BuildTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                     const std::wstring& name, const std::wstring& message, bool annotated,
                     const std::wstring& target = {}, bool force = false);

// 推送标签：`git push <remote> <name>` / `git push <remote> --tags`
TagPlan BuildPushTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const std::wstring& name, bool all, const std::wstring& remote = {});

// 删除标签：本地 `git tag -d <name>`；远端 `git push <remoteName> :refs/tags/<name>`
// 两者都要就给两条命令。destructive = true。
TagPlan BuildDeleteTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                           const std::wstring& name, bool remote, const std::wstring& remoteName = {});

// ------------------------------------------------------------------ 执行
struct TagStep {
    std::wstring commandLine;
    int          exitCode = 0;
    std::wstring out, err;
};
struct TagResult {
    bool                 ok = false;
    std::wstring         error;
    std::vector<TagStep> steps;
};

// 逐条执行计划里的命令（用 RunGitSync）；任一条失败即停。
// onCommand 收到 "> git …"，onOutput 收到 git 的原始输出（与 ApplyRestore 一致）。
TagResult ApplyTagPlan(const std::wstring& gitExe, const std::wstring& repoRoot, const TagPlan& plan,
                       const std::function<void(const std::wstring&)>& onCommand = {},
                       const std::function<void(const std::string&)>& onOutput = {});

}  // namespace grt
