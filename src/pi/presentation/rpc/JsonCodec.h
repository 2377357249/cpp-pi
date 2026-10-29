#pragma once

#include <folly/json/json.h>

#include <pi/domain/message/Message.h>
#include <pi/domain/session/Session.h>

namespace pi::rpc {

// ---------------------------------------------------------------------------
// domain <-> JSON 编解码 (folly::dynamic). 字段名对齐 pi rpc-types.ts.
// ---------------------------------------------------------------------------
inline auto roleToString(domain::Role r) -> const char* {
   switch (r) {
      case domain::Role::User:      return "user";
      case domain::Role::Assistant: return "assistant";
      case domain::Role::Tool:      return "tool";
   }
   return "user";
}

inline auto messageToJson(domain::Message const& m) -> folly::dynamic {
   auto content = folly::dynamic::array();
   if (!m.text.empty()) {
      content.push_back(folly::dynamic::object("type", "text")("text", m.text));
   }
   for (auto const& tc : m.toolCalls) {
      content.push_back(folly::dynamic::object("type", "toolCall")
                        ("id", tc.id)("name", tc.tool)("arguments", tc.input));
   }
   folly::dynamic obj = folly::dynamic::object("role", roleToString(m.role))
                                             ("content", std::move(content));
   if (m.role == domain::Role::Tool) {
      obj["toolCallId"] = m.toolCallId;
   }
   return obj;
}

inline auto messagesToJson(domain::Session const& s) -> folly::dynamic {
   auto arr = folly::dynamic::array();
   for (auto const& m : s.messages()) arr.push_back(messageToJson(m));
   return arr;
}

} // namespace pi::rpc
