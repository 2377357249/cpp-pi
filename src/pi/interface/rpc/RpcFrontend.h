#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/compaction/CompactionPolicy.h>
#include <pi/domain/model/Model.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/domain/session/Session.h>
#include <pi/domain/session/SessionFork.h>
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/process/Shell.h>
#include <pi/infra/util/Ids.h>
#include <pi/infra/util/JsonSafe.h>
#include <pi/interface/rpc/JsonCodec.h>
#include <pi/interface/rpc/RpcEventSink.h>
#include <pi/interface/session/SessionExport.h>
#include <pi/interface/trans/AgentSessionService.h>
#include <pi/interface/trans/CompactRunner.h>

namespace pi::iface::rpc {

// ---------------------------------------------------------------------------
// RPC 前端: 与 pi TS TUI 对接的 NDJSON 协议端 (rpc-types.ts 1:1).
//   命令帧 {id?, type, ...}; 响应帧 {id?, type:"response", command, success,
//   data|error}; 事件帧即 AgentSessionEvent —— domain role 在发生点经
//   IAgentEventSink 同步回调, sink_ 在此序列化上线 (pi runAgentLoop 序).
// ---------------------------------------------------------------------------
class RpcFrontend {
public:
   static constexpr std::array kThinkingLevels{
      "off", "minimal", "low", "medium", "high", "xhigh", "max"};

   RpcFrontend(AgentSessionService&   service,
               domain::Session&       session,
               infra::EventLoop&      loop,
               domain::ISessionStore* store = nullptr,
               std::istream&          in  = std::cin,
               std::ostream&          out = std::cout)
      : service_(service), session_(session), store_(store),
        loop_(loop), in_(in), out_(out),
        sink_([this](folly::dynamic f) { emit(std::move(f)); }),
        compact_(service, session, store,
                 [this](folly::dynamic f) { emit(std::move(f)); }) {}

   // 模型选择态收编进 service (ModelCtx) —— RPC/TUI 共享同一真值.
   auto model() const -> domain::Model const& { return service_.model(); }
   auto thinkingLevel() const -> std::string const& {
      return service_.thinkingLevel();
   }
   auto sessionFile() const -> std::string { return sessionFilePath(); }

   int run() {
      service_.setAbortFlag(abortRequested_);
      service_.setEventSink(sink_);
      // stdin 是唯一自带线程的事件源 (阻塞 getline); 行+EOF 都经
      // loop.post 回 owner 线程 —— 命令处理/事务/发射全在 loop 线程.
      std::jthread reader([this] {
         for (std::string line; std::getline(in_, line);)
            loop_.post([this, line = std::move(line)] {
               inbox_.push_back(std::move(line));
               pumpCommands();
            });
         loop_.post([this] { eof_ = true; });
      });
      loop_.runUntil([this] {
         return eof_ && inbox_.empty() && !busy_;
      });
      return 0;
   }

private:
   // ---------- 并发模型 (pi: prompt 返回 started 后 turn 后台跑) ----------
   // 单线程事件循环: stdin reader 线程只负责投递行; 命令按行序在
   // pumpCommands 里处理 —— busy_ 时快路径命令 (steer/follow_up/prompt
   // 排队语义) 就地消费, 其余堵在 inbox 头部等 turn 收尾 (等价旧
   // turnMu_ 串行化 + 严格保序). abortRequested_ 仍是原子旗标:
   // 写方包括未来的跨线程源.
   std::atomic<bool>       abortRequested_{false};
   std::deque<std::string> inbox_;
   bool                    eof_     = false;
   bool                    busy_    = false;   // run 或其队列排干链进行中
   bool                    pumping_ = false;
   infra::EventLoop&       loop_;

   // pi: 这些命令先中止进行中的 run (compact 也 await this.abort()).
   static auto isPreempt(std::string_view type) -> bool {
      return type == "compact" || type == "new_session"
             || type == "switch_session" || type == "fork"
             || type == "clone";
   }

