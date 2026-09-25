#pragma once
// ---------------------------------------------------------------------------
// 发布（GitHub Release）—— 标签/发布阶段
//
//   GitHub Release 不是 git 的东西：它由 **GitHub CLI（gh.exe）** 创建。
//   因此这里比 tag.* 多一层"能力探测"：
//     · DetectGh()：PATH 上找得到 gh.exe 吗（找不到 → 中文提示怎么装）
//     · LoadReleases()：`gh release list --json …` 的极简手写解析（不引新依赖）
//     · BuildReleasePlan()：gh 不可用 / 标签不存在 / 附件不存在 → 一律拒绝
//
//   危险级别：**Careful（会公开）**，不是 Destructive —— 它不改历史、不删东西，
//   但创建出来全世界都能看到，所以界面/CLI 都要先给用户看命令。
// ---------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

#include "core.h"
#include "git_runner.h"

namespace grt {

// ------------------------------------------------------------------ gh 能力
struct GhInfo {
    bool         available = false;
    std::wstring exe;        // 找到的 gh.exe 绝对路径
    std::wstring version;    // `gh --version` 第一行
    std::wstring error;      // 不可用时的中文原因（可直接展示）
};

GhInfo DetectGh();

// 跑任意可执行文件（gh 不是 git，所以不能走 RunGitSync）。照 RunGitSync 抄，
// exe 可变：成功/失败都通过 RunResult 返回，不抛异常。
RunResult RunProcessSync(const std::wstring& exe, const std::vector<std::wstring>& argv,
                         const std::wstring& cwd, uint32_t timeoutMs, std::wstring* out = nullptr,
                         std::wstring* err = nullptr);

// ------------------------------------------------------------------ 发布清单
struct ReleaseEntry {
    std::wstring tag;
    std::wstring name;         // Release 标题
    std::wstring publishedAt;
    bool         draft = false;
    bool         prerelease = false;
};

// gh 不可用 / 未登录 / 不是 GitHub 仓库 → 空列表 + 中文 error，**绝不崩**。
bool LoadReleases(const std::wstring& gitExe, const std::wstring& repoRoot,
                  std::vector<ReleaseEntry>* out, std::wstring* error);

// ------------------------------------------------------------------ 计划
struct ReleasePlan {
    bool                      ok = false;
    std::wstring              error;
    std::wstring              tag, title, notes;
    bool                      draft = false;
    bool                      prerelease = false;
    bool                      pushTag = false;
    bool                      generateNotes = false;
    bool                      destructive = false;   // 恒 false（发布是 Careful，不是 Destructive）
    std::vector<std::wstring> assets;          // 附件（已确认存在）
    std::vector<std::wstring> commandLines;    // 展示用
    std::vector<std::vector<std::wstring>> argvList;   // 执行用（与 commandLines 一一对应）
    std::vector<std::wstring> warnings;
};

ReleasePlan BuildReleasePlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                             const std::wstring& tag, const std::wstring& title,
                             const std::wstring& notes, bool draft, bool prerelease, bool pushTag,
                             const std::vector<std::wstring>& assets, bool generateNotes);

// ------------------------------------------------------------------ 执行
struct ReleaseStep {
    std::wstring commandLine;
    int          exitCode = 0;
    std::wstring out, err;
};
struct ReleaseResult {
    bool                     ok = false;
    std::wstring             error;
    std::vector<ReleaseStep> steps;
};

ReleaseResult ApplyReleasePlan(const std::wstring& repoRoot, const ReleasePlan& plan,
                               const std::function<void(const std::wstring&)>& onCommand = {},
                               const std::function<void(const std::string&)>& onOutput = {});

}  // namespace grt
