#pragma once

#include <string>

#include <folly/json/json.h>

#include <pi/domain/llm/ILlmProvider.h>
#include <pi/infra/http/HttpClient.h>
#include <pi/infra/log/Logging.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// OpenAI Responses API provider (POST {baseUrl}/responses, 非流式).
// 同步实现端口, 语义仍是"回调一次交出 CompletionResult" —— 事务无感.
// 消息映射:
//   user       -> {type:"message", role:"user", content:[{type:"input_text"}]}
//   assistant  -> 文本 {type:"message",role:"assistant",content:[output_text]}
//                 + 每个 toolCall 一条 {type:"function_call",call_id,...}
//   toolResult -> {type:"function_call_output", call_id, output}
// ---------------------------------------------------------------------------
class OpenAiProvider : public domain::ILlmProvider {
public:
   OpenAiProvider(std::string apiKey, std::string baseUrl, std::string model)
      : apiKey_(std::move(apiKey))
      , baseUrl_(std::move(baseUrl))
      , model_(std::move(model)) {}

   void complete(domain::CompletionRequest req,
                 domain::CompletionHandler done) override {
      auto body = buildBody(req);
      auto resp = post(baseUrl_ + "/responses", folly::toJson(body));
      done(parse(resp));
   }

private:
   // ---------- 请求构造 ----------
   auto buildBody(domain::CompletionRequest const& req) const -> folly::dynamic {
      auto input = folly::dynamic::array();
      for (auto const& m : req.messages) {
         switch (m.role) {
            case domain::Role::User:
               input.push_back(folly::dynamic::object
                  ("type", "message")("role", "user")
                  ("content", folly::dynamic::array(
                     folly::dynamic::object("type", "input_text")
                                           ("text", m.text))));
               break;
            case domain::Role::Assistant:
               if (!m.text.empty())
                  input.push_back(folly::dynamic::object
                     ("type", "message")("role", "assistant")
                     ("content", folly::dynamic::array(
                        folly::dynamic::object("type", "output_text")
                                              ("text", m.text))));
               for (auto const& tc : m.toolCalls)
                  input.push_back(folly::dynamic::object
                     ("type", "function_call")
                     ("call_id", tc.id)("name", tc.tool)
                     ("arguments", tc.input));
               break;
            case domain::Role::Tool:
               input.push_back(folly::dynamic::object
                  ("type", "function_call_output")
                  ("call_id", m.toolCallId)("output", m.text));
               break;
         }
      }

      auto tools = folly::dynamic::array();
      for (auto const& t : req.tools) {
         folly::dynamic params;
         try { params = folly::parseJson(t.inputSchema); }
         catch (...) { params = folly::dynamic::object("type", "object"); }
         tools.push_back(folly::dynamic::object
            ("type", "function")("name", t.name)
            ("description", t.description)("parameters", std::move(params))
            ("strict", false));
      }

      return folly::dynamic::object
         ("model", model_)("input", std::move(input))
         ("tools", std::move(tools))
         ("stream", false)("store", false);
   }

   // ---------- 响应解析 ----------
   static auto parse(std::string const& raw) -> domain::CompletionResult {
      folly::dynamic j;
      try {
         j = folly::parseJson(raw);
      } catch (std::exception const& e) {
         return {{domain::Role::Assistant,
                  std::string{"provider: invalid json: "} + e.what(), {}, {}},
                 domain::StopReason::Error};
      }
      if (j.count("error") && j["error"].isObject()) {
         return {{domain::Role::Assistant,
                  "provider error: " +
                     j["error"]["message"].getString(), {}, {}},
                 domain::StopReason::Error};
      }

      domain::Message msg{domain::Role::Assistant, {}, {}, {}};
      if (j.count("output")) {
         for (auto const& item : j["output"]) {
            auto type = item.count("type") ? item["type"].getString()
                                           : std::string{};
            if (type == "message" && item.count("content")) {
               for (auto const& c : item["content"]) {
                  if (c.count("type") && c["type"].getString() == "output_text")
                     msg.text += c["text"].getString();
               }
            } else if (type == "function_call") {
               msg.toolCalls.push_back(domain::ToolCall{
                  item.count("call_id") ? item["call_id"].getString()
                                        : std::string{},
                  item.count("name") ? item["name"].getString()
                                     : std::string{},
                  item.count("arguments") ? item["arguments"].getString()
                                          : std::string{}});
            }
         }
      }
      return {std::move(msg),
              msg.toolCalls.empty() ? domain::StopReason::Stop
                                    : domain::StopReason::ToolUse};
   }

   // ---------- HTTP ----------
   auto post(std::string const& url, std::string const& body) const
      -> std::string {
      auto r = infra::httpPostJson(url, body,
                                   {"Authorization: Bearer " + apiKey_,
                                    "Content-Type: application/json"});
      if (!r.error.empty()) {
         if (r.error == "curl init failed") {
            return R"({"error":{"message":"curl init failed"}})";
         }
         return std::string{"{\"error\":{\"message\":\"curl: "} + r.error
                          + "\"}";
      }
      if (r.status >= 400 && r.body.find("\"error\"") == std::string::npos) {
         return std::string{"{\"error\":{\"message\":\"http "}
                          + std::to_string(r.status) + ": "
                          + r.body.substr(0, 400) + "\"}";
      }
      LOG_INFO(pi::infra::log::logger(), "llm <- http {}", r.status);
      return r.body;
   }

   std::string apiKey_, baseUrl_, model_;
};

} // namespace pi::iface::adapter
