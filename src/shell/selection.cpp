// 选择上下文解析实现（《技术实现设计》§4.4）
#include "selection.h"

#include "probe_log.h"

#include <shlobj.h>

#include <deque>
#include <mutex>
#include <unordered_map>

namespace grt::shell {
namespace {

// --------------------------------------------------------------- 探测结果缓存
// LRU(64) + 3 秒新鲜度窗口。
// 为什么带窗口（与 §4.6 的"仅 LRU"略有偏差）：`git init` 之后立刻右键，
// 若缓存了"非仓库"就会给出错误菜单；而一次 ProbeRepo 在 NTFS 上 < 0.5 ms，
// 新鲜度带来的正确性远比这点耗时重要。窗口内的重复右键仍然是零成本。
constexpr size_t   kProbeCacheMax = 64;
constexpr uint64_t kProbeFreshMs = 3000;

struct ProbeEntry {
    RepoProbeResult result;
    uint64_t        at = 0;
};

std::mutex                                    g_probeMutex;
std::deque<std::wstring>                      g_probeOrder;
std::unordered_map<std::wstring, ProbeEntry>  g_probeCache;

std::wstring CacheKey(const std::wstring& path) {
    // 大小写不敏感（NTFS）+ 去尾反斜杠：用 小写化 的规范化路径作键
    return ToLowerAscii(NormalizePath(path));
}

// ---------------------------------------------------------------- 粘性选区
std::mutex                              g_lastSelMutex;
std::shared_ptr<const SelectionContext> g_lastSel;
uint64_t                                g_lastSelAt = 0;

}  // namespace

void RememberSelection(const SelectionContext& ctx) {
    auto copy = std::make_shared<SelectionContext>(ctx);
    std::lock_guard lock(g_lastSelMutex);
    g_lastSel = std::move(copy);
    g_lastSelAt = ::GetTickCount64();
    GRT_LOGT("shell.sel", "记住选区 selMask=" << ctx.selMask << " paths=" << ctx.paths.size()
                                             << " repo=" << (ctx.IsRepo() ? 1 : 0));
}

std::shared_ptr<const SelectionContext> LastKnownSelection(uint64_t ttlMs) {
    std::lock_guard lock(g_lastSelMutex);
    if (!g_lastSel) return nullptr;
    if (::GetTickCount64() - g_lastSelAt > ttlMs) return nullptr;   // 过期 → 宁可"未知"也不给错
    return g_lastSel;
}

void ClearLastSelection() {
    std::lock_guard lock(g_lastSelMutex);
    g_lastSel.reset();
    g_lastSelAt = 0;
}

RepoProbeResult ProbeRepoCached(const std::wstring& anyPath) {
    const std::wstring key = CacheKey(anyPath);
    const uint64_t now = ::GetTickCount64();
    {
        std::lock_guard lock(g_probeMutex);
        const auto it = g_probeCache.find(key);
        if (it != g_probeCache.end() && now - it->second.at <= kProbeFreshMs) return it->second.result;
    }
    RepoProbeResult result = ProbeRepo(anyPath);
    {
        std::lock_guard lock(g_probeMutex);
        if (!g_probeCache.count(key)) g_probeOrder.push_back(key);
        g_probeCache[key] = ProbeEntry{result, now};
        while (g_probeOrder.size() > kProbeCacheMax) {
            g_probeCache.erase(g_probeOrder.front());
            g_probeOrder.pop_front();
        }
    }
    return result;
}

void ClearProbeCache() {
    std::lock_guard lock(g_probeMutex);
    g_probeCache.clear();
    g_probeOrder.clear();
}

// ------------------------------------------------------------------ 解析
namespace {
HRESULT ParseSelectionImpl(IShellItemArray* items, SelectionContext* out) noexcept {
    if (!out) return E_POINTER;
    GRT_PROBE_TIMER("ParseSelection");

    *out = SelectionContext{};
    out->attempted = true;
    if (!items) {
        // 文件夹空白处（若 Shell 传 nullptr；多数情况下仍会传该目录的 item）
        out->selMask = kSelBg;
        out->commonRoot = NormalizePath(GetModuleDir());   // 仅供参考，不用于 git
        out->probe = ProbeRepoCached(out->commonRoot);
        return S_OK;
    }

    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) return E_FAIL;
    if (count > kMaxSelection) {
        out->truncated = true;
        count = kMaxSelection;
    }

    std::wstring firstDir;
    std::wstring firstFileParent;
    bool anyPath = false;
    for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
        LPWSTR name = nullptr;
        const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
        if (FAILED(hr) || !name) {
            // 虚拟项（库 / 网络根 / 控制面板）→ 整个选区不可操作
            out->nonFilesystem = true;
            item->Release();
            continue;
        }
        const std::wstring path = NormalizePath(name);
        ::CoTaskMemFree(name);
        item->Release();
        if (path.empty()) continue;

        const DWORD attr = ::GetFileAttributesW(path.c_str());
        const bool isDir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            out->hasReparsePoint = true;
        if (IsNetworkPath(path)) out->isNetwork = true;

        out->paths.push_back(path);
        out->isDir.push_back(isDir ? 1 : 0);
        if (isDir) {
            ++out->dirCount;
            if (firstDir.empty()) firstDir = path;
        } else {
            ++out->fileCount;
            if (firstFileParent.empty()) firstFileParent = ParentOf(path);
        }
        anyPath = true;
    }

    if (out->nonFilesystem) {
        // 只要有一个虚拟项就不给菜单（§4.4 规则表第一行）
        out->selMask = 0;
        return S_OK;
    }
    if (!anyPath) {
        out->selMask = kSelBg;
        return S_OK;
    }

    if (out->fileCount) out->selMask |= kSelFile;
    if (out->dirCount) out->selMask |= kSelDir;
    if (out->paths.size() > 1) out->selMask |= kSelMulti;
    // ⚠️M0 遗留问题：Shell 在「文件夹上」与「文件夹空白处」传进来的 item 都是该目录，
    // 两者无法可靠区分（§4.4 的待实测项）。按 §4.4 的可见性矩阵，这两种场景要求
    // 完全相同的菜单（"全量"），因此这里把"单选一个目录"同时标记为 Dir|Bg，
    // 使两种场景得到一致结果；多选时不置 Bg（语义明确）。
    if (out->dirCount == 1 && out->fileCount == 0 && out->paths.size() == 1) out->selMask |= kSelBg;

    // 公共父目录：单目录 = 其自身；否则 = 第一个文件/目录的父级
    if (out->paths.size() == 1 && out->dirCount == 1) {
        out->commonRoot = firstDir;
    } else if (!firstDir.empty()) {
        out->commonRoot = firstDir;
    } else {
        out->commonRoot = firstFileParent;
    }

    if (out->isNetwork) {
        // UNC 默认不做任何状态查询（§4.4）：只标记，不触碰网络
        out->probe.flags = RF_Network;
        return S_OK;
    }
    out->probe = ProbeRepoCached(out->commonRoot.empty()
                                     ? (out->paths.empty() ? std::wstring() : out->paths.front())
                                     : out->commonRoot);
    return S_OK;
}
}  // namespace

HRESULT ParseSelection(IShellItemArray* items, SelectionContext* out) noexcept {
    const HRESULT hr = ParseSelectionImpl(items, out);
    // 只有"外壳真的给了数组"的解析结果才值得记住：items == nullptr 是**子项**调用
    // （外壳没给选区），记住它等于把"空选区"当成"用户什么都没选"，会让后续子项全灭。
    if (out && items != nullptr) RememberSelection(*out);
    return hr;
}

}  // namespace grt::shell
