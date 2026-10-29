#pragma once

#include <pi/application/context/EventQueueCtx.h>
#include <pi/application/context/SessionCtx.h>
#include <pi/application/context/TurnStateCtx.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// AgentTurn 的 context 树: 继承若干 context 碎片.
// 与 AgentTurnService 配对, 由 AgentTurnImpl 合流.
// ---------------------------------------------------------------------------
struct AgentTurnContext : EventQueueCtx, TurnStateCtx, SessionCtx {
   AgentTurnContext(domain::Session& s, std::string prompt)
      : SessionCtx(s, std::move(prompt)) {}
};

} // namespace pi::app
