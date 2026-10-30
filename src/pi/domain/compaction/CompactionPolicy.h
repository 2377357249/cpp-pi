#pragma once

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <pi/domain/session/EntryProjection.h>
#include <pi/domain/session/Session.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 压缩策略 —— pi core/compaction/compaction.ts 的纯函数移植.
//   切点查找 / token 估算 / 会话序列化 / 提示词常量 / 文件操作提取.
// 不碰 LLM/IO; 调用方 (interface) 拿 Preparation 去发摘要请求.
// ---------------------------------------------------------------------------

inline constexpr int64_t kReserveTokens      = 16384;
inline constexpr int64_t kKeepRecentTokens   = 20000;
inline constexpr size_t  kToolResultMaxChars = 2000;
inline constexpr int64_t kCharsPerToken      = 4;

struct CompactionSettings {
   bool    enabled         = true;
   int64_t reserveTokens   = kReserveTokens;
   int64_t keepRecentTokens = kKeepRecentTokens;
};

// --- pi messages.ts: 摘要消息注入前缀 ---
inline constexpr std::string_view kCompactionSummaryPrefix =
   "The conversation history before this point was compacted into the "
   "following summary:\n\n<summary>\n";
inline constexpr std::string_view kCompactionSummarySuffix = "\n</summary>";
inline constexpr std::string_view kBranchSummaryPrefix =
   "The following is a summary of a branch that this conversation came "
   "back from:\n\n<summary>\n";
inline constexpr std::string_view kBranchSummarySuffix = "</summary>";

// --- pi compaction.ts 提示词 (逐字) ---
inline constexpr std::string_view kSummarizationSystemPrompt =
   "You are a context summarization assistant. Your task is to read a "
   "conversation between a user and an AI assistant, then produce a "
   "structured summary following the exact format specified.\n\n"
   "Do NOT continue the conversation. Do NOT respond to any questions in "
   "the conversation. ONLY output the structured summary.";

inline constexpr std::string_view kSummarizationPrompt =
   "The messages above are a conversation to summarize. Create a "
   "structured context checkpoint summary that another LLM will use to "
   "continue the work.\n\n"
   "Use this EXACT format:\n\n"
   "## Goal\n"
   "[What is the user trying to accomplish? Can be multiple items if the "
   "session covers different tasks.]\n\n"
   "## Constraints & Preferences\n"
   "- [Any constraints, preferences, or requirements mentioned by user]\n"
   "- [Or \"(none)\" if none were mentioned]\n\n"
   "## Progress\n"
   "### Done\n"
   "- [x] [Completed tasks/changes]\n\n"
   "### In Progress\n"
   "- [ ] [Current work]\n\n"
   "### Blocked\n"
   "- [Issues preventing progress, if any]\n\n"
   "## Key Decisions\n"
   "- **[Decision]**: [Brief rationale]\n\n"
   "## Next Steps\n"
   "1. [Ordered list of what should happen next]\n\n"
   "## Critical Context\n"
   "- [Any data, examples, or references needed to continue]\n"
   "- [Or \"(none)\" if not applicable]\n\n"
   "Keep each section concise. Preserve exact file paths, function names, "
   "and error messages.";

inline constexpr std::string_view kUpdateSummarizationPrompt =
   "The messages above are NEW conversation messages to incorporate into "
   "the existing summary provided in <previous-summary> tags.\n\n"
   "Update the existing structured summary with new information. RULES:\n"
   "- PRESERVE all existing information from the previous summary\n"
   "- ADD new progress, decisions, and context from the new messages\n"
   "- UPDATE the Progress section: move items from \"In Progress\" to "
   "\"Done\" when completed\n"
   "- UPDATE \"Next Steps\" based on what was accomplished\n"
   "- PRESERVE exact file paths, function names, and error messages\n"
   "- If something is no longer relevant, you may remove it\n\n"
   "Use this EXACT format:\n\n"
   "## Goal\n"
   "[Preserve existing goals, add new ones if the task expanded]\n\n"
   "## Constraints & Preferences\n"
   "- [Preserve existing, add new ones discovered]\n\n"
   "## Progress\n"
   "### Done\n"
   "- [x] [Include previously done items AND newly completed items]\n\n"
   "### In Progress\n"
   "- [ ] [Current work - update based on progress]\n\n"
   "### Blocked\n"
   "- [Current blockers - remove if resolved]\n\n"
   "## Key Decisions\n"
   "- **[Decision]**: [Brief rationale] (preserve all previous, add new)\n\n"
   "## Next Steps\n"
   "1. [Update based on current state]\n\n"
   "## Critical Context\n"
   "- [Preserve important context, add new if needed]\n\n"
   "Keep each section concise. Preserve exact file paths, function names, "
   "and error messages.";

