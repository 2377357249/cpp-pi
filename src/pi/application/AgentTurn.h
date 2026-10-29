#pragma once

#include <cstdio>

#include <trans-dsl/trans-dsl.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/application/AgentTurnImpl.h>
#include <pi/application/actions/ExecuteTools.h>
#include <pi/application/actions/PersistDelta.h>
#include <pi/application/actions/RequestCompletion.h>
#include <pi/application/context/TurnStateCtx.h>

namespace pi::app {

// 轮次结束谓词: 本次 completion 未再请求工具调用
struct TurnDone {
   auto operator()(TSL_NS::TransactionInfo const& info) -> bool {
      auto* turn = info.toRole<TurnStateCtx>();
      return turn->lastCompletion.message.toolCalls.empty();
   }
};

// ---------------------------------------------------------------------------
// pi 的 agent loop, 事务化表达:
//   loop { LLM completion → 执行工具(可空) → 落盘增量(可空) } until 无 tool calls
// 上限 64 轮防失控; 耗尽返回 USER_FATAL_BUG.
// (本版 trans-dsl 的 __loop_max/__forever 引用未定义的 Loop_,
//  直接实例化 Loop<N,...> —— __loop 即 Loop<1,...>)
// ---------------------------------------------------------------------------
inline constexpr uint32_t MAX_TURN_ITERATIONS = 64;

using AgentTurn = __transaction
( TSL_NS::details::Loop<MAX_TURN_ITERATIONS,
      __with_id(1, __asyn(RequestCompletion))
    , __with_id(2, __asyn(ExecuteTools))
    , __with_id(5, __asyn(PersistDelta))
    , __until(TurnDone)
   >
);

// ---------------------------------------------------------------------------
// 可观测性: telemetry / footer 状态条的雏形.
// __with_id 给 action 打上 AID, 监听器只观测关心的生命周期.
// ---------------------------------------------------------------------------
struct TurnObserver : TSL_NS::ObservedActionIdRegistry<1, 2, 5> {
   auto onActionStarting(TSL_NS::ActionId aid, TSL_NS::TransactionInfo const&) -> void {
      std::printf("    >> turn: action #%u starting\n", aid);
   }
   auto onActionDone(TSL_NS::ActionId aid, TSL_NS::TransactionInfo const&, TSL_NS::Status st) -> void {
      std::printf("    >> turn: action #%u done, status=%u\n", aid, st);
   }
};

using TurnListeners      = __listeners(TurnObserver);
using MonitoredAgentTurn = __bind_listener(AgentTurn, TurnListeners);

} // namespace pi::app
