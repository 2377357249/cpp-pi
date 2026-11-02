#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <folly/json/json.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <pi/domain/session/ISessionStore.h>
#include <pi/domain/session/Session.h>
#include <pi/domain/session/SessionFork.h>
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/async/ThreadPool.h>
#include <pi/infra/process/Shell.h>
#include <pi/infra/util/Ids.h>
#include <pi/infra/util/JsonSafe.h>
#include <pi/interface/session/SessionExport.h>
#include <pi/interface/trans/AgentSessionService.h>
#include <pi/interface/trans/CompactRunner.h>
#include <pi/interface/tui/ChatModel.h>

namespace pi::iface::tui {

// ---------------------------------------------------------------------------
// TUI 前端: FTXUI 全屏界面, 命令面对齐 pi interactive mode.
//   布局: 滚动聊天区 (markdown-lite + thinking dim + 工具块) + 选择器
//         overlay + 底部输入框 + footer (模型/thinking/队列/token/ctx%).
//   键位 (pi keybindings.ts): ctrl+p 循环模型, ctrl+l 模型选择器,
//         shift+tab 循环 thinking, ctrl+t 思维块显隐, Esc 中止, ^C 退出.
//   线程: UI 线程 = ScreenInteractive; 领域态全在 EventLoop 线程 —
//         提交经 loop_.post 进 pending_ 队列, pumpPending 在 loop 线程
//         排干 (prompt → service.startTurn 异步; Cmd → execCmd 同步段
//         + compact/resume 的异步段). 领域态零锁, chat_ 视图态仍靠 mu.
// ---------------------------------------------------------------------------
class TuiFrontend {
public:
   static constexpr std::array kThinkingLevels{
      "off", "minimal", "low", "medium", "high", "xhigh", "max"};

   TuiFrontend(AgentSessionService&   service,
               domain::Session&       session,
               domain::ISessionStore& store,
               infra::EventLoop&      loop,
               infra::ThreadPool&     pool)
      : service_(service), session_(session), store_(store),
        loop_(loop), pool_(pool),
        screen_(ftxui::ScreenInteractive::Fullscreen()),
        sink_(chat_, [this] { screen_.PostEvent(ftxui::Event::Custom); }),
        compact_(service, session, &store,
                 [this](folly::dynamic f) { onCompactEvent(f); }) {}

   auto run() -> int {
      service_.setEventSink(sink_);
      syncModel();
      syncSessionName();

      // 领域线程: loop_.runUntil 是 owner-only —— 由专属 jthread 驱动,
      // UI 线程一切提交/命令都经 post 落到这条线程.
      loopThread_ = std::jthread(
         [this] { loop_.runUntil([this] { return quit_.load(); }); });

      ftxui::InputOption iopt;
      iopt.multiline   = false;   // 默认 true: Enter 会先把 '\n' 插进 content
      iopt.placeholder =
         "问点什么…  (/help, Esc=中止, ^C=退出, ^P=换模型, ⇧Tab=thinking)";
      iopt.on_enter    = [this] {
         if (!input_.empty()) {
            submit(std::move(input_));
            input_.clear();
         }
      };
      auto input = ftxui::Input(&input_, iopt);

      auto root = ftxui::Renderer(input, [&] {
         ftxui::Elements es;
         {
            std::lock_guard lk{chat_.mu};
            for (auto const& b : chat_.blocks) es.push_back(renderBlock(b));
         }
         if (!es.empty()) es.back() = std::move(es.back()) | ftxui::focus;
         auto chat = ftxui::vbox(std::move(es)) | ftxui::yframe
                   | ftxui::yflex;

         ftxui::Element middle = ftxui::vbox(
            chat, ftxui::separator(),
            ftxui::hbox(ftxui::text(" ❯ ") | ftxui::bold,
                        input->Render()) | ftxui::xflex);
         if (sel_ != SelKind::None && !selLabels_.empty())
            middle = ftxui::vbox(
               chat, selectorHeader() | ftxui::dim,
               menu_->Render()
                  | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN,
                                kSelMaxRows)
                  | ftxui::border,
               ftxui::separator(),
               ftxui::hbox(ftxui::text(" ❯ ") | ftxui::bold,
                           input->Render()) | ftxui::xflex);
         return ftxui::vbox(middle | ftxui::yflex,
                            ftxui::separator(),
                            renderFooter());
      });

