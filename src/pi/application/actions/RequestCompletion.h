#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/event/Events.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): 业务在 role Completer 里, 这里只负责挂起/落地时机.
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(RequestCompletion) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      info.toRole<domain::Completer>()->begin();
      return WAIT_ON(domain::EV_LLM_DONE, MSG_HANDLER(domain::LlmDoneMsg) {
         trans.toRole<domain::Completer>()->settle();
         return TSL_NS::Result::SUCCESS;
      });
   }
};

} // namespace pi::app
