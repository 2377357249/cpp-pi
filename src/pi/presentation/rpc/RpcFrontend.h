#pragma once

#include <iostream>
#include <optional>
#include <string>

#include <folly/json/json.h>

#include <pi/application/AgentSessionService.h>
#include <pi/domain/session/Session.h>
#include <pi/presentation/rpc/JsonCodec.h>

namespace pi::rpc {

// ---------------------------------------------------------------------------
// RPC 前端: 与 pi TS TUI 对接的 NDJSON 协议端.
// stdin 收 RpcCommand(JSON 行) -> stdout 出 response + AgentSessionEvent 行.
// 协议面以 pi packages/coding-agent/src/modes/rpc/rpc-types.ts 为准.
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
            emit(error_(folly::dynamic{}, "parse",
                        std::string{"invalid json: "} + e.what()));
            continue;
         }

         emit(handle(*cmd));
      }
      return 0;
   }

private:
   // ---------- 协议帧 ----------
   auto emit(folly::dynamic const& obj) -> void {
      out_ << folly::toJson(obj) << '\n' << std::flush;
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

   // ---------- 会话事件 (骨架: 按 turn 粒度发, 流式增量接 provider 后再细分) ----------
   auto emitTurnEvents() -> void {
      auto const& msgs = session_.messages();
      if (msgs.empty()) return;
      emit(folly::dynamic::object("type", "turn_start"));
      emit(folly::dynamic::object("type", "message_end")
                                 ("message", messageToJson(msgs.back())));
      emit(folly::dynamic::object("type", "turn_end"));
      emit(folly::dynamic::object("type", "agent_settled")("aborted", false));
   }

   // ---------- 命令分发 ----------
   auto handle(folly::dynamic const& cmd) -> folly::dynamic {
      auto id   = cmd.count("id") ? cmd["id"] : folly::dynamic{};
      auto type = cmd.count("type") && cmd["type"].isString() ? cmd["type"].getString() : "";

      if (type == "prompt") {
         auto msg = cmd.count("message") ? cmd["message"].getString() : "";
         service_.runTurn(std::move(msg));
         emitTurnEvents();
         return ok_(id, "prompt",
                    folly::dynamic::object("disposition", "prompt_handled"));
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
         auto text = msgs.empty() ? "" : msgs.back().text;
         return ok_(id, "get_last_assistant_text",
                    folly::dynamic::object("text", text));
      }
      if (type == "abort") {
         return ok_(id, "abort");   // EV_ABORT 链路待接
      }
      if (type == "new_session") {
         return ok_(id, "new_session",
                    folly::dynamic::object("cancelled", false));
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
                    folly::dynamic::object("models", folly::dynamic::array()));
      }
      if (type == "get_available_thinking_levels") {
         return ok_(id, "get_available_thinking_levels",
                    folly::dynamic::object("levels", folly::dynamic::array()));
      }
      // 状态类 setter 骨架: 先应答成功, 语义后续接入
      if (type == "set_model" || type == "cycle_model" ||
          type == "set_thinking_level" || type == "cycle_thinking_level" ||
          type == "set_steering_mode" || type == "set_follow_up_mode" ||
          type == "set_auto_compaction" || type == "set_auto_retry" ||
          type == "set_session_name") {
         return ok_(id, type);
      }
      return error_(id, type.empty() ? "unknown" : type,
                    "unsupported command");
   }

   auto sessionStateJson() -> folly::dynamic {
      folly::dynamic d = folly::dynamic::object
         ("thinkingLevel", "off")
         ("isStreaming", false)("isCompacting", false)
         ("steeringMode", "all")("followUpMode", "all")
         ("sessionId", session_.id())
         ("autoCompactionEnabled", false)
         ("messageCount", static_cast<int64_t>(session_.messages().size()))
         ("pendingMessageCount", 0);
      return d;
   }

   auto sessionStatsJson() -> folly::dynamic {
      folly::dynamic d = folly::dynamic::object
         ("sessionId", session_.id())
         ("messageCount", static_cast<int64_t>(session_.messages().size()));
      return d;
   }

   app::AgentSessionService& service_;
   domain::Session&          session_;
   std::istream&             in_;
   std::ostream&             out_;
};

} // namespace pi::rpc
