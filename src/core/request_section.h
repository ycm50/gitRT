#pragma once
// ---------------------------------------------------------------------------
// 共享内存请求段（《技术实现设计》§4.5 / §5.4）
//
// 用途：Shell DLL 的 Invoke 把"命令 + 选择上下文"写进一个命名段，然后把段名交给
//      接收方（今天的直启 GUI，将来的常驻 Broker）。DLL 侧写完后**不立即关闭句柄**
//      的原因见 request_section.cpp 的 KeepAlive 注释（命名段在无人持有句柄时会
//      立即销毁，若 DLL 先关闭、接收方后打开就会丢请求）。
//
// 布局（小端）：
//   [GrtReqHeader 48B] [paths: count × (u32 字节数 + UTF-16LE 数据)] [options JSON]
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include "core.h"

namespace grt {

#pragma pack(push, 8)
struct GrtReqHeader {
    uint32_t magic       = 0x51525247;  // 'GRRQ'
    uint16_t version     = 1;
    uint16_t headerSize  = 48;
    uint16_t cmdId       = 0;
    uint16_t selMask     = 0;
    uint32_t callerPid   = 0;
    uint32_t callerTid   = 0;
    uint64_t createdMs   = 0;
    uint32_t pathCount   = 0;
    uint32_t pathBytes   = 0;   // 路径区总字节数（含每条的长度前缀）
    uint32_t optionsBytes = 0;  // options JSON 字节数（UTF-8，不含结尾 NUL）
    uint32_t reserved    = 0;
};
#pragma pack(pop)
static_assert(sizeof(GrtReqHeader) == 48, "GrtReqHeader 必须是 48 字节（§5.4 wire format）");

struct ShellRequest {
    uint16_t                  cmdId = 0;
    uint16_t                  selMask = 0;
    std::wstring              cwd;         // 选择项公共父目录（可为空）
    std::vector<std::wstring> paths;       // 已规范化的选择项（绝对路径）
    std::string               optionsJson; // flags 覆盖等（UTF-8 JSON，可为空）
};

// 生成一个不重名的段名：Local\GitRT.Req.<pid>-<tick>-<n>
std::wstring MakeRequestSectionName();

// 写段 + 保活句柄（见实现注释）。失败返回 false 并写日志。
bool WriteRequestSection(const std::wstring& name, const ShellRequest& req);

// 打开并读取一个请求段（接收方使用）。读取后调用方无需释放段：写方保活，
// 由写方按 FIFO 上限回收。失败（不存在/版本不符/校验失败）返回 false。
bool ReadRequestSection(const std::wstring& name, ShellRequest* out);

// 释放当前进程保活的全部请求段句柄（进程退出前调用；也可不调用，OS 会回收）
void ReleaseKeptRequestSections();

}  // namespace grt
