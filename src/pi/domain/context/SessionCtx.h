#pragma once

#include <string>

#include <pi/domain/session/Session.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// context 碎片: 本事务绑定的聚合与会话级配置
// ---------------------------------------------------------------------------
struct SessionCtx {
   SessionCtx(domain::Session& s, std::string prompt)
      : session(s), systemPrompt(std::move(prompt)) {}

   domain::Session& session;
   std::string      systemPrompt;
};

} // namespace pi::domain
