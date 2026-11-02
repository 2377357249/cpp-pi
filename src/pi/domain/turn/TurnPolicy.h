#pragma once

#include <pi/domain/llm/Completion.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 领域规则: 一轮 turn 是否结束 / 是否需要执行工具 ——
// 判定统一走这里, 不在谓词/action 里散落 toolCalls.empty().
// ---------------------------------------------------------------------------
struct TurnPolicy {
   // pi runLoop: error/aborted 的 assistant 直接收尾 (turn_end+agent_end),
   // 不进工具派发 —— 即使消息上挂着 toolCalls.
   static auto isFinished(CompletionResult const& r) -> bool {
      return r.stopReason == StopReason::Error
          || r.stopReason == StopReason::Aborted
          || r.message.toolCalls.empty();
   }
   static auto needsTools(CompletionResult const& r) -> bool {
      return !isFinished(r);
   }
};

} // namespace pi::domain