inline constexpr std::string_view kTurnPrefixPrompt =
   "The messages above are earlier context from an ongoing conversation. "
   "Later messages are stored separately and do not need to be "
   "reconstructed.\n\n"
   "Create a concise checkpoint of the user's request and the progress "
   "shown above. This checkpoint will be placed before the later messages "
   "so the conversation can continue with the necessary context.\n\n"
   "## Original Request\n"
   "[What did the user ask for?]\n\n"
   "## Progress So Far\n"
   "- [Key decisions and work completed in these messages]\n\n"
   "## Context Needed to Continue\n"
   "- [Information from these messages needed to understand the later "
   "work]\n\n"
   "Only summarize information explicitly present above. Do not infer or "
   "recreate later messages.";

// ---------------------------------------------------------------------------
// estimateTokens (pi: ceil(chars/4), 保守高估)
// ---------------------------------------------------------------------------
inline auto estimateTokens(Message const& m) -> int64_t {
   size_t chars = 0;
   switch (m.role) {
      case Role::User:
      case Role::Tool:
      case Role::Custom:
      case Role::System:
         chars = m.text.size() + m.contentJson.size();
         break;
      case Role::Assistant:
         chars += m.text.size();
         for (auto const& t : m.thinking)    chars += t.thinking.size();
         for (auto const& tc : m.toolCalls)
            chars += tc.tool.size() + tc.input.size();
         break;
      case Role::BashExecution:
         chars = m.command.size() + m.text.size();
         break;
      case Role::BranchSummary:
      case Role::CompactionSummary:
         chars = m.summary.size();
         break;
   }
   return static_cast<int64_t>((chars + kCharsPerToken - 1) / kCharsPerToken);
}

// pi calculateContextTokens + estimateContextTokens:
// 有最后一条有效 usage 的 assistant → usageTokens + 其后消息估算; 否则全估.
inline auto estimateContextTokens(std::vector<Message> const& msgs)
   -> int64_t {
   int64_t lastUsageIdx = -1;
   int64_t usageTokens  = 0;
   for (int64_t i = static_cast<int64_t>(msgs.size()) - 1; i >= 0; --i) {
      auto const& m = msgs[static_cast<size_t>(i)];
      if (m.role != Role::Assistant) continue;
      auto const t = m.usage.totalTokens != 0
            ? m.usage.totalTokens
            : m.usage.input + m.usage.output + m.usage.cacheRead
              + m.usage.cacheWrite;
      if (t <= 0) continue;
      usageTokens  = t;
      lastUsageIdx = i;
      break;
   }
   if (lastUsageIdx < 0) {
      int64_t est = 0;
      for (auto const& m : msgs) est += estimateTokens(m);
      return est;
   }
   int64_t trailing = 0;
   for (size_t i = static_cast<size_t>(lastUsageIdx) + 1; i < msgs.size(); ++i)
      trailing += estimateTokens(msgs[i]);
   return usageTokens + trailing;
}

// ---------------------------------------------------------------------------
// serializeConversation (pi: convertToLlm 后的文本化, thinking 省略)
// ---------------------------------------------------------------------------
inline auto bashExecutionToText(Message const& m) -> std::string {
   auto text = std::format("Ran `{}`\n", m.command);
   if (!m.text.empty()) text += std::format("```\n{}\n```", m.text);
   else                 text += "(no output)";
   if (m.cancelled)          text += "\n\n(command cancelled)";
   else if (m.exitCode != -9999 && m.exitCode != 0)
      text += std::format("\n\nCommand exited with code {}", m.exitCode);
   if (m.truncated && !m.fullOutputPath.empty())
      text += std::format("\n\n[Output truncated. Full output: {}]",
                          m.fullOutputPath);
   return text;
}

