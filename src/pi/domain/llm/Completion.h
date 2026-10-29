#pragma once

#include <string>
#include <vector>

#include <pi/domain/llm/StopReason.h>
#include <pi/domain/message/Message.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

struct CompletionRequest {
   std::vector<Message> messages;
   std::vector<Tool>    tools;
   std::string          systemPrompt;
};

struct CompletionResult {
   Message    message;     // assistant 消息, 可携带 toolCalls
   StopReason stopReason;
};

} // namespace pi::domain
