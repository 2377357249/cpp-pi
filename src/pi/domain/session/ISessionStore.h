#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <pi/domain/session/Entry.h>
#include <pi/domain/session/Session.h>

namespace pi::domain {

using StoreDoneHandler = std::function<void(bool ok)>;
using LoadDoneHandler  = std::function<void(Session session, std::string filePath)>;

// /resume 列表项: 文件路径 + 会话元数据预览 (header + 首条 user + 最新名字).
struct SessionListItem {
   std::string path;
   std::string sessionId;
   std::string timestamp;      // header ISO 时间
   std::string name;           // 最新 session_info entry 的名字 (可空)
   std::string firstUserText;  // 首条 user message 预览 (可空)
   int64_t     mtimeMs = 0;    // 文件 mtime (排序用)
};

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
   // 列出 cwd 目录下的历史会话 (新→旧排序). 默认空 (内存存储无列表).
   virtual auto list(std::string_view cwd) const
      -> std::vector<SessionListItem> {
      (void)cwd;
      return {};
   }
};

} // namespace pi::domain
