#pragma once

#include <string>

#include <cub/dci/Role.h>

#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/SessionCtx.h>
#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/event/Events.h>
#include <pi/domain/session/ISessionStore.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// role: 把聚合的未持久化增量交给持久化端口, 等 EV_STORE_DONE.
// 存/取都走事件机制 —— DB 是第三个异步依赖, 与 LLM/工具同构.
// ---------------------------------------------------------------------------
struct Persister {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(TurnStateCtx);
   USE_ROLE(SessionCtx);

   Persister(domain::ISessionStore& store, std::string sessionId)
      : store_(store), sessionId_(std::move(sessionId)) {}

   // 返回 false = 无增量, action 同步通过
   auto begin() -> bool {
      auto delta = ROLE(SessionCtx).session.unpersisted();
      if (delta.empty()) return false;

      store_.append(sessionId_, std::move(delta), [this](bool ok) {
         ROLE(TurnStateCtx).lastStoreOk = ok;
         ROLE(EventQueueCtx).post(EV_STORE_DONE, StoreDoneMsg{});
      });
      return true;
   }

   // EV_STORE_DONE 到达后: 聚合标记已落盘 (失败由 __on_fail/__redo_if 决策)
   auto settle() -> void {
      ROLE(SessionCtx).session.markPersisted();
   }

private:
   domain::ISessionStore& store_;
   std::string            sessionId_;
};

} // namespace pi::domain
