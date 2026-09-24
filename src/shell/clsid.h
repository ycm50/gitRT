#pragma once
// ---------------------------------------------------------------------------
// 稳定标识集中定义（《技术实现设计》§3.1）
//   ★ 一旦发布永不更改：AppxManifest 的 com:Class/@Id、desktop5:Verb/@Clsid、
//     DLL 内嵌 msix 清单三处必须与本文件一致。
// ---------------------------------------------------------------------------

#include <windows.h>

#include "clsid_values.h"   // 由 CMake 从 packaging/identity.json 生成（唯一真相源）

namespace grt::shell {

// 唯一的 Shell 命令类（值来自 packaging/identity.json，禁止在此手抄）
inline constexpr CLSID kClsidGitRTCommand = GRT_CLSID_COMMAND_INIT;

// GetCanonicalName 的返回值（供 Shell 内部统计；改动会被视为"新命令"）
inline constexpr GUID kGuidGitRTCommandCanonical = GRT_GUID_CANONICAL_INIT;

// 预留：多 verb 拆分时的第二 CLSID（仅当 16 项上限实测触发才启用）
inline constexpr CLSID kClsidGitRTVerbRoot = {
    0x4C8E2A17, 0x9B6D, 0x4E35, {0xA2, 0xF0, 0x8D, 0x1C, 0x3B, 0x4E, 0x5F, 0x60}};

// 图标资源 ID（gitrt_shell.rc）
constexpr int kIconApp = 101;    // GitRT 图标
constexpr int kIconWarn = 102;   // 危险命令图标

// 接口 IID（不依赖头文件里 __CRT_UUID_DECL 是否生效，见《技术实现设计》§1）
inline constexpr GUID kIidIExplorerCommand = {
    0xa08ce4d0, 0xfa25, 0x44ab, {0xb5, 0x7c, 0xc7, 0xb1, 0xc3, 0x23, 0xe0, 0xb9}};
inline constexpr GUID kIidIEnumExplorerCommand = {
    0xa88826f8, 0x186f, 0x4987, {0xaa, 0xde, 0xea, 0x0c, 0xef, 0x8f, 0xbf, 0xe8}};

// 菜单条目的资源 ID 约定（§3.3）
//   2000 段 = 命令标题；1000 段 = 分组名；3000 段 = flags 标签；6100 段 = 菜单专用
}  // namespace grt::shell
