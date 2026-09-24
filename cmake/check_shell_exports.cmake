# ---------------------------------------------------------------------------
# check_shell_exports.cmake —— GitRT.Shell.dll 的强制校验（《技术实现设计》§2.3）
#
#   ① 导出表：只允许 DllGetClassObject / DllCanUnloadNow
#      （开发构建额外允许 DllRegisterServer / DllUnregisterServer）
#      多一个导出就是把内部符号暴露给 Explorer 进程，必须构建失败。
#   ② 依赖 DLL：绝不能出现 libstdc++-6.dll / libwinpthread-1.dll
#      （Explorer 加载 DLL 时找不到它们 = 菜单静默消失）
#
# 用法（由 src/shell/CMakeLists.txt 的 POST_BUILD 调用）：
#   cmake -DOBJDUMP=<objdump> -DDLL=<dll> -DDEV_REGISTER=ON|OFF -P check_shell_exports.cmake
# ---------------------------------------------------------------------------

if(NOT EXISTS "${DLL}")
  message(FATAL_ERROR "找不到 ${DLL}")
endif()

execute_process(COMMAND "${OBJDUMP}" -p "${DLL}" OUTPUT_VARIABLE _out ERROR_VARIABLE _err
                RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "objdump -p 失败（${_rc}）：${_err}")
endif()

# ---------------------------------------------------------------- ① 导出表
set(_allowed DllGetClassObject DllCanUnloadNow)
if(DEV_REGISTER)
  list(APPEND _allowed DllRegisterServer DllUnregisterServer)
endif()

string(FIND "${_out}" "[Ordinal/Name Pointer] Table" _pos)
set(_exports "")
if(NOT _pos EQUAL -1)
  string(SUBSTRING "${_out}" ${_pos} -1 _names)
  # objdump 的导出名行形如：
  #   [   1] +base[   2]  0001 DllGetClassObject
  # 逐行解析（遇到空行 = 小节结束），避免把后面的重定位表项（DIR64 等）误当导出。
  string(REPLACE "\r" "" _names "${_names}")
  string(REPLACE "\n" ";" _lines "${_names}")
  foreach(_ln IN LISTS _lines)
    if(_ln MATCHES "^[ ]*$")
      break()
    endif()
    if(_ln MATCHES "^[ \t]*\\[[ ]*[0-9]+\\]")
      string(REGEX REPLACE "^.*[ \t]([A-Za-z_][A-Za-z0-9_]*)[ \t]*$" "\\1" _name "${_ln}")
      list(APPEND _exports "${_name}")
    endif()
  endforeach()
endif()

foreach(_e IN LISTS _exports)
  if(NOT _e IN_LIST _allowed)
    message(FATAL_ERROR
      "GitRT.Shell.dll 导出了不允许的符号 '${_e}'。允许集合：${_allowed}\n"
      "（多半是 -Wl,--exclude-all-symbols 或 .def 被改动，见 §2.3）")
  endif()
endforeach()

foreach(_need IN LISTS _allowed)
  if(NOT _need IN_LIST _exports)
    message(FATAL_ERROR "GitRT.Shell.dll 缺少必需的导出 '${_need}'（.def 或实现被裁剪？）")
  endif()
endforeach()

# ------------------------------------------------------------- ② 依赖 DLL
string(REGEX MATCHALL "DLL Name: [^\r\n]+" _deps "${_out}")
set(_bad "")
foreach(_d IN LISTS _deps)
  string(TOLOWER "${_d}" _dl)
  if(_dl MATCHES "libstdc\\+\\+-6|libwinpthread-1|libgcc_s")
    list(APPEND _bad "${_d}")
  endif()
endforeach()
if(_bad)
  message(FATAL_ERROR
    "GitRT.Shell.dll 依赖了运行时 DLL：${_bad}\n"
    "（必须静态链接运行时，否则 Explorer 里静默加载失败，见 §2.4）")
endif()

list(LENGTH _exports _n)
message(STATUS "GitRT.Shell.dll 校验通过：导出 ${_n} 个（${_exports}）；无运行时 DLL 依赖")
