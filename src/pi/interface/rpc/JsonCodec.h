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
//   system     {role, content, timestamp}            (会话内消息流不出现 system)
//   user       {role, content: string|(text|image)[], timestamp}
//   assistant  {role, content:(text|thinking|toolCall)[], api, provider, model,
//               responseModel?/responseId?/errorMessage?/durationMs?/endTurn?,
//               usage, stopReason, timestamp}
//   toolResult {role, toolCallId, toolName, content:(text|image)[], details?,
//               isError, durationMs?, timestamp}
// ---------------------------------------------------------------------------
inline auto nowMs() -> int64_t {
   return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline auto roleToString(domain::Role r) -> const char* {
   switch (r) {
      case domain::Role::System:    return "system";
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

inline auto imageToJson(domain::ImageData const& im) -> folly::dynamic {
   return folly::dynamic::object("type", "image")
                                ("data", im.data)("mimeType", im.mimeType);
}

inline auto usageToJson(domain::Usage const& u) -> folly::dynamic {
   folly::dynamic d = folly::dynamic::object
      ("input", u.input)("output", u.output)
      ("cacheRead", u.cacheRead)("cacheWrite", u.cacheWrite)
      ("totalTokens", u.totalTokens);
   if (u.reasoning >= 0)    d["reasoning"]    = u.reasoning;
   if (u.cacheWrite1h >= 0) d["cacheWrite1h"] = u.cacheWrite1h;
   folly::dynamic cost = folly::dynamic::object
      ("input", u.cost.input)("output", u.cost.output)
      ("cacheRead", u.cost.cacheRead)("cacheWrite", u.cost.cacheWrite)
      ("total", u.cost.total);
   if (u.cost.totalFromProvider) cost["totalFromProvider"] = true;
   d["cost"] = std::move(cost);
   return d;
}

inline auto messageToJson(domain::Message const& m) -> folly::dynamic {
   auto ts = m.timestampMs != 0 ? m.timestampMs : nowMs();

   if (m.role == domain::Role::User) {
      if (m.images.empty()) {
         return folly::dynamic::object("role", "user")
                                      ("content", m.text)("timestamp", ts);
      }
      auto content = folly::dynamic::array();
      if (!m.text.empty())
         content.push_back(folly::dynamic::object("type", "text")
                                                ("text", m.text));
      for (auto const& im : m.images) content.push_back(imageToJson(im));
      return folly::dynamic::object("role", "user")
                                   ("content", std::move(content))
                                   ("timestamp", ts);
   }

   if (m.role == domain::Role::Tool) {
      auto content = folly::dynamic::array();
      if (!m.text.empty() || m.images.empty())
         content.push_back(folly::dynamic::object("type", "text")
                                                ("text", m.text));
      for (auto const& im : m.images) content.push_back(imageToJson(im));
      folly::dynamic d = folly::dynamic::object
         ("role", "toolResult")
         ("toolCallId", m.toolCallId)("toolName", m.toolName)
         ("content", std::move(content))
         ("isError", m.isError)("timestamp", ts);
      if (!m.detailsJson.empty()) {
         try { d["details"] = folly::parseJson(m.detailsJson); }
         catch (...) { d["details"] = m.detailsJson; }
      }
      if (m.durationMs >= 0) d["durationMs"] = m.durationMs;
      return d;
   }

   if (m.role == domain::Role::System) {
      return folly::dynamic::object("role", "system")("content", m.text)
                                    ("timestamp", ts);
   }

   // ---- assistant ----
   auto content = folly::dynamic::array();
   for (auto const& t : m.thinking) {
      folly::dynamic b = folly::dynamic::object
         ("type", "thinking")("thinking", t.thinking);
      if (!t.signature.empty()) b["thinkingSignature"] = t.signature;
      if (t.redacted)           b["redacted"]          = true;
      content.push_back(std::move(b));
   }
   if (!m.text.empty()) {
      folly::dynamic b = folly::dynamic::object("type", "text")
                                              ("text", m.text);
      if (!m.textSignature.empty()) b["textSignature"] = m.textSignature;
      content.push_back(std::move(b));
   }
   for (auto const& tc : m.toolCalls) {
      folly::dynamic b = folly::dynamic::object("type", "toolCall")
         ("id", tc.id)("name", tc.tool);
      if (!tc.argumentsError.empty()) {
         b["arguments"]      = folly::dynamic::object();
         b["argumentsError"] = tc.argumentsError;
      } else {
         b["arguments"] = inputToDynamic(tc.input);
      }
      if (!tc.thoughtSignature.empty())
         b["thoughtSignature"] = tc.thoughtSignature;
      if (!tc.ns.empty()) b["namespace"] = tc.ns;
      content.push_back(std::move(b));
   }

   folly::dynamic d = folly::dynamic::object
      ("role", "assistant")
      ("content", std::move(content))
      ("api",      m.api.empty()      ? "mock"   : m.api)
      ("provider", m.provider.empty() ? "mock"   : m.provider)
      ("model",    m.model.empty()    ? "mock-1" : m.model)
      ("usage", usageToJson(m.usage))
      ("stopReason", domain::toWire(m.stopReason == domain::StopReason::Pending
                                       ? domain::stopReasonOf(m)
                                       : m.stopReason))
      ("timestamp", ts);
   if (!m.responseModel.empty())  d["responseModel"]  = m.responseModel;
   if (!m.responseId.empty())     d["responseId"]     = m.responseId;
   if (!m.errorMessage.empty())   d["errorMessage"]   = m.errorMessage;
   if (m.durationMs >= 0)         d["durationMs"]     = m.durationMs;
   if (m.hasEndTurn)              d["endTurn"]        = m.endTurn;
   return d;
}

inline auto messagesToJson(domain::Session const& s) -> folly::dynamic {
   auto arr = folly::dynamic::array();
   for (auto const& m : s.messages()) arr.push_back(messageToJson(m));
   return arr;
}

} // namespace pi::iface::rpc
