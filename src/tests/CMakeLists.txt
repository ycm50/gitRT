# GitRT.CoreTests —— 无窗口的核心单元测试（《技术实现设计》§13 的 L1 落点）
#
# 为什么单独一个可执行文件：这批断言原先锁在 GitRT.exe 的 --self-test 里，而
# `RunSelfTest(HWND mainWnd, …)` 需要一个真实窗口句柄（还会真的建参数面板），
# 于是"验证一个纯函数边界"也得先起一个 GUI 程序。控制台目标让它们在任意
# CI / 容器里秒级可跑，失败直接打在 stdout 上。
#
# 只链接 gitrt_core —— 这个目标**不允许**依赖任何 GUI 代码：
# 一旦有人往 core_tests.cpp 里用了 gui.h 的东西，链接期就会失败（这正是我们要的约束）。
add_executable(GitRT.CoreTests core_tests.cpp)

target_include_directories(GitRT.CoreTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(GitRT.CoreTests PRIVATE gitrt_core)
target_link_options(GitRT.CoreTests PRIVATE -static -static-libgcc -static-libstdc++)

# 与 gui_selftest / shell_selftest 并列；用例名保持 *_selftest 风格
add_test(NAME core_selftest COMMAND GitRT.CoreTests)
