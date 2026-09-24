# GitRT.exe —— GUI 主程序（《技术实现设计》§9）
file(GLOB GRT_GUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB GRT_GUI_RC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.rc")

add_executable(GitRT WIN32 ${GRT_GUI_SOURCES} ${GRT_GUI_RC})
target_include_directories(GitRT PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/src/core")
target_link_libraries(GitRT PRIVATE gitrt_core comctl32 uxtheme dwmapi ole32 oleaut32 shell32 shlwapi winhttp)
target_link_options(GitRT PRIVATE -static -static-libgcc -static-libstdc++)

# 同理（见 src/shell/CMakeLists.txt 的说明）：resources.rc 通过 #include 依赖共享
# 字符串片段，必须显式声明依赖，否则改串不会触发资源重编译。
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources.rc" APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/core/menu_strings.zh.rcinc;${CMAKE_SOURCE_DIR}/src/core/menu_strings.en.rcinc;${CMAKE_SOURCE_DIR}/src/core/resource_ids.h")
