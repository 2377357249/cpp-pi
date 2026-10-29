#include <pi/domain/session/Session.h>

namespace pi::domain {

auto Session::appendUser(std::string text) -> void {
   history_.push_back(Message{Role::User, std::move(text), {}, {}});
}

auto Session::appendAssistant(Message msg) -> void {
   msg.role = Role::Assistant;
   history_.push_back(std::move(msg));
}

auto Session::appendToolResults(std::vector<ToolResult> const& results) -> void {
   for (auto const& r : results) {
      history_.push_back(Message{Role::Tool, r.content, {}, r.toolCallId});
   }
}

} // namespace pi::domain
