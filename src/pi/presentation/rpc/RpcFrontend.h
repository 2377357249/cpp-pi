#pragma once

#include <array>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <sys/wait.h>

#include <folly/json/json.h>

#include <pi/application/AgentSessionService.h>
#include <pi/domain/session/Session.h>
#include <pi/presentation/rpc/JsonCodec.h>

namespace pi::rpc {

// ---------------------------------------------------------------------------
// RPC 前端: 与 pi TS TUI 对接的 NDJSON 协议端.
// stdin 收 RpcCommand(JSON 行) -> stdout 出 response + AgentEvent 行.
// 协议面对齐 pi packages/coding-agent/src/modes/rpc/rpc-types.ts:
//   命令帧 {id?, type, ...}; 响应帧 {id?, type:"response", command, success,
//   data|error}; 事件帧即 AgentEvent
//   (agent_start/turn_start/message_start/message_end/turn_end/agent_end).
// 骨架阶段事件按 run 粒度合成: 事务跑完后按新增消息补发 message_*.
// ---------------------------------------------------------------------------
class RpcFrontend {
public:
   RpcFrontend(app::AgentSessionService& service,
               domain::Session&          session,
               std::istream&             in  = std::cin,
               std::ostream&             out = std::cout)
      : service_(service), session_(session), in_(in), out_(out) {}

   int run() {
      for (std::string line; std::getline(in_, line);) {
         if (line.empty()) continue;

         std::optional<folly::dynamic> cmd;
         try {
            cmd = folly::parseJson(line);
         } catch (std::exception const& e) {
            emit(error_(folly::dynamic{}, "unknown",
                        std::string{"invalid json: "} + e.what()));
            continue;
         }

         auto resp = handle(*cmd);
         if (!resp.isNull()) emit(resp);   // prompt 的响应在 handle 内已发
      }
      return 0;
   }

private:
   // ---------- 协议帧 ----------
   auto emit(folly::dynamic const& obj) -> void {
      out_ << folly::toJson(obj) << '\n' << std::flush;
   }

   static auto idOf(folly::dynamic const& cmd) -> folly::dynamic {
      return cmd.count("id") ? cmd["id"] : folly::dynamic{};
   }

   static auto ok_(folly::dynamic const& id, std::string_view command,
                   folly::dynamic data = folly::dynamic::object()) -> folly::dynamic {
      folly::dynamic r = folly::dynamic::object("type", "response")
                                              ("command", std::string{command})
                                              ("success", true);
      if (id.isString()) r["id"] = id;
      if (!data.empty()) r["data"] = std::move(data);
      return r;
   }

   static auto error_(folly::dynamic const& id, std::string_view command,
                      std::string_view message) -> folly::dynamic {
      folly::dynamic r = folly::dynamic::object("type", "response")
                                              ("command", std::string{command})
                                              ("success", false)
                                              ("error", std::string{message});
      if (id.isString()) r["id"] = id;
      return r;
   }

   // ---------- 队列 ----------
   auto emitQueueUpdate() -> void {
      auto steering = folly::dynamic::array();
      auto followUp = folly::dynamic::array();
      for (auto const& s : steering_)  steering.push_back(s);
      for (auto const& s : followUp_)  followUp.push_back(s);
      emit(folly::dynamic::object("type", "queue_update")
                                 ("steering", std::move(steering))
                                 ("followUp", std::move(followUp)));
   }

   // run 结束后排干队列: steering 先于 followUp (pi 语义: steer 在两轮
   // 迭代间注入, followUp 在 agent 完全空闲后才处理).
   auto drainQueues() -> void {
      while (!steering_.empty() || !followUp_.empty()) {
         std::string msg;
         if (!steering_.empty()) {
            msg = std::move(steering_.front());
            steering_.pop_front();
         } else {
            msg = std::move(followUp_.front());
            followUp_.pop_front();
         }
         emitQueueUpdate();
         service_.runTurn(msg);
         emitTurnEvents();
      }
   }

