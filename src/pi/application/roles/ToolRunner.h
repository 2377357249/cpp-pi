#pragma once

#include <cub/dci/Role.h>

#include <pi/application/context/EventQueueCtx.h>
#include <pi/application/context/SessionCtx.h>
#include <pi/application/context/TurnStateCtx.h>
#include <pi/application/Events.h>
#include <pi/domain/tool/Tool.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// role: 派发并收敛本轮 tool calls 的业务.
// 动态数量的并行调用聚合成单个 EV_TOOLS_DONE (结构确定的并行才用 __concurrent).
// ---------------------------------------------------------------------------
struct ToolRunner {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(TurnStateCtx);
   USE_ROLE(SessionCtx);

   explicit ToolRunner(domain::IToolExecutor& tools) : tools_(tools) {}

   // 返回 false = 无 tool call, action 同步通过
   auto dispatch() -> bool {
      auto& turn  = ROLE(TurnStateCtx);
      auto const& calls = turn.lastCompletion.message.toolCalls;
      if (calls.empty()) return false;

      turn.pendingToolResults.clear();
      turn.toolsExpected = calls.size();
      for (auto const& call : calls) {
         tools_.execute(call, [this](domain::ToolResult r) { onResult(std::move(r)); });
      }
      return true;
   }

   // EV_TOOLS_DONE 到达后: 工具结果落聚合 + 记持久化增量
   auto settle() -> void {
      auto& turn = ROLE(TurnStateCtx);
      for (auto const& r : turn.pendingToolResults) {
         turn.deltaMessages.push_back(
            domain::Message{domain::Role::Tool, r.content, {}, r.toolCallId});
      }
      ROLE(SessionCtx).session.appendToolResults(turn.pendingToolResults);
   }

private:
   auto onResult(domain::ToolResult r) -> void {
      auto& turn = ROLE(TurnStateCtx);
      turn.pendingToolResults.push_back(std::move(r));
      if (turn.pendingToolResults.size() == turn.toolsExpected) {
         ROLE(EventQueueCtx).post(EV_TOOLS_DONE, ToolsDoneMsg{});
      }
   }

   domain::IToolExecutor& tools_;
};

} // namespace pi::app
