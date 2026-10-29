#pragma once

#include <functional>

#include <pi/domain/llm/Completion.h>

namespace pi::domain {

using CompletionHandler = std::function<void(CompletionResult)>;

// ---------------------------------------------------------------------------
// 端口: LLM 提供者
// 实现方只需"最终调一次 done 交出结果"——可以同步返回, 也可以异步回调.
// 不感知事务/事件的存在.
// ---------------------------------------------------------------------------
struct ILlmProvider {
   virtual ~ILlmProvider() = default;

   virtual void complete(CompletionRequest request, CompletionHandler done) = 0;
};

} // namespace pi::domain
