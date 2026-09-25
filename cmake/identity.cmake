# ---------------------------------------------------------------------------
# identity.cmake —— 读取 packaging/identity.json，导出 GRT_ID_* 变量
#   《技术实现设计》§3.4 / §11.1：publisher / packageName / applicationId / CLSID
#   是"三处必须一致、不一致就静默失败"的值，因此它们只有一个写入点（identity.json），
#   由本文件同时喂给：① DLL 内嵌 msix 清单 ② AppxManifest.xml ③ clsid_values.h
# ---------------------------------------------------------------------------

set(GRT_IDENTITY_JSON "${CMAKE_SOURCE_DIR}/packaging/identity.json")
if(NOT EXISTS "${GRT_IDENTITY_JSON}")
  message(FATAL_ERROR "找不到 packaging/identity.json —— 它是身份与打包参数的唯一真相源")
endif()

# ★ 改了 identity.json 必须触发重新 configure：否则 AppxManifest.xml / version_info.rc /
#   clsid_values.h 都是"上一次 configure 的产物"，改了版本号却不生效（或只有一半生效）。
#   CMake 默认不会因为 file(READ) 过的文件变化而重跑，必须显式登记。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GRT_IDENTITY_JSON}")

file(READ "${GRT_IDENTITY_JSON}" GRT_IDENTITY_TEXT)

function(grt_identity_get key out)
  string(JSON _v ERROR_VARIABLE _err GET "${GRT_IDENTITY_TEXT}" "${key}")
  if(_err)
    message(FATAL_ERROR "identity.json 缺少键 '${key}'：${_err}")
  endif()
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

grt_identity_get(publisher             GRT_ID_PUBLISHER)
grt_identity_get(packageName           GRT_ID_PACKAGE_NAME)
grt_identity_get(applicationId         GRT_ID_APPLICATION_ID)
grt_identity_get(version               GRT_ID_VERSION)
grt_identity_get(displayName           GRT_ID_DISPLAY_NAME)
grt_identity_get(publisherDisplayName  GRT_ID_PUBLISHER_DISPLAY_NAME)
grt_identity_get(description           GRT_ID_DESCRIPTION)
grt_identity_get(executable            GRT_ID_EXECUTABLE)
grt_identity_get(shellDll              GRT_ID_SHELL_DLL)
grt_identity_get(clsid                 GRT_ID_CLSID)
grt_identity_get(canonicalGuid         GRT_ID_CANONICAL_GUID)
grt_identity_get(verbId                GRT_ID_VERB)
grt_identity_get(minOsVersion          GRT_ID_MIN_OS)
grt_identity_get(maxOsVersionTested    GRT_ID_MAX_OS_TESTED)

# MSIX 版本必须是 4 段数字（0x80073CF9 的常见原因就是这里写错）
if(NOT GRT_ID_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "identity.json 的 version 必须是 4 段数字（如 0.1.0.0），当前为 '${GRT_ID_VERSION}'")
endif()

# .rc 的 FILEVERSION / PRODUCTVERSION 需要逗号形式（0,1,0,0）。
# ★ 它和 GRT_VERSION 都从这里派生：资源里的版本号**不允许**再写字面量，
#   否则 install.ps1 从 exe 读到的 FileVersion 会和清单/User-Agent 悄悄分叉。
string(REPLACE "." "," GRT_ID_VERSION_COMMA "${GRT_ID_VERSION}")

# ---------------------------------------------------------------- 目标架构
# 不可写死：ARM64 宿主上 amd64 的清单会加载失败（§2.6）
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64|arm64")
    set(GRT_ARCH "arm64")
  else()
    set(GRT_ARCH "amd64")
  endif()
else()
  set(GRT_ARCH "x86")
endif()

# ------------------------------------------------------ CLSID 的两种书写形式
# ★ M0-B 实测（0x80080204 / 0xC00CE169）：AppxManifest 的 `com:Class/@Id` 与
#   `desktop5:Verb/@Clsid` 按 schema 必须是 **不带花括号** 的 GUID；带 {} 会清单校验失败。
#   为方便阅读，identity.json 允许两种写法，这里统一规范化。
string(REGEX REPLACE "[{}]" "" GRT_ID_CLSID_BARE "${GRT_ID_CLSID}")
# 注意：CMake 的正则不把 {} 当量词，因此这里按"-"拆分后逐段校验长度与十六进制字符
string(REPLACE "-" ";" _grt_clsid_parts "${GRT_ID_CLSID_BARE}")
list(LENGTH _grt_clsid_parts _grt_clsid_n)
set(_grt_clsid_ok TRUE)
if(NOT _grt_clsid_n EQUAL 5)
  set(_grt_clsid_ok FALSE)
