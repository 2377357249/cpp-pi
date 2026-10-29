#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/application/AgentTurnImpl.h>
#include <pi/application/Events.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): ToolRunner::dispatch() 无 tool call 时同步通过.
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(ExecuteTools) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      if (!info.toRole<ToolRunner>()->dispatch()) {
         return TSL_NS::Result::SUCCESS;
      }
      return WAIT_ON(EV_TOOLS_DONE, MSG_HANDLER(ToolsDoneMsg) {
         trans.toRole<ToolRunner>()->settle();
         return TSL_NS::Result::SUCCESS;
      });
   }
};

} // namespace pi::app
