#pragma once
// ---------------------------------------------------------------------------
// 极简断言框架（零第三方依赖，符合本项目的"不引第三方"决策）
//
// 为什么不用 gtest / Catch2：本项目连 JSON 解析都是手写的，为了测试引入依赖不划算。
// 测试真正需要的只有三件事：
//   ① 失败能定位（断言名写清楚"验的是什么行为"）
//   ② 全绿/有红一眼可见
//   ③ 有失败时退出码非 0（CTest 才会判红）
//
// 输出格式刻意与 GitRT.exe 的 `--self-test` 报告保持一致
// （"[PASS] …" / "== 结果: N 通过 / M 失败 =="），
// 这样 CI 里两套自检能用同一条规则抓结果。
// ---------------------------------------------------------------------------

#include <windows.h>   // SetConsoleOutputCP

#include <cstdio>
#include <string>

#include "core.h"      // grt::WideToUtf8

namespace grt::test {

class Harness {
public:
    Harness() { ::SetConsoleOutputCP(CP_UTF8); }   // 中文断言名在控制台/CI 日志里也看得见

    void Check(bool ok, const std::wstring& what) {
        const std::string name = WideToUtf8(what);
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name.c_str());
        if (ok) {
            ++m_pass;
        } else {
            ++m_fail;
        }
    }

    // 与 GUI 自检同一套措辞；返回值直接当进程退出码（0 = 全绿）
    int Summary(const char* title) const {
        std::printf("\n== %s 结果: %d 通过 / %d 失败 ==\n", title, m_pass, m_fail);
        return m_fail == 0 ? 0 : 1;
    }

private:
    int m_pass = 0;
    int m_fail = 0;
};

}  // namespace grt::test
