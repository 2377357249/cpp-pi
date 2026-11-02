#pragma once

#include <pi/domain/event/IAgentEventSink.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// context 碎片: 本轮 agent 事件的出口. 默认为 Null (CLI/无观察者场景);
// RPC 前端在启动时注入 RpcEventSink.
// ---------------------------------------------------------------------------
struct EventSinkCtx {
   IAgentEventSink* sink = defaultSink();

   static auto defaultSink() -> IAgentEventSink* {
      static NullAgentEventSink s;
      return &s;
   }
};

} // namespace pi::domain
