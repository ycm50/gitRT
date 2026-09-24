// 菜单模型实现（《技术实现设计》§3.6 / §4.3 / §4.4）
#include "menu_model.h"

#include "config.h"
#include "clsid.h"
#include "probe_log.h"
#include "resource_ids.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace grt::shell {
namespace {

constexpr uint64_t kCacheRefreshMs = 5000;   // 与 ConfigStore 的读盘节流窗口一致

std::mutex                g_cacheMutex;
uint64_t                  g_cacheAt = 0;
std::shared_ptr<MenuNode> g_root;
MenuConfig                g_cfg;
bool                      g_gitAvailable = true;

std::mutex                                 g_strMutex;
std::unordered_map<UINT, std::wstring>     g_strCache;
HMODULE                                    g_self = nullptr;

std::string Lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool Contains(const std::vector<std::string>& v, const std::string& key) {
    const std::string k = Lower(key);
    for (const auto& x : v)
        if (Lower(x) == k) return true;
    return false;
}

bool IsGroupHidden(const MenuConfig& cfg, GroupId g) {
    return Contains(cfg.hiddenGroups, GroupKey(g));
}

bool IsCommandHidden(const MenuConfig& cfg, const CommandSpec& spec) {
    return Contains(cfg.hiddenCommands, spec.key);
}

// ---------------------------------------------------------------- 资源字符串
// 注意：DLL 里的 GetModuleHandleW(nullptr) 返回的是**宿主进程**（explorer.exe），
// 不是本 DLL。因此必须用 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 拿到自己的句柄。
HMODULE SelfModule() {
    if (!g_self) {
        HMODULE h = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&SelfModule), &h);
        g_self = h;
    }
    return g_self;
}

std::wstring LoadRes(UINT id) {
    if (!id) return {};
    {
        std::lock_guard lock(g_strMutex);
        const auto it = g_strCache.find(id);
        if (it != g_strCache.end()) return it->second;
    }
    wchar_t buf[512]{};
    const int n = ::LoadStringW(SelfModule(), id, buf, 511);
    std::wstring s = (n > 0) ? std::wstring(buf, static_cast<size_t>(n)) : std::wstring();
    if (s.empty()) GRT_LOGW("shell.menu", "资源字符串缺失 id=" << id);
    {
        std::lock_guard lock(g_strMutex);
        g_strCache[id] = s;
    }
    return s;
}

// ------------------------------------------------------------------ 树构造
std::shared_ptr<MenuNode> MakeCommand(const CommandSpec& spec) {
    auto n = std::make_shared<MenuNode>();
    n->kind = NodeKind::Command;
    n->cmdId = spec.id;
    n->spec = &spec;
    n->group = spec.group;
    n->titleRes = spec.titleRes;
    return n;
}

std::shared_ptr<MenuNode> MakeGroup(GroupId g, uint16_t titleRes) {
    auto n = std::make_shared<MenuNode>();
    n->kind = NodeKind::Group;
    n->group = g;
    n->titleRes = titleRes;
    return n;
}

std::shared_ptr<MenuNode> MakeSeparator() {
    auto n = std::make_shared<MenuNode>();
    n->kind = NodeKind::Separator;
    return n;
}

std::shared_ptr<MenuNode> MakeNotice(uint16_t titleRes) {
    auto n = std::make_shared<MenuNode>();
    n->kind = NodeKind::Notice;
    n->titleRes = titleRes;
    return n;
}

uint16_t GroupTitleRes(GroupId g) {
    switch (g) {
        case GroupId::Repo:     return IDS_GROUP_REPO;
        case GroupId::Commit:   return IDS_GROUP_COMMIT;
        case GroupId::Sync:     return IDS_GROUP_SYNC;
        case GroupId::Branch:   return IDS_GROUP_BRANCH;
        case GroupId::Inspect:  return IDS_GROUP_INSPECT;
        case GroupId::Stash:    return IDS_GROUP_STASH;
        case GroupId::Advanced: return IDS_GROUP_ADVANCED;
        default:                return IDS_GROUP_APP;
    }
}

// 哪些 flag 值得出现在菜单里（§4.3 层级 1）：
//   · Radio —— 互斥单选（拉取策略 / 重置模式 / 终端类型），必须可见
//   · Toggle / Dangerous（有 git 参数）—— 复选开关
//   · Value —— 需要文本输入，只在参数面板里出现
bool IsMenuFlag(const FlagSpec& f) {
    if (f.kind == FlagKind::Radio) return true;
    if (f.kind == FlagKind::Value) return false;
    return f.gitArg != nullptr;
}

