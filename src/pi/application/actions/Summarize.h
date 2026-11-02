#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/event/Events.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): 摘要 completion —— 业务在 Compactor role; 用两次
// (History / Prefix), 由 CompactCtx.phase 决定本步摘要哪一段.
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(Summarize) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      info.toRole<domain::Compactor>()->beginSummarize();
      return WAIT_ON(domain::EV_LLM_DONE, MSG_HANDLER(domain::LlmDoneMsg) {
         return trans.toRole<domain::Compactor>()->settleSummarize()
                ? TSL_NS::Result::SUCCESS : TSL_NS::Result::FAILED;
      });
   }
};

} // namespace pi::app
