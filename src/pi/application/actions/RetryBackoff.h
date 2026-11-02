#pragma once

#include <trans-dsl/action/SimpleAsyncAction.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/event/Events.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// action(薄壳): auto-retry 的 backoff 睡眠 —— Retryer::begin 发
// auto_retry_start 并把延时投递交给 ioPool; EV_RETRY_DUE 到达后 settle
// (正常到期 -> 链在飞; 被取消 -> retryCancelled + auto_retry_end).
// ---------------------------------------------------------------------------
DEF_SIMPLE_ASYNC_ACTION(RetryBackoff) {
   auto exec(TSL_NS::TransactionInfo const& info) -> TSL_NS::Status {
      info.toRole<domain::Retryer>()->begin();
      return WAIT_ON(domain::EV_RETRY_DUE, MSG_HANDLER(domain::TimerDoneMsg) {
         trans.toRole<domain::Retryer>()->settle();
         return TSL_NS::Result::SUCCESS;
      });
   }
};

} // namespace pi::app
