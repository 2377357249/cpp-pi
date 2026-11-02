#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <pi/domain/message/Message.h>

namespace pi::domain {

// 工具规格(发给 LLM 的描述)
struct Tool {
   std::string name;
   std::string description;
   std::string inputSchema;  // JSON Schema
};

// 一次工具执行的结果 (pi ToolResultMessage)
struct ToolResult {
   std::string toolCallId;
   std::string toolName;
   std::string content;                 // text 块内容
   std::vector<ImageData> images;       // image 块
   std::string detailsJson;             // details (JSON 原文, 如 edit diff)
   int64_t     durationMs = -1;         // <0 = 未知
   bool        isError = false;
};

using ToolResultHandler = std::function<void(ToolResult)>;

// ToolResult -> toolResult 消息的唯一构造点 (pi createToolResultMessage):
// 发射帧与持久化必须共用同一条消息, 时间戳在建消息时钉死.
inline auto toolResultMessage(ToolResult const& r) -> Message {
   Message m{Role::Tool, r.content, {}, r.toolCallId, r.toolName, r.isError};
   m.images      = r.images;
   m.detailsJson = r.detailsJson;
   m.durationMs  = r.durationMs;
   m.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
   return m;
}

// ---------------------------------------------------------------------------
// 端口: 工具执行器
// 实现方只需"最终调一次 done 交出结果", 不感知事务/事件的存在.
// ---------------------------------------------------------------------------
struct IToolExecutor {
   virtual ~IToolExecutor() = default;

   virtual void execute(ToolCall const& call, ToolResultHandler done) = 0;
   virtual auto availableTools() const -> std::vector<Tool> = 0;
};

} // namespace pi::domain
