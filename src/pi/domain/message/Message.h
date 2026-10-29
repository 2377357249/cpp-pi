#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pi::domain {

enum class Role { System, User, Assistant, Tool };

// pi StopReason 全集 ("pending"|"stop"|"length"|"toolUse"|"error"|"aborted"|"deferred").
// 枚举放这里(而不是 StopReason.h)避免 include 环: Message 携带 stopReason 字段.
enum class StopReason { Pending, Stop, Length, ToolUse, Error, Aborted, Deferred };

// 值对象: 一次工具调用请求
// (pi ToolCall: {type:"toolCall", id, name, arguments, thoughtSignature?,
//                namespace?, argumentsError?}; arguments 在本层保持 JSON 串)
struct ToolCall {
   std::string id;
   std::string tool;
   std::string input;             // JSON 参数, 原样透传给执行器
   std::string thoughtSignature;  // Google 思维签名(回放用)
   std::string ns;                // OpenAI responses 命名空间
   std::string argumentsError;    // 参数非法 JSON 时, arguments 记为 {} 并记录原因
};

// 值对象: 图片块 (pi ImageContent: {type:"image", data:base64, mimeType})
struct ImageData {
   std::string data;      // base64
   std::string mimeType;
};

// 值对象: 思维块 (pi ThinkingContent)
struct ThinkingBlock {
   std::string thinking;
   std::string signature;   // thinkingSignature
   bool        redacted = false;
};

// pi Usage.cost
struct ModelCost {
   double input = 0, output = 0, cacheRead = 0, cacheWrite = 0, total = 0;
   bool   totalFromProvider = false;
};

// pi Usage (assistant 消息 / token 计费)
struct Usage {
   int64_t input = 0, output = 0, cacheRead = 0, cacheWrite = 0;
   int64_t totalTokens  = 0;
   int64_t reasoning    = -1;   // <0 = provider 未上报
   int64_t cacheWrite1h = -1;   // <0 = 未上报
   ModelCost cost;
};

// 值对象: 一条对话消息 (pi Message 联合类型的域内表达).
// 扁平主字段保持向后兼容: text = 全部 TextContent 拼接,
// toolCalls/images/thinking 承载结构化块; 线格式由 interface/rpc 编解码.
struct Message {
   Role                  role;
   std::string           text;
   std::vector<ToolCall> toolCalls;
   std::string           toolCallId;  // role == Tool 时, 对应的调用 id
   std::string           toolName;    // role == Tool 时, 工具名
   bool                  isError = false;

   // ---- 结构化块 ----
   std::vector<ThinkingBlock> thinking;       // assistant
   std::vector<ImageData>     images;         // user / toolResult
   std::string detailsJson;                   // toolResult details (JSON 原文)
   std::string textSignature;                 // assistant textSignature

   // ---- 时间 ----
   int64_t timestampMs = 0;   // 0 = 由 codec 现场生成
   int64_t durationMs  = -1;  // <0 = 未知 (assistant/toolResult)

   // ---- assistant 元数据 (pi AssistantMessage) ----
   std::string api, provider, model;
   std::string responseModel, responseId, errorMessage;
   StopReason  stopReason = StopReason::Pending;  // Pending = 未标注, codec 推导
   Usage       usage;
   bool        hasEndTurn = false;   // provider 是否上报了 endTurn
   bool        endTurn    = false;
};

} // namespace pi::domain
