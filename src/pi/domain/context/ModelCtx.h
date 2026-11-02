#pragma once

#include <string>

#include <pi/domain/model/Model.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// context 碎片: 当前激活的模型选择 (pi AgentSession.model + thinkingLevel).
// 由前端命令 (set_model/set_thinking_level、TUI /model /thinking) 经
// AgentSessionService 改写; Completer 每次发起 completion 时读它填进
// CompletionRequest —— 切换模型不重建 provider, 只改下一次请求.
// ---------------------------------------------------------------------------
struct ModelCtx {
   domain::Model activeModel;
   std::string   thinkingLevel = "medium";   // pi ThinkingLevel 全集之一
};

} // namespace pi::domain
