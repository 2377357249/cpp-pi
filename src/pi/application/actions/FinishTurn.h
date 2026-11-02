#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/domain/AgentTurnImpl.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): 一轮 assistant round 收尾, 同步发 turn_end ——
// pi inner loop 的 emit({type:"turn_end"}) 对应点.
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(FinishTurn) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      info.toRole<domain::TurnReporter>()->reportTurnEnd();
      return TSL_NS::Result::SUCCESS;
   }
};

} // namespace pi::app