      menu_ = ftxui::Menu(&selLabels_, &selIdx_);

      root |= ftxui::CatchEvent([&](ftxui::Event const& e) {
         // 选择器打开时吃掉全部事件 (Enter/Esc 处理, 其余喂 menu)
         if (sel_ != SelKind::None) {
            if (e == ftxui::Event::Escape) { closeSel(); return true; }
            if (e == ftxui::Event::Return) { applySel(); return true; }
            if (e == ftxui::Event::CtrlC)  { closeSel(); return true; }
            if (menu_) menu_->OnEvent(e);
            return true;
         }
         if (e == ftxui::Event::Escape) {            // 中止当前 turn
            service_.requestAbort();
            return true;
         }
         if (e == ftxui::Event::CtrlC) {             // 退出
            screen_.Exit();
            return true;
         }
         if (e == ftxui::Event::CtrlP)  { enqueue("cycle_model",
                                       ItemKind::Cmd); return true; }
         if (e == ftxui::Event::AltP)   { enqueue("cycle_model_rev",
                                       ItemKind::Cmd); return true; }
         if (e == ftxui::Event::CtrlL)  { openSelector(SelKind::Model);
                                        return true; }
         if (e == ftxui::Event::CtrlO)  {            // 展开/收起工具输出 (pi 语义)
            { std::lock_guard lk{chat_.mu};
              chat_.expandTools = !chat_.expandTools; }
            screen_.PostEvent(ftxui::Event::Custom);
            return true;
         }
         if (e == ftxui::Event::TabReverse) {
            enqueue("cycle_thinking", ItemKind::Cmd); return true;
         }
         if (e == ftxui::Event::CtrlT)  {            // 思维块显隐 (pi 语义)
            { std::lock_guard lk{chat_.mu};
              chat_.showThinking = !chat_.showThinking; }
            screen_.PostEvent(ftxui::Event::Custom);
            return true;
         }
         // 任何交互后把焦点钉回输入框
         input->TakeFocus();
         return false;
      });

      screen_.Loop(root);
      // 退出: 中止在飞事务 + 停 loop 线程 (jthread 析构也会 join)
      service_.requestAbort();
      quit_ = true;
      loop_.post([] {});
      if (loopThread_.joinable()) loopThread_.join();
      return 0;
   }

private:
   static constexpr int kSelMaxRows = 10;

   enum class ItemKind { Prompt, Cmd };
   struct QueuedItem { ItemKind kind; std::string text; };
   enum class SelKind  { None, Model, Thinking, Session, Fork, Tree };

   // ============================ 提交/队列 ============================
   // '/' 前缀 -> slashDispatch (UI 线程只做打开选择器/即时命令;
   // 状态修改一律规范化成 Cmd 经 post 排进 pending_, loop 线程排干).
   auto submit(std::string text) -> void {
      if (!text.empty() && text.front() == '/' && slashDispatch(text))
         return;
      enqueue(std::move(text));
   }

   auto enqueue(std::string text, ItemKind k = ItemKind::Prompt) -> void {
      loop_.post([this, text = std::move(text), k] {
         pending_.push_back({k, std::move(text)});
         sink_.setQueued(pending_.size());
         pumpPending();
      });
   }

   // loop 线程排干: turn/compact/load 任一在飞即暂停; prompt ->
   // startTurn 异步挂起, Cmd -> execCmd (可能起异步 compact/load).
   auto pumpPending() -> void {
      while (!pending_.empty() && !turnBusy_ && !ioBusy_
             && !service_.compacting()) {
         auto item = std::move(pending_.front());
         pending_.pop_front();
         sink_.setQueued(pending_.size());
         if (item.kind == ItemKind::Cmd) {
            execCmd(item.text);
         } else {
            turnBusy_ = true;
            service_.startTurn(std::move(item.text),
                               [this](auto) { onTurnDone(); });
         }
      }
      sink_.setQueued(pending_.size());
      syncModel();
      screen_.PostEvent(ftxui::Event::Custom);
   }

