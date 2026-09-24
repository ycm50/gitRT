# GitRT.ShellProbe.exe —— Shell 扩展现地验证工具（控制台程序）
#   链接 gitrt_core（需要 Chunk/config 等）；加载 GitRT.Shell.dll 走 COM 接口与菜单树。
#   静态链接运行时，保证单文件可分发（与 §2.4 一致）。
add_executable(GitRT.ShellProbe main.cpp)

target_include_directories(GitRT.ShellProbe PRIVATE
  "${CMAKE_SOURCE_DIR}/src/core"
  "${CMAKE_SOURCE_DIR}/src/shell"
  "${GRT_BIN_DIR}/generated")

target_link_libraries(GitRT.ShellProbe PRIVATE gitrt_core ole32 oleaut32 shlwapi shell32 uuid)
target_link_options(GitRT.ShellProbe PRIVATE -static -static-libgcc -static-libstdc++ -municode)
