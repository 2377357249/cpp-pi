#pragma once

#include <pi/domain/llm/Completion.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 领域规则: 一轮 turn 是否结束 / 是否需要执行工具 ——
// 判定统一走这里, 不在谓词/action 里散落 toolCalls.empty().
// ---------------------------------------------------------------------------
struct TurnPolicy {
   static auto isFinished(CompletionResult const& r) -> bool {
      return r.message.toolCalls.empty();
   }
   static auto needsTools(CompletionResult const& r) -> bool {
      return !r.message.toolCalls.empty();
   }
};

} // namespace pi::domain