   auto onTurnDone() -> void {
      turnBusy_ = false;
      // pi: 阈值压缩检查在队列排干之前; maybeAutoCompact 恒调 done.
      compact_.maybeAutoCompact([this] { pumpPending(); });
   }

   // ============================ slash 命令 ============================
   // 返回 true = 已消费 (不进 prompt 队列).
   auto slashDispatch(std::string const& text) -> bool {
      auto const sp   = text.find(' ');
      auto const name = text.substr(1, sp == std::string::npos
                                       ? std::string::npos : sp - 1);
      auto const arg  = sp == std::string::npos
         ? std::string{} : trim(text.substr(sp + 1));

      if (name == "q" || name == "quit" || name == "exit") {
         screen_.Exit();
         return true;
      }
      // 选择器开启命令 (无参数时打开 overlay; 带参数直接执行)
      if (name == "model") {
         if (arg.empty()) openSelector(SelKind::Model);
         else enqueue("model " + arg, ItemKind::Cmd);
         return true;
      }
      if (name == "thinking") {
         if (arg.empty()) openSelector(SelKind::Thinking);
         else enqueue("thinking " + arg, ItemKind::Cmd);
         return true;
      }
      if (name == "resume") {
         if (arg.empty()) openSelector(SelKind::Session);
         else enqueue("resume " + arg, ItemKind::Cmd);
         return true;
      }
      if (name == "fork") {
         if (arg.empty()) openSelector(SelKind::Fork);
         else enqueue("fork " + arg, ItemKind::Cmd);
         return true;
      }
      if (name == "tree") {
         if (arg.empty()) openSelector(SelKind::Tree);
         else enqueue("tree " + arg, ItemKind::Cmd);
         return true;
      }
      static constexpr std::array kQueued{
         "clone", "new", "compact", "name", "session", "export",
         "copy", "help", "hotkeys"};
      if (std::ranges::find(kQueued, name) != kQueued.end()) {
         enqueue(arg.empty() ? name : name + " " + arg, ItemKind::Cmd);
         return true;
      }
      info(std::format("Unknown command: /{} (try /help)", name));
      return true;
   }

