// IEnumExplorerCommand 实现（《技术实现设计》§4.4）
#include "enum_commands.h"

#include "clsid.h"
#include "com_guard.h"

namespace grt::shell {

HRESULT EnumExplorerCommand::Create(const std::vector<std::shared_ptr<MenuNode>>& nodes,
                                   std::shared_ptr<SelectionContext> ctx,
                                   IEnumExplorerCommand** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        auto* obj = new (std::nothrow) EnumExplorerCommand(std::move(ctx));
        if (!obj) return E_OUTOFMEMORY;
        obj->m_nodes = nodes;   // 复制 shared_ptr：子树生命周期由枚举器持有
        *out = static_cast<IEnumExplorerCommand*>(obj);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

IFACEMETHODIMP EnumExplorerCommand::QueryInterface(REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIidIEnumExplorerCommand) {
        *ppv = static_cast<IEnumExplorerCommand*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) EnumExplorerCommand::AddRef() noexcept {
    return static_cast<ULONG>(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
}

IFACEMETHODIMP_(ULONG) EnumExplorerCommand::Release() noexcept {
    const LONG n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) {
        delete this;
        return 0;
    }
    return static_cast<ULONG>(n);
}

IFACEMETHODIMP EnumExplorerCommand::Next(ULONG celt, IExplorerCommand** rgelt, ULONG* fetched) noexcept {
    if (!rgelt || (celt != 1 && !fetched)) return E_POINTER;
    GRT_COM_TRY
    ULONG produced = 0;
    for (ULONG i = 0; i < celt; ++i) {
        if (m_pos >= m_nodes.size()) break;
        IExplorerCommand* cmd = nullptr;
        const HRESULT hr = ExplorerCommand::Create(m_nodes[m_pos], m_ctx, &cmd);
        if (FAILED(hr)) break;
        rgelt[i] = cmd;
        ++m_pos;
        ++produced;
    }
    if (fetched) *fetched = produced;
    const HRESULT result = (produced == celt) ? S_OK : S_FALSE;
#if defined(GRT_SHELL_PROBE)
    GRT_LOGT("shell.call", "Next celt=" << celt << " produced=" << produced << " pos=" << m_pos << "/"
                                       << m_nodes.size() << " hr=" << (result == S_OK ? "S_OK" : "S_FALSE")
                                       << " fetchedPtr=" << (fetched ? 1 : 0));
#endif
    return result;
    GRT_COM_CATCH
}

// 官方文档：Skip / Clone "Not currently implemented"
IFACEMETHODIMP EnumExplorerCommand::Skip(ULONG) noexcept { return E_NOTIMPL; }

IFACEMETHODIMP EnumExplorerCommand::Reset() noexcept {
    m_pos = 0;
    return S_OK;
}

IFACEMETHODIMP EnumExplorerCommand::Clone(IEnumExplorerCommand**) noexcept { return E_NOTIMPL; }

}  // namespace grt::shell
