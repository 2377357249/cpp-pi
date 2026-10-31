#pragma once

#include <functional>
#include <string>

#include <pi/domain/llm/StopReason.h>
#include <pi/domain/message/Message.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// pi AssistantMessageEvent (ai/types.ts): 流式 completion 的增量事件协议.
//   start → (text|thinking|toolcall)_{start,delta,end}* → done | error
// 线格式 (modes/json-event.ts): 剥掉 partial, toolcall_start 展开 {id,toolName},
// 其余字段原样; done/error 携带最终消息 —— 回放时由 codec 填.
// ---------------------------------------------------------------------------
enum class AssistantStreamType {
   Start,
   TextStart, TextDelta, TextEnd,
   ThinkingStart, ThinkingDelta, ThinkingEnd,
   ToolCallStart, ToolCallDelta, ToolCallEnd,
   Done, Error,
};

struct AssistantStreamEvent {
   AssistantStreamType type;
   int         contentIndex = 0;
   std::string delta;        // *_delta.delta; *_end.content
   std::string toolCallId;   // toolcall_start
   std::string toolName;     // toolcall_start
   domain::ToolCall toolCall;// toolcall_end.toolCall
   StopReason  reason = StopReason::Pending;  // done/error
   Usage       usage;        // event 时刻 partial.message.usage 快照
};

using AssistantStreamHandler =
   std::function<void(AssistantStreamEvent const&)>;

} // namespace pi::domain
