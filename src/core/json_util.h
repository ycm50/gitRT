#pragma once
// ---------------------------------------------------------------------------
// 极简 JSON 工具（只覆盖 GitRT 自己需要的场景，零第三方依赖）
//   · JsonEscape / JsonUnescape：UTF-8 字节层面的转义与反转义（含 \uXXXX 代理对）
//   · JsonFindString / JsonFindRaw：从 OpenAI 兼容响应里取出某个字段
//   · JsonFindStringMap：取出 {"k":"v", ...} 形式的参数/flags 对象
//
// 为什么不用 nlohmann/json：Shell DLL 侧不允许引入任何第三方依赖（§3.7），
// 且这里需要解析的结构是完全可控的；代价是必须把边界情况写清楚并单测。
// ---------------------------------------------------------------------------

#include <map>

#include "core.h"

namespace grt {

// UTF-8 文本 → 可嵌入 JSON 字符串字面量的内容（不含两端引号）
std::string JsonEscape(std::string_view utf8);

// JSON 字符串字面量的内容（不含两端引号）→ UTF-8 文本
std::string JsonUnescape(std::string_view escaped);

// 在 json 中定位对象成员 "key"（忽略其后的空白与冒号），返回冒号后的位置
size_t JsonFindKeyPos(const std::string& json, std::string_view key, size_t from = 0);

// 取 "key": "字符串值"
bool JsonFindString(const std::string& json, std::string_view key, std::string* out,
                    size_t from = 0);

// 取 "key": <数字/true/false/对象/数组> 的原文
bool JsonFindRaw(const std::string& json, std::string_view key, std::string* out,
                 size_t from = 0);

// 取 "key": {"a":"b","c":"d"}（值必须是字符串，数字会被转成文本）
bool JsonFindStringMap(const std::string& json, std::string_view key,
                       std::map<std::string, std::string>* out, size_t from = 0);

// 去掉可能存在的 markdown 代码块围栏（模型偶尔会加 ```json ... ```）
std::string StripCodeFence(const std::string& text);

}  // namespace grt
