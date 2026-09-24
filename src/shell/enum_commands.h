#pragma once
// ---------------------------------------------------------------------------
// IEnumExplorerCommand 实现（《技术实现设计》§4.4）
//   · Skip / Clone：官方标注 "Not currently implemented" → 返回 E_NOTIMPL
//   · Next 必须支持 celt > 1（Shell 可能一次取多项）
// ---------------------------------------------------------------------------

#include <atomic>
#include <memory>
#include <vector>

#include <shobjidl.h>

#include "explorer_command.h"
#include "menu_model.h"
#include "selection.h"

namespace grt::shell {

class EnumExplorerCommand final : public IEnumExplorerCommand {
public:
    static HRESULT Create(const std::vector<std::shared_ptr<MenuNode>>& nodes,
                          std::shared_ptr<SelectionContext> ctx, IEnumExplorerCommand** out) noexcept;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override;
    IFACEMETHODIMP_(ULONG) AddRef() noexcept override;
    IFACEMETHODIMP_(ULONG) Release() noexcept override;

    IFACEMETHODIMP Next(ULONG celt, IExplorerCommand** rgelt, ULONG* fetched) noexcept override;
    IFACEMETHODIMP Skip(ULONG celt) noexcept override;
    IFACEMETHODIMP Reset() noexcept override;
    IFACEMETHODIMP Clone(IEnumExplorerCommand** out) noexcept override;

private:
    explicit EnumExplorerCommand(std::shared_ptr<SelectionContext> ctx) : m_ctx(std::move(ctx)) {}
    ~EnumExplorerCommand() = default;

    std::atomic<LONG>                              m_ref{1};
    std::vector<std::shared_ptr<MenuNode>>         m_nodes;
    size_t                                         m_pos = 0;
    std::shared_ptr<SelectionContext>              m_ctx;
};

}  // namespace grt::shell
