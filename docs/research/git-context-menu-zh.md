# Windows 右键菜单 Git 管理器 · 技术调研笔记

**版本坐标**：Git 最新稳定版 2.55.0（2.56.0-rc2 在途）；libgit2 最新 v1.9.7（2026-08-13，安全发布）。⚠️ 表示未找到一手依据、需自行验证的不确定项。

## 1. `git status` 性能：git CLI vs libgit2

- **libgit2 慢 2.5–6 倍，且至今未修**：2017 年在 gentoo 仓库（99,540 文件 / 27,306 目录）实测 `git status --short` 0.257s、开 `core.untrackedCache` 后 0.097s，而 git2go 的 `git_status_list_new` 要 0.626s；issue #4230 至今仍是 open，2025-05 仍有用户追问进展。
- **瓶颈是缓存缺失，不是语言**：libgit2 不实现 `core.untrackedCache` / `core.fsmonitor`，用 `lstat()` 而非 `fstatat()`，对目录做多余 stat，`.gitignore` 匹配也不缓存。
- **两个下游项目已改用 CLI**：starship issue #1446 原文称「我们最大的性能瓶颈一直是 git2，比直接调用 `git` 差得多」，PR #2465 因此改为 `git status -b --porcelain=2 -uall` + `git stash list`。
- **基准（chromium，i9-7900X / ext4 SSD）**：gitstatus 291ms 冷 / 30.9ms 热；`git`（开 untrackedCache、关 fsmonitor）876ms / 295ms；libgit2 示例程序 lg2 1730ms / 1310ms。Git 侧真正的加速开关是 `core.untrackedCache=true` + `core.fsmonitor=true`（内置 daemon 仅 Windows / macOS），官方文档并提醒需跑数轮 `git status` 预热才见效。
- ⚠️ **文档纠偏**：fsmonitor / untracked cache / index v4 **都不是 Git 2.53 的新特性**（index v4 自 Git 1.8.0 / 2012 年，v4 可把索引缩小 30–50%）。我通读了 2.53.0 的 Release Notes，其中没有 fsmonitor / untracked cache 的性能条目 —— 请勿在文档里写成 2.53 特性。

**数据/依据**
- libgit2 issue #4230「git_status_list is slower than `git status`」（2017-05 开，含原始数字；2025-05 仍有人追问）— https://github.com/libgit2/libgit2/issues/4230
- starship issue #1446（动机原文）+ PR #2465（改调 CLI 的实现说明）— https://github.com/starship/starship/issues/1446 ，https://github.com/starship/starship/pull/2465
- gitstatus README「Benchmarks」（三方基准表）+ gitstatusd 作者对 libgit2 的 CPU profile 分析 — https://github.com/romkatv/gitstatus#benchmarks
- `git-status(1)`「UNTRACKED FILES AND PERFORMANCE」段（untracked cache / fsmonitor 效果与预热说明）— https://git-scm.com/docs/git-status
- `core.fsmonitor`「仅 Windows 与 macOS 提供内置 daemon」— https://git-scm.com/docs/git-config
- index v4 的版本沿革（Git 1.8.0 / libgit2 2016 / JGit 2020）— https://git-scm.com/docs/git-update-index
- 2.53.0 Release Notes 全文（无 fsmonitor 条目）— https://raw.githubusercontent.com/git/git/v2.53.0/Documentation/RelNotes/2.53.0.adoc

## 2. gitstatusd（romkatv/gitstatus）为什么快

- **「10x」是作者自称，且不是靠 daemon 缓存命中**：基准在干净仓库上跑，提前终止扫描、优先复检上次脏文件等短路都未触发，各工具做的是同一份工作。
- **加速来自三处**：用户态算法与数据结构优化（比 libgit2 少 32x 用户态 CPU）、更省的系统调用（1.9x，`fstatat` 代替 `lstat`、用 `openat` 从父目录打开目录、直调 `getdents64` 快 23%）、多线程近线性扩展（12.4x）。等价于 libgit2 `git_diff_index_to_workdir` 的那部分**快 46.3 倍**。
- **它缓存什么**：自研「untracked cache 等价物」—— 记住每个目录的 mtime 与该目录下的未跟踪文件列表，mtime 未变就跳过 `readdir`。**不依赖 Git 的 fsmonitor 协议**。
- **shell 怎么调用它**：gitstatusd 从 stdin 读请求（请求 ID + 目录），向 stdout 写回响应（同一 ID + 机器可读状态）；shell binding 用管道同步或异步调用，进程内存中保留已见目录的状态。
- **可借鉴**：① 常驻进程 + 自有目录级缓存；② 只需布尔「脏/不脏」而非列全部文件，可提前终止；③ 请求 ID / 响应协议天然适配 shell 扩展的异步查询。
- ⚠️ **不可直接复用**：GPL-3.0 许可；**无原生 Win32 构建**（运行仅支持 Linux/macOS/FreeBSD/Android/WSL/Cygwin/MSYS2）；作者声明项目「仅极有限支持、无新功能、多数 bug 不修」；最新 release 为 v1.5.5（2024-03-30）；且它不是 `git status` 的 drop-in 替代。

