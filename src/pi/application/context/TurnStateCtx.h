#pragma once

#include <vector>

#include <pi/domain/llm/Completion.h>
#include <pi/domain/tool/Tool.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// context 碎片: 一轮 turn 内的跨 action 中间态
// ---------------------------------------------------------------------------
struct TurnStateCtx {
   domain::CompletionResult        lastCompletion;      // 本次 LLM 响应
   std::vector<domain::ToolResult> pendingToolResults;  // 收集中的工具结果
   size_t                          toolsExpected = 0;
   std::vector<domain::Message>    deltaMessages;       // 待持久化的增量消息
   bool                            lastStoreOk = true;
};

} // namespace pi::app