   // worker 线程执行 (会话/模型状态只在这里写).
   auto execCmd(std::string const& cmd) -> void {
      auto const sp   = cmd.find(' ');
      auto const name = sp == std::string::npos ? cmd : cmd.substr(0, sp);
      auto const arg  = sp == std::string::npos
         ? std::string{} : trim(cmd.substr(sp + 1));

      if (name == "model") {
         auto err = service_.setModel(arg);
         if (err) { info("model: " + *err); return; }
         session_.appendEntry(domain::Entry{domain::EntryBase{},
            domain::ModelChangeEntry{service_.model().provider,
                                     service_.model().id}});
         info(std::format("model → {}/{}", service_.model().provider,
                          service_.model().id));
         return;
      }
      if (name == "cycle_model" || name == "cycle_model_rev") {
         // pi cycleModel: 全目录顺序循环 (跨 provider), 反向走 Alt+P
         auto const* next =
            service_.cycleModel(name == "cycle_model_rev" ? -1 : 1);
         if (next == nullptr) { info("cycle_model: 无可循环模型"); return; }
         session_.appendEntry(domain::Entry{domain::EntryBase{},
            domain::ModelChangeEntry{next->provider, next->id}});
         info(std::format("model → {}/{}", next->provider, next->id));
         return;
      }
      if (name == "thinking") {
         if (std::ranges::find(kThinkingLevels, arg) == kThinkingLevels.end()) {
            info(std::format("thinking: 未知级别 '{}' "
                             "(off|minimal|low|medium|high|xhigh|max)", arg));
            return;
         }
         setThinking(arg);
         return;
      }
      if (name == "cycle_thinking") {
         if (!service_.model().reasoning) {
            info("cycle_thinking: 当前模型不支持 thinking"); return;
         }
         auto const it = std::ranges::find(kThinkingLevels,
                                           service_.thinkingLevel());
         auto const n  = static_cast<size_t>(it - kThinkingLevels.begin());
         setThinking(kThinkingLevels[(n + 1) % kThinkingLevels.size()]);
         return;
      }
      if (name == "new") {
         session_.reset();
         session_.setId(infra::uuidv7());
         session_.setFilePath({});
         rebuild();
         info("new session");
         return;
      }
      if (name == "compact") {
         // 协程化: CompactProc 事务跑摘要; 结束回 pumpPending.
         compact_.run("manual", arg, false,
                      [this](auto, auto) { pumpPending(); });
         return;
      }
      if (name == "name") {
         if (arg.empty()) { info("name: 需要会话名"); return; }
         session_.appendEntry(domain::Entry{domain::EntryBase{},
            domain::SessionInfoEntry{arg, true}});
         syncSessionName();
         info("session name → " + arg);
         return;
      }
      if (name == "session") { info(sessionInfo()); return; }
      if (name == "export") {
         info("exported → " + exportSessionHtml(session_, arg));
         return;
      }
      if (name == "copy") {
         auto const t = lastAssistantText();
         if (t.empty()) { info("copy: 没有 assistant 回复"); return; }
         copyToClipboard(t);
         info("copied " + std::to_string(t.size()) + " chars");
         return;
      }
      if (name == "resume") {
         if (arg.empty()) { info("resume: 需要会话文件路径"); return; }
         // 协程化: JSONL 全量读丢 pool; ioBusy_ 闸住后续队列.
         ioBusy_ = true;
         pool_.submit([this, arg] {
            std::optional<domain::Session> loaded;
            store_.load(arg, [&](domain::Session s, std::string) {
               loaded = std::move(s);
            });
            loop_.post([this, arg, loaded = std::move(loaded)]() mutable {
               ioBusy_ = false;
               if (!loaded || loaded->entries().empty()) {
                  info("resume: 非法会话文件 " + arg);
               } else {
                  session_ = std::move(*loaded);
                  rebuild();
                  info("resumed " + arg);
               }
               pumpPending();
            });
         });
         return;
      }
      if (name == "fork") {
         std::string err;
         auto sel = domain::forkSession(session_, &store_, arg, false, &err);
         if (!sel) { info("fork: " + err); return; }
         rebuild();
         info("forked" + (sel->empty() ? "" : " (from: " +
                          sel->substr(0, 60) + ")"));
         return;
      }
      if (name == "clone") {
         std::string err;
         auto sel = domain::forkSession(session_, &store_,
                                        session_.leafId(), true, &err);
         if (!sel) { info("clone: " + err); return; }
         rebuild();
         info("cloned session");
         return;
      }
      if (name == "tree") {
         if (!session_.setLeaf(arg)) { info("tree: 无效 entry " + arg); return; }
         rebuild();
         info("tree → " + arg);
         return;
      }
      if (name == "help")     { info(helpText()); return; }
      if (name == "hotkeys")  { info(hotkeysText()); return; }
      info("Unknown command: " + name);
   }

