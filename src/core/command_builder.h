#pragma once
// ---------------------------------------------------------------------------
// 命令构造：CommandSpec + 用户输入 → 可执行的 argv（《技术实现设计》§7.5）
//   · 唯一入口：所有 git 调用都必须经过这里（注入防护）
//   · 白名单：flags 只接受 FlagSpec 表内的 key
//   · 任何情况下都不拼接 shell 字符串，只产出 argv 数组
// ---------------------------------------------------------------------------

#include <map>

#include "command_spec.h"
#include "core.h"

namespace grt {

struct BuildInput {
    const CommandSpec* spec = nullptr;
    std::wstring       gitExe;                                 // git.exe 绝对路径
    std::wstring       repoRoot;                               // 可为空（非仓库命令）
    std::wstring       cwd;                                    // 空 = 用 repoRoot
    std::vector<std::wstring> paths;                           // 已规范化
    std::map<std::string, std::wstring> flags;                 // key → 值（Toggle 用 "1"/"0"）
    std::map<std::string, std::wstring> params;                // paramKey → 值
};

struct BuiltCommand {
    std::vector<std::vector<std::wstring>> argvList;  // 顺序执行的多条命令（不含 exe）
    std::wstring                           cwd;
    std::wstring                           display;   // 等价命令行（多行）
    std::vector<std::wstring>              notes;     // 给用户看的提示（如"将先预览"）
};

struct BuildError {
    uint32_t     code = 0;   // 0 = 无错误
    std::wstring message;
    const char*  field = nullptr;
};

// 返回值：true = 成功；false 时 err->message 已填好（中文，直接可显示）
bool BuildCommand(const BuildInput& in, BuiltCommand* out, BuildError* err);

// 供 UI 展示"将要执行什么"（不做校验，宽容失败）
std::wstring BuildDisplayText(const BuiltCommand& cmd);
std::wstring QuoteArg(const std::wstring& arg);

// 参数校验（供参数面板做实时校验）
bool IsValidBranchName(std::wstring_view s);
bool IsValidRemoteName(std::wstring_view s);
bool IsValidRevision(std::wstring_view s);
bool IsValidUrl(std::wstring_view s);
bool HasSelectedPaths(const BuildInput& in);

}  // namespace grt
