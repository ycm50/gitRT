#pragma once
// ---------------------------------------------------------------------------
// git status --porcelain=v2 -z 解析（《技术实现设计》§7.6）
//
// 已用本机 git 2.53.0.windows.3 实测固化的三条规则：
//   ① header 行在 -z 模式下同样以 NUL 结尾（不是 LF）→ 可统一按 NUL 切分
//   ② type 2（重命名/复制）：path 之后紧跟一个额外的 NUL 分隔的 origPath
//      "2 R. N... ... R100 b.txt\0a.txt\0"
//   ③ header 是可选的：无 upstream 时没有 # branch.ab；无储藏时没有 # stash
//      → 缺行属于正常情况，绝不能因此判定解析失败
// ---------------------------------------------------------------------------

#include "core.h"

namespace grt {

struct StatusEntry {
    char        x = '.', y = '.';   // X = 暂存侧, Y = 工作区侧
    char        type = '1';         // '1' | '2' | 'u' | '?'
    std::string path;               // UTF-8 原始字节
    std::string origPath;           // 仅 type 2
};

struct RepoStatus {
    bool        parsed = false;
    std::string oid;
    std::string head;        // 分支名；分离头指针时为 "(detached)"
    std::string upstream;
    int         ahead = 0;
    int         behind = 0;
    int         stashCount = 0;
    bool        detached = false;
    std::vector<StatusEntry> entries;

    int staged = 0, modified = 0, untracked = 0, conflicted = 0;

    void Recount();
    bool IsClean() const { return staged == 0 && modified == 0 && untracked == 0 && conflicted == 0; }
};

RepoStatus ParsePorcelainV2(std::string_view bytes);

// 状态字符 → 简短中文/符号说明（供列表显示）
std::wstring DescribeEntry(const StatusEntry& e);

}  // namespace grt