// convertToLlm: 自定义角色 -> user 文本 (excludeFromContext 的丢弃)
inline auto llmText(Message const& m) -> std::optional<std::string> {
   switch (m.role) {
      case Role::User:
      case Role::Tool:
         return m.text;
      case Role::Assistant:
         return std::nullopt;   // 走专用分支
      case Role::BashExecution:
         if (m.excludeFromContext) return std::nullopt;
         return bashExecutionToText(m);
      case Role::Custom: {
         // contentJson 是 content 线格式; 字符串则去掉引号
         if (m.contentJson.size() >= 2 && m.contentJson.front() == '"'
             && m.contentJson.back() == '"')
            return m.contentJson.substr(1, m.contentJson.size() - 2);
         return m.contentJson.empty() ? std::optional<std::string>{}
                                      : m.contentJson;
      }
      case Role::BranchSummary:
         return std::string{kBranchSummaryPrefix} + m.summary
                + std::string{kBranchSummarySuffix};
      case Role::CompactionSummary:
         return std::string{kCompactionSummaryPrefix} + m.summary
                + std::string{kCompactionSummarySuffix};
      case Role::System:
         return std::nullopt;
   }
   return std::nullopt;
}

inline auto truncateForSummary(std::string_view text) -> std::string {
   if (text.size() <= kToolResultMaxChars) return std::string{text};
   return std::format("{}\n\n[... {} more characters truncated]",
                      text.substr(0, kToolResultMaxChars),
                      text.size() - kToolResultMaxChars);
}

inline auto serializeConversation(std::vector<Message> const& msgs)
   -> std::string {
   std::string out;
   auto line = [&out](std::string_view s) {
      if (!out.empty()) out += "\n\n";
      out += s;
   };
   for (auto const& m : msgs) {
      switch (m.role) {
         case Role::Assistant: {
            if (!m.text.empty()) line(std::format("[Assistant]: {}", m.text));
            if (!m.toolCalls.empty()) {
               std::string calls;
               for (auto const& tc : m.toolCalls) {
                  if (!calls.empty()) calls += "; ";
                  calls += std::format("{}({})", tc.tool, tc.input);
               }
               line(std::format("[Assistant tool calls]: {}", calls));
            }
            break;
         }
         case Role::Tool: {
            auto t = llmText(m);
            if (t && !t->empty()) line(std::format("[Tool result]: {}",
                                       truncateForSummary(*t)));
            break;
         }
         case Role::System: break;
         default: {
            auto t = llmText(m);
            if (t && !t->empty())
               line(std::format("[User]: {}", *t));
            break;
         }
      }
   }
   return out;
}

// pi promptText 组装: <conversation>..</conversation> + <previous-summary>?
// + base prompt + "Additional focus: X" 尾注
inline auto buildSummarizationPrompt(std::vector<Message> const& msgs,
                                     std::string_view previousSummary,
                                     std::string_view customInstructions)
   -> std::string {
   auto promptText = std::format("<conversation>\n{}\n</conversation>\n\n",
                                 serializeConversation(msgs));
   if (!previousSummary.empty())
      promptText += std::format("<previous-summary>\n{}\n</previous-summary>\n\n",
                                previousSummary);
   promptText += previousSummary.empty()
                     ? std::string{kSummarizationPrompt}
                     : std::string{kUpdateSummarizationPrompt};
   if (!customInstructions.empty())
      promptText += std::format("\n\nAdditional focus: {}", customInstructions);
   return promptText;
}

inline auto buildTurnPrefixPrompt(std::vector<Message> const& msgs,
                                  std::string_view customInstructions)
   -> std::string {
   auto p = std::format("# Conversation\n{}\n\n# Instructions\n{}",
                        serializeConversation(msgs), kTurnPrefixPrompt);
   if (!customInstructions.empty())
      p += std::format("\n\nAdditional focus: {}", customInstructions);
   return p;
}

