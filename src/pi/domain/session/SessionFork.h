#pragma once

#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <pi/domain/session/Entry.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/domain/session/Session.h>
#include <pi/infra/util/Ids.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 领域服务: fork / clone 的会话手术 (原 RpcFrontend::doFork 的会话部分).
//   atLeaf=false (fork): 目标必须是 user message entry, 新会话 = root->parent
//     路径 (消息本身不带); 返回被选中的 user 文本供前端预填.
//   atLeaf=true  (clone): 目标 = 当前 leaf, 新会话 = root->leaf 全路径.
// 新 Session 换 uuid, parentSession 指回旧文件, 持久化交给 store.writeBranch.
// 返回 selectedText; nullopt + err = 目标非法.
// ---------------------------------------------------------------------------
inline auto forkSession(domain::Session&  session,
                        domain::ISessionStore* store,
                        std::string_view  entryId,
                        bool              atLeaf,
                        std::string*      err)
   -> std::optional<std::string> {
   auto bad = [&](std::string_view msg) -> std::optional<std::string> {
      if (err) *err = std::string{msg};
      return std::nullopt;
   };

   if (entryId.empty())
      return bad(atLeaf ? "Cannot clone session: no current entry selected"
                        : "Invalid entry ID for forking");

   auto const* target = session.findEntry(std::string{entryId});
   std::string selectedText;
   if (auto const* me = target
          ? std::get_if<domain::MessageEntry>(&target->payload)
          : nullptr) {
      if (me->message.role == domain::Role::User)
         selectedText = me->message.text;
   }

   if (!atLeaf) {
      // fork: 目标是 user message, leaf 移到其 parent (position "before")
      if (!target
          || !std::holds_alternative<domain::MessageEntry>(target->payload)
          || std::get<domain::MessageEntry>(target->payload)
                  .message.role != domain::Role::User)
         return bad("Invalid entry ID for forking");
   }
   auto const targetLeaf = atLeaf ? std::string{entryId}
                                  : target->parentId;

   // 新会话 = root->targetLeaf 路径 (持久化时另写文件)
   std::string newFile;
   if (store) newFile = store->writeBranch(session, targetLeaf);

   domain::Session next{infra::uuidv7(), session.cwd()};
   next.setParentSessionPath(session.filePath().empty()
                                ? session.id() + ".jsonl"
                                : session.filePath());
   next.setFilePath(newFile);
   std::vector<domain::Entry> path;
   for (std::string cur = targetLeaf; !cur.empty();) {
      auto const* e = session.findEntry(cur);
      if (!e) break;
      path.push_back(*e);
      cur = e->parentId;
   }
   std::string parent;
   for (auto const& e : path | std::views::reverse) {
      auto copy     = e;
      copy.parentId = parent;
      parent        = copy.id;
      next.appendLoaded(std::move(copy));
   }
   next.markPersisted();
   session = std::move(next);
   return selectedText;
}

} // namespace pi::domain