   // 按行序处理 inbox 头部. busy_ 时非快路径命令在头部阻塞 ——
   // 后续命令仍在它们之后排队 (pi 单事件循环同义).
   auto pumpCommands() -> void {
      if (pumping_) return;
      pumping_ = true;
      while (!inbox_.empty()) {
         // 拷贝而非移动: break 分支要把原样留在头部等空闲
         auto line = inbox_.front();
         if (line.empty()) { inbox_.pop_front(); continue; }
         if (line.back() == '\r') line.pop_back();

         auto parsed = infra::parseJsonSafe(line);
         if (parsed.hasError()) {
            emit(error_(folly::dynamic{}, "parse",
                        std::format("Failed to parse command: {}",
                                    parsed.error())));
            inbox_.pop_front();
            continue;
         }
         auto const type = strField(*parsed, "type");

         if (busy_) {
            // 快路径: 能插进运行中 run 的命令就地分发
            if (type == "steer" || type == "follow_up"
                || type == "prompt") {
               auto resp = handle(*parsed);
               if (!resp.isNull()) emit(resp);
               inbox_.pop_front();
               continue;
            }
            // abort/抢占类置旗, 响应延后到 run 收尾 (pi await run 同义)
            if (type == "abort" || isPreempt(type))
               abortRequested_ = true;
            break;
         }

         if (type == "abort") {
            abortRequested_ = true;
            emit(ok_(idOf(*parsed), "abort"));
            inbox_.pop_front();
            continue;
         }
         if (isPreempt(type)) abortRequested_ = true;
         auto resp = handle(*parsed);
         if (!resp.isNull()) emit(resp);   // prompt 的响应在 handle 内已发
         inbox_.pop_front();
      }
      pumping_ = false;
   }

   auto startTurn(std::string msg) -> void {
      abortRequested_ = false;
      busy_           = true;
      service_.startTurn(std::move(msg), [this](auto) { onRunDone(); });
   }

   // 一轮 run 收尾 (service 的 RunDoneHandler, loop 线程):
   // pi: 阈值压缩检查 → steering 先于 followUp 排干 (abort 旗标下
   // 保留剩余队列) → 空闲后放行 inbox 阻塞命令.
   auto onRunDone() -> void {
      compact_.maybeAutoCompact();
      if (!abortRequested_ && (!steering_.empty() || !followUp_.empty())) {
         std::string msg;
         if (!steering_.empty()) {
            msg = std::move(steering_.front());
            steering_.pop_front();
         } else {
            msg = std::move(followUp_.front());
            followUp_.pop_front();
         }
         emitQueueUpdate();
         service_.startTurn(std::move(msg),
                            [this](auto) { onRunDone(); });
         return;
      }
      busy_ = false;
      pumpCommands();
   }

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

   // ---------- 命令分发 ----------
   // 只在 loop 线程被 pumpCommands 调用; busy_/队列排序由它把守,
   // abort/抢占的置旗在 pumpCommands 完成 (pi: 须等 run 空转才响应).
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

      if (type == "steer" || type == "follow_up") {
         (type == "steer" ? steering_ : followUp_)
            .push_back(strField(cmd, "message"));
         emitQueueUpdate();
         return ok_(id, type,
                    folly::dynamic::object("disposition", "queued"));
      }

