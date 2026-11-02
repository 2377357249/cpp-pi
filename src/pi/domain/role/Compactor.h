#pragma once

#include <format>
#include <string>
#include <utility>
#include <vector>

#include <cub/dci/Role.h>

#include <folly/json/json.h>

#include <pi/domain/compaction/CompactionPolicy.h>
#include <pi/domain/context/CompactCtx.h>
#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/ModelCtx.h>
#include <pi/domain/context/SessionCtx.h>
#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/event/Events.h>
#include <pi/domain/llm/ILlmProvider.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// role: 压缩摘要的业务 (pi compaction.ts 的 LLM 段).
//   beginSummarize 按 ctx.phase 选 prompt 发起 completion, done 回 post
//   EV_LLM_DONE; settleSummarize 校验结果 (summarizationFailure) 推进 phase;
//   assemble 合成最终摘要 + CompactionEntry 落聚合 —— 持久化由 Persister 接力.
//   摘要流不经 agent 事件转发 (pi: 摘要请求不上 message_* 帧).
// ---------------------------------------------------------------------------
struct Compactor {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(TurnStateCtx);
   USE_ROLE(SessionCtx);
   USE_ROLE(ModelCtx);
   USE_ROLE(CompactCtx);

   explicit Compactor(domain::ILlmProvider& llm) : llm_(llm) {}

   auto beginSummarize() -> void {
      auto& cx  = ROLE(CompactCtx);
      auto& sel = ROLE(ModelCtx);
      auto const prompt =
         cx.phase == CompactCtx::Phase::History
            ? domain::buildSummarizationPrompt(
                 cx.prep->messagesToSummarize, cx.prep->previousSummary,
                 cx.instructions)
            : domain::buildTurnPrefixPrompt(cx.prep->turnPrefixMessages,
                                            cx.instructions);
      domain::Message u{domain::Role::User, prompt, {}, {}};
      llm_.complete(
         domain::CompletionRequest{
            std::vector<domain::Message>{std::move(u)}, {},
            std::string{domain::kSummarizationSystemPrompt},
            sel.activeModel, sel.thinkingLevel},
         [](domain::AssistantStreamEvent const&) {},
         [this](domain::CompletionResult r) {
            ROLE(CompactCtx).lastResult = std::move(r);
            ROLE(EventQueueCtx).post(EV_LLM_DONE, LlmDoneMsg{});
         });
   }

   // 返回 false = 失败/被中止 (ctx.error 已填), action 转 FAILED 收尾.
   auto settleSummarize() -> bool {
      auto& cx = ROLE(CompactCtx);
      auto& r  = *cx.lastResult;
      if (ROLE(TurnStateCtx).aborted()) {
         cx.error   = "Compaction cancelled";
         cx.aborted = true;
         return false;
      }
      if (auto f = domain::summarizationFailure(
             r, cx.phase == CompactCtx::Phase::History
                   ? "Summarization" : "Turn prefix summarization")) {
         cx.error = *f;
         return false;
      }
      if (cx.phase == CompactCtx::Phase::History) {
         cx.historySummary = r.message.text;
         cx.historyUsage   = r.message.usage;
         cx.hasHistory     = true;
         cx.phase          = CompactCtx::Phase::Prefix;
      } else {
         cx.prefixSummary = r.message.text;
         cx.prefixUsage   = r.message.usage;
         cx.hasPrefix     = true;
         cx.phase         = CompactCtx::Phase::Done;
      }
      return true;
   }

   // pi compact 收尾: summary += formatFileOperations; CompactionEntry 落库.
   // 组装产物同时挂回 ctx 供 runner 出 compaction_end 帧.
   auto assemble() -> void {
      auto& cx = ROLE(CompactCtx);
      std::string   summary;
      domain::Usage usage;
      if (cx.prep->isSplitTurn && cx.hasPrefix) {
         summary = std::format(
            "{}\n\n---\n\n**Turn Context (split turn):**\n\n{}",
            cx.hasHistory ? cx.historySummary : "No prior history.",
            cx.prefixSummary);
         usage = cx.hasHistory ? mergeUsage(cx.historyUsage, cx.prefixUsage)
                               : cx.prefixUsage;
      } else {
         summary = cx.historySummary;
         usage   = cx.historyUsage;
      }
      summary += domain::formatFileOperations(cx.prep->filesRead,
                                              cx.prep->filesModified);

      folly::dynamic readOnly = folly::dynamic::array();
      for (auto const& f : cx.prep->filesRead)
         if (!cx.prep->filesModified.contains(f)) readOnly.push_back(f);
      folly::dynamic modified = folly::dynamic::array();
      for (auto const& f : cx.prep->filesModified) modified.push_back(f);
      folly::dynamic details =
         folly::dynamic::object("readFiles", std::move(readOnly))
                               ("modifiedFiles", std::move(modified));

      domain::CompactionEntry ce;
      ce.summary          = summary;
      ce.firstKeptEntryId = cx.prep->firstKeptEntryId;
      ce.tokensBefore     = cx.prep->tokensBefore;
      ce.detailsJson      = folly::toJson(details);
      ce.usage            = usage;
      ce.hasUsage         = true;
      ROLE(SessionCtx).session.appendEntry(
         domain::Entry{domain::EntryBase{}, std::move(ce)});

      cx.summary     = std::move(summary);
      cx.usage       = usage;
      cx.detailsJson = folly::toJson(std::move(details));
   }

private:
   static auto mergeUsage(domain::Usage const& a, domain::Usage const& b)
      -> domain::Usage {
      domain::Usage u = a;
      u.input       += b.input;
      u.output      += b.output;
      u.cacheRead   += b.cacheRead;
      u.cacheWrite  += b.cacheWrite;
      u.totalTokens += b.totalTokens;
      return u;
   }

   domain::ILlmProvider& llm_;
};

} // namespace pi::domain
