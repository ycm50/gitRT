# GUI 与窗口（怎么加/改界面）

> 这一页回答"我要改某个窗口，得先知道什么"：窗口清单、消息与所有权约定、
> 深色模式与 DPI、以及**加一个新窗口/新控件的固定套路**。
> 结构（四个产物、两个静态库）见 [01-架构总览](01-架构总览.md)，命令表字段见 [02-命令表与参数面板](02-命令表与参数面板.md)。

## 1. 全部窗口一览（都是**自注册窗口类 + 手写 WndProc**）

全工程**没有**资源脚本对话框（没有 `DIALOGEX`），也没有 MFC/Qt：每个窗口是一个
`RegisterClassExW` + `CreateWindowExW` + `WndProc` 的组合，类名统一 `GitRT.*`。

| 窗口 | 类名 | 创建入口 | 单例字段 |
| --- | --- | --- | --- |
| 主窗口 | `GitRT.MainWindow` | `main.cpp` 里 `CreateWindowExW` | `App().main` |
| 参数面板（子窗口） | `GitRT.ParamPanel` | `CreateParamPanelEx()` | 主窗口 `AwState::panel` |
| 状态视图（子窗口） | `GitRT.StatusView` | `CreateStatusView()` | 主窗口 `AwState::view` |
| 进度窗口 | `GitRT.ProgressWindow` | `RunBuiltCommand()` | 每个任务一个 |
| 文本窗口（差异/历史/自检） | `GitRT.TextWindow` | `ShowTextWindow()` | 无（可多开） |
| AI 助手 | `GitRT.AiWindow` | `ShowAiWindow()` | 无 |
| 首选项（AI 设置） | `GitRT.SettingsWindow` | `ShowSettingsWindow()` | 无 |
| 合并提交 | `GitRT.SquashWindow` | `ShowSquashWindow()` | `App().squash` |
| 还原到提交 | `GitRT.RestoreWindow` | `ShowRestoreWindow()` | `App().restore` |
| 远端分支与地址 | `GitRT.RemoteWindow` | `ShowRemoteWindow()` | `App().remote` |
| 标签 | `GitRT.TagWindow` | `ShowTagWindow()` | `App().tags` |
| 发布 | `GitRT.ReleaseWindow` | `ShowReleaseWindow()` | `App().releases` |

"单例" = `App()` 里存着 HWND，窗口已存在时**刷新并提到前台**，不是再开一个
（标签/发布窗口被重复点开时必须刷新列表，否则显示的是过期数据 —— 有自检断言看住它）。

### 1.1 控件 ID：窗口内唯一即可（每个窗口自己一套编号）

这些窗口都不用对话框模板，`hMenu` 参数就是控件 ID，作用域**只在本窗口内**。所以几个窗口都用
`300` 开头是正常的（标签/发布/远端/还原/合并提交都是 `300, 301, 302 …`），**不需要全局唯一**。

每个窗口文件顶部都有一个 `enum : int { … }` 块，照它的习惯续号：

| 块 | 惯例 |
| --- | --- |
| `300-319` | 列表 / 状态行 / 预览框 / 日志框 / 按钮（`*_LIST` `*_STATUS` `*_PREVIEW` `*_LOG` `*_DO` `*_REFRESH` `*_CLOSE`） |
| `320-329` | 输入控件（`*_NAME` `*_TARGET` `*_MESSAGE` `*_REMOTE` `*_URL` `*_BRANCH` …） |
| `330-339` | **标签**静态文本（`*_LBL_*`）—— 标签也要建成子控件才能进"栈式"布局 |
| `340+` | 复选框（`*_ANNOTATED` `*_FORCE` `*_DRAFT` `*_PRE` …） |

另外两处是**常量基址 + 序号**，别占用同一段数字：参数面板 `IDC_PP_PARAM_BASE=4000` /
`IDC_PP_FLAG_BASE=5000`（`ToggleValue` 的值输入框用 `id + 500`），主窗口/状态视图/AI 助手/首选项
各有自己的段（700 / 600 / 900 / 1000）。

**真正会踩的坑是同一窗口内撞号**（跨窗口撞号无害）：`GetDlgItem` 会拿到先创建的那个控件，
症状是"输入框填了没反应"或"读回来是空"——因为拿到的是标签。加控件前先看这个文件的 `enum`。

## 2. 跨线程消息与所有权（最要紧的一条约定）

所有"工作线程 → UI 线程"的交接都走"`PostMessageW` 带一个 new 出来的对象"，并且**必须**用
`PostOwned`（`src/gui/post_owned.h`）：投递失败（目标窗口已销毁）就地回收。
**接收方一律用 `unique_ptr` 接管，包括"要丢弃"的分支**。理由与 25 处泄漏的来历见
[04-约定与踩过的坑 §1](04-约定与踩过的坑.md)。

