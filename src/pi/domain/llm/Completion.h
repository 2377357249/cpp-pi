#pragma once

#include <string>
#include <vector>

#include <pi/domain/llm/StopReason.h>
#include <pi/domain/message/Message.h>
#include <pi/domain/model/Model.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

struct CompletionRequest {
   std::vector<Message> messages;
   std::vector<Tool>    tools;
   std::string          systemPrompt;
   // 模型选择 (ModelCtx 快照): model.id 空 = provider 构造默认;
   // thinkingLevel ∈ off|minimal|low|medium|high|xhigh|max, provider 自行映射.
   domain::Model        model;
   std::string          thinkingLevel = "medium";
};

struct CompletionResult {
   Message    message;     // assistant 消息, 可携带 toolCalls
   StopReason stopReason;
};

} // namespace pi::domain
