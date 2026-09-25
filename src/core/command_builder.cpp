#include "command_builder.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace grt {

// ============================================================ 参数校验（§7.5）
static bool IsAsciiAlnum(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
}

bool IsValidBranchName(std::wstring_view s) {
    if (s.empty() || s.size() > 255) return false;
    if (s.front() == L'-' || s.front() == L'.' || s.front() == L'/' || s.back() == L'/') return false;
    if (s.back() == L'.') return false;
    std::wstring t(s);
    if (t.find(L"..") != std::wstring::npos) return false;
    if (t.find(L"@{") != std::wstring::npos) return false;
    if (t.find(L"//") != std::wstring::npos) return false;
    if (t.size() >= 5 && t.compare(t.size() - 5, 5, L".lock") == 0) return false;
    for (wchar_t c : s) {
        if (c < 0x20 || c == 0x7F) return false;
        if (c == L' ' || c == L'~' || c == L'^' || c == L':' || c == L'?' || c == L'*' ||
            c == L'[' || c == L'\\')
            return false;
    }
    return true;
}

bool IsValidRemoteName(std::wstring_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (wchar_t c : s) {
        if (!(IsAsciiAlnum(c) || c == L'.' || c == L'_' || c == L'-')) return false;
    }
    return true;
}

bool IsValidRevision(std::wstring_view s) {
    if (s.empty() || s.size() > 255) return false;
    if (s.front() == L'-') return false;
    // 允许 git 修订语法：HEAD~1、HEAD^、@{upstream}、main:path 等
    // 因为我们从不经过 shell，所以只需要排除"不可能属于修订表达式"的字符
    for (wchar_t c : s) {
        if (c < 0x20 || c == 0x7F) return false;
        const bool ok = IsAsciiAlnum(c) || c == L'.' || c == L'_' || c == L'-' || c == L'/' ||
                        c == L'~' || c == L'^' || c == L'@' || c == L'{' || c == L'}' || c == L':';
        if (!ok) return false;
    }
    return true;
}

