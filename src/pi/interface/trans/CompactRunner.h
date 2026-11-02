#pragma once

#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/compaction/CompactionPolicy.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/domain/session/Session.h>
#include <pi/interface/rpc/JsonCodec.h>
#include <pi/interface/trans/AgentSessionService.h>

namespace pi::iface {

// ---------------------------------------------------------------------------
// 压缩执行器 (pi agent-session.compact + compaction.ts), 两前端共用.
// reason ∈ {manual, threshold, overflow}; willRetry 仅 overflow 可为 true.
// emit 回调收到 compaction_start/compaction_end 帧 —— RPC 直接上线,
// TUI 翻成提示块. errOut 带回命令响应用的裸错误串; 事件 errorMessage 带
// pi 前缀 (manual: "Compaction failed:" 等).
// ---------------------------------------------------------------------------
class CompactRunner {
public:
   CompactRunner(AgentSessionService&       service,
                 domain::Session&           session,
                 domain::ISessionStore*     store,
                 std::function<void(folly::dynamic)> emit)
      : service_(service), session_(session), store_(store),
        emit_(std::move(emit)) {}

   auto setAutoEnabled(bool v) -> void { autoEnabled_ = v; }
   auto autoEnabled() const -> bool { return autoEnabled_; }

   auto run(std::string_view reason,
            std::string_view instructions, bool willRetry,
            std::string* errOut)
      -> std::optional<folly::dynamic> {
      emit_(folly::dynamic::object("type", "compaction_start")
                                  ("reason", std::string{reason}));

      auto fail = [&](std::string_view msg)
         -> std::optional<folly::dynamic> {
         if (errOut) *errOut = std::string{msg};
         std::string_view prefix =
            reason == "manual"    ? "Compaction failed: "
            : reason == "overflow" ? "Context overflow recovery failed: "
                                   : "Auto-compaction failed: ";
         emit_(folly::dynamic::object("type", "compaction_end")
                  ("reason", std::string{reason})
                  ("aborted", false)("willRetry", false)
                  ("errorMessage", std::format("{}{}", prefix, msg)));
         return std::nullopt;
      };

      auto const path = session_.branchEntries();
      if (!path.empty()
          && std::holds_alternative<domain::CompactionEntry>(
                 path.back().payload))
         return fail("Already compacted");
      auto prep = domain::prepareCompaction(session_);
      if (!prep) return fail("Nothing to compact (session too small)");

      // LLM 摘要请求 (pi buildSummarizationContext: system + 单条 user)
      std::string summarizeErr;
      auto summarize = [&](std::string promptText,
                           std::string_view label)
         -> std::optional<domain::CompletionResult> {
         domain::Message u{domain::Role::User, std::move(promptText), {}, {}};
         auto r = service_.completeOnce(domain::CompletionRequest{
            std::vector<domain::Message>{std::move(u)},
            {},
            std::string{domain::kSummarizationSystemPrompt}});
         if (auto f = domain::summarizationFailure(r, label)) {
            summarizeErr = *f;
            return std::nullopt;
         }
         return r;
      };

      std::string   summary;
      domain::Usage usage;
      if (prep->isSplitTurn && !prep->turnPrefixMessages.empty()) {
         std::string history = prep->previousSummary.empty()
                                  ? "No prior history." : "";
         bool haveHist = false;
         if (!prep->messagesToSummarize.empty()) {
            auto r = summarize(domain::buildSummarizationPrompt(
                                  prep->messagesToSummarize,
                                  prep->previousSummary, instructions),
                               "Summarization");
            if (!r) return fail(summarizeErr);
            history  = r->message.text;
            usage    = r->message.usage;
            haveHist = true;
         }
         auto pr = summarize(domain::buildTurnPrefixPrompt(
                                prep->turnPrefixMessages, instructions),
                             "Turn prefix summarization");
         if (!pr) return fail(summarizeErr);
         summary = std::format("{}\n\n---\n\n**Turn Context (split turn):**\n\n{}",
                               history, pr->message.text);
         if (haveHist) {
            usage.input += pr->message.usage.input;
            usage.output += pr->message.usage.output;
            usage.cacheRead += pr->message.usage.cacheRead;
            usage.cacheWrite += pr->message.usage.cacheWrite;
            usage.totalTokens += pr->message.usage.totalTokens;
         } else usage = pr->message.usage;
      } else {
         auto r = summarize(domain::buildSummarizationPrompt(
                               prep->messagesToSummarize,
                               prep->previousSummary, instructions),
                            "Summarization");
         if (!r) return fail(summarizeErr);
         summary = r->message.text;
         usage   = r->message.usage;
      }

      // pi: summary += formatFileOperations; details={readFiles,modifiedFiles}
      summary += domain::formatFileOperations(prep->filesRead,
                                              prep->filesModified);
      folly::dynamic readOnly = folly::dynamic::array();
      for (auto const& f : prep->filesRead)
         if (!prep->filesModified.contains(f)) readOnly.push_back(f);
      folly::dynamic modified = folly::dynamic::array();
      for (auto const& f : prep->filesModified) modified.push_back(f);
      folly::dynamic details =
         folly::dynamic::object("readFiles", std::move(readOnly))
                               ("modifiedFiles", std::move(modified));

      domain::CompactionEntry ce;
      ce.summary          = summary;
      ce.firstKeptEntryId = prep->firstKeptEntryId;
      ce.tokensBefore     = prep->tokensBefore;
      ce.detailsJson      = folly::toJson(details);
      ce.usage            = usage;
      ce.hasUsage         = true;
      auto const& saved = session_.appendEntry(
         domain::Entry{domain::EntryBase{}, std::move(ce)});
      if (store_)
         store_->append(session_, session_.unpersisted(),
                        [](bool) {});
      session_.markPersisted();

      auto const estimatedAfter =
         domain::estimateContextTokens(session_.messages());
      folly::dynamic result = folly::dynamic::object
         ("summary", summary)
         ("firstKeptEntryId", prep->firstKeptEntryId)
         ("tokensBefore", prep->tokensBefore)
         ("estimatedTokensAfter", estimatedAfter)
         ("usage", rpc::usageToJson(usage))
         ("details", std::move(details));
      emit_(folly::dynamic::object("type", "compaction_end")
               ("reason", std::string{reason})("result", result)
               ("aborted", false)("willRetry", willRetry));
      (void)saved;
      return result;
   }

