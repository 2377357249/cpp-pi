#pragma once

#include <chrono>
#include <optional>

#include <folly/json/json.h>

#include <pi/domain/llm/StopReason.h>
#include <pi/domain/message/Message.h>
#include <pi/domain/session/Session.h>

namespace pi::iface::rpc {

// ---------------------------------------------------------------------------
// domain <-> JSON 编解码 (folly::dynamic).
// 消息形状对齐 pi packages/ai/src/types.ts 的 Message 联合类型:
//   user       {role, content: string, timestamp}
//   assistant  {role, content: (text|toolCall)[], provider, model, stopReason,
//               usage, timestamp}
//   toolResult {role, toolCallId, toolName, content: text[], isError, timestamp}
// ---------------------------------------------------------------------------
inline auto nowMs() -> int64_t {
   return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline auto roleToString(domain::Role r) -> const char* {
   switch (r) {
      case domain::Role::User:      return "user";
      case domain::Role::Assistant: return "assistant";
      case domain::Role::Tool:      return "toolResult";
   }
   return "user";
}

// ToolCall.input 是 JSON 串, 尽量解析成内嵌对象; 解不动就原样放字符串.
inline auto inputToDynamic(std::string const& input) -> folly::dynamic {
   try {
      return folly::parseJson(input);
   } catch (...) {
      return folly::dynamic{input};
   }
}

inline auto messageToJson(domain::Message const& m) -> folly::dynamic {
   auto ts = nowMs();

   if (m.role == domain::Role::User) {
      return folly::dynamic::object("role", "user")("content", m.text)
                                   ("timestamp", ts);
   }

   if (m.role == domain::Role::Tool) {
      auto content = folly::dynamic::array(
         folly::dynamic::object("type", "text")("text", m.text));
      return folly::dynamic::object("role", "toolResult")
                                   ("toolCallId", m.toolCallId)
                                   ("toolName", m.toolName)
                                   ("content", std::move(content))
                                   ("isError", m.isError)
                                   ("timestamp", ts);
   }

   // assistant
   auto content = folly::dynamic::array();
   if (!m.text.empty()) {
      content.push_back(folly::dynamic::object("type", "text")("text", m.text));
   }
   for (auto const& tc : m.toolCalls) {
      content.push_back(folly::dynamic::object("type", "toolCall")
                        ("id", tc.id)("name", tc.tool)
                        ("arguments", inputToDynamic(tc.input)));
   }
   auto usage = folly::dynamic::object
      ("input", 0)("output", 0)("cacheRead", 0)("cacheWrite", 0)
      ("totalTokens", 0)("cost", folly::dynamic::object
         ("input", 0)("output", 0)("cacheRead", 0)("cacheWrite", 0)("total", 0));
   return folly::dynamic::object
      ("role", "assistant")
      ("content", std::move(content))
      ("api", "mock")("provider", "mock")("model", "mock-1")
      ("usage", std::move(usage))
      ("stopReason", domain::toWire(domain::stopReasonOf(m)))
      ("timestamp", ts);
}

inline auto messagesToJson(domain::Session const& s) -> folly::dynamic {
   auto arr = folly::dynamic::array();
   for (auto const& m : s.messages()) arr.push_back(messageToJson(m));
   return arr;
}

} // namespace pi::iface::rpc
