// IClassFactory 实现（《技术实现设计》§4.1）
#include "class_factory.h"

#include "clsid.h"
#include "com_guard.h"
#include "explorer_command.h"
#include "menu_model.h"
#include "selection.h"

namespace grt::shell {

HRESULT CreateRootCommand(IExplorerCommand** out) noexcept {
    // 每次菜单弹出都应拿到一个全新的选择上下文（选择内容每次都不同）
    auto ctx = std::make_shared<SelectionContext>();
    std::shared_ptr<MenuNode> root = MenuRoot();
    if (!root) return E_FAIL;
    return ExplorerCommand::Create(std::move(root), std::move(ctx), out);
}

namespace {

class ClassFactory final : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() noexcept override {
        return static_cast<ULONG>(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    IFACEMETHODIMP_(ULONG) Release() noexcept override {
        const LONG n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) noexcept override {
        GRT_COM_TRY
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;   // 不做聚合

        IExplorerCommand* cmd = nullptr;
        const HRESULT hr = CreateRootCommand(&cmd);
        if (FAILED(hr)) return hr;
        const HRESULT hr2 = cmd->QueryInterface(riid, ppv);
        cmd->Release();
        return hr2;
        GRT_COM_CATCH
    }

    IFACEMETHODIMP LockServer(WINBOOL) noexcept override { return S_OK; }

private:
    ~ClassFactory() = default;
    std::atomic<LONG> m_ref{1};
};

}  // namespace

HRESULT CreateClassFactory(REFCLSID clsid, REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != kClsidGitRTCommand) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        auto* f = new (std::nothrow) ClassFactory();
        if (!f) return E_OUTOFMEMORY;
        const HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

}  // namespace grt::shell
