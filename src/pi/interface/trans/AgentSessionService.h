#pragma once

#include <atomic>
#include <format>
#include <functional>
#include <optional>
#include <string>

#include <event/impl/ConsecutiveEventInfo.h>
#include <event/concept/Event.h>
#include <trans-dsl/trans-dsl.h>

#include <pi/interface/trans/AgentTurn.h>
#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/model/ModelCatalog.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/infra/async/EventLoop.h>

namespace pi::iface {

// ---------------------------------------------------------------------------
// 应用服务: 持有 AgentTurnImpl(DCI 合流对象), 在 EventLoop owner 线程上
// 驱动 AgentTurn 事务.
//   startTurn 启动一轮 run 后立即返回 —— 事务在 WAIT_ON 处挂起, 异步
//   源 (LLM/工具/存储) 的回调经适配器 marshal 回 loop 线程 post 事件,
//   EventQueueCtx::onPost 钩子安排 deferred pump 恢复事务. 领域态只在
//   loop 线程被触碰; pump 用 pumping_ 防重入 (post 可能发生在
//   handleEvent 调用栈内).
// ---------------------------------------------------------------------------
class AgentSessionService {
public:
   using RunDoneHandler = std::function<void(TSL_NS::Status)>;

   AgentSessionService(domain::Session&       session,
                       domain::ILlmProvider&  turnLlm,
                       domain::ILlmProvider&  oneShotLlm,
                       domain::IToolExecutor& tools,
                       domain::ISessionStore& store,
                       std::string            systemPrompt,
                       infra::EventLoop&      loop)
      : oneShotLlm_{oneShotLlm}
      , impl_{session, turnLlm, tools, store, std::move(systemPrompt)}
      , loop_{loop} {
      impl_.onPost = [this] { loop_.post([this] { pump(); }); };
   }