   // pi _checkCompaction 自动 case 3: 阈值压缩 (无重试).
   // usage 有效 → calculateContextTokens; error/零 usage → 全量估算.
   // 早于最新压缩边界的 assistant 不重复触发.
   auto maybeAutoCompact() -> void {
      if (!autoEnabled_ || service_.model().contextWindow <= 0) return;
      auto const msgs = session_.messages();
      domain::Message const* lastA = nullptr;
      for (auto const& m : msgs | std::views::reverse) {
         if (m.role == domain::Role::Assistant) { lastA = &m; break; }
      }
      if (lastA == nullptr
          || lastA->stopReason == domain::StopReason::Aborted) return;

      for (auto const& e : session_.branchEntries() | std::views::reverse) {
         if (!std::holds_alternative<domain::CompactionEntry>(e.payload))
            continue;
         if (lastA->timestampMs != 0
             && lastA->timestampMs
                   <= domain::isoToMs(e.timestampIso))
            return;
         break;
      }

      auto const& u = lastA->usage;
      auto const direct = u.totalTokens != 0
         ? u.totalTokens : u.input + u.output + u.cacheRead + u.cacheWrite;
      auto const contextTokens =
         (lastA->stopReason == domain::StopReason::Error || direct == 0)
            ? domain::estimateContextTokens(msgs)
            : direct;
      if (contextTokens
          <= service_.model().contextWindow - domain::kReserveTokens)
         return;
      // pi _runAutoCompaction: prepare 先行, 失败静默 (不发 compaction_*)
      if (!domain::prepareCompaction(session_)) return;
      run("threshold", "", false, nullptr);
   }

private:
   AgentSessionService&                 service_;
   domain::Session&                     session_;
   domain::ISessionStore*               store_;
   std::function<void(folly::dynamic)>  emit_;
   bool                                 autoEnabled_ = true;
};

} // namespace pi::iface
