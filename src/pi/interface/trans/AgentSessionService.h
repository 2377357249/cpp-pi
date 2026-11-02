#pragma once

#include <atomic>
#include <format>
#include <functional>
#include <optional>
#include <string>

#include <event/impl/ConsecutiveEventInfo.h>
#include <event/concept/Event.h>
#include <folly/json/json.h>
#include <trans-dsl/trans-dsl.h>

#include <pi/interface/trans/AgentTurn.h>
#include <pi/interface/trans/CompactProc.h>
#include <pi/domain/AgentTurnImpl.h>
#include <pi/domain/compaction/CompactionPolicy.h>
#include <pi/domain/model/ModelCatalog.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/async/ThreadPool.h>
#include <pi/infra/messaging/PubSub.h>

namespace pi::iface {

// ---------------------------------------------------------------------------
// 应用服务: 持有 AgentTurnImpl(DCI 合流对象), 在 EventLoop owner 线程上
// 驱动两条事务类型 —— AgentTurn (run) 与 CompactProc (压缩), 任一时刻
// 只有一条活跃, 事件经同一 EventQueueCtx 邮箱按活跃事务分发.
//   startTurn/startCompact 启动后立即返回 —— 事务在 WAIT_ON 处挂起,
//   异步源 (LLM/工具/存储/backoff) 的回调经适配器 marshal 回 loop 线程
//   post 事件, EventQueueCtx::onPost 钩子安排 deferred pump 恢复事务.
//   领域态只在 loop 线程被触碰; pump 用 pumping_ 防重入.
//   abort 旗标由本服务自持 (impl_.abortRequested 指向它): 前端调
//   requestAbort, 事务谓词/工具派发/backoff/bash spawn 周期检查.
// ---------------------------------------------------------------------------
class AgentSessionService {
public:
   using RunDoneHandler = std::function<void(TSL_NS::Status)>;

   AgentSessionService(domain::Session&       session,
                       domain::ILlmProvider&  turnLlm,
                       domain::IToolExecutor& tools,
                       domain::ISessionStore& store,
                       std::string            systemPrompt,
                       infra::EventLoop&      loop,
                       infra::ThreadPool&     ioPool,
                       infra::IPubSub&        internalBus)
      : impl_{session, turnLlm, tools, store, std::move(systemPrompt),
              ioPool, loop}
      , loop_{loop}
      , internalBus_{internalBus} {
      impl_.abortRequested = &abortRequested_;
      impl_.onPost = [this] { loop_.post([this] { pump(); }); };
   }

   // ---------- abort (pi agent.abort) ----------
   // 旗标自持: 事务谓词 (TurnDone/ToolRunner/Completer)、bash spawn 的
   // aborted 钩子、provider abortCheck、Retryer backoff 都读同一个原子量.
   auto requestAbort() -> void { abortRequested_ = true; }
   auto aborted() const -> bool { return abortRequested_.load(); }

   // abort_retry: 只杀在飞的 backoff sleep, 不动 run 本身.
   auto cancelRetry() -> void { impl_.Retryer::cancel(); }

   // set_auto_retry: 重试链开关 (pi settings.retry.enabled)
   auto setAutoRetry(bool v) -> void { impl_.autoRetryEnabled = v; }

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

   // ---------- 事务驱动 ----------
   auto running() const -> bool { return turn_.has_value() || compactTx_; }
   auto compacting() const -> bool { return compactTx_.has_value(); }

