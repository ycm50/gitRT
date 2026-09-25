# GitRT.Shell.dll —— 现代右键菜单扩展（《技术实现设计》§4）
#
# 硬性约束（§2.2 / §5.2）：
#   · 只链接 gitrt_core_tiny（链接期强制"Shell 不得依赖 git 执行/网络代码"）
#   · 静态链接运行时：绝不能依赖 libstdc++-6.dll / libwinpthread-1.dll
#     （Explorer 加载 DLL 时找不到即静默失败）
#   · 导出表只有 DllGetClassObject / DllCanUnloadNow

configure_file(gitrt.shell.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/gitrt.shell.manifest" @ONLY)
configure_file(gitrt_shell.rc.in "${CMAKE_CURRENT_BINARY_DIR}/gitrt_shell.rc" @ONLY)

# 导出表按构建类型生成：发行版只有 2 个 COM 入口（§2.8）
if(GRT_DEV_REGISTER)
  set(GRT_DEV_DEF_EXPORTS "    DllRegisterServer   PRIVATE\n    DllUnregisterServer PRIVATE")
else()
  set(GRT_DEV_DEF_EXPORTS "")
endif()
configure_file(gitrt_shell.def.in "${CMAKE_CURRENT_BINARY_DIR}/gitrt_shell.def" @ONLY)

add_library(gitrt_shell SHARED
  dllmain.cpp
  class_factory.cpp
  explorer_command.cpp
  enum_commands.cpp
  selection.cpp
  menu_model.cpp
  snapshot_reader.cpp
  broker_client.cpp
  probe_log.cpp
  shell_strings.rc
  "${CMAKE_CURRENT_BINARY_DIR}/gitrt_shell.rc")

# ★ .rc 通过 #include 依赖共享字符串片段，而 ninja 不会跟踪 windres 的 include
#   （windres 无可靠的 -MD），于是"改了片段但资源没重编译"——症状是 DLL 里还是旧串，
#   极难排查。这里显式声明依赖，任何片段改动都会触发资源重编译。
set(GRT_RC_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/core/menu_strings.zh.rcinc"
  "${CMAKE_SOURCE_DIR}/src/core/menu_strings.en.rcinc"
  "${CMAKE_SOURCE_DIR}/src/core/resource_ids.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/shell_strings.rc")
set_property(SOURCE shell_strings.rc APPEND PROPERTY OBJECT_DEPENDS "${GRT_RC_DEPENDS}")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/gitrt_shell.rc" APPEND PROPERTY OBJECT_DEPENDS
             "${GRT_RC_DEPENDS};${CMAKE_CURRENT_SOURCE_DIR}/gitrt_shell.rc.in;${GRT_IDENTITY_JSON}")

target_include_directories(gitrt_shell PRIVATE
  "${CMAKE_SOURCE_DIR}/src/core"
  "${GRT_BIN_DIR}/generated")   # clsid_values.h（由 identity.json 生成）

# 体积直接影响 Explorer 里的加载时间（§3.7 的 5 ms 预算）：
#   函数/数据分段 + --gc-sections 把静态库里用不到的代码丢掉。
#   注：不要在目标上加 -Os——与 libstdc++ 的某些内联符号（basic_string 移动构造）
#   组合会在 MinGW 上出现 undefined reference；优化级别跟随构建类型即可。
target_compile_options(gitrt_shell PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(gitrt_shell PRIVATE gitrt_core_tiny ole32 oleaut32 shlwapi uuid advapi32)

# MinGW 默认给 DLL 加 "lib" 前缀 → 必须去掉，否则清单里的 Path 与实际文件名不一致
set_target_properties(gitrt_shell PROPERTIES
  PREFIX "" OUTPUT_NAME "GitRT.Shell"
  LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gitrt_shell.def")

target_link_options(gitrt_shell PRIVATE
  -static -static-libgcc -static-libstdc++
  "-Wl,${CMAKE_CURRENT_BINARY_DIR}/gitrt_shell.def"
  -Wl,--exclude-all-symbols          # 只有 .def 里的导出
  -Wl,--enable-auto-image-base
  -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va
  -Wl,--gc-sections
  $<$<CONFIG:Release>:-s>)

if(GRT_DEV_REGISTER)
  target_compile_definitions(gitrt_shell PRIVATE GRT_DEV_REGISTER)
endif()
if(GRT_SHELL_PROBE)
  target_compile_definitions(gitrt_shell PRIVATE GRT_SHELL_PROBE)
endif()

# 导出表 / 依赖 DLL 校验（§2.3 的强制步骤）：不满足即构建失败
find_program(GRT_OBJDUMP objdump)
if(GRT_OBJDUMP)
  add_custom_command(TARGET gitrt_shell POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DOBJDUMP=${GRT_OBJDUMP}
            -DDLL=$<TARGET_FILE:gitrt_shell>
            -DDEV_REGISTER=${GRT_DEV_REGISTER}
            -P "${CMAKE_SOURCE_DIR}/cmake/check_shell_exports.cmake"
    COMMENT "GitRT.Shell.dll 导出表 / 依赖校验")
else()
  message(WARNING "未找到 objdump：跳过 Shell DLL 的导出表校验（§2.3 要求它必须在 CI 上执行）")
endif()
