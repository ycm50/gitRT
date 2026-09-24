#pragma once
// ---------------------------------------------------------------------------
// Invoke 转发（《技术实现设计》§4.5 / §5.3 / §6.6）
//
// 顺序（任何一步成功都不再往下走）：
//   ① 把请求写进共享内存段（Local\GitRT.Req.<pid>-<tick>-<n>）
//   ② 尝试连常驻 Broker 的命名管道（\\.\pipe\GitRT.<sid>.v1，50 ms 超时）
//   ③ 回落：直接 CreateProcessW(GitRT.exe, "--request-section <name>")
//
// 本增量（M0）还没有 Broker，所以实际走的是 ③；② 的代码路径保留并可用，
// 因为它是 §14.4 第 8 步（Broker）落地后**不需要改动 DLL** 的接口。
// Invoke 必须"立即返回"（§4.2 预算 ≤ 10 ms，不含目标进程启动）。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "core.h"

namespace grt::shell {

struct InvokeRequest {
    uint16_t                  cmdId = 0;
    uint16_t                  selMask = 0;
    std::vector<std::wstring> paths;
    std::string               optionsJson;   // flags 覆盖等（UTF-8）
    std::wstring              cwd;
};

// 转交请求。返回 true = 已交给某个接收方（不等待结果）。
// 失败时把原因写进 errorText（调用方决定是否弹一个最小 MessageBox）。
bool ForwardInvoke(const InvokeRequest& req, std::wstring* errorText);

// 管道名（§3.1：按用户 SID 隔离；取不到 SID 时返回空串 → 跳过管道路径）
std::wstring BrokerPipeName();

// C:\...\GitRT.exe 的候选路径（DLL 同目录优先，其次是 per-user 安装目录）
std::vector<std::wstring> GuiExecutableCandidates();

}  // namespace grt::shell
