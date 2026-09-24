// 右键菜单请求的接收端（《技术实现设计》§4.5 / §5.4）
//   Shell DLL 的 Invoke 把"命令 + 选区 + flags"写进共享内存段并直启本进程
//   （--request-section <name>）。这里负责读取、校验并翻译成 GUI 的请求结构。
#include "gui.h"

#include "config.h"
#include "json_util.h"
#include "request_section.h"

namespace grt::gui {

bool LoadShellMenuRequest(const std::wstring& sectionName, ShellMenuRequest* out, std::wstring* why) {
    if (!out) return false;
    grt::ShellRequest raw;
    if (sectionName.empty() || !grt::ReadRequestSection(sectionName, &raw)) {
        if (why) *why = Str(IDS_MSG_REQ_MISSING);
        return false;
    }
    out->cmdId = raw.cmdId;
    out->selMask = raw.selMask;
    out->paths = raw.paths;

    std::string v;
    if (JsonFindString(raw.optionsJson, "cmdKey", &v)) out->cmdKey = W(v);
    if (JsonFindString(raw.optionsJson, "repoRoot", &v)) out->repoHint = W(v);

    // flags 覆盖：DLL 已按 config.json 写好了，这里再兜一层（配置里开启的开关始终生效）
    std::map<std::string, std::string> flags;
    if (JsonFindStringMap(raw.optionsJson, "flags", &flags)) {
        for (const auto& kv : flags) out->flags[kv.first] = W(kv.second);
    }
    const CommandSpec* spec = FindCommand(out->cmdId);
    if (spec) {
        auto& store = ConfigStore::Instance();
        store.Reload();
        for (uint8_t i = 0; i < spec->flagCount; ++i) {
            const FlagSpec& f = spec->flags[i];
            if (out->flags.count(f.key)) continue;
            if (store.GetBool(FlagConfigKey(spec->key, f.key), f.defaultOn)) out->flags[f.key] = L"1";
        }
    }
    // 选区只有一条目录时它就是"工作目录/仓库提示"
    if (out->repoHint.empty() && raw.paths.size() == 1 && PathIsDirectory(raw.paths.front()))
        out->repoHint = raw.paths.front();

    GRT_LOGI("gui", "收到菜单请求 id=" << out->cmdId << " key=" << U8(out->cmdKey)
                                       << " paths=" << out->paths.size());
    return true;
}

}  // namespace grt::gui