   // 事务外一次性 completion (compaction 摘要等). provider 契约: 最终调一次
   // done; 走同步 oneShotLlm_ —— 阻塞 loop 线程, 协程化留待 Compact proc.
   // 摘要路径的增量事件不上线 (pi 的摘要流不经 agent event 转发).
   auto completeOnce(domain::CompletionRequest req) -> domain::CompletionResult {
      std::optional<domain::CompletionResult> out;
      oneShotLlm_.complete(std::move(req),
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

   // RPC 前端注入事件汇; 未注入时 impl_.sink 是 Null (CLI/测试).
   auto setEventSink(domain::IAgentEventSink& sink) -> void {
      impl_.sink = &sink;
   }

   // ---------- 模型选择 (pi set_model / set_thinking_level 的服务层语义) ----
   // 目录由启动时装配 (静态内置 + models.json); setModel 校验目录后改写
   // ModelCtx —— 下一次 completion 生效; onModelChange 钩子由 main.cpp
   // 接 LlmProviderMux::use 完成跨 provider 切换.
   auto setCatalog(domain::ModelCatalog c) -> void {
      catalog_ = std::move(c);
   }
   auto catalog() const -> domain::ModelCatalog const& { return catalog_; }
   auto model() const -> domain::Model const& { return impl_.activeModel; }
   auto thinkingLevel() const -> std::string const& {
      return impl_.thinkingLevel;
   }

   // 返回错误串; nullopt = 切换成功. provider/modelId 必须命中目录.
   auto setModel(std::string_view provider, std::string_view modelId)
      -> std::optional<std::string> {
      auto const* m = catalog_.find(provider, modelId);
      if (m == nullptr)
         return std::format("Model not found: {}/{}", provider, modelId);
      if (providerChange_ && m->provider != impl_.activeModel.provider
          && !providerChange_(m->provider))
         return std::format("No provider available for: {}", m->provider);
      impl_.activeModel = *m;
      return std::nullopt;
   }

   // 裸 term 版 (TUI /model <x>): "provider/id" 或唯一裸 id.
   auto setModel(std::string_view term) -> std::optional<std::string> {
      auto const* m = catalog_.find(term);
      if (m == nullptr)
         return std::format("Model not found: {}", term);
      return setModel(m->provider, m->id);
   }

   // pi cycleModel: 在全目录顺序上循环 (跨 provider); dir=±1.
   // ≤1 候选或 provider 无实现 -> nullptr.
   auto cycleModel(int dir = 1) -> domain::Model const* {
      auto const& models = catalog_.models;
      if (models.size() <= 1) return nullptr;
      size_t cur = 0;
      for (size_t i = 0; i < models.size(); ++i)
         if (models[i].provider == impl_.activeModel.provider
             && models[i].id == impl_.activeModel.id) { cur = i; break; }
      auto const n    = static_cast<int>(models.size());
      auto const next = (static_cast<int>(cur) + dir + n) % n;
      auto const* nxt = &models[static_cast<size_t>(next)];
      if (setModel(nxt->provider, nxt->id)) return nullptr;
      return &impl_.activeModel;
   }

   auto setThinkingLevel(std::string_view level) -> void {
      impl_.thinkingLevel = std::string{level};
   }

   // provider 切换钩子 (main.cpp 接 LlmProviderMux::use); 返回 false =
   // 该 provider 无可用实现.
   auto setProviderChangeHandler(
      std::function<bool(std::string_view providerId)> fn) -> void {
      providerChange_ = std::move(fn);
   }

   auto running() const -> bool { return turn_.has_value(); }

   // pi runAgentLoop: agent_start → turn_start → 输入消息 message_* →
   // 事务化 inner loop (每轮 turn_* 边界由 Completer/TurnReporter 发射) →
   // agent_end{messages: 本轮全部新消息}. 立即返回; 收尾经 onDone.
   auto startTurn(std::string   userInput,
                  RunDoneHandler onDone) -> void {
      // unpersisted() 天然覆盖轮次间隙的命令 entry (sessionInfo/thinkingLevel)
      // 和本轮 user 消息, 无需 turn 起点标界.
      auto& sink = *impl_.sink;
      baseIdx_   = impl_.session.messages().size();
      auto userMsg = impl_.session.appendUser(std::move(userInput));

      sink.agentStart();
      sink.turnStart();
      sink.messageStart(userMsg);
      sink.messageEnd(userMsg);

      // run 作用域的发射态复位 (一个 run 可含多个 assistant round)
      impl_.lastAssistant.reset();
      impl_.roundToolResults.clear();
      impl_.streamingStarted = false;
      impl_.completionsSeen = 0;

      onDone_ = std::move(onDone);
      turn_.emplace();
      turn_->updateUserContext(&impl_);
      status_ = turn_->start();
      pump();
   }

   // 同步版 (CLI/TUI worker 线程): 内嵌跑 loop 直到本轮 run 收尾.
   auto runTurn(std::string userInput) -> TSL_NS::Status {
      TSL_NS::Status st = TSL_NS::Result::SUCCESS;
      startTurn(std::move(userInput),
                [&st](TSL_NS::Status s) { st = s; });
      loop_.runUntil([this] { return !running(); });
      return st;
   }

   auto impl() -> domain::AgentTurnImpl& { return impl_; }

private:
   // 泵送事务: 队列事件逐个喂 handleEvent 直到 working 队列空.
   // 事务脱离 working 即 finish —— onDone 可能链式启动下一轮.
   auto pump() -> void {
      if (pumping_) return;
      pumping_ = true;
      while (turn_ && TSL_NS::is_working_status(status_)
             && !impl_.queue.empty()) {
         auto& front = impl_.queue.front();
         EV_NS::ConsecutiveEventInfo info{front.id, front.bytes.data(),
                                        front.size};
         EV_NS::Event event{info};
         status_ = turn_->handleEvent(event);
         impl_.queue.pop_front();
      }
      pumping_ = false;
      if (turn_ && !TSL_NS::is_working_status(status_)) finish();
   }

   auto finish() -> void {
      auto const msgs = impl_.session.messages();
      impl_.sink->agentEnd(
         std::vector<domain::Message>{msgs.begin() + baseIdx_, msgs.end()},
         /*willRetry=*/false);
      auto st   = status_;
      auto done = std::move(onDone_);
      onDone_   = nullptr;
      turn_.reset();          // 先于 onDone 复位 —— running() 在回调内已假
      if (done) done(st);
   }

   domain::ILlmProvider&  oneShotLlm_;
   domain::AgentTurnImpl  impl_;
   infra::EventLoop&      loop_;
   domain::ModelCatalog   catalog_;
   std::function<bool(std::string_view)> providerChange_;

   std::optional<MonitoredAgentTurn> turn_;
   TSL_NS::Status        status_  = TSL_NS::Result::SUCCESS;
   RunDoneHandler        onDone_;
   size_t                baseIdx_ = 0;
   bool                  pumping_ = false;
};

} // namespace pi::iface
