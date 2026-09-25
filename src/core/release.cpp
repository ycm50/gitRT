// 发布（GitHub Release）实现 —— 见 release.h 顶部说明
#include "release.h"

#include <algorithm>

#include "command_builder.h"
#include "tag.h"

namespace grt {

namespace {

const wchar_t* kGhInstallHint =
    L"没装 GitHub CLI（gh）：GitHub 发布只能由 gh 创建。装法：winget install GitHub.cli，"
    L"装完重开 GitRT 让它重新探测 PATH";

std::wstring QuoteCmdArgs(const std::wstring& exe, const std::vector<std::wstring>& argv) {
    std::wstring s = exe;
    for (const auto& a : argv) {
        s += L' ';
        s += QuoteArg(a);
    }
    return s;
}

std::wstring ExpandPath(const std::wstring& p) {
    wchar_t buf[32768]{};
    const DWORD n = ::ExpandEnvironmentStringsW(p.c_str(), buf, 32768);
    if (n == 0 || n > 32768) return p;
    return std::wstring(buf, n ? n - 1 : 0);
}

std::vector<std::wstring> SplitPathList(const wchar_t* value) {
    std::vector<std::wstring> out;
    if (!value) return out;
    std::wstring cur;
    for (const wchar_t* p = value; *p; ++p) {
        if (*p == L';') {
            if (!Trim(cur).empty()) out.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

// ------------------------------------------------------- 极简 JSON 取值
// 只为 `gh release list --json …` 这种"gh 自己产出的规整 JSON"服务：
// 不做递归解析、不建对象树，只是按 key 找字面量（见 release.h 的说明）。
bool JsonStringAt(const std::wstring& s, size_t pos, std::wstring* out) {
    while (pos < s.size() && (s[pos] == L' ' || s[pos] == L'\t' || s[pos] == L'\r' || s[pos] == L'\n')) ++pos;
    if (pos >= s.size() || s[pos] != L'"') return false;
    ++pos;
    std::wstring v;
    while (pos < s.size()) {
        const wchar_t c = s[pos];
        if (c == L'\\' && pos + 1 < s.size()) {
            const wchar_t n = s[pos + 1];
            switch (n) {
                case L'n': v += L'\n'; break;
                case L'r': v += L'\r'; break;
                case L't': v += L'\t'; break;
                case L'u': {
                    // \uXXXX：只处理 BMP 内的简单情形，够 gh 的输出用
                    if (pos + 5 < s.size()) {
                        const std::wstring hex = s.substr(pos + 2, 4);
                        const unsigned long cp = ::wcstoul(hex.c_str(), nullptr, 16);
                        if (cp) v += static_cast<wchar_t>(cp);
                        pos += 4;
                    }
                    break;
                }
                default: v += n; break;
            }
            pos += 2;
            continue;
        }
        if (c == L'"') { *out = v; return true; }
        v += c;
        ++pos;
    }
    return false;
}

bool JsonValueAt(const std::wstring& s, size_t pos, std::wstring* out) {
    while (pos < s.size() && (s[pos] == L' ' || s[pos] == L'\t')) ++pos;
    if (pos < s.size() && s[pos] == L'"') return JsonStringAt(s, pos, out);
    const size_t start = pos;
    while (pos < s.size() && s[pos] != L',' && s[pos] != L'}' && s[pos] != L']' && s[pos] != L'\n') ++pos;
    *out = Trim(s.substr(start, pos - start));
    return !out->empty();
}

// 找 "key" 之后的值；from 之后第一次出现。
bool JsonField(const std::wstring& s, const std::wstring& key, size_t from, std::wstring* out,
               size_t* valuePos = nullptr) {
    const std::wstring pat = L"\"" + key + L"\"";
    const size_t k = s.find(pat, from);
    if (k == std::wstring::npos) return false;
    size_t colon = s.find(L':', k + pat.size());
    if (colon == std::wstring::npos) return false;
    const bool ok = JsonValueAt(s, colon + 1, out);
    if (ok && valuePos) *valuePos = colon + 1;
    return ok;
}

bool JsonBool(const std::wstring& v) { return v == L"true" || v == L"1"; }

}  // namespace

// ------------------------------------------------------------------ gh 探测
GhInfo DetectGh() {
    GhInfo info;

    // 1) 先看 PATH 上有没有 gh.exe / gh（不启动进程，便宜且无副作用）
    {
        const std::vector<std::wstring> dirs = SplitPathList(::_wgetenv(L"PATH"));
        for (const auto& d : dirs) {
            const std::wstring dir = ExpandPath(d);
            const std::wstring candidate = JoinPath(dir, L"gh.exe");
            if (PathExists(candidate)) { info.exe = candidate; break; }
            const std::wstring plain = JoinPath(dir, L"gh");
            if (PathExists(plain)) { info.exe = plain; break; }
            const std::wstring cmd = JoinPath(dir, L"gh.cmd");
            if (PathExists(cmd)) { info.exe = cmd; break; }
        }
    }
    // 2) 兜底：常见安装位置（用户级 winget/scoop 不一定进了当前进程的 PATH）
    if (info.exe.empty()) {
        for (const wchar_t* rel : {L"\\GitHub CLI\\gh.exe", L"\\Programs\\GitHub CLI\\gh.exe",
                                   L"\\scoop\\shims\\gh.exe", L"\\Programs\\gh\\bin\\gh.exe"}) {
            const std::wstring candidate = ExpandPath(L"%LOCALAPPDATA%") + rel;
            if (PathExists(candidate)) { info.exe = candidate; break; }
        }
    }

    if (info.exe.empty()) {
        info.error = kGhInstallHint;
        return info;
    }

    std::wstring out, err;
    const RunResult r = RunProcessSync(info.exe, {L"--version"}, L"", 15000, &out, &err);
    if (r.spawnFailed || r.exitCode != 0) {
        info.error = std::wstring(kGhInstallHint) + L"（找到了 " + info.exe + L" 但跑不起来）";
        info.exe.clear();
        return info;
    }
    // `gh --version` 第一行形如 "gh version 2.63.2 (2024-11-20)"
    std::wstring first = out;
    if (const size_t nl = first.find(L'\n'); nl != std::wstring::npos) first = first.substr(0, nl);
    info.version = Trim(first);
    info.available = true;
    return info;
}

// ------------------------------------------------------------------ 通用进程
RunResult RunProcessSync(const std::wstring& exe, const std::vector<std::wstring>& argv,
                         const std::wstring& cwd, uint32_t timeoutMs, std::wstring* out,
                         std::wstring* err) {
    Invocation inv;
    inv.exe = exe;
    inv.argv = argv;
    inv.cwd = cwd;
    inv.env = BuildGitEnvironment(false, L"");   // git 环境同样适合 gh（清掉泄漏变量 + 稳定的 PATH）
    inv.timeoutMs = timeoutMs;
    auto runner = MakeProcessGitRunner();
    const RunResult r = runner->RunSync(inv, nullptr);
    if (out) *out = W(r.out);
    if (err) *err = W(r.err);
    return r;
}

// ------------------------------------------------------------------ 发布清单
bool LoadReleases(const std::wstring& gitExe, const std::wstring& repoRoot,
                  std::vector<ReleaseEntry>* out, std::wstring* error) {
    if (out) out->clear();
    if (error) error->clear();
    if (!out) return false;

    const GhInfo gh = DetectGh();
    if (!gh.available) {
        if (error) *error = gh.error.empty() ? kGhInstallHint : gh.error;
        return false;
    }
    if (repoRoot.empty()) {
        if (error) *error = L"没有可用的仓库（先选一个 GitHub 仓库）";
        return false;
    }

    std::wstring ro, re;
    const RunResult r = RunProcessSync(
        gh.exe,
        {L"release", L"list", L"--limit", L"30", L"--json",
         L"tagName,name,publishedAt,isDraft,isPrerelease"},
        repoRoot, 60000, &ro, &re);
    if (r.spawnFailed) {
        if (error) *error = L"启动 gh 失败";
        return false;
    }
    if (r.exitCode != 0) {
        const std::wstring why = Trim(re).empty() ? Trim(ro) : Trim(re);
        if (error) {
            *error = why.empty() ? (L"gh release list 失败（exit=" + std::to_wstring(r.exitCode) + L"）")
                                 : (L"gh release list 失败：" + why);
        }
        return false;
    }

    // 极简解析：每次从 "tagName" 往后取同一条记录里的其余字段
    size_t pos = std::wstring::npos;
    {
        const std::wstring key = L"\"tagName\"";
        pos = ro.find(key);
    }
    while (pos != std::wstring::npos) {
        ReleaseEntry e;
        if (!JsonField(ro, L"tagName", pos, &e.tag)) break;
        JsonField(ro, L"name", pos, &e.name);
        JsonField(ro, L"publishedAt", pos, &e.publishedAt);
        {
            std::wstring b;
            if (JsonField(ro, L"isDraft", pos, &b)) e.draft = JsonBool(b);
            if (JsonField(ro, L"isPrerelease", pos, &b)) e.prerelease = JsonBool(b);
        }
        out->push_back(std::move(e));
        pos = ro.find(L"\"tagName\"", pos + 9);
    }
    return true;
}

// ------------------------------------------------------------------ 计划
ReleasePlan BuildReleasePlan(const std::wstring& gitExe, const std::wstring& repoRoot,
                             const std::wstring& tag, const std::wstring& title,
                             const std::wstring& notes, bool draft, bool prerelease, bool pushTag,
                             const std::vector<std::wstring>& assets, bool generateNotes) {
    ReleasePlan plan;
    plan.tag = Trim(tag);
    plan.title = Trim(title);
    plan.notes = notes;
    plan.draft = draft;
    plan.prerelease = prerelease;
    plan.pushTag = pushTag;
    plan.generateNotes = generateNotes;
    plan.assets = assets;
    plan.destructive = false;   // 发布是 Careful：会公开，但不破坏东西

    if (gitExe.empty() || repoRoot.empty()) {
        plan.error = L"没有可用的 git 或仓库";
        return plan;
    }

    const GhInfo gh = DetectGh();
    if (!gh.available) {
        plan.error = gh.error.empty() ? kGhInstallHint : gh.error;
        return plan;
    }

    if (plan.tag.empty()) {
        plan.error = L"请填写要发布的标签名（例如 v1.0.0）";
        return plan;
    }
    // 标签必须已经存在：先打标签再发布，是 GitHub 的硬要求
    {
        const RunResult r = RunGitSync(gitExe,
                                       {L"rev-parse", L"--verify", L"--quiet",
                                        L"refs/tags/" + plan.tag},
                                       repoRoot, 30000);
        if (r.exitCode != 0) {
            plan.error = L"标签不存在：" + plan.tag + L"（先「打标签…」创建它，或改用 --push-tag 配合已有标签）";
            return plan;
        }
    }

    // 附件：每个都必须真的存在（附件路径写错，gh 的报错很难懂）
    for (const auto& a : plan.assets) {
        const std::wstring f = Trim(a);
        if (f.empty()) continue;
        if (!PathExists(f)) {
            plan.error = L"找不到附件文件：" + f;
            return plan;
        }
    }

    // ① 先把标签推上去（可选）
    if (pushTag) {
        plan.argvList.push_back({L"push", L"origin", plan.tag});
        plan.commandLines.push_back(QuoteCmdArgs(L"git", {L"push", L"origin", plan.tag}));
    }

    // ② gh release create
    std::vector<std::wstring> argv{L"release", L"create", plan.tag};
    argv.push_back(L"--title");
    argv.push_back(plan.title.empty() ? plan.tag : plan.title);
    const bool hasNotes = !Trim(plan.notes).empty();
    if (hasNotes) {
        argv.push_back(L"--notes");
        argv.push_back(plan.notes);
    } else if (plan.generateNotes) {
        argv.push_back(L"--generate-notes");
    }
    if (plan.draft) argv.push_back(L"--draft");
    if (plan.prerelease) argv.push_back(L"--prerelease");
    for (const auto& a : plan.assets) {
        const std::wstring f = Trim(a);
        if (f.empty()) continue;
        argv.push_back(f);
    }
    plan.argvList.push_back(argv);
    plan.commandLines.push_back(QuoteCmdArgs(L"gh", argv));

    if (!hasNotes && !plan.generateNotes) {
        plan.warnings.push_back(L"既没给发布说明也没勾「自动生成说明」：Release 正文会是空的");
    }
    if (hasNotes && plan.generateNotes) {
        plan.warnings.push_back(L"同时给了说明和「自动生成说明」：以你写的说明为准（不加 --generate-notes）");
    }
    if (plan.draft) {
        plan.warnings.push_back(L"这是草稿（--draft）：只有你能看到，确认后手动发布");
    } else {
        plan.warnings.push_back(L"发布后**公开可见**（GitHub Release），仓库访问级别决定谁能看到");
    }
    if (plan.pushTag) {
        plan.warnings.push_back(L"会先把标签 " + plan.tag + L" 推送到 origin");
    }
    if (!plan.assets.empty()) {
        plan.warnings.push_back(L"会带上 " + std::to_wstring(plan.assets.size()) + L" 个附件");
    }
    plan.ok = true;
    return plan;
}

// ------------------------------------------------------------------ 执行
ReleaseResult ApplyReleasePlan(const std::wstring& repoRoot, const ReleasePlan& plan,
                               const std::function<void(const std::wstring&)>& onCommand,
                               const std::function<void(const std::string&)>& onOutput) {
    ReleaseResult res;
    if (!plan.ok) {
        res.error = plan.error.empty() ? L"发布计划不可执行" : plan.error;
        return res;
    }
    const GhInfo gh = DetectGh();
    if (!gh.available) {
        res.error = gh.error.empty() ? kGhInstallHint : gh.error;
        return res;
    }
    const std::wstring gitExe = FindGitExecutable();

    const size_t n = std::min(plan.argvList.size(), plan.commandLines.size());
    for (size_t i = 0; i < n; ++i) {
        const std::vector<std::wstring>& argv = plan.argvList[i];
        // 计划里的第一条（如果存在）永远是 git push；其余都交给 gh
        const bool isPush = !argv.empty() && argv[0] == L"push";
        const std::wstring exe = isPush ? gitExe : gh.exe;
        if (exe.empty()) {
            res.error = L"没有可用的 git.exe";
            return res;
        }

        if (onCommand) onCommand(L"> " + plan.commandLines[i]);
        std::wstring o, e;
        const RunResult r = RunProcessSync(exe, argv, repoRoot, 300000, &o, &e);
        ReleaseStep step;
        step.commandLine = plan.commandLines[i];
        step.exitCode = r.exitCode;
        step.out = o;
        step.err = e;
        res.steps.push_back(step);
        if (onOutput) {
            if (!o.empty()) onOutput(WideToUtf8(o));
            if (!e.empty()) onOutput(WideToUtf8(e));
        }
        if (r.spawnFailed) {
            res.error = L"启动进程失败：" + exe;
            return res;
        }
        if (r.exitCode != 0) {
            res.error = Trim(e).empty() ? (L"命令失败（exit=" + std::to_wstring(r.exitCode) + L"）")
                                        : Trim(e);
            return res;
        }
    }
    res.ok = true;
    return res;
}

}  // namespace grt
