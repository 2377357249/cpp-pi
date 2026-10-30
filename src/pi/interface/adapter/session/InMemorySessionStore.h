#pragma once

#include <map>
#include <vector>

#include <pi/domain/session/ISessionStore.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 内存会话存储: 不落盘 (--no-session 场景的对应物). entry 粒度存全量.
// ---------------------------------------------------------------------------
class InMemorySessionStore : public domain::ISessionStore {
public:
   void append(domain::Session const&          session,
               std::vector<domain::Entry> const& delta,
               domain::StoreDoneHandler          done) override {
      auto& log = sessions_[session.id()];
      log.insert(log.end(), delta.begin(), delta.end());
      done(true);
   }

   void load(std::string const& sessionPath,
             domain::LoadDoneHandler done) override {
      auto it = sessions_.find(sessionPath);
      domain::Session s{sessionPath};
      if (it != sessions_.end()) {
         for (auto e : it->second) s.appendEntry(std::move(e));
      }
      done(std::move(s), it == sessions_.end() ? std::string{} : sessionPath);
   }

private:
   std::map<domain::Session::Id, std::vector<domain::Entry>> sessions_;
};

} // namespace pi::iface::adapter
