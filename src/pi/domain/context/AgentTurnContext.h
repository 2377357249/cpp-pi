#pragma once

#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/EventSinkCtx.h>
#include <pi/domain/context/ModelCtx.h>
#include <pi/domain/context/SessionCtx.h>
#include <pi/domain/context/TurnStateCtx.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// AgentTurn 的 context 树: 继承若干 context 碎片.
// 与 AgentTurnService 配对, 由 AgentTurnImpl 合流.
// ---------------------------------------------------------------------------
struct AgentTurnContext : EventQueueCtx, EventSinkCtx, TurnStateCtx, SessionCtx,
                          ModelCtx {
   AgentTurnContext(domain::Session& s, std::string prompt)
      : SessionCtx(s, std::move(prompt)) {}
};

} // namespace pi::domain
