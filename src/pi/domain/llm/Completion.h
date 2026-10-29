#pragma once

#include <string>
#include <vector>

#include <pi/domain/message/Message.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

struct CompletionRequest {
   std::vector<Message> messages;
   std::vector<Tool>    tools;
   std::string          systemPrompt;
};

struct CompletionResult {
   Message     message;     // assistant 消息, 可携带 toolCalls
   std::string stopReason;  // "end_turn" | "tool_use" | ...
};

} // namespace pi::domain