// 16 项预算：超出的部分折叠进自动生成的「更多…」子菜单（§3.6）
std::vector<std::shared_ptr<MenuNode>> FoldToBudget(std::vector<std::shared_ptr<MenuNode>> items,
                                                   size_t budget, int depth) {
    if (items.size() <= budget || depth >= 3 || budget < 2) return items;
    std::vector<std::shared_ptr<MenuNode>> kept;
    kept.reserve(budget);
    for (size_t i = 0; i + 1 < budget && i < items.size(); ++i) kept.push_back(items[i]);
    std::vector<std::shared_ptr<MenuNode>> rest;
    for (size_t i = budget - 1; i < items.size(); ++i) rest.push_back(items[i]);
    auto more = std::make_shared<MenuNode>();
    more->kind = NodeKind::Group;
    more->titleRes = IDS_MENU_MORE;
    more->group = GroupId::Count;   // 非真实分组
    more->children = FoldToBudget(std::move(rest), budget, depth + 1);
    kept.push_back(std::move(more));
    GRT_LOGW("shell.menu", "菜单层超过 " << budget << " 项，已折叠进「更多…」");
    return kept;
}

std::shared_ptr<MenuNode> BuildTree(const MenuConfig& cfg) {
    GRT_PROBE_TIMER("BuildMenuTree");
    auto root = std::make_shared<MenuNode>();
    root->kind = NodeKind::Root;
    root->titleRes = IDS_MENU_ROOT;

    // ── 形态 ①（默认）：菜单里只有一个「GitRT」入口，点击打开主窗口 ──────────────
    // 产品决策：把"选什么命令"留给 GUI（那里有完整列表、参数面板、AI 助手），
    // 右键菜单只承担"把 GitRT 打开在当前位置"这一件事。
    if (cfg.mode == MenuMode::App) {
        GRT_LOGI("shell.menu", "菜单形态=app（单一入口，无子菜单）");
        return root;   // children 为空 → GetFlags 不含 HASSUBCOMMANDS、EnumSubCommands 返回 E_NOTIMPL
    }

    std::vector<std::shared_ptr<MenuNode>> top;
    // 提示项：默认隐藏，仅在"未找到 git"/"操作进行中"时由 GetState 显示
    top.push_back(MakeNotice(IDS_MENU_NO_GIT));
    top.push_back(MakeNotice(IDS_MENU_IN_PROGRESS));

    // ── 产品决策（2026-09-24，由产品负责人确认）───────────────────────────────
    // **菜单顶层只保留分组，分组就是 UI 入口**；不再默认放"高频直达"命令项。
    // 想要直达的用户可在 config.json 里设置 `menu.directTop`（非空即生效，并自动补分隔符）。
    if (!cfg.directTop.empty()) {
        for (const auto& key : cfg.directTop) {
            const CommandSpec* spec = FindCommandByKey(key);
            if (spec && !IsCommandHidden(cfg, *spec)) top.push_back(MakeCommand(*spec));
        }
        top.push_back(MakeSeparator());
    }

    // 分组（每组自成一级子菜单）
    for (int gi = 0; gi < static_cast<int>(GroupId::Count); ++gi) {
        const auto g = static_cast<GroupId>(gi);
        if (IsGroupHidden(cfg, g)) continue;
        auto grp = MakeGroup(g, GroupTitleRes(g));
        std::vector<std::shared_ptr<MenuNode>> flagNodes;
        for (size_t ci = 0; ci < CommandTableSize(); ++ci) {
            const CommandSpec& spec = CommandTable()[ci];
            if (spec.group != g) continue;
            if (IsCommandHidden(cfg, spec)) continue;
            grp->children.push_back(MakeCommand(spec));
            // ── 同上产品决策：**组内默认不放复选/单选开关**，只放可执行命令。
            //    开关属于"单次调用的参数"，放在 GUI 参数面板里；需要菜单内联开关的
            //    用户可设 `menu.showFlags=true`（§4.7 的可配置性保留）。
            if (!cfg.showFlags) continue;
            for (uint8_t fi = 0; fi < spec.flagCount; ++fi) {
                const FlagSpec& f = spec.flags[fi];
                if (!IsMenuFlag(f)) continue;
                auto fn = std::make_shared<MenuNode>();
                fn->kind = NodeKind::Flag;
                fn->cmdId = spec.id;
                fn->spec = &spec;
                fn->group = spec.group;
                fn->flag = &f;
                flagNodes.push_back(std::move(fn));
            }
        }
        if (grp->children.empty()) continue;
        if (!flagNodes.empty()) {
            grp->children.push_back(MakeSeparator());
            for (auto& fn : flagNodes) grp->children.push_back(std::move(fn));
        }
        grp->children = FoldToBudget(std::move(grp->children), kMaxItemsPerLevel, 0);
        top.push_back(std::move(grp));
    }

    root->children = FoldToBudget(std::move(top), kMaxItemsPerLevel, 0);
    return root;
}

