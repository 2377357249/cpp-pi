#pragma once

#include <atomic>
#include <vector>

#include <pi/domain/llm/Completion.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// context 碎片: 一轮 turn 内的跨 action 中间态
// ---------------------------------------------------------------------------
struct TurnStateCtx {
   domain::CompletionResult        lastCompletion;      // 本次 LLM 响应
   std::vector<domain::ToolResult> pendingToolResults;  // 收集中的工具结果
   size_t                          toolsExpected = 0;
   bool                            lastStoreOk = true;
   // 用户中止旗标 (pi agent.abort()): 非空时由谓词/工具派发周期检查.
   std::atomic<bool>*              abortRequested = nullptr;

   auto aborted() const -> bool {
      return abortRequested != nullptr && abortRequested->load();
   }
};

} // namespace pi::domain
