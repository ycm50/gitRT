// IExplorerCommand 实现（《技术实现设计》§4.2 – §4.5）
#include "explorer_command.h"

#include "broker_client.h"
#include "clsid.h"
#include "com_guard.h"
#include "config.h"
#include "enum_commands.h"
#include "probe_log.h"
#include "json_util.h"
#include "resource_ids.h"
#include "snapshot_reader.h"

#include <shlwapi.h>

namespace grt::shell {
namespace {

// 调用追踪（只在 GRT_SHELL_PROBE 构建里编译）：把资源管理器/外壳真实调用序列
// 写进 gitrt-YYYYMMDD.log，用于定位"菜单里少了东西"这类只能在实际宿主里复现的问题。
#if defined(GRT_SHELL_PROBE)
const char* KindName(NodeKind k) {
    switch (k) {
        case NodeKind::Root: return "Root";
        case NodeKind::Group: return "Group";
        case NodeKind::Command: return "Command";
        case NodeKind::Flag: return "Flag";
        case NodeKind::Separator: return "Separator";
        case NodeKind::Notice: return "Notice";
    }
    return "?";
}
#define GRT_SHELL_TRACE(expr) GRT_LOGT("shell.call", expr)
#else
#define GRT_SHELL_TRACE(expr) ((void)0)
#endif

void TraceCall(const char* method, const MenuNode& node) {
#if defined(GRT_SHELL_PROBE)
    GRT_LOGT("shell.call", method << " pid=" << ::GetCurrentProcessId() << " kind=" << KindName(node.kind)
                                 << " key=" << NodeIdentityKey(node) << " titleRes=" << node.titleRes
                                 << " children=" << node.children.size());
#else
    (void)method;
    (void)node;
#endif
}

// 把配置里的复选状态带上，交给 GUI 预填参数面板（§4.3 层级 2）
std::string BuildOptionsJson(const CommandSpec& spec, const SelectionContext& ctx) {
    auto& store = ConfigStore::Instance();
    store.Reload();
    std::string json = "{\"cmdKey\":\"" + std::string(spec.key) + "\"";
    if (ctx.IsRepo()) json += ",\"repoRoot\":\"" + JsonEscape(WideToUtf8(ctx.probe.repoRoot)) + "\"";
    if (ctx.selMask) json += ",\"selMask\":" + std::to_string(ctx.selMask);
    if (spec.flagCount) {
        json += ",\"flags\":{";
        for (uint8_t i = 0; i < spec.flagCount; ++i) {
            const FlagSpec& f = spec.flags[i];
            const bool on = store.GetBool(FlagConfigKey(spec.key, f.key), f.defaultOn);
            if (i) json += ",";
            // 值用 "1"/"0" 字符串：接收方（GUI）用 JsonFindStringMap 解析，避免布尔值分歧
            json += "\"" + std::string(f.key) + "\":\"" + (on ? "1" : "0") + "\"";
        }
        json += "}";
    }
    json += "}";
    return json;
}

}  // namespace

// ------------------------------------------------------------------ 创建
ExplorerCommand::ExplorerCommand(std::shared_ptr<MenuNode> node,
                                 std::shared_ptr<SelectionContext> ctx)
    : m_node(std::move(node)), m_ctx(std::move(ctx)) {
    if (!m_ctx) m_ctx = std::make_shared<SelectionContext>();
}

HRESULT ExplorerCommand::Create(std::shared_ptr<MenuNode> node, std::shared_ptr<SelectionContext> ctx,
                               IExplorerCommand** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        auto* obj = new (std::nothrow) ExplorerCommand(std::move(node), std::move(ctx));
        if (!obj) return E_OUTOFMEMORY;
        *out = static_cast<IExplorerCommand*>(obj);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

// ------------------------------------------------------------------ IUnknown
IFACEMETHODIMP ExplorerCommand::QueryInterface(REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIidIExplorerCommand) {
        *ppv = static_cast<IExplorerCommand*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) ExplorerCommand::AddRef() noexcept {
    return static_cast<ULONG>(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
}

IFACEMETHODIMP_(ULONG) ExplorerCommand::Release() noexcept {
    const LONG n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) {
        delete this;
        return 0;
    }
    return static_cast<ULONG>(n);
}

// ------------------------------------------------------------------ 上下文
const SelectionContext& ExplorerCommand::UpdateContext(IShellItemArray* items) noexcept {
    try {
        if (items && m_ctx && !m_ctx->attempted) ParseSelection(items, m_ctx.get());
    } catch (...) {
        // 解析失败绝不向外抛：菜单退化为"无选择上下文"（由 EvaluateNode 隐藏相关命令）
        GRT_LOGE("shell.com", "解析 IShellItemArray 失败，退化为空上下文");
    }
    return *m_ctx;
}

// ------------------------------------------------------------------ 标题/图标
IFACEMETHODIMP ExplorerCommand::GetTitle(IShellItemArray* items, LPWSTR* name) noexcept {
    GRT_COM_TRY
    TraceCall("GetTitle(in)", *m_node);
    if (!name) return E_POINTER;
    *name = nullptr;
    UpdateContext(items);
    const std::wstring title = NodeTitle(*m_node);
    // 分隔符没有标题，但 Shell 期望这里仍返回 S_OK（失败会导致整项不显示）
    const HRESULT hr = ::SHStrDupW(title.c_str(), name);
    GRT_SHELL_TRACE("GetTitle(out) key=" << NodeIdentityKey(*m_node) << " hr=0x" << std::hex
                                         << static_cast<unsigned long>(hr) << std::dec
                                         << " len=" << title.size());
    return hr;
    GRT_COM_CATCH
}

IFACEMETHODIMP ExplorerCommand::GetIcon(IShellItemArray* items, LPWSTR* icon) noexcept {
    GRT_COM_TRY
    if (!icon) return E_POINTER;
    *icon = nullptr;
    UpdateContext(items);
    if (m_node->kind == NodeKind::Separator) return S_FALSE;
    wchar_t dll[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(SelfModuleHandle(), dll, MAX_PATH);
    if (n == 0) return S_FALSE;
    const bool warn = (m_node->kind == NodeKind::Command && m_node->spec &&
                       m_node->spec->danger == Danger::Destructive);
    const std::wstring spec = std::wstring(dll, n) + L",-" + std::to_wstring(warn ? kIconWarn : kIconApp);
    return ::SHStrDupW(spec.c_str(), icon);
    GRT_COM_CATCH
}

IFACEMETHODIMP ExplorerCommand::GetToolTip(IShellItemArray* items, LPWSTR* tip) noexcept {
    GRT_COM_TRY
    if (!tip) return E_POINTER;
    *tip = nullptr;
    const auto& ctx = UpdateContext(items);
    const std::wstring text = NodeToolTip(*m_node, ctx);
    if (text.empty()) return S_FALSE;
    return ::SHStrDupW(text.c_str(), tip);
    GRT_COM_CATCH
}

IFACEMETHODIMP ExplorerCommand::GetCanonicalName(GUID* guid) noexcept {
    GRT_COM_TRY
    if (!guid) return E_POINTER;
    // ★ 每个菜单项都给**稳定且唯一**的规范名（不是 GUID_NULL）：
    //   现代菜单会丢弃规范名为空的叶子项（只在 L1 显示带子菜单的分组，组内空白）。
    *guid = NodeCanonicalGuid(*m_node);
    return S_OK;
    GRT_COM_CATCH
}

// ------------------------------------------------------------------ 状态位
IFACEMETHODIMP ExplorerCommand::GetState(IShellItemArray* items, WINBOOL okToBeSlow,
                                         EXPCMDSTATE* state) noexcept {
    (void)okToBeSlow;
    GRT_COM_TRY
    GRT_PROBE_TIMER("GetState");
    TraceCall("GetState(in)", *m_node);
    if (!state) return E_POINTER;
    *state = ECS_ENABLED;
    const auto& ctx = UpdateContext(items);

    RepoSnapshot snap{};
    const bool haveSnap = SnapshotReader::Instance().TryGet(ctx.probe.repoRoot, &snap);
    const Visibility v = EvaluateNode(*m_node, ctx, haveSnap ? &snap : nullptr);
    if (!v.visible) {
        *state = ECS_HIDDEN;
        GRT_SHELL_TRACE("GetState(out) key=" << NodeIdentityKey(*m_node) << " → HIDDEN selMask="
                                             << ctx.selMask << " attempted=" << ctx.attempted);
        return S_OK;
    }
    if (!v.enabled) {
        // 灰显而不是隐藏：用户能看到"为什么不行"（tooltip 给出原因）
        *state = ECS_DISABLED;
        GRT_SHELL_TRACE("GetState(out) key=" << NodeIdentityKey(*m_node) << " → DISABLED");
        return S_OK;
    }
    if (m_node->kind == NodeKind::Flag && m_node->spec && m_node->flag) {
        const bool on = FlagChecked(*m_node->spec, *m_node->flag);
        if (m_node->flag->kind == FlagKind::Radio)
            *state = static_cast<EXPCMDSTATE>(ECS_RADIOCHECK | (on ? ECS_CHECKED : 0));
        else
            *state = static_cast<EXPCMDSTATE>(ECS_CHECKBOX | (on ? ECS_CHECKED : 0));
    }
    GRT_SHELL_TRACE("GetState(out) key=" << NodeIdentityKey(*m_node) << " → " << *state);
    return S_OK;
    GRT_COM_CATCH
}

// ------------------------------------------------------------------ 执行
IFACEMETHODIMP ExplorerCommand::Invoke(IShellItemArray* items, IBindCtx* bc) noexcept {
    (void)bc;
    GRT_COM_TRY
    TraceCall("Invoke(in)", *m_node);
    const auto& ctx = UpdateContext(items);

    auto forward = [&](CommandId id, const char* key, bool withFlags) {
        InvokeRequest req;
        req.cmdId = id;
        req.selMask = static_cast<uint16_t>(ctx.selMask);
        req.paths = ctx.paths;
        req.cwd = ctx.commonRoot;
        if (withFlags) {
            req.optionsJson = BuildOptionsJson(*m_node->spec, ctx);
        } else {
            // cmdId=0 / 首选项：只需 key 与仓库提示
            req.optionsJson = std::string("{\"cmdKey\":\"") + key + "\"";
            if (ctx.IsRepo())
                req.optionsJson += ",\"repoRoot\":\"" + JsonEscape(WideToUtf8(ctx.probe.repoRoot)) + "\"";
            req.optionsJson += "}";
        }
        std::wstring err;
        if (!ForwardInvoke(req, &err)) {
            // §4.5 的最后一道降级：最小 MessageBox 说明，绝不静默失败
            ::MessageBoxW(nullptr, err.c_str(), L"GitRT", MB_ICONERROR | MB_OK);
        }
    };

    switch (m_node->kind) {
        case NodeKind::Root:
            // ── 模式 App（默认，菜单里只有这一个入口）：点击 = 打开 GitRT 主窗口 ──
            // git 缺失时进"首选项"（那里能设置路径），否则只把窗口打开在当前位置。
            if (m_node->children.empty()) {
                if (GitAvailable()) {
                    GRT_LOGI("shell.invoke", "菜单入口：打开 GitRT 主窗口 paths=" << ctx.paths.size());
                    forward(0, "app.main", false);
                } else {
                    forward(1701, "app.settings", false);
                }
            }
            return S_OK;   // Tree 模式的根只有子菜单，没有动作
        case NodeKind::Group:
        case NodeKind::Separator:
            return S_OK;   // 只有子菜单，没有动作
        case NodeKind::Notice: {
            if (m_node->titleRes != IDS_MENU_NO_GIT) return S_OK;
            // "未找到 git"：把用户送到首选项（那里能设置 git 路径）
            forward(1701, "app.settings", false);
            return S_OK;
        }
        case NodeKind::Flag: {
            if (!m_node->spec || !m_node->flag) return S_OK;
            // 粘性开关：只切换配置并落盘，**不执行任何 Git 操作**（§4.3 层级 1）
            if (m_node->flag->kind == FlagKind::Radio)
                SelectRadioFlag(*m_node->spec, *m_node->flag);
            else
                ToggleFlag(*m_node->spec, *m_node->flag);
            return S_OK;
        }
        case NodeKind::Command: {
            if (!m_node->spec) return S_OK;
            GRT_LOGI("shell.invoke", "菜单命令 key=" << m_node->spec->key << " paths=" << ctx.paths.size()
                                                      << " selMask=" << ctx.selMask);
            forward(m_node->spec->id, m_node->spec->key, true);
            return S_OK;
        }
    }
    return S_OK;
    GRT_COM_CATCH
}

// ------------------------------------------------------------------ 标志位
IFACEMETHODIMP ExplorerCommand::GetFlags(EXPCMDFLAGS* flags) noexcept {
    GRT_COM_TRY
    if (!flags) return E_POINTER;
    *flags = ECF_DEFAULT;
    switch (m_node->kind) {
        case NodeKind::Root:
        case NodeKind::Group:
            if (!m_node->children.empty())
                *flags = static_cast<EXPCMDFLAGS>(ECF_HASSUBCOMMANDS);
            break;
        case NodeKind::Flag:
            *flags = ECF_TOGGLEABLE;
            break;
        case NodeKind::Separator:
            *flags = ECF_ISSEPARATOR;
            break;
        default:
            break;
    }
    return S_OK;
    GRT_COM_CATCH
}

IFACEMETHODIMP ExplorerCommand::EnumSubCommands(IEnumExplorerCommand** out) noexcept {
    GRT_COM_TRY
    TraceCall("EnumSubCommands(in)", *m_node);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (m_node->children.empty()) return E_NOTIMPL;   // 叶子节点：不提供子菜单
    const HRESULT hr = EnumExplorerCommand::Create(m_node->children, m_ctx, out);
    GRT_SHELL_TRACE("EnumSubCommands(out) key=" << NodeIdentityKey(*m_node) << " children="
                                                << m_node->children.size() << " hr=0x" << std::hex
                                                << static_cast<unsigned long>(hr));
    return hr;
    GRT_COM_CATCH
}

}  // namespace grt::shell
