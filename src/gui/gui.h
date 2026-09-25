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
    bool         hasWork = false;  // 是否走 CLI 分支（不建窗口）
};
int RunCliCommand(const CliOptions& o);
int RunCliAi(const CliOptions& o);
int RunCliListCommands(const CliOptions& o);
int RunCliSquash(const CliOptions& o);   // --squash：脚本化的"合并连续提交"
int RunCliRemote(const CliOptions& o);   // --remote-info/--set-upstream：远端基线与上游
int RunCliRestore(const CliOptions& o);  // --restore：按提交还原（检出/新建分支/重置）

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
