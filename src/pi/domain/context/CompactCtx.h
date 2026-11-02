#pragma once

#include <optional>
#include <string>

#include <pi/domain/compaction/CompactionPolicy.h>
#include <pi/domain/llm/Completion.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// context 碎片: 一次压缩事务 (CompactProc) 的跨 action 中间态.
//   prep 由调用方 (interface CompactRunner) 在 loop 线程算好填入;
//   摘要阶段产物与最终组装结果都挂在这里, 事务结束后由 runner 读走出帧.
// ---------------------------------------------------------------------------
struct CompactCtx {
   enum class Phase { History, Prefix, Done };

   // ---- 输入 (startCompact 填入) ----
   std::optional<CompactionPreparation> prep;
   std::string                          instructions;
   std::string                          reason;
   bool                                 willRetry = false;
   Phase                                phase = Phase::History;

   // ---- 摘要阶段中间态 ----
   std::optional<CompletionResult>      lastResult;
   std::string                          historySummary, prefixSummary;
   Usage                                historyUsage, prefixUsage;
   bool                                 hasHistory = false;
   bool                                 hasPrefix  = false;
   std::string                          error;       // 非空 = 失败原因
   bool                                 aborted    = false;

   // ---- assemble 产物 (runner 出帧读) ----
   std::string                          summary;
   Usage                                usage;
   std::string                          detailsJson;
};

} // namespace pi::domain
