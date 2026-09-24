# GitRT.Core：核心静态库（清单见《技术实现设计》§2.1）
#
# 拆成两个静态库（§2.2 的架构约束）：
#   gitrt_core_tiny —— 允许被 Shell DLL 链接的极小子集（Explorer UI 线程里可调用，
#                      不含 git 进程封装 / 网络 / 任何第三方依赖）
#   gitrt_core      —— 其余全部（Broker / GUI / AskPass 使用）
# 约束靠**链接期**强制：shell 只链接 tiny，误用会直接链接失败。
set(GRT_CORE_TINY_SOURCES
  core.cpp
  config.cpp
  command_spec.cpp
  json_util.cpp
  request_section.cpp)

add_library(gitrt_core_tiny STATIC ${GRT_CORE_TINY_SOURCES})
target_include_directories(gitrt_core_tiny PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# tiny 只用到 kernel32 级别的系统调用（无 winhttp / shell32）
target_link_libraries(gitrt_core_tiny PUBLIC advapi32)

file(GLOB GRT_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# 注意：gitrt_core 也包含 tiny 的那几个源文件（同一份代码编译进两个库）。
# 这是刻意的——tiny 给 shell，full 给 GUI/Broker，**没有任何目标同时链接两者**
# （否则同一份匿名命名空间状态会出现两个实例）。
add_library(gitrt_core STATIC ${GRT_CORE_SOURCES})
target_include_directories(gitrt_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gitrt_core PUBLIC ole32 oleaut32 shlwapi shell32 advapi32 winhttp)