void RefreshCaches() {
    g_cfg = LoadMenuConfig();
    g_root = BuildTree(g_cfg);
    g_gitAvailable = !FindGitExecutable().empty();
    g_cacheAt = ::GetTickCount64();
    GRT_LOGI("shell.menu", "菜单树已构建 顶层项=" << (g_root ? g_root->children.size() : 0)
                                                 << " git=" << g_gitAvailable);
}

void EnsureCaches() {
    const uint64_t now = ::GetTickCount64();
    if (g_root && now - g_cacheAt < kCacheRefreshMs) return;
    std::lock_guard lock(g_cacheMutex);
    if (g_root && ::GetTickCount64() - g_cacheAt < kCacheRefreshMs) return;
    RefreshCaches();
}

// ---------------------------------------------------------------- 可见性判定
bool IsOperationBlocker(const CommandSpec& spec) {
    // 仓库处于 merge/rebase/cherry-pick/revert/bisect 中时，这些命令会开启新的
    // 操作（git 自己也会拒绝），因此灰显而不是隐藏——用户能看到"为什么不行"。
    static const char* kBlocked[] = {"sync.pull", "sync.sync", "branch.merge", "branch.rebase",
                                     "branch.switch"};
    for (const char* k : kBlocked)
        if (std::string_view(spec.key) == k) return true;
    return false;
}

Visibility EvaluateCommand(const CommandSpec& spec, const SelectionContext& ctx,
                          const RepoSnapshot* snap) {
    Visibility v;
    // §4.4 最后一行：没有 git.exe 时"仅 1 项"（未找到 git 的提示项）
    if (!g_gitAvailable) {
        v.visible = false;
        return v;
    }
    // ★★ M0-B 实测（关键修正）：
    //   外壳对**二级/三级**菜单项经常传 `psiItemArray == nullptr`（它的顺序是
    //   先 EnumSubCommands 建树、再逐项 GetState(child, nullptr)），因此子项拿不到
    //   选区。若此时仍按"空选区"裁剪，整个子菜单会变成空白（最糟的体验）。
    //   处理原则与 §4.3 第 5 步"状态未知 → 乐观显示"一致：**选区未知 → 全量显示**；
    //   真正的闸门交给 GUI 参数面板（它知道仓库与路径，构建失败会禁用执行按钮）。
    const bool selectionKnown = ctx.attempted;
    if (selectionKnown) {
        if (ctx.nonFilesystem || (ctx.selMask & spec.applies) == 0) {
            v.visible = false;
            return v;
        }
        // §4.4 可见性矩阵：已经在仓库里就不再提供"初始化仓库"
        if (std::string_view(spec.key) == "repo.init" && ctx.IsRepo()) {
            v.visible = false;
            return v;
        }
        if (spec.requiresRepo && !ctx.IsRepo()) {
            v.visible = false;
            v.reasonRes = IDS_TIP_NEED_REPO;
            return v;
        }
    }
    if (IsCommandHidden(g_cfg, spec)) {
        v.visible = false;
        return v;
    }
    if (selectionKnown && ctx.probe.InProgress() && IsOperationBlocker(spec)) {
        v.enabled = false;
        v.reasonRes = IDS_MENU_IN_PROGRESS;
    }
    if (snap && snap->known) {
        // 快照可用时才做"内容相关"的裁剪；未知一律乐观显示（§4.3 第 5 步）
        if (std::string_view(spec.key) == "commit.unstage" && !snap->staged) {
            v.visible = false;
            return v;
        }
        if (std::string_view(spec.key) == "stash.pop" && !snap->stash) {
            v.enabled = false;
            v.reasonRes = IDS_MENU_STATE_UNKNOWN;
        }
        if (snap->computing && v.enabled) {
            v.reasonRes = IDS_MENU_STATE_UNKNOWN;
        }
    }
    return v;
}

// ---------------------------------------------------------------- 有效选区
// ★ M0-B 实测修正：外壳对子菜单项常传 `psiItemArray == nullptr`（attempted=false）。
//   此时用"进程内最近一次真实选区"（同一次菜单调用里外壳通常给根项传过）兜底；
//   连粘性选区也没有时，才退化为"选区未知"（由 EvaluateCommand 决定全量显示）。
struct EffectiveContext {
    std::shared_ptr<const SelectionContext> sticky;
    const SelectionContext*                 ctx = nullptr;
};

