#pragma once
// ---------------------------------------------------------------------------
// GitRT GUI 共享接口（《技术实现设计》§9）
//   主窗口 / 参数面板 / 状态视图 / 进度窗口 / 文本窗口
// ---------------------------------------------------------------------------

#include "../core/command_builder.h"
#include "../core/command_spec.h"
#include "../core/core.h"
#include "../core/git_runner.h"
#include "../core/resource_ids.h"
#include "../core/status.h"

// 注意：commctrl.h 依赖 windows.h（由 core.h 引入），必须在 core.h 之后包含
#include <commctrl.h>
#include <objbase.h>

namespace grt::gui {

// ------------------------------------------------------------------ 全局状态
struct AppState {
    std::wstring gitExe;                 // 探测到的 git.exe（空 = 未找到）
    std::wstring gitVersion;
    std::wstring repoRoot;               // 当前仓库根（空 = 未选择）
    RepoProbeResult probe;
    RepoStatus   status;
    bool         statusLoaded = false;
    std::wstring lastError;
    UINT         dpi = 96;
    HWND         main = nullptr;
    HWND         squash = nullptr;   // 合并提交窗口（单例）
    HWND         restore = nullptr;  // 还原到提交窗口（单例）
    HWND         remote = nullptr;   // 远端分支与地址窗口（单例）
    HWND         tags = nullptr;     // 标签窗口（单例，命令 2308/2309/2310）
    HWND         releases = nullptr; // 发布窗口（单例，命令 2312）
    bool         autoFetchBusy = false;   // 自动抓取进行中（防重入）
};
AppState& App();

// ------------------------------------------------------------------ 主题
struct Theme {
    bool     dark = false;
    COLORREF bg = 0, panel = 0, text = 0, subText = 0, border = 0, editBg = 0, accent = 0;
    HBRUSH   bgBrush = nullptr, panelBrush = nullptr, editBrush = nullptr;
    HFONT    fontUi = nullptr, fontBold = nullptr, fontMono = nullptr, fontSmall = nullptr;
};
Theme& Th();
void   ThemeInit(HWND sampleWnd);
void   ThemeApply(HWND hwnd);
bool   IsSystemDark();

// 在每个窗口的 WM_CTLCOLOR* 中调用；返回 true 表示已处理
bool HandleCtlColor(UINT msg, HDC dc, HWND child, LRESULT* result);

// 窗口自身的背景擦除（深色模式下用主题刷填满客户区）。
// 在各窗口的 `case WM_ERASEBKGND:` 里调用；返回 true 就直接 `return 1`。
// 为什么需要：窗口背景由**窗口类的画刷**决定，而好几个窗口注册时用的是 `COLOR_WINDOW + 1`
// （系统浅色）→ 深色模式下会出现"深色控件 + 浅色背景"的割裂（用户反馈"色彩不协调"）。
bool HandleEraseBkgnd(HWND hwnd, HDC dc);

// 表格类控件（ListView）跟随主题：深色模式下套 DarkMode_Explorer + 改背景/文字色 + 表头单独套主题。
// 注意 `ThemeApply()` 的 SetWindowTheme **不会**传给子控件，所以每个 ListView 都要单独调一次。
void ThemeApplyToTableView(HWND listView);

int  Scale(int px);
int  ScaleFont(int pt);

// ------------------------------------------------------------------ 字符串
const std::wstring& Str(UINT id);

// ------------------------------------------------------------------ 控件工具
HWND MakeChild(HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style,
               DWORD exStyle, int id, HFONT font);
void SetText(HWND h, const std::wstring& s);
// 多行文本：Win32 Edit 不认裸 LF，必须转 CRLF（见 ui_util.cpp 注释）
std::wstring ToCrlf(std::wstring s);
void SetTextMl(HWND h, const std::wstring& s);
std::wstring GetText(HWND h);
void CenterOnOwner(HWND hwnd, HWND owner);
void SubclassDarkEdit(HWND edit);
void CopyTextToClipboard(HWND owner, const std::wstring& text);

// ------------------------------------------------------------------ 业务
bool RefreshRepoStatus(HWND notify);
std::vector<std::wstring> ListBranches(ParamSource src);
std::wstring GitRootOf(const std::wstring& gitExe);

// ------------------------------------------------------------------ 窗口类名
extern const wchar_t* kAppWindowClass;
extern const wchar_t* kParamPanelClass;
extern const wchar_t* kStatusViewClass;
extern const wchar_t* kProgressClass;
extern const wchar_t* kTextWindowClass;
void RegisterAppWindowClass();

// ------------------------------------------------------------------ 组件
// 文本窗口：差异 / 日志 / 自检结果
void ShowTextWindow(HWND owner, UINT titleRes, const std::wstring& subtitle, const std::wstring& body);

// AI 助手窗口（自然语言 → 命令方案 → 执行 → 显示结果）
void ShowAiWindow(HWND owner);

// GitRT 应用图标：exe 内嵌的 IDI_GRT_APP —— 与右键菜单里的图标是**同一张 .ico**
// （packaging/Assets/gitrt.ico 同时打进 GitRT.Shell.dll 与 GitRT.exe）。
// 加载失败时退回通用图标，绝不返回空（空会让任务栏用别的图标）。
HICON GitRTAppIcon();

// AI 设置窗口（首选项）：端点/模型/Key/超时 + 测试连接；
// Key 明文存 exe 同目录的 GitRT.ai.json（产品决策 2026-09-24）。
// notify：保存/关闭后接收 WM_GRT_AI_CONFIG_RELOAD，用来刷新区里的配置显示（AI 窗口用）
void ShowSettingsWindow(HWND owner, HWND notify = nullptr);
// 查看类命令（提交历史 / 查看差异 / 文件历史）：内容只在 core 里产出一份，
// 文本窗口与参数面板的"自动显示"共用（见 internal_commands.cpp）
bool IsViewCommand(CommandId id);
bool BuildViewText(CommandId id, const std::vector<std::wstring>& paths,
                   const std::map<std::string, std::wstring>& flags, uint16_t* titleRes, std::wstring* body);
void ShowSquashWindow(HWND owner);   // 合并提交：复选连续的提交 → 合并成一条
void ShowRestoreWindow(HWND owner);  // 还原到提交：只读检出 / 新建分支 / 重置
void ShowRemoteWindow(HWND owner);   // 远端分支与地址：抓取 / 检出 / 跟踪 / 设上游 / 改地址
// 标签窗口（阶段二）：列表 + 新建（轻量/附注/覆盖/目标修订）+ 推送（单个/全部）+ 删除（本地/远端）
void ShowTagWindow(HWND owner);
// 发布窗口（阶段二）：gh 能力探测 + Release 列表 + 创建发布（标题/说明/草稿/预发布/附件）
void ShowReleaseWindow(HWND owner);

// ------------------------------------------------------- 内部命令的"去向"（唯一一份）
// 为什么要有这个枚举：内部命令（ExecKind::Internal）以及几个走专用窗口的命令，都由
// ExecuteInternalCommand() 分派；以前分派是个 switch，**没人能回答"每个内部命令都有去处吗"**。
// 于是「标签列表 / 发布列表」在菜单里看得见、点下去却弹「该功能尚未实现」——
// 这类"菜单里有、点下去未实现"的 bug 靠人眼是看不出来的。
// 现在把映射抽成纯函数：分派用它，GUI 自检也用它遍历命令表断言"没有命令落到 Unimplemented"。
enum class InternalAction {
    Console,         // app.console：把主窗口带到前台
    Terminal,        // app.terminal
    Gitignore,       // commit.ignore
    SquashWindow,    // commit.squash
    TextWindow,      // inspect.log / inspect.diff / inspect.filelog
    StatusView,      // inspect.status
    Settings,        // app.settings
    Doctor,          // app.doctor
    Ai,              // app.ai
    RestoreWindow,   // history.restore
    RemoteWindow,    // remote.panel
    TagWindow,       // tag.list / tag.create / tag.push / tag.delete
    ReleaseWindow,   // release.list / release.create
    Unimplemented,   // 还没有实现（点了会弹「该功能尚未实现」）
};
InternalAction ActionOfInternal(CommandId id);
// 该命令是否打开"专用窗口"：参数面板据此把它当**启动器**（预览显示提示、执行直接开窗，
// 而不是拼 argv 执行）。注意这几个命令的 ExecKind 不统一（有的是 Internal，有的是 CliPanel），
// 所以判断必须集中在这里，别在别处再抄一份。
bool OpensDedicatedWindow(CommandId id);
// 自检用：某个专用窗口当前列表控件的行数（窗口没开或还没有列表 → -1）。
// 用途：断言「点开标签列表看到的行数 == LoadTags 读到的条数」，防止窗口开了却是空壳。
int TagWindowRowCount();
int ReleaseWindowRowCount();
// 自动化运行（GUI 自检 / CI）期间**抑制模态弹框**。
// 为什么需要：模态框会**阻塞调用线程**，自动化里没人去点它 → 回归的表现不是"失败"而是"永远不返回"
// （实测：分派缺一个映射时，自检的报告文件根本没生成，任务挂了一百多秒直到被外部杀掉）。
// 自检在开头打开它；用户正常使用时不受影响（没人调用它）。
void SuppressModalDialogs(bool suppress);
bool ModalDialogsSuppressed();
// AI 配置变化通知（ShowSettingsWindow 的 notify 会收到）
constexpr UINT WM_GRT_AI_CONFIG_RELOAD = WM_APP + 65;
    // 合并提交完成（lParam = new SquashResult，接收方负责 delete）
    constexpr UINT WM_GRT_SQ_DONE = WM_APP + 71;
    // 参数面板里的"查看类命令自动预览"：工作线程算好内容 → lParam = new std::wstring（接收方 delete）
    constexpr UINT WM_GRT_PANEL_PREVIEW = WM_APP + 72;
    // 还原窗口/远端窗口：工作线程完成 → lParam = new RestoreResult / 空
    constexpr UINT WM_GRT_RST_DONE = WM_APP + 73;
    constexpr UINT WM_GRT_RM_DONE = WM_APP + 74;
    constexpr UINT WM_GRT_AUTOFETCH_DONE = WM_APP + 75;   // lParam = new std::wstring(错误，空=成功)
    // 标签窗口 / 发布窗口：工作线程完成（照 remote_window 的写法，lParam 恒为 nullptr，
    // 低 8 位 = 哪个操作，0x100 = 失败；失败原因由线程先写进日志，窗口用最后一行回填状态行）
    constexpr UINT WM_GRT_TAG_DONE = WM_APP + 76;
    constexpr UINT WM_GRT_REL_DONE = WM_APP + 77;

// 进度窗口：顺序执行 BuiltCommand 的多条命令，支持取消
void RunBuiltCommand(HWND owner, const CommandSpec& spec, const BuiltCommand& cmd);

// 在 target 窗口上**就地**执行 BuiltCommand（不建进度窗口）：
// 把 "> git …"、输出、进度、结束都投给 target 的 WM_GRT_TASK_LOG / PROGRESS / DONE。
// 参数面板的"运行结果"框用它；进度窗口仍走 RunBuiltCommand。
void RunBuiltCommandOn(HWND target, const CommandSpec& spec, const BuiltCommand& cmd,
                       std::shared_ptr<CancellationToken> cancel);

// 参数面板（子窗口，返回 HWND）
HWND CreateParamPanel(HWND parent, const CommandSpec& spec, const std::vector<std::wstring>& paths);
// 带 flags 初值的版本：右键菜单的复选状态（config.json 的 flags.*）由此预填（§4.3 层级 1/2）
HWND CreateParamPanelEx(HWND parent, const CommandSpec& spec, const std::vector<std::wstring>& paths,
                        const std::map<std::string, std::wstring>& flags);
void ParamPanelRelayout(HWND panel, int x, int y, int w, int h);
void ParamPanelSetPaths(HWND panel, const std::vector<std::wstring>& paths);

// ------------------------------------------- 右键菜单请求（Shell DLL → GUI，§4.5）
struct ShellMenuRequest {
    uint16_t                            cmdId = 0;
    uint16_t                            selMask = 0;
    std::wstring                        cmdKey;      // 与 cmdId 交叉校验用
    std::wstring                        repoHint;    // 选区探测到的仓库根（可为空）
    std::vector<std::wstring>           paths;
    std::map<std::string, std::wstring> flags;
};
// 读取共享内存请求段（--request-section <name>）；失败返回 false 并填充 why
bool LoadShellMenuRequest(const std::wstring& sectionName, ShellMenuRequest* out, std::wstring* why);
// 把请求应用到已创建的主窗口（切到对应命令的参数面板并预填）
bool AppWindowApplyRequest(HWND main, const ShellMenuRequest& req);

// 状态视图（子窗口）
HWND CreateStatusView(HWND parent);
void StatusViewRelayout(HWND view, int x, int y, int w, int h);
void StatusViewReload(HWND view);
std::vector<std::wstring> StatusViewSelectedPaths(HWND view);

// 命令执行分派（Internal 命令的 GUI 实现集中在这里）
void ExecuteInternalCommand(HWND owner, const CommandSpec& spec,
                            const std::vector<std::wstring>& paths,
                            const std::map<std::string, std::wstring>* flags = nullptr);

// ------------------------------------------------- 非交互门面（§14.3，便于脚本化测试）
struct CliOptions {
    std::wstring runKey;      // --run <command-key>
    std::wstring aiPrompt;    // --ai "<自然语言>"
    std::wstring cwd;         // --cwd <dir>
    std::wstring outPath;     // --out <file>
    std::wstring pathsSpec;   // --paths "p1;p2"
    std::map<std::string, std::wstring> flags;    // --flag k[=v]
    std::map<std::string, std::wstring> params;   // --param k=v
    bool         dryRun = false;   // --dry-run（只构造 argv，不执行）
    bool         aiRun = false;    // --ai-run（AI 方案也执行）
    bool         listCommands = false;  // --list-commands（导出命令表，供测试/CI）
    std::wstring squashHashes;    // --squash "h1,h2,h3"（合并连续提交）
    std::wstring squashMessage;   // --message "合并后的提交信息"（可省，默认拼 subject）
    // ---- 远端跟踪 / 按提交还原（脚本化入口）
    std::wstring remoteInfoMode;  // --remote-info（空 = 不跑；可给 "fetch" 表示先抓取）
    std::wstring restoreHash;     // --restore <hash>
    std::wstring restoreMode;     // --mode detach|branch|soft|mixed|hard（默认 detach）
    std::wstring restoreBranch;   // --branch <名字>（仅 mode=branch）
    std::wstring setUpstream;     // --set-upstream origin/main
    std::wstring remoteAdd;       // --remote-add name=url
    std::wstring remoteSetUrl;    // --remote-set-url name=url
    std::wstring remoteRemove;    // --remote-remove name
    bool         forceRestore = false;   // --force（脏工作区也允许 hard 重置）
    // ---- 标签 / 发布（脚本化入口；命令表编号 2307-2312）
    // workKind：CLI 分派的唯一权威。选项名有歧义时（例如只给 --remote 这种
    // 修饰选项、没给 --tag-push/--tag-delete），不能靠"某个字段非空"猜意图。
    enum class CliWork { None, TagList, TagCreate, TagPush, TagDelete, ReleaseList, ReleaseCreate };
    CliWork      workKind = CliWork::None;
    bool         tagList = false;        // --tag-list
    std::wstring tagCreateName;          // --tag-create <name>
    std::wstring tagMessage;             // --message <m>（附注标签的信息；--squash 也复用它）
    bool         tagAnnotated = false;   // --annotated
    std::wstring tagTarget;              // --target <rev>（空 = HEAD）
    bool         tagForce = false;       // --force（覆盖同名标签 / 真删标签）
    bool         tagPushAll = false;     // --all（推送全部标签）
    std::wstring tagPushName;            // --tag-push <name>
    std::wstring tagDeleteName;          // --tag-delete <name>
    bool         tagDeleteRemote = false; // --remote（远端上的同名标签也删）
    std::wstring tagRemoteName;          // --remote <r>（默认 origin）
    bool         releaseList = false;    // --release-list
    std::wstring releaseTag;             // --release-create <tag>
    std::wstring releaseTitle;           // --title <t>
    std::wstring releaseNotes;           // --notes <n>
    bool         releaseGenerateNotes = false;  // --generate-notes
    bool         releaseDraft = false;          // --draft
    bool         releasePrerelease = false;     // --prerelease
    bool         releasePushTag = false;        // --push-tag
    std::vector<std::wstring> releaseAssets;    // --asset <file>（可重复）
    bool         hasWork = false;  // 是否走 CLI 分支（不建窗口）
};
int RunCliCommand(const CliOptions& o);
int RunCliAi(const CliOptions& o);
int RunCliListCommands(const CliOptions& o);
int RunCliSquash(const CliOptions& o);   // --squash：脚本化的"合并连续提交"
int RunCliRemote(const CliOptions& o);   // --remote-info/--set-upstream：远端基线与上游
int RunCliRestore(const CliOptions& o);  // --restore：按提交还原（检出/新建分支/重置）
int RunCliTag(const CliOptions& o);      // --tag-*：标签列表/新建/推送/删除
int RunCliRelease(const CliOptions& o);  // --release-*：发布列表/创建（GitHub，走 gh）

}  // namespace grt::gui

