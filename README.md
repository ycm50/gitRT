# GitRT（工作代号）

> 把 Git 的高频操作放回你**已经在的地方**——Windows 11 资源管理器的**现代右键菜单**；
> 用「分层菜单 + 复选开关 + 参数面板」让图形界面具备命令行级表达力，而用户不必记住任何命令。

本仓库是 **C++20 实现**（决策记录见 [docs/产品设计.md](docs/产品设计.md) §0）。

---

## 当前状态

| 层 | 状态 |
| --- | --- |
| 命令模型 / 参数构造 / git 执行引擎 | ✅ 32 条命令，声明式命令表 |
| GUI（参数面板、进度窗口、状态面板、差异/历史、AI 助手） | ✅ |
| CLI 门面（脚本化/CI） | ✅ `--run` `--dry-run` `--list-commands` `--out` … |
| **Shell 扩展 `GitRT.Shell.dll`（`IExplorerCommand`）** | ✅ 右键菜单里**只有一个「GitRT」入口：点击打开 GitRT 主窗口**（并把上下文切到该目录）；`"menu.mode": "tree"` 可切回分层菜单（8 分组 + 可选直达项/内联开关） |
| 稀疏身份包清单 + 开发循环/打包脚本 | ✅ 已在本机**免签名注册成功**（`Status=Ok`，DLL 由 `DllHost.exe` 代理进程加载）；现代菜单肉眼确认见 [docs/支持矩阵.md](docs/支持矩阵.md) §4.1 |
| Broker（状态缓存 / 目录监视 / 共享内存快照） | ⬜ 下一增量 |
| 图标覆盖（overlay icons） | ❌ 明确不做（与"只做现代菜单"的决策冲突，见产品文档 §3.9） |

进度与偏差的完整记录见 [docs/技术实现设计.md](docs/技术实现设计.md) §15、§16；
M0 实测结论见 [docs/支持矩阵.md](docs/支持矩阵.md)。

---

## 构建

前置：MSYS2 UCRT64（g++ ≥ 13）、CMake ≥ 3.25、Ninja。

```powershell
cmake --preset ucrt64-debug   -DCMAKE_MAKE_PROGRAM=A:/msys64/ucrt64/bin/ninja.exe
cmake --build build/debug

cmake --preset ucrt64-release -DCMAKE_MAKE_PROGRAM=A:/msys64/ucrt64/bin/ninja.exe
cmake --build build/release
```

产物：

| 产物 | 位置 | 说明 |
| --- | --- | --- |
| `GitRT.exe` | `build/<cfg>/src/gui/` | GUI 主程序（`-mwindows`；脚本里用 `Start-Process -Wait` 调用） |
| `GitRT.Shell.dll` | `build/<cfg>/src/shell/` | 右键菜单扩展（构建期强制校验：导出表 + 无运行时 DLL 依赖） |
| `GitRT.ShellProbe.exe` | `build/<cfg>/src/shellprobe/` | 「无资源管理器」验证工具（`--dump` / `--self-test` / `--invoke`） |

构建期选项：`GRT_DEV_REGISTER`（默认 Debug=ON / Release=OFF，控制 `DllRegisterServer`）、
`GRT_SHELL_PROBE`（菜单构建期探针日志）、`GRT_BUILD_TOOLS`、`GRT_WERROR`。

---

## 测试

```powershell
# 1) GUI 内置自检（porcelain 固件 / 命令表 dry-run / 安全闸门 / 参数面板 32/32）
Start-Process build\debug\src\gui\GitRT.exe -ArgumentList "--self-test=$PWD\build\selftest-gui.txt" -Wait

# 2) Shell 扩展自检（COM 接口 / 菜单树 / 复选开关 / 内嵌 msix 身份 / 性能预算）
build\debug\src\shellprobe\GitRT.ShellProbe.exe --self-test=build\selftest-shell.txt `
  --dll build\debug\src\shell\GitRT.Shell.dll --identity packaging\identity.json `
  --appx build\debug\packaging\AppxManifest.xml --workdir build\debug

# 3) 看一眼菜单树（在任意目录/文件上模拟右键）
build\debug\src\shellprobe\GitRT.ShellProbe.exe --dump $PWD --dll build\debug\src\shell\GitRT.Shell.dll

# 4) 全量端到端（命令表 dry-run + 真实 git 执行断言 + AI 链路 + Shell 端到端）
#    注意：Exe 要放在工作区**外**（脚本默认 %TEMP%\GitRT-run\）
Copy-Item build\debug\src\gui\GitRT.exe, build\debug\src\shell\GitRT.Shell.dll, `
          build\debug\src\shellprobe\GitRT.ShellProbe.exe "$env:TEMP\GitRT-run\" -Force
pwsh -File tools\test-all.ps1
```

---

## 开发期注册（两条路，用途不同）

```powershell
# 路径 A：传统注册（秒级，出现在「显示更多选项」）—— 先证明"实现是对的"
pwsh -File tools\dev\register-legacy.ps1            # 注册 + 重启 explorer
pwsh -File tools\dev\register-legacy.ps1 -Unregister

# 路径 B/C：现代菜单（需要身份包；默认走免签名 loose 注册，不需要 Windows SDK）
pwsh -File packaging\scripts\dev-install.ps1 -Config Debug   # 铺文件 + 注册 + 重启 explorer
pwsh -File packaging\scripts\dev-reload.ps1                  # 只改了 DLL 时用
pwsh -File packaging\scripts\dev-uninstall.ps1               # 卸载（独立进程，避免 0x80073CFA）

# 正式打包（需要 Windows SDK 的 makeappx/signtool；本机未装）
pwsh -File packaging\scripts\build-msix.ps1 -Config Release
pwsh -File packaging\scripts\build-msix.ps1 -LayoutOnly     # 只组装稀疏包目录（免签名路径）
```

> ⚠️ 注册/升级后**必须重启 `explorer.exe`**，否则菜单不出现；只改 DLL 时不需要重新注册身份包。
> 若现代菜单不出现，先跑 `GitRT.ShellProbe.exe --dump` 排除"实现问题"，再查注册链路（`docs/支持矩阵.md` §4）。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/产品设计.md](docs/产品设计.md) | 定位、平台约束（D1–D4）、命令清单、菜单语义、里程碑、风险 |
| [docs/技术实现设计.md](docs/技术实现设计.md) | 工程结构、COM 实现、wire format、Git 引擎、打包、测试落点、**进度与偏差记录** |
| [docs/支持矩阵.md](docs/支持矩阵.md) | M0 待实测项（S1–S6）结论、场景 × 菜单可见性矩阵 |
| [research-git-context-menu-zh.md](research-git-context-menu-zh.md) | 调研材料（含逐条 URL 出处：菜单机制、git 后端、凭据、性能） |

许可证：MIT（见 [LICENSE](LICENSE)）。
