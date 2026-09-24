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

// 进度窗口：顺序执行 BuiltCommand 的多条命令，支持取消
void RunBuiltCommand(HWND owner, const CommandSpec& spec, const BuiltCommand& cmd);

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
    bool         hasWork = false;  // 是否走 CLI 分支（不建窗口）
};
int RunCliCommand(const CliOptions& o);
int RunCliAi(const CliOptions& o);
int RunCliListCommands(const CliOptions& o);

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
struct TaskSummary {
    bool         ok = false;
    bool         cancelled = false;
    int          exitCode = 0;
    uint64_t     ms = 0;
    std::wstring command;   // 实际执行的命令行（多行）
    std::wstring output;    // 合并输出（已截断到合理长度）
};
#define WM_GRT_TASK_FINISHED (WM_APP + 16)  // lParam = new TaskSummary
