#pragma once

#include <atomic>
#include <optional>
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
      : llm_{llm}
      , impl_{session, llm, tools, store, std::move(systemPrompt)} {}

   // 事务外一次性 completion (compaction 摘要等). provider 契约: 最终调一次
   // done; 当前实现同步完成, 异步实现留待 EV 链路接入.
   // 摘要路径的增量事件不上线 (pi 的摘要流不经 agent event 转发).
   auto completeOnce(domain::CompletionRequest req) -> domain::CompletionResult {
      std::optional<domain::CompletionResult> out;
      llm_.complete(std::move(req),
                    [](domain::AssistantStreamEvent const&) {},
                    [&out](domain::CompletionResult r) { out = std::move(r); });
      return out.value_or(domain::CompletionResult{
         domain::Message{domain::Role::Assistant, {}, {}, {}},
         domain::StopReason::Error});
   }

   // pi agent.abort(): 注入外部中止旗标 (RPC 前端持有的 atomic).
   auto setAbortFlag(std::atomic<bool>& flag) -> void {
      impl_.abortRequested = &flag;
   }

   auto runTurn(std::string userInput) -> TSL_NS::Status {
      // unpersisted() 天然覆盖轮次间隙的命令 entry (sessionInfo/thinkingLevel)
      // 和本轮 user 消息, 无需 turn 起点标界.
      impl_.session.appendUser(std::move(userInput));

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
   domain::ILlmProvider& llm_;
   domain::AgentTurnImpl impl_;
};

} // namespace pi::iface
