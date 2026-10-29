#pragma once

#include <string>

#include <cub/dci/Role.h>

#include <pi/application/context/EventQueueCtx.h>
#include <pi/application/context/TurnStateCtx.h>
#include <pi/application/Events.h>
#include <pi/domain/session/ISessionStore.h>

namespace pi::app {

// ---------------------------------------------------------------------------
// role: 把本轮增量消息交给持久化端口, 等 EV_STORE_DONE.
// 存/取都走事件机制 —— DB 是第三个异步依赖, 与 LLM/工具同构.
// ---------------------------------------------------------------------------
struct Persister {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(TurnStateCtx);

   Persister(domain::ISessionStore& store, std::string sessionId)
      : store_(store), sessionId_(std::move(sessionId)) {}

   // 返回 false = 无增量, action 同步通过
   auto begin() -> bool {
      auto& turn = ROLE(TurnStateCtx);
      if (turn.deltaMessages.empty()) return false;

      store_.append(sessionId_, turn.deltaMessages, [this](bool ok) {
         ROLE(TurnStateCtx).lastStoreOk = ok;
         ROLE(EventQueueCtx).post(EV_STORE_DONE, StoreDoneMsg{});
      });
      return true;
   }

   // EV_STORE_DONE 到达后: 增量清零 (失败由 __on_fail/__redo_if 决策, 此处只记账)
   auto settle() -> void {
      ROLE(TurnStateCtx).deltaMessages.clear();
   }

private:
   domain::ISessionStore& store_;
   std::string            sessionId_;
};

} // namespace pi::app