   // ---------- AgentEvent 流 ----------
   // pi 语义: agent_start/turn_start 包裹一轮 run; 每条新消息一对
   // message_start/message_end; turn_end 带 assistant message + toolResults;
   // agent_end 带本 run 全部新消息.
   auto emitTurnEvents() -> void {
      auto const& msgs = session_.messages();
      if (msgs.size() <= emittedCount_) return;

      auto newMsgs = folly::dynamic::array();
      for (auto i = emittedCount_; i < msgs.size(); ++i)
         newMsgs.push_back(messageToJson(msgs[i]));

      emit(folly::dynamic::object("type", "agent_start"));
      emit(folly::dynamic::object("type", "turn_start"));

      auto toolResults = folly::dynamic::array();
      folly::dynamic    lastAssistant;
      for (auto i = emittedCount_; i < msgs.size(); ++i) {
         auto mj = messageToJson(msgs[i]);
         emit(folly::dynamic::object("type", "message_start")
                                    ("message", mj));
         emit(folly::dynamic::object("type", "message_end")
                                    ("message", mj));
         if (msgs[i].role == domain::Role::Tool) toolResults.push_back(mj);
         if (msgs[i].role == domain::Role::Assistant) lastAssistant = mj;
      }
      emittedCount_ = msgs.size();

      emit(folly::dynamic::object("type", "turn_end")
                                 ("message", lastAssistant.isNull()
                                                ? folly::dynamic::object()
                                                : std::move(lastAssistant))
                                 ("toolResults", std::move(toolResults)));
      emit(folly::dynamic::object("type", "agent_end")
                                 ("messages", std::move(newMsgs)));
   }