消息号全在 `gui.h`（`WM_APP + N`），**新增消息必须接着编号并写清 lParam 的所有权**：

| 消息 | lParam | 谁收 |
| --- | --- | --- |
| `WM_GRT_TASK_PROGRESS` (+11) | `new int(percent)`，-1 = 不确定 | 进度窗口 / 参数面板 |
| `WM_GRT_TASK_LOG` (+12) | `new std::string`（UTF-8 一行） | 同上 |
| `WM_GRT_TASK_DONE` (+13) | `new TaskOutcome` | 同上 |
| `WM_GRT_STATUS_RELOAD` (+14) | `wp=1` = 先重读 `git status` 再刷视图 | 主窗口 |
| `WM_GRT_SHOW_STATUS` (+15) | — | 主窗口 |
| `WM_GRT_TASK_FINISHED` (+16) | `new TaskSummary` | 触发方的宿主窗口（AI 窗口靠它显示结果） |
| `WM_GRT_AI_DONE` (+17) / `WM_GRT_AI_PLAN_READY` (+18) | AI 结果 / 已校验方案 | AI 窗口 |
| `WM_GRT_AI_CONFIG_RELOAD` (+65) | — | AI 窗口（首选项保存后刷新配置行） |
| `WM_GRT_PANEL_PREVIEW` (+72) | `new std::wstring`（查看类命令的自动预览） | 参数面板 |
| `WM_GRT_RST_DONE` / `RM_DONE` / `TAG_DONE` / `REL_DONE` / `SQ_DONE` / `AUTOFETCH_DONE` | 各窗口自己的结果对象 | 对应专用窗口 |

**别在 UI 线程里跑 git。** 需要 git 的动作只有两个去处：`RunBuiltCommand*`（命令表路径），
或专用窗口里自己 `std::thread` + core 调用 + `PostOwned`。

## 3. 执行路径：两个入口，一条实现

| 入口 | 用在 | 行为 |
| --- | --- | --- |
| `RunBuiltCommandOn(target, …)` | **参数面板** | **就地**执行：不建窗口，`> git …`/输出/结束都投给参数面板的「运行结果」框；运行中按钮变「取消」 |
| `RunBuiltCommand(owner, …)` | AI 窗口（以及任何没有就地输出框的地方） | 建**进度窗口**，窗口在工作线程启动前就显示，结束后保留 |

两者的**实现是同一段**（`progress_window.cpp` 的 `RunCommandSequence`）：顺序跑
`BuiltCommand.argvList`、逐行回投、汇总结果、结束后一定发 `WM_GRT_STATUS_RELOAD(wp=1)`。
所以"执行完状态没变"这类问题只会在这一段里。

## 4. 加一个新窗口的固定套路

以标签窗口（`tag_window.cpp`）为完整样例：

1. `gui.h` 声明 `void ShowXxxWindow(HWND owner);`；需要单例就在 `AppState` 加字段；
2. 文件内 `const wchar_t* kXxxWindowClass = L"GitRT.XxxWindow";` + `EnsureXxxClass()`
   （懒注册、`hIcon = GitRTAppIcon()`）；
3. `WM_NCCREATE` 里把创建参数**拷进窗口状态**（`new` 一个 `XxxState` 存进 `GWLP_USERDATA`）。
   需要参数就走 `CreateWindowExW` 的 `lpCreateParams`，**不要**在 `WM_CREATE` 之后赋值 ——
   工作线程可能已经在 `WM_CREATE` 里启动了，晚了就是数据竞争（进度窗口头部注释专门写了这条）；
4. `WM_CREATE` 建控件 → `ThemeApply(hwnd)` → 填初值；
5. `WM_SIZE` 手工布局（一律 `Scale(px)`，见 §5）；
6. `WM_CTLCOLORSTATIC/EDIT/BTN` → `HandleCtlColor(...)`；`WM_ERASEBKGND` → `HandleEraseBkgnd(...)`；
   表格另加 `ThemeApplyToTableView(hwndListView)`；
7. 执行动作：`std::thread` + core 调用 + `PostOwned(hwnd, WM_GRT_XXX_DONE, …)`；
   结果分支里刷列表 + `PostMessageW(App().main, WM_GRT_STATUS_RELOAD, 1, 0)`；
8. `WM_NCDESTROY`：`worker.join()` + `delete st` + 清 `GWLP_USERDATA`（漏 join 会在
   线程还在跑时把窗口状态删掉）；
9. 接到命令表：`ActionOfInternal()` 加映射 + `ExecuteInternalCommand()` 加 `case` +
   `OpensDedicatedWindow()` 认它（这样参数面板把它当"启动器"，不会因为没填参数就禁用「执行」）。

