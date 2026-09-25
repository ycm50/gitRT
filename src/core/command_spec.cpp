#include "command_spec.h"

namespace grt {

// ---------------------------------------------------------------------- flags
namespace {

// sync.pull
constexpr FlagSpec kPullFlags[] = {
    {"rebase",   "--rebase",   FlagKind::Radio, "pull-strategy", IDS_FLAG_REBASE,   true,  false},
    {"ff-only",  "--ff-only",  FlagKind::Radio, "pull-strategy", IDS_FLAG_FF_ONLY,  false, false},
    {"prune",    "--prune",    FlagKind::Toggle, nullptr,        IDS_FLAG_PRUNE,    false, false},
    {"autostash", "--autostash", FlagKind::Toggle, nullptr,       IDS_FLAG_AUTOSTASH, false, false},
};

// sync.push
constexpr FlagSpec kPushFlags[] = {
    {"set-upstream",     "--set-upstream",     FlagKind::Toggle,   nullptr, IDS_FLAG_SET_UPSTREAM, true,  false},
    {"tags",             "--tags",             FlagKind::Toggle,   nullptr, IDS_FLAG_TAGS,         false, false},
    {"force-with-lease", "--force-with-lease", FlagKind::Dangerous, nullptr, IDS_FLAG_FORCE_WITH_LEASE, false, true},
    {"dry-run",          "--dry-run",          FlagKind::Toggle,   nullptr, IDS_FLAG_DRY_RUN,      false, false},
};

// sync.fetch
constexpr FlagSpec kFetchFlags[] = {
    {"all",   "--all",   FlagKind::Toggle, nullptr, IDS_FLAG_ALL,   true,  false},
    {"prune", "--prune", FlagKind::Toggle, nullptr, IDS_FLAG_PRUNE, true,  false},
    {"tags",  "--tags",  FlagKind::Toggle, nullptr, IDS_FLAG_TAGS,  false, false},
};

// commit.commit / commit.amend
constexpr FlagSpec kCommitFlags[] = {
    {"amend",     "--amend",     FlagKind::Toggle,   nullptr, IDS_FLAG_AMEND,     false, false},
    {"signoff",   "--signoff",   FlagKind::Toggle,   nullptr, IDS_FLAG_SIGNOFF,   false, false},
    {"no-verify", "--no-verify", FlagKind::Dangerous, nullptr, IDS_FLAG_NO_VERIFY, false, true},
};

constexpr FlagSpec kAmendFlags[] = {
    {"signoff", "--signoff", FlagKind::Toggle, nullptr, IDS_FLAG_SIGNOFF, false, false},
};

// commit.stage / commit.discard
constexpr FlagSpec kStageFlags[] = {
    {"intent-to-add", "--intent-to-add", FlagKind::Toggle, nullptr, IDS_FLAG_INTENT_TO_ADD, false, false},
};

// commit.unstage
constexpr FlagSpec kUnstageFlags[] = {
    {"staged", nullptr, FlagKind::Toggle, nullptr, IDS_FLAG_STAGED, false, false},
};

// stash.push
constexpr FlagSpec kStashPushFlags[] = {
    {"include-untracked", "--include-untracked", FlagKind::Toggle, nullptr, IDS_FLAG_INCLUDE_UNTRACKED, true,  false},
    {"keep-index",        "--keep-index",        FlagKind::Toggle, nullptr, IDS_FLAG_KEEP_INDEX,        false, false},
};

// stash.pop
constexpr FlagSpec kStashPopFlags[] = {
    {"index", "--index", FlagKind::Toggle, nullptr, IDS_FLAG_INDEX, false, false},
};

// branch.new
constexpr FlagSpec kBranchNewFlags[] = {
    {"track", "--track={v}", FlagKind::Value, nullptr, IDS_FLAG_TRACK_VALUE, false, false},
};

// branch.merge
constexpr FlagSpec kMergeFlags[] = {
    {"no-ff",  "--no-ff",  FlagKind::Toggle, nullptr, IDS_FLAG_NO_FF,  false, false},
    {"squash", "--squash", FlagKind::Toggle, nullptr, IDS_FLAG_SQUASH, false, false},
};

// branch.rebase
constexpr FlagSpec kRebaseFlags[] = {
    {"interactive", "--interactive", FlagKind::Dangerous, nullptr, IDS_FLAG_INTERACTIVE, false, true},
    {"autostash",   "--autostash",   FlagKind::Toggle,    nullptr, IDS_FLAG_AUTOSTASH,   false, false},
};

// branch.delete
constexpr FlagSpec kBranchDeleteFlags[] = {
    {"force", "-D", FlagKind::Dangerous, nullptr, IDS_FLAG_FORCE_DELETE, false, true},
};

// tag.create：轻量（默认）/ 附注（-a -m，annotated 由界面/CLI 读）/ 覆盖（-f）/ 目标修订
//   · annotated 是**纯 UI 开关**（gitArg=nullptr）：它决定 argv 形状，不是简单追加一个 flag
//   · target 是值型 → --target=<rev>，空值不产出（默认 = HEAD）
constexpr FlagSpec kTagCreateFlags[] = {
    {"annotated", nullptr,        FlagKind::Toggle,   nullptr, IDS_FLAG_TAG_ANNOTATED, false, false},
    {"force",     "-f",           FlagKind::Dangerous, nullptr, IDS_FLAG_TAG_FORCE,    false, true},
    {"target",    "--target={v}", FlagKind::Value,    nullptr, IDS_FLAG_TAG_TARGET,     false, false},
};

// tag.push：推哪个 / 推全部 / 远端（remote 是纯 UI 值，由界面读出来传给 git push）
constexpr FlagSpec kTagPushFlags[] = {
    {"all",    "--tags", FlagKind::Toggle, nullptr, IDS_FLAG_PUSH_ALL_TAGS, false, false},
    {"remote", nullptr,  FlagKind::Value,  nullptr, IDS_FLAG_REMOTE_VALUE,  false, false},
};

// tag.delete：远端也删（两个都是纯 UI，由界面/CLI 决定命令形状）
constexpr FlagSpec kTagDeleteFlags[] = {
    {"remote",     nullptr, FlagKind::Toggle, nullptr, IDS_FLAG_DELETE_REMOTE, false, false},
    {"remoteName", nullptr, FlagKind::Value,  nullptr, IDS_FLAG_REMOTE_VALUE,  false, false},
};

// release.create（gh，不是 git）：标题是必填参数 tag，其余走 flags
constexpr FlagSpec kReleaseCreateFlags[] = {
    {"title",          "--title={v}",   FlagKind::Value,  nullptr, IDS_FLAG_RELEASE_TITLE,  false, false},
    {"notes",          "--notes={v}",   FlagKind::Value,  nullptr, IDS_FLAG_RELEASE_NOTES,  false, false},
    {"generate-notes", "--generate-notes", FlagKind::Toggle, nullptr, IDS_FLAG_GENERATE_NOTES, false, false},
    {"draft",          "--draft",       FlagKind::Toggle, nullptr, IDS_FLAG_DRAFT,          false, false},
    {"prerelease",     "--prerelease",  FlagKind::Toggle, nullptr, IDS_FLAG_PRERELEASE,     false, false},
};

// inspect.diff
constexpr FlagSpec kDiffFlags[] = {
    {"staged",    "--staged",    FlagKind::Toggle, nullptr, IDS_FLAG_STAGED,    false, false},
    {"stat",      "--stat",      FlagKind::Toggle, nullptr, IDS_FLAG_STAT,      false, false},
    {"name-only", "--name-only", FlagKind::Toggle, nullptr, IDS_FLAG_NAME_ONLY, false, false},
};

// adv.clean
constexpr FlagSpec kCleanFlags[] = {
    {"dirs",    "-d", FlagKind::Toggle,    nullptr, IDS_FLAG_CLEAN_DIRS,    true,  false},
    {"ignored", "-x", FlagKind::Dangerous, nullptr, IDS_FLAG_CLEAN_IGNORED, false, true},
};

// adv.reset（Radio：三种模式互斥，默认 mixed）
constexpr FlagSpec kResetFlags[] = {
    {"soft",  "--soft",  FlagKind::Radio, "reset-mode", IDS_FLAG_RESET_SOFT,  false, false},
    {"mixed", "--mixed", FlagKind::Radio, "reset-mode", IDS_FLAG_RESET_MIXED, true,  false},
    {"hard",  "--hard",  FlagKind::Radio, "reset-mode", IDS_FLAG_RESET_HARD,  false, true},
};

// adv.maintenance
constexpr FlagSpec kMaintenanceFlags[] = {
    {"aggressive", "--aggressive", FlagKind::Toggle, nullptr, IDS_FLAG_AGGRESSIVE, false, false},
};

// repo.init
constexpr FlagSpec kInitFlags[] = {
    {"branch", "--initial-branch={v}", FlagKind::Value, nullptr, IDS_FLAG_BRANCH_VALUE, false, false},
};

// repo.clone
// 克隆选项：顺序即 argv 顺序（表内声明顺序 = 稳定输出）
//   depth / branch / filter 是「值型」——用 --opt={v} 形式，保证是**一个** argv 元素
constexpr FlagSpec kCloneFlags[] = {
    {"depth", "--depth={v}", FlagKind::Value, nullptr, IDS_FLAG_CLONE_DEPTH, false, false},
    {"single-branch", "--single-branch", FlagKind::Toggle, nullptr, IDS_FLAG_CLONE_SINGLE, false, false},
    {"branch", "--branch={v}", FlagKind::Value, nullptr, IDS_FLAG_CLONE_BRANCH, false, false},
    {"no-tags", "--no-tags", FlagKind::Toggle, nullptr, IDS_FLAG_CLONE_NO_TAGS, false, false},
    {"filter", "--filter={v}", FlagKind::Value, nullptr, IDS_FLAG_CLONE_FILTER, false, false},
    {"recurse-submodules", "--recurse-submodules", FlagKind::Toggle, nullptr, IDS_FLAG_RECURSE_SUBMODULE, false, false},
};

// app.terminal：纯 UI 单选（不进 argv），由 GUI 读取后启动对应终端
constexpr FlagSpec kTerminalFlags[] = {
    {"git-bash",    nullptr, FlagKind::Radio, "terminal", IDS_TERM_GIT_BASH,   false, false},
    {"powershell",  nullptr, FlagKind::Radio, "terminal", IDS_TERM_POWERSHELL, false, false},
    {"windows-terminal", nullptr, FlagKind::Radio, "terminal", IDS_TERM_WT,   true,  false},
    {"cmd",         nullptr, FlagKind::Radio, "terminal", IDS_TERM_CMD,        false, false},
};

constexpr CommandSpec C(CommandId id, const char* key, GroupId g, uint16_t title, SelMask sel,
                        bool needRepo, Danger dg, ParamKind pk, ParamSource ps, const char* pkey,
                        ExecKind ex, const FlagSpec* fl, uint8_t fn, const char* tmpl) {
    return CommandSpec{id, key, g, title, 0, sel, needRepo, dg, pk, ps, pkey, ex, fl, fn, tmpl};
}

}  // namespace

// ------------------------------------------------------------------ 命令表
// 见《技术实现设计》§3.5。未在本表出现的命令（tag.create / adv.submodule / adv.lfs /
// worktree 等）按文档 §14.4 实施顺序在后续增量补齐——ID 只增不改。
const CommandSpec kCommands[] = {
    // ------------------------------------------------------------ 仓库
    C(1001, "repo.init", GroupId::Repo, IDS_CMD_REPO_INIT,
      kSelDir | kSelBg, false, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliPanel, kInitFlags, 1, "git init {flags}"),
    C(1002, "repo.clone", GroupId::Repo, IDS_CMD_REPO_CLONE,
      kSelDir | kSelBg, false, Danger::Safe, ParamKind::Url, ParamSource::None, "url",
      ExecKind::CliStream, kCloneFlags, 6, "git clone {flags} {url}"),
    C(1003, "app.console", GroupId::Repo, IDS_CMD_APP_CONSOLE,
      kSelAny, false, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(1004, "app.terminal", GroupId::Repo, IDS_CMD_APP_TERMINAL,
      kSelDir | kSelBg, false, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, kTerminalFlags, 4, nullptr),

    // ------------------------------------------------------------ 提交
    C(1101, "commit.stage", GroupId::Commit, IDS_CMD_COMMIT_STAGE,
      kSelAny, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Cli, kStageFlags, 1, "git add -A {flags} {paths}"),
    C(1102, "commit.unstage", GroupId::Commit, IDS_CMD_COMMIT_UNSTAGE,
      kSelAny, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Cli, nullptr, 0, "git restore --staged {paths}"),
    C(1103, "commit.discard", GroupId::Commit, IDS_CMD_COMMIT_DISCARD,
      kSelAny, true, Danger::Destructive, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliPanel, nullptr, 0, "git restore --worktree {paths}"),
    C(1104, "commit.ignore", GroupId::Commit, IDS_CMD_COMMIT_IGNORE,
      kSelAny, true, Danger::Safe, ParamKind::Pattern, ParamSource::None, "pattern",
      ExecKind::Internal, nullptr, 0, nullptr),
    C(1105, "commit.commit", GroupId::Commit, IDS_CMD_COMMIT_COMMIT,
      kSelAny, true, Danger::Safe, ParamKind::CommitMessage, ParamSource::None, "msg",
      ExecKind::CliPanel, kCommitFlags, 3, "git commit {flags} -m {msg}"),
    C(1106, "commit.amend", GroupId::Commit, IDS_CMD_COMMIT_AMEND,
      kSelAny, true, Danger::Careful, ParamKind::CommitMessage, ParamSource::None, "msg",
      ExecKind::CliPanel, kAmendFlags, 1, "git commit --amend {flags} -m {msg}"),
    C(1107, "commit.undo", GroupId::Commit, IDS_CMD_COMMIT_UNDO,
      kSelDir | kSelBg, true, Danger::Destructive, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliPanel, nullptr, 0, "git reset --soft HEAD~1"),
    // 合并提交：复选连续的提交 → 合成一条（窗口 gui/squash_window.cpp；核心 core/squash.cpp）
    // Internal：交互在"复选列表"里完成，不由参数面板拼 argv（脚本/CI 用 GitRT.exe --squash）
    C(1108, "commit.squash", GroupId::Commit, IDS_CMD_COMMIT_SQUASH,
      kSelDir | kSelBg, true, Danger::Destructive, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),

    // ------------------------------------------------------------ 同步
    C(2109, "history.restore", GroupId::Inspect, IDS_CMD_HISTORY_RESTORE,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(2110, "remote.panel", GroupId::Sync, IDS_CMD_REMOTE_PANEL,
      kSelDir | kSelBg, false, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(1201, "sync.pull", GroupId::Sync, IDS_CMD_SYNC_PULL,
      kSelDir | kSelBg, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliStream, kPullFlags, 4, "git pull {flags}"),
    C(1202, "sync.push", GroupId::Sync, IDS_CMD_SYNC_PUSH,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliStream, kPushFlags, 4, "git push {flags}"),
    C(1203, "sync.fetch", GroupId::Sync, IDS_CMD_SYNC_FETCH,
      kSelDir | kSelBg, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliStream, kFetchFlags, 3, "git fetch {flags}"),
    C(1204, "sync.sync", GroupId::Sync, IDS_CMD_SYNC_SYNC,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliStream, nullptr, 0, "git pull --rebase --autostash ; git push"),

    // ------------------------------------------------------- 分支与标签
    C(1301, "branch.switch", GroupId::Branch, IDS_CMD_BRANCH_SWITCH,
      kSelDir | kSelBg, true, Danger::Safe, ParamKind::ExistingBranch, ParamSource::LocalBranches, "branch",
      ExecKind::Cli, nullptr, 0, "git switch {branch}"),
    C(1302, "branch.new", GroupId::Branch, IDS_CMD_BRANCH_NEW,
      kSelDir | kSelBg, true, Danger::Safe, ParamKind::BranchName, ParamSource::None, "branch",
      ExecKind::CliPanel, kBranchNewFlags, 1, "git switch -c {branch} {flags}"),
    C(1303, "branch.merge", GroupId::Branch, IDS_CMD_BRANCH_MERGE,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::ExistingBranch, ParamSource::AllBranches, "branch",
      ExecKind::CliStream, kMergeFlags, 2, "git merge {flags} {branch}"),
    C(1304, "branch.rebase", GroupId::Branch, IDS_CMD_BRANCH_REBASE,
      kSelDir | kSelBg, true, Danger::Destructive, ParamKind::ExistingBranch, ParamSource::AllBranches, "branch",
      ExecKind::CliStream, kRebaseFlags, 2, "git rebase {flags} {branch}"),
    C(1306, "branch.delete", GroupId::Branch, IDS_CMD_BRANCH_DELETE,
      kSelDir | kSelBg, true, Danger::Destructive, ParamKind::ExistingBranch, ParamSource::LocalBranches, "branch",
      ExecKind::CliPanel, kBranchDeleteFlags, 1, "git branch {flags} {branch}"),

    // ------------------------------------------------------------ 标签与发布
    // 标签（tag.*）与发布（release.*）：
    //   · 视图类（tag.list / release.list）走 Internal，与 history.* 一致（GUI 窗口里渲染）
    //   · 有参数的写操作走 CliPanel（参数面板拼 argv，与 branch.delete 同一套路）
    //   ★ 发布走 gh（GitHub CLI）而不是 git，面板里会把命令原样给用户看（Careful：会公开）
    C(2307, "tag.list", GroupId::Inspect, IDS_CMD_TAG_LIST,
      kSelDir | kSelBg, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(2308, "tag.create", GroupId::Branch, IDS_CMD_TAG_CREATE,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::TagName, ParamSource::None, "name",
      ExecKind::CliPanel, kTagCreateFlags, 3, "git tag {flags} {name}"),
    C(2309, "tag.push", GroupId::Branch, IDS_CMD_TAG_PUSH,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::TagName, ParamSource::Tags, "name",
      ExecKind::CliPanel, kTagPushFlags, 2, "git push {flags} {name}"),
    C(2310, "tag.delete", GroupId::Branch, IDS_CMD_TAG_DELETE,
      kSelDir | kSelBg, true, Danger::Destructive, ParamKind::TagName, ParamSource::Tags, "name",
      ExecKind::CliPanel, kTagDeleteFlags, 2, "git tag -d {name}"),
    C(2311, "release.list", GroupId::Inspect, IDS_CMD_RELEASE_LIST,
      kSelDir | kSelBg, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(2312, "release.create", GroupId::Branch, IDS_CMD_RELEASE_CREATE,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::TagName, ParamSource::Tags, "tag",
      ExecKind::CliPanel, kReleaseCreateFlags, 5, "gh release create {tag} {flags}"),

    // ------------------------------------------------------------ 查看
    C(1401, "inspect.log", GroupId::Inspect, IDS_CMD_INSPECT_LOG,
      kSelDir | kSelBg, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(1402, "inspect.diff", GroupId::Inspect, IDS_CMD_INSPECT_DIFF,
      kSelAny, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, kDiffFlags, 3, nullptr),
    C(1403, "inspect.filelog", GroupId::Inspect, IDS_CMD_INSPECT_FILELOG,
      kSelFile, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(1405, "inspect.status", GroupId::Inspect, IDS_CMD_INSPECT_STATUS,
      kSelAny, true, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),

    // ------------------------------------------------------------ 储藏
    C(1501, "stash.push", GroupId::Stash, IDS_CMD_STASH_PUSH,
      kSelAny, true, Danger::Safe, ParamKind::Text, ParamSource::None, "msg",
      ExecKind::CliPanel, kStashPushFlags, 2, "git stash push {flags} -m {msg}"),
    C(1503, "stash.pop", GroupId::Stash, IDS_CMD_STASH_POP,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliStream, kStashPopFlags, 1, "git stash pop {flags}"),

    // ------------------------------------------------------------ 高级
    C(1601, "adv.clean", GroupId::Advanced, IDS_CMD_ADV_CLEAN,
      kSelDir | kSelBg, true, Danger::Destructive, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliPanel, kCleanFlags, 2, "git clean -nd ; git clean -f {flags}"),
    C(1602, "adv.reset", GroupId::Advanced, IDS_CMD_ADV_RESET,
      kSelDir | kSelBg, true, Danger::Destructive, ParamKind::Revision, ParamSource::None, "rev",
      ExecKind::CliPanel, kResetFlags, 3, "git reset {flags} {rev}"),
    C(1607, "adv.maintenance", GroupId::Advanced, IDS_CMD_ADV_MAINTENANCE,
      kSelDir | kSelBg, true, Danger::Careful, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::CliStream, kMaintenanceFlags, 1, "git gc {flags}"),

    // ------------------------------------------------------------ 应用
    C(1701, "app.settings", GroupId::App, IDS_CMD_APP_SETTINGS,
      kSelAny, false, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(1702, "app.doctor", GroupId::App, IDS_CMD_APP_DOCTOR,
      kSelAny, false, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
    C(1703, "app.ai", GroupId::App, IDS_CMD_APP_AI,
      kSelAny, false, Danger::Safe, ParamKind::None, ParamSource::None, nullptr,
      ExecKind::Internal, nullptr, 0, nullptr),
};

const size_t kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

const CommandSpec* CommandTable() { return kCommands; }
size_t             CommandTableSize() { return kCommandCount; }

const CommandSpec* FindCommand(CommandId id) {
    for (const auto& c : kCommands)
        if (c.id == id) return &c;
    return nullptr;
}

const CommandSpec* FindCommandByKey(std::string_view key) {
    for (const auto& c : kCommands)
        if (key == c.key) return &c;
    return nullptr;
}

const wchar_t* GroupName(GroupId g) {
    switch (g) {
        case GroupId::Repo:     return L"\u4ed3\u5e93";          // 仓库
        case GroupId::Commit:   return L"\u63d0\u4ea4";          // 提交
        case GroupId::Sync:     return L"\u540c\u6b65";          // 同步
        case GroupId::Branch:   return L"\u5206\u652f\u4e0e\u6807\u7b7e";  // 分支与标签
        case GroupId::Inspect:  return L"\u67e5\u770b";          // 查看
        case GroupId::Stash:    return L"\u50a8\u85cf";          // 储藏
        case GroupId::Advanced: return L"\u9ad8\u7ea7";          // 高级
        case GroupId::App:      return L"\u5e94\u7528";          // 应用
        default:                return L"";
    }
}

const char* GroupKey(GroupId g) {
    switch (g) {
        case GroupId::Repo:     return "repo";
        case GroupId::Commit:   return "commit";
        case GroupId::Sync:     return "sync";
        case GroupId::Branch:   return "branch";
        case GroupId::Inspect:  return "inspect";
        case GroupId::Stash:    return "stash";
        case GroupId::Advanced: return "advanced";
        case GroupId::App:      return "app";
        default:                return "?";
    }
}

const FlagSpec* FindFlag(const CommandSpec& spec, std::string_view key) {
    for (uint8_t i = 0; i < spec.flagCount; ++i)
        if (key == spec.flags[i].key) return &spec.flags[i];
    return nullptr;
}

}  // namespace grt
