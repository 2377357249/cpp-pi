#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>

#include <event/concept/EventId.h>

namespace pi::domain {

// 一件待投递的协议事件; payload 在 handleEvent 期间保持存活
struct QueuedEvent {
   EV_NS::EventId            id;
   std::array<std::byte, 64> bytes{};
   size_t                    size = 0;
};

// ---------------------------------------------------------------------------
// context 碎片: 事务的"邮箱" —— 所有协议事件的唯一漏斗.
// (journal 事件落盘以后就在 post() 这一个点 tee)
// ---------------------------------------------------------------------------
struct EventQueueCtx {
   std::deque<QueuedEvent> queue;
   // 投递钩子: AgentSessionService 挂 loop.post(pump) —— 泵送延迟到
   // loop 线程的下一个任务执行, 绝不重入 handleEvent/start 调用栈.
   // (post 本身仍只在 loop 线程被调用 —— 适配器回调已先行 marshal.)
   std::function<void()>   onPost;

   template<typename T>
   auto post(EV_NS::EventId id, T const& msg) -> void {
      static_assert(sizeof(T) <= sizeof(QueuedEvent::bytes));
      QueuedEvent ev{id, {}, sizeof(T)};
      std::memcpy(ev.bytes.data(), &msg, sizeof(T));
      queue.push_back(ev);
      if (onPost) onPost();
   }
};

} // namespace pi::domain
