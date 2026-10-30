#pragma once

#include <functional>
#include <vector>

#include <pi/domain/session/Entry.h>
#include <pi/domain/session/Session.h>

namespace pi::domain {

using StoreDoneHandler = std::function<void(bool ok)>;
using LoadDoneHandler  = std::function<void(Session session, std::string filePath)>;

// ---------------------------------------------------------------------------
// 端口: 会话持久化. 回调式签名, 同步/异步实现均可, 不感知事件.
// 语义约束:
//   - append 只追加 (pi JSONL 的 append-only 语义); 失败回调 false.
//   - load 读 JSONL 文件 (SessionHeader + entries), 返回完整 Session.
// 文件布局/命名/迁移归实现 (interface/adapter/session).
// ---------------------------------------------------------------------------
struct ISessionStore {
   virtual ~ISessionStore() = default;

   virtual void append(Session const&          session,
                       std::vector<Entry> const& delta,
                       StoreDoneHandler          done) = 0;
   virtual void load(std::string const& sessionPath,
                     LoadDoneHandler    done) = 0;
   // fork/clone: 把 root->targetLeafId 路径写成新会话文件; 返回文件路径.
   // 默认实现 (内存存储) 不落盘, 返回空串.
   virtual auto writeBranch(Session const&   src,
                            std::string_view targetLeafId) -> std::string {
      (void)src; (void)targetLeafId;
      return {};
   }
   // 会话的持久化文件路径 (内存存储恒为空)
   virtual auto fileOf(Session const& session) const -> std::string {
      (void)session;
      return {};
   }
};

} // namespace pi::domain
