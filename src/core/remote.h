#pragma once
// ---------------------------------------------------------------------------
// 远端跟踪 —— 《产品设计》新增需求
//
//   1) **远端基线**：所有提交以「远端分支的最新提交」为基，往上累加本地提交。
//      读出来就是一个基线视图：
//        · 上游分支名 / 上游指向的提交
//        · 领先（本地新增，远端还没有）/ 落后（远端新增，本地还没有）条数
//        · 哪些提交属于"本地新增"（`git rev-list HEAD --not <upstream>`）
//      纯只读，不改历史。
//   2) **远端分支列表**：`refs/remotes/**` 的清单（名称/短哈希/日期/是否当前上游），
//      供"远端分支…"面板做检出、新建跟踪分支、设为上游。
//
//   所有判定都在这里做，UI/CLI 只展示（与 squash 同一套路）。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "core.h"
#include "git_runner.h"

namespace grt {

// ------------------------------------------------------------------ 上游信息
struct RemoteInfo {
    bool         ok = false;          // 命令本身是否跑通
    std::wstring error;               // ok=false 的原因
    bool         hasUpstream = false; // 当前分支是否设置了上游
    std::wstring branch;              // 当前分支名（detached 时为 HEAD 短哈希）
    std::wstring upstream;            // 例如 origin/main
    std::wstring upstreamHash;        // 上游指向的提交（完整哈希）
    std::wstring upstreamShort;       // 上游短哈希
    std::wstring upstreamSubject;     // 上游那条提交的 subject
    size_t       ahead = 0;           // 本地新增（远端还没有）
    size_t       behind = 0;          // 远端新增（本地还没有）
    std::wstring remoteUrl;           // 上游对应远端的 URL（可能为空）

    bool upToDate() const { return hasUpstream && ahead == 0 && behind == 0; }
};

// 读当前分支的上游与领先/落后。仓库没有提交/没有上游时 hasUpstream=false，仍 ok=true。
RemoteInfo LoadRemoteInfo(const std::wstring& gitExe, const std::wstring& repoRoot);

// ------------------------------------------------------------------ 基线视图
struct BaselineCommit {
    std::wstring hash;
    std::wstring shortHash;
    std::wstring subject;
    std::wstring date;
    bool         localOnly = false;    // 本地新增（远端还没有）—— "以远端为基往上累加"的那部分
    bool         remoteOnly = false;   // 远端新增（本地还没有）—— 落后时需要看得见
};

struct RemoteBaseline {
    RemoteInfo                 info;
    // 展示顺序：**本地新增在上**（最新在前），**远端基线及以下在下**（最新在前）。
    // 这样"以远端提交为基、往上累加"在界面上就是从上往下的两段。
    std::vector<BaselineCommit> commits;
    size_t                     localCount = 0;      // 本地新增条数
    size_t                     baseCount = 0;       // 远端基线及以下条数
    size_t                     remoteOnlyCount = 0; // 其中"远端新增"条数（本地还没有）
};

// 供「提交历史」窗口用：读第一父链并逐条标注是不是"本地新增"
RemoteBaseline LoadRemoteBaseline(const std::wstring& gitExe, const std::wstring& repoRoot,
                                  int limit = 300);

// 把基线视图渲染成文本（历史窗口的头部/图例；无上游时如实说明）
std::wstring DescribeRemoteBaseline(const RemoteBaseline& base);

// "本地新增" 的提交（最新在前）：`git rev-list HEAD --not <upstream>`
std::vector<std::wstring> LocalOnlyCommits(const std::wstring& gitExe, const std::wstring& repoRoot,
                                           const RemoteInfo& info, size_t limit = 200);

// ------------------------------------------------------------------ 远端分支
struct RemoteBranch {
    std::wstring fullName;    // refs/remotes/origin/main
    std::wstring name;        // origin/main
    std::wstring shortHash;
    std::wstring date;        // yyyy-MM-dd
    std::wstring subject;
    bool         isUpstream = false;   // 是不是当前分支的上游
    bool         isHead = false;       // 远端 HEAD（origin/HEAD → origin/main）
};

std::vector<RemoteBranch> LoadRemoteBranches(const std::wstring& gitExe, const std::wstring& repoRoot);

// 远端分支的短列表（给远端分支面板做文本展示）
std::wstring DescribeRemoteBranches(const std::vector<RemoteBranch>& branches);

// ------------------------------------------------------------------ 上游设置
// 把当前分支的上游设成 upstreamName（例如 origin/main）。
// 返回空字符串表示成功，否则是给用户看的原因。
std::wstring SetUpstream(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const std::wstring& upstreamName);

// ------------------------------------------------------------------ 远端地址管理
struct RemoteEntry {
    std::wstring name;       // origin
    std::wstring fetchUrl;   // git remote get-url <name>
    std::wstring pushUrl;    // 单独设置了 pushurl 时才有
};

std::vector<RemoteEntry> LoadRemotes(const std::wstring& gitExe, const std::wstring& repoRoot);

// 校验远端名/地址（返回空 = 合法）
std::wstring ValidateRemoteName(const std::wstring& name);
std::wstring ValidateRemoteUrl(const std::wstring& url);

// 成功返回空字符串；失败返回可直接展示的原因
std::wstring AddRemote(const std::wstring& gitExe, const std::wstring& repoRoot,
                       const std::wstring& name, const std::wstring& url);
std::wstring SetRemoteUrl(const std::wstring& gitExe, const std::wstring& repoRoot,
                          const std::wstring& name, const std::wstring& url);
std::wstring RemoveRemote(const std::wstring& gitExe, const std::wstring& repoRoot,
                          const std::wstring& name);

// ------------------------------------------------------------------ 抓取
// `git fetch --prune`（可选指定远端）。返回空字符串表示成功。
std::wstring FetchRemote(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const std::wstring& remote = {});

}  // namespace grt
