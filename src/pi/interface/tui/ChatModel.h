#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <pi/domain/event/IAgentEventSink.h>
#include <pi/domain/session/Session.h>

namespace pi::iface::tui {

// ---------------------------------------------------------------------------
// 聊天视图模型: 渲染线程只读 blocks; sink 在 worker 线程经 mu 改写.
// 一块 = 用户消息 / assistant(流式正文+thinking) / 一次工具执行 / 提示行.
// ---------------------------------------------------------------------------
enum class BlockKind { User, Assistant, ToolExec, Info };

struct ChatBlock {
   BlockKind   kind = BlockKind::Info;
   std::string text;                    // user 原文 / assistant 正文 / info
   std::string thinking;                // assistant thinking (dim 渲染)
   std::string toolCallId, toolName, args;   // ToolExec 专用
   bool        isError = false;
   bool        done    = true;          // Assistant: 流式中=false
};

struct ChatModel {
   mutable std::mutex    mu;
   std::deque<ChatBlock> blocks;
   bool                  running = false;   // run 进行中 (agent_start/end)
   size_t                queued  = 0;       // 等待中的用户输入
   domain::Usage         lastUsage;         // 最近增量事件的 usage 快照
   bool                  showThinking = true;   // ctrl+t 切换 (pi 语义)
   bool                  expandTools   = false; // ctrl+o 展开工具输出 (pi 语义)
   std::string           sessionName;         // footer 显示 (session_info)
   // 模型快照: service_.model()/thinkingLevel() 的 UI 读缓存 —
   // UI 线程不碰 service (worker 独占写), syncModel() 在 worker 侧更新.
   std::string           modelLabel, thinkingLevel;
   int64_t               contextWindow  = 0;
   bool                  modelReasoning = false;
};

// 会话消息 -> 聊天块重建 (switch/resume/fork/tree/new 后调用; worker 线程,
// 持 mu 写). 与 sink 增量路径同构: emitted 形状与持久化消息一致.
inline auto rebuildBlocks(ChatModel&           m,
                          std::vector<domain::Message> const& msgs) -> void {
   std::lock_guard lk{m.mu};
   m.blocks.clear();
   for (auto const& msg : msgs) {
      switch (msg.role) {
         case domain::Role::User:
            m.blocks.push_back({BlockKind::User, msg.text, {}, {}, {}, {},
                                false, true});
            break;
         case domain::Role::Assistant: {
            ChatBlock b{BlockKind::Assistant, msg.text, {}, {}, {}, {},
                        false, true};
            for (auto const& t : msg.thinking) b.thinking += t.thinking;
            m.blocks.push_back(std::move(b));
            break;
         }
         case domain::Role::Tool:
            m.blocks.push_back({BlockKind::ToolExec, msg.text, {},
                                msg.toolCallId, msg.toolName, {},
                                msg.isError, true});
            break;
         case domain::Role::BashExecution:
            m.blocks.push_back({BlockKind::ToolExec, msg.text, {},
                                {}, "bash", msg.command,
                                msg.exitCode != 0 && msg.exitCode != -9999,
                                true});
            break;
         case domain::Role::CompactionSummary:
            m.blocks.push_back({BlockKind::Info,
                                "— compacted: " + msg.summary.substr(0, 80),
                                {}, {}, {}, {}, false, true});
            break;
         default: break;
      }
   }
   m.sessionName.clear();
   m.lastUsage = {};
}

// ---------------------------------------------------------------------------
// IAgentEventSink 的 TUI 实现: domain 事件 -> ChatModel 改写 + refresh 通知.
// 与 RpcEventSink 同位: 端口翻译, 不写协议帧.
// ---------------------------------------------------------------------------
class TuiEventSink : public domain::IAgentEventSink {
public:
   using Refresh = std::function<void()>;

   TuiEventSink(ChatModel& m, Refresh refresh)
      : m_(m), refresh_(std::move(refresh)) {}

   auto agentStart() -> void override {
      { std::lock_guard lk{m_.mu}; m_.running = true; }
      refresh_();
   }

   auto turnStart() -> void override { refresh_(); }

