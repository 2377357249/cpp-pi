#pragma once

#include <utility>

#include <cub/dci/Role.h>

#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/EventSinkCtx.h>
#include <pi/domain/context/SessionCtx.h>
#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/event/Events.h>
#include <pi/domain/tool/Tool.h>
#include <pi/domain/turn/TurnPolicy.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// role: 派发并收敛本轮 tool calls 的业务.
// 动态数量的并行调用聚合成单个 EV_TOOLS_DONE (结构确定的并行才用 __concurrent).
// 事件发射 (pi executeToolCallsParallel): 每次调用先 tool_execution_start,
// abort 落在派发中 -> 该次回 "Operation aborted" 结果后 break
// (之后的调用连 start 都不发); 结果到达即 tool_execution_end +
// toolResult message_start/end; 消息本体经 toolResultMessage 一次构造,
// 发射与落库共用.
// ---------------------------------------------------------------------------
struct ToolRunner {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(EventSinkCtx);
   USE_ROLE(TurnStateCtx);
   USE_ROLE(SessionCtx);

   explicit ToolRunner(domain::IToolExecutor& tools) : tools_(tools) {}

   // 返回 false = 无 tool call / 本回合不派发 (error/aborted), action 同步通过
   auto dispatch() -> bool {
      auto& turn = ROLE(TurnStateCtx);
      if (!domain::TurnPolicy::needsTools(turn.lastCompletion)) return false;

      auto const& calls = turn.lastCompletion.message.toolCalls;
      auto&       sink  = *ROLE(EventSinkCtx).sink;

      turn.roundToolResults.clear();
      turn.toolsExpected    = 0;
      turn.toolsDispatching = true;
      for (auto const& call : calls) {
         sink.toolExecutionStart(call.id, call.tool, call.input);
         ++turn.toolsExpected;   // 每个 start 保证配对一份 result
         // pi: 中止后回 "Operation aborted" isError 结果, 并停止宣布新 start
         if (turn.aborted()) {
            onResult(domain::ToolResult{call.id, call.tool,
                                        "Operation aborted", {}, "", -1, true});
            break;
         }
         tools_.execute(call, [this](domain::ToolResult r) {
            onResult(std::move(r));
         });
         if (turn.aborted()) break;
      }
      turn.toolsDispatching = false;
      maybePostToolsDone();   // 同步执行器的结果在派发循环内已收齐
      return true;
   }

   // EV_TOOLS_DONE 到达后: 已发射的 toolResult 消息原样落库
   // (emitted == persisted, 持久化增量由 Session 记账)
   auto settle() -> void {
      auto& turn  = ROLE(TurnStateCtx);
      auto& sess  = ROLE(SessionCtx).session;
      for (auto const& m : turn.roundToolResults) sess.appendMessage(m);
   }

private:
   auto onResult(domain::ToolResult r) -> void {
      auto& turn = ROLE(TurnStateCtx);
      auto& sink = *ROLE(EventSinkCtx).sink;
      auto msg = domain::toolResultMessage(r);
      sink.toolExecutionEnd(msg.toolCallId, msg.toolName, msg);
      sink.messageStart(msg);
      sink.messageEnd(msg);
      turn.roundToolResults.push_back(std::move(msg));
      maybePostToolsDone();
   }

   // 收齐 = 派发循环结束 且 已宣布的调用全有结果 (abort 截断时
   // toolsExpected 只计已宣布数 —— 与 pi finalizedCalls 一致).
   auto maybePostToolsDone() -> void {
      auto& turn = ROLE(TurnStateCtx);
      if (!turn.toolsDispatching && turn.toolsExpected > 0
          && turn.roundToolResults.size() == turn.toolsExpected) {
         ROLE(EventQueueCtx).post(EV_TOOLS_DONE, ToolsDoneMsg{});
      }
   }

   domain::IToolExecutor& tools_;
};

} // namespace pi::domain