else()
  set(_grt_want 8 4 4 4 12)
  foreach(_i RANGE 0 4)
    list(GET _grt_clsid_parts ${_i} _part)
    list(GET _grt_want ${_i} _want)
    string(LENGTH "${_part}" _len)
    if(NOT _len EQUAL _want OR NOT _part MATCHES "^[0-9a-fA-F]+$")
      set(_grt_clsid_ok FALSE)
    endif()
  endforeach()
endif()
if(NOT _grt_clsid_ok)
  message(FATAL_ERROR "identity.json 的 clsid 不是合法 GUID：'${GRT_ID_CLSID}'")
endif()
set(GRT_ID_CLSID "{${GRT_ID_CLSID_BARE}}")   # 带括号形式（C++ 初始化子 / 日志用）

# ------------------------------------------------------------- 路径（正斜杠）
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" GRT_SRC_DIR)
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" GRT_BIN_DIR)
set(GRT_SHELL_MANIFEST_FILE "${GRT_BIN_DIR}/src/shell/gitrt.shell.manifest")
set(GRT_ICON_APP_FILE  "${GRT_SRC_DIR}/packaging/Assets/gitrt.ico")
set(GRT_ICON_WARN_FILE "${GRT_SRC_DIR}/packaging/Assets/gitrt-warn.ico")

# ----------------------------------------------------- GUID 字面量（C++ 初始化子）
# "{8E1D2F40-7C3A-4B8E-9F01-2A5C6D7E8F90}" → 0x8E1D2F40, 0x7C3A, 0x4B8E, {0x9F,0x01,...}
function(grt_guid_init guid out)
  string(REPLACE "{" "" _g "${guid}")
  string(REPLACE "}" "" _g "${_g}")
  string(REPLACE "-" ";" _parts "${_g}")
  list(LENGTH _parts _n)
  if(NOT _n EQUAL 5)
    message(FATAL_ERROR "GUID 格式不正确：${guid}")
  endif()
  list(GET _parts 0 _a)
  list(GET _parts 1 _b)
  list(GET _parts 2 _c)
  list(GET _parts 3 _d)   # 4 位 → 2 字节
  list(GET _parts 4 _e)   # 12 位 → 6 字节
  string(SUBSTRING "${_d}" 0 2 _d1)
  string(SUBSTRING "${_d}" 2 2 _d2)
  set(_bytes "")
  foreach(_i RANGE 0 5)
    math(EXPR _off "${_i} * 2")
    string(SUBSTRING "${_e}" ${_off} 2 _byte)
    if(_bytes STREQUAL "")
      set(_bytes "0x${_byte}")
    else()
      set(_bytes "${_bytes}, 0x${_byte}")
    endif()
  endforeach()
  set(${out} "{0x${_a}, 0x${_b}, 0x${_c}, {0x${_d1}, 0x${_d2}, ${_bytes}}}" PARENT_SCOPE)
endfunction()

grt_guid_init("${GRT_ID_CLSID_BARE}" GRT_ID_CLSID_INIT)
grt_guid_init("${GRT_ID_CANONICAL_GUID}" GRT_ID_CANONICAL_INIT)

# 生成 clsid_values.h（放进二进制目录，避免污染源码树）
file(MAKE_DIRECTORY "${GRT_BIN_DIR}/generated")
configure_file("${CMAKE_SOURCE_DIR}/packaging/clsid_values.h.in"
               "${GRT_BIN_DIR}/generated/clsid_values.h" @ONLY)

message(STATUS "GitRT 身份：${GRT_ID_PACKAGE_NAME} ${GRT_ID_VERSION} (${GRT_ARCH}) publisher=${GRT_ID_PUBLISHER}")
message(STATUS "GitRT CLSID：${GRT_ID_CLSID} → ${GRT_ID_CLSID_INIT}")