// ------------------------------------------------------------- 自定义消息
#define WM_GRT_TASK_PROGRESS (WM_APP + 11)  // lParam = new int(percent)，-1 = 不确定
#define WM_GRT_TASK_LOG      (WM_APP + 12)  // lParam = new std::string(UTF-8)
#define WM_GRT_TASK_DONE     (WM_APP + 13)  // lParam = new TaskOutcome
#define WM_GRT_STATUS_RELOAD (WM_APP + 14)  // 状态视图刷新请求
#define WM_GRT_SHOW_STATUS   (WM_APP + 15)  // 主窗口切回状态视图
#define WM_GRT_AI_DONE       (WM_APP + 17)  // lParam = new AiDone（AI 请求完成）
#define WM_GRT_AI_PLAN_READY (WM_APP + 18)  // AI 方案已校验，可由窗口执行

// 命令执行结束的汇总（进度窗口 → 宿主窗口；接收方负责 delete）
// 一条 BuiltCommand 跑完后的汇总（进度窗口与参数面板的"运行结果"框共用）
struct TaskOutcome {
    bool         ok = false;
    bool         cancelled = false;
    int          exitCode = 0;
    uint64_t     ms = 0;
    std::wstring failedCommand;
    std::string  err;
    std::string  out;      // 合并输出（供宿主窗口显示"运行结果"）
};
struct TaskSummary {
    bool         ok = false;
    bool         cancelled = false;
    int          exitCode = 0;
    uint64_t     ms = 0;
    std::wstring command;   // 实际执行的命令行（多行）
    std::wstring output;    // 合并输出（已截断到合理长度）
};
#define WM_GRT_TASK_FINISHED (WM_APP + 16)  // lParam = new TaskSummary
