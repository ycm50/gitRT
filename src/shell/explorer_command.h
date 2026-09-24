#pragma once
// ---------------------------------------------------------------------------
// IExplorerCommand 实现（《技术实现设计》§4.1 / §4.2）
//
// 设计要点：**只用一个 C++ 类**承载所有层级的菜单项（根 / 分组 / 命令 / 开关 /
// 分隔符 / 提示），用 m_node 区分。理由：子命令数量可达数十个且生命周期由 Shell
// 控制，单一类 + 工厂函数比多态继承更省代码、更好控制内存。
// ---------------------------------------------------------------------------

#include <atomic>
#include <memory>

#include <shobjidl.h>

#include "menu_model.h"
#include "selection.h"

namespace grt::shell {

class ExplorerCommand final : public IExplorerCommand {
public:
    // 返回的接口引用计数为 1（COM 创建约定）
    static HRESULT Create(std::shared_ptr<MenuNode> node, std::shared_ptr<SelectionContext> ctx,
                          IExplorerCommand** out) noexcept;

    // ---------------- IUnknown ----------------
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override;
    IFACEMETHODIMP_(ULONG) AddRef() noexcept override;
    IFACEMETHODIMP_(ULONG) Release() noexcept override;

    // ---------------- IExplorerCommand ----------------
    IFACEMETHODIMP GetTitle(IShellItemArray* items, LPWSTR* name) noexcept override;
    IFACEMETHODIMP GetIcon(IShellItemArray* items, LPWSTR* icon) noexcept override;
    IFACEMETHODIMP GetToolTip(IShellItemArray* items, LPWSTR* tip) noexcept override;
    IFACEMETHODIMP GetCanonicalName(GUID* guid) noexcept override;
    IFACEMETHODIMP GetState(IShellItemArray* items, WINBOOL okToBeSlow,
                            EXPCMDSTATE* state) noexcept override;
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx* bc) noexcept override;
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) noexcept override;
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** out) noexcept override;

private:
    ExplorerCommand(std::shared_ptr<MenuNode> node, std::shared_ptr<SelectionContext> ctx);
    ~ExplorerCommand() = default;

    // 若传入 array 且尚未解析过，则解析并缓存（§4.4 的三层防御）
    const SelectionContext& UpdateContext(IShellItemArray* items) noexcept;

    std::atomic<LONG>                 m_ref{1};
    std::shared_ptr<MenuNode>         m_node;
    std::shared_ptr<SelectionContext> m_ctx;   // 与整棵树共享
};

}  // namespace grt::shell