   // pi runAgentLoop: agent_start → turn_start → 输入消息 message_* →
   // 事务化 inner loop (每轮 turn_* 边界由 Completer/TurnReporter 发射) →
   // agent_end{messages: 本轮全部新消息}. 立即返回; 收尾经 onDone.
   auto startTurn(std::string   userInput,
                  RunDoneHandler onDone) -> void {
      // 一轮新 run 起点: abort/retry 链状态复位 (旗标只影响在飞的 run)
      abortRequested_       = false;
      impl_.retryAttempt    = 0;
      impl_.retryInFlight   = false;
      impl_.retryCancelled  = false;

      // unpersisted() 天然覆盖轮次间隙的命令 entry (sessionInfo/thinkingLevel)
      // 和本轮 user 消息, 无需 turn 起点标界.
      auto& sink = *impl_.sink;
      baseIdx_   = impl_.session.messages().size();
      auto userMsg = impl_.session.appendUser(std::move(userInput));

      sink.agentStart();
      sink.turnStart();
      sink.messageStart(userMsg);
      sink.messageEnd(userMsg);
      internalBus_.publish({
         "agent.turn.started",
         folly::toJson(folly::dynamic::object("sessionId", impl_.session.id()))});

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

   // pi agent-session.compact: prep 由调用方在 loop 线程算好 (同步纯函数),
   // 摘要/落盘走 CompactProc 事务协程式挂起; 完成经 onDone 回 status.
   auto startCompact(domain::CompactionPreparation prep,
                     std::string reason, std::string instructions,
                     bool willRetry,
                     RunDoneHandler done) -> void {
      // compact 是 preempt 命令: 它置的 abort 旗标本是杀前一个 run 的,
      // 事务起点必须复位 —— 否则 settleSummarize 首跳即 "cancelled".
      // 压缩中途到达的 abort 会重新置位, 仍可取消.
      abortRequested_ = false;
      domain::CompactCtx& cx = impl_;   // impl IS-A CompactCtx
      cx.prep         = std::move(prep);
      cx.reason       = std::move(reason);
      cx.instructions = std::move(instructions);
      cx.willRetry    = willRetry;
      // phase 对齐首个要跑的 Summarize: 无历史可摘要时第一跳直接 Prefix,
      // 否则第二个 __optional 会拿 History prompt 去摘要 prefix 消息.
      cx.phase        = cx.prep->messagesToSummarize.empty()
                           ? domain::CompactCtx::Phase::Prefix
                           : domain::CompactCtx::Phase::History;
      cx.lastResult.reset();
      cx.historySummary.clear(); cx.prefixSummary.clear();
      cx.historyUsage = {};      cx.prefixUsage = {};
      cx.hasHistory = false;     cx.hasPrefix = false;
      cx.error.clear();          cx.aborted = false;
      cx.summary.clear();        cx.usage = {};
      cx.detailsJson.clear();

      compactDone_ = std::move(done);
      compactTx_.emplace();
      compactTx_->updateUserContext(&impl_);
      status_ = compactTx_->start();
      pump();
   }

   // 同步版 (CLI 主循环): 内嵌跑 loop 直到本轮 run 收尾.
   auto runTurn(std::string userInput) -> TSL_NS::Status {
      TSL_NS::Status st = TSL_NS::Result::SUCCESS;
      startTurn(std::move(userInput),
                [&st](TSL_NS::Status s) { st = s; });
      loop_.runUntil([this] { return !running(); });
      return st;
   }

   auto impl() -> domain::AgentTurnImpl& { return impl_; }

private:
   // 泵送事务: 队列事件逐个喂活跃事务的 handleEvent 直到 working 队列空.
   auto pump() -> void {
      if (pumping_) return;
      pumping_ = true;
      while ((turn_ || compactTx_) && TSL_NS::is_working_status(status_)
             && !impl_.queue.empty()) {
         auto& front = impl_.queue.front();
         EV_NS::ConsecutiveEventInfo info{front.id, front.bytes.data(),
                                        front.size};
         EV_NS::Event event{info};
         status_ = turn_ ? turn_->handleEvent(event)
                         : compactTx_->handleEvent(event);
         impl_.queue.pop_front();
      }
      pumping_ = false;
      if (turn_ && !TSL_NS::is_working_status(status_)) finish();
      else if (compactTx_ && !TSL_NS::is_working_status(status_))
         finishCompact();
   }

   auto finish() -> void {
      auto const msgs = impl_.session.messages();
      impl_.sink->agentEnd(
         std::vector<domain::Message>{msgs.begin() + baseIdx_, msgs.end()},
         /*willRetry=*/false);
      internalBus_.publish({
         "agent.turn.finished",
         folly::toJson(folly::dynamic::object("sessionId", impl_.session.id())
                                       ("status", static_cast<int64_t>(status_)))});
      auto st   = status_;
      auto done = std::move(onDone_);
      onDone_   = nullptr;
      turn_.reset();          // 先于 onDone 复位 —— running() 在回调内已假
      if (done) done(st);
   }

   auto finishCompact() -> void {
      auto st   = status_;
      auto done = std::move(compactDone_);
      compactDone_ = nullptr;
      compactTx_.reset();     // compacting() 在回调内已假
      if (done) done(st);
   }

   domain::AgentTurnImpl  impl_;
   infra::EventLoop&      loop_;
   infra::IPubSub&        internalBus_;
   domain::ModelCatalog   catalog_;
   std::function<bool(std::string_view)> providerChange_;

   std::atomic<bool>     abortRequested_{false};

   std::optional<MonitoredAgentTurn> turn_;
   std::optional<CompactProc>        compactTx_;
   TSL_NS::Status        status_  = TSL_NS::Result::SUCCESS;
   RunDoneHandler        onDone_;
   RunDoneHandler        compactDone_;
   size_t                baseIdx_ = 0;
   bool                  pumping_ = false;
};

} // namespace pi::iface
