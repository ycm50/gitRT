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
    // 系统提示词：留空 = 用内置默认（见 BuildPlannerSystemPrompt / EffectiveSystemPrompt）
    std::wstring systemPrompt;
    std::wstring configPath;      // %APPDATA%\GitRT\config.json
    std::string  loadNote;        // 配置读取/生成的说明（非致命）

    // ── 产品决策（2026-09-24，产品负责人选择）──────────────────────────────
    // AI 设置的**主存储**是 exe 同目录的 `GitRT.ai.json`（**明文**，方便直接查看/修改，
    // 也不受"改了环境变量要重启资源管理器"的影响）。键的解析优先级：
    //   ① GitRT.ai.json 的 apiKey（界面里填的，明文）
    //   ② 该文件（或 config.json）里 apiKeyEnv 指定的环境变量（默认 DEEPSEEK_API_KEY）
    // 安全提示：明文落盘意味着**任何能读该目录的进程都能读到 Key**；介意的话把
    // apiKey 留空、只设环境变量即可（两条路都保留）。
    std::wstring keyFilePath;     // <exe 目录>\GitRT.ai.json
    std::wstring apiKey;          // 来自 keyFilePath 的明文 Key（空 = 未设置）
    bool         keyFromFile = false;
    bool         keyFileUsable = false;   // 目录可写（不可写时界面要明确告知）
};

AiConfig LoadAiConfig();
// 写 exe 同目录的 GitRT.ai.json（端点/模型/超时/Key 全在里面，明文）
bool     SaveAiConfig(const AiConfig& cfg);
// 取 Key：优先 GitRT.ai.json 的明文 apiKey，其次 apiKeyEnv 指向的环境变量。
// 返回空 = 未配置（此时 UI 给出配置指引）
std::wstring ResolveApiKey(const AiConfig& cfg);
// exe 同目录的 AI 设置文件路径（界面与自检共用）
std::wstring AiKeyFilePath();

// ------------------------------------------------------------------ 计划
struct AiPlan {
    bool         ok = false;
    bool         noCommand = false;   // 模型认为没有合适的命令
    std::string  commandKey;
    // 模型想"直接给命令"时走这里：一条只读 git 命令行（如 git status -sb）。
    // 与 commandKey 互斥：cmdline 非空优先。真正的放行由 PlanToCommand() 的只读白名单决定。
    std::string  cmdline;
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

// GET（模型列表等只读接口用）
HttpResponse HttpGetJson(const std::wstring& url,
                         const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                         uint32_t timeoutMs);

// ------------------------------------------------------------ 模型列表
// OpenAI 兼容：GET <endpoint 的 base>/models → {"data":[{"id":"deepseek-chat"}, …]}
// endpoint 里的 "/chat/completions"（或 "/completions"）会被自动换成 "/models"。
struct ModelListResult {
    int                       status = 0;   // HTTP 状态（0 = 没连上）
    std::vector<std::wstring> models;
    std::wstring              url;          // 实际请求的地址（界面显示/排错用）
    std::string               error;        // 传输层错误（status==0 时看这里）
};
ModelListResult FetchModelList(const AiConfig& cfg);

// endpoint → 模型列表地址（导出来做单测：/chat/completions 与 /completions 会被替换成 /models）
std::wstring AiModelsUrlFromEndpoint(const std::wstring& endpoint);

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
// 生效的系统提示词：配置里填了就用它，否则用内置默认
std::string EffectiveSystemPrompt(const AiConfig& cfg, const TitleResolver& titleRes);

// AI 直出命令的只读校验：把一行 "git …" 拆成 argv（不含 exe）。
// 只放行只读子命令；写操作一律拒绝并给出中文原因（why）。
bool ParseReadOnlyGitCommand(const std::wstring& line, std::vector<std::wstring>* argv, std::wstring* why);

PlanToCommandResult PlanToCommand(const AiPlan& plan, const std::wstring& repoRoot,
                                  const std::vector<std::wstring>& paths,
                                  const std::wstring& gitExe);

}  // namespace grt
