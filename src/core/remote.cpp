// 远端跟踪（上游/领先落后/远端分支/基线视图）实现 —— 见 remote.h 顶部说明
#include "remote.h"

#include <algorithm>
#include <set>

#include "core.h"

namespace grt {

namespace {

std::wstring RunOut(const std::wstring& gitExe, const std::wstring& repoRoot,
                    const std::vector<std::wstring>& argv, int* exitCode = nullptr,
                    std::wstring* errOut = nullptr) {
    const RunResult r = RunGitSync(gitExe, argv, repoRoot, 30000);
    if (exitCode) *exitCode = r.exitCode;
    if (errOut) *errOut = Trim(W(r.err));
    return W(r.out);
}

std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (const wchar_t c : s) {
        if (c == L'\n') {
            if (!cur.empty() && cur.back() == L'\r') cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::wstring> SplitFields(const std::wstring& line, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (const wchar_t c : line) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::wstring ShortHash(const std::wstring& gitExe, const std::wstring& repoRoot, const std::wstring& rev) {
    int rc = 0;
    const std::wstring s = Trim(RunOut(gitExe, repoRoot, {L"rev-parse", L"--short", rev}, &rc));
    return rc == 0 ? s : std::wstring();
}

}  // namespace

// ------------------------------------------------------------------ 上游信息
RemoteInfo LoadRemoteInfo(const std::wstring& gitExe, const std::wstring& repoRoot) {
    RemoteInfo info;
    if (gitExe.empty() || repoRoot.empty()) {
        info.error = L"没有可用的 git 或仓库";
        return info;
    }
    int rc = 0;
    // 当前分支（detached 时 --abbrev-ref 给 "HEAD"）
    info.branch = Trim(RunOut(gitExe, repoRoot, {L"rev-parse", L"--abbrev-ref", L"HEAD"}, &rc));
    if (rc != 0) {
        info.branch.clear();
        info.error = L"这个仓库还没有提交（或不是 git 仓库）";
        info.ok = true;
        return info;
    }

    // 上游：@{upstream} 的完整名字（没设置就失败 → hasUpstream=false）
    int upRc = 0;
    std::wstring upErr;
    info.upstream = Trim(RunOut(gitExe, repoRoot,
                                {L"rev-parse", L"--abbrev-ref", L"--symbolic-full-name", L"@{upstream}"},
                                &upRc, &upErr));
    if (upRc != 0 || info.upstream.empty()) {
        info.ok = true;   // 信息本身读到了（只是没上游）
        info.hasUpstream = false;
        return info;
    }
    info.hasUpstream = true;

    int hRc = 0;
    info.upstreamHash = Trim(RunOut(gitExe, repoRoot, {L"rev-parse", info.upstream}, &hRc));
    if (hRc != 0) info.upstreamHash.clear();
    info.upstreamShort = info.upstreamHash.empty()
                             ? std::wstring()
                             : Trim(RunOut(gitExe, repoRoot, {L"rev-parse", L"--short", info.upstreamHash}));

    // 领先/落后：git rev-list --left-right --count <upstream>...HEAD → "落后\t领先"
    const std::wstring counts = Trim(RunOut(gitExe, repoRoot,
                                            {L"rev-list", L"--left-right", L"--count",
                                             info.upstream + L"...HEAD"}));
    if (!counts.empty()) {
        std::wstring a, b;
        bool tab = false;
        for (const wchar_t c : counts) {
            if (c == L'\t') { tab = true; continue; }
            (tab ? b : a) += c;
        }
        info.behind = static_cast<size_t>(_wtoi(a.c_str()));
        info.ahead = static_cast<size_t>(_wtoi(b.c_str()));
    }

    // 上游那条提交的 subject / 远端 URL
    info.upstreamSubject = Trim(RunOut(gitExe, repoRoot,
                                       {L"log", L"-1", L"--format=%s", info.upstream}));
    std::wstring url = Trim(RunOut(gitExe, repoRoot, {L"remote", L"get-url",
                                                      info.upstream.substr(0, info.upstream.find(L'/'))}));
    info.remoteUrl = url;
    info.ok = true;
    return info;
}

std::vector<std::wstring> LocalOnlyCommits(const std::wstring& gitExe, const std::wstring& repoRoot,
                                           const RemoteInfo& info, size_t limit) {
    std::vector<std::wstring> out;
    if (!info.hasUpstream || info.upstream.empty()) return out;
    const std::wstring txt = RunOut(gitExe, repoRoot,
                                    {L"rev-list", L"HEAD", L"--not", info.upstream,
                                     L"-n", std::to_wstring(limit)});
    for (const auto& line : SplitLines(txt)) {
        out.push_back(Trim(line));
    }
    return out;
}

// ------------------------------------------------------------------ 基线视图
RemoteBaseline LoadRemoteBaseline(const std::wstring& gitExe, const std::wstring& repoRoot, int limit) {
    RemoteBaseline base;
    base.info = LoadRemoteInfo(gitExe, repoRoot);

    std::set<std::wstring> localOnly;
    for (const auto& h : LocalOnlyCommits(gitExe, repoRoot, base.info)) localOnly.insert(h);
    std::set<std::wstring> remoteOnly;
    if (base.info.hasUpstream && !base.info.upstream.empty()) {
        const std::wstring txt = RunOut(gitExe, repoRoot,
                                        {L"rev-list", base.info.upstream, L"--not", L"HEAD",
                                         L"-n", std::to_wstring(limit)});
        for (const auto& line : SplitLines(txt)) remoteOnly.insert(Trim(line));
    }

    auto readCommits = [&](const std::vector<std::wstring>& revArgs, bool localSection,
                           std::vector<BaselineCommit>* out) {
        std::vector<std::wstring> argv{L"log", L"--first-parent", L"-n", std::to_wstring(limit),
                                       L"--pretty=format:%H%x1f%h%x1f%ad%x1f%s", L"--date=short"};
        argv.insert(argv.end(), revArgs.begin(), revArgs.end());
        const std::wstring txt = RunOut(gitExe, repoRoot, argv);
        for (const auto& line : SplitLines(txt)) {
            const auto f = SplitFields(line, 0x1f);
            if (f.size() < 4) continue;
            BaselineCommit c;
            c.hash = f[0];
            c.shortHash = f[1];
            c.date = f[2];
            c.subject = f[3];
            c.localOnly = localSection || localOnly.count(c.hash) != 0;
            c.remoteOnly = remoteOnly.count(c.hash) != 0;
            out->push_back(std::move(c));
        }
    };

    // 上段：本地新增（沿 HEAD 的第一父链，走到上游之前为止）
    if (base.info.hasUpstream && !base.info.upstream.empty()) {
        readCommits({L"HEAD", L"--not", base.info.upstream}, true, &base.commits);
    }
    // 下段：远端基线及以下（远端分支的第一父链；没有上游时退化成 HEAD）
    readCommits({(base.info.hasUpstream && !base.info.upstream.empty()) ? base.info.upstream : L"HEAD"},
                false, &base.commits);

    for (const auto& c : base.commits) {
        if (c.localOnly) ++base.localCount;
        else ++base.baseCount;
        if (c.remoteOnly) ++base.remoteOnlyCount;
    }
    return base;
}

std::wstring DescribeRemoteBaseline(const RemoteBaseline& base) {
    std::wstring s;
    if (!base.info.hasUpstream) {
        s += L"== 远端基线：未设置上游（本地分支没有跟踪任何远端分支）==\r\n";
        s += L"   设置上游：远端分支… 面板里选一个远端分支 →「设为上游」，或 `git branch -u origin/xxx`\r\n";
    } else {
        s += L"== 远端基线 " + base.info.upstream;
        if (!base.info.upstreamShort.empty()) s += L" (" + base.info.upstreamShort + L")";
        s += L" ==\r\n";
        if (!base.info.upstreamSubject.empty()) {
            s += L"   远端最新提交：" + base.info.upstreamSubject + L"\r\n";
        }
        s += L"   本地新增（在远端之上累加）" + std::to_wstring(base.info.ahead) + L" 条";
        s += L"，远端新增（本地还没有）" + std::to_wstring(base.info.behind) + L" 条";
        s += base.info.upToDate() ? L"   —— 与远端一致" : L"";
        s += L"\r\n";
    }
    s += L"   【本地】= 远端还没有的提交（在上面）；【远端】= 已进入远端的提交（在下面）\r\n";
    return s;
}
// ------------------------------------------------------------------ 远端分支
std::vector<RemoteBranch> LoadRemoteBranches(const std::wstring& gitExe, const std::wstring& repoRoot) {
    std::vector<RemoteBranch> out;
    const RemoteInfo info = LoadRemoteInfo(gitExe, repoRoot);
    // ★ for-each-ref 的格式串**不支持** %x1f（会原样吐字面量，实测踩过），用 %09(Tab) 当分隔符
    const std::wstring txt = RunOut(gitExe, repoRoot,
                                    {L"for-each-ref", L"--sort=-committerdate",
                                     L"--format=%(refname)%09%(refname:short)%09%(objectname:short)"
                                     L"%09%(committerdate:short)%09%(subject)",
                                     L"refs/remotes"});
    for (const auto& line : SplitLines(txt)) {
        const auto f = SplitFields(line, L'\t');
        if (f.size() < 5) continue;
        RemoteBranch b;
        b.fullName = f[0];
        b.name = f[1];
        b.shortHash = f[2];
        b.date = f[3];
        b.subject = f[4];
        for (size_t k = 5; k < f.size(); ++k) b.subject += L"\t" + f[k];   // subject 里若有 Tab，拼回去
        // origin/HEAD 是"远端默认分支"的符号引用，单独标注，不当成人可检出的分支
        // ★ 用**全名**判断：%(refname:short) 会把 origin/HEAD 显示成 "origin"，短名判不出来（踩过）
        b.isHead = b.fullName.size() >= 5 && b.fullName.substr(b.fullName.size() - 5) == L"/HEAD";
        b.isUpstream = info.hasUpstream && (b.name == info.upstream);
        out.push_back(std::move(b));
    }
    return out;
}

std::wstring DescribeRemoteBranches(const std::vector<RemoteBranch>& branches) {
    std::wstring s;
    if (branches.empty()) return L"（没有远端分支：先「抓取」一个配置了远端的仓库）\r\n";
    for (const auto& b : branches) {
        s += b.name;
        s += L"  ";
        s += b.shortHash;
        s += L"  ";
        s += b.date;
        s += L"  ";
        s += b.subject;
        if (b.isHead) s += L"  [远端默认分支]";
        if (b.isUpstream) s += L"  [当前上游]";
        s += L"\r\n";
    }
    return s;
}

// ------------------------------------------------------------------ 上游设置
std::wstring SetUpstream(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const std::wstring& upstreamName) {
    const std::wstring up = Trim(upstreamName);
    if (up.empty()) return L"请选择要跟踪的远端分支";
    if (up.find(L' ') != std::wstring::npos || up[0] == L'-') return L"上游名字不合法：" + up;
    int rc = 0;
    std::wstring err;
    RunOut(gitExe, repoRoot, {L"branch", L"--set-upstream-to=" + up}, &rc, &err);
    if (rc != 0) return err.empty() ? (L"设置上游失败：" + up) : err;
    return {};
}

// ------------------------------------------------------------------ 远端地址管理
std::vector<RemoteEntry> LoadRemotes(const std::wstring& gitExe, const std::wstring& repoRoot) {
    std::vector<RemoteEntry> out;
    const std::wstring names = RunOut(gitExe, repoRoot, {L"remote"});
    for (const auto& n : SplitLines(names)) {
        const std::wstring name = Trim(n);
        if (name.empty()) continue;
        RemoteEntry e;
        e.name = name;
        e.fetchUrl = Trim(RunOut(gitExe, repoRoot, {L"remote", L"get-url", name}));
        int rc = 0;
        e.pushUrl = Trim(RunOut(gitExe, repoRoot, {L"remote", L"get-url", L"--push", name}, &rc));
        if (rc != 0) e.pushUrl.clear();
        out.push_back(std::move(e));
    }
    return out;
}

std::wstring ValidateRemoteName(const std::wstring& name) {
    const std::wstring n = Trim(name);
    if (n.empty()) return L"远端名字不能为空（常用 origin）";
    if (n.size() > 64) return L"远端名字太长";
    for (const wchar_t c : n) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                        (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.';
        if (!ok) return L"远端名字只能有字母数字与 - _ . ：" + n;
    }
    if (n[0] == L'-') return L"远端名字不能以 - 开头";
    return {};
}

std::wstring ValidateRemoteUrl(const std::wstring& url) {
    const std::wstring u = Trim(url);
    if (u.empty()) return L"远端地址不能为空";
    if (u.size() > 4096) return L"远端地址太长";
    if (u[0] == L'-') return L"远端地址不能以 - 开头";
    for (const wchar_t c : u) {
        if (c < 0x20 || c == L'"' || c == L'\'' || c == L'`') {
            return L"远端地址里有不允许的字符（空白/引号/控制字符）";
        }
    }
    return {};
}

std::wstring AddRemote(const std::wstring& gitExe, const std::wstring& repoRoot,
                       const std::wstring& name, const std::wstring& url) {
    if (const std::wstring e = ValidateRemoteName(name); !e.empty()) return e;
    if (const std::wstring e = ValidateRemoteUrl(url); !e.empty()) return e;
    for (const auto& r : LoadRemotes(gitExe, repoRoot)) {
        if (r.name == Trim(name)) return L"远端已存在：" + Trim(name) + L"（可以改地址）";
    }
    int rc = 0;
    std::wstring err;
    RunOut(gitExe, repoRoot, {L"remote", L"add", Trim(name), Trim(url)}, &rc, &err);
    if (rc != 0) return err.empty() ? L"添加远端失败" : err;
    return {};
}

std::wstring SetRemoteUrl(const std::wstring& gitExe, const std::wstring& repoRoot,
                          const std::wstring& name, const std::wstring& url) {
    if (const std::wstring e = ValidateRemoteName(name); !e.empty()) return e;
    if (const std::wstring e = ValidateRemoteUrl(url); !e.empty()) return e;
    bool exists = false;
    for (const auto& r : LoadRemotes(gitExe, repoRoot)) {
        if (r.name == Trim(name)) exists = true;
    }
    if (!exists) return L"没有这个远端：" + Trim(name) + L"（可以先「添加远端」）";
    int rc = 0;
    std::wstring err;
    RunOut(gitExe, repoRoot, {L"remote", L"set-url", Trim(name), Trim(url)}, &rc, &err);
    if (rc != 0) return err.empty() ? L"改远端地址失败" : err;
    return {};
}

std::wstring RemoveRemote(const std::wstring& gitExe, const std::wstring& repoRoot,
                          const std::wstring& name) {
    if (const std::wstring e = ValidateRemoteName(name); !e.empty()) return e;
    int rc = 0;
    std::wstring err;
    RunOut(gitExe, repoRoot, {L"remote", L"remove", Trim(name)}, &rc, &err);
    if (rc != 0) return err.empty() ? L"删除远端失败" : err;
    return {};
}
std::wstring FetchRemote(const std::wstring& gitExe, const std::wstring& repoRoot,
                         const std::wstring& remote) {
    std::vector<std::wstring> argv{L"fetch", L"--prune"};
    const std::wstring r = Trim(remote);
    if (!r.empty()) argv.push_back(r);
    int rc = 0;
    std::wstring err;
    RunOut(gitExe, repoRoot, argv, &rc, &err);
    if (rc != 0) return err.empty() ? L"抓取失败（exit=" + std::to_wstring(rc) + L"）" : err;
    return {};
}

}  // namespace grt