   // ============================ 选择器 ============================
   auto openSelector(SelKind k) -> void {
      sel_ = k;
      selIdx_   = 0;
      selLabels_.clear();
      selCmds_.clear();
      switch (k) {
         case SelKind::Model:
            for (auto const& m : service_.catalog().models) {
               auto const cur = chat_.modelLabel
                              == m.provider + "/" + m.id;
               selLabels_.push_back(
                  std::format("{}{}/{}  {}{}", cur ? "● " : "  ",
                              m.provider, m.id,
                              m.name == m.id ? "" : "(" + m.name + ") ",
                              m.reasoning ? "" : "[no-thinking]"));
               selCmds_.push_back("model " + m.provider + "/" + m.id);
            }
            break;
         case SelKind::Thinking:
            for (auto lv : kThinkingLevels) {
               auto const cur = lv == chat_.thinkingLevel;
               selLabels_.push_back(std::format("{}{}", cur ? "● " : "  ",
                                                lv));
               selCmds_.push_back(std::string{"thinking "} + lv);
            }
            break;
         case SelKind::Session: {
            for (auto const& it : store_.list(session_.cwd())) {
               auto label = !it.name.empty() ? it.name
                            : !it.firstUserText.empty() ? it.firstUserText
                            : it.sessionId;
               if (label.size() > 70) label = label.substr(0, 70) + "…";
               selLabels_.push_back("  " + label + "  (" +
                                    it.timestamp.substr(0, 16) + ")");
               selCmds_.push_back("resume " + it.path);
            }
            if (selLabels_.empty()) { info("resume: 本目录无历史会话");
                                      sel_ = SelKind::None; }
            break;
         }
         case SelKind::Fork:
            for (auto const& e : session_.entries()) {
               auto const* me =
                  std::get_if<domain::MessageEntry>(&e.payload);
               if (!me || me->message.role != domain::Role::User
                   || me->message.text.empty()) continue;
               auto t = me->message.text;
               if (t.size() > 70) t = t.substr(0, 70) + "…";
               selLabels_.push_back("  " + t);
               selCmds_.push_back("fork " + e.id);
            }
            if (selLabels_.empty()) { info("fork: 没有可 fork 的用户消息");
                                      sel_ = SelKind::None; }
            break;
         case SelKind::Tree:
            for (auto const& e : session_.branchEntries()
                                 | std::views::reverse) {
               auto const* me =
                  std::get_if<domain::MessageEntry>(&e.payload);
               if (!me) continue;
               auto t = me->message.text;
               if (t.size() > 60) t = t.substr(0, 60) + "…";
               selLabels_.push_back(std::format("  {}  {}",
                                    rpc::roleToString(me->message.role), t));
               selCmds_.push_back("tree " + e.id);
            }
            if (selLabels_.empty()) { info("tree: 分支为空");
                                      sel_ = SelKind::None; }
            break;
         default: break;
      }
      screen_.PostEvent(ftxui::Event::Custom);
   }

   auto applySel() -> void {
      if (selIdx_ >= 0
          && selIdx_ < static_cast<int>(selCmds_.size()))
         enqueue(selCmds_[selIdx_], ItemKind::Cmd);
      closeSel();
   }
   auto closeSel() -> void {
      sel_ = SelKind::None;
      screen_.PostEvent(ftxui::Event::Custom);
   }
   auto selectorHeader() const -> ftxui::Element {
      using namespace ftxui;
      auto const title =
         sel_ == SelKind::Model    ? " /model — 选择模型 (Enter 应用, Esc 取消)"
         : sel_ == SelKind::Thinking ? " /thinking — 推理级别"
         : sel_ == SelKind::Session  ? " /resume — 恢复会话"
         : sel_ == SelKind::Fork     ? " /fork — 从该消息分出新会话"
                                          : " /tree — 切换分支位置";
      return text(title);
   }

   // ============================ 杂项命令实现 ============================
   auto setThinking(std::string_view level) -> void {
      service_.setThinkingLevel(level);
      session_.appendEntry(domain::Entry{domain::EntryBase{},
         domain::ThinkingLevelEntry{std::string{level}}});
      info(std::format("thinking → {}", level));
   }

   auto onCompactEvent(folly::dynamic const& f) -> void {
      auto const t = f.count("type") ? f["type"].getString() : std::string{};
      if (t == "compaction_start")
         info(std::format("compacting ({})…",
              f.count("reason") ? f["reason"].getString()
                                : std::string{"manual"}));
      else if (t == "compaction_end") {
         if (f.count("errorMessage"))
            info(f["errorMessage"].getString());
         else if (f.count("result")
                  && f["result"].count("tokensBefore"))
            info(std::format("compacted: {} → ~{} tokens",
                 f["result"]["tokensBefore"].asInt(),
                 f["result"]["estimatedTokensAfter"].asInt()));
      }
   }

