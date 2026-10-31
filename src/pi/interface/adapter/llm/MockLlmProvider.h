#pragma once

#include <charconv>
#include <string>

#include <pi/domain/llm/ILlmProvider.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 假 LLM: 上一条是 tool 结果 → 汇总并结束本轮; 否则请求调用一次 "echo" 工具.
// 仅用于验证事务循环. 真 provider 走 proxygen(HTTPS+SSE), 实现同一端口.
// 流式: 按 pi AssistantMessageEvent 协议发合成增量 ——
//   文本: start → text_start → text_delta×2 → text_end → done
//   工具: start → toolcall_start → toolcall_delta → toolcall_end → done
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

   void complete(domain::CompletionRequest req,
                 domain::AssistantStreamHandler onEvent,
                 domain::CompletionHandler done) override {
      auto mk = [](std::string text) {
         domain::Message m{domain::Role::Assistant, std::move(text), {}, {}};
         m.api = "mock"; m.provider = "mock"; m.model = "mock-1";
         return m;
      };
      // pi 流协议: 文本应答的增量序列
      auto streamText = [&](domain::Message const& m,
                            domain::StopReason reason,
                            domain::CompletionResult r) {
         domain::AssistantStreamEvent e{};
         e.type = domain::AssistantStreamType::Start;
         onEvent(e);
         auto const half = m.text.size() / 2;
         e.type = domain::AssistantStreamType::TextStart;
         onEvent(e);
         e.type = domain::AssistantStreamType::TextDelta;
         e.delta = m.text.substr(0, half);
         onEvent(e);
         e.delta = m.text.substr(half);
         onEvent(e);
         e.type = domain::AssistantStreamType::TextEnd;
         e.delta = m.text;
         onEvent(e);
         e.type = domain::AssistantStreamType::Done;
         e.delta.clear();
         e.reason = reason;
         e.usage  = m.usage;
         onEvent(e);
         done(std::move(r));
      };
      // compaction 摘要请求: system prompt 是 summarization assistant → 纯文本
      if (req.systemPrompt.find("context summarization assistant")
          != std::string::npos) {
         auto m = mk("## Goal\nmock summary of the conversation");
         streamText(m, domain::StopReason::Stop,
                    domain::CompletionResult{m,
                                             domain::StopReason::Stop});
         return;
      }
      if (!req.messages.empty() && req.messages.back().role == domain::Role::Tool) {
         auto m = mk("工具返回: " + req.messages.back().text);
         streamText(m, domain::StopReason::Stop,
                    domain::CompletionResult{m,
                                             domain::StopReason::Stop});
         return;
      }
      auto const& input = req.messages.empty() ? std::string{} : req.messages.back().text;
      // "usage:<N>": 带 usage 的完成应答 —— 驱动自动压缩阈值测试
      if (input.rfind("usage:", 0) == 0) {
         int64_t n = 0;
         std::from_chars(input.data() + 6,
                         input.data() + input.size(), n);
         auto m = mk("usage reply");
         m.usage.totalTokens = n;
         m.usage.input = n;
         streamText(m, domain::StopReason::Stop,
                    domain::CompletionResult{m,
                                             domain::StopReason::Stop});
         return;
      }
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
      domain::ToolCall call{"call-1", tool, arg};
      msg.toolCalls.push_back(call);
      // 增量序列 (pi 内容块交错): text@0 → toolcall@1 → done(toolUse)
      domain::AssistantStreamEvent e{};
      e.type = domain::AssistantStreamType::Start;
      onEvent(e);
      e.type = domain::AssistantStreamType::TextStart;
      onEvent(e);
      e.type  = domain::AssistantStreamType::TextDelta;
      e.delta = msg.text;
      onEvent(e);
      e.type = domain::AssistantStreamType::TextEnd;
      onEvent(e);
      e.delta.clear();
      e.contentIndex = 1;
      e.type = domain::AssistantStreamType::ToolCallStart;
      e.toolCallId = call.id;
      e.toolName   = call.tool;
      onEvent(e);
      e.type = domain::AssistantStreamType::ToolCallDelta;
      e.toolCallId.clear(); e.toolName.clear();
      e.delta = call.input;
      onEvent(e);
      e.type = domain::AssistantStreamType::ToolCallEnd;
      e.delta.clear();
      e.toolCall = call;
      onEvent(e);
      e.type = domain::AssistantStreamType::Done;
      e.toolCall = {};
      e.reason = domain::StopReason::ToolUse;
      e.usage  = msg.usage;
      onEvent(e);
      done(domain::CompletionResult{std::move(msg),
                                    domain::StopReason::ToolUse});
   }
};

} // namespace pi::iface::adapter
