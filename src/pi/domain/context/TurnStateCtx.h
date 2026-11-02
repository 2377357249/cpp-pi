#pragma once

#include <atomic>
#include <optional>
#include <vector>

#include <pi/domain/llm/Completion.h>
#include <pi/domain/llm/StreamEvent.h>
#include <pi/domain/message/Message.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// context 碎片: 一轮 turn 内的跨 action 中间态
// ---------------------------------------------------------------------------
struct TurnStateCtx {
   domain::CompletionResult        lastCompletion;      // 本次 LLM 响应
   size_t                          toolsExpected = 0;   // 已宣布的 tool 数
   bool                            toolsDispatching = false; // 派发循环进行中
   bool                            lastStoreOk = true;
   // ---- 本轮 run 的事件发射态 (runTurn 起点复位) ----
   // lastAssistant: 最近一条落库的 assistant (turn_end.message);
   // roundToolResults: 本轮已发射的 toolResult 消息 (turn_end.toolResults,
   //   持久化时按它们落库, emitted == persisted);
   // streamingStarted: 本次 completion 是否已见 Start 事件 (发过骨架);
   // completionsSeen: 本 run 已发起的 completion 数, ≥2 的轮次要补 turn_start.
   std::optional<domain::Message>  lastAssistant;
   std::vector<domain::Message>    roundToolResults;
   bool                            streamingStarted = false;
   size_t                          completionsSeen = 0;
   // 用户中止旗标 (pi agent.abort()): 非空时由谓词/工具派发周期检查.
   std::atomic<bool>*              abortRequested = nullptr;

   auto aborted() const -> bool {
      return abortRequested != nullptr && abortRequested->load();
   }
};

} // namespace pi::domain
