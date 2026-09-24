#pragma once
// ---------------------------------------------------------------------------
// 菜单构建期计时与探针日志（《技术实现设计》§4.8）
//   · 只在定义 GRT_SHELL_PROBE 的开发构建里编译进来（Release 完全消失）
//   · 计时用 QueryPerformanceCounter；超预算写 WARN（§3.7 的 10 ms 预算）
// ---------------------------------------------------------------------------

#include "core.h"

namespace grt::shell {

// 菜单构建期预算（§3.7）：GetTitle/GetIcon/GetState/EnumSubCommands 合计
constexpr double kMenuBudgetMs = 10.0;

void ProbeLog(const char* what, double us);

#if defined(GRT_SHELL_PROBE)
class MethodTimer {
public:
    explicit MethodTimer(const char* what) noexcept;
    ~MethodTimer();

private:
    const char* m_what;
    long long   m_freq = 0;
    long long   m_start = 0;
};
#define GRT_PROBE_TIMER(what) ::grt::shell::MethodTimer grt_probe_timer_(what)
#else
#define GRT_PROBE_TIMER(what) ((void)0)
#endif

}  // namespace grt::shell