bool IsValidUrl(std::wstring_view s) {
    if (s.empty() || s.size() > 2048) return false;
    if (s.front() == L'-') return false;                      // 防止被当成 flag
    for (wchar_t c : s)
        if (c < 0x20 || c == 0x7F) return false;
    static const wchar_t* kSchemes[] = {L"https://", L"http://", L"ssh://", L"git://", L"file://"};
    for (const wchar_t* sc : kSchemes) {
        const size_t n = std::wcslen(sc);
        if (s.size() > n && _wcsnicmp(s.data(), sc, n) == 0) return true;
    }
    // 本地路径也是合法来源：绝对盘符路径、UNC，或确实存在的路径。
    // 因为我们从不经过 shell（只把它作为单个 argv 元素），所以不存在注入面。
    const bool looksPath = (s.size() >= 3 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) ||
                           (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\');
    if (looksPath || PathExists(std::wstring(s))) return true;
    // SCP-like: user@host:path
    const size_t at = s.find(L'@');
    const size_t colon = s.find(L':');
    return at != std::wstring_view::npos && colon != std::wstring_view::npos && at < colon;
}

bool HasSelectedPaths(const BuildInput& in) { return !in.paths.empty(); }

// ================================================================ 命令行渲染
std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring BuildDisplayText(const BuiltCommand& cmd) {
    // argv 里不含程序名（由 Invocation.exe 提供），这里补上 "git " 以便用户看到完整命令行
    std::wstring out;
    for (size_t i = 0; i < cmd.argvList.size(); ++i) {
        if (i) out += L"\n";
        out += L"git";
        for (size_t j = 0; j < cmd.argvList[i].size(); ++j) {
            out += L' ';
            out += QuoteArg(cmd.argvList[i][j]);
        }
    }
    return out;
}

// ============================================================== flag 展开
namespace {

bool FlagEnabled(const std::map<std::string, std::wstring>& flags, const FlagSpec& f) {
    const auto it = flags.find(f.key);
    if (it == flags.end()) return f.defaultOn;
    return it->second == L"1";
}

std::wstring FlagValue(const std::map<std::string, std::wstring>& flags, const FlagSpec& f) {
    const auto it = flags.find(f.key);
    return it == flags.end() ? std::wstring() : it->second;
}

// 把 {flags} 展开为 argv 片段（顺序 = 表内声明顺序，保证稳定）
std::vector<std::wstring> ExpandFlags(const CommandSpec& spec,
                                      const std::map<std::string, std::wstring>& flags,
                                      BuildError* err) {
    std::vector<std::wstring> out;
    if (!spec.flags) return out;

    // 互斥组（Radio）：只要用户/AI 显式给了组内任一选项，该组就**不再**套用默认值，
    // 否则会出现"--mixed --hard"同时进 argv 这种自相矛盾的命令。
    std::map<std::string, bool> groupExplicit;
    for (uint8_t i = 0; i < spec.flagCount; ++i) {
        const FlagSpec& f = spec.flags[i];
        if (f.kind != FlagKind::Radio || !f.radioGroup) continue;
        const auto it = flags.find(f.key);
        if (it != flags.end() && it->second == L"1") groupExplicit[f.radioGroup] = true;
    }

    for (uint8_t i = 0; i < spec.flagCount; ++i) {
        const FlagSpec& f = spec.flags[i];
        if (f.kind == FlagKind::Value) {
            const std::wstring v = FlagValue(flags, f);
            if (v.empty()) continue;
            std::wstring token = W(f.gitArg ? f.gitArg : "");
            const size_t pos = token.find(L"{v}");
            if (pos != std::wstring::npos) token.replace(pos, 3, v);
            out.push_back(token);
            continue;
        }
        bool on = false;
        if (f.kind == FlagKind::Radio && f.radioGroup) {
            const auto it = flags.find(f.key);
            if (it != flags.end()) {
                on = (it->second == L"1");
            } else {
                // 组内没有显式选择时才使用默认项
                on = f.defaultOn && !groupExplicit[f.radioGroup];
            }
        } else {
            on = FlagEnabled(flags, f);
        }
        if (on && f.gitArg) out.push_back(W(f.gitArg));
    }
    (void)err;
    return out;
}

// 值型 flag 的白名单式校验（目前只针对 clone 选项；要加别的命令往这里补规则）
bool ValidateFlagValues(const CommandSpec& spec, const std::map<std::string, std::wstring>& flags,
                        BuildError* err) {
    if (!spec.flags) return true;
    for (uint8_t i = 0; i < spec.flagCount; ++i) {
        const FlagSpec& f = spec.flags[i];
        if (f.kind != FlagKind::Value) continue;
        const std::wstring v = FlagValue(flags, f);
        if (v.empty()) continue;
        const std::string key = f.key ? f.key : "";
        if (key == "depth") {
            // 浅克隆深度：正整数（git 也接受 --depth=<n>），上限给个常识值
            bool ok = !v.empty() && v.size() <= 7;
            for (const wchar_t c : v) {
                if (c < L'0' || c > L'9') ok = false;
            }
            if (ok && v.find_first_not_of(L'0') == std::wstring::npos) ok = false;   // 全 0
            if (!ok) {
                err->code = 5;
                err->field = f.key;
                err->message = L"浅克隆深度要是正整数（例如 1），当前输入：" + v;
                return false;
            }
        } else if (key == "branch") {
            if (!IsValidBranchName(v)) {
                err->code = 5;
                err->field = f.key;
                err->message = L"分支名不合法：" + v;
                return false;
            }
        } else if (key == "filter") {
            // 形如 blob:none / tree:0 / blob:limit=1m——只允许字母数字与 : , = + - . /
            bool ok = true;
            for (const wchar_t c : v) {
                const bool good = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                                  (c >= L'0' && c <= L'9') || c == L':' || c == L',' || c == L'=' ||
                                  c == L'+' || c == L'-' || c == L'.' || c == L'/';
                if (!good) ok = false;
            }
            if (!ok) {
                err->code = 5;
                err->field = f.key;
                err->message = L"--filter 只允许字母数字与 : , = + - . / （例如 blob:none）：" + v;
                return false;
            }
        }
    }
    return true;
}
bool ParamRequired(const CommandSpec& spec) {
    return spec.param != ParamKind::None && spec.param != ParamKind::Pattern;
}

}  // namespace

// ================================================================ 主构造器
bool BuildCommand(const BuildInput& in, BuiltCommand* out, BuildError* err) {
    BuildError local;
    if (!err) err = &local;
    err->code = 0;
    err->message.clear();
    err->field = nullptr;

    if (!in.spec || !out) {
        err->code = 1;
        err->message = L"内部错误：命令表项为空";
        return false;
    }
    const CommandSpec& spec = *in.spec;
    out->argvList.clear();
    out->notes.clear();
    out->cwd = !in.cwd.empty() ? in.cwd : in.repoRoot;

    if (spec.exec == ExecKind::Internal) {   // GUI 内部动作，不生成 argv
        out->display.clear();
        return true;
    }

    // ---- 参数校验（按 ParamKind）----
    const std::wstring paramValue =
        spec.paramKey ? in.params.count(spec.paramKey) ? in.params.at(spec.paramKey) : std::wstring()
                      : std::wstring();
    if (ParamRequired(spec) && Trim(paramValue).empty()) {
        err->code = 4022;
        err->message = L"请填写必填参数";
        err->field = spec.paramKey;
        return false;
    }
    switch (spec.param) {
        case ParamKind::CommitMessage:
            if (paramValue.size() > 65536) {
                err->code = 4021;
                err->message = L"提交信息过长";
                err->field = spec.paramKey;
                return false;
            }
            break;
        case ParamKind::BranchName:
            if (!IsValidBranchName(paramValue)) {
                err->code = 4019;
                err->message = L"分支名不合法（不能以 - . / 开头，不能含空格、.. 、@{ 等）";
                err->field = spec.paramKey;
                return false;
            }
            break;
        case ParamKind::ExistingBranch:
            if (!IsValidBranchName(paramValue)) {
                err->code = 4019;
                err->message = L"请选择一个合法的分支";
                err->field = spec.paramKey;
                return false;
            }
            break;
        case ParamKind::RemoteName:
            if (!IsValidRemoteName(paramValue)) {
                err->code = 4019;
                err->message = L"远端名不合法（只允许字母、数字、. _ -）";
                err->field = spec.paramKey;
                return false;
            }
            break;
        case ParamKind::Revision:
            if (!IsValidRevision(paramValue)) {
                err->code = 4021;
                err->message = L"修订表达式不合法";
                err->field = spec.paramKey;
                return false;
            }
            break;
        case ParamKind::Url:
            if (!IsValidUrl(paramValue)) {
                err->code = 4020;
                err->message = L"地址不合法（支持 https/http/ssh/git/file，或 user@host:path）";
                err->field = spec.paramKey;
                return false;
            }
            break;
        default:
            break;
    }

    // ---- 路径校验 ----
    std::vector<std::wstring> paths = in.paths;
    for (auto it = paths.begin(); it != paths.end();) {
        if (it->empty() || (*it)[0] == L'-') {
            out->notes.push_back(L"路径非法，已忽略：" + *it);
            it = paths.erase(it);
        } else {
            ++it;
        }
    }
    if (!in.repoRoot.empty()) {
        const std::wstring rootLower = ToLowerAscii(NormalizePath(in.repoRoot));
        for (auto it = paths.begin(); it != paths.end();) {
            const std::wstring p = ToLowerAscii(NormalizePath(*it));
            const bool inside = p == rootLower ||
                                (p.size() > rootLower.size() &&
                                 p.compare(0, rootLower.size(), rootLower) == 0 &&
                                 (p[rootLower.size()] == L'\\' || p[rootLower.size()] == L'/'));
            if (!inside) {
                out->notes.push_back(L"不在当前仓库内，已忽略：" + *it);
                it = paths.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 去重（大小写不敏感，Windows 语义）
    {
        std::vector<std::wstring> uniq;
        for (const auto& p : paths) {
            const std::wstring low = ToLowerAscii(p);
            bool dup = false;
            for (const auto& q : uniq)
                if (ToLowerAscii(q) == low) { dup = true; break; }
            if (!dup) uniq.push_back(p);
        }
        paths.swap(uniq);
    }

    // ---- 需要选中路径的命令 ----
    const bool needsPaths = spec.argvTemplate && std::string_view(spec.argvTemplate).find("{paths}") != std::string_view::npos;
    if (needsPaths && paths.empty() && spec.id != 1102) {   // 1102(unstage) 允许整仓
        // 不是硬错误：git 会自己报错，但提前给出更友好的提示
        out->notes.push_back(L"未选择任何文件，将对整个工作区生效");
    }

    // ---- 模板展开（按 " ; " 分段 → 多条命令）----
    // ---- 值型 flag 的轻校验：宁可在这里拦下，也不要让用户看到 git 那句难懂的报错 ----
    if (!ValidateFlagValues(spec, in.flags, err)) return false;    const std::vector<std::wstring> flagsArgv = ExpandFlags(spec, in.flags, err);
    const std::wstring tmpl = W(spec.argvTemplate ? spec.argvTemplate : "");

    std::vector<std::wstring> segments;
    {
        size_t start = 0;
        while (start <= tmpl.size()) {
            size_t sep = tmpl.find(L" ; ", start);
            if (sep == std::wstring::npos) {
                segments.push_back(Trim(tmpl.substr(start)));
                break;
            }
            segments.push_back(Trim(tmpl.substr(start, sep - start)));
            start = sep + 3;
        }
    }

    for (const auto& seg : segments) {
        if (seg.empty()) continue;
        std::vector<std::wstring> argv;
        const std::vector<std::wstring> tokens = SplitWhitespace(WideToUtf8(seg));
        bool pathsInserted = false;
        for (size_t ti = 0; ti < tokens.size(); ++ti) {
            const std::wstring& token = tokens[ti];
            // 模板里的程序名要去掉：真正的可执行文件由 Invocation.exe 提供，
            // 否则会执行成 "git git reset ..."（此前只有真正跑一遍才暴露出来）
            if (ti == 0 && (token == L"git" || token == L"git.exe")) continue;
            if (token == L"{flags}") {
                argv.insert(argv.end(), flagsArgv.begin(), flagsArgv.end());
            } else if (token == L"{paths}") {
                pathsInserted = true;
                if (!paths.empty()) {
                    argv.push_back(L"--");
                    argv.insert(argv.end(), paths.begin(), paths.end());
                }
            } else if (!token.empty() && token.front() == L'{' && token.back() == L'}') {
                const std::string key = WideToUtf8(std::wstring_view(token).substr(1, token.size() - 2));
                const auto pit = in.params.find(key);
                if (pit == in.params.end() || pit->second.empty()) {
                    err->code = 4022;
                    err->message = L"缺少参数：" + W(key);
                    err->field = nullptr;   // 注意：不可返回局部字符串的 c_str()
                    return false;
                }
                argv.push_back(pit->second);
            } else {
                argv.push_back(token);
            }
        }
        (void)pathsInserted;
        if (!argv.empty()) out->argvList.push_back(std::move(argv));
    }

    // ---- 特例：多步 / 条件命令（见《技术实现设计》§3.5 与本文档的偏差说明）----
    if (spec.id == 1105 /*commit.commit*/ && !paths.empty()) {
        // 先暂存选中路径，再提交（不把 pathspec 传给 commit，避免绕过索引的语义差异）
        std::vector<std::wstring> add{L"add", L"-A", L"--"};
        add.insert(add.end(), paths.begin(), paths.end());
        out->argvList.insert(out->argvList.begin(), std::move(add));
        out->notes.push_back(L"将先暂存所选内容，再提交");
    }
    if (spec.id == 1601 /*adv.clean*/) {
        out->notes.push_back(L"将先执行预览（git clean -nd），确认无损失后再执行清理");
    }

    if (out->argvList.empty()) {
        err->code = 4006;
        err->message = L"未能生成可执行的命令";
        return false;
    }
    out->display = BuildDisplayText(*out);
    return true;
}

}  // namespace grt
