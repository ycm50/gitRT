#include "status.h"

namespace grt {

void RepoStatus::Recount() {
    staged = modified = untracked = conflicted = 0;
    for (const auto& e : entries) {
        switch (e.type) {
            case '?': ++untracked; break;
            case 'u': ++conflicted; break;
            default:
                if (e.x != '.' && e.x != ' ') ++staged;
                if (e.y != '.' && e.y != ' ') ++modified;
                break;
        }
    }
}

namespace {

// 跳过 n 个以空格分隔的字段，返回其余部分（路径可能含空格，只能按字段数切）
std::string_view AfterFields(std::string_view tok, int n) {
    size_t p = 0;
    for (int k = 0; k < n; ++k) {
        p = tok.find(' ', p);
        if (p == std::string_view::npos) return {};
        ++p;
    }
    return tok.substr(p);
}

std::string_view TrimView(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

int ToInt(std::string_view s) {
    int v = 0;
    bool neg = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

}  // namespace

RepoStatus ParsePorcelainV2(std::string_view bytes) {
    RepoStatus st;
    st.parsed = true;

    size_t i = 0;
    auto nextToken = [&](std::string_view* out) -> bool {
        if (i >= bytes.size()) return false;
        size_t z = bytes.find('\0', i);
        if (z == std::string_view::npos) z = bytes.size();
        *out = bytes.substr(i, z - i);
        i = z + 1;
        return true;
    };

    std::string_view tok;
    while (nextToken(&tok)) {
        tok = TrimView(tok);
        if (tok.empty()) continue;

        if (tok[0] == '#') {
            // header 同样是 NUL 结尾（实测①）
            // 格式为 "# <key> [<value>]"：必须先跳过 '#' 后的空格，再在 key/value 之间切分。
            // （此前误用“第一个空格”切分，导致 "# stash 1" 的 key 变成 "stash 1" 而全部 header 失配）
            const size_t p1 = tok.find(' ');
            if (p1 == std::string_view::npos) continue;
            const size_t p2 = tok.find(' ', p1 + 1);
            const std::string_view key =
                (p2 == std::string_view::npos) ? tok.substr(p1 + 1) : tok.substr(p1 + 1, p2 - p1 - 1);
            const std::string_view val =
                (p2 == std::string_view::npos) ? std::string_view{} : TrimView(tok.substr(p2 + 1));
            if (key == "branch.oid") {
                st.oid.assign(val);
                if (val == "(initial)") st.oid.clear();
            } else if (key == "branch.head") {
                st.head.assign(val);
                if (val == "(detached)") st.detached = true;
            } else if (key == "branch.upstream") {
                st.upstream.assign(val);
            } else if (key == "branch.ab") {
                // "+2 -3"
                const size_t sp2 = val.find(' ');
                if (sp2 != std::string_view::npos) {
                    st.ahead = ToInt(val.substr(0, sp2));
                    st.behind = ToInt(val.substr(sp2 + 1));
                }
            } else if (key == "stash") {
                st.stashCount = ToInt(val);
            }
            continue;
        }

        StatusEntry e;
        if (tok[0] == '1') {
            e.type = '1';
            if (tok.size() >= 4) { e.x = tok[2]; e.y = tok[3]; }
            e.path.assign(AfterFields(tok, 8));           // 8 个字段后是路径
            st.entries.push_back(std::move(e));
        } else if (tok[0] == '2') {
            e.type = '2';
            if (tok.size() >= 4) { e.x = tok[2]; e.y = tok[3]; }
            e.path.assign(AfterFields(tok, 9));           // 9 个字段后是路径
            // 实测②：重命名/复制条目额外消费一个 NUL 分隔的 origPath
            std::string_view orig;
            if (nextToken(&orig)) e.origPath.assign(TrimView(orig));
            st.entries.push_back(std::move(e));
        } else if (tok[0] == 'u') {
            e.type = 'u';
            if (tok.size() >= 4) { e.x = tok[2]; e.y = tok[3]; }
            e.path.assign(AfterFields(tok, 10));
            st.entries.push_back(std::move(e));
        } else if (tok[0] == '?') {
            e.type = '?';
            e.x = e.y = '?';
            e.path.assign(tok.size() > 2 ? tok.substr(2) : std::string_view{});
            st.entries.push_back(std::move(e));
        } else if (tok[0] == '!') {
            // 忽略项（我们从不请求 --ignored，出现则忽略）
        }
    }

    st.Recount();
    return st;
}

std::wstring DescribeEntry(const StatusEntry& e) {
    switch (e.type) {
        case '?': return L"\u672a\u8ddf\u8e2a";           // 未跟踪
        case 'u': return L"\u51b2\u7a81";                  // 冲突
        case '2': return L"\u91cd\u547d\u540d/\u590d\u5236";  // 重命名/复制
        default: break;
    }
    std::wstring s;
    if (e.x != '.' && e.x != ' ') s += L"\u6682\u5b58";     // 暂存
    if (e.y != '.' && e.y != ' ') {
        if (!s.empty()) s += L"+";
        s += L"\u5df2\u4fee\u6539";                          // 已修改
    }
    return s.empty() ? L"\u5df2\u77e5" : s;
}

}  // namespace grt
