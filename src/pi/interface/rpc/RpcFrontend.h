#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
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
               domain::ISessionStore* store = nullptr,
               std::istream&          in  = std::cin,
               std::ostream&          out = std::cout)
      : service_(service), session_(session), store_(store),
        in_(in), out_(out),
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
      joinTurn();   // 输入关闭时等 in-flight turn 收尾
      return 0;
   }

private:
   // ---------- 并发模型 (pi: prompt 返回 started 后 turn 后台跑) ----------
   // emitMu_: NDJSON 输出不交错; turnMu_: session_ 串行化,
   //   持锁单位=整轮事务 (会话读命令遇忙则等到 turn 收尾);
   // queueMu_: steering_/followUp_ 短临界区 (steer 必须能插进运行中的 turn);
   // abortRequested_: 事务周期检查的中止旗标 —— abort 不拿任何互斥.
   // 锁序: turnMu_ → queueMu_ → emitMu_ (反向不得持有).
   std::mutex        emitMu_;
   std::mutex        turnMu_;
   std::mutex        queueMu_;
   std::atomic<bool> abortRequested_{false};
   std::atomic<bool> turnRunning_{false};
   std::thread       turnThread_;

   auto joinTurn() -> void {
      if (turnThread_.joinable()) turnThread_.join();
   }

   // worker 侧驱动: turnMu_ 贯穿 事务+事件; 之后清旗+排干循环 —
   // "flag=false" 与"队列为空"在同一 queueMu_ 临界区判定, 与主线程
   // push 互斥, 不会出现"已退出 drain 后才有新排队"的滞留窗口.
   auto runTurnWorker(std::string msg) -> void {
      {
         std::lock_guard lk{turnMu_};
         service_.runTurn(std::move(msg));
         compact_.maybeAutoCompact();
         for (;;) {
            drainQueues();
            std::lock_guard q{queueMu_};
            if ((steering_.empty() && followUp_.empty())
                || abortRequested_) {
               turnRunning_ = false;
               break;
            }
         }
      }
      turnRunning_ = false;
   }

   auto startTurn(std::string msg) -> void {
      joinTurn();
      abortRequested_ = false;
      turnRunning_    = true;
      turnThread_ = std::thread(&RpcFrontend::runTurnWorker, this,
                                std::move(msg));
   }

   // ---------- 协议帧 ----------
   auto emit(folly::dynamic const& obj) -> void {
      std::lock_guard lk{emitMu_};
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

   // ---------- 队列 (queueMu_ 短临界区) ----------
   auto emitQueueUpdate() -> void {
      std::lock_guard lk{queueMu_};
      auto steering = folly::dynamic::array();
      auto followUp = folly::dynamic::array();
      for (auto const& s : steering_)  steering.push_back(s);
      for (auto const& s : followUp_)  followUp.push_back(s);
      emit(folly::dynamic::object("type", "queue_update")
                                 ("steering", std::move(steering))
                                 ("followUp", std::move(followUp)));
   }

   // run 结束后排干队列: steering 先于 followUp; 中止旗标下保留剩余队列
   // (pi abort: 中止当前 run, 排队消息留给下一轮). worker 持 turnMu_ 调用.
   auto drainQueues() -> void {
      for (;;) {
         std::string msg;
         {
            std::lock_guard lk{queueMu_};
            if ((steering_.empty() && followUp_.empty())
                || abortRequested_) break;
            if (!steering_.empty()) {
               msg = std::move(steering_.front());
               steering_.pop_front();
            } else {
               msg = std::move(followUp_.front());
               followUp_.pop_front();
            }
         }
         emitQueueUpdate();
         service_.runTurn(std::move(msg));
         compact_.maybeAutoCompact();
      }
   }

   // ---------- 命令分发 ----------
   // 无锁快路径: abort (atomic+join) / steer|follow_up (queueMu_) ——
   // 必须能插进运行中的 turn; 其余命令 turnMu_ 与 worker 串行.
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

      // pi session.abort(): 请求中止并等 run 空转; 响应恒 success
      if (type == "abort") {
         abortRequested_ = true;
         joinTurn();
         return ok_(id, "abort");
      }

      if (type == "steer" || type == "follow_up") {
         {
            std::lock_guard lk{queueMu_};
            (type == "steer" ? steering_ : followUp_)
               .push_back(strField(cmd, "message"));
         }
         emitQueueUpdate();
         return ok_(id, type,
                    folly::dynamic::object("disposition", "queued"));
      }

      if (type == "prompt") {
         auto msg = strField(cmd, "message");
         // pi: 运行中 prompt 排队 (followUp 通道), disposition "queued"
         if (turnRunning_) {
            {
               std::lock_guard lk{queueMu_};
               followUp_.push_back(std::move(msg));
            }
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

      // pi: 这些命令先中止进行中的 run (compact 也 await this.abort()).
      // 在 turnMu_ 前置旗+join —— 锁内 join 会与持有它的 worker 互等死锁.
      if (type == "compact" || type == "new_session"
          || type == "switch_session" || type == "fork" || type == "clone") {
         abortRequested_ = true;
         joinTurn();
      }

      // 其余命令与 worker turn 串行 (pi 单事件循环同义).
      std::lock_guard lk{turnMu_};

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