      if (type == "prompt") {
         auto msg = strField(cmd, "message");
         // pi: 运行中 prompt 排队 (followUp 通道), disposition "queued"
         if (busy_) {
            followUp_.push_back(std::move(msg));
            emitQueueUpdate();
            emit(ok_(id, "prompt",
                     folly::dynamic::object("disposition", "queued")));
            return folly::dynamic{};
         }
         emit(ok_(id, "prompt",
                  folly::dynamic::object("disposition", "started")));
         startTurn(std::move(msg));
         return folly::dynamic{};
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
         auto arr = folly::dynamic::array();
         for (auto const& m : service_.catalog().models)
            arr.push_back(modelJson(m));
         return ok_(id, "get_available_models",
                    folly::dynamic::object("models", std::move(arr)));
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
         auto err = service_.setModel(provider, modelId);
         if (err) return error_(id, "set_model", *err);
         session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::ModelChangeEntry{provider, modelId}});
         return ok_(id, "set_model", modelJson());
      }
      if (type == "cycle_model") {
         // pi cycleModel: 全目录顺序前进一格 (跨 provider);
         // ≤1 候选或切换失败 -> data:null; 成功 -> ModelCycleResult 形状.
         auto const* next = service_.cycleModel(1);
         if (next == nullptr) return okData_(id, "cycle_model", nullptr);
         session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::ModelChangeEntry{next->provider, next->id}});
         return ok_(id, "cycle_model",
                    folly::dynamic::object("model", modelJson())
                       ("thinkingLevel", thinkingLevel())
                       ("isScoped", false));
      }
      if (type == "set_thinking_level") {
         auto const level = strField(cmd, "level");
         if (std::ranges::find(kThinkingLevels, level)
             != kThinkingLevels.end() && level != thinkingLevel()) {
            service_.setThinkingLevel(level);
            session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::ThinkingLevelEntry{level}});
            emit(folly::dynamic::object("type", "thinking_level_changed")
                                       ("level", level));
         }
         return ok_(id, "set_thinking_level");
      }
      if (type == "cycle_thinking_level") {
         if (!model().reasoning)
            return okData_(id, "cycle_thinking_level", nullptr);
         auto const it = std::ranges::find(kThinkingLevels, thinkingLevel());
         auto const n  = static_cast<size_t>(it - kThinkingLevels.begin());
         auto const lv = kThinkingLevels[(n + 1) % kThinkingLevels.size()];
         service_.setThinkingLevel(lv);
         session_.appendEntry(domain::Entry{domain::EntryBase{}, domain::ThinkingLevelEntry{lv}});
         emit(folly::dynamic::object("type", "thinking_level_changed")
                                    ("level", lv));
         return ok_(id, "cycle_thinking_level",
                    folly::dynamic::object("level", lv));
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
         compact_.setAutoEnabled(boolField(cmd, "enabled", true));
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
         return ok_(id, "bash",
                    folly::dynamic::object("output", r.output)
                       ("exitCode", r.exitCode)("cancelled", false)
                       ("truncated", r.truncated));
      }
      if (type == "compact") {
         std::string compactErr;
         auto r = compact_.run("manual",
                               strField(cmd, "customInstructions"),
                               false, &compactErr);
         return r ? ok_(id, "compact", std::move(*r))
                  : error_(id, "compact", compactErr);
      }
      if (type == "export_html") {
         auto path = strField(cmd, "outputPath");
         path = exportSessionHtml(session_, path);
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

   // ---------- fork/clone (手术本体在 domain::forkSession) ----------
   auto doFork(folly::dynamic const& id, folly::dynamic const& cmd, bool at)
      -> folly::dynamic {
      auto const entryId = at ? session_.leafId()
                              : strField(cmd, "entryId");
      std::string err;
      auto selected = domain::forkSession(session_, store_, entryId, at, &err);
      if (!selected)
         return error_(id, at ? "clone" : "fork",
                       err.empty() ? "Invalid entry ID for forking" : err);
      folly::dynamic data = folly::dynamic::object("cancelled", false);
      if (!selected->empty()) data["text"] = *selected;
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
      return ok_(id, "switch_session",
                 folly::dynamic::object("cancelled", false));
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

   auto modelJson() const -> folly::dynamic { return modelJson(model()); }
   static auto modelJson(domain::Model const& md) -> folly::dynamic {
      auto input = folly::dynamic::array();
      for (auto const& m : md.input.empty()
                                ? std::vector<std::string>{"text"}
                                : md.input)
         input.push_back(m);
      return folly::dynamic::object
         ("id", md.id)("name", md.name.empty() ? md.id : md.name)
         ("api", md.api)("provider", md.provider)
         ("baseUrl", md.baseUrl)("reasoning", md.reasoning)
         ("input", std::move(input))
         ("cost", folly::dynamic::object
            ("input", md.cost.input)("output", md.cost.output)
            ("cacheRead", md.cost.cacheRead)
            ("cacheWrite", md.cost.cacheWrite))
         ("contextWindow", md.contextWindow)
         ("maxTokens", md.maxTokens);
   }

   auto sessionFilePath() const -> std::string {
      if (!session_.filePath().empty()) return session_.filePath();
      return store_ ? store_->fileOf(session_) : std::string{};
   }

   auto sessionStateJson() const -> folly::dynamic {
      folly::dynamic d = folly::dynamic::object
         ("thinkingLevel", thinkingLevel())
         ("isStreaming", false)("isCompacting", false)
         ("steeringMode", steeringMode_)("followUpMode", followUpMode_)
         ("sessionId", session_.id())
         ("autoCompactionEnabled", compact_.autoEnabled())
         ("messageCount",
            static_cast<int64_t>(session_.messages().size()))
         ("pendingMessageCount",
            static_cast<int64_t>(steering_.size() + followUp_.size()));
      if (!model().id.empty()) d["model"] = modelJson();
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

   // RPC 会话状态 (pi AgentSession 对应物); 模型/thinking 收编进 service.
   std::string   steeringMode_          = "one-at-a-time";
   std::string   followUpMode_          = "one-at-a-time";
   bool          autoRetryEnabled_      = true;
   std::deque<std::string> steering_;
   std::deque<std::string> followUp_;
   RpcEventSink            sink_;   // domain 事件 -> 线帧 (run() 注入 impl)
   CompactRunner           compact_; // pi compact/threshold 语义 (emit 共享)
};

} // namespace pi::iface::rpc
