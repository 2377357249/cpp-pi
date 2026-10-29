#pragma once

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

// 一次工具执行的结果
struct ToolResult {
   std::string toolCallId;
   std::string toolName;
   std::string content;
   bool        isError = false;
};

using ToolResultHandler = std::function<void(ToolResult)>;

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
