cmake_minimum_required(VERSION 3.25)
# 版本号不在这里写：identity.json 是唯一真相源（见 cmake/identity.cmake），
# 下面用 GRT_ID_VERSION 派生 GRT_VERSION。原先这里另写了一份 "0.1.0"，
# 已经和 identity.json 的 0.1.0.0 不一致——User-Agent 与"关于"对话框会跟着漂。
project(GitRT LANGUAGES CXX RC)

# 《技术实现设计》D4：MSYS2 UCRT64 (MinGW-w64) 为主工具链
if(NOT MINGW)
  message(WARNING "本工程以 MSYS2 UCRT64 (MinGW-w64) 为主工具链（决策 D4）；当前工具链为 ${CMAKE_CXX_COMPILER_ID}，可能编译失败。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# 身份与打包参数的唯一真相源（packaging/identity.json）
#   → 导出 GRT_ID_*、GRT_ARCH、GUID 初始化子，并生成 clsid_values.h（§3.4 / §11.1）
include(cmake/identity.cmake)

# AppxManifest.xml（稀疏身份包；由 identity.json 注入，见 §11.2）
configure_file(
  "${CMAKE_SOURCE_DIR}/packaging/AppxManifest.xml.in"
  "${CMAKE_BINARY_DIR}/packaging/AppxManifest.xml" @ONLY)

# _WIN32_WINNT 必须 >= 0x0A00：STARTUPINFOEXW / PROC_THREAD_ATTRIBUTE_HANDLE_LIST 等依赖它
add_compile_definitions(
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
  GRT_VERSION="${GRT_ID_VERSION}")   # 派生自 identity.json（唯一真相源）

add_compile_options(-Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -fno-rtti)

# .rc 是 UTF-8（含中文字符串）
set(CMAKE_RC_FLAGS "--codepage=65001")

option(GRT_WERROR "把编译警告当错误" OFF)
if(GRT_WERROR)
  add_compile_options(-Werror)
endif()

# GCC 静态分析（-fanalyzer）：找的是真实的资源泄漏 / 空指针 / 越界 / 重复释放，且**零第三方依赖**。
# 默认 OFF（正常开发不跑，单文件约 2–7 秒）；本地或 CI 单独跑：
#   cmake -S . -B build/analyze -DGRT_ANALYZER=ON && cmake --build build/analyze
# 走 CMake 而不是自己拼 g++ 命令，是为了让每个目标拿到**正确**的包含路径与宏定义。
option(GRT_ANALYZER "编译期开启 GCC 静态分析（-fanalyzer，明显变慢）" OFF)
if(GRT_ANALYZER)
  add_compile_options(-fanalyzer)
  message(STATUS "已开启 GCC -fanalyzer 静态分析（编译会明显变慢）")
endif()

# 开发期传统注册（DllRegisterServer）与探针默认值随构建类型走：
#   发行版（Release）的导出表**必须**只有 DllGetClassObject / DllCanUnloadNow（§2.8）
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(GRT_DEV_REGISTER_DEFAULT OFF)
else()
  set(GRT_DEV_REGISTER_DEFAULT ON)
endif()
option(GRT_DEV_REGISTER "编译 DllRegisterServer/DllUnregisterServer（仅开发期传统注册测试，§2.8）" ${GRT_DEV_REGISTER_DEFAULT})
option(GRT_SHELL_PROBE  "把菜单构建期计时/探针日志编入 Shell DLL（仅开发构建，§4.8）" OFF)
option(GRT_BUILD_TOOLS  "构建 Shell 探针/自检工具（GitRT.ShellProbe.exe）" ON)
option(GRT_BUILD_TESTS  "构建无窗口的核心单元测试（GitRT.CoreTests.exe，注册进 CTest）" ON)

add_subdirectory(src/core)
add_subdirectory(src/gui)
add_subdirectory(src/shell)
if(GRT_BUILD_TOOLS)
  add_subdirectory(src/shellprobe)
endif()

# ------------------------------------------------------------------ CTest
# 把两套自检注册成测试（§13.1 的 L1 落点在本工程用"内置自检"承载）：
#   ctest --test-dir build/debug --output-on-failure
enable_testing()

# 无窗口的核心单元测试：纯函数/纯构造，任何环境（含无桌面会话的 CI）都能跑
if(GRT_BUILD_TESTS)
  add_subdirectory(src/tests)
endif()

if(GRT_BUILD_TOOLS)
  # 路径必须用原生命令行形式（Shell 的 IShellItem 解析器不接受混合分隔符）
  file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" GRT_BIN_NATIVE)
  file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/packaging/identity.json" GRT_IDENTITY_NATIVE)
  file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/packaging/AppxManifest.xml" GRT_APPX_NATIVE)
  add_test(NAME gui_selftest
           COMMAND GitRT "--self-test=${GRT_BIN_NATIVE}\\ctest-gui.txt")
  add_test(NAME shell_selftest
           COMMAND GitRT.ShellProbe "--self-test=${GRT_BIN_NATIVE}\\ctest-shell.txt"
                   --dll "$<TARGET_FILE:gitrt_shell>"
                   --identity "${GRT_IDENTITY_NATIVE}"
                   --appx "${GRT_APPX_NATIVE}"
                   --workdir "${GRT_BIN_NATIVE}")
endif()
