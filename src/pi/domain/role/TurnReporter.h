#pragma once

#include <cub/dci/Role.h>

#include <pi/domain/context/EventSinkCtx.h>
#include <pi/domain/context/TurnStateCtx.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// role: 一轮 assistant round 收尾时发 turn_end{message, toolResults}.
// "turn" 在 pi = 一个 assistant round (assistant 消息 + 其工具结果),
// 一轮 run 可以含多个 turn.
// ---------------------------------------------------------------------------
struct TurnReporter {
   USE_ROLE(EventSinkCtx);
   USE_ROLE(TurnStateCtx);

   auto reportTurnEnd() -> void {
      auto& turn = ROLE(TurnStateCtx);
      auto const& assistant = turn.lastAssistant.has_value()
                                ? *turn.lastAssistant
                                : turn.lastCompletion.message;
      ROLE(EventSinkCtx).sink->turnEnd(assistant, turn.roundToolResults);
      turn.roundToolResults.clear();
   }
};

} // namespace pi::domain
