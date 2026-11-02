#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/llm/Completion.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 领域规则: pi auto-retry (rpc agent loop 的 retryable error 续跑).
//   末条 assistant 为可重试 provider 错误 (RETRYABLE_PROVIDER_ERROR_PATTERN
//   命中且非 overflow) 时: backoff = base2000 * 2^(n-1) cap 60000, 最多
//   kMaxAttempts 次; 中止旗标/abort_retry 随时杀链.
// ---------------------------------------------------------------------------
struct RetryPolicy {
   static constexpr size_t  kMaxAttempts    = 3;
   static constexpr int64_t kBaseDelayMs    = 2000;
   static constexpr int64_t kMaxDelayMs     = 60000;
   static constexpr int64_t kCancelSliceMs  = 50;   // backoff 轮询取消间隔

   static auto delayMs(size_t attempt) -> int64_t {
      int64_t d = kBaseDelayMs;
      for (size_t i = 1; i < attempt; ++i) d = std::min(d * 2, kMaxDelayMs);
      return std::min(d, kMaxDelayMs);
   }

   // pi RETRYABLE_PROVIDER_ERROR_PATTERN: 传输/限流/过载/5xx 瞬时错误.
   // overflow (context length) 不重试 —— 那是 auto-compact 的职责.
   static auto isRetryable(std::string_view msg) -> bool {
      std::string lower;
      lower.reserve(msg.size());
      for (char c : msg)
         lower += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
      static constexpr std::array kPatterns{
         "overloaded", "rate limit", "rate_limit", "too many requests",
         "429", "timeout", "timed out", "connection reset",
         "connection refused", "econnreset", "socket", "temporarily",
         "unavailable", "bad gateway", "502", "503", "504",
         "internal error", "server error", "upstream",
      };
      // overflow 类错误不可重试 (pi: context overflow 走压缩恢复)
      static constexpr std::array kOverflow{
         "context length", "context_length", "too long",
         "maximum context", "context window", "overflow",
      };
      for (auto p : kOverflow)
         if (lower.find(p) != std::string::npos) return false;
      for (auto p : kPatterns)
         if (lower.find(p) != std::string::npos) return true;
      return false;
   }

   // 本轮 completion 是否该进 retry 链 (RetryPending 谓词的唯一出口)
   static auto shouldRetry(TurnStateCtx const& turn) -> bool {
      return turn.autoRetryEnabled && !turn.aborted() && !turn.retryCancelled
          && turn.retryAttempt < kMaxAttempts
          && turn.lastCompletion.stopReason == StopReason::Error
          && isRetryable(turn.lastCompletion.message.errorMessage);
   }
};

} // namespace pi::domain
