#pragma once

#include <string>

#include <event/impl/ConsecutiveEventInfo.h>
#include <event/concept/Event.h>
#include <trans-dsl/trans-dsl.h>

#include <pi/interface/trans/AgentTurn.h>
#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/session/ISessionStore.h>

namespace pi::iface {

// ---------------------------------------------------------------------------
// 应用服务: 持有 AgentTurnImpl(DCI 合流对象), 驱动 AgentTurn 事务 ——
// start() 启动, 不断把事件队列里的协议事件喂给事务直到脱离 working 状态.
// (RPC 模式下换成 SessionLoop 常驻事务 + 异步喂事件; CLI 下按轮驱动)
// ---------------------------------------------------------------------------
class AgentSessionService {
public:
   AgentSessionService(domain::Session&       session,
                       domain::ILlmProvider&  llm,
                       domain::IToolExecutor& tools,
                       domain::ISessionStore& store,
                       std::string            systemPrompt)
      : impl_{session, llm, tools, store, std::move(systemPrompt)} {}

   auto runTurn(std::string userInput) -> TSL_NS::Status {
      impl_.session.appendUser(std::move(userInput));
      impl_.session.beginTurn();

      MonitoredAgentTurn turn{};
      turn.updateUserContext(&impl_);

      auto status = turn.start();
      while (TSL_NS::is_working_status(status) && !impl_.queue.empty()) {
         auto& front = impl_.queue.front();
         EV_NS::ConsecutiveEventInfo info{front.id, front.bytes.data(), front.size};
         EV_NS::Event event{info};
         status = turn.handleEvent(event);
         impl_.queue.pop_front();
      }
      return status;
   }

   auto impl() -> domain::AgentTurnImpl& { return impl_; }

private:
   domain::AgentTurnImpl impl_;
};

} // namespace pi::iface