// pi getSummarizationFailure
inline auto summarizationFailure(CompletionResult const& r,
                                 std::string_view label)
   -> std::optional<std::string> {
   if (r.stopReason == StopReason::Error)
      return std::format("{} failed: {}", label,
                         r.message.errorMessage.empty()
                            ? "Unknown error" : r.message.errorMessage);
   if (r.stopReason == StopReason::Length)
      return std::format("{} failed: generation hit the token cap and the "
                         "summary is incomplete", label);
   if (!r.message.toolCalls.empty())
      return std::format("{} attempted to call a tool", label);
   return std::nullopt;
}

// ---------------------------------------------------------------------------
// 切点/准备 (pi prepareCompaction)
// ---------------------------------------------------------------------------
inline auto isCutPointRole(Role r) -> bool {
   switch (r) {
      case Role::User:
      case Role::Assistant:
      case Role::BashExecution:
      case Role::Custom:
      case Role::BranchSummary:
      case Role::CompactionSummary: return true;
      default: return false;   // toolResult/system 不可切
   }
}
inline auto isTurnStartRole(Role r) -> bool {
   switch (r) {
      case Role::User:
      case Role::BashExecution:
      case Role::Custom:
      case Role::BranchSummary:
      case Role::CompactionSummary: return true;
      default: return false;   // assistant/toolResult 不算轮次起点
   }
}

struct CompactionPreparation {
   std::string          firstKeptEntryId;
   std::vector<Message> messagesToSummarize;
   std::vector<Message> turnPrefixMessages;
   bool                 isSplitTurn   = false;
   int64_t              tokensBefore  = 0;
   std::string          previousSummary;
   std::set<std::string> filesRead;
   std::set<std::string> filesModified;
};

