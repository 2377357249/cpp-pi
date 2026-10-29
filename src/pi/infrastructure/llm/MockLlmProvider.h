#pragma once

#include <string>

#include <pi/domain/llm/ILlmProvider.h>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 假 LLM: 上一条是 tool 结果 → 汇总并结束本轮; 否则请求调用一次 "echo" 工具.
// 仅用于验证事务循环. 真 provider 走 proxygen(HTTPS+SSE), 实现同一端口.
// ---------------------------------------------------------------------------
class MockLlmProvider : public domain::ILlmProvider {
public:
   void complete(domain::CompletionRequest req, domain::CompletionHandler done) override {
      if (!req.messages.empty() && req.messages.back().role == domain::Role::Tool) {
         done(domain::CompletionResult{
            domain::Message{domain::Role::Assistant,
                            "工具返回: " + req.messages.back().text, {}, {}},
            "end_turn"});
         return;
      }
      auto const& input = req.messages.empty() ? std::string{} : req.messages.back().text;
      // 前缀路由不同工具路径: "bash:<cmd>" / "unknown:<x>" / 默认 echo
      std::string tool = "echo", arg = input;
      if (input.rfind("bash:", 0) == 0)    { tool = "bash";    arg = input.substr(5); }
      if (input.rfind("unknown:", 0) == 0) { tool = "no_such"; arg = input.substr(8); }
      done(domain::CompletionResult{
         domain::Message{domain::Role::Assistant,
                         "我需要先调用 " + tool + " 工具",
                         {domain::ToolCall{"call-1", tool, arg}}, {}},
         "tool_use"});
   }
};

} // namespace pi::infra
