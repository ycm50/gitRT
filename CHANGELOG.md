# 更新记录（Changelog）

本文件记录**用户可感知的变化**（新增 / 修复 / 变更 / 移除）。
更早的历史没有逐条归档：工程层面的决策、实测偏差与验证证据在
[docs/技术实现设计.md](docs/技术实现设计.md) 的 §15/§16，产品决策在 [docs/产品设计.md](docs/产品设计.md)。

版本号规则：**最新 git tag + 0.0.1**（每节上限 9，进位；一个 tag 都没有时用 `1.0.0`），
见 `packaging/scripts/next-version.ps1`。仓库目前**没有正式发版**，下面全部属于「未发布」。

---

## [未发布]

一轮工程收敛：把"能跑"变成"可验证、可回溯、安全边界写清楚"。完整记录（问题 → 改动 →
验证证据 → 取舍）在 [docs/改进规划.md](docs/改进规划.md)。

### 新增

- **无窗口的核心单元测试**（`src/tests/`，CTest 用例 `core_selftest`）：
  42 项断言覆盖 porcelain 固件、命令表 argv 构造与参数校验、JSON 工具、AI 响应解析与安全闸门、
  本机端点判定、Key 加密不落明文。**零第三方依赖**（自写 45 行断言框架 `check.h`），
  不依赖窗口与桌面会话。
- **CI 静态分析**：新增 `GRT_ANALYZER` 选项与独立 job（`g++ -fanalyzer` + `-Werror`，日志作为 artifact）。
  它当场找出并修掉了 GUI 里 25 处 `PostMessageW` 交接失败路径上的**真实内存泄漏**（见"修复"）。
- **AI 助手支持本地免鉴权服务**：接口地址指向本机（`localhost` / `127.x` / `[::1]`）时
  **API Key 可以留空**，请求不带 `Authorization` 头 —— Ollama / LM Studio / vLLM / llama.cpp server
  这类本地服务不再需要"编一个假 Key"。
- **AI Key 可选用 Windows DPAPI 加密保存**（`CryptProtectData`，按当前用户）：首选项里勾选后
  文件里只留 `dpapi:<base64>`；**默认仍是明文**（保持可直接查看/修改的既有体验），不强制迁移。
- **明文 HTTP 告警**：接口地址是 `http://` 且非本机时，保存/测试会弹一次确认（默认按钮是"取消"），
  状态栏与 AI 窗口配置行常驻提醒 —— Key 与提示词（含仓库路径/分支/文件名）会明文过网。
- [`SECURITY.md`](SECURITY.md)：威胁模型、凭据处理、信任边界、审计位置索引。
- [`CONTRIBUTING.md`](CONTRIBUTING.md)：三条硬约束、提交前必跑的四件事、"测试该写在哪"对照表。
- [`.gitattributes`](.gitattributes) / [`.editorconfig`](.editorconfig)：行尾与缩进从"各机本地配置"
  变成仓库规则（此前 `git ls-files --eol` 有 130 个文件是"索引 LF / 工作区 CRLF"，全靠各机 `core.autocrlf`）。
- `packaging/scripts/stage-artifacts.ps1`：把 CI 产物归位成构建目录布局，
  于是**手动下载产物也能不重编译直接重打包**。
- `tools/check-docs.py`（**零依赖**，本机可选跑）：体检**站内链接目标是否存在**与
  **表格列数是否一致**。文档量比代码大，而 Markdown 的坏链接/粘错行不会报错、只会静默渲染歪 ——
  它上线当天就抓出 2 条死链、5 处未转义的 `|`、2 处表格结构损坏。
  用法：`python -X utf8 tools/check-docs.py`（**不进 CI**，原因见"变更"）。

### 修复

- **「标签列表」/「发布列表」点「执行」不再弹「该功能尚未实现」**（用户报告）：这两个命令的数据层
  （`LoadTags` / `LoadReleases`）与文本渲染早就有，但**没接到内部分派函数上** ——
  `ExecuteInternalCommand()` 没有它们的 `case`，于是落到 `default:` 弹出
  「该功能尚未实现（当前为 GUI 首个增量）」。菜单里看得见、点下去说没实现。
  现在它们打开**已有的标签 / 发布窗口**（列表 + 选中一行即自动填进表单 +
  打标签/推送/删除/创建发布全在同一个窗口里），与 `history.restore` / `remote.panel` 同一套打法。
  根因是"内部命令的去向"散在一个 switch 里、**没人能回答"每个命令都有去处吗"**，所以顺带做了两件事：
  - 把去向抽成唯一一份映射（`ActionOfInternal`），分派、参数面板的"是否专用窗口"判断、自检都问它；
  - 新增自检断言：**遍历命令表，凡内部命令都不许落到「未实现」**（失败时会点名是哪个命令），
    外加"窗口里的列表行数 == core 读到的条数"的抽验（只断言"窗口开了"不够，空壳窗口同样是"开了"），
    以及"重复点开要刷新"的抽验。
  CLI 侧：`--run tag.list` / `--run release.list` 会多打印一条 `hint=`，指出命令行里有
  `--tag-list` / `--release-list` 可以**直接**列出（退出码仍是 1 —— 确实什么都没执行）。
