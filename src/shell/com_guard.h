#pragma once
// ---------------------------------------------------------------------------
// COM 方法统一异常边界（《技术实现设计》§4.2 / §14.2）
//   MinGW 没有 SEH（__try/__except 不可用），因此每个 COM 方法必须用
//   try/catch(...) 兜底：Explorer 的调用栈上**绝不能**让异常逃逸。
// ---------------------------------------------------------------------------

#include "core.h"

#include <new>

#define GRT_COM_TRY try {
#define GRT_COM_CATCH                                     \
    }                                                     \
    catch (const std::bad_alloc&) {                       \
        GRT_LOGE("shell.com", "COM 方法内存不足");         \
        return E_OUTOFMEMORY;                             \
    }                                                     \
    catch (...) {                                         \
        GRT_LOGE("shell.com", "COM 方法未捕获异常");       \
        return E_FAIL;                                    \
    }