**唯一一份分派**：`ActionOfInternal` 是"内部命令 → 窗口"的唯一映射，GUI 自检遍历命令表断言
没有命令落到 `Unimplemented`；`ExecuteInternalCommand` 的 `switch` **故意没有 `default:`**，
新增一种去向忘了写 `case` 在 `-Werror=switch` 下是**编译错误**。别在别处再抄一份窗口名单
（抄漏过一次：「标签列表 / 发布列表」点执行弹"未实现"）。

## 5. 深色模式与 DPI：四件必须做的事

```cpp
ThemeApply(hwnd);                                          // ① 标题栏 + 子按钮 + 主题
if (HandleCtlColor(msg, (HDC)wp, (HWND)lp, &res)) return res;   // ② 每次 WM_CTLCOLOR*
if (HandleEraseBkgnd(hwnd, (HDC)wp)) return 1;             // ③ 每次 WM_ERASEBKGND
ThemeApplyToTableView(listView);                           // ④ 每个 ListView 单独套
```

- 尺寸一律 `Scale(px)`（基准 96 DPI）、字体 `ScaleFont(pt)`；**不要写死像素**。
- `Th()` 的画刷/字体在 `ThemeInit()` 创建、`DestroyThemeGdi()` 销毁 —— 新加画刷/字体要同时加到
  这两个函数，否则换主题会漏 GDI 对象。
- 深浅色由 `IsSystemDark()`（注册表 `AppsUseLightTheme`）决定，**没有**程序内切换开关。

## 6. 主窗口的结构（`app_window.cpp`）

```
[选择仓库…] [仓库路径]                    [首选项] [AI 助手] [刷新]
┌ 命令列表（按 GroupId 分组，命令项缩进）─┐ ┌ 右侧：状态视图 或 参数面板 ─┐
│ ▸ 仓库 / 提交 / 同步 / …                │ │                              │
└─────────────────────────────────────────┘ └──────────────────────────────┘
底部状态栏：git 版本 | 仓库路径 | 暂存/修改/未跟踪/冲突 计数
```

- 列表项 → 命令 ID 的映射存在 `AwState::itemCmd`（`0` = 分组标题）；
- 选中命令 = 销毁旧面板、按新 spec 建面板并 `Layout()`；**状态面板**（1405）是特例：不建面板，
  只把状态视图显示回来；
- 定时器 `kTimerAutoFetch`（60 s）读 `config.json` 的 `autoFetchMinutes`，到点在工作线程里
  `git fetch --prune`，完成后 `WM_GRT_STATUS_RELOAD(wp=1)`；失败只写日志（离线是常态）；
- 右键菜单请求由 `AppWindowApplyRequest()` 落地：`cmdId=0` = 只切上下文、不选命令；否则
  **交叉校验 id 与 key**（DLL/GUI 版本脱节就拒绝执行），再定位列表项 + 预填选区与选项。

## 7. 自动化怎么驱动这些窗口

`tools/demo-*.ps1` 用 `Add-Type` 声明 `user32` 的 `EnumWindows/GetClassName/GetDlgItem/SendMessageW`，
然后**按类名找窗口、按 ID 找控件、发 `BM_CLICK`/`WM_SETTEXT`** 真点按钮。要点：

- 找窗口：`EnumWindows` + `GetWindowThreadProcessId` 过滤进程，再比 `GetClassNameW`；
- 命令列表是 ListBox：`LB_GETCOUNT`(0x018B) / `LB_GETTEXT`(0x0189) 找行，
  `LB_SETCURSEL`(0x0186) + 向主窗口发 `WM_COMMAND`（`(1 << 16) | 703`）模拟选中；
- 读控件文字用 `WM_GETTEXT`(0x000D)，**不要**跨进程 `GetWindowText` 读 Edit（常常读回空）；
- `WM_SETTEXT` **不会**产生 `EN_CHANGE`：脚本要自己补一条父窗口的 `WM_COMMAND` 通知，
  否则预览不刷新（验证代理面板时就靠这条）；
- 这类脚本需要交互式桌面会话，**只在本地跑**，不进 CI。

## 8. 参数面板（`param_panel.cpp`）的三个易错点

1. **创建参数必须在 `WM_NCCREATE` 就拷进状态**（`PanelCreateParams`）：`WM_CREATE` 里就要算预览
   与"忽略模式"默认值，之后再赋值会按空选区算（旧实现踩过）。
2. **布局是纵向栈**（`stack`：控件 + 高度，负高度 = 弹性拉伸），加控件 = 往栈里 push；
   忘了 push 的控件会停在 (0,0) 且不可见 —— 看起来像"没建出来"。
3. **`ToggleValue` 是两条 `CtrlInfo`**：复选框与它的值输入框各自登记（值框 key 带 `.value` 后缀），
   `SyncValues()` 靠 `valueH` 把它们关联起来；勾选状态变化时还要 `EnableWindow(valueH)`。