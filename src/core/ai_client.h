#pragma once
// ---------------------------------------------------------------------------
// AI 助手（《产品设计》§4.3 层级 4 / 《技术实现设计》§16）
//
// 安全模型（最重要的一条）：
//   AI 只输出"从命令表中选哪条 + 参数 + 选项"的结构化计划，**永远不执行 AI 生成的命令行**。
//   计划必须通过 PlanToCommand() 走完与菜单/参数面板完全相同的校验与 argv 构造路径；
//   任何不在命令表内的 key、任何不在 FlagSpec 白名单内的选项都会被拒绝或忽略。
// ---------------------------------------------------------------------------

#include <functional>
#include <map>

#include "command_builder.h"
#include "command_spec.h"
#include "core.h"

namespace grt {

// 标题解析器：由调用方（GUI / CLI，二者都带 resources.rc）提供，
// 这样 core 不必依赖 GetModuleHandle + LoadString，未来 Shell DLL 也能复用同一套提示词。
using TitleResolver = std::function<std::string(uint16_t titleRes)>;

// ------------------------------------------------------------------ 配置
struct AiConfig {
    std::wstring endpoint = L"https://api.deepseek.com/chat/completions";
    std::wstring model = L"deepseek-chat";
    std::string  apiKeyEnv = "DEEPSEEK_API_KEY";
    uint32_t     timeoutMs = 60000;
    std::wstring configPath;      // %APPDATA%\GitRT\config.json
    std::string  loadNote;        // 配置读取/生成的说明（非致命）
};

AiConfig LoadAiConfig();
bool     SaveAiConfig(const AiConfig& cfg);
// 只从环境变量取；返回空 = 未配置（此时 UI 给出配置指引）
std::wstring ResolveApiKey(const AiConfig& cfg);

// ------------------------------------------------------------------ 计划
struct AiPlan {
    bool         ok = false;
    bool         noCommand = false;   // 模型认为没有合适的命令
    std::string  commandKey;
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> flags;
    std::wstring explanation;
    std::wstring rawReply;            // 模型原始回复（界面"详情"用）
    std::wstring error;               // 失败原因（中文，可直接显示）
    int          httpStatus = 0;
    uint64_t     elapsedMs = 0;
    std::string  requestBody;         // 便于排查（已脱敏，不含 api key）
};

// ------------------------------------------------------------------ HTTP
struct HttpResponse {
    int         status = 0;
    std::string body;
    std::string error;
};
HttpResponse HttpPostJson(const std::wstring& url,
                          const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                          const std::string& bodyUtf8, uint32_t timeoutMs);

// ------------------------------------------------------------ 提示词与解析
// 由 CommandTable() 自动生成，保证与实际命令表永不脱节；
// titleRes 为空时只输出 key/参数/选项（仍然可用）
std::string BuildPlannerSystemPrompt(const TitleResolver& titleRes = {});
std::string BuildPlannerUserPrompt(const std::wstring& userText, const std::wstring& repoRoot,
                                   const std::wstring& branch,
                                   const std::vector<std::wstring>& paths);
// 从 OpenAI 兼容响应体里取出 choices[0].message.content
bool ExtractChatContent(const std::string& envelope, std::string* content);
// 解析模型回复（内容字符串）为计划；可单测
AiPlan ParsePlanReply(const std::string& replyContent);

// 端到端：配置 + 提示词 + HTTP + 解析
AiPlan GeneratePlan(const AiConfig& cfg, const std::string& systemPrompt, const std::string& userPrompt);

// ------------------------------------------------- 计划 → 可执行命令（安全闸门）
struct PlanToCommandResult {
    bool               ok = false;
    const CommandSpec* spec = nullptr;
    BuiltCommand       built;
    std::wstring       error;
    std::vector<std::wstring> warnings;   // 被忽略的未知选项等
};
PlanToCommandResult PlanToCommand(const AiPlan& plan, const std::wstring& repoRoot,
                                  const std::vector<std::wstring>& paths,
                                  const std::wstring& gitExe);

}  // namespace grt
