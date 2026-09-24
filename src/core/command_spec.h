#pragma once
// ---------------------------------------------------------------------------
// 声明式命令表（《技术实现设计》§3.4 / §3.5）
//   · 菜单标题、可用性、flags 白名单、参数 schema、危险级别全部是数据
//   · GUI（参数面板）与 Shell DLL（菜单项）共用本表
// ---------------------------------------------------------------------------

#include "core.h"
#include "resource_ids.h"

namespace grt {

using CommandId = uint16_t;

enum class GroupId : uint8_t {
    Repo = 0, Commit, Sync, Branch, Inspect, Stash, Advanced, App, Count
};

// 适用场景位掩码
using SelMask = uint16_t;
constexpr SelMask kSelFile    = 0x0001;
constexpr SelMask kSelDir     = 0x0002;
constexpr SelMask kSelBg      = 0x0004;
constexpr SelMask kSelMulti   = 0x0008;
constexpr SelMask kSelAny     = 0xFFFF;

enum class Danger : uint8_t { Safe, Careful, Destructive };

enum class ParamKind : uint8_t {
    None, Text, CommitMessage, BranchName, ExistingBranch,
    RemoteName, Revision, TagName, Url, Pattern
};

enum class ParamSource : uint8_t {
    None, LocalBranches, RemoteBranches, AllBranches, Tags, Remotes, StashEntries
};

enum class ExecKind : uint8_t { Internal, Cli, CliPanel, CliStream };

enum class FlagKind : uint8_t { Toggle, Radio, Value, Dangerous };

struct FlagSpec {
    const char* key;         // 稳定 key（配置持久化、白名单校验）
    const char* gitArg;      // → argv；Value 型用 {v} 占位；nullptr = 纯 UI 开关
    FlagKind    kind;
    const char* radioGroup;  // Radio 互斥组名；非 Radio 为 nullptr
    uint16_t    labelRes;
    bool        defaultOn;
    bool        danger;      // 勾选后执行按钮需二次确认
};

struct CommandSpec {
    CommandId       id;
    const char*     key;          // "sync.pull"（CLI / 配置 / 日志）
    GroupId         group;
    uint16_t        titleRes;
    uint16_t        descRes;      // 0 = 无工具提示
    SelMask         applies;
    bool            requiresRepo;
    Danger          danger;
    ParamKind       param;
    ParamSource     paramSource;
    const char*     paramKey;     // 模板中 {key} 的名字；nullptr = 无参数
    ExecKind        exec;
    const FlagSpec* flags;
    uint8_t         flagCount;
    const char*     argvTemplate; // 单命令模板；分段用 " ; "（见 command_builder）
};

extern const CommandSpec kCommands[];
extern const size_t      kCommandCount;

// 访问器：kCommands 是不完整数组类型（无界），不能用于 range-for，
// 因此统一通过这两个函数遍历（见 gui/main.cpp、gui/app_window.cpp）
const CommandSpec* CommandTable();
size_t             CommandTableSize();

const CommandSpec* FindCommand(CommandId id);
const CommandSpec* FindCommandByKey(std::string_view key);
const wchar_t*     GroupName(GroupId g);
const char*        GroupKey(GroupId g);
const FlagSpec*    FindFlag(const CommandSpec& spec, std::string_view key);

}  // namespace grt