EffectiveContext MakeEffectiveContext(const SelectionContext& ctx) {
    EffectiveContext eff;
    eff.ctx = &ctx;
    if (!ctx.attempted) {
        eff.sticky = LastKnownSelection();
        if (eff.sticky) {
            eff.ctx = eff.sticky.get();
            GRT_LOGT("shell.sel", "子项未收到数组 → 使用粘性选区 selMask=" << eff.sticky->selMask
                                                                          << " paths=" << eff.sticky->paths.size());
        }
    }
    return eff;
}

}  // namespace

// ------------------------------------------------------------------ 配置
MenuConfig LoadMenuConfig() {
    auto& store = ConfigStore::Instance();
    store.Reload();
    MenuConfig cfg;
    cfg.valid = store.LastLoadOk();
    // 形态：默认 "app"（单一入口，打开主窗口）；"tree" 恢复分层菜单
    const std::wstring wmode = ToLowerAscii(Utf8ToWide(store.GetString("menu.mode", "app")));
    const std::string  mode = WideToUtf8(wmode);
    cfg.mode = (mode == "tree" || mode == "menu") ? MenuMode::Tree : MenuMode::App;
    // 默认**空**：顶层只有分组（分组即入口）。用户可自行配置直达项，例如：
    //   "menu.directTop": ["commit.commit", "sync.pull", "sync.push", "inspect.status"]
    cfg.directTop = store.GetStringArray("menu.directTop");
    // 默认**关**：开关（复选/单选）不上菜单，只在 GUI 参数面板里操作。
    cfg.showFlags = store.GetBool("menu.showFlags", false);
    cfg.hiddenGroups = store.GetStringArray("menu.hiddenGroups");
    cfg.hiddenCommands = store.GetStringArray("menu.hiddenCommands");
    return cfg;
}

const MenuConfig& MenuConfigCached() {
    EnsureCaches();
    return g_cfg;
}

const std::shared_ptr<MenuNode>& MenuRoot() {
    EnsureCaches();
    return g_root;
}

bool GitAvailable() {
    EnsureCaches();
    return g_gitAvailable;
}

void InvalidateMenuCaches() {
    std::lock_guard lock(g_cacheMutex);
    g_cacheAt = 0;
    g_root.reset();
    {
        std::lock_guard lockStr(g_strMutex);
        g_strCache.clear();
    }
}

// ---------------------------------------------------------------- 可见性 API
Visibility EvaluateNode(const MenuNode& node, const SelectionContext& ctxIn, const RepoSnapshot* snap) {
    EnsureCaches();   // 保证 g_cfg/g_gitAvailable 已就绪（带 5 s 节流）
    const EffectiveContext eff = MakeEffectiveContext(ctxIn);
    const SelectionContext& ctx = *eff.ctx;
    Visibility v;
    switch (node.kind) {
        case NodeKind::Root:
        case NodeKind::Separator:
            return v;   // 永远可见可用
        case NodeKind::Notice:
            if (node.titleRes == IDS_MENU_NO_GIT) {
                v.visible = !GitAvailable();
                v.enabled = v.visible;
            } else {
                // "操作进行中"提示只在**确知**进行中时显示（选区未知 → probe 为空 → 不显示）
                v.visible = ctx.attempted && ctx.probe.InProgress();
                v.enabled = false;
                v.reasonRes = IDS_MENU_IN_PROGRESS;
            }
            return v;
        case NodeKind::Group:
            v.visible = AnyChildVisible(node, ctx, snap);
            return v;
        case NodeKind::Flag: {
            if (!node.spec) {
                v.visible = false;
                return v;
            }
            const Visibility host = EvaluateCommand(*node.spec, ctx, snap);
            v.visible = host.visible;
            v.enabled = host.enabled;
            v.reasonRes = host.reasonRes;
            return v;
        }
        case NodeKind::Command:
            if (!node.spec) {
                v.visible = false;
                return v;
            }
            return EvaluateCommand(*node.spec, ctx, snap);
    }
    return v;
}

bool AnyChildVisible(const MenuNode& group, const SelectionContext& ctx, const RepoSnapshot* snap) {
    EnsureCaches();
    for (const auto& child : group.children) {
        if (!child) continue;
        const Visibility cv = EvaluateNode(*child, ctx, snap);
        if (cv.visible) return true;
    }
    return false;
}

// ------------------------------------------------------------------ 复选开关
bool FlagChecked(const CommandSpec& spec, const FlagSpec& flag) {
    auto& store = ConfigStore::Instance();
    store.Reload();
    return store.GetBool(FlagConfigKey(spec.key, flag.key), flag.defaultOn);
}

