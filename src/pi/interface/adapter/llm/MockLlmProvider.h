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
   // toolCall.arguments 是 JSON 串: 拼字符串字段时需要转义
   static auto escapeJson(std::string_view s) -> std::string {
      std::string out;
      for (char c : s) {
         switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
         }
      }
      return out;
   }

   void complete(domain::CompletionRequest req, domain::CompletionHandler done) override {
      auto mk = [](std::string text) {
         domain::Message m{domain::Role::Assistant, std::move(text), {}, {}};
         m.api = "mock"; m.provider = "mock"; m.model = "mock-1";
         return m;
      };
      // compaction 摘要请求: system prompt 是 summarization assistant → 纯文本
      if (req.systemPrompt.find("context summarization assistant")
          != std::string::npos) {
         done(domain::CompletionResult{
            mk("## Goal\nmock summary of the conversation"),
            domain::StopReason::Stop});
         return;
      }
      if (!req.messages.empty() && req.messages.back().role == domain::Role::Tool) {
         done(domain::CompletionResult{
            mk("工具返回: " + req.messages.back().text),
            domain::StopReason::Stop});
         return;
      }
      auto const& input = req.messages.empty() ? std::string{} : req.messages.back().text;
      // 前缀路由不同工具路径:
      //   "bash:<cmd>" / "unknown:<x>" / "tool:<name>:<json>" / 默认 echo
      std::string tool = "echo", arg = input;
      if (input.rfind("bash:", 0) == 0) {
         tool = "bash";
         arg  = std::string{R"({"command":")"} + escapeJson(input.substr(5))
                + "\"}";
      }
      if (input.rfind("unknown:", 0) == 0) { tool = "no_such"; arg = input.substr(8); }
      if (input.rfind("tool:", 0) == 0) {
         auto const sep = input.find(':', 5);
         if (sep != std::string::npos) {
            tool = input.substr(5, sep - 5);
            arg  = input.substr(sep + 1);
         }
      }
      auto msg = mk("我需要先调用 " + tool + " 工具");
      msg.toolCalls.push_back(domain::ToolCall{"call-1", tool, arg});
      done(domain::CompletionResult{std::move(msg),
                                    domain::StopReason::ToolUse});
   }
};

} // namespace pi::iface::adapter
