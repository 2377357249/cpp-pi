#pragma once

#include <cub/dci/Role.h>

#include <pi/application/context/EventQueueCtx.h>
#include <pi/application/context/SessionCtx.h>
#include <pi/application/context/TurnStateCtx.h>
#include <pi/application/Events.h>
#include <pi/domain/llm/ILlmProvider.h>
#include <pi/domain/tool/Tool.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// role: 请求 LLM completion 的业务.
// USE_ROLE 声明依赖的 context 碎片 —— 异步回调触发时手里没有 TransactionInfo,
// ROLE(...) 靠同一 impl 对象上的虚分发找回 context.
// ---------------------------------------------------------------------------
struct Completer {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(TurnStateCtx);
   USE_ROLE(SessionCtx);

   Completer(domain::ILlmProvider& llm, domain::IToolExecutor& tools)
      : llm_(llm), tools_(tools) {}

   // 发起 completion, 完成后经回调 post EV_LLM_DONE
   auto begin() -> void {
      auto& sess = ROLE(SessionCtx);
      llm_.complete(
         domain::CompletionRequest{sess.session.messages(),
                                   tools_.availableTools(),
                                   sess.systemPrompt},
         [this](domain::CompletionResult r) {
            ROLE(TurnStateCtx).lastCompletion = std::move(r);
            ROLE(EventQueueCtx).post(EV_LLM_DONE, LlmDoneMsg{});
         });
   }

   // EV_LLM_DONE 到达后: assistant 消息落聚合 + 记持久化增量
   // (不 move: TurnDone 谓词还要读 lastCompletion.toolCalls)
   auto settle() -> void {
      auto& turn = ROLE(TurnStateCtx);
      turn.deltaMessages.push_back(turn.lastCompletion.message);
      ROLE(SessionCtx).session.appendAssistant(turn.lastCompletion.message);
   }

private:
   domain::ILlmProvider&  llm_;
   domain::IToolExecutor& tools_;
};

} // namespace pi::app
