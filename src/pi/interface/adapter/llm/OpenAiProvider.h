#pragma once

#include <format>

#include <pi/infra/util/JsonSafe.h>

#include <chrono>
#include <string>
#include <unordered_map>

#include <folly/json/json.h>

#include <pi/domain/llm/ILlmProvider.h>
#include <pi/infra/http/HttpClient.h>
#include <pi/infra/log/Logging.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// OpenAI Responses API provider (POST {baseUrl}/responses, SSE 流式).
// 同步实现端口 —— 语义仍是"事件增量 + 回调一次交出 CompletionResult".
// SSE 事件翻译对齐 pi packages/ai openai-responses-shared.ts:
//   response.output_item.added    reasoning→thinking_start / message→text_start
//                                 / function_call→toolcall_start
//   response.reasoning_*_text.delta   → thinking_delta
//   response.reasoning_summary_part.done → thinking_delta("\n\n")
//   response.output_text/refusal.delta   → text_delta
//   response.function_call_arguments.{delta,done} → toolcall_delta
//   response.output_item.done       → thinking_end / text_end / toolcall_end
//   response.completed|incomplete   → done{reason, usage}
//   response.failed | error         → error
// reasoning 块回放: thinking.signature 存原始 item JSON (含
// encrypted_content), 下一轮原样塞回 input —— store:false 多轮推理连续.
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
                 domain::AssistantStreamHandler onEvent,
                 domain::CompletionHandler done) override {
      auto const modelId = req.model.id.empty() ? model_ : req.model.id;
      auto t0   = std::chrono::steady_clock::now();
      auto body = buildBody(req, modelId);

      SseState st{onEvent, modelId, provider_};
      auto resp = infra::httpPostJsonStream(
         baseUrl_ + "/responses", folly::toJson(body),
         {"Authorization: Bearer " + apiKey_,
          "Content-Type: application/json",
          "Accept: text/event-stream"},
         /*timeoutSec=*/300,
         [&st](std::string_view chunk) { return st.feed(chunk); });

      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
      LOG_INFO(pi::infra::log::logger(), "llm <- http {} ({}ms)",
               resp.status, ms);

      if (st.result) {
         st.result->message.durationMs = ms;
         done(std::move(*st.result));
         return;
      }
      // 流未走到终态: HTTP 错误体 / 传输失败 / 缺 response.completed.
      auto errText = !resp.error.empty() ? "curl: " + resp.error
                     : resp.status >= 400 ? httpError(resp)
                     : st.error.empty()   ? "stream ended without a terminal "
                                          "response event"
                                          : st.error;
      domain::Message err{domain::Role::Assistant, {}, {}, {}};
      err.api = "openai-responses"; err.provider = provider_;
      err.model = modelId; err.durationMs = ms;
      err.errorMessage = "provider error: " + errText;
      domain::AssistantStreamEvent e{};
      e.type = domain::AssistantStreamType::Error;
      e.reason = domain::StopReason::Error;
      onEvent(e);
      done(domain::CompletionResult{std::move(err),
                                    domain::StopReason::Error});
   }

