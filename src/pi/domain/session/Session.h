#pragma once

#include <string>
#include <vector>

#include <pi/domain/message/Message.h>
#include <pi/domain/session/Entry.h>
#include <pi/domain/tool/Tool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 聚合根: 一个会话 = append-only 的 entry 树 (pi SessionManager 的域内对应物).
// 不变量:
//   - entry 只可追加; id 8-hex, parentId 空 = 根, timestamp ISO8601.
//   - leafId_ 只在内存 (不落盘); 加载时 = 最后一条 entry.
//   - messages() = 沿当前 leaf 的上下文投影 (compaction/context_edit 生效).
// ---------------------------------------------------------------------------
class Session {
public:
   using Id = std::string;

   explicit Session(Id id, std::string cwd = "")
      : id_(std::move(id)), cwd_(std::move(cwd)) {}

   // ---- 追加 (entry 生成: id/parentId=leafId_/timestamp 自动补) ----
   auto appendUser(std::string text) -> void;
   auto appendAssistant(Message msg) -> void;
   auto appendToolResults(std::vector<ToolResult> const& results) -> void;
   // 低层: 手写 payload 的追加; 返回已入库 entry 引用
   auto appendEntry(Entry e) -> Entry const&;
   // 加载专用: 原样入库 (不自动链 parentId, 不覆盖已有 id/timestamp)
   auto appendLoaded(Entry e) -> Entry const&;

   // ---- 树 ----
   auto entries() const -> std::vector<Entry> const& { return entries_; }
   auto leafId() const -> std::string const& { return leafId_; }
   // 当前 leaf 到根的路径 (root 在前)
   auto branchEntries() const -> std::vector<Entry>;
   auto findEntry(std::string const& id) const -> Entry const*;
   // branch(): 移动 leaf; 下一次 append 在目标下分叉
   auto setLeaf(std::string const& entryId) -> bool;
   auto resetLeaf() -> void { leafId_.clear(); }

   // ---- 投影 (pi sessionEntryToContextMessages + buildContextEntries) ----
   // 沿 leaf 路径: 最新 compaction 之后 + firstKeptEntryId 起的非 system
   // message + compaction 之后全部; context_edit 生效 (null → 剔除)
   auto messages() const -> std::vector<Message>;

   // ---- 生命周期 ----
   auto reset() -> void {
      entries_.clear();
      leafId_.clear();
      persistedUpTo_ = 0;
   }

   // ---- 持久化窗口 (entry 粒度) ----
   auto beginTurn() -> void { persistedUpTo_ = entries_.size(); }
   auto markPersisted() -> void { persistedUpTo_ = entries_.size(); }
   auto unpersisted() const -> std::vector<Entry> {
      return {entries_.begin() + static_cast<long>(persistedUpTo_),
              entries_.end()};
   }

   auto id() const -> Id const& { return id_; }
   auto cwd() const -> std::string const& { return cwd_; }
   auto parentSessionPath() const -> std::string const& {
      return parentSessionPath_;
   }
   auto setParentSessionPath(std::string p) -> void {
      parentSessionPath_ = std::move(p);
   }
   auto setLeafDirect(std::string id) { leafId_ = std::move(id); }
   auto filePath() const -> std::string const& { return filePath_; }
   auto setFilePath(std::string p) -> void { filePath_ = std::move(p); }
   auto setId(Id id) -> void { id_ = std::move(id); }
   auto setCwd(std::string c) -> void { cwd_ = std::move(c); }

private:
   Id          id_;
   std::string cwd_;
   std::string parentSessionPath_;      // fork/clone 来源文件路径
   std::string filePath_;               // 持久化文件 (内存会话为空)

   std::vector<Entry> entries_;
   std::string        leafId_;
   size_t             persistedUpTo_ = 0;
};

} // namespace pi::domain
