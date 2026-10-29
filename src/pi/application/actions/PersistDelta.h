#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/event/Events.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): 无增量时同步通过; 否则挂起等 EV_STORE_DONE.
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(PersistDelta) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      if (!info.toRole<domain::Persister>()->begin()) {
         return TSL_NS::Result::SUCCESS;
      }
      return WAIT_ON(domain::EV_STORE_DONE, MSG_HANDLER(domain::StoreDoneMsg) {
         trans.toRole<domain::Persister>()->settle();
         return TSL_NS::Result::SUCCESS;
      });
   }
};

} // namespace pi::app