**数据/依据**
- gitstatus README（How it works / Benchmarks / Why fast / Requirements / License）— https://github.com/romkatv/gitstatus#how-it-works
- 项目健康度与最新版本（v1.5.5，2024-03-30；GPL-3.0）— https://api.github.com/repos/romkatv/gitstatus ，https://api.github.com/repos/romkatv/gitstatus/releases/latest
- HN 讨论（chromium 下 25,000 个目录的实测语境）— https://news.ycombinator.com/item?id=19598404

## 3. 调用 git CLI 的坑

- **凭据链路与挂死风险**：`GIT_ASKPASS` → `core.askPass` → `SSH_ASKPASS` → 终端提示 → credential helper；askpass 程序**以提示语为命令行参数、从 stdout 返回密码**。必须同时设 `GIT_TERMINAL_PROMPT=false`，否则无终端时子进程可能永久挂住。
- **现成范例**：TortoiseGit 专门写了 `SshAskPass.exe` 当 askpass 程序 —— 其架构文档原话是「从 TortoiseGitProc.exe 启动时没有 terminal attached，因此 git.exe 与 ssh.exe 无法询问凭据」。⚠️ `SSH_ASKPASS_REQUIRE=force`（OpenSSH 8.4+）可强制 ssh 走 askpass，但 Git for Windows 是否默认设置我未验证。
- **索引锁**：`git status` 默认会刷新并**写回** index；常驻/后台进程应加 `--no-optional-locks`（等价 `GIT_OPTIONAL_LOCKS=0`）。官方文档明确建议后台脚本用 `git --no-optional-locks status`。代价是放弃写回后，后续 status 无法复用 stat 缓存 —— 需权衡。
- **稳定机器可读输出**：`--porcelain=v2 -z`（NUL 分隔、不转义、不做 C 引号）；不加 `-z` 就受 `core.quotePath` 影响，需 `-c core.quotepath=false`。porcelain v1/v2 承诺跨版本稳定，而 long format 文档明说「内容与格式随时可能变」。另需显式关掉 pager 与 advice（`GIT_PAGER=cat`、`GIT_ADVICE=0`）。
- **不弹黑框**：`CreateProcess` 传 `CREATE_NO_WINDOW`（0x08000000，「以控制台应用运行但不创建控制台窗口」）；它与 `CREATE_NEW_CONSOLE` / `DETACHED_PROCESS` 互斥，对非控制台程序无效。
- **Windows 专属红利**：`GIT_REDIRECT_STDIN/STDOUT/STDERR` 可把子进程 stdio 重定向到路径，**官方推荐的用法是命名管道**（如 `\\.\pipe\my-git-stdin-123`）—— 该特性的设计动机正是多线程 GUI：避免要求句柄可继承而导致「每个子进程都继承，可能阻塞常规 Git 操作」。
- **发现性能与安全前提**：`GIT_CEILING_DIRECTORIES` 阻止向上 chdir 到慢的网络目录；`GIT_DISCOVERY_ACROSS_FILESYSTEM` 控制跨卷发现。安全上 git(1) SECURITY 明确写着：在来自不受信任来源的 `.git` 目录（或围绕它的工作树）里执行 Git 命令**不安全**，其 config 与 hooks 会被照常执行；`safe.directory` 默认拒绝操作非本人拥有的仓库 —— 右键菜单面向任意目录，必须正视这一点。hook 与 LFS 因此会真实影响 GUI 客户端的可预测性与耗时。⚠️ LFS 的影响我未找到权威量化数据。

**数据/依据**
- `git(1)` ENVIRONMENT VARIABLES（GIT_ASKPASS / GIT_TERMINAL_PROMPT / GIT_OPTIONAL_LOCKS / GIT_CEILING_DIRECTORIES / GIT_REDIRECT_STD*) 与 SECURITY 段 — https://man7.org/linux/man-pages/man1/git.1.html ，https://git-scm.com/docs/git
- `git-status(1)` OPTIONS（`--porcelain`、`-z`、`-u`）与 BACKGROUND REFRESH 段 — https://git-scm.com/docs/git-status
- `gitcredentials(7)`（helper 协议、askpass 顺序、内置 helper 清单与 store 的明文风险）— https://git-scm.com/docs/gitcredentials
- TortoiseGit `architecture.txt`（TGitCache.exe、SshAskPass.exe、Stub/主 DLL/Proc.exe 分工；「多数操作直接调用 git.exe」）— https://github.com/TortoiseGit/TortoiseGit/blob/master/architecture.txt
- `CREATE_NO_WINDOW` 定义 — https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags
- `SSH_ASKPASS_REQUIRE` 引入于 OpenSSH 8.4 — https://www.openssh.org/txt/release-8.4

