// 探针日志与计时实现（《技术实现设计》§4.8）
#include "probe_log.h"

namespace grt::shell {

void ProbeLog(const char* what, double us) {
    // 探针只在开发构建里有意义；Release 下调用点已被宏去掉，这里做二次保护。
    GRT_LOGT("shell.probe", what << " us=" << us);
    if (us > kMenuBudgetMs * 1000.0) {
        GRT_LOGW("shell.probe", what << " 超出菜单构建预算 us=" << us);
    }
}

#if defined(GRT_SHELL_PROBE)
MethodTimer::MethodTimer(const char* what) noexcept : m_what(what) {
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    m_freq = f.QuadPart ? f.QuadPart : 1;
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    m_start = c.QuadPart;
}

MethodTimer::~MethodTimer() {
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    const double us = static_cast<double>(c.QuadPart - m_start) * 1e6 / static_cast<double>(m_freq);
    ProbeLog(m_what, us);
}
#endif

}  // namespace grt::shell
