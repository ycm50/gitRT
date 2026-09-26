# 参与开发（Contributing）

欢迎提 issue 与 PR。这个仓库的约定比较"实"，先看这 6 条能省很多来回。

## 0. 三条硬约束（不接受讨论的）

1. **零第三方依赖**：Win32 + COM + WinHTTP + 标准库。不要引入 vcpkg/Conan/Boost/Qt/任何 JSON 库，
   **测试也不要引 Catch2/gtest**（`src/tests/check.h` 45 行就够，见下）。
2. **主工具链是 MSYS2 UCRT64（MinGW-w64 g++）**，不是 MSVC（决策 D4）。
   CMake ≥ 3.25 + Ninja；`-fno-rtti`；静态链接 CRT。
3. **`packaging/identity.json` 是身份与版本号的唯一真相源**：`GRT_VERSION`、exe/DLL 的 `FileVersion`、
   MSIX 清单全部由它派生。**不要**在别处写死版本号（改了它 CMake 会自动重新 configure）。

## 1. 构建

见 [README 的「快速开始 → 方式二」](README.md#方式二从源码构建开发者)。最短路径：

```powershell
cmake --preset ucrt64-debug   -DCMAKE_MAKE_PROGRAM=A:/msys64/ucrt64/bin/ninja.exe
cmake --build build/debug
```

## 2. 提交前必须跑的四件事

```powershell
# ① 告警门禁：CI 就是这么跑的，本地提交前也建议开一次
cmake --preset ucrt64-debug -DGRT_WERROR=ON && cmake --build build/debug

# ② 三个 CTest 用例（core_selftest 是**无窗口**的单元测试，毫秒级）
ctest --test-dir build/debug --output-on-failure

# ③ 功能套件（纯 CLI + 真 git；CI 也跑这四套）
pwsh -File tools/test-remote.ps1   -Exe build/debug/src/gui/GitRT.exe
pwsh -File tools/test-squash.ps1   -Exe build/debug/src/gui/GitRT.exe
pwsh -File tools/test-clone.ps1    -Exe build/debug/src/gui/GitRT.exe
pwsh -File tools/test-tag-release.ps1 -Exe build/debug/src/gui/GitRT.exe

# ④ 全量端到端（A–C 阶段在 %TEMP% 里跑，不碰工作区；D 阶段需要开发者模式+桌面）
pwsh -File tools/test-all.ps1 -Exe build/debug/src/gui/GitRT.exe -SkipShell
```

改动碰到 **AI 设置 / Key / 端点判定 / 参数校验 / 只读白名单** 这些路径时，
请同步更新 `src/tests/core_tests.cpp` 里的断言 —— 那里是这些边界的家。

## 3. 测试写在哪（别写错地方）

| 要测的东西 | 放哪 |
| --- | --- |
| **纯函数 / 纯构造**（参数校验、argv 构造、JSON 工具、AI 响应解析、端点判定、计划安全闸门） | `src/tests/core_tests.cpp`（控制台、无窗口、`ctest -R core_selftest`） |
| 需要**真 git 仓库**的命令行端到端 | `tools/test-*.ps1` |
| 需要**窗口 / 图标 / 参数面板 / 真仓库**的 GUI 行为 | `GitRT.exe --self-test` 里的 `RunSelfTest()` |
| COM 菜单 / 清单 / 导出表 | `GitRT.ShellProbe.exe --self-test` / `--dump` |

往 `main.cpp` 的 `--self-test` 里塞纯函数断言是**反模式**：它需要一个真实窗口句柄才能跑
（这正是把 37 条断言搬进 `src/tests/` 的原因）。

## 4. 风格与行尾

- **行尾由 [`.gitattributes`](.gitattributes) 定**：源码/文档 LF，PowerShell 与 `.rc` CRLF。
  别用编辑器整篇转换行尾（那会产生整文件 diff）。缩进看 [`.editorconfig`](.editorconfig)：
  C/C++ 4 空格，CMake / `.rc` / JSON 2 空格。
- **注释写"为什么"**，尤其是踩过的坑（本仓库的注释密度就是这么来的：`★`/`踩过`/`实测` 都是真实教训）。
- 中文字符串在 `.cpp` 里可以直接写（源文件是 UTF-8）；`.rc` 由 `--codepage=65001` 处理。

## 5. 提交信息

用 `type(scope): 一句话`，正文写**问题 → 改动 → 验证**三段（能贴实测数字最好）：

```
fix(build): 版本号收敛到 identity.json 唯一真相源

问题：版本号有 5 个写入点，且已经不一致 —— …
改动：…
验证：-Werror 构建 0 告警；ctest 3/3；tools/test-*.ps1 161 通过 / 0 失败
```

## 6. 文档放哪

| 类型 | 位置 |
| --- | --- |
| 产品决策与需求 | `docs/产品设计.md` |
| 技术设计与实现（**代码注释广泛引用它的 § 编号，改动章节号请全局搜一遍**） | `docs/技术实现设计.md` |
| 平台实测结论（哪条能用/不能用） | `docs/支持矩阵.md` |
| 调研材料与 URL 依据链 | `docs/research/` |
| 安全策略 / 威胁模型 | [`SECURITY.md`](SECURITY.md) |
| 用户可感知的变化（新增/修复/变更/升级提示） | [`CHANGELOG.md`](CHANGELOG.md) —— 改了行为就顺手加一条 |

**改了文档可以顺手跑一次 `python -X utf8 tools/check-docs.py`**（**可选**，不进 CI）：它查站内链接
目标是否存在、以及表格列数是否一致（未转义的 `|` 会让整行错位、两张表粘在一起会静默渲染歪 ——
这两类都真实发生过）。要带 `-X utf8` 是因为它打印中文，而 `-X utf8` 之前在西文代码页的终端/runner
上会 `UnicodeEncodeError`（详见 [docs/改进规划.md](docs/改进规划.md) §16）。

CI（`.github/workflows/build.yml`）与本地是同一套流程；工具链的「安装 + 定位」那段逻辑在
[`.github/actions/setup-ucrt64`](.github/actions/setup-ucrt64/action.yml) —— 只有一份，别在 job 里再复制一份。