private:
   // ---------- SSE 流状态 ----------
   struct Slot {
      enum class Kind { Thinking, Text, Tool } kind;
      int               contentIndex = 0;
      std::string       buf;              // thinking / text / args 累积
      domain::ToolCall  tool;
   };

   struct SseState {
      domain::AssistantStreamHandler       onEvent;
      std::string                          modelId, provider;
      std::string                          buf;          // 行缓冲
      std::unordered_map<int, Slot>        slots;        // output_index -> slot
      int                                  nextIndex = 0;
      bool                                 started = false;
      bool                                 terminated = false;
      std::string                          error;        // SSE error 帧消息
      std::optional<domain::CompletionResult> result;    // response.completed
      std::string                          responseId;

      auto emit(domain::AssistantStreamType t, int idx = 0,
                std::string delta = {},
                domain::StopReason reason = domain::StopReason::Pending)
         -> void {
         domain::AssistantStreamEvent e{};
         e.type = t; e.contentIndex = idx; e.delta = std::move(delta);
         e.reason = reason;
         e.model = modelId; e.provider = provider;
         onEvent(e);
      }
      auto ensureStart() -> void {
         if (started) return;
         started = true;
         domain::AssistantStreamEvent e{};
         e.type = domain::AssistantStreamType::Start;
         e.api = "openai-responses"; e.provider = provider;
         e.model = modelId;
         onEvent(e);
      }

      // 喂字节块: 按行切, "data: {...}" 逐条进 handle.
      auto feed(std::string_view chunk) -> bool {
         buf += chunk;
         for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.starts_with("data:")) continue;
            auto payload = line.substr(5);
            if (auto b = payload.find_first_not_of(' ');
                b != std::string::npos) payload = payload.substr(b);
            if (payload == "[DONE]") continue;
            if (!handle(payload)) return false;
         }
         return true;
      }

      auto handle(std::string_view payload) -> bool {
         auto pj = infra::parseJsonSafe(payload);
         if (pj.hasError()) return true;   // 心跳/非 JSON 行
         auto const& j = *pj;
         // data: 不一定是 object (代理可能回裸数组/错误体) — 非事件帧跳过
         if (!j.isObject() || !j.count("type") || !j["type"].isString())
            return true;
         auto const type = j["type"].getString();
         ensureStart();

         if (type == "response.output_item.added") {
            onItemAdded(j);
         } else if (type == "response.reasoning_summary_text.delta"
                    || type == "response.reasoning_text.delta") {
            if (auto* s = slot(j, Slot::Kind::Thinking)) {
               s->buf += j["delta"].getString();
               emit(domain::AssistantStreamType::ThinkingDelta,
                    s->contentIndex, j["delta"].getString());
            }
         } else if (type == "response.reasoning_summary_part.done") {
            if (auto* s = slot(j, Slot::Kind::Thinking)) {
               s->buf += "\n\n";
               emit(domain::AssistantStreamType::ThinkingDelta,
                    s->contentIndex, "\n\n");
            }
         } else if (type == "response.output_text.delta"
                    || type == "response.refusal.delta") {
            if (auto* s = slot(j, Slot::Kind::Text)) {
               s->buf += j["delta"].getString();
               emit(domain::AssistantStreamType::TextDelta,
                    s->contentIndex, j["delta"].getString());
            }
         } else if (type == "response.function_call_arguments.delta") {
            if (auto* s = slot(j, Slot::Kind::Tool)) {
               s->buf += j["delta"].getString();
               emit(domain::AssistantStreamType::ToolCallDelta,
                    s->contentIndex, j["delta"].getString());
            }
         } else if (type == "response.function_call_arguments.done") {
            if (auto* s = slot(j, Slot::Kind::Tool)) {
               // pi: replace + 补发尾部差量
               auto const full = j["arguments"].getString();
               if (full.starts_with(s->buf) && full.size() > s->buf.size())
                  emit(domain::AssistantStreamType::ToolCallDelta,
                       s->contentIndex, full.substr(s->buf.size()));
               s->buf = full;
            }
         } else if (type == "response.output_item.done") {
            onItemDone(j);
         } else if (type == "response.completed"
                    || type == "response.incomplete") {
            terminated = true;
            if (j.count("response") && j["response"].isObject())
               result = OpenAiProvider::parseResponse(
                  j["response"], modelId, provider);
            emit(domain::AssistantStreamType::Done, 0, {},
                 result ? result->stopReason : domain::StopReason::Error);
         } else if (type == "response.failed") {
            terminated = true;
            auto const& r = j["response"];
            if (!r.isObject()) { error = "response failed"; return true; }
            if (r.count("error") && r["error"].isObject())
               error = r["error"]["message"].getString();
            else if (r.count("incomplete_details")
                     && r["incomplete_details"].count("reason"))
               error = "incomplete: "
                     + r["incomplete_details"]["reason"].getString();
            if (error.empty()) error = "response failed";
            domain::Message err{domain::Role::Assistant, {}, {}, {}};
            err.api = "openai-responses"; err.provider = provider;
            err.model = modelId; err.errorMessage = "provider: " + error;
            result = domain::CompletionResult{std::move(err),
                                              domain::StopReason::Error};
            domain::AssistantStreamEvent e{};
            e.type = domain::AssistantStreamType::Error;
            e.reason = domain::StopReason::Error;
            e.delta = error;
            onEvent(e);
         } else if (type == "error") {
            error = j.count("message") ? j["message"].getString()
                                       : "unknown stream error";
            return false;   // 中止 curl (pi: throw)
         }
         return true;
      }

      auto slot(folly::dynamic const& j, Slot::Kind k) -> Slot* {
         if (!j.count("output_index") || !j["output_index"].isInt())
            return nullptr;
         auto const i = static_cast<int>(j["output_index"].asInt());
         auto const it = slots.find(i);
         return it != slots.end() && it->second.kind == k ? &it->second
                                                        : nullptr;
      }

      auto onItemAdded(folly::dynamic const& j) -> void {
         if (!j.count("item") || !j["item"].isObject()
             || !j.count("output_index")) return;
         auto const& item = j["item"];
         auto const  i    = static_cast<int>(j["output_index"].asInt());
         auto const  t    = item.count("type") ? item["type"].getString()
                                             : std::string{};
         if (t == "reasoning") {
            slots[i] = Slot{Slot::Kind::Thinking, nextIndex++, {}, {}};
            emit(domain::AssistantStreamType::ThinkingStart,
                 slots[i].contentIndex);
         } else if (t == "message") {
            slots[i] = Slot{Slot::Kind::Text, nextIndex++, {}, {}};
            emit(domain::AssistantStreamType::TextStart,
                 slots[i].contentIndex);
         } else if (t == "function_call" || t == "custom_tool_call") {
            domain::ToolCall tc;
            tc.id   = item.count("call_id") ? item["call_id"].getString()
                                            : std::string{};
            tc.tool = item.count("name") ? item["name"].getString()
                                         : std::string{};
            if (item.count("namespace"))
               tc.ns = item["namespace"].getString();
            auto const initial = item.count("arguments")
                                    ? item["arguments"].getString()
                                    : std::string{};
            slots[i] = Slot{Slot::Kind::Tool, nextIndex++, initial,
                            std::move(tc)};
            auto& s = slots[i];
            domain::AssistantStreamEvent e{};
            e.type = domain::AssistantStreamType::ToolCallStart;
            e.contentIndex = s.contentIndex;
            e.toolCallId = s.tool.id; e.toolName = s.tool.tool;
            e.model = modelId; e.provider = provider;
            onEvent(e);
            if (!initial.empty())
               emit(domain::AssistantStreamType::ToolCallDelta,
                    s.contentIndex, initial);
         }
      }

      auto onItemDone(folly::dynamic const& j) -> void {
         if (!j.count("item") || !j["item"].isObject()
             || !j.count("output_index")) return;
         auto const& item = j["item"];
         auto const  i    = static_cast<int>(j["output_index"].asInt());
         auto const  t    = item.count("type") ? item["type"].getString()
                                             : std::string{};
         auto const it = slots.find(i);
         if (it == slots.end()) return;
         auto& s = it->second;
         if (t == "reasoning" && s.kind == Slot::Kind::Thinking) {
            // 终项 summary/content 优先于增量 (pi 同序覆盖)
            std::string full;
            if (item.count("summary"))
               for (auto const& p : item["summary"])
                  if (p.isObject() && p.count("type") && p.count("text")
                      && p["type"].getString() == "summary_text") {
                     if (!full.empty()) full += "\n\n";
                     full += p["text"].getString();
                  }
            if (full.empty() && item.count("content"))
               for (auto const& c : item["content"])
                  if (c.isObject() && c.count("type") && c.count("text")
                      && c["type"].getString() == "reasoning_text") {
                     if (!full.empty()) full += "\n\n";
                     full += c["text"].getString();
                  }
            if (!full.empty()) s.buf = std::move(full);
            emit(domain::AssistantStreamType::ThinkingEnd,
                 s.contentIndex, s.buf);
         } else if (t == "message" && s.kind == Slot::Kind::Text) {
            if (item.count("content") && item["content"].isArray()) {
               std::string full;
               for (auto const& c : item["content"]) {
                  auto const ct = c.isObject() && c.count("type")
                                    ? c["type"].getString()
                                    : std::string{};
                  if (ct == "output_text" && c.count("text"))
                     full += c["text"].getString();
                  else if (ct == "refusal" && c.count("refusal"))
                     full += c["refusal"].getString();
               }
               if (!full.empty()) s.buf = std::move(full);
            }
            emit(domain::AssistantStreamType::TextEnd,
                 s.contentIndex, s.buf);
         } else if ((t == "function_call" || t == "custom_tool_call")
                    && s.kind == Slot::Kind::Tool) {
            if (item.count("arguments"))
               s.buf = item["arguments"].getString();
            else if (item.count("input"))
               s.buf = item["input"].getString();
            s.tool.input = s.buf;
            domain::AssistantStreamEvent e{};
            e.type = domain::AssistantStreamType::ToolCallEnd;
            e.contentIndex = s.contentIndex;
            e.toolCall = s.tool;
            e.model = modelId; e.provider = provider;
            onEvent(e);
         }
         slots.erase(it);
      }
   };

   // ---------- 请求构造 ----------
   // pi convertToLlm: 自定义角色转 user 文本.
   static auto llmText(domain::Message const& m) -> std::string {
      switch (m.role) {
         case domain::Role::BashExecution: {
            if (m.excludeFromContext) return {};
            std::string t = "Ran `" + m.command + "`\n```\n"
                          + (m.text.empty() ? std::string{"(no output)"}
                                            : m.text)
                          + "\n```";
            if (m.cancelled)      t += "\n\n(command cancelled)";
            else if (m.exitCode != -9999 && m.exitCode != 0)
               t += "\n\nCommand exited with code "
                  + std::to_string(m.exitCode);
            if (m.truncated && !m.fullOutputPath.empty())
               t += "\n\n[Output truncated. Full output: "
                  + m.fullOutputPath + "]";
            return t;
         }
         case domain::Role::Custom: {
            if (!m.contentJson.empty()) return m.contentJson;
            return m.text;
         }
         case domain::Role::BranchSummary:
            return "The following is a summary of a branch that this "
                   "conversation came back from:\n\n<summary>\n"
                 + m.summary + "\n</summary>";
         case domain::Role::CompactionSummary:
            return "The conversation history before this point was "
                   "compacted into the following summary:\n\n<summary>\n"
                 + m.summary + "\n</summary>";
         default: return m.text;
      }
   }

   static auto httpError(infra::HttpResponse const& r) -> std::string {
      auto pj = infra::parseJsonSafe(r.body);
      if (pj && pj->count("error") && (*pj)["error"].isObject()
          && (*pj)["error"].count("message"))
         return "http " + std::to_string(r.status) + ": "
              + (*pj)["error"]["message"].getString();
      return "http " + std::to_string(r.status) + ": "
           + r.body.substr(0, 400);
   }

   auto buildBody(domain::CompletionRequest const& req,
                  std::string const&             modelId) const
      -> folly::dynamic {
      auto input = folly::dynamic::array();
      for (auto const& m : req.messages) {
         switch (m.role) {
            case domain::Role::BashExecution:
            case domain::Role::Custom:
            case domain::Role::BranchSummary:
            case domain::Role::CompactionSummary: {
               if (m.role == domain::Role::BashExecution
                   && m.excludeFromContext) break;
               auto t = llmText(m);
               if (t.empty()) break;
               input.push_back(folly::dynamic::object
                  ("type", "message")("role", "user")
                  ("content", folly::dynamic::array(
                     folly::dynamic::object("type", "input_text")
                                           ("text", std::move(t)))));
               break;
            }
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
            case domain::Role::Assistant: {
               // reasoning 块回放: signature = 原始 item JSON
               // (store:false 需要 encrypted_content 维持推理连续)
               for (auto const& th : m.thinking) {
                  if (th.signature.empty()) continue;
                  auto sig = infra::parseJsonSafe(th.signature);
                  if (sig) input.push_back(std::move(*sig));
               }
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
            }
            case domain::Role::Tool:
               input.push_back(folly::dynamic::object
                  ("type", "function_call_output")
                  ("call_id", m.toolCallId)("output", m.text));
               break;
         }
      }

      auto tools = folly::dynamic::array();
      for (auto const& t : req.tools) {
         auto pj = infra::parseJsonSafe(t.inputSchema);
         folly::dynamic params = pj
            ? std::move(*pj)
            : folly::dynamic::object("type", "object");
         tools.push_back(folly::dynamic::object
            ("type", "function")("name", t.name)
            ("description", t.description)("parameters", std::move(params))
            ("strict", false));
      }

      folly::dynamic body = folly::dynamic::object
         ("model", modelId)("input", std::move(input))
         ("tools", std::move(tools))
         ("stream", true)("store", false);

      // reasoning effort (pi openai-responses.ts): 模型支持推理时,
      // off → effort "none"; 其他级别原样下传 ("max" 收敛 "xhigh" —
      // Responses API 无 max 档), 且 include encrypted_content + auto summary.
      if (req.model.reasoning) {
         auto lv = req.thinkingLevel;
         if (lv == "max") lv = "xhigh";
         if (lv == "off") lv = "none";
         body["reasoning"] = folly::dynamic::object
            ("effort", std::move(lv))("summary", "auto");
         body["include"] = folly::dynamic::array(
            "reasoning.encrypted_content");
      }
      return body;
   }

   // ---------- 响应解析 (response.completed 的 response 对象) ----------
   static auto parseResponse(folly::dynamic const& j,
                             std::string const& modelId,
                             std::string const& provider)
      -> domain::CompletionResult {
      domain::Message msg{domain::Role::Assistant, {}, {}, {}};
      msg.api        = "openai-responses";
      msg.provider   = provider;
      msg.model      = modelId;
      if (j.count("id"))           msg.responseId    = j["id"].getString();
      if (j.count("model"))        msg.responseModel = j["model"].getString();

      if (j.count("output") && j["output"].isArray()) {
         for (auto const& item : j["output"]) {
            if (!item.isObject()) continue;
            auto type = item.count("type") ? item["type"].getString()
                                           : std::string{};
            if (type == "message" && item.count("content")
                && item["content"].isArray()) {
               for (auto const& c : item["content"]) {
                  if (c.isObject() && c.count("type")
                      && c["type"].getString() == "output_text"
                      && c.count("text"))
                     msg.text += c["text"].getString();
               }
            } else if (type == "reasoning") {
               std::string t;
               if (item.count("summary") && item["summary"].isArray())
                  for (auto const& s : item["summary"])
                     if (s.isObject() && s.count("type")
                         && s["type"].getString() == "summary_text"
                         && s.count("text"))
                        t += s["text"].getString();
               domain::ThinkingBlock b{std::move(t), {}};
               b.signature = folly::toJson(item);   // encrypted_content 回放
               msg.thinking.push_back(std::move(b));
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
      if (j.count("usage") && j["usage"].isObject()) {
         auto const& u = j["usage"];
         msg.usage.input  = u.count("input_tokens")  ? u["input_tokens"].asInt()  : 0;
         msg.usage.output = u.count("output_tokens") ? u["output_tokens"].asInt() : 0;
         msg.usage.totalTokens = u.count("total_tokens")
                                 ? u["total_tokens"].asInt()
                                 : msg.usage.input + msg.usage.output;
         if (u.count("output_tokens_details")
             && u["output_tokens_details"].isObject()) {
            auto const& d = u["output_tokens_details"];
            if (d.count("reasoning_tokens"))
               msg.usage.reasoning = d["reasoning_tokens"].asInt();
         }
         if (u.count("input_tokens_details")
             && u["input_tokens_details"].isObject()) {
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

   std::string apiKey_, baseUrl_, model_, provider_;
};

} // namespace pi::iface::adapter
