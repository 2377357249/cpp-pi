#pragma once

#include <algorithm>
#include <cctype>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/model/Model.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/domain/session/Session.h>
#include <pi/infra/process/Shell.h>
#include <pi/infra/util/Ids.h>
#include <pi/infra/util/JsonSafe.h>
#include <pi/interface/rpc/JsonCodec.h>
#include <pi/interface/trans/AgentSessionService.h>

namespace pi::iface::rpc {

// ---------------------------------------------------------------------------
// RPC 前端: 与 pi TS TUI 对接的 NDJSON 协议端 (rpc-types.ts 1:1).
//   命令帧 {id?, type, ...}; 响应帧 {id?, type:"response", command, success,
//   data|error}; 事件帧即 AgentSessionEvent.
// 同步执行模型: 事件按序合成 (流式/并发在后续批次接 EV_* 链路).
// ---------------------------------------------------------------------------
class RpcFrontend {
public:
   static constexpr std::array kThinkingLevels{
      "off", "minimal", "low", "medium", "high", "xhigh", "max"};

   RpcFrontend(AgentSessionService&   service,
               domain::Session&       session,
               domain::ISessionStore* store = nullptr,
               std::istream&          in  = std::cin,
               std::ostream&          out = std::cout)
      : service_(service), session_(session), store_(store),
        in_(in), out_(out) {}

   auto setModel(domain::Model m) -> void { model_ = std::move(m); }

