#pragma once

#include <trans-dsl/trans-dsl.h>
#include <trans-dsl/action/TransactionInfo.h>

#include <pi/application/actions/AssembleCompaction.h>
#include <pi/application/actions/PersistDelta.h>
#include <pi/application/actions/Summarize.h>
#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/context/CompactCtx.h>

namespace pi::iface {

// ---------------------------------------------------------------------------
// 压缩事务 (pi compact): Summarize(History) → [split turn 时 Summarize(Prefix)]
//   → AssembleCompaction (组 entry 落聚合) → PersistDelta (EV_STORE_DONE).
// 摘要 completion 与 turn 共用 EV_LLM_DONE 通道 —— 任一时刻只有一条事务
// 活跃, 事件不会串台.
// ---------------------------------------------------------------------------
struct NeedsHistorySummary {
   auto operator()(TSL_NS::TransactionInfo const& info) -> bool {
      auto* cx = info.toRole<domain::CompactCtx>();
      return cx->prep && !cx->prep->messagesToSummarize.empty();
   }
};

struct NeedsPrefixSummary {
   auto operator()(TSL_NS::TransactionInfo const& info) -> bool {
      auto* cx = info.toRole<domain::CompactCtx>();
      return cx->prep && cx->prep->isSplitTurn
             && !cx->prep->turnPrefixMessages.empty();
   }
};

using CompactProc = __transaction
(    __optional(NeedsHistorySummary, __with_id(1, __asyn(app::Summarize)))
   , __optional(NeedsPrefixSummary,  __with_id(2, __asyn(app::Summarize)))
   , __with_id(3, __asyn(app::AssembleCompaction))
   , __with_id(4, __asyn(app::PersistDelta))
);

} // namespace pi::iface
