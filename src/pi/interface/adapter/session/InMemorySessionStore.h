#pragma once

#include <map>
#include <vector>

#include <pi/domain/session/ISessionStore.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 内存会话存储(骨架): 验证端口与 EV_STORE_DONE 链路.
// 正式的 JsonlSessionStore 用 folly::dynamic 序列化, 落 ~/.cpp-pi/sessions/*.jsonl.
// ---------------------------------------------------------------------------
class InMemorySessionStore : public domain::ISessionStore {
public:
   void append(domain::Session::Id const& id,
               std::vector<domain::Message> const& delta,
               domain::StoreDoneHandler done) override {
      auto& log = sessions_[id];
      log.insert(log.end(), delta.begin(), delta.end());
      done(true);
   }

   void load(domain::Session::Id const& id, domain::LoadDoneHandler done) override {
      auto it = sessions_.find(id);
      done(it == sessions_.end() ? std::vector<domain::Message>{} : it->second);
   }

private:
   std::map<domain::Session::Id, std::vector<domain::Message>> sessions_;
};

} // namespace pi::iface::adapter
