# 安全策略（Security Policy）

GitRT 是一个**本地** Windows 工具：它把 Git 的高频操作放到资源管理器右键菜单里，
所有 git 命令都在**用户自己的权限**下、在用户的仓库目录里执行。
这份文档说明它处理敏感数据的方式、已知的信任边界，以及如何报告漏洞。

---

## 支持的版本

仓库目前**没有正式发版**：只有 `main` 分支与 CI 产出的调试用二进制/发布包。
安全修复直接进 `main`；请以 `main` 为基准。

---

## 报告漏洞

请**不要**开公开 issue。用 GitHub 的私密渠道：

> 仓库 → **Security** → **Report a vulnerability**（Security Advisories）
> <https://github.com/ycm50/gitRT/security/advisories/new>

请尽量给出：影响的版本/提交、复现步骤、影响面（能读到什么/能执行什么）、
以及你期望的修复方向。我们会先确认收到，再一起定披露时间。

---

## 凭据（AI API Key）怎么处理

这是本项目唯一需要长期保存的秘密。三种模式，**默认是明文**（产品决策：方便直接查看与修改）：

| 模式 | 落盘位置 | 说明 |
| --- | --- | --- |
| 明文 | `<exe 同目录>\GitRT.ai.json` 的 `apiKey` | 默认。任何能读该目录的进程都能读到 |
| 加密 | 同上，但值是 `dpapi:<base64>` | 首选项里勾「用 Windows 加密保存 Key（DPAPI）」；`CryptProtectData` 按**当前用户**加密，换用户/换机器解不开（只提示重填，**不会**清空文件） |
| 不落盘 | — | `apiKey` 留空，改用 `apiKeyEnv` 指定的环境变量（默认 `DEEPSEEK_API_KEY`） |

相关保证与已核实事实：

- **凭据永不写日志**：全仓库没有任何一处把 Key 或请求体写进日志（`Authorization` 头只在 HTTP 层拼装）。
- **界面上不回显 Key 本身**：AI 窗口只显示"已设置 ✓ / 未设置"，设置窗口按产品决策明文显示在输入框里（用户自己要看）。
- **Git 远端地址里的凭据会脱敏显示**：`https://user:token@host/...` 在界面与日志里显示为 `https://***@host/...`。
- **`.gitignore` 已覆盖**：`GitRT.ai.json`、`*.ai.json`、`*.pem`、`*.key`、`.env`、`*.pfx`。
- **卸载不丢**：`uninstall.ps1` 先把 `GitRT.ai.json` 备份到 `%APPDATA%\GitRT\uninstall-backup-<时间>\`，重装时自动恢复。
- **加密失败时 fail-closed**：勾了加密但 `CryptProtectData` 失败 → 不写 Key 并报"保存失败"，**不会**悄悄回退成明文。

### 提示词里会发出什么

发给 AI 服务的只有：**仓库路径、当前分支名、最多 20 个已选文件的路径**、以及你输入的自然语言。
**不发送文件内容、不发送 diff、不发送提交信息正文**（查看类 git 命令的输出只显示在你本地界面上，不会回传模型）。

### 明文传输会被提醒

如果接口地址是 `http://` 且**不是本机**（`localhost` / `127.0.0.0/8` / `[::1]` 之外），
那么 Key 与提示词都会明文过网：设置窗口在保存/测试时会弹一次确认框（默认按钮是"取消"），
AI 窗口的配置行也会常驻提醒。程序**不禁止**这种配置（内网代理是真实用法），但不会让你在不知情的情况下用它。

---

## 信任边界（明确写出来，避免误解）

1. **git 永远不经过 shell**：所有 git 调用都是 `CreateProcessW` + argv 数组，参数经白名单校验
   （分支名/远端名/修订表达式/URL），因此没有命令注入面。子进程用 Job Object 包住，
   取消/超时会连同整棵进程树一起结束（不留孤儿 `ssh` / credential helper）。
2. **AI 不能执行任意命令**：模型只能从**命令表**里选一条（未知 key 直接拒绝、未知选项忽略并提示），
   或者给一条**只读** git 命令行（`status/log/show/diff/branch/remote/tag -l/…` 白名单，写操作一律拒绝）。
   计划必须走与菜单、参数面板**完全相同**的校验与 argv 构造路径。
3. **AI 服务端本身在信任边界之外**：它能看到你发出的提示词；模型回复被视为**不可信输入**（只解析、不执行）。
4. **不是沙箱**：程序以你的用户身份运行 git，因此仓库里的 git 钩子、`core.pager` 之类配置
   仍可能在你执行命令时做事情 —— 这与直接在终端跑 git 等价。
5. **本机即信任**：能读写 exe 所在目录的进程可以读到明文 Key（除非启用 DPAPI 加密）。

### 不在范围内

- 需要"攻击者已经能读写你的用户目录/以你的身份运行代码"的场景（那时 Key 与仓库本来就都在它手里）。
- 物理接触、屏幕截图、键盘记录。
- 第三方 AI 服务自身的漏洞（请报告给对应服务商）。
- 你自己配置的 git 钩子/别名/凭据助手的行为。

---

## 安全相关的实现位置（方便审计）

| 关注点 | 代码位置 |
| --- | --- |
| Key 的读写、DPAPI、Base64 | [`src/core/ai_client.cpp`](src/core/ai_client.cpp) |
| 本机/明文端点判定（免 Key 与告警的依据） | `IsLoopbackEndpoint` / `IsInsecureRemoteEndpoint`（同上） |
| 参数白名单与 argv 构造 | [`src/core/command_builder.cpp`](src/core/command_builder.cpp) |
| 进程启动、句柄白名单、Job Object、超时/取消 | [`src/core/git_runner.cpp`](src/core/git_runner.cpp) |
| AI 计划 → 命令的安全闸门、只读命令白名单 | `PlanToCommand` / `ParseReadOnlyGitCommand`（[`src/core/ai_client.cpp`](src/core/ai_client.cpp)） |
| AI 提示词里究竟放了什么 | `BuildPlannerUserPrompt`（同上） |
| 注册表只写 `HKCU`、安装/卸载 | [`packaging/scripts/install.ps1`](packaging/scripts/install.ps1)、`uninstall.ps1` |

> 本项目的安全边界也有对应的**自动化断言**：`ctest` 里的 `core_selftest`（参数校验、AI 闸门、
> 只读白名单、端点判定、Key 加密不落明文）与 `tools/test-remote.ps1` 的拒绝条件用例（选项注入、
> 非法分支名、脏工作区强重置等）。改动这些路径时请一并更新它们。
