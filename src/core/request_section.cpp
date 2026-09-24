// 共享内存请求段实现（《技术实现设计》§4.5 / §5.4）
#include "request_section.h"

#include <cstring>
#include <mutex>

namespace grt {
namespace {

std::mutex            g_keepMutex;
std::vector<void*>    g_keepHandles;   // FIFO，上限见 kKeepAliveMax
uint32_t              g_seq = 0;

// 为什么保留句柄：命名段对象在"最后一个句柄关闭"时被销毁。若 DLL 写完就
// CloseHandle，接收方（可能还在启动 CreateProcess 的路上）再 OpenFileMapping
// 就会拿到 ERROR_FILE_NOT_FOUND —— 表现为"点了菜单没反应"。
// 因此这里按 FIFO 保活最多 8 个句柄（有界，不会随点击次数无界增长）。
constexpr size_t kKeepAliveMax = 8;

void KeepAlive(void* h) {
    std::lock_guard lock(g_keepMutex);
    g_keepHandles.push_back(h);
    while (g_keepHandles.size() > kKeepAliveMax) {
        void* old = g_keepHandles.front();
        g_keepHandles.erase(g_keepHandles.begin());
        if (old && old != INVALID_HANDLE_VALUE) ::CloseHandle(static_cast<HANDLE>(old));
    }
}

bool WriteAll(const std::wstring& name, const void* data, size_t bytes) {
    UniqueHandle mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(bytes), name.c_str()));
    if (mapping.get() == INVALID_HANDLE_VALUE || mapping.get() == nullptr) return false;
    void* view = ::MapViewOfFile(static_cast<HANDLE>(mapping.get()), FILE_MAP_WRITE, 0, 0, bytes);
    if (!view) return false;
    std::memcpy(view, data, bytes);
    ::UnmapViewOfFile(view);
    // mapping 的所有权转交给保活列表（不能在这里析构）
    KeepAlive(mapping.release());
    return true;
}

}  // namespace

std::wstring MakeRequestSectionName() {
    std::lock_guard lock(g_keepMutex);
    ++g_seq;
    return L"Local\\GitRT.Req." + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(g_seq);
}

bool WriteRequestSection(const std::wstring& name, const ShellRequest& req) {
    GrtReqHeader hdr;
    hdr.cmdId = req.cmdId;
    hdr.selMask = req.selMask;
    hdr.callerPid = ::GetCurrentProcessId();
    hdr.callerTid = ::GetCurrentThreadId();
    hdr.createdMs = ::GetTickCount64();

    std::vector<uint8_t> blob;
    blob.resize(sizeof(GrtReqHeader));
    std::memcpy(blob.data(), &hdr, sizeof(GrtReqHeader));

    uint32_t pathBytes = 0;
    for (const auto& p : req.paths) {
        const uint32_t n = static_cast<uint32_t>(p.size() * sizeof(wchar_t));
        const size_t at = blob.size();
        blob.resize(at + 4 + n);
        std::memcpy(blob.data() + at, &n, 4);
        if (n) std::memcpy(blob.data() + at + 4, p.data(), n);
        pathBytes += 4 + n;
    }
    if (!req.optionsJson.empty()) {
        blob.insert(blob.end(), req.optionsJson.begin(), req.optionsJson.end());
    }
    auto* hdrOut = reinterpret_cast<GrtReqHeader*>(blob.data());
    hdrOut->pathCount = static_cast<uint32_t>(req.paths.size());
    hdrOut->pathBytes = pathBytes;
    hdrOut->optionsBytes = static_cast<uint32_t>(req.optionsJson.size());

    if (!WriteAll(name, blob.data(), blob.size())) {
        GRT_LOGW("shell.req", "写请求段失败 name=" << U8(name) << " err=" << ::GetLastError());
        return false;
    }
    GRT_LOGI("shell.req", "请求段已写入 name=" << U8(name) << " cmd=" << req.cmdId
                                                 << " paths=" << req.paths.size()
                                                 << " bytes=" << blob.size());
    return true;
}

bool ReadRequestSection(const std::wstring& name, ShellRequest* out) {
    if (!out) return false;
    UniqueHandle mapping(::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()));
    if (mapping.get() == nullptr || mapping.get() == INVALID_HANDLE_VALUE) {
        GRT_LOGW("shell.req", "打开请求段失败 name=" << U8(name) << " err=" << ::GetLastError());
        return false;
    }
    // 先映射头部拿长度，再整体映射（两次映射，避免把整个段大小写死）
    const void* view = ::MapViewOfFile(static_cast<HANDLE>(mapping.get()), FILE_MAP_READ, 0, 0,
                                       sizeof(GrtReqHeader));
    if (!view) return false;
    GrtReqHeader hdr{};
    std::memcpy(&hdr, view, sizeof(GrtReqHeader));
    ::UnmapViewOfFile(view);
    if (hdr.magic != 0x51525247 || hdr.version != 1 || hdr.headerSize != sizeof(GrtReqHeader)) {
        GRT_LOGW("shell.req", "请求段头非法 magic=" << hdr.magic << " ver=" << hdr.version);
        return false;
    }
    const size_t total = sizeof(GrtReqHeader) + hdr.pathBytes + hdr.optionsBytes;
    const void* full = ::MapViewOfFile(static_cast<HANDLE>(mapping.get()), FILE_MAP_READ, 0, 0, total);
    if (!full) return false;
    const uint8_t* base = static_cast<const uint8_t*>(full);

    out->cmdId = hdr.cmdId;
    out->selMask = hdr.selMask;
    out->paths.clear();
    size_t at = sizeof(GrtReqHeader);
    for (uint32_t i = 0; i < hdr.pathCount; ++i) {
        if (at + 4 > total) break;
        uint32_t n = 0;
        std::memcpy(&n, base + at, 4);
        at += 4;
        if (at + n > total) break;
        out->paths.emplace_back(reinterpret_cast<const wchar_t*>(base + at), n / sizeof(wchar_t));
        at += n;
    }
    if (hdr.optionsBytes && at + hdr.optionsBytes <= total)
        out->optionsJson.assign(reinterpret_cast<const char*>(base + at), hdr.optionsBytes);
    else
        out->optionsJson.clear();
    ::UnmapViewOfFile(full);

    // cwd 约定：第一条路径若是目录则作为 cwd（shell 侧不再单独传字段）
    out->cwd.clear();
    if (!out->paths.empty() && PathIsDirectory(out->paths.front())) out->cwd = out->paths.front();
    GRT_LOGI("shell.req", "请求段已读取 cmd=" << out->cmdId << " paths=" << out->paths.size()
                                             << " options=" << out->optionsJson.size());
    return true;
}

void ReleaseKeptRequestSections() {
    std::lock_guard lock(g_keepMutex);
    for (void* h : g_keepHandles)
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(static_cast<HANDLE>(h));
    g_keepHandles.clear();
}

}  // namespace grt