   auto rebuild() -> void {
      rebuildBlocks(chat_, session_.messages());
      syncSessionName();
   }

   // 模型/思考级别 -> UI 缓存 (worker 线程调用; UI 线程不碰 service)
   auto syncModel() -> void {
      std::lock_guard lk{chat_.mu};
      auto const& m = service_.model();
      chat_.modelLabel     = m.provider + "/" + m.id;
      chat_.thinkingLevel  = service_.thinkingLevel();
      chat_.contextWindow  = m.contextWindow;
      chat_.modelReasoning = m.reasoning;
   }

   auto syncSessionName() -> void {
      std::string name;
      for (auto const& e : session_.entries() | std::views::reverse)
         if (auto const* si
             = std::get_if<domain::SessionInfoEntry>(&e.payload)) {
            name = si->hasName ? si->name : "";
            break;
         }
      { std::lock_guard lk{chat_.mu}; chat_.sessionName = name; }
   }

   auto sessionInfo() const -> std::string {
      return std::format(
         "session: {}\nfile: {}\nmessages: {}  entries: {}\n"
         "model: {}/{}   thinking: {}\nqueued: {}",
         session_.id(),
         session_.filePath().empty() ? "(unsaved)" : session_.filePath(),
         session_.messages().size(), session_.entries().size(),
         service_.model().provider, service_.model().id,
         service_.thinkingLevel(), chat_.queued);
   }

   auto lastAssistantText() const -> std::string {
      for (auto const& m : session_.messages() | std::views::reverse)
         if (m.role == domain::Role::Assistant && !m.text.empty())
            return m.text;
      return {};
   }

   static auto copyToClipboard(std::string const& text) -> void {
      std::string esc;
      for (char c : text) esc += (c == '\'') ? "'\\''" : std::string(1, c);
      infra::runShell("printf '%s' '" + esc + "' | pbcopy", 64);
   }

   auto info(std::string text) -> void {
      {
         std::lock_guard lk{chat_.mu};
         chat_.blocks.push_back({BlockKind::Info, std::move(text),
                                 {}, {}, {}, {}, false, true});
      }
      screen_.PostEvent(ftxui::Event::Custom);
   }

   static auto trim(std::string s) -> std::string {
      auto const b = s.find_first_not_of(" \t");
      if (b == std::string::npos) return {};
      auto const e = s.find_last_not_of(" \t");
      return s.substr(b, e - b + 1);
   }

   static auto helpText() -> std::string {
      return
         "/model [p/m]     选择/切换模型 (无参开选择器, ^P 循环)\n"
         "/thinking [lv]   推理级别 off|minimal|low|medium|high|xhigh|max\n"
         "/new             新会话\n"
         "/compact [ins]   手动压缩上下文\n"
         "/name <n>        会话命名\n"
         "/session         会话信息\n"
         "/export [path]   导出 HTML\n"
         "/copy            复制最近回复到剪贴板\n"
         "/resume [file]   恢复历史会话\n"
         "/fork            从某条用户消息分出新会话\n"
         "/clone           复制当前会话\n"
         "/tree            切换分支位置\n"
         "/hotkeys         快捷键\n"
         "/q               退出";
   }

   static auto hotkeysText() -> std::string {
      return
         "ctrl+p     循环模型 (同 provider)\n"
         "alt+p      反向循环模型\n"
         "ctrl+l     模型选择器\n"
         "shift+tab  循环 thinking 级别\n"
         "ctrl+t     思维块显隐\n"
         "ctrl+o     工具输出展开/折叠\n"
         "esc        中止当前 turn / 关闭选择器\n"
         "ctrl+c     退出";
   }

