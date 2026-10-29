#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/application/AgentTurnImpl.h>
#include <pi/application/Events.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): 业务在 role Completer 里, 这里只负责挂起/落地时机.
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(RequestCompletion) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      info.toRole<Completer>()->begin();
      return WAIT_ON(EV_LLM_DONE, MSG_HANDLER(LlmDoneMsg) {
         trans.toRole<Completer>()->settle();
         return TSL_NS::Result::SUCCESS;
      });
   }
};

} // namespace pi::app
