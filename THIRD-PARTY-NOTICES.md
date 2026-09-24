# 第三方组件与许可证说明

本程序按 **MIT** 许可发布。本文件说明运行时**实际涉及**的外部组件与对应义务
（《产品设计》§5.8.4 的许可证审查结论的工程化落地）。

## 1. 本仓库链接/打包的第三方代码：**无**

- 全部源码为本仓库自研，**不链接**任何第三方库（包括 libgit2 —— 见下）。
- MSYS2 UCRT64 的 `libstdc++` / `libgcc` 以**静态**方式链入；二者属 GCC 运行库，
  按 **GCC Runtime Library Exception** 使用，不构成对本项目许可证的限制。
- 发行版**不打包** `git.exe`：本程序把它当作外部程序调用。

## 2. 运行时依赖（用户环境，不由本项目分发）

| 组件 | 用途 | 许可证 | 本项目的义务 |
| --- | --- | --- | --- |
| **Git for Windows**（`git.exe`） | 唯一的 Git 真相源（§5.4）；所有命令都由它执行 | GPL-2.0 | **只调用、不打包、不链接**：调用外部程序不产生 GPL 传染；用户自行安装 |
| Git Credential Manager | 凭据存储（可选的 `credential.helper`） | MIT | 无 |
| Git LFS | `git lfs pull` 等命令 | MIT | 无 |
| Windows 系统 DLL（`ole32` / `shlwapi` / `advapi32` / `comctl32` …） | 平台 API | 系统组件 | 无 |

## 3. 明确不引入的组件（以及原因）

| 组件 | 不用的原因 |
| --- | --- |
| **libgit2** | 官方定位不是替代 git；不执行 hooks、不支持 interactive rebase / sparse-checkout / partial clone，SHA-256 仍为实验；且 `git_status_list_new` 在大型仓库上比 CLI 慢 2.5–6 倍（§5.4 有逐条出处）。**v1 完全不链接**；若将来用于只读快路径，必须保留 CLI 回退并在诊断中标注后端 |
| **gitstatusd** | GPL-3.0，且无原生 Win32 构建、作者已声明仅极小维护 |
| **任何遥测 / 云同步 SDK** | 产品明确不做（§1.5） |

## 4. 打包产物的自检

- `GitRT.Shell.dll`：构建期由 `cmake/check_shell_exports.cmake` 强制校验
  ① 导出表只含 COM 入口；② **不依赖** `libstdc++-6.dll` / `libwinpthread-1.dll`
  （把运行时 DLL 泄漏进 Explorer 进程会直接导致菜单静默消失）。
- 发布包内容：`GitRT.msix`（稀疏身份包）+ 程序文件 + 本文件 + `LICENSE`。
  不含 `git.exe`，因此**不承担** GPL 的分发义务。
