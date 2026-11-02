#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <pi/domain/llm/StreamEvent.h>
#include <pi/domain/message/Message.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 端口: agent 事件汇 (pi agent-loop.ts 的 emit() 回调).
// 实现方把每个生命周期事件转成 wire 帧; 事件在产生点同步调用 (worker 线程),
// 实现内部只允许持有自己的序列化锁, 不得再取会话/队列锁.
// 事件序 (pi runAgentLoop):
//   agent_start → turn_start → 初始消息 message_start/message_end →
//   每轮: [turn_start(≥2)] → assistant message_start → message_update* →
//   message_end → 每个工具 tool_execution_start/end + toolResult
//   message_start/end → turn_end → ... → agent_end
// ---------------------------------------------------------------------------
struct IAgentEventSink {
   virtual ~IAgentEventSink() = default;

   virtual auto agentStart() -> void = 0;
   virtual auto turnStart() -> void = 0;
   virtual auto messageStart(Message const& m) -> void = 0;
   virtual auto messageUpdate(Usage const& usage,
                              AssistantStreamEvent const& e) -> void = 0;
   virtual auto messageEnd(Message const& m) -> void = 0;
   // argsJson = ToolCall.input 原文 (JSON 串或非 JSON 裸串, 由实现方解析)
   virtual auto toolExecutionStart(std::string_view toolCallId,
                                   std::string_view toolName,
                                   std::string_view argsJson) -> void = 0;
   virtual auto toolExecutionEnd(std::string_view toolCallId,
                                 std::string_view toolName,
                                 Message const& toolResultMsg) -> void = 0;
   virtual auto turnEnd(Message const& assistant,
                        std::vector<Message> const& toolResults) -> void = 0;
   virtual auto agentEnd(std::vector<Message> const& messages,
                         bool willRetry) -> void = 0;

   // pi auto_retry 事件 (非纯虚 —— 不关心的 sink 可留默认空实现):
   // auto_retry_start{attempt,maxAttempts,delayMs,errorMessage}
   // auto_retry_end{success,attempt,finalError?}
   virtual auto autoRetryStart(size_t attempt, size_t maxAttempts,
                               int64_t delayMs,
                               std::string_view errorMessage) -> void {
      (void)attempt; (void)maxAttempts; (void)delayMs; (void)errorMessage;
   }
   virtual auto autoRetryEnd(bool success, size_t attempt,
                             std::string_view finalError) -> void {
      (void)success; (void)attempt; (void)finalError;
   }
};

// 无 sink 环境 (CLI/测试) 的默认实现: 全部 no-op.
struct NullAgentEventSink : IAgentEventSink {
   auto agentStart() -> void override {}
   auto turnStart() -> void override {}
   auto messageStart(Message const&) -> void override {}
   auto messageUpdate(Usage const&, AssistantStreamEvent const&)
      -> void override {}
   auto messageEnd(Message const&) -> void override {}
   auto toolExecutionStart(std::string_view, std::string_view,
                           std::string_view) -> void override {}
   auto toolExecutionEnd(std::string_view, std::string_view,
                         Message const&) -> void override {}
   auto turnEnd(Message const&, std::vector<Message> const&)
      -> void override {}
   auto agentEnd(std::vector<Message> const&, bool) -> void override {}
};

} // namespace pi::domain