- **标签 / 发布窗口：已打开时再点一次会刷新列表**（原先是只把旧窗口提到前台）。
  「标签列表」「发布列表」这两个命令的语义就是"看当前有什么"，而期间可能在 CLI 或 gh 那边
  刚改过；不刷新就会显示过期数据。
- **回归时不再"挂死"而是"点名失败"**（做反向验证时发现的失败模式）：故意删掉一个命令的映射后，
  自检原先会**卡在模态框上永远不返回**（`MessageBoxW` 阻塞调用线程，自动化里没人去点它 →
  报告文件都不生成，CI 会拖到超时）。现在：自动化运行期间抑制模态框、自检里先确认映射再调用分派，
  同一场景变成 **2.5 秒内退出并列出 4 条失败**（含"未实现： 标签列表"）。另外给分派的 `switch`
  去掉了 `default:`，于是"新增一种去向却忘了分派"会变成 **编译错误**（`-Werror=switch`）。

- **GUI 25 处内存泄漏**（`-fanalyzer` 找出）：工作线程把结果 `new` 出来交给 `PostMessageW`，
  但目标窗口已销毁时该调用返回 `FALSE` —— 消息不投递、接收端永不 `delete`，那块内存就泄漏了
  （关掉进度窗口/标签窗口后任务才跑完即可复现）。已统一为"失败即自己回收"。
- **版本号三处漂移**：`project(VERSION 0.1.0)` / `GRT_VERSION="0.1.0"` / 资源 `FileVersion` 各自写死，
  与 `packaging/identity.json` 的 `0.1.0.0` 已经不一致（进 User-Agent、关于对话框、安装注册表）。
  现在**全部由 `identity.json` 派生**；并把它登记进 `CMAKE_CONFIGURE_DEPENDS` ——
  这是"改了 identity.json 却不生效"的根因（CMake 不会因为 `file(READ)` 过的文件变化而重新 configure，
  于是清单/资源/`clsid_values.h` 会停在上一次产物上）。
- **2 条长期存在的编译告警**（`-Wmisleading-indentation`、`-Wunused-function`），
  并新增 `GRT_WERROR` 门禁（**CI 开、本地默认关**）。
- **`Check(表达式, 消息)` 的实参求值顺序隐患**：消息里若调用会填充 `message` 的函数，
  打印出来的可能是空串（C++ 实参求值顺序未指定）。已改为先求值再拼消息。
- **文档渲染**：修掉 2 条站内死链（复合动作改名后残留）、5 处代码块里未转义的 `|`
  （GFM 会当成分隔符导致整行错位）、2 处表格结构损坏（两张表被逐行粘在一起、一个空行把表截断）。
  另外把 `docs/技术实现设计.md` §2.3 / §13.1 的"施工图 vs 实现"差异写成了显式注记，
  并修正其中**照抄会重新引入缺陷**的版本号写法。

### 变更

- **CI 里去掉了 `docs`（文档体检）job**：它在本机一直通过，但在 runner 上**10 秒就失败** ——
  脚本要打印中文，而 runner 的 stdout 是西文代码页 → `UnicodeEncodeError`（本地是 GBK 所以照过；
  已用 `PYTHONIOENCODING=cp1252` 精确复现，`python -X utf8` 可修）。按用户要求先去掉这个检查，
  工具保留在 `tools/check-docs.py` 供本机手动跑。要恢复成门禁：加回 job 并在命令里带 `-X utf8`。
- **CTest 由 2 个用例变为 3 个**（新增 `core_selftest`）；`src/gui/main.cpp` 的 `--self-test`
  只保留需要真实窗口的断言（1017 → 662 行）。
- **CI 结构瘦身**：
  - 工具链「安装包清单 + 定位路径」合并为一个复合动作 `.github/actions/setup-ucrt64`
    （此前那段 60 行候选路径逻辑在多个 job 里逐字重复）；
  - 功能套件**只在 Release 那一路跑**（纯 CLI 行为，与构建类型无关；跑两遍只是重复最慢的一段）；
  - `package` job **改为复用 build job 的 Release 产物**，不再重装 MSYS2、不再重建；
  - 新增 `analysis` job（见"新增"）。
