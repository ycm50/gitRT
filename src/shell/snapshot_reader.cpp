// 状态快照读取实现（seqlock 读者，见《技术实现设计》§4.7 / §5.2）
#include "snapshot_reader.h"

#include <cstring>

namespace grt::shell {
namespace {

constexpr uint32_t kMagic = 0x47525453;   // 'GRTS'
constexpr uint16_t kVersion = 1;
constexpr uint64_t kRetryIntervalMs = 2000;   // 段不可用时的重试节流
constexpr int      kProbeAttempts = 8;        // 线性探测上限

}  // namespace

std::wstring SnapshotSectionName() { return L"Local\\GitRT.Status.v1"; }

SnapshotReader& SnapshotReader::Instance() {
    static SnapshotReader inst;
    return inst;
}

void SnapshotReader::Close() {
    if (m_base) {
        ::UnmapViewOfFile(m_base);
        m_base = nullptr;
    }
    if (m_mapping) {
        ::CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
}

bool SnapshotReader::EnsureOpen() {
    if (m_base) return true;
    const uint64_t now = ::GetTickCount64();
    if (m_lastAttemptMs && now - m_lastAttemptMs < kRetryIntervalMs) return false;
    m_lastAttemptMs = now;

    HANDLE h = ::OpenFileMappingW(FILE_MAP_READ, FALSE, SnapshotSectionName().c_str());
    if (!h) {
        m_lastWin32Error = ::GetLastError();
        return false;
    }
    const void* base = ::MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(GrtSnapshotHeader));
    if (!base) {
        m_lastWin32Error = ::GetLastError();
        ::CloseHandle(h);
        return false;
    }
    const auto* hdr = static_cast<const GrtSnapshotHeader*>(base);
    m_lastVersion = hdr->version;
    if (hdr->magic != kMagic || hdr->version != kVersion || hdr->headerSize != sizeof(GrtSnapshotHeader)) {
        // 版本不匹配 → 视为"快照不可用"（§5.2），绝不复用旧布局
        GRT_LOGW("shell.snap", "快照头不兼容 magic=" << hdr->magic << " ver=" << hdr->version
                                                     << " size=" << hdr->headerSize);
        ::UnmapViewOfFile(base);
        ::CloseHandle(h);
        m_lastWin32Error = 0;
        return false;
    }
    m_slotCount = hdr->slotCount;
    m_slotMask = hdr->slotMask;
    ::UnmapViewOfFile(base);

    const size_t total = sizeof(GrtSnapshotHeader) + static_cast<size_t>(m_slotCount) * sizeof(SnapshotSlot);
    const void* full = ::MapViewOfFile(h, FILE_MAP_READ, 0, 0, total);
    if (!full) {
        m_lastWin32Error = ::GetLastError();
        ::CloseHandle(h);
        return false;
    }
    m_mapping = h;
    m_base = static_cast<const uint8_t*>(full);
    m_lastWin32Error = 0;
    GRT_LOGI("shell.snap", "快照段已映射 slots=" << m_slotCount);
    return true;
}

bool SnapshotReader::Available() { return EnsureOpen(); }

bool SnapshotReader::TryGet(const std::wstring& repoRoot, RepoSnapshot* out) noexcept {
    if (!out) return false;
    *out = RepoSnapshot{};
    if (repoRoot.empty() || !EnsureOpen()) return false;

    const uint64_t want = HashRoot(NormalizePath(repoRoot));
    const auto* hdr = reinterpret_cast<const GrtSnapshotHeader*>(m_base);
    const auto* slots = reinterpret_cast<const SnapshotSlot*>(
        m_base + sizeof(GrtSnapshotHeader));

    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t seq0 = hdr->seq;
        if (seq0 & 1u) continue;   // 写入中
        const uint32_t slotCount = hdr->slotCount;
        const uint32_t slotMask = hdr->slotMask;
        if (slotCount == 0 || slotCount > 65536) break;

        bool found = false;
        SnapshotSlot slot{};
        uint32_t idx = static_cast<uint32_t>(want) & slotMask;
        for (int probe = 0; probe < kProbeAttempts; ++probe) {
            const SnapshotSlot& s = slots[idx & slotMask];
            if (s.pathHash == 0) break;                 // 空槽 → 未命中
            if (s.pathHash == want) {
                slot = s;
                found = true;
                break;
            }
            idx = (idx + 1) & slotMask;
        }
        const uint32_t seq1 = hdr->seq;
        if (seq0 != seq1) continue;   // 期间有写入 → 重试（撕裂数据绝不采用）
        if (!found || !(slot.flags & SF_Known)) return false;
        out->known = true;
        out->computing = (slot.flags & SF_Computing) != 0;
        out->dirty = (slot.flags & SF_Dirty) != 0;
        out->staged = (slot.flags & SF_Staged) != 0;
        out->untracked = (slot.flags & SF_Untracked) != 0;
        out->conflicted = (slot.flags & SF_Conflicted) != 0;
        out->stash = (slot.flags & SF_StashPresent) != 0;
        out->ahead = slot.ahead;
        out->behind = slot.behind;
        out->probeFlags = slot.flags & 0x7FFFu;
        out->updatedMs = slot.updatedMs;
        return true;
    }
    return false;
}

std::wstring SnapshotReader::Diagnose() {
    std::wstring s;
    if (m_base) {
        s = L"已映射 " + SnapshotSectionName() + L"  slots=" + std::to_wstring(m_slotCount);
    } else {
        s = L"未映射 " + SnapshotSectionName() + L"  win32err=" + std::to_wstring(m_lastWin32Error) +
            L" (183=已存在, 2=不存在 → Broker 未运行属正常降级)";
        if (m_lastVersion) s += L"  lastVersion=" + std::to_wstring(m_lastVersion);
    }
    return s;
}

}  // namespace grt::shell
