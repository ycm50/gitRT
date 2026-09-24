#include "json_util.h"

#include <cstdio>

namespace grt {

// =================================================================== 转义
std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));   // UTF-8 字节原样保留
                }
        }
    }
    return out;
}

// ================================================================ 反转义
static void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp <= 0x7F) {
        out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static bool Hex4(const std::string& s, size_t pos, uint32_t* out) {
    if (pos + 4 > s.size()) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = s[pos + i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

std::string JsonUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            out.push_back(s[i]);
            continue;
        }
        if (++i >= s.size()) break;
        switch (s[i]) {
            case '"':  out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                const std::string buf(s);
                uint32_t cp = 0;
                if (!Hex4(buf, i + 1, &cp)) break;
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < buf.size() && buf[i + 1] == '\\' &&
                    buf[i + 2] == 'u') {
                    uint32_t lo = 0;
                    if (Hex4(buf, i + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                AppendUtf8(cp, &out);
                break;
            }
            default:
                out.push_back(s[i]);
                break;
        }
    }
    return out;
}

// ============================================================== 定位与取值
static void SkipWs(const std::string& j, size_t* p) {
    while (*p < j.size() && (j[*p] == ' ' || j[*p] == '\t' || j[*p] == '\r' || j[*p] == '\n')) ++*p;
}

size_t JsonFindKeyPos(const std::string& json, std::string_view key, size_t from) {
    // 找 "key" 后紧跟（允许空白）冒号的位置。
    // 注意 "reasoning_content" 不会误命中 "content"：因为要求 key 前必须是引号。
    std::string needle = "\"" + std::string(key) + "\"";
    size_t p = from;
    while ((p = json.find(needle, p)) != std::string::npos) {
        size_t q = p + needle.size();
        SkipWs(json, &q);
        if (q < json.size() && json[q] == ':') {
            return q + 1;
        }
        p += needle.size();
    }
    return std::string::npos;
}

// 从 pos 起解析一个 JSON 字符串字面量
static bool ParseStringLit(const std::string& j, size_t* p, std::string* out) {
    SkipWs(j, p);
    if (*p >= j.size() || j[*p] != '"') return false;
    ++*p;
    const size_t start = *p;
    std::string raw;
    while (*p < j.size()) {
        const char c = j[*p];
        if (c == '\\') {
            if (*p + 1 >= j.size()) return false;
            // 保留转义序列原样，最后统一反转义
            raw.append(j, *p, 2);
            *p += 2;
            continue;
        }
        if (c == '"') {
            *out = JsonUnescape(raw);
            ++*p;
            return true;
        }
        raw.push_back(c);
        ++*p;
    }
    (void)start;
    return false;
}

bool JsonFindString(const std::string& json, std::string_view key, std::string* out, size_t from) {
    const size_t p = JsonFindKeyPos(json, key, from);
    if (p == std::string::npos) return false;
    size_t q = p;
    return ParseStringLit(json, &q, out);
}

bool JsonFindRaw(const std::string& json, std::string_view key, std::string* out, size_t from) {
    const size_t p = JsonFindKeyPos(json, key, from);
    if (p == std::string::npos) return false;
    size_t q = p;
    SkipWs(json, &q);
    if (q >= json.size()) return false;
    const size_t start = q;
    if (json[q] == '{' || json[q] == '[') {
        const char open = json[q];
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        bool inStr = false;
        for (; q < json.size(); ++q) {
            const char c = json[q];
            if (inStr) {
                if (c == '\\') { ++q; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == open) ++depth;
            else if (c == close) {
                if (--depth == 0) {
                    ++q;
                    out->assign(json, start, q - start);
                    return true;
                }
            }
        }
        return false;
    }
    if (json[q] == '"') return ParseStringLit(json, &q, out);
    while (q < json.size() && json[q] != ',' && json[q] != '}' && json[q] != ']') ++q;
    out->assign(json, start, q - start);
    while (!out->empty() && (out->back() == ' ' || out->back() == '\n' || out->back() == '\r' ||
                             out->back() == '\t'))
        out->pop_back();
    return true;
}

bool JsonFindStringMap(const std::string& json, std::string_view key,
                       std::map<std::string, std::string>* out, size_t from) {
    const size_t p = JsonFindKeyPos(json, key, from);
    if (p == std::string::npos) return false;
    size_t q = p;
    SkipWs(json, &q);
    if (q >= json.size() || json[q] != '{') return false;
    ++q;
    for (;;) {
        SkipWs(json, &q);
        if (q >= json.size()) return false;
        if (json[q] == '}') return true;
        std::string k;
        if (!ParseStringLit(json, &q, &k)) return false;
        SkipWs(json, &q);
        if (q >= json.size() || json[q] != ':') return false;
        ++q;
        SkipWs(json, &q);
        if (q >= json.size()) return false;
        std::string v;
        if (json[q] == '"') {
            if (!ParseStringLit(json, &q, &v)) return false;
        } else if (json[q] == '{' || json[q] == '[') {
            // 嵌套对象/数组：本项目不会用到，整体跳过（避免死循环）
            const char open = json[q];
            const char close = (open == '{') ? '}' : ']';
            int depth = 0;
            bool inStr = false;
            for (; q < json.size(); ++q) {
                const char c = json[q];
                if (inStr) {
                    if (c == '\\') { ++q; continue; }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') { inStr = true; continue; }
                if (c == open) ++depth;
                else if (c == close && --depth == 0) { ++q; break; }
            }
        } else {
            const size_t start = q;
            while (q < json.size() && json[q] != ',' && json[q] != '}') ++q;
            v.assign(json, start, q - start);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\n' || v.back() == '\r' ||
                                  v.back() == '\t'))
                v.pop_back();
            // JSON 字面量 true/false/null 归一化为 1/0/空，便于当作 flag 值使用
            if (v == "true") v = "1";
            else if (v == "false" || v == "null") v = "0";
        }
        (*out)[k] = v;
        SkipWs(json, &q);
        if (q < json.size() && json[q] == ',') {
            ++q;
            continue;
        }
        if (q < json.size() && json[q] == '}') return true;
        return false;
    }
}

std::string StripCodeFence(const std::string& text) {
    std::string s = text;
    // 去掉首尾空白
    auto trim = [](std::string& t) {
        size_t b = 0, e = t.size();
        while (b < e && (t[b] == ' ' || t[b] == '\n' || t[b] == '\r' || t[b] == '\t')) ++b;
        while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\n' || t[e - 1] == '\r' || t[e - 1] == '\t')) --e;
        t = t.substr(b, e - b);
    };
    trim(s);
    if (s.rfind("```", 0) == 0) {
        const size_t nl = s.find('\n');
        if (nl != std::string::npos) s = s.substr(nl + 1);
        const size_t end = s.rfind("```");
        if (end != std::string::npos) s = s.substr(0, end);
    }
    trim(s);
    // 只保留第一个 { 到最后一个 } 之间的内容（模型可能前后加话）
    const size_t b = s.find('{');
    const size_t e = s.rfind('}');
    if (b != std::string::npos && e != std::string::npos && e > b) s = s.substr(b, e - b + 1);
    return s;
}

}  // namespace grt