- **core 内部接口收敛**：4 份逐字相同的 `RunOut` → `git_runner.h` 的 `RunGitOut` / `RevParseShort`；
  删除死代码 `ShortHash`。这是内部改动，不影响行为。
- **AI 配置多了一个字段**：`GitRT.ai.json` 新增 `protectKey`（`true` 时 `apiKey` 为 `dpapi:<base64>`）；
  旧文件（没有该字段）照常按明文读取。
- 根目录的调研材料移入 [`docs/research/`](docs/research/)；`docs/技术实现设计.md` 加了目录
  （锚点由脚本按 GitHub slug 规则从真实标题生成）。

### 升级提示

- 无破坏性变更，**不需要重新填写 API Key**：老的 `GitRT.ai.json`（明文）继续可用。
- 想启用加密：首选项 → 勾「用 Windows 加密保存 Key（DPAPI）」→ 保存。**换机器或换用户后解不开**，
  届时会提示重新填写（文件本身不会被清空）。
- 用本地模型的用户：把接口地址改成 `http://127.0.0.1:<端口>/v1/chat/completions`，Key 留空即可。

### 本轮验证（本机实测）

| 项 | 结果 |
| --- | --- |
| `-DGRT_WERROR=ON` 构建 | **0 告警** |
| `ctest`（Debug） | **3/3 通过** —— `core_selftest` 0.02 s / `gui_selftest` 2.0 s / `shell_selftest` 0.05 s |
| `GitRT.CoreTests.exe` | **42 通过 / 0 失败**（约 22 ms） |
| `tools/test-{remote,squash,clone,tag-release}.ps1` | **161 通过 / 0 失败 / 1 跳过** |
| `tools/test-all.ps1 -SkipShell`（Phase A–C） | **全部通过**（含新增的 C8 本机免 Key / C8b 远端仍拦下） |
| 静态分析（`-fanalyzer` + `-Werror`，从零编译） | **0 告警** |
| `tools/demo-ai-settings.ps1`（GUI 驱动 + 截图） | **16 通过 / 0 失败** |
| 发布包组装（只用 CI 产物，不编译） | 清单校验 8 项全 OK → `app\` + `package\` + install/uninstall + zip，exit=0 |

### 本轮改动（按主题）

> **注意哈希的坑**：本地历史里，上面这批改动先是被 `git reset` 回 `29a8c3a`、再压成了**一个提交 `82132aa`**
> （它的消息由 8 条标题串成、且**首行后缺空行**，所以 `git log --oneline` 会显示成很长的一行）。
> 早期那批哈希已**不可达**（`git reflog` 里还能找到，`git branch/tag --contains` 均为空）。
> 所以这里按**主题**列，不再逐一挂哈希 —— 需要追溯细节看这一节上面对应的条目与
> [docs/改进规划.md](docs/改进规划.md) 里的验证证据。

| 主题 | 内容 | 提交 |
| --- | --- | --- |
| `fix(build)` | 版本号收敛到 `identity.json` 唯一真相源 | `29a8c3a` |
| `refactor(core)` | 收敛 4 份重复的 `RunOut`，去掉死代码 `ShortHash` | `82132aa` |
| `test` | 无窗口核心单测 `GitRT.CoreTests`（CTest 用例 `core_selftest`） | `82132aa` |
| `chore` | `.gitattributes` / `.editorconfig`（行尾规则进仓库） | `82132aa` |
| `ci` | 复合动作 `setup-ucrt64` + 功能套件只跑一次 + package 复用产物 | `82132aa` |
| `feat(ai)` | 本机服务免 Key；明文 HTTP（非本机）给明确告警 | `82132aa` |
| `feat(ai)` | 可选用 Windows DPAPI 加密保存 Key | `82132aa` |
| `feat(ci)` | `g++ -fanalyzer` 静态分析门禁 | `82132aa` |
| `fix(gui)` | 修掉 25 处 `PostMessageW` 交接失败路径上的真实泄漏 | `82132aa` |
| `docs` | `SECURITY.md` / `CONTRIBUTING.md` + 研究文档归位 + 设计文档目录 | `82132aa` |
| `docs` | 修 2 条死链与 7 处表格渲染问题 + 设计文档"实现现状"注记 | `82132aa` |
| `feat(ci)` | 文档体检 `tools/check-docs.py`（后改为本机可选，见"变更"） | `82132aa` |
| `feat(gui)` | 「标签列表」「发布列表」点执行打开对应窗口 + 内部命令去向唯一映射 + 守卫断言 | `04954ed` |
