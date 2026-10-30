#pragma once

#include <cub/dci/Role.h>

#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/SessionCtx.h>
#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/event/Events.h>
#include <pi/domain/tool/Tool.h>
#include <pi/domain/turn/TurnPolicy.h>

namespace pi::domain {

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
      auto& turn = ROLE(TurnStateCtx);
      if (!domain::TurnPolicy::needsTools(turn.lastCompletion)) return false;

      auto const& calls = turn.lastCompletion.message.toolCalls;

      turn.pendingToolResults.clear();
      turn.toolsExpected = calls.size();
      for (auto const& call : calls) {
         // pi: 中止后剩余调用不再执行, 回 "Operation aborted" isError 结果
         if (turn.aborted()) {
            onResult(domain::ToolResult{call.id, call.tool,
                                        "Operation aborted", {}, "", -1, true});
            continue;
         }
         tools_.execute(call, [this](domain::ToolResult r) { onResult(std::move(r)); });
      }
      return true;
   }

   // EV_TOOLS_DONE 到达后: 工具结果经聚合落地 (Message 构造与增量记账在 Session)
   auto settle() -> void {
      auto& turn = ROLE(TurnStateCtx);
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

} // namespace pi::domain