   // ============================ 渲染 ============================
   // markdown-lite: ```code → cyan 等宽缩进; #标题 → bold; >引用 → dim;
   // **bold** / `code` 行内 span; 其余段落原文.
   static auto mdInline(std::string_view line) -> ftxui::Element {
      using namespace ftxui;
      Elements spans;
      std::string cur;
      auto flush = [&] {
         if (!cur.empty()) { spans.push_back(text(cur)); cur.clear(); }
      };
      for (size_t i = 0; i < line.size();) {
         if (line.substr(i, 2) == "**") {
            if (auto e = line.find("**", i + 2); e != std::string_view::npos) {
               flush();
               spans.push_back(text(std::string{line.substr(i + 2, e - i - 2)})
                               | bold);
               i = e + 2; continue;
            }
         }
         if (line[i] == '`') {
            if (auto e = line.find('`', i + 1); e != std::string_view::npos) {
               flush();
               spans.push_back(text(std::string{line.substr(i + 1, e - i - 1)})
                               | color(Color::Cyan));
               i = e + 1; continue;
            }
         }
         cur += line[i++];
      }
      flush();
      return hbox(std::move(spans)) | xflex;
   }

   static auto mdText(std::string_view text) -> ftxui::Elements {
      using namespace ftxui;
      Elements out;
      bool inCode = false;
      size_t pos = 0;
      while (pos <= text.size()) {
         auto const nl = text.find('\n', pos);
         auto const line = text.substr(
            pos, nl == std::string_view::npos ? nl : nl - pos);
         if (line.starts_with("```")) {
            inCode = !inCode;
            out.push_back(ftxui::text(std::string{line}) | dim);
         } else if (inCode) {
            out.push_back(ftxui::text("  " + std::string{line})
                          | color(Color::Cyan));
         } else if (line.starts_with("###")) {
            out.push_back(mdInline(line.substr(3)) | bold);
         } else if (line.starts_with("##")) {
            out.push_back(mdInline(line.substr(2)) | bold);
         } else if (line.starts_with("#")) {
            out.push_back(mdInline(line.substr(1)) | bold);
         } else if (line.starts_with("> ")) {
            out.push_back(ftxui::paragraph(std::string{line.substr(2)})
                          | dim | ftxui::italic);
         } else {
            // 普通段落: 有行内标记走 span 行 (不折行), 否则段落折行
            if (line.find("**") != std::string_view::npos
                || line.find('`') != std::string_view::npos)
               out.push_back(mdInline(line));
            else
               out.push_back(ftxui::paragraph(std::string{line}));
         }
         if (nl == std::string_view::npos) break;
         pos = nl + 1;
      }
      return out;
   }

   // 工具参数摘要 (pi tool-execution 的单行概览):
   //   bash→command; read/edit/write→path; grep/find→pattern+path; 其他→截断
   static auto toolArgs(std::string_view name,
                        std::string_view argsJson) -> std::string {
      auto pj = infra::parseJsonSafe(argsJson);
      auto field = [&](char const* k) -> std::string {
         if (!pj || !pj->count(k) || !(*pj)[k].isString()) return {};
         return (*pj)[k].getString();
      };
      std::string s;
      if (name == "bash")                s = field("command");
      else if (name == "read" || name == "edit" || name == "write"
               || name == "ls")          s = field("path");
      else if (name == "grep" || name == "find") {
         s = field("pattern");
         if (auto p = field("path"); !p.empty()) s += " " + p;
      } else {
         s = std::string{argsJson};
      }
      std::string one;
      for (char c : s) one += (c == '\n') ? ' ' : c;
      if (one.size() > 90) one = one.substr(0, 90) + "…";
      return one;
   }

