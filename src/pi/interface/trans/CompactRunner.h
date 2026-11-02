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
#include <pi/infra/util/JsonSafe.h>
#include <pi/interface/rpc/JsonCodec.h>
#include <pi/interface/trans/AgentSessionService.h>

namespace pi::iface {

// ---------------------------------------------------------------------------
// 压缩执行器 (pi agent-session.compact + compaction.ts), 两前端共用.
// reason ∈ {manual, threshold, overflow}; willRetry 仅 overflow 可为 true.
//   同步段: compaction_start 帧 + prepare 检查 (loop 线程);
//   异步段: 摘要/落盘走 service.startCompact 的 CompactProc 事务 ——
//   期间 loop 保持响应 (prompt 报 compaction-in-progress 错误, abort 可取消).
// done(result|null, err) 恒被调一次 —— 同步失败立即回, 异步收尾经事务回调.
// emit 收到 compaction_start/compaction_end 帧 —— RPC 直接上线, TUI 翻提示块.
// ---------------------------------------------------------------------------
class CompactRunner {
public:
   using Done =
      std::function<void(std::optional<folly::dynamic>, std::string err)>;

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
            Done done) -> void {
      emit_(folly::dynamic::object("type", "compaction_start")
                                  ("reason", std::string{reason}));

      auto fail = [&](std::string_view msg) {
         std::string_view prefix =
            reason == "manual"    ? "Compaction failed: "
            : reason == "overflow" ? "Context overflow recovery failed: "
                                   : "Auto-compaction failed: ";
         emit_(folly::dynamic::object("type", "compaction_end")
                  ("reason", std::string{reason})
                  ("aborted", false)("willRetry", false)
                  ("errorMessage", std::format("{}{}", prefix, msg)));
         if (done) done(std::nullopt, std::string{msg});
      };

      auto const path = session_.branchEntries();
      if (!path.empty()
          && std::holds_alternative<domain::CompactionEntry>(
                 path.back().payload))
         return fail("Already compacted");
      auto prep = domain::prepareCompaction(session_);
      if (!prep) return fail("Nothing to compact (session too small)");

      service_.startCompact(
         std::move(*prep), std::string{reason},
         std::string{instructions}, willRetry,
         [this, reason = std::string{reason}, willRetry,
          done = std::move(done)](TSL_NS::Status) mutable {
            domain::CompactCtx const& cx = service_.impl();
            if (!cx.error.empty()) {
               std::string_view prefix =
                  reason == "manual"    ? "Compaction failed: "
                  : reason == "overflow" ? "Context overflow recovery failed: "
                                        : "Auto-compaction failed: ";
               emit_(folly::dynamic::object("type", "compaction_end")
                     ("reason", reason)("aborted", cx.aborted)
                     ("willRetry", willRetry)
                     ("errorMessage",
                      std::format("{}{}", prefix, cx.error)));
               if (done) done(std::nullopt, cx.error);
               return;
            }
            finishSuccess(cx, reason, willRetry, done);
         });
   }

   // pi _checkCompaction 自动 case 3: 阈值压缩 (无重试).
   // usage 有效 → calculateContextTokens; error/零 usage → 全量估算.
   // 早于最新压缩边界的 assistant 不重复触发.
   // done 恒被调一次 (未启动也立即调) —— 前端靠它推进队列/泵.
   auto maybeAutoCompact(std::function<void()> done) -> void {
      auto no = [&] { if (done) done(); };
      if (!autoEnabled_ || service_.model().contextWindow <= 0
          || service_.compacting())
         return no();
      auto const msgs = session_.messages();
      domain::Message const* lastA = nullptr;
      for (auto const& m : msgs | std::views::reverse) {
         if (m.role == domain::Role::Assistant) { lastA = &m; break; }
      }
      if (lastA == nullptr
          || lastA->stopReason == domain::StopReason::Aborted) return no();

      for (auto const& e : session_.branchEntries() | std::views::reverse) {
         if (!std::holds_alternative<domain::CompactionEntry>(e.payload))
            continue;
         if (lastA->timestampMs != 0
             && lastA->timestampMs
                   <= domain::isoToMs(e.timestampIso))
            return no();
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
         return no();
      // pi _runAutoCompaction: prepare 先行, 失败静默 (不发 compaction_*)
      if (!domain::prepareCompaction(session_)) return no();
      run("threshold", "", false,
          [done = std::move(done)](auto, auto) { if (done) done(); });
   }

private:
   auto finishSuccess(domain::CompactCtx const& cx,
                      std::string const& reason, bool willRetry,
                      Done const& done) -> void {
      auto details = infra::parseJsonSafe(cx.detailsJson);
      auto const estimatedAfter =
         domain::estimateContextTokens(session_.messages());
      folly::dynamic result = folly::dynamic::object
         ("summary", cx.summary)
         ("firstKeptEntryId", cx.prep->firstKeptEntryId)
         ("tokensBefore", cx.prep->tokensBefore)
         ("estimatedTokensAfter", estimatedAfter)
         ("usage", rpc::usageToJson(cx.usage));
      if (details) result["details"] = *details;
      emit_(folly::dynamic::object("type", "compaction_end")
               ("reason", reason)("result", result)
               ("aborted", cx.aborted)("willRetry", willRetry));
      if (done) done(std::move(result), {});
   }

   AgentSessionService&                 service_;
   domain::Session&                     session_;
   domain::ISessionStore*               store_;
   std::function<void(folly::dynamic)>  emit_;
   bool                                 autoEnabled_ = true;
};

} // namespace pi::iface
