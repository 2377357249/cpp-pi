#pragma once

#include <string>

#include <curl/curl.h>
#include <folly/json/json.h>

#include <pi/domain/llm/ILlmProvider.h>
#include <pi/log/Logging.h>

namespace pi::infra {

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
                 "error"};
      }
      if (j.count("error") && j["error"].isObject()) {
         return {{domain::Role::Assistant,
                  "provider error: " +
                     j["error"]["message"].getString(), {}, {}},
                 "error"};
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
              msg.toolCalls.empty() ? "end_turn" : "tool_use"};
   }

   // ---------- HTTP ----------
   auto post(std::string const& url, std::string const& body) const
      -> std::string {
      CURL* curl = curl_easy_init();
      if (!curl) return R"({"error":{"message":"curl init failed"}})";

      std::string out;
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                       static_cast<long>(body.size()));
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

      auto auth = "Authorization: Bearer " + apiKey_;
      curl_slist* headers = nullptr;
      headers = curl_slist_append(headers, auth.c_str());
      headers = curl_slist_append(headers, "Content-Type: application/json");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      auto rc = curl_easy_perform(curl);
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      if (rc != CURLE_OK) {
         return std::string{"{\"error\":{\"message\":\"curl: "}
                          + curl_easy_strerror(rc) + "\"}";
      }
      if (code >= 400 && out.find("\"error\"") == std::string::npos) {
         return std::string{"{\"error\":{\"message\":\"http "}
                          + std::to_string(code) + ": " + out.substr(0, 400)
                          + "\"}";
      }
      LOG_INFO(pi::log::logger(), "llm <- http {}", code);
      return out;
   }

   static auto writeCb(char* p, size_t s, size_t n, void* ud) -> size_t {
      static_cast<std::string*>(ud)->append(p, s * n);
      return s * n;
   }

   std::string apiKey_, baseUrl_, model_;
};

} // namespace pi::infra
