#pragma once

#include <pi/domain/message/Message.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 领域枚举: 一次 completion 的终止原因.
// toWire() 给出 pi 协议线上的字符串 ("stop"/"toolUse"/"error").
// ---------------------------------------------------------------------------
enum class StopReason { Stop, ToolUse, Error };

inline auto toWire(StopReason r) -> const char* {
   switch (r) {
      case StopReason::Stop:    return "stop";
      case StopReason::ToolUse: return "toolUse";
      case StopReason::Error:   return "error";
   }
   return "error";
}

// Message 本身不携带 stop reason: 按 toolCalls 推导 (codec 用).
inline auto stopReasonOf(Message const& m) -> StopReason {
   return m.toolCalls.empty() ? StopReason::Stop : StopReason::ToolUse;
}

} // namespace pi::domain
