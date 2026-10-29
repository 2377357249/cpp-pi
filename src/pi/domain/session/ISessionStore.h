#pragma once

#include <functional>
#include <vector>

#include <pi/domain/message/Message.h>
#include <pi/domain/session/Session.h>

namespace pi::domain {

using StoreDoneHandler = std::function<void(bool ok)>;
using LoadDoneHandler  = std::function<void(std::vector<Message>)>;

// ---------------------------------------------------------------------------
// 端口: 会话持久化. 回调式签名, 同步/异步实现均可, 不感知事件.
// 语义约束: append 只追加, Session 聚合保证消息序.
// ---------------------------------------------------------------------------
struct ISessionStore {
   virtual ~ISessionStore() = default;

   virtual void append(Session::Id const& id,
                       std::vector<Message> const& delta,
                       StoreDoneHandler done) = 0;
   virtual void load(Session::Id const& id, LoadDoneHandler done) = 0;
};

} // namespace pi::domain