   auto renderBlock(ChatBlock const& b) const -> ftxui::Element {
      using namespace ftxui;
      switch (b.kind) {
         case BlockKind::User:
            return vbox(text(""),
                        hbox(text(" ❯ ") | color(Color::Green) | bold,
                             paragraph(b.text)));
         case BlockKind::Assistant: {
            Elements es;
            if (!b.thinking.empty()) {
               if (chat_.showThinking) {
                  es.push_back(text("thinking") | dim | italic);
                  es.push_back(paragraph(b.thinking) | dim | italic);
               } else {
                  // pi 折叠态: 只留标签
                  es.push_back(text("[thinking]") | dim | italic);
               }
            }
            Element body = vbox(mdText(b.text));
            if (!b.done)
               body = vbox(std::move(body), text("▌") | blink);
            if (b.isError)
               body = std::move(body) | color(Color::Red);
            es.push_back(hbox(text("   "), std::move(body) | ftxui::xflex));
            return vbox(std::move(es));
         }
         case BlockKind::ToolExec: {
            Element head = hbox(text(" ⚙ "),
                                text(b.toolName) | bold | color(Color::Cyan),
                                separator(),
                                paragraph(toolArgs(b.toolName, b.args)) | dim);
            if (!b.done) return vbox(std::move(head), text("   …") | dim);
            // pi 折叠语义: 输出超过 6 行截断显示 (ctrl+o 展开)
            auto shown = b.text;
            if (!chat_.expandTools) {
               size_t lines = 0, cut = std::string::npos;
               for (size_t i = 0; i < shown.size(); ++i)
                  if (shown[i] == '\n' && ++lines == 6) { cut = i; break; }
               if (cut != std::string::npos)
                  shown = shown.substr(0, cut)
                        + "\n… (^O 展开 " + std::to_string(lines) + "+ 行)";
            }
            Element out = paragraph(shown.empty() ? "(no output)" : shown);
            return vbox(std::move(head),
                        hbox(text("   "),
                             std::move(out) | ftxui::xflex
                                | (b.isError ? color(Color::Red) : dim)));
         }
         case BlockKind::Info:
         default:
            return paragraph(b.text) | dim;
      }
   }

   auto renderFooter() const -> ftxui::Element {
      using namespace ftxui;
      std::lock_guard lk{chat_.mu};
      auto status = chat_.running ? text(" running…") | color(Color::Yellow)
                                  : text(" idle") | dim;
      auto const ctxPct = chat_.contextWindow > 0
         ? chat_.lastUsage.totalTokens * 100 / chat_.contextWindow : 0;
      Elements es{
         text(" " + chat_.modelLabel) | bold,
         separator(),
         text(" " + chat_.thinkingLevel) | dim,
         separator(), std::move(status), separator(),
         text(" queued:" + std::to_string(chat_.queued)) | dim,
         separator(),
         text(" in:" + std::to_string(chat_.lastUsage.input)
              + " out:" + std::to_string(chat_.lastUsage.output)) | dim,
         separator(),
         text(" ctx:" + std::to_string(ctxPct) + "%") | dim,
      };
      if (!chat_.sessionName.empty()) {
         es.push_back(separator());
         es.push_back(text(" " + chat_.sessionName) | dim | italic);
      }
      return hbox(std::move(es));
   }

   AgentSessionService&      service_;
   domain::Session&          session_;
   domain::ISessionStore&    store_;
   infra::EventLoop&         loop_;
   infra::ThreadPool&        pool_;

   ChatModel                 chat_;
   ftxui::ScreenInteractive  screen_;
   TuiEventSink              sink_;
   CompactRunner             compact_;

   std::string               input_;
   // 以下全部只在 loop 线程被触碰 (quit_ 是 UI→loop 的跨线程旗标)
   std::deque<QueuedItem>    pending_;
   bool                      turnBusy_ = false;   // run 在飞
   bool                      ioBusy_   = false;   // 异步 load 在飞
   std::atomic<bool>         quit_{false};
   std::jthread              loopThread_;

   // 选择器状态 (仅 UI 线程读写)
   SelKind                   sel_ = SelKind::None;
   std::vector<std::string>  selLabels_;
   std::vector<std::string>  selCmds_;
   int                       selIdx_ = 0;
   ftxui::Component          menu_;
};

} // namespace pi::iface::tui
