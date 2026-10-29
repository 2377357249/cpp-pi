#pragma once

#include <string>

#include <pi/domain/llm/ILlmProvider.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 假 LLM: 上一条是 tool 结果 → 汇总并结束本轮; 否则请求调用一次 "echo" 工具.
// 仅用于验证事务循环. 真 provider 走 proxygen(HTTPS+SSE), 实现同一端口.
// ---------------------------------------------------------------------------
class MockLlmProvider : public domain::ILlmProvider {
public:
   void complete(domain::CompletionRequest req, domain::CompletionHandler done) override {
      auto mk = [](std::string text) {
         domain::Message m{domain::Role::Assistant, std::move(text), {}, {}};
         m.api = "mock"; m.provider = "mock"; m.model = "mock-1";
         return m;
      };
      if (!req.messages.empty() && req.messages.back().role == domain::Role::Tool) {
         done(domain::CompletionResult{
            mk("工具返回: " + req.messages.back().text),
            domain::StopReason::Stop});
         return;
      }
      auto const& input = req.messages.empty() ? std::string{} : req.messages.back().text;
      // 前缀路由不同工具路径: "bash:<cmd>" / "unknown:<x>" / 默认 echo
      std::string tool = "echo", arg = input;
      if (input.rfind("bash:", 0) == 0)    { tool = "bash";    arg = input.substr(5); }
      if (input.rfind("unknown:", 0) == 0) { tool = "no_such"; arg = input.substr(8); }
      auto msg = mk("我需要先调用 " + tool + " 工具");
      msg.toolCalls.push_back(domain::ToolCall{"call-1", tool, arg});
      done(domain::CompletionResult{std::move(msg),
                                    domain::StopReason::ToolUse});
   }
};

} // namespace pi::iface::adapter