   auto messageStart(domain::Message const& m) -> void override {
      {
         std::lock_guard lk{m_.mu};
         switch (m.role) {
            case domain::Role::User:
               m_.blocks.push_back({BlockKind::User, m.text, {}, {}, {}, {},
                                    false, true});
               break;
            case domain::Role::Assistant:
               m_.blocks.push_back({BlockKind::Assistant, m.text, {}, {},
                                    {}, {}, false, false});
               currentAssistant_ = m_.blocks.size() - 1;
               break;
            default: break;   // toolResult 走 tool_execution_end 渲染
         }
      }
      refresh_();
   }

   auto messageUpdate(domain::Usage const&              usage,
                      domain::AssistantStreamEvent const& e)
      -> void override {
      {
         std::lock_guard lk{m_.mu};
         m_.lastUsage = usage;
         if (currentAssistant_ < m_.blocks.size()) {
            auto& b = m_.blocks[currentAssistant_];
            using T = domain::AssistantStreamType;
            if (e.type == T::TextDelta)     b.text += e.delta;
            if (e.type == T::ThinkingDelta) b.thinking += e.delta;
         }
      }
      refresh_();
   }

   auto messageEnd(domain::Message const& m) -> void override {
      {
         std::lock_guard lk{m_.mu};
         // 终态 assistant 携带累计 usage; 其他角色不覆盖
         if (m.usage.totalTokens > 0) m_.lastUsage = m.usage;
         if (m.role == domain::Role::Assistant
             && currentAssistant_ < m_.blocks.size()) {
            auto& b   = m_.blocks[currentAssistant_];
            b.text    = m.text;
            b.thinking.clear();
            for (auto const& t : m.thinking) b.thinking += t.thinking;
            b.done    = true;
            if (!m.errorMessage.empty()) b.isError = true;
         }
      }
      refresh_();
   }

   auto toolExecutionStart(std::string_view toolCallId,
                           std::string_view toolName,
                           std::string_view argsJson) -> void override {
      {
         std::lock_guard lk{m_.mu};
         m_.blocks.push_back({BlockKind::ToolExec, {}, {},
                              std::string{toolCallId},
                              std::string{toolName},
                              std::string{argsJson}, false, false});
      }
      refresh_();
   }

   auto toolExecutionEnd(std::string_view toolCallId, std::string_view,
                         domain::Message const& toolResultMsg)
      -> void override {
      {
         std::lock_guard lk{m_.mu};
         for (auto& b : m_.blocks | std::views::reverse) {
            if (b.kind == BlockKind::ToolExec && b.toolCallId == toolCallId) {
               b.text    = toolResultMsg.text;
               b.isError = toolResultMsg.isError;
               b.done    = true;
               break;
            }
         }
      }
      refresh_();
   }

   auto turnEnd(domain::Message const&,
                std::vector<domain::Message> const&) -> void override {
      refresh_();
   }

   auto agentEnd(std::vector<domain::Message> const&, bool) -> void override {
      { std::lock_guard lk{m_.mu}; m_.running = false; }
      refresh_();
   }

   // auto-retry -> 提示块 (pi TUI 的 hint 对应物)
   auto autoRetryStart(size_t attempt, size_t, int64_t delayMs,
                       std::string_view errorMessage) -> void override {
      { std::lock_guard lk{m_.mu};
        m_.blocks.push_back({BlockKind::Info,
            std::format("auto-retry #{} in {}ms ({})", attempt, delayMs,
                        errorMessage), {}, {}, {}, {}, false, true}); }
      refresh_();
   }
   auto autoRetryEnd(bool success, size_t attempt,
                     std::string_view finalError) -> void override {
      { std::lock_guard lk{m_.mu};
        m_.blocks.push_back({BlockKind::Info,
            success ? std::format("auto-retry #{} succeeded", attempt)
                    : std::format("auto-retry #{} failed: {}", attempt,
                                  finalError),
            {}, {}, {}, {}, false, true}); }
      refresh_();
   }

   // 前端维护的排队数 (非端口事件)
   auto setQueued(size_t n) -> void {
      { std::lock_guard lk{m_.mu}; m_.queued = n; }
      refresh_();
   }

private:
   ChatModel& m_;
   Refresh    refresh_;
   size_t     currentAssistant_ = SIZE_MAX;
};

} // namespace pi::iface::tui