bool ToggleFlag(const CommandSpec& spec, const FlagSpec& flag) {
    const bool now = FlagChecked(spec, flag);
    auto& store = ConfigStore::Instance();
    store.Reload();
    store.SetBool(FlagConfigKey(spec.key, flag.key), !now);
    const bool ok = store.Save();
    GRT_LOGI("shell.menu", "复选开关切换 " << spec.key << "." << flag.key << " → " << (!now)
                                          << " saved=" << ok);
    return ok;
}

bool SelectRadioFlag(const CommandSpec& spec, const FlagSpec& flag) {
    auto& store = ConfigStore::Instance();
    store.Reload();
    for (uint8_t i = 0; i < spec.flagCount; ++i) {
        const FlagSpec& f = spec.flags[i];
        if (!f.radioGroup || !flag.radioGroup) continue;
        if (std::string_view(f.radioGroup) != std::string_view(flag.radioGroup)) continue;
        store.SetBool(FlagConfigKey(spec.key, f.key), std::string_view(f.key) == std::string_view(flag.key));
    }
    const bool ok = store.Save();
    GRT_LOGI("shell.menu", "单选开关切换 " << spec.key << "." << flag.key << " saved=" << ok);
    return ok;
}

// ------------------------------------------------------------------ 文本/图标
HMODULE SelfModuleHandle() { return SelfModule(); }

std::wstring LoadResString(UINT id) { return LoadRes(id); }

std::string NodeIdentityKey(const MenuNode& node) {
    switch (node.kind) {
        case NodeKind::Root:
            return "root";
        case NodeKind::Group:
            return node.group == GroupId::Count ? std::string("group.more")
                                                : "group." + std::string(GroupKey(node.group));
        case NodeKind::Command:
            return node.spec ? "cmd." + std::string(node.spec->key) : std::string("cmd.?");
        case NodeKind::Flag:
            return (node.spec && node.flag)
                       ? "flag." + std::string(node.spec->key) + "." + node.flag->key
                       : std::string("flag.?");
        case NodeKind::Notice:
            return "notice." + std::to_string(node.titleRes);
        case NodeKind::Separator:
            break;
    }
    return "separator";
}

GUID NodeCanonicalGuid(const MenuNode& node) {
    if (node.kind == NodeKind::Root) return kGuidGitRTCommandCanonical;   // 根保持既有规范名
    const std::string key = NodeIdentityKey(node);
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (const unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    GUID g{};
    g.Data1 = 0x4B525447;   // 'GRTK'
    g.Data2 = 0x0001;
    g.Data3 = 0x0001;
    for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>((h >> (8 * i)) & 0xFF);
    return g;
}

std::wstring NodeTitle(const MenuNode& node) {
    switch (node.kind) {
        case NodeKind::Root:
            return LoadRes(IDS_MENU_ROOT);
        case NodeKind::Group:
            return LoadRes(node.titleRes);
        case NodeKind::Notice:
            return LoadRes(node.titleRes);
        case NodeKind::Separator:
            return {};
        case NodeKind::Flag:
            if (!node.spec || !node.flag) return {};
            // 自解释：命令名 + 开关名（§4.3 层级 1）
            return LoadRes(node.spec->titleRes) + L" · " + LoadRes(node.flag->labelRes);
        case NodeKind::Command:
            return node.spec ? LoadRes(node.spec->titleRes) : std::wstring();
    }
    return {};
}

std::wstring NodeToolTip(const MenuNode& node, const SelectionContext& ctxIn) {
    const EffectiveContext eff = MakeEffectiveContext(ctxIn);
    const SelectionContext& ctx = *eff.ctx;
    switch (node.kind) {
        case NodeKind::Root:
            // App 形态（无子菜单）：这个入口就是"打开 GitRT"；git 缺失时提示直接说明原因
            if (node.children.empty()) {
                if (!GitAvailable()) return LoadRes(IDS_MENU_NO_GIT);
                return LoadRes(IDS_TIP_ROOT_APP);
            }
            return LoadRes(IDS_TIP_ROOT);
        case NodeKind::Group: return LoadRes(IDS_TIP_GROUP);
        case NodeKind::Flag:  return LoadRes(IDS_TIP_FLAG);
        case NodeKind::Notice:return LoadRes(node.titleRes);
        case NodeKind::Command: {
            if (!node.spec) return {};
            if (ctx.IsRepo()) return LoadRes(IDS_LABEL_REPO) + L"：" + ctx.probe.repoRoot;
            return LoadRes(IDS_TIP_NEED_REPO);
        }
        default: return {};
    }
}

}  // namespace grt::shell
