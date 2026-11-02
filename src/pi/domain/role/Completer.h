#pragma once

#include <cub/dci/Role.h>

#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/EventSinkCtx.h>
#include <pi/domain/context/ModelCtx.h>
#include <pi/domain/context/SessionCtx.h>
#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/event/Events.h>
#include <pi/domain/llm/ILlmProvider.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// role: 请求 LLM completion 的业务.
// USE_ROLE 声明依赖的 context 碎片 —— 异步回调触发时手里没有 TransactionInfo,
// ROLE(...) 靠同一 impl 对象上的虚分发找回 context.
// 事件发射 (pi agent-loop): ≥2 轮先补 turn_start; 流 Start -> message_start
// {骨架}; 增量 -> message_update; settle -> message_end{最终}.
// ---------------------------------------------------------------------------
struct Completer {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(EventSinkCtx);
   USE_ROLE(TurnStateCtx);
   USE_ROLE(SessionCtx);
   USE_ROLE(ModelCtx);

   Completer(domain::ILlmProvider& llm, domain::IToolExecutor& tools)
      : llm_(llm), tools_(tools) {}

   // 发起 completion, 完成后经回调 post EV_LLM_DONE;
   // pi inner loop: 第 ≥2 个 assistant round 在流式前补发 turn_start.
   auto begin() -> void {
      auto& sess = ROLE(SessionCtx);
      auto& turn = ROLE(TurnStateCtx);
      auto& sink = *ROLE(EventSinkCtx).sink;
      if (turn.completionsSeen > 0) sink.turnStart();
      ++turn.completionsSeen;
      turn.streamingStarted = false;
      auto const& sel = ROLE(ModelCtx);
      llm_.complete(
         domain::CompletionRequest{sess.session.messages(),
                                   tools_.availableTools(),
                                   sess.systemPrompt,
                                   sel.activeModel,
                                   sel.thinkingLevel},
         [this](domain::AssistantStreamEvent const& e) { onStream(e); },
         [this](domain::CompletionResult r) {
            ROLE(TurnStateCtx).lastCompletion = std::move(r);
            ROLE(EventQueueCtx).post(EV_LLM_DONE, LlmDoneMsg{});
         });
   }

   // EV_LLM_DONE 到达后: assistant 消息落聚合 (持久化增量由 Session 记账)
   // (不 move: TurnDone 谓词还要读 lastCompletion.toolCalls)
   auto settle() -> void {
      auto& turn = ROLE(TurnStateCtx);
      auto& sink = *ROLE(EventSinkCtx).sink;
      auto msg = turn.lastCompletion.message;
      // pi: abort 到达时本次响应记 stopReason "aborted"
      msg.stopReason = turn.aborted() ? domain::StopReason::Aborted
                                    : turn.lastCompletion.stopReason;
      // pi !addedPartial: 没见过 start 时补发完整 message_start
      if (!turn.streamingStarted) sink.messageStart(msg);
      auto stored = ROLE(SessionCtx).session.appendAssistant(std::move(msg));
      sink.messageEnd(stored);
      turn.lastAssistant = std::move(stored);
   }

private:
   auto onStream(domain::AssistantStreamEvent const& e) -> void {
      auto& turn = ROLE(TurnStateCtx);
      auto& sink = *ROLE(EventSinkCtx).sink;
      switch (e.type) {
         case domain::AssistantStreamType::Start: {
            // pi message_start{partial}: 空内容骨架 + api/provider/model +
            // 零 usage + stopReason pending
            domain::Message skel{domain::Role::Assistant, {}, {}, {}};
            skel.api        = e.api;
            skel.provider   = e.provider;
            skel.model      = e.model;
            skel.stopReason = domain::StopReason::Pending;
            sink.messageStart(skel);
            turn.streamingStarted = true;
            break;
         }
         case domain::AssistantStreamType::TextStart:
         case domain::AssistantStreamType::TextDelta:
         case domain::AssistantStreamType::TextEnd:
         case domain::AssistantStreamType::ThinkingStart:
         case domain::AssistantStreamType::ThinkingDelta:
         case domain::AssistantStreamType::ThinkingEnd:
         case domain::AssistantStreamType::ToolCallStart:
         case domain::AssistantStreamType::ToolCallDelta:
         case domain::AssistantStreamType::ToolCallEnd:
            sink.messageUpdate(e.usage, e);
            break;
         default: break;   // Done/Error -> 不上 update 帧
      }
   }

   domain::ILlmProvider&  llm_;
   domain::IToolExecutor& tools_;
};

} // namespace pi::domain