   int run() {
      for (std::string line; std::getline(in_, line);) {
         if (line.empty()) continue;
         if (!line.empty() && line.back() == '\r') line.pop_back();

         auto parsed = infra::parseJsonSafe(line);
         if (parsed.hasError()) {
            emit(error_(folly::dynamic{}, "parse",
                        std::format("Failed to parse command: {}",
                                    parsed.error())));
            continue;
         }
         auto resp = handle(*parsed);
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
      auto it = cmd.find("id");
      return it != cmd.items().end() ? it->second : folly::dynamic{};
   }

   static auto ok_(folly::dynamic const& id, std::string_view command,
                   folly::dynamic data = folly::dynamic::object())
      -> folly::dynamic {
      folly::dynamic r = folly::dynamic::object("type", "response")
                                              ("command", std::string{command})
                                              ("success", true);
      if (id.isString()) r["id"] = id;
      if (!data.empty()) r["data"] = std::move(data);
      return r;
   }

   // 显式 data (允许 null): cycle_*/≤1 候选返回 data:null.
   static auto okData_(folly::dynamic const& id, std::string_view command,
                       folly::dynamic data) -> folly::dynamic {
      folly::dynamic r = folly::dynamic::object("type", "response")
                                              ("command", std::string{command})
                                              ("success", true);
      if (id.isString()) r["id"] = id;
      r["data"] = std::move(data);
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

   // run 结束后排干队列: steering 先于 followUp.
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
   auto emitTurnEvents() -> void {
      auto const msgs = session_.messages();
      if (msgs.size() <= emittedCount_) return;

      auto newMsgs = folly::dynamic::array();
      for (auto const& m : msgs | std::views::drop(emittedCount_))
         newMsgs.push_back(messageToJson(m));

      emit(folly::dynamic::object("type", "agent_start"));
      emit(folly::dynamic::object("type", "turn_start"));

      auto toolResults = folly::dynamic::array();
      folly::dynamic    lastAssistant;
      for (auto const& m : msgs | std::views::drop(emittedCount_)) {
         auto mj = messageToJson(m);
         emit(folly::dynamic::object("type", "message_start")
                                    ("message", mj));
         emit(folly::dynamic::object("type", "message_end")
                                    ("message", mj));
         if (m.role == domain::Role::Tool) toolResults.push_back(mj);
         if (m.role == domain::Role::Assistant) lastAssistant = mj;
      }
      emittedCount_ = msgs.size();

      emit(folly::dynamic::object("type", "turn_end")
            ("message", lastAssistant.isNull() ? folly::dynamic::object()
                                               : std::move(lastAssistant))
            ("toolResults", std::move(toolResults)));
      emit(folly::dynamic::object("type", "agent_end")
            ("messages", std::move(newMsgs))("willRetry", false));
   }

   // ---------- 命令分发 ----------
   auto handle(folly::dynamic const& cmd) -> folly::dynamic {
      auto const id   = idOf(cmd);
      auto const type = strField(cmd, "type");

      // pi: cmd.type 缺失 -> command 键省略, error "Unknown command: undefined"
      if (type.empty()) {
         folly::dynamic r = folly::dynamic::object
            ("type", "response")("success", false)
            ("error", "Unknown command: undefined");
         if (id.isString()) r["id"] = id;
         return r;
      }

      if (type == "prompt") {
         emit(ok_(id, "prompt",
                  folly::dynamic::object("disposition", "started")));
         service_.runTurn(strField(cmd, "message"));
         emitTurnEvents();
         drainQueues();
         return folly::dynamic{};
      }
      if (type == "steer" || type == "follow_up") {
         (type == "steer" ? steering_ : followUp_)
            .push_back(strField(cmd, "message"));
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
         folly::dynamic text;
         for (auto const& m : session_.messages() | std::views::reverse) {
            // pi: 跳过 aborted && 空 content
            if (m.role != domain::Role::Assistant) continue;
            if (m.stopReason == domain::StopReason::Aborted
                && m.text.empty()) continue;
            if (m.text.empty()) continue;
            text = m.text;
            break;
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
         auto levels = folly::dynamic::array();
         for (auto l : kThinkingLevels) levels.push_back(l);
         return ok_(id, "get_available_thinking_levels",
                    folly::dynamic::object("levels", std::move(levels)));
      }
      if (type == "set_model") {
         auto const provider = strField(cmd, "provider");
         auto const modelId  = strField(cmd, "modelId");
         // pi: getAvailableSnapshot lookup; 单模型目录阶段只校验非空
         if (provider.empty() || modelId.empty())
            return error_(id, "set_model",
                          std::format("Model not found: {}/{}",
                                      provider, modelId));
         model_.provider = provider;
         model_.id = model_.name = modelId;
         session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::ModelChangeEntry{provider, modelId}});
         return ok_(id, "set_model", modelJson());
      }
      if (type == "cycle_model") {
         // 当前模型目录只含一个模型: ≤1 候选 -> data:null (pi 语义)
         return okData_(id, "cycle_model", nullptr);
      }
      if (type == "set_thinking_level") {
         auto const level = strField(cmd, "level");
         if (std::ranges::find(kThinkingLevels, level)
             != kThinkingLevels.end() && level != thinkingLevel_) {
            thinkingLevel_ = level;
            session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::ThinkingLevelEntry{level}});
            emit(folly::dynamic::object("type", "thinking_level_changed")
                                       ("level", level));
         }
         return ok_(id, "set_thinking_level");
      }
      if (type == "cycle_thinking_level") {
         if (!model_.reasoning)
            return okData_(id, "cycle_thinking_level", nullptr);
         auto const it = std::ranges::find(kThinkingLevels, thinkingLevel_);
         auto const n  = static_cast<size_t>(it - kThinkingLevels.begin());
         thinkingLevel_ = kThinkingLevels[(n + 1) % kThinkingLevels.size()];
         session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::ThinkingLevelEntry{thinkingLevel_}});
         emit(folly::dynamic::object("type", "thinking_level_changed")
                                    ("level", thinkingLevel_));
         return ok_(id, "cycle_thinking_level",
                    folly::dynamic::object("level", thinkingLevel_));
      }
      if (type == "set_steering_mode") {
         steeringMode_ = strField(cmd, "mode", steeringMode_);
         return ok_(id, "set_steering_mode");
      }
      if (type == "set_follow_up_mode") {
         followUpMode_ = strField(cmd, "mode", followUpMode_);
         return ok_(id, "set_follow_up_mode");
      }
      if (type == "set_auto_compaction") {
         autoCompactionEnabled_ = boolField(cmd, "enabled", true);
         return ok_(id, "set_auto_compaction");
      }
      if (type == "set_auto_retry") {
         autoRetryEnabled_ = boolField(cmd, "enabled", true);
         return ok_(id, "set_auto_retry");
      }
      if (type == "set_session_name") {
         auto name = strField(cmd, "name");
         // pi appendSessionInfo: [\r\n]+ -> " ", trim; 空名是错误
         name = squashWs(std::move(name));
         if (name.empty())
            return error_(id, "set_session_name",
                          "Session name cannot be empty");
         session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::SessionInfoEntry{name, true}});
         emit(folly::dynamic::object("type", "session_info_changed")
                                    ("name", name));
         return ok_(id, "set_session_name");
      }
      if (type == "new_session") {
         session_.reset();
         session_.setId(infra::uuidv7());
         if (auto p = strField(cmd, "parentSession"); !p.empty())
            session_.setParentSessionPath(p);
         session_.setFilePath({});
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
         auto r = infra::runShell(strField(cmd, "command"));
         // recordBashResult: bashExecution 消息入会话 (不发 message_*)
         domain::Message bm{domain::Role::BashExecution, {}, {}, {}};
         bm.command          = strField(cmd, "command");
         bm.text             = r.output;
         bm.exitCode         = r.exitCode;
         bm.truncated        = r.truncated;
         bm.excludeFromContext = boolField(cmd, "excludeFromContext", false);
         bm.timestampMs      = infra::nowMs();
         session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::MessageEntry{std::move(bm)}});
         emittedCount_ = session_.messages().size();   // 不出现在事件流
         return ok_(id, "bash",
                    folly::dynamic::object("output", r.output)
                       ("exitCode", r.exitCode)("cancelled", false)
                       ("truncated", r.truncated));
      }
      if (type == "compact") {
         auto const n = static_cast<int64_t>(session_.entries().size());
         return ok_(id, "compact",
                    folly::dynamic::object
                       ("summary", std::format("mock compaction of {} entries", n))
                       ("firstKeptEntryId", session_.leafId().empty()
                          ? folly::dynamic(nullptr)
                          : folly::dynamic(session_.leafId()))
                       ("tokensBefore", n));
      }
      if (type == "export_html") {
         auto path = strField(cmd, "outputPath");
         if (path.empty())
            path = std::format("pi-session-{}.html", session_.id());
         exportHtml(path);
         return ok_(id, "export_html",
                    folly::dynamic::object("path", path));
      }
      if (type == "get_entries") {
         return getEntries(id, cmd);
      }
      if (type == "get_tree") {
         return ok_(id, "get_tree", treeJson());
      }
      if (type == "get_fork_messages") {
         auto msgs = folly::dynamic::array();
         for (auto const& e : session_.entries()) {
            auto const* me = std::get_if<domain::MessageEntry>(&e.payload);
            if (!me || me->message.role != domain::Role::User) continue;
            if (me->message.text.empty()) continue;
            msgs.push_back(folly::dynamic::object
               ("entryId", e.id)("text", me->message.text));
         }
         return ok_(id, "get_fork_messages",
                    folly::dynamic::object("messages", std::move(msgs)));
      }
      if (type == "fork")  return doFork(id, cmd, /*at=*/false);
      if (type == "clone") return doFork(id, cmd, /*at=*/true);
      if (type == "switch_session") {
         return switchSession(id, strField(cmd, "sessionPath"));
      }
      return error_(id, type, std::format("Unknown command: {}", type));
   }

   // ---------- bash ----------
   // (已内联进 handle; BashExecutionMessage 由 session_.appendEntry 记录)

   // ---------- fork/clone ----------
   auto doFork(folly::dynamic const& id, folly::dynamic const& cmd, bool at)
      -> folly::dynamic {
      auto const entryId = at ? session_.leafId()
                              : strField(cmd, "entryId");
      if (entryId.empty())
         return error_(id, at ? "clone" : "fork",
                       "Cannot clone session: no current entry selected");

      auto const* target = session_.findEntry(entryId);
      std::string selectedText;
      if (auto const* me = target
             ? std::get_if<domain::MessageEntry>(&target->payload)
             : nullptr) {
         if (me->message.role == domain::Role::User)
            selectedText = me->message.text;
      }

      if (at) {
         // clone: leafId 必须在, 上面已校验
      } else {
         // position "before": 目标是 user message, leaf 移到 parentId
         if (!target
             || !std::holds_alternative<domain::MessageEntry>(target->payload)
             || std::get<domain::MessageEntry>(target->payload)
                    .message.role != domain::Role::User)
            return error_(id, "fork", "Invalid entry ID for forking");
      }

      auto const targetLeaf = at ? entryId : target->parentId;

      // 新会话 = root->targetLeaf 路径 (持久化时另写文件)
      std::string newFile;
      if (store_) newFile = store_->writeBranch(session_, targetLeaf);

      domain::Session next{infra::uuidv7(), session_.cwd()};
      next.setParentSessionPath(session_.filePath().empty()
                                   ? session_.id() + ".jsonl"
                                   : session_.filePath());
      next.setFilePath(newFile);
      std::vector<domain::Entry> path;
      for (std::string cur = targetLeaf; !cur.empty();) {
         auto const* e = session_.findEntry(cur);
         if (!e) break;
         path.push_back(*e);
         cur = e->parentId;
      }
      std::string parent;
      for (auto const& e : path | std::views::reverse) {
         auto copy     = e;
         copy.parentId = parent;
         parent = copy.id;
         next.appendLoaded(std::move(copy));
      }
      next.markPersisted();
      session_ = std::move(next);
      emittedCount_ = session_.messages().size();

      folly::dynamic data = folly::dynamic::object("cancelled", false);
      if (!selectedText.empty()) data["text"] = selectedText;
      return ok_(id, at ? "clone" : "fork", std::move(data));
   }

   auto switchSession(folly::dynamic const& id, std::string const& path)
      -> folly::dynamic {
      if (path.empty() || !store_)
         return error_(id, "switch_session",
                       std::format("Session file not found: {}", path));

      std::optional<domain::Session> loaded;
      std::string                    loadedPath;
      store_->load(path, [&](domain::Session s, std::string fp) {
         loaded     = std::move(s);
         loadedPath = std::move(fp);
      });
      if (!loaded || loadedPath.empty())
         return error_(id, "switch_session",
                       "Session file is not a valid pi session");

      session_ = std::move(*loaded);
      emittedCount_ = session_.messages().size();
      return ok_(id, "switch_session",
                 folly::dynamic::object("cancelled", false));
   }

   // ---------- export_html ----------
   auto exportHtml(std::string const& path) const -> void {
      std::ofstream f{path};
      f << "<!doctype html><meta charset=utf-8><title>"
        << session_.id() << "</title><pre>\n";
      for (auto const& m : session_.messages())
         f << "[" << roleToString(m.role) << "] " << m.text << "\n";
   }

   // ---------- entries / tree ----------
   auto getEntries(folly::dynamic const& id, folly::dynamic const& cmd) const
      -> folly::dynamic {
      auto const& entries = session_.entries();
      size_t      begin   = 0;
      if (auto since = strField(cmd, "since"); !since.empty()) {
         auto const it = std::ranges::find_if(
            entries, [&](domain::Entry const& e) { return e.id == since; });
         if (it == entries.end())
            return error_(id, "get_entries",
                          std::format("Entry not found: {}", since));
         begin = static_cast<size_t>(it - entries.begin()) + 1;
      }
      auto arr = folly::dynamic::array();
      for (auto const& e : entries | std::views::drop(begin))
         arr.push_back(entryToJson(e));
      folly::dynamic leaf;
      if (!session_.leafId().empty()) leaf = session_.leafId();
      return ok_(id, "get_entries",
                 folly::dynamic::object("entries", std::move(arr))
                                       ("leafId", leaf));
   }

   auto treeJson() const -> folly::dynamic {
      auto const& entries = session_.entries();

      // 最新 label 条目: targetId -> {label, ts}
      std::unordered_map<std::string,
                         std::pair<std::string, std::string>> labels;
      for (auto const& e : entries) {
         if (auto const* le = std::get_if<domain::LabelEntry>(&e.payload)) {
            if (le->clear) labels.erase(le->targetId);
            else           labels[le->targetId] = {le->label, e.timestampIso};
         }
      }

      // 建节点 map; roots = parentId 空/自指/父缺失
      std::unordered_map<std::string, size_t> byId;
      for (size_t i = 0; i < entries.size(); ++i) byId[entries[i].id] = i;

      std::vector<std::vector<size_t>> children(entries.size());
      std::vector<size_t>              roots;
      for (size_t i = 0; i < entries.size(); ++i) {
         auto const& p = entries[i].parentId;
         auto        it = byId.find(p);
         if (p.empty() || p == entries[i].id || it == byId.end())
            roots.push_back(i);
         else
            children[it->second].push_back(i);
      }

      std::function<folly::dynamic(size_t)> build = [&](size_t i) {
         folly::dynamic n = folly::dynamic::object
            ("entry", entryToJson(entries[i]));
         auto ch = folly::dynamic::array();
         // children 按 timestamp 升序
         auto idx = children[i];
         std::ranges::sort(idx, {}, [&](size_t k) {
            return entries[k].timestampIso;
         });
         for (auto c : idx) ch.push_back(build(c));
         n["children"] = std::move(ch);
         if (auto l = labels.find(entries[i].id); l != labels.end()) {
            n["label"]          = l->second.first;
            n["labelTimestamp"] = l->second.second;
         }
         return n;
      };

      std::ranges::sort(roots, {}, [&](size_t k) {
         return entries[k].timestampIso;
      });
      auto tree = folly::dynamic::array();
      for (auto r : roots) tree.push_back(build(r));

      folly::dynamic leaf;
      if (!session_.leafId().empty()) leaf = session_.leafId();
      return folly::dynamic::object("tree", std::move(tree))
                                   ("leafId", leaf);
   }

   // ---------- 状态序列化 ----------
   static auto strField(folly::dynamic const& j, char const* k,
                        std::string def = {}) -> std::string {
      auto it = j.find(k);
      return it != j.items().end() && it->second.isString()
                ? it->second.getString() : std::move(def);
   }
   static auto boolField(folly::dynamic const& j, char const* k, bool def)
      -> bool {
      auto it = j.find(k);
      return it != j.items().end() && it->second.isBool()
                ? it->second.getBool() : def;
   }
   static auto squashWs(std::string s) -> std::string {
      // [\r\n]+ -> " ", trim (pi appendSessionInfo)
      auto out = std::string{};
      out.reserve(s.size());
      bool pending = false;
      for (char c : s) {
         if (c == '\r' || c == '\n') { pending = true; continue; }
         if (pending && !out.empty()) out += ' ';
         pending = false;
         out += c;
      }
      while (!out.empty() && std::isspace(out.back())) out.pop_back();
      auto const first = out.find_first_not_of(" \t");
      return first == std::string::npos ? std::string{} : out.substr(first);
   }

   auto sessionName() const -> std::string {
      // 最新 session_info entry 的 name
      for (auto const& e : session_.entries() | std::views::reverse) {
         if (auto const* si
             = std::get_if<domain::SessionInfoEntry>(&e.payload))
            return si->hasName ? si->name : std::string{};
      }
      return {};
   }

   auto modelJson() const -> folly::dynamic {
      auto input = folly::dynamic::array();
      for (auto const& m : model_.input.empty()
                                ? std::vector<std::string>{"text"}
                                : model_.input)
         input.push_back(m);
      return folly::dynamic::object
         ("id", model_.id)("name", model_.name.empty() ? model_.id : model_.name)
         ("api", model_.api)("provider", model_.provider)
         ("baseUrl", model_.baseUrl)("reasoning", model_.reasoning)
         ("input", std::move(input))
         ("cost", folly::dynamic::object
            ("input", model_.cost.input)("output", model_.cost.output)
            ("cacheRead", model_.cost.cacheRead)
            ("cacheWrite", model_.cost.cacheWrite))
         ("contextWindow", model_.contextWindow)
         ("maxTokens", model_.maxTokens);
   }

   auto sessionFilePath() const -> std::string {
      if (!session_.filePath().empty()) return session_.filePath();
      return store_ ? store_->fileOf(session_) : std::string{};
   }

   auto sessionStateJson() const -> folly::dynamic {
      folly::dynamic d = folly::dynamic::object
         ("thinkingLevel", thinkingLevel_)
         ("isStreaming", false)("isCompacting", false)
         ("steeringMode", steeringMode_)("followUpMode", followUpMode_)
         ("sessionId", session_.id())
         ("autoCompactionEnabled", autoCompactionEnabled_)
         ("messageCount",
            static_cast<int64_t>(session_.messages().size()))
         ("pendingMessageCount",
            static_cast<int64_t>(steering_.size() + followUp_.size()));
      if (!model_.id.empty()) d["model"] = modelJson();
      if (auto const f = sessionFilePath(); !f.empty()) d["sessionFile"] = f;
      if (auto n = sessionName(); !n.empty()) d["sessionName"] = n;
      return d;
   }

   auto sessionStatsJson() const -> folly::dynamic {
      // pi: 全量 entry (全分支, 含被 compact 历史)
      int64_t user = 0, assistant = 0, toolCalls = 0, toolResults = 0,
              totalMessages = 0;
      int64_t tin = 0, tout = 0, tcacheR = 0, tcacheW = 0;
      double  cost = 0;
      auto addUsage = [&](domain::Usage const& u) {
         tin += u.input;     tout    += u.output;
         tcacheR += u.cacheRead; tcacheW += u.cacheWrite;
         cost += u.cost.total;
      };
      for (auto const& e : session_.entries()) {
         if (auto const* me
             = std::get_if<domain::MessageEntry>(&e.payload)) {
            ++totalMessages;
            switch (me->message.role) {
               case domain::Role::User:      ++user; break;
               case domain::Role::Assistant: ++assistant;
                  toolCalls += static_cast<int64_t>(
                                 me->message.toolCalls.size());
                  addUsage(me->message.usage);
                  break;
               case domain::Role::Tool:      ++toolResults;
                  addUsage(me->message.usage);
                  break;
               default: break;
            }
         } else if (auto const* ue
                    = std::get_if<domain::UsageRecordEntry>(&e.payload)) {
            addUsage(ue->usage);
         } else if (auto const* ce
                    = std::get_if<domain::CompactionEntry>(&e.payload)) {
            if (ce->hasUsage) addUsage(ce->usage);
         } else if (auto const* be
                    = std::get_if<domain::BranchSummaryEntry>(&e.payload)) {
            if (be->hasUsage) addUsage(be->usage);
         }
      }
      auto tokens = folly::dynamic::object
         ("input", tin)("output", tout)("cacheRead", tcacheR)
         ("cacheWrite", tcacheW)
         ("total", tin + tout + tcacheR + tcacheW);
      folly::dynamic d = folly::dynamic::object
         ("sessionId", session_.id())
         ("userMessages", user)("assistantMessages", assistant)
         ("toolCalls", toolCalls)("toolResults", toolResults)
         ("totalMessages", totalMessages)
         ("tokens", std::move(tokens))("cost", cost);
      if (auto const f = sessionFilePath(); !f.empty()) d["sessionFile"] = f;
      if (auto n = sessionName(); !n.empty()) d["sessionName"] = n;
      return d;
   }

   AgentSessionService&   service_;
   domain::Session&       session_;
   domain::ISessionStore* store_;
   std::istream&          in_;
   std::ostream&          out_;

   // RPC 会话状态 (pi AgentSession 对应物)
   domain::Model model_{/*id*/ "mock-1", /*name*/ "mock-1", /*api*/ "mock",
                       /*provider*/ "mock", /*baseUrl*/ "", /*input*/ {"text"},
                       /*cost*/ {}, /*contextWindow*/ 200000,
                       /*maxTokens*/ 8192, /*reasoning*/ true};
   std::string   thinkingLevel_         = "medium";
   std::string   steeringMode_          = "one-at-a-time";
   std::string   followUpMode_          = "one-at-a-time";
   bool          autoCompactionEnabled_ = true;
   bool          autoRetryEnabled_      = true;
   std::deque<std::string> steering_;
   std::deque<std::string> followUp_;
   size_t        emittedCount_ = 0;   // 已发 message_* 的投影消息数
};

} // namespace pi::iface::rpc
