# GitRT.exe —— GUI 主程序（《技术实现设计》§9）
file(GLOB GRT_GUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB GRT_GUI_RC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.rc")

# 图标资源：与 Shell DLL 用**同一对 .ico**（任务栏图标 == 右键菜单图标）。
# 路径由 configure_file 注入，避免依赖 windres 对相对路径/‑I 搜索的行为。
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icons.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/icons.rc" @ONLY)

# 版本信息资源：FileVersion 会被 install.ps1 读出来写进注册表，
# 必须与 identity.json（唯一真相源）同源，所以同样走 configure_file 注入。
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version_info.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/version_info.rc" @ONLY)

add_executable(GitRT WIN32 ${GRT_GUI_SOURCES} ${GRT_GUI_RC}
  "${CMAKE_CURRENT_BINARY_DIR}/icons.rc"
  "${CMAKE_CURRENT_BINARY_DIR}/version_info.rc")
target_include_directories(GitRT PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/src/core")
# comdlg32：发布窗口的"选择文件…"（GetOpenFileNameW，OFN_ALLOWMULTISELECT 多选附件）
target_link_libraries(GitRT PRIVATE gitrt_core comctl32 comdlg32 uxtheme dwmapi ole32 oleaut32 shell32 shlwapi winhttp)
target_link_options(GitRT PRIVATE -static -static-libgcc -static-libstdc++)

# 同理（见 src/shell/CMakeLists.txt 的说明）：resources.rc 通过 #include 依赖共享
# 字符串片段，必须显式声明依赖，否则改串不会触发资源重编译。
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources.rc" APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/core/menu_strings.zh.rcinc;${CMAKE_SOURCE_DIR}/src/core/menu_strings.en.rcinc;${CMAKE_SOURCE_DIR}/src/core/resource_ids.h")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/icons.rc" APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/core/resource_ids.h;${CMAKE_CURRENT_SOURCE_DIR}/icons.rc.in")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/version_info.rc" APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/packaging/identity.json;${CMAKE_CURRENT_SOURCE_DIR}/version_info.rc.in")
