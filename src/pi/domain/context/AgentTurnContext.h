#pragma once

#include <pi/domain/context/CompactCtx.h>
#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/EventSinkCtx.h>
#include <pi/domain/context/ModelCtx.h>
#include <pi/domain/context/SessionCtx.h>
#include <pi/domain/context/TurnStateCtx.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// AgentTurn 的 context 树: 继承若干 context 碎片.
// 与 AgentTurnService 配对, 由 AgentTurnImpl 合流.
// CompactCtx 同时是 CompactProc 事务的状态碎片 —— 同一 impl 上两条
// 事务类型共享 context 树 (任一时刻只有一条活跃).
// ---------------------------------------------------------------------------
struct AgentTurnContext : EventQueueCtx, EventSinkCtx, TurnStateCtx, SessionCtx,
                          ModelCtx, CompactCtx {
   AgentTurnContext(domain::Session& s, std::string prompt)
      : SessionCtx(s, std::move(prompt)) {}
};

} // namespace pi::domain