// 返回 nullopt: 尾条已是 compaction (报 "Already compacted") 或无可摘要内容
// (报 "Nothing to compact (session too small)"). 由调用方区分.
inline auto prepareCompaction(Session const&           session,
                              CompactionSettings const& settings = {})
   -> std::optional<CompactionPreparation> {
   auto const path = session.branchEntries();
   if (!path.empty()
       && std::holds_alternative<CompactionEntry>(path.back().payload))
      return std::nullopt;

   // context_edit nullReplacement -> 该目标 entry 不投影消息
   std::set<std::string> omitted;
   for (auto const& e : path)
      if (auto const* ce = std::get_if<ContextEditEntry>(&e.payload);
          ce && ce->nullReplacement)
         omitted.insert(ce->targetId);

   // 每条 entry 的投影消息 (含 compaction 的 systemMessage+summary)
   auto proj = std::vector<std::vector<Message>>(path.size());
   for (size_t i = 0; i < path.size(); ++i)
      if (!omitted.contains(path[i].id))
         entryContextMessages(path[i], proj[i], isoToMs(path[i].timestampIso));

   // 最新 compaction: 从它之后开始; previousSummary 取其 summary
   size_t boundaryStart = 0;
   std::string previousSummary;
   for (size_t i = path.size(); i-- > 0;)
      if (std::holds_alternative<CompactionEntry>(path[i].payload)) {
         previousSummary =
            std::get<CompactionEntry>(path[i].payload).summary;
         boundaryStart = i + 1;
         break;
      }
   auto const boundaryEnd = path.size();

   // 投影总 tokens (usage 优先 + 尾部估算)
   std::vector<Message> flat;
   for (size_t i = 0; i < boundaryEnd; ++i)
      for (auto const& m : proj[i]) flat.push_back(m);
   auto const tokensBefore = estimateContextTokens(flat);

   // 切点 = 投影出 cut-point 消息的 entry (toolResult 永不切)
   std::vector<size_t> cutPoints;
   for (size_t i = boundaryStart; i < boundaryEnd; ++i)
      if (!std::holds_alternative<CompactionEntry>(path[i].payload)
          && std::ranges::any_of(proj[i], [](Message const& m) {
                return isCutPointRole(m.role); }))
         cutPoints.push_back(i);

   size_t cutIndex = boundaryStart;
   if (!cutPoints.empty()) {
      cutIndex = cutPoints.front();
      int64_t acc = 0;
      for (size_t i = boundaryEnd; i-- > boundaryStart;) {
         int64_t t = 0;
         for (auto const& m : proj[i]) t += estimateTokens(m);
         if (t == 0) continue;
         acc += t;
         if (acc >= settings.keepRecentTokens) {
            auto const it = std::ranges::find_if(
               cutPoints, [i](size_t c) { return c >= i; });
            cutIndex = it != cutPoints.end() ? *it : cutPoints.back();
            break;
         }
      }
      // 跳过投影为空的 entry (pi while loop)
      while (cutIndex > boundaryStart) {
         auto const& prev = path[cutIndex - 1];
         if (std::holds_alternative<CompactionEntry>(prev.payload)
             || !proj[cutIndex - 1].empty())
            break;
         --cutIndex;
      }
   }

   // 切点 entry 是否轮次起点; 否则回溯找轮次起点 (split turn)
   auto startsTurn = std::ranges::any_of(
      proj[cutIndex], [](Message const& m) { return isTurnStartRole(m.role); });
   size_t turnStartIndex = boundaryEnd;   // npos
   if (!startsTurn)
      for (size_t i = cutIndex + 1; i-- > boundaryStart;)
         if (std::ranges::any_of(proj[i], [](Message const& m) {
                return isTurnStartRole(m.role); })) {
            turnStartIndex = i;
            break;
         }
   auto const isSplit = !startsTurn && turnStartIndex != boundaryEnd;

   CompactionPreparation p;
   p.firstKeptEntryId = path[cutIndex].id;
   p.isSplitTurn      = isSplit;
   p.tokensBefore     = tokensBefore;
   p.previousSummary  = std::move(previousSummary);

   auto const historyEnd = isSplit ? turnStartIndex : cutIndex;
   for (size_t i = boundaryStart; i < historyEnd; ++i)
      for (auto const& m : proj[i]) p.messagesToSummarize.push_back(m);
   if (isSplit)
      for (size_t i = turnStartIndex; i < cutIndex; ++i)
         for (auto const& m : proj[i]) p.turnPrefixMessages.push_back(m);

   if (p.messagesToSummarize.empty() && p.turnPrefixMessages.empty())
      return std::nullopt;

   // 文件操作提取: assistant toolCall {read,write,edit} 的 path 参数.
   // input 是 JSON 串 — 轻扫 "path"":" 足够 (编码层会重解析校验).
   auto addOp = [&p](std::string_view tool, std::string_view input) {
      auto const key = input.find("\"path\"");
      if (key == std::string_view::npos) return;
      auto const colon = input.find(':', key + 6);
      auto const q1    = input.find('"', colon + 1);
      if (q1 == std::string_view::npos) return;
      auto const q2 = input.find('"', q1 + 1);
      if (q2 == std::string_view::npos) return;
      auto const f = std::string{input.substr(q1 + 1, q2 - q1 - 1)};
      if (f.empty()) return;
      if (tool == "read")                  p.filesRead.insert(f);
      else if (tool == "write" || tool == "edit") p.filesModified.insert(f);
   };
   for (auto const& m : p.messagesToSummarize)
      if (m.role == Role::Assistant)
         for (auto const& tc : m.toolCalls) addOp(tc.tool, tc.input);
   for (auto const& m : p.turnPrefixMessages)
      if (m.role == Role::Assistant)
         for (auto const& tc : m.toolCalls) addOp(tc.tool, tc.input);
   return p;
}

// pi computeFileLists + formatFileOperations (read-only 排除已修改)
inline auto formatFileOperations(std::set<std::string> const& read,
                                 std::set<std::string> const& modified)
   -> std::string {
   std::vector<std::string> readOnly;
   for (auto const& f : read)
      if (!modified.contains(f)) readOnly.push_back(f);
   std::string out;
   if (!readOnly.empty()) {
      out += "\n\n<read-files>\n";
      for (auto const& f : readOnly) { out += f; out += '\n'; }
      out += "</read-files>";
   }
   if (!modified.empty()) {
      out += "\n\n<modified-files>\n";
      for (auto const& f : modified) { out += f; out += '\n'; }
      out += "</modified-files>";
   }
   return out;
}

} // namespace pi::domain
