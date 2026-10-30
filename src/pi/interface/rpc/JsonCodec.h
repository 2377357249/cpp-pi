#pragma once

#include <chrono>
#include <optional>

#include <folly/json/json.h>

#include <pi/domain/llm/StopReason.h>
#include <pi/domain/message/Message.h>
#include <pi/domain/session/Session.h>
#include <pi/infra/util/JsonSafe.h>

namespace pi::iface::rpc {

// ---------------------------------------------------------------------------
// domain <-> JSON 编解码 (folly::dynamic).
// 消息形状对齐 pi packages/ai/src/types.ts 的 Message 联合类型:
//   system     {role, content, timestamp}            (会话内消息流不出现 system)
//   user       {role, content: string|(text|image)[], timestamp}
//   assistant  {role, content:(text|thinking|toolCall)[], api, provider, model,
//               responseModel?/responseId?/errorMessage?/durationMs?/endTurn?,
//               usage, stopReason, timestamp}
//   toolResult {role, toolCallId, toolName, content:(text|image)[], details?,
//               isError, durationMs?, timestamp}
// ---------------------------------------------------------------------------
inline auto nowMs() -> int64_t {
   return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline auto roleToString(domain::Role r) -> const char* {
   switch (r) {
      case domain::Role::System:            return "system";
      case domain::Role::User:              return "user";
      case domain::Role::Assistant:         return "assistant";
      case domain::Role::Tool:              return "toolResult";
      case domain::Role::BashExecution:     return "bashExecution";
      case domain::Role::Custom:            return "custom";
      case domain::Role::BranchSummary:     return "branchSummary";
      case domain::Role::CompactionSummary: return "compactionSummary";
   }
   return "user";
}

using infra::jsonOr;

// ToolCall.input 是 JSON 串, 尽量解析成内嵌对象; 解不动就原样放字符串.
inline auto inputToDynamic(std::string const& input) -> folly::dynamic {
   return jsonOr(input, folly::dynamic{input});
}

inline auto imageToJson(domain::ImageData const& im) -> folly::dynamic {
   return folly::dynamic::object("type", "image")
                                ("data", im.data)("mimeType", im.mimeType);
}

inline auto usageToJson(domain::Usage const& u) -> folly::dynamic {
   folly::dynamic d = folly::dynamic::object
      ("input", u.input)("output", u.output)
      ("cacheRead", u.cacheRead)("cacheWrite", u.cacheWrite)
      ("totalTokens", u.totalTokens);
   if (u.reasoning >= 0)    d["reasoning"]    = u.reasoning;
   if (u.cacheWrite1h >= 0) d["cacheWrite1h"] = u.cacheWrite1h;
   folly::dynamic cost = folly::dynamic::object
      ("input", u.cost.input)("output", u.cost.output)
      ("cacheRead", u.cost.cacheRead)("cacheWrite", u.cost.cacheWrite)
      ("total", u.cost.total);
   if (u.cost.totalFromProvider) cost["totalFromProvider"] = true;
   d["cost"] = std::move(cost);
   return d;
}

inline auto messageToJson(domain::Message const& m) -> folly::dynamic {
   auto ts = m.timestampMs != 0 ? m.timestampMs : nowMs();

   if (m.role == domain::Role::User) {
      if (m.images.empty()) {
         return folly::dynamic::object("role", "user")
                                      ("content", m.text)("timestamp", ts);
      }
      auto content = folly::dynamic::array();
      if (!m.text.empty())
         content.push_back(folly::dynamic::object("type", "text")
                                                ("text", m.text));
      for (auto const& im : m.images) content.push_back(imageToJson(im));
      return folly::dynamic::object("role", "user")
                                   ("content", std::move(content))
                                   ("timestamp", ts);
   }

   if (m.role == domain::Role::Tool) {
      auto content = folly::dynamic::array();
      if (!m.text.empty() || m.images.empty())
         content.push_back(folly::dynamic::object("type", "text")
                                                ("text", m.text));
      for (auto const& im : m.images) content.push_back(imageToJson(im));
      folly::dynamic d = folly::dynamic::object
         ("role", "toolResult")
         ("toolCallId", m.toolCallId)("toolName", m.toolName)
         ("content", std::move(content))
         ("isError", m.isError)("timestamp", ts);
      if (!m.detailsJson.empty()) {
         d["details"] = jsonOr(m.detailsJson, m.detailsJson);
      }
      if (m.durationMs >= 0) d["durationMs"] = m.durationMs;
      return d;
   }

   if (m.role == domain::Role::System) {
      return folly::dynamic::object("role", "system")("content", m.text)
                                    ("timestamp", ts);
   }

   // ---- 自定义角色 (pi declaration-merged roles) ----
   if (m.role == domain::Role::BashExecution) {
      folly::dynamic d = folly::dynamic::object
         ("role", "bashExecution")("command", m.command)("output", m.text)
         ("cancelled", m.cancelled)("truncated", m.truncated)
         ("timestamp", ts);
      if (m.exitCode != -9999)            d["exitCode"]         = m.exitCode;
      if (!m.fullOutputPath.empty())      d["fullOutputPath"]   = m.fullOutputPath;
      if (m.excludeFromContext)           d["excludeFromContext"] = true;
      return d;
   }
   if (m.role == domain::Role::Custom) {
      folly::dynamic d = folly::dynamic::object
         ("role", "custom")("customType", m.customType)
         ("display", m.display)("timestamp", ts);
      d["content"] = jsonOr(m.contentJson, m.contentJson);
      if (!m.detailsJson.empty()) {
         d["details"] = jsonOr(m.detailsJson, m.detailsJson);
      }
      return d;
   }
   if (m.role == domain::Role::BranchSummary) {
      folly::dynamic d = folly::dynamic::object
         ("role", "branchSummary")("summary", m.summary)("timestamp", ts);
      if (!m.fromId.empty()) d["fromId"] = m.fromId;
      else                   d["fromId"] = nullptr;
      return d;
   }
   if (m.role == domain::Role::CompactionSummary) {
      folly::dynamic d = folly::dynamic::object
         ("role", "compactionSummary")("summary", m.summary)("timestamp", ts);
      if (m.tokensBefore >= 0) d["tokensBefore"] = m.tokensBefore;
      return d;
   }

   // ---- assistant ----
   auto content = folly::dynamic::array();
   for (auto const& t : m.thinking) {
      folly::dynamic b = folly::dynamic::object
         ("type", "thinking")("thinking", t.thinking);
      if (!t.signature.empty()) b["thinkingSignature"] = t.signature;
      if (t.redacted)           b["redacted"]          = true;
      content.push_back(std::move(b));
   }
   if (!m.text.empty()) {
      folly::dynamic b = folly::dynamic::object("type", "text")
                                              ("text", m.text);
      if (!m.textSignature.empty()) b["textSignature"] = m.textSignature;
      content.push_back(std::move(b));
   }
   for (auto const& tc : m.toolCalls) {
      folly::dynamic b = folly::dynamic::object("type", "toolCall")
         ("id", tc.id)("name", tc.tool);
      if (!tc.argumentsError.empty()) {
         b["arguments"]      = folly::dynamic::object();
         b["argumentsError"] = tc.argumentsError;
      } else {
         b["arguments"] = inputToDynamic(tc.input);
      }
      if (!tc.thoughtSignature.empty())
         b["thoughtSignature"] = tc.thoughtSignature;
      if (!tc.ns.empty()) b["namespace"] = tc.ns;
      content.push_back(std::move(b));
   }

   folly::dynamic d = folly::dynamic::object
      ("role", "assistant")
      ("content", std::move(content))
      ("api",      m.api.empty()      ? "mock"   : m.api)
      ("provider", m.provider.empty() ? "mock"   : m.provider)
      ("model",    m.model.empty()    ? "mock-1" : m.model)
      ("usage", usageToJson(m.usage))
      ("stopReason", domain::toWire(m.stopReason == domain::StopReason::Pending
                                       ? domain::stopReasonOf(m)
                                       : m.stopReason))
      ("timestamp", ts);
   if (!m.responseModel.empty())  d["responseModel"]  = m.responseModel;
   if (!m.responseId.empty())     d["responseId"]     = m.responseId;
   if (!m.errorMessage.empty())   d["errorMessage"]   = m.errorMessage;
   if (m.durationMs >= 0)         d["durationMs"]     = m.durationMs;
   if (m.hasEndTurn)              d["endTurn"]        = m.endTurn;
   return d;
}

inline auto messagesToJson(domain::Session const& s) -> folly::dynamic {
   auto arr = folly::dynamic::array();
   for (auto const& m : s.messages()) arr.push_back(messageToJson(m));
   return arr;
}

// ---------------------------------------------------------------------------
// SessionEntry -> wire. {type,id,parentId:null|string,timestamp:ISO} + 子类型字段.
// parentId 空 = null (JSON.stringify 语义).
// ---------------------------------------------------------------------------
inline auto entryToJson(domain::Entry const& e) -> folly::dynamic {
   folly::dynamic d = folly::dynamic::object("id", e.id)("timestamp", e.timestampIso);
   d["parentId"] = e.parentId.empty() ? folly::dynamic(nullptr)
                                    : folly::dynamic(e.parentId);
   d["type"]     = domain::entryType(e);

   switch (e.payload.index()) {
      case 0:
         d["message"] = messageToJson(
            std::get<domain::MessageEntry>(e.payload).message);
         break;
      case 1:
         d["thinkingLevel"] =
            std::get<domain::ThinkingLevelEntry>(e.payload).thinkingLevel;
         break;
      case 2: {
         auto const& p = std::get<domain::ModelChangeEntry>(e.payload);
         d["provider"] = p.provider; d["modelId"] = p.modelId;
         break;
      }
      case 3: {
         auto const& p = std::get<domain::UsageRecordEntry>(e.payload);
         d["kind"] = p.kind; d["provider"] = p.provider;
         d["model"] = p.model; d["usage"] = usageToJson(p.usage);
         if (!p.note.empty()) d["note"] = p.note;
         break;
      }
      case 4: {
         auto const& p = std::get<domain::CompactionEntry>(e.payload);
         d["summary"] = p.summary;
         d["firstKeptEntryId"] = p.firstKeptEntryId.empty()
            ? folly::dynamic(nullptr) : folly::dynamic(p.firstKeptEntryId);
         d["tokensBefore"] = p.tokensBefore;
         if (!p.detailsJson.empty())
            d["details"] = jsonOr(p.detailsJson, p.detailsJson);
         if (p.hasUsage)         d["usage"] = usageToJson(p.usage);
         if (p.fromHook)         d["fromHook"] = true;
         if (p.hasSystemMessage) d["systemMessage"] = messageToJson(p.systemMessage);
         break;
      }
      case 5: {
         auto const& p = std::get<domain::BranchSummaryEntry>(e.payload);
         d["summary"] = p.summary;
         d["fromId"]  = p.fromId.empty() ? folly::dynamic(nullptr)
                                       : folly::dynamic(p.fromId);
         if (!p.detailsJson.empty())
            d["details"] = jsonOr(p.detailsJson, p.detailsJson);
         if (p.hasUsage) d["usage"] = usageToJson(p.usage);
         if (p.fromHook) d["fromHook"] = true;
         break;
      }
      case 6: {
         auto const& p = std::get<domain::CustomEntry>(e.payload);
         d["customType"] = p.customType;
         if (!p.dataJson.empty()) d["data"] = jsonOr(p.dataJson, p.dataJson);
         break;
      }
      case 7: {
         auto const& p = std::get<domain::LabelEntry>(e.payload);
         d["targetId"] = p.targetId;
         if (!p.clear) d["label"] = p.label;
         break;
      }
      case 8: {
         auto const& p = std::get<domain::SessionInfoEntry>(e.payload);
         d["name"] = p.hasName ? folly::dynamic(p.name)
                               : folly::dynamic(nullptr);
         break;
      }
      case 9: {
         auto const& p = std::get<domain::CustomMessageEntry>(e.payload);
         d["customType"] = p.customType;
         d["display"]    = p.display;
         d["content"]    = jsonOr(p.contentJson, p.contentJson);
         if (!p.detailsJson.empty())
            d["details"] = jsonOr(p.detailsJson, p.detailsJson);
         break;
      }
      case 10: {
         auto const& p = std::get<domain::ContextEditEntry>(e.payload);
         d["targetId"] = p.targetId;
         d["replacement"] = p.nullReplacement
            ? folly::dynamic(nullptr)
            : jsonOr(p.replacementJson, nullptr);
         break;
      }
   }
   return d;
}

// JSONL -> Entry (switch_session / fork 加载用). 字段缺失/类型不符 -> nullopt
// 或按缺省值走, 全程不抛.
namespace detail {
   inline auto strOr(folly::dynamic const& j, char const* k,
                     std::string_view def = {}) -> std::string {
      auto it = j.find(k);
      return it != j.items().end() && it->second.isString()
                ? it->second.getString() : std::string{def};
   }
   inline auto boolOr(folly::dynamic const& j, char const* k, bool def = false)
      -> bool {
      auto it = j.find(k);
      return it != j.items().end() && it->second.isBool()
                ? it->second.getBool() : def;
   }
   inline auto intOr(folly::dynamic const& j, char const* k, int64_t def = 0)
      -> int64_t {
      auto it = j.find(k);
      return it != j.items().end() && it->second.isInt()
                ? it->second.getInt() : def;
   }
} // namespace detail

inline auto entryFromJson(folly::dynamic const& j) -> std::optional<domain::Entry> {
   if (!j.isObject()) return std::nullopt;
   auto const type = detail::strOr(j, "type");
   auto const id   = detail::strOr(j, "id");
   if (type.empty() || id.empty()) return std::nullopt;

   domain::Entry e;
   e.id           = id;
   if (auto it = j.find("parentId");
       it != j.items().end() && it->second.isString())
      e.parentId  = it->second.getString();
   e.timestampIso = detail::strOr(j, "timestamp");

   if (type == "message") {
      auto it = j.find("message");
      if (it == j.items().end() || !it->second.isObject()) return std::nullopt;
      auto const& mj = it->second;
      domain::Message m;
      if (auto c = mj.find("content"); c != mj.items().end()) {
         if (c->second.isString())    m.text = c->second.getString();
         else if (c->second.isArray())
            m.text = folly::toJson(c->second);   // 块内容原样回灌
      }
      auto const role = detail::strOr(mj, "role", "user");
      if      (role == "user")              m.role = domain::Role::User;
      else if (role == "assistant")         m.role = domain::Role::Assistant;
      else if (role == "toolResult")        m.role = domain::Role::Tool;
      else if (role == "system")            m.role = domain::Role::System;
      else if (role == "bashExecution")     m.role = domain::Role::BashExecution;
      else if (role == "custom")            m.role = domain::Role::Custom;
      else if (role == "branchSummary")     m.role = domain::Role::BranchSummary;
      else if (role == "compactionSummary") m.role = domain::Role::CompactionSummary;
      else return std::nullopt;
      m.toolCallId  = detail::strOr(mj, "toolCallId");
      m.toolName    = detail::strOr(mj, "toolName");
      m.isError     = detail::boolOr(mj, "isError");
      m.timestampMs = detail::intOr(mj, "timestamp");
      e.payload = domain::MessageEntry{std::move(m)};
   } else if (type == "thinking_level_change") {
      e.payload = domain::ThinkingLevelEntry{
         detail::strOr(j, "thinkingLevel")};
   } else if (type == "model_change") {
      e.payload = domain::ModelChangeEntry{
         detail::strOr(j, "provider"), detail::strOr(j, "modelId")};
   } else if (type == "usage") {
      e.payload = domain::UsageRecordEntry{
         detail::strOr(j, "kind"), detail::strOr(j, "provider"),
         detail::strOr(j, "model"), domain::Usage{},
         detail::strOr(j, "note")};
   } else if (type == "compaction") {
      domain::CompactionEntry p;
      p.summary          = detail::strOr(j, "summary");
      p.firstKeptEntryId = detail::strOr(j, "firstKeptEntryId");
      p.tokensBefore     = detail::intOr(j, "tokensBefore");
      e.payload = std::move(p);
   } else if (type == "branch_summary") {
      e.payload = domain::BranchSummaryEntry{
         detail::strOr(j, "fromId"), detail::strOr(j, "summary")};
   } else if (type == "custom") {
      e.payload = domain::CustomEntry{detail::strOr(j, "customType")};
   } else if (type == "label") {
      domain::LabelEntry p;
      p.targetId = detail::strOr(j, "targetId");
      p.label    = detail::strOr(j, "label");
      p.clear    = !j.count("label") || j.at("label").isNull();
      e.payload = std::move(p);
   } else if (type == "session_info") {
      domain::SessionInfoEntry p;
      p.hasName = j.count("name") && j.at("name").isString();
      p.name    = detail::strOr(j, "name");
      e.payload = std::move(p);
   } else if (type == "custom_message") {
      domain::CustomMessageEntry p;
      p.customType = detail::strOr(j, "customType");
      p.display    = detail::boolOr(j, "display");
      if (auto it = j.find("content"); it != j.items().end())
         p.contentJson = folly::toJson(it->second);
      e.payload = std::move(p);
   } else if (type == "context_edit") {
      domain::ContextEditEntry p;
      p.targetId = detail::strOr(j, "targetId");
      p.nullReplacement = false;
      if (auto it = j.find("replacement"); it != j.items().end()) {
         if (it->second.isNull()) p.nullReplacement = true;
         else                     p.replacementJson = folly::toJson(it->second);
      }
      e.payload = std::move(p);
   } else {
      return std::nullopt;   // 未知子类型跳过 (坏行容忍)
   }
   return e;
}

} // namespace pi::iface::rpc
