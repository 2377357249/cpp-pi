#pragma once

#include <trans-dsl/trans-dsl.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/infra/log/Logging.h>

#include <pi/domain/AgentTurnImpl.h>
#include <pi/application/actions/ExecuteTools.h>
#include <pi/application/actions/FinishTurn.h>
#include <pi/application/actions/PersistDelta.h>
#include <pi/application/actions/RequestCompletion.h>
#include <pi/application/actions/RetryBackoff.h>
#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/turn/RetryPolicy.h>
#include <pi/domain/turn/TurnPolicy.h>

namespace pi::iface {

// 轮次结束谓词: 本次 completion 未再请求工具调用 或 用户已中止
// (规则在 domain::TurnPolicy; abort 旗标在 TurnStateCtx).
// retry 链未决 (backoff 已落地等重试) 时不可收官.
struct TurnDone {
   auto operator()(TSL_NS::TransactionInfo const& info) -> bool {
      auto* turn = info.toRole<domain::TurnStateCtx>();
      return turn->aborted()
             || (domain::TurnPolicy::isFinished(turn->lastCompletion)
                 && !domain::RetryPolicy::shouldRetry(*turn)
                 && !turn->retryInFlight);
   }
};

// backoff 前置谓词: 末次 completion 是可重试错误且链未满 -> 跑 RetryBackoff.
struct RetryPending {
   auto operator()(TSL_NS::TransactionInfo const& info) -> bool {
      return domain::RetryPolicy::shouldRetry(
         *info.toRole<domain::TurnStateCtx>());
   }
};

// ---------------------------------------------------------------------------
// pi 的 agent loop, 事务化表达:
//   loop { LLM completion → 执行工具(可空) → 落盘增量(可空)
//          → [retryable error 时 backoff 等 EV_RETRY_DUE] } until 轮次结束
// 上限 64 轮防失控; 耗尽返回 USER_FATAL_BUG.
// (本版 trans-dsl 的 __loop_max/__forever 引用未定义的 Loop_,
//  直接实例化 Loop<N,...> —— __loop 即 Loop<1,...>)
// ---------------------------------------------------------------------------
inline constexpr uint32_t MAX_TURN_ITERATIONS = 64;

using AgentTurn = __transaction
( TSL_NS::details::Loop<MAX_TURN_ITERATIONS,
      __with_id(1, __asyn(app::RequestCompletion))
    , __with_id(2, __asyn(app::ExecuteTools))
    , __with_id(3, __asyn(app::FinishTurn))
    , __with_id(5, __asyn(app::PersistDelta))
    , __optional(RetryPending, __with_id(6, __asyn(app::RetryBackoff)))
    , __until(TurnDone)
   >
);

// ---------------------------------------------------------------------------
// 可观测性: telemetry / footer 状态条的雏形.
// __with_id 给 action 打上 AID, 监听器只观测关心的生命周期.
// ---------------------------------------------------------------------------
struct TurnObserver : TSL_NS::ObservedActionIdRegistry<1, 2, 3, 5, 6> {
   auto onActionStarting(TSL_NS::ActionId aid, TSL_NS::TransactionInfo const&) -> void {
      if (auto* lg = pi::infra::log::logger())
         LOG_INFO(lg, "turn: action #{} starting", aid);
   }
   auto onActionDone(TSL_NS::ActionId aid, TSL_NS::TransactionInfo const&, TSL_NS::Status st) -> void {
      if (auto* lg = pi::infra::log::logger())
         LOG_INFO(lg, "turn: action #{} done, status={}", aid, st);
   }
};

using TurnListeners      = __listeners(TurnObserver);
using MonitoredAgentTurn = __bind_listener(AgentTurn, TurnListeners);

} // namespace pi::iface
