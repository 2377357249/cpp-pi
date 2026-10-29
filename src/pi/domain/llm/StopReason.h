#pragma once

#include <string_view>

#include <pi/domain/message/Message.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// StopReason 枚举在 Message.h(消息携带该字段). 这里放线格式映射.
// pi: "pending" | "stop" | "length" | "toolUse" | "error" | "aborted" | "deferred"
// ---------------------------------------------------------------------------
inline auto toWire(StopReason r) -> const char* {
   switch (r) {
      case StopReason::Pending:  return "pending";
      case StopReason::Stop:     return "stop";
      case StopReason::Length:   return "length";
      case StopReason::ToolUse:  return "toolUse";
      case StopReason::Error:    return "error";
      case StopReason::Aborted:  return "aborted";
      case StopReason::Deferred: return "deferred";
   }
   return "error";
}

inline auto stopReasonFromWire(std::string_view s) -> StopReason {
   if (s == "stop")     return StopReason::Stop;
   if (s == "length")   return StopReason::Length;
   if (s == "toolUse")  return StopReason::ToolUse;
   if (s == "aborted")  return StopReason::Aborted;
   if (s == "deferred") return StopReason::Deferred;
   if (s == "pending")  return StopReason::Pending;
   return StopReason::Error;
}

// 历史消息缺 stop reason 时按 toolCalls 推导 (codec 兜底).
inline auto stopReasonOf(Message const& m) -> StopReason {
   return m.toolCalls.empty() ? StopReason::Stop : StopReason::ToolUse;
}

} // namespace pi::domain
