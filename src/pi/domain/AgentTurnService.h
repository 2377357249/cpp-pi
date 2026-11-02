#pragma once

#include <string>

#include <pi/domain/role/Completer.h>
#include <pi/domain/role/Persister.h>
#include <pi/domain/role/ToolRunner.h>
#include <pi/domain/role/TurnReporter.h>
#include <pi/domain/session/ISessionStore.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// xxxService: role 树 —— 只绑 role, 不碰 context.
// 同一套 role 捆扎可配不同 context 树 (如测试用 TestContext).
// ---------------------------------------------------------------------------
struct AgentTurnService : Completer, ToolRunner, TurnReporter, Persister {
   AgentTurnService(domain::ILlmProvider&  llm,
                    domain::IToolExecutor& tools,
                    domain::ISessionStore& store,
                    std::string            sessionId)
      : Completer(llm, tools)
      , ToolRunner(tools)
      , Persister(store, std::move(sessionId)) {}
};

} // namespace pi::domain
