#pragma once

#include <chrono>
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
   OpenAiProvider(std::string apiKey, std::string baseUrl,
                  std::string model, std::string providerName = "openai-codex")
      : apiKey_(std::move(apiKey))
      , baseUrl_(std::move(baseUrl))
      , model_(std::move(model))
      , provider_(std::move(providerName)) {}

   void complete(domain::CompletionRequest req,
                 domain::CompletionHandler done) override {
      auto t0   = std::chrono::steady_clock::now();
      auto body = buildBody(req);
      auto resp = post(baseUrl_ + "/responses", folly::toJson(body));
      auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0).count();
      done(parse(resp, ms));
   }

private:
   // ---------- 请求构造 ----------
   auto buildBody(domain::CompletionRequest const& req) const -> folly::dynamic {
      auto input = folly::dynamic::array();
      for (auto const& m : req.messages) {
         switch (m.role) {
            case domain::Role::System:
               input.push_back(folly::dynamic::object
                  ("type", "message")("role", "developer")
                  ("content", folly::dynamic::array(
                     folly::dynamic::object("type", "input_text")
                                           ("text", m.text))));
               break;
            case domain::Role::User: {
               auto content = folly::dynamic::array();
               if (!m.text.empty())
                  content.push_back(folly::dynamic::object
                     ("type", "input_text")("text", m.text));
               for (auto const& im : m.images)
                  content.push_back(folly::dynamic::object
                     ("type", "input_image")
                     ("image_url", "data:" + im.mimeType + ";base64," + im.data));
               input.push_back(folly::dynamic::object
                  ("type", "message")("role", "user")
                  ("content", std::move(content)));
               break;
            }
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
   auto parse(std::string const& raw, int64_t durationMs) const
      -> domain::CompletionResult {
      folly::dynamic j;
      try {
         j = folly::parseJson(raw);
      } catch (std::exception const& e) {
         domain::Message err{domain::Role::Assistant, {}, {}, {}};
         err.errorMessage = std::string{"provider: invalid json: "}
                          + e.what();
         return {std::move(err), domain::StopReason::Error};
      }
      if (j.count("error") && j["error"].isObject()) {
         domain::Message err{domain::Role::Assistant, {}, {}, {}};
         err.errorMessage = "provider error: "
                          + j["error"]["message"].getString();
         return {std::move(err), domain::StopReason::Error};
      }

      domain::Message msg{domain::Role::Assistant, {}, {}, {}};
      msg.api        = "openai-responses";
      msg.provider   = provider_;
      msg.model      = model_;
      msg.durationMs = durationMs;
      if (j.count("id"))           msg.responseId    = j["id"].getString();
      if (j.count("model"))        msg.responseModel = j["model"].getString();

      if (j.count("output")) {
         for (auto const& item : j["output"]) {
            auto type = item.count("type") ? item["type"].getString()
                                           : std::string{};
            if (type == "message" && item.count("content")) {
               for (auto const& c : item["content"]) {
                  if (c.count("type") && c["type"].getString() == "output_text")
                     msg.text += c["text"].getString();
               }
            } else if (type == "reasoning" && item.count("summary")) {
               std::string t;
               for (auto const& s : item["summary"]) {
                  if (s.count("type") && s["type"].getString() == "summary_text")
                     t += s["text"].getString();
               }
               if (!t.empty())
                  msg.thinking.push_back(domain::ThinkingBlock{std::move(t), {}});
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

      // usage: Responses API 字段 (input_tokens/output_tokens/total_tokens +
      // input_tokens_details.cached_tokens + output_tokens_details.reasoning_tokens)
      if (j.count("usage")) {
         auto const& u = j["usage"];
         msg.usage.input  = u.count("input_tokens")  ? u["input_tokens"].asInt()  : 0;
         msg.usage.output = u.count("output_tokens") ? u["output_tokens"].asInt() : 0;
         msg.usage.totalTokens = u.count("total_tokens")
                                 ? u["total_tokens"].asInt()
                                 : msg.usage.input + msg.usage.output;
         if (u.count("output_tokens_details")) {
            auto const& d = u["output_tokens_details"];
            if (d.count("reasoning_tokens"))
               msg.usage.reasoning = d["reasoning_tokens"].asInt();
         }
         if (u.count("input_tokens_details")) {
            auto const& d = u["input_tokens_details"];
            if (d.count("cached_tokens"))
               msg.usage.cacheRead = d["cached_tokens"].asInt();
         }
      }

      auto reason = domain::StopReason::Stop;
      if (!msg.toolCalls.empty()) reason = domain::StopReason::ToolUse;
      if (j.count("status") && j["status"].getString() == "incomplete")
         reason = domain::StopReason::Length;
      if (j.count("status") && j["status"].getString() == "failed")
         reason = domain::StopReason::Error;
      return {std::move(msg), reason};
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

   std::string apiKey_, baseUrl_, model_, provider_;
};

} // namespace pi::iface::adapter
