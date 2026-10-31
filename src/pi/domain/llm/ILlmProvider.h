#pragma once

#include <functional>

#include <pi/domain/llm/Completion.h>
#include <pi/domain/llm/StreamEvent.h>

namespace pi::domain {

using CompletionHandler = std::function<void(CompletionResult)>;

// ---------------------------------------------------------------------------
// 端口: LLM 提供者
// onEvent 流式回调按 pi AssistantMessageEvent 协议逐个递交增量;
// 最后必须恰调一次 done 交出结果 —— 可以同步返回, 也可以异步回调.
// 不感知事务/事件的存在; 无增量通道的实现可给 onEvent 发合成事件.
// ---------------------------------------------------------------------------
struct ILlmProvider {
   virtual ~ILlmProvider() = default;

   virtual void complete(CompletionRequest request,
                         AssistantStreamHandler onEvent,
                         CompletionHandler done) = 0;
};

} // namespace pi::domain