## 4. libgit2 现状（2026）

- **官方定位就是「不替代 git」**：README 明确 libgit2 不打算替代 git 工具或其面向用户的命令，许多用户会敲的命令「超出本库直接实现的范围」，并以 `git change` label 追踪与 git 的落后/不兼容 —— 这是「不能只用 libgit2」的最强论据。许可证为 GPLv2 + Linking Exception，可安全链接进闭源产品。
- **版本**：最新 v1.9.7（2026-08-13），属安全发布（libssh2 路径转义，CVE-2026-5917）；v1.8 / v1.9 维护线并行。
- **支持**：index v4（2016 年起）、worktree API（v1.9.7 头文件含 `git_worktree_list/lookup/add/lock/unlock/validate/prune`）、以及基于 `GIT_INDEX_ENTRY_SKIP_WORKTREE` 的基础 sparse-checkout 处理。
- **不支持 / 仅实验**：
  - **interactive rebase**：issue #6332（open，22 个 👍），`rebase.c` 判定 interactive state 不支持；#3795 请求「自定义 rebase operation 列表」亦未实现。
  - **hooks**：完全不执行任何 git hook。issue #964（pre-commit / post-commit / post-merge）2015 年关闭未实现；#3343 指出 `git init` 会重填 hooks 而 libgit2 不会；hook 支持的设计讨论 #3004 已关闭未落地。
  - **sparse-checkout / partial clone（promisor）**：均未支持。issue #2263（2014 起 open）、#5564（partial clone）、#6880（**无法打开**带 `extensions.partialclone` 的仓库）；PR #7364（2026-09-04）才提议加 filtered fetch / promisor hydration / cone-mode sparse checkout，仍是 open 未合并。
  - **SHA-256**：⚠️ 仍属实验。main 分支 `oid.h` 原文「SHA1 is currently the only supported object ID type」，并称 SHA-256 是破坏性 API 变更、「仅供应用兼容性测试」。
  - **untracked cache / fsmonitor**：无实现。全仓库检索 `fsmonitor` 只命中一个安全类 issue（#6400），没有 untracked cache 相关实现或 API。
  - **外部 merge driver / gitattributes 合并属性**：`.gitattributes` 里的 `-merge` 不生效（issue #5328，2019 起 open）。⚠️ 外部 merge driver、textconv 的逐项缺失清单我未找到官方汇总。

**数据/依据**
- libgit2 README（定位声明、SHA256 experimental、GPLv2 + Linking Exception、`git change` label）— https://github.com/libgit2/libgit2#readme
- 最新版本与安全发布内容（v1.9.7 / v1.8.7，2026-08-13）— https://api.github.com/repos/libgit2/libgit2/releases
- issue #4230（状态性能）、#6332（interactive rebase）、#3795、#964 / #3343 / #3004（hooks）、#2263 / #5564 / #6880 / PR #7364（sparse-checkout + partial clone）、#5328（merge 属性）— https://github.com/libgit2/libgit2/issues/6332 （其余同仓库路径）
- `oid.h`「SHA1 is currently the only supported object ID type」— https://raw.githubusercontent.com/libgit2/libgit2/main/include/git2/oid.h
- `worktree.h`（v1.9.7 API 全集）— https://raw.githubusercontent.com/libgit2/libgit2/v1.9.7/include/git2/worktree.h
- index v4 支持声明 — https://git-scm.com/docs/git-update-index

## 5. 凭据安全（Windows）

