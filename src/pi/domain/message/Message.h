#pragma once

#include <string>
#include <vector>

namespace pi::domain {

enum class Role { User, Assistant, Tool };

// 值对象: 一次工具调用请求
struct ToolCall {
   std::string id;
   std::string tool;
   std::string input;   // JSON 参数, 原样透传给执行器
};

// 值对象: 一条对话消息
struct Message {
   Role                  role;
   std::string           text;
   std::vector<ToolCall> toolCalls;
   std::string           toolCallId;  // role == Tool 时, 对应的调用 id
   std::string           toolName;    // role == Tool 时, 工具名
   bool                  isError = false;  // role == Tool 时, 执行是否出错
};

} // namespace pi::domain
