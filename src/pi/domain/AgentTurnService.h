#pragma once

#include <string>

#include <pi/domain/role/Compactor.h>
#include <pi/domain/role/Completer.h>
#include <pi/domain/role/Persister.h>
#include <pi/domain/role/Retryer.h>
#include <pi/domain/role/ToolRunner.h>
#include <pi/domain/role/TurnReporter.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/async/ThreadPool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// xxxService: role 树 —— 只绑 role, 不碰 context.
// 同一套 role 捆扎可配不同 context 树 (如测试用 TestContext).
//   ioPool: Retryer backoff / 重阻塞原语的收容线程池 (infra 层设施).
// ---------------------------------------------------------------------------
struct AgentTurnService : Completer, ToolRunner, TurnReporter, Persister,
                          Compactor, Retryer {
   AgentTurnService(domain::ILlmProvider&  llm,
                    domain::IToolExecutor& tools,
                    domain::ISessionStore& store,
                    std::string            sessionId,
                    infra::ThreadPool&     ioPool,
                    infra::EventLoop&      loop)
      : Completer(llm, tools)
      , ToolRunner(tools)
      , Persister(store, std::move(sessionId))
      , Compactor(llm)
      , Retryer(ioPool, loop) {}
};

} // namespace pi::domain