- **首选：不自己存密码，交给 Git credential helper / GCM**。Git Credential Manager（git-ecosystem/git-credential-manager，.NET，MIT）是跨平台官方 helper，**Windows 上默认 store 就是 Windows Credential Manager**（走 `wincred.h` API，即 `wincredman`），另可选 DPAPI 加密文件（默认落在 `%USERPROFILE%\.gcm\dpapi_store`）。
- **GCM 存的是凭据 / OAuth 刷新令牌，而非用户密码**，并负责 MFA、Entra broker；覆盖 Azure DevOps / GitHub / GitLab / Bitbucket，支持 Windows / macOS / Linux，且只用于 HTTP(S) 远端（SSH 不走 GCM）。
- **helper 协议**：`git credential fill` / `approve` / `reject`，经 stdin/stdout 传 key=value，`credential.helper` 可叠加多个。内置 helper 中 `cache` 只在内存且有超时；`store` 是**磁盘明文**，产品中不应使用。
- **必须自己落盘时只用 OS 密钥库**：`CredWriteW` / `CredReadW` / `CredDeleteW`（wincred.h，advapi32）把凭据写入**当前登录会话**的 credential set；若加密自己的文件则用 DPAPI `CryptProtectData`（默认绑定用户 + 机器，`CRYPTPROTECT_LOCAL_MACHINE` 可改为机器级，`pOptionalEntropy` 加盐）。⚠️ DPAPI 基于提示（PromptStruct）的流程已弃用、将于 **2027-02** 移除，新代码不应使用。
- **交互归属必须显式决定**：`GCM_INTERACTIVE=false` 禁止 GCM 一切交互（需要交互即报错）；`GCM_GUI_PROMPT=0` 让 GCM 改用终端文本提示（无终端时可能失败）。GUI 客户端若想自己拥有凭据 UI，需实现 `GIT_ASKPASS` 小程序并妥善设置上述变量 —— 这也是 TortoiseGit 用户长期抱怨 GCM 弹窗的根源。⚠️ Git for Windows 安装器当前是否默认启用 GCM，我未拿到可引用的一手页面。

**数据/依据**
- GCM README（.NET、MIT、平台与远端范围、替代两代旧 GCM）— https://github.com/git-ecosystem/git-credential-manager#readme
- GCM `docs/credstores.md`（Windows 默认 wincredman + wincred.h；DPAPI store 路径；明文 store 的存在与限制；网络会话限制）— https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/credstores.md
- GCM `docs/environment.md`（`GCM_INTERACTIVE`、`GCM_GUI_PROMPT` 语义）— https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/environment.md
- `gitcredentials(7)`（helper 协议与 `store` 的明文注意事项）— https://git-scm.com/docs/gitcredentials
- `CredWriteW`（credential set 与登录会话绑定、返回值）— https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credwritew
- `CryptProtectData`（用户 + 机器绑定、LOCAL_MACHINE、entropy、PromptStruct 2027-02 移除）— https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata

## 对产品设计的启示

1. **默认后端用 git CLI，不自研 git 协议与对象库。** libgit2 自称不替代 git，且在 hooks / interactive rebase / sparse-checkout / partial clone / fsmonitor 上缺失，其行为差异会直接变成 bug 工单。
2. **做常驻「状态观察者」进程，缓存 仓库→状态 映射**，仿 TortoiseGit 的 `TGitCache.exe`（后台算状态、供 shell 扩展查询、并监视 Explorer 打开过的目录以即时刷新）与 gitstatusd。右键点击时只做 IPC 查询，避免在 Explorer 线程里跑 git。
3. **shell 扩展与业务进程解耦**（照抄 TortoiseGit 的 Stub DLL + 主 DLL + Proc.exe 三层）：32/64 位宿主兼容、崩溃隔离、不拖慢 Explorer。
4. **状态查询固定成一条命令**：`git --no-optional-locks -c core.quotepath=false status --porcelain=v2 -z --branch --show-stash`，并注入 `GIT_TERMINAL_PROMPT=0`、`GIT_PAGER=cat`、`GIT_ADVICE=0`；把「启用 `core.untrackedCache` + `core.fsmonitor`」做成可一键开启的推荐项，并容忍首次预热变慢。
5. **子进程一律 `CREATE_NO_WINDOW` + 显式管道（优先命名管道）**，不让 git 继承可继承句柄、不碰终端；环境变量白名单化，避免把宿主的 `GIT_DIR`、代理等泄漏给子进程。
6. **凭据只交给 GCM / credential helper**，自己最多实现一个 `GIT_ASKPASS` / `SSH_ASKPASS` 小程序（SshAskPass.exe 的思路）；必须自存时只用 `CredWriteW` 或 `CryptProtectData`，绝不写明文、绝不自研加密。
7. **libgit2 只做只读快路径**（diff 预览、blame、log 渲染、离线检索），凡 interactive rebase、hooks、sparse-checkout / partial clone、外部 merge driver、LFS 一律回落 git CLI；并在诊断信息里暴露「当前用的是哪个后端」。
8. **安全与体验边界**：在不受信任的 `.git` 上执行 git 有 RCE 与性能双重风险，建议把「只读状态查询」与「写操作」分级，写操作前做 `safe.directory` 检查与提示；图标覆盖槽位极少（TortoiseGit 自述要当「好公民」并主动限制占用），Windows 11 现代菜单应以命令项为主、覆盖图标作为可选增强。
