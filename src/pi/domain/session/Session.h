#pragma once

#include <string>
#include <vector>

#include <pi/domain/message/Message.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 聚合根: 一个会话 = 一段有序消息历史
// 不变量: 消息只可追加; role == Tool 的消息经 appendToolResults 统一落地.
// ---------------------------------------------------------------------------
class Session {
public:
   using Id = std::string;

   explicit Session(Id id) : id_(std::move(id)) {}

   auto appendUser(std::string text) -> void;
   auto appendAssistant(Message msg) -> void;
   auto appendToolResults(std::vector<ToolResult> const& results) -> void;

   // new_session 边界: 清空历史(聚合生命周期操作, 非消息操作)
   auto reset() -> void {
      history_.clear();
      persistedUpTo_ = 0;
   }

   // 轮次边界: runTurn 在追加 user 消息后调用,
   // 之后 append 的消息才进入待持久化窗口
   auto beginTurn() -> void { persistedUpTo_ = history_.size(); }

   // 持久化窗口: [persistedUpTo_, end) 是尚未落盘的增量消息
   auto markPersisted() -> void { persistedUpTo_ = history_.size(); }
   auto unpersisted() const -> std::vector<Message> {
      return {history_.begin() + static_cast<long>(persistedUpTo_),
              history_.end()};
   }

   auto messages() const -> std::vector<Message> const& { return history_; }
   auto id() const -> Id const& { return id_; }

private:
   Id                   id_;
   std::vector<Message> history_;
   size_t               persistedUpTo_ = 0;
};

} // namespace pi::domain
