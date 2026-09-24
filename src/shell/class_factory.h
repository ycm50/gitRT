#pragma once
// ---------------------------------------------------------------------------
// IClassFactory（《技术实现设计》§4.1）
//   DllGetClassObject → ClassFactory → CreateInstance → ExplorerCommand（根节点）
// ---------------------------------------------------------------------------

#include <shobjidl.h>

#include <objbase.h>

namespace grt::shell {

// 创建根命令对象（供 ClassFactory 与自检/探针工具共用）
HRESULT CreateRootCommand(IExplorerCommand** out) noexcept;

// DllGetClassObject 的实现（dllmain.cpp 只做转发）
HRESULT CreateClassFactory(REFCLSID clsid, REFIID riid, void** ppv) noexcept;

}  // namespace grt::shell
