#pragma once
// ---------------------------------------------------------------------------
// 菜单模型（《技术实现设计》§3.6 / §4.3 / §4.4）
//
//   分层：L0 根 → L1（提示项 + 高频直达 + 8 个分组 + 溢出"更多…"）
//              → L2（该组的命令 + 分隔符 + 复选/单选开关）
//
//   为什么把"标题/可用性/复选状态"全部建模成数据 + 纯函数：
//   菜单构建期的四个方法必须 ≤ 10 ms 且零 git 调用，数据化之后可以做
//   完整的单元/自检验证（不需要 Explorer 参与）。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "command_spec.h"
#include "selection.h"
#include "snapshot_reader.h"

namespace grt::shell {

// 单个菜单层最多 16 项（非分隔项；§3.6 的社区实测结论）
constexpr size_t kMaxItemsPerLevel = 16;

enum class NodeKind : uint8_t { Root, Group, Command, Flag, Separator, Notice };

struct MenuNode {
    NodeKind                                   kind = NodeKind::Command;
    CommandId                                  cmdId = 0;      // Command / Flag 的宿主命令
    const CommandSpec*                         spec = nullptr;  // Command
    GroupId                                    group = GroupId::Repo;
    uint16_t                                   titleRes = 0;    // Group / Notice / Root
    const FlagSpec*                            flag = nullptr;  // Flag
    std::vector<std::shared_ptr<MenuNode>>     children;
};

// 菜单相关的配置子集（§12.1）
// 菜单形态（《产品设计》§4.1；由 config.json 的 `menu.mode` 选择）
//   App  —— **默认**：右键菜单里只有一个可点的「GitRT」入口，点击直接打开 GitRT 主窗口
//   Tree —— 分层菜单：GitRT → 8 个分组 → 命令（`menu.directTop` / `menu.showFlags` 生效）
// 两种形态共用同一套命令表与参数面板，切换不需要改代码。
enum class MenuMode { App, Tree };

struct MenuConfig {
    MenuMode                 mode = MenuMode::App;
    std::vector<std::string> directTop;       // 仅 Tree 模式：L1 置顶直达（cmdKey）
    std::vector<std::string> hiddenGroups;    // 仅 Tree 模式：整组隐藏（groupKey）
    std::vector<std::string> hiddenCommands;  // 单命令隐藏（cmdKey），两种模式都生效
    bool showFlags = false;                   // 仅 Tree 模式：把复选/单选开关铺进菜单
    bool                     valid = true;
};

MenuConfig LoadMenuConfig();
const MenuConfig& MenuConfigCached();

// 根节点（进程内缓存；配置变化后由 CachedRefreshMs 的节流窗口收敛）
const std::shared_ptr<MenuNode>& MenuRoot();

// 判定结果（供 GetState 直接映射为 EXPCMDSTATE）
struct Visibility {
    bool     visible = true;
    bool     enabled = true;
    uint16_t reasonRes = 0;   // 非 0 = 工具提示里说明"为什么灰显"
};

Visibility EvaluateNode(const MenuNode& node, const SelectionContext& ctx, const RepoSnapshot* snap);
bool       AnyChildVisible(const MenuNode& group, const SelectionContext& ctx, const RepoSnapshot* snap);

// 复选/单选开关的当前值（来自 config.json，见 §4.3 层级 1）
bool  FlagChecked(const CommandSpec& spec, const FlagSpec& flag);
// 点击开关：写配置并落盘（**不执行任何 Git 操作**）
bool  ToggleFlag(const CommandSpec& spec, const FlagSpec& flag);
bool  SelectRadioFlag(const CommandSpec& spec, const FlagSpec& flag);

// git.exe 是否可用（带 30 s 缓存；用于"未找到 git"时的单条提示菜单）
bool GitAvailable();

// 统一的菜单标题/工具提示解析（LoadStringW + 缓存；flag 节点由命令名 + 开关名合成）
std::wstring NodeTitle(const MenuNode& node);
std::wstring NodeToolTip(const MenuNode& node, const SelectionContext& ctx);

// 本 DLL 的 HMODULE（DLL 内的 GetModuleHandleW(nullptr) 返回宿主进程，不能用）
HMODULE      SelfModuleHandle();
// 本 DLL 的资源字符串（找不到时返回空串并记日志）
std::wstring LoadResString(UINT id);

// 菜单项的**稳定身份**（《技术实现设计》§4.2）
//   · IdentityKey：人类可读的稳定 key（"cmd.sync.pull" / "flag.sync.pull.rebase" / …）
//   · CanonicalGuid：由 IdentityKey 派生的稳定 GUID（跨运行、跨进程一致）
//   为什么需要：现代菜单里 `GetCanonicalName` 返回 NULL GUID 的**叶子项**会被丢弃，
//   而带子菜单的项不会（M0-B 实测：只见 8 个分组、组内为空）。因此每个可点项都必须
//   有自己的规范名；且绝不能用 CoCreateGuid（每次运行都不同 = 身份不稳定）。
std::string NodeIdentityKey(const MenuNode& node);
GUID        NodeCanonicalGuid(const MenuNode& node);

// 进程内缓存清理（--self-test / 探针工具用）
void InvalidateMenuCaches();

}  // namespace grt::shell
