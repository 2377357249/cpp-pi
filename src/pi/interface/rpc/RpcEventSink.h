#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/event/IAgentEventSink.h>
#include <pi/interface/rpc/JsonCodec.h>

namespace pi::iface::rpc {

// ---------------------------------------------------------------------------
// IAgentEventSink 的 RPC 实现: domain role 在事件发生点同步回调,
// 每个生命周期事件在这里序列化成 pi AgentSessionEvent 线帧交给 emit 出口.
// emit 由 RpcFrontend 注入 (内部带输出串行锁, worker 线程直调安全).
// ---------------------------------------------------------------------------
class RpcEventSink : public domain::IAgentEventSink {
public:
   using Emit = std::function<void(folly::dynamic)>;

   explicit RpcEventSink(Emit emit) : emit_(std::move(emit)) {}

   auto agentStart() -> void override {
      emit_(folly::dynamic::object("type", "agent_start"));
   }

   auto turnStart() -> void override {
      emit_(folly::dynamic::object("type", "turn_start"));
   }

   auto messageStart(domain::Message const& m) -> void override {
      emit_(folly::dynamic::object("type", "message_start")
                                   ("message", messageToJson(m)));
   }

   // pi message_update{usage, assistantMessageEvent}; 事件本体在
   // assistantStreamEventJson (start/done/error 返回 null -> 不上帧).
   auto messageUpdate(domain::Usage const&              usage,
                      domain::AssistantStreamEvent const& e)
      -> void override {
      auto ev = assistantStreamEventJson(e);
      if (ev.isNull()) return;
      emit_(folly::dynamic::object("type", "message_update")
                ("usage", usageToJson(usage))
                ("assistantMessageEvent", std::move(ev)));
   }

   auto messageEnd(domain::Message const& m) -> void override {
      emit_(folly::dynamic::object("type", "message_end")
                                   ("message", messageToJson(m)));
   }

   // argsJson = ToolCall.input 原文; 非 JSON 裸串按字符串 args 上线.
   auto toolExecutionStart(std::string_view toolCallId,
                           std::string_view toolName,
                           std::string_view argsJson) -> void override {
      emit_(folly::dynamic::object("type", "tool_execution_start")
               ("toolCallId", std::string{toolCallId})
               ("toolName", std::string{toolName})
               ("args", inputToDynamic(std::string{argsJson})));
   }

   auto toolExecutionEnd(std::string_view toolCallId,
                         std::string_view toolName,
                         domain::Message const& toolResultMsg)
      -> void override {
      folly::dynamic f = folly::dynamic::object
         ("type", "tool_execution_end")
         ("toolCallId", std::string{toolCallId})
         ("toolName", std::string{toolName})
         ("result", toolResultPayloadJson(toolResultMsg))
         ("isError", toolResultMsg.isError);
      if (toolResultMsg.durationMs >= 0)
         f["durationMs"] = toolResultMsg.durationMs;
      emit_(std::move(f));
   }

   auto turnEnd(domain::Message const&              assistant,
                std::vector<domain::Message> const& toolResults)
      -> void override {
      auto trs = folly::dynamic::array();
      for (auto const& m : toolResults) trs.push_back(messageToJson(m));
      emit_(folly::dynamic::object("type", "turn_end")
               ("message", messageToJson(assistant))
               ("toolResults", std::move(trs)));
   }

   auto agentEnd(std::vector<domain::Message> const& messages,
                 bool willRetry) -> void override {
      auto arr = folly::dynamic::array();
      for (auto const& m : messages) arr.push_back(messageToJson(m));
      emit_(folly::dynamic::object("type", "agent_end")
               ("messages", std::move(arr))("willRetry", willRetry));
   }

   // pi auto_retry_start{attempt,maxAttempts,delayMs,errorMessage}
   auto autoRetryStart(size_t attempt, size_t maxAttempts,
                       int64_t delayMs,
                       std::string_view errorMessage) -> void override {
      emit_(folly::dynamic::object("type", "auto_retry_start")
               ("attempt", static_cast<int64_t>(attempt))
               ("maxAttempts", static_cast<int64_t>(maxAttempts))
               ("delayMs", delayMs)
               ("errorMessage", std::string{errorMessage}));
   }

   // pi auto_retry_end{success,attempt,finalError?}
   auto autoRetryEnd(bool success, size_t attempt,
                     std::string_view finalError) -> void override {
      auto f = folly::dynamic::object("type", "auto_retry_end")
                  ("success", success)
                  ("attempt", static_cast<int64_t>(attempt));
      if (!finalError.empty())
         f("finalError", std::string{finalError});
      emit_(std::move(f));
   }

private:
   Emit emit_;
};

} // namespace pi::iface::rpc
