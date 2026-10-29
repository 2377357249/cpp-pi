#pragma once

#include <string>

#include <trans-dsl/sched/domain/Unknown.h>

#include <pi/domain/AgentTurnService.h>
#include <pi/domain/context/AgentTurnContext.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// xxxImpl : xxxService, xxxContext —— DCI 合流点.
// IMPL_ROLE 补 context 槽位: impl 自己 IS-A context, 一把 IMPL_ROLE.
// 继承 TSL_NS::Unknown 使 TransactionInfo::toRole<>() 能投射.
// ---------------------------------------------------------------------------
class AgentTurnImpl : public TSL_NS::Unknown
                    , public AgentTurnService
                    , public AgentTurnContext {
public:
   AgentTurnImpl(domain::Session&       session,
                 domain::ILlmProvider&  llm,
                 domain::IToolExecutor& tools,
                 domain::ISessionStore& store,
                 std::string            systemPrompt)
      : AgentTurnService(llm, tools, store, session.id())
      , AgentTurnContext(session, std::move(systemPrompt)) {}

   IMPL_ROLE(EventQueueCtx)
   IMPL_ROLE(TurnStateCtx)
   IMPL_ROLE(SessionCtx)
};

} // namespace pi::domain
