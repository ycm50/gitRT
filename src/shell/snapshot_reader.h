#pragma once
// ---------------------------------------------------------------------------
// 状态快照读取（《技术实现设计》§4.7 / §5.2）
//   · 只读、无锁（seqlock）：读者永不阻塞写者，也不阻塞 Explorer UI 线程
//   · Broker 未运行时（今天就是这样）TryGet 返回 false → 菜单乐观显示
//   · 段不存在/版本不符时**绝不**猜测状态，只降级为"状态未知"
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

#include "core.h"

namespace grt::shell {

// 状态位（§4.7）
enum StatusFlag : uint32_t {
    SF_Known        = 1u << 0,
    SF_Dirty        = 1u << 1,
    SF_Staged       = 1u << 2,
    SF_Untracked    = 1u << 3,
    SF_Conflicted   = 1u << 4,
    SF_Ahead        = 1u << 5,
    SF_Behind       = 1u << 6,
    SF_StashPresent = 1u << 7,
    SF_Computing    = 1u << 8,
};

#pragma pack(push, 8)
// 槽位布局：以 §4.7 的**字段表**为准（8+4+4+4+4+8+4+4 = 40 字节）。
// 注：§5.2 写"256 × SnapshotSlot = 8KB"（= 32 字节/槽），与字段表不一致；
// 实现按字段表，段内槽位表大小 = 256 × 40 = 10240 字节。已在文档偏差表登记。
struct SnapshotSlot {
    uint64_t pathHash;     // FNV-1a 64（小写化、无尾反斜杠的仓库根）；0 = 空槽
    uint32_t rootLen;
    uint32_t flags;        // RepoFlag | StatusFlag
    int32_t  ahead;
    int32_t  behind;
    uint64_t updatedMs;    // GetTickCount64 基准
    uint32_t generation;
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(SnapshotSlot) == 40, "SnapshotSlot 必须是 40 字节（§4.7）");

// 段头（§5.2，精确偏移）
#pragma pack(push, 8)
struct GrtSnapshotHeader {
    uint32_t magic;          // 0 = 0x47525453 ('GRTS')
    uint16_t version;        // 1
    uint16_t headerSize;     // 64
    volatile uint32_t seq;   // 奇数 = 写入中
    uint32_t slotCount;      // 256
    uint32_t slotMask;       // 255
    uint32_t flags;          // 1 = Broker 正在全量校准
    uint64_t publishedMs;
    uint32_t pathPoolOffset;
    uint32_t pathPoolSize;
    uint32_t repoListOffset;
    uint32_t repoListCount;
    uint8_t  reserved[16];
};
#pragma pack(pop)
static_assert(sizeof(GrtSnapshotHeader) == 64, "GrtSnapshotHeader 必须是 64 字节（§5.2）");

// 供菜单使用的解码结果（只包含菜单需要的位）
struct RepoSnapshot {
    bool     known = false;
    bool     computing = false;
    bool     dirty = false;
    bool     staged = false;
    bool     untracked = false;
    bool     conflicted = false;
    bool     stash = false;
    int32_t  ahead = 0;
    int32_t  behind = 0;
    uint32_t probeFlags = 0;   // RepoFlag（来自快照，可与本地探测互补）
    uint64_t updatedMs = 0;
};

// 段名：Local\GitRT.Status.v1（提权实例用 .elev 后缀，§6.5）
std::wstring SnapshotSectionName();

class SnapshotReader {
public:
    static SnapshotReader& Instance();

    // 段是否可读且版本匹配（打开失败会节流重试，避免每次菜单都尝试）
    bool Available();

    // 按仓库根取状态；false = 状态未知（调用方必须走"乐观显示"路径）
    bool TryGet(const std::wstring& repoRoot, RepoSnapshot* out) noexcept;

    // 段/版本诊断信息（doctor 与探针日志）
    std::wstring Diagnose();

private:
    SnapshotReader() = default;
    bool EnsureOpen();
    void Close();

    HANDLE          m_mapping = nullptr;
    const uint8_t*  m_base = nullptr;
    uint32_t        m_slotMask = 0;
    uint32_t        m_slotCount = 0;
    uint64_t        m_lastAttemptMs = 0;
    uint32_t        m_lastWin32Error = 0;
    uint16_t        m_lastVersion = 0;
};

}  // namespace grt::shell
