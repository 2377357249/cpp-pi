#include <chrono>

#include <pi/domain/session/Session.h>

namespace pi::domain {

namespace {
auto nowMs() -> int64_t {
   return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

auto Session::appendUser(std::string text) -> void {
   Message m{Role::User, std::move(text), {}, {}};
   m.timestampMs = nowMs();
   history_.push_back(std::move(m));
}

auto Session::appendAssistant(Message msg) -> void {
   msg.role = Role::Assistant;
   if (msg.timestampMs == 0) msg.timestampMs = nowMs();
   history_.push_back(std::move(msg));
}

auto Session::appendToolResults(std::vector<ToolResult> const& results) -> void {
   for (auto const& r : results) {
      Message m{Role::Tool, r.content, {}, r.toolCallId, r.toolName, r.isError};
      m.images      = r.images;
      m.detailsJson = r.detailsJson;
      m.durationMs  = r.durationMs;
      m.timestampMs = nowMs();
      history_.push_back(std::move(m));
   }
}

} // namespace pi::domain
