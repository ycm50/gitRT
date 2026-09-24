#pragma once
// ---------------------------------------------------------------------------
// 选择上下文捕获（《技术实现设计》§4.4）
//
// 关键约束：EnumSubCommands 不带 IShellItemArray 参数，而子命令的
// GetState/Invoke 都需要知道用户选了什么。因此：
//   ① 任何带 array 的方法（GetTitle/GetIcon/GetState/Invoke）都会解析并缓存；
//   ② EnumSubCommands 用父对象的缓存构造子对象（共享同一个 shared_ptr）；
//   ③ 子对象若先收到 array，则再解析一次（幂等）；
//   ④ ★ M0-B 实测补充：外壳对**子菜单项**常传 nullptr，于是还需要"进程内粘性选区"
//      （见文件末尾 RememberSelection / LastKnownSelection）。
// ---------------------------------------------------------------------------

#include "core.h"
#include "command_spec.h"

#include <shobjidl.h>

#include <memory>

namespace grt::shell {

// 选区上限（超过只保留前 N 项用于显示，§4.4）
constexpr uint32_t kMaxSelection = 200;

struct SelectionContext {
    std::vector<std::wstring> paths;          // 已规范化（去 \\?\、绝对路径）
    std::vector<uint8_t>      isDir;          // 与 paths 平行
    uint32_t                  selMask = 0;    // kSel* 位掩码
    uint32_t                  fileCount = 0;
    uint32_t                  dirCount = 0;
    bool                      truncated = false;
    bool                      isNetwork = false;
    bool                      hasReparsePoint = false;
    bool                      nonFilesystem = false;   // 虚拟项/库/网络根 → 菜单隐藏
    bool                      attempted = false;       // 是否已尝试过解析（幂等依据）
    std::wstring              commonRoot;              // 选区公共父目录
    RepoProbeResult           probe;                   // 廉价探测结果（零 git 调用）

    bool IsRepo() const { return probe.IsRepo(); }
    bool Empty() const { return paths.empty(); }
    uint32_t Total() const { return fileCount + dirCount; }
};

// 解析 IShellItemArray → SelectionContext。items 为 nullptr 表示"文件夹空白处"。
// 约定：永不抛异常；失败返回错误码，但 out 保持"安全"状态（selMask=0 → 菜单隐藏）。
HRESULT ParseSelection(IShellItemArray* items, SelectionContext* out) noexcept;

// 探测缓存（LRU 64 + 3 s 新鲜度窗口；见实现里的"为什么有窗口"注释）
RepoProbeResult ProbeRepoCached(const std::wstring& anyPath);
void            ClearProbeCache();

// ---------------------------------------------------------------- 粘性选区
// ★ M0-B 实测：外壳对**子菜单项**调用 GetState/GetTitle 时常传 `psiItemArray == nullptr`
//   （顺序是 EnumSubCommands 建树 → 逐项 GetState(child, nullptr)），子项因此拿不到选区。
//   但同一次菜单调用里，外壳**通常至少给根项传过一次真实数组** → 把最近一次解析成功的
//   选区在进程内记住（TTL 5 s）供子项复用。没有它就会出现"子菜单空白"（全部被判 HIDDEN）。
void RememberSelection(const SelectionContext& ctx);
std::shared_ptr<const SelectionContext> LastKnownSelection(uint64_t ttlMs = 5000);
void ClearLastSelection();

}  // namespace grt::shell
