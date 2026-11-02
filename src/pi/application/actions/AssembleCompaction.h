#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/domain/AgentTurnImpl.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): 摘要齐了 -> Compactor::assemble 组 CompactionEntry 落聚合.
// 持久化增量交给后面的 PersistDelta (与 turn 共用同一 WAIT_ON 通道).
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(AssembleCompaction) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      info.toRole<domain::Compactor>()->assemble();
      return TSL_NS::Result::SUCCESS;
   }
};

} // namespace pi::app