   // ---------- 命令分发 ----------
   auto handle(folly::dynamic const& cmd) -> folly::dynamic {
      auto id   = idOf(cmd);
      auto type = cmd.count("type") && cmd["type"].isString()
                    ? cmd["type"].getString() : std::string{};

      if (type == "prompt") {
         auto msg = cmd.count("message") && cmd["message"].isString()
                      ? cmd["message"].getString() : std::string{};
         // pi 语义: preflight 通过即回 disposition=started, 事件随后流出
         emit(ok_(id, "prompt",
                  folly::dynamic::object("disposition", "started")));
         service_.runTurn(msg);
         emitTurnEvents();
         drainQueues();
         return folly::dynamic{};   // 响应已发
      }
      if (type == "steer" || type == "follow_up") {
         auto msg = cmd.count("message") && cmd["message"].isString()
                      ? cmd["message"].getString() : std::string{};
         (type == "steer" ? steering_ : followUp_).push_back(msg);
         emitQueueUpdate();
         return ok_(id, type,
                    folly::dynamic::object("disposition", "queued"));
      }
      if (type == "get_state") {
         return ok_(id, "get_state", sessionStateJson());
      }
      if (type == "get_messages") {
         return ok_(id, "get_messages",
                    folly::dynamic::object("messages", messagesToJson(session_)));
      }
      if (type == "get_last_assistant_text") {
         auto const& msgs = session_.messages();
         folly::dynamic text;
         for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->role == domain::Role::Assistant && !it->text.empty()) {
               text = it->text;
               break;
            }
         }
         return ok_(id, "get_last_assistant_text",
                    folly::dynamic::object("text", text));
      }
      if (type == "get_session_stats") {
         return ok_(id, "get_session_stats", sessionStatsJson());
      }
      if (type == "get_commands") {
         return ok_(id, "get_commands",
                    folly::dynamic::object("commands", folly::dynamic::array()));
      }
      if (type == "get_available_models") {
         return ok_(id, "get_available_models",
                    folly::dynamic::object("models",
                       folly::dynamic::array(modelJson())));
      }
      if (type == "get_available_thinking_levels") {
         return ok_(id, "get_available_thinking_levels",
                    folly::dynamic::object("levels",
                       folly::dynamic::array("off", "minimal", "low",
                                             "medium", "high", "xhigh")));
      }
      if (type == "set_model") {
         modelProvider_ = cmd.count("provider") ? cmd["provider"].getString()
                                                : modelProvider_;
         modelId_       = cmd.count("modelId")  ? cmd["modelId"].getString()
                                                : modelId_;
         return ok_(id, "set_model");
      }
      if (type == "cycle_model") {
         return ok_(id, "cycle_model",
                    folly::dynamic::object("model", modelJson()));
      }
      if (type == "set_thinking_level") {
         if (cmd.count("level") && cmd["level"].isString())
            thinkingLevel_ = cmd["level"].getString();
         return ok_(id, "set_thinking_level");
      }
      if (type == "cycle_thinking_level") {
         static char const* kLevels[] = {"off", "minimal", "low",
                                         "medium", "high", "xhigh"};
         for (auto i = 0; i < 6; ++i) {
            if (thinkingLevel_ == kLevels[i]) {
               thinkingLevel_ = kLevels[(i + 1) % 6];
               break;
            }
         }
         return ok_(id, "cycle_thinking_level",
                    folly::dynamic::object("level", thinkingLevel_));
      }
      if (type == "set_steering_mode") {
         if (cmd.count("mode") && cmd["mode"].isString())
            steeringMode_ = cmd["mode"].getString();
         return ok_(id, "set_steering_mode");
      }
      if (type == "set_follow_up_mode") {
         if (cmd.count("mode") && cmd["mode"].isString())
            followUpMode_ = cmd["mode"].getString();
         return ok_(id, "set_follow_up_mode");
      }
      if (type == "set_auto_compaction") {
         autoCompactionEnabled_ =
            !cmd.count("enabled") || cmd["enabled"].getBool();
         return ok_(id, "set_auto_compaction");
      }
      if (type == "set_auto_retry") {
         autoRetryEnabled_ = !cmd.count("enabled") || cmd["enabled"].getBool();
         return ok_(id, "set_auto_retry");
      }
      if (type == "set_session_name") {
         if (cmd.count("name") && cmd["name"].isString())
            sessionName_ = cmd["name"].getString();
         return ok_(id, "set_session_name");
      }
      if (type == "new_session") {
         session_.reset();
         emittedCount_ = 0;
         return ok_(id, "new_session",
                    folly::dynamic::object("cancelled", false));
      }
      if (type == "abort" || type == "abort_bash" || type == "abort_retry") {
         return ok_(id, type);   // EV_ABORT 链路待接
      }
      if (type == "clear_queue") {
         auto steering = folly::dynamic::array();
         auto followUp = folly::dynamic::array();
         for (auto const& s : steering_)  steering.push_back(s);
         for (auto const& s : followUp_) followUp.push_back(s);
         steering_.clear();
         followUp_.clear();
         emitQueueUpdate();
         return ok_(id, "clear_queue",
                    folly::dynamic::object("steering", std::move(steering))
                                          ("followUp", std::move(followUp)));
      }
      if (type == "bash") {
         auto cmdStr = cmd.count("command") && cmd["command"].isString()
                         ? cmd["command"].getString() : std::string{};
         auto r = runBash(cmdStr);
         return ok_(id, "bash",
                    folly::dynamic::object("output", r.output)
                       ("exitCode", r.exitCode)("cancelled", false)
                       ("truncated", r.truncated));
      }
      if (type == "compact") {
         // 骨架: 不改写历史, 只回报指标. 真 compaction 走 LLM 摘要事务.
         auto n = static_cast<int64_t>(session_.messages().size());
         return ok_(id, "compact",
                    folly::dynamic::object
                       ("summary", "mock compaction of " +
                                      std::to_string(n) + " messages")
                       ("firstKeptEntryId", folly::dynamic{})
                       ("tokensBefore", n));
      }
      if (type == "export_html") {
         auto path = cmd.count("outputPath") && cmd["outputPath"].isString()
                       ? cmd["outputPath"].getString()
                       : session_.id() + ".html";
         exportHtml(path);
         return ok_(id, "export_html",
                    folly::dynamic::object("path", path));
      }
      if (type == "get_entries") {
         auto data = entriesJson(cmd);
         if (data.count("__error"))
            return error_(id, "get_entries", data["__error"].getString());
         return ok_(id, "get_entries", std::move(data));
      }
      if (type == "get_tree") {
         return ok_(id, "get_tree", treeJson());
      }
      return error_(id, type.empty() ? "unknown" : type,
                    "unsupported command");
   }

   // ---------- bash ----------
   struct BashOut { std::string output; int exitCode; bool truncated; };
   static auto runBash(std::string const& cmd) -> BashOut {
      std::string out;
      std::array<char, 4096> buf{};
      bool truncated = false;
      FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
      if (pipe) {
         while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            out += buf.data();
            if (out.size() > 8192) { out.resize(8192); truncated = true; break; }
         }
         auto rc = ::pclose(pipe);
         return {std::move(out), WEXITSTATUS(rc), truncated};
      }
      return {"popen failed", 127, false};
   }

   // ---------- export_html ----------
   auto exportHtml(std::string const& path) const -> void {
      std::ofstream f{path};
      f << "<!doctype html><meta charset=utf-8><title>"
        << session_.id() << "</title><pre>\n";
      for (auto const& m : session_.messages()) {
         f << "[" << roleToString(m.role) << "] " << m.text << "\n";
      }
   }

   // ---------- entries / tree ----------
   // 骨架: entry id 按消息下标合成 e-<n>, parentId 链式. 真实现里
   // entry 是 session store 的持久化记录 (uuid).
   static auto entryId(size_t i) -> std::string {
      return "e-" + std::to_string(i);
   }

   auto entriesJson(folly::dynamic const& cmd) const -> folly::dynamic {
      auto const& msgs = session_.messages();
      size_t begin = 0;
      if (cmd.count("since") && cmd["since"].isString()) {
         auto const& since = cmd["since"].getString();
         bool found = false;
         for (size_t i = 0; i < msgs.size(); ++i) {
            if (entryId(i) == since) { begin = i + 1; found = true; break; }
         }
         if (!found) {
            // pi 行为: 未知 since id 是错误响应. 调用方需要 error 帧——
            // 包一层 sentinel 让 handle() 转成 error.
            return folly::dynamic::object("__error", "Entry not found");
         }
      }
      auto entries = folly::dynamic::array();
      for (size_t i = begin; i < msgs.size(); ++i) {
         folly::dynamic e = folly::dynamic::object
            ("type", "message")
            ("id", entryId(i))
            ("parentId", i == 0 ? folly::dynamic{} : folly::dynamic{entryId(i - 1)})
            ("timestamp", "")
            ("message", messageToJson(msgs[i]));
         entries.push_back(std::move(e));
      }
      folly::dynamic leaf;
      if (!msgs.empty()) leaf = entryId(msgs.size() - 1);
      return folly::dynamic::object("entries", std::move(entries))
                                  ("leafId", leaf);
   }

   auto treeJson() const -> folly::dynamic {
      // 线性链 -> 单根树. SessionTreeNode: {entry, children[]}
      auto const& msgs = session_.messages();
      folly::dynamic leaf;
      if (!msgs.empty()) leaf = entryId(msgs.size() - 1);
      auto tree = folly::dynamic::array();
      if (!msgs.empty()) {
         // 自底向上建链
         folly::dynamic node;
         for (size_t i = msgs.size(); i-- > 0;) {
            folly::dynamic e = folly::dynamic::object
               ("type", "message")("id", entryId(i))
               ("parentId", i == 0 ? folly::dynamic{}
                                  : folly::dynamic{entryId(i - 1)})
               ("timestamp", "")
               ("message", messageToJson(msgs[i]));
            folly::dynamic n = folly::dynamic::object("entry", std::move(e));
            n["children"] = node.isNull() ? folly::dynamic::array()
                                         : folly::dynamic::array(std::move(node));
            node = std::move(n);
         }
         tree.push_back(std::move(node));
      }
      return folly::dynamic::object("tree", std::move(tree))
                                  ("leafId", leaf);
   }

   // ---------- 状态序列化 ----------
   auto modelJson() const -> folly::dynamic {
      return folly::dynamic::object
         ("id", modelId_)("name", modelId_)
         ("api", "mock")("provider", modelProvider_)
         ("baseUrl", "")("reasoning", false)
         ("input", folly::dynamic::array("text"))
         ("cost", folly::dynamic::object
            ("input", 0)("output", 0)("cacheRead", 0)("cacheWrite", 0))
         ("contextWindow", 200000)("maxTokens", 8192);
   }

   auto sessionStateJson() const -> folly::dynamic {
      folly::dynamic d = folly::dynamic::object
         ("model", modelJson())
         ("thinkingLevel", thinkingLevel_)
         ("isStreaming", false)("isCompacting", false)
         ("steeringMode", steeringMode_)("followUpMode", followUpMode_)
         ("sessionId", session_.id())
         ("autoCompactionEnabled", autoCompactionEnabled_)
         ("messageCount", static_cast<int64_t>(session_.messages().size()))
         ("pendingMessageCount", 0);
      if (!sessionName_.empty()) d["sessionName"] = sessionName_;
      return d;
   }

   auto sessionStatsJson() const -> folly::dynamic {
      int64_t user = 0, assistant = 0, toolCalls = 0, toolResults = 0;
      for (auto const& m : session_.messages()) {
         switch (m.role) {
            case domain::Role::User:      ++user; break;
            case domain::Role::Assistant: ++assistant;
               toolCalls += static_cast<int64_t>(m.toolCalls.size()); break;
            case domain::Role::Tool:      ++toolResults; break;
         }
      }
      auto tokens = folly::dynamic::object
         ("input", 0)("output", 0)("cacheRead", 0)("cacheWrite", 0)("total", 0);
      folly::dynamic d = folly::dynamic::object
         ("sessionFile", session_.id() + ".jsonl")   // 骨架: 声明式路径
         ("sessionId", session_.id())
         ("userMessages", user)("assistantMessages", assistant)
         ("toolCalls", toolCalls)("toolResults", toolResults)
         ("totalMessages", static_cast<int64_t>(session_.messages().size()))
         ("tokens", std::move(tokens))("cost", 0);
      if (!sessionName_.empty()) d["sessionName"] = sessionName_;
      return d;
   }

   app::AgentSessionService& service_;
   domain::Session&          session_;
   std::istream&             in_;
   std::ostream&             out_;

   // RPC 会话状态骨架 (pi AgentSession 对应物)
   std::string modelProvider_  = "mock";
   std::string modelId_        = "mock-1";
   std::string thinkingLevel_  = "off";
   std::string steeringMode_   = "all";
   std::string followUpMode_   = "all";
   std::string sessionName_;
   bool        autoCompactionEnabled_ = false;
   bool        autoRetryEnabled_      = true;
   size_t      emittedCount_          = 0;
   std::deque<std::string> steering_;
   std::deque<std::string> followUp_;
};

} // namespace pi::rpc
