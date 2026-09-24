#pragma once
// ---------------------------------------------------------------------------
// config.json 访问层（《技术实现设计》§12.1）
//
// 为什么需要它（而不是各处直接读写文件）：
//   1) config.json 是**多个进程共享**的（Shell DLL 切复选开关、GUI 存 AI 配置、
//      将来的 Broker 存状态参数）。任何"整文件覆盖写"都会丢掉别人的键，
//      因此这里把文件建模为**扁平键值表**（键可含点号，如
//      "flags.sync.pull.rebase"），读写都经过同一个有序表。
//   2) Shell 侧只允许做"读小 JSON 缓存"级别的 I/O（§3.7），所以读入结果带
//      时间戳缓存，同一进程 5 秒内不重复读盘；写盘为原子替换。
//
// 尚未识别的键**原样保留**（不丢用户配置）；损坏的 JSON 会备份为
// config.bad-<ts>.json 后以默认值继续（§12.1 的迁移要求）。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core.h"

namespace grt {

class ConfigStore {
public:
    static ConfigStore& Instance();

    // 从磁盘刷新（默认带 5 s 节流；force = 忽略节流）。菜单/面板调用点用默认参数。
    void Reload(bool force = false);

    std::wstring Path() const;

    // ---- 读（缺失/类型不符一律返回默认值，绝不抛异常）----
    std::string  GetString(const std::string& key, const std::string& def = {}) const;
    bool         GetBool(const std::string& key, bool def) const;
    int          GetInt(const std::string& key, int def) const;
    // 字符串数组；JSON 数组或"分号/换行分隔的字符串"都接受
    std::vector<std::string> GetStringArray(const std::string& key) const;
    bool         Has(const std::string& key) const;

    // ---- 写（只改内存，Save() 才落盘）----
    void SetString(const std::string& key, const std::string& value);
    void SetBool(const std::string& key, bool value);
    void SetInt(const std::string& key, int value);
    void SetStringArray(const std::string& key, const std::vector<std::string>& values);

    // 原子写盘：同目录 .tmp → MoveFileEx(REPLACE_EXISTING)。失败只返回 false。
    bool Save();

    // 供测试与 doctor 使用：当前内存中的键数、最近一次解析是否成功
    size_t      KeyCount() const;
    bool        LastLoadOk() const;
    std::string LastLoadNote() const;

    // 清空内存表并以默认值重建（测试用）
    void ResetForTest();

private:
    ConfigStore() = default;
    struct Impl;
};

// ---------------------------------------------------------------- 语义化便捷函数
// 复选开关的持久化键：flags.<commandKey>.<flagKey>（§12.1）
std::string FlagConfigKey(const std::string& commandKey, const std::string& flagKey);
bool        ReadFlagOverride(const std::string& commandKey, const std::string& flagKey, bool defaultOn);
void        WriteFlagOverride(const std::string& commandKey, const std::string& flagKey, bool value);

}  // namespace grt
