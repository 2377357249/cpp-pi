#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include <cub/dci/Role.h>

#include <pi/domain/context/EventQueueCtx.h>
#include <pi/domain/context/EventSinkCtx.h>
#include <pi/domain/context/TurnStateCtx.h>
#include <pi/domain/event/Events.h>
#include <pi/domain/turn/RetryPolicy.h>
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/async/ThreadPool.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// role: auto-retry backoff 的业务 —— 把"睡 delay 再重试"做成事件等待:
//   worker 线程分段轮询睡眠 (响应 abort/abort_retry), 到期 post EV_RETRY_DUE.
//   cancel() 是 abort_retry 命令的入口 —— 只杀 sleep, 不动 run.
// ---------------------------------------------------------------------------
struct Retryer {
   USE_ROLE(EventQueueCtx);
   USE_ROLE(EventSinkCtx);
   USE_ROLE(TurnStateCtx);

   Retryer(infra::ThreadPool& pool, infra::EventLoop& loop)
      : pool_(pool), loop_(loop) {}

   // abort_retry: 杀掉在飞的 backoff sleep (worker 下一片刻即醒)
   auto cancel() -> void { cancelled_ = true; }

   auto begin() -> void {
      auto& turn = ROLE(TurnStateCtx);
      ++turn.retryAttempt;
      auto const delay = RetryPolicy::delayMs(turn.retryAttempt);
      ROLE(EventSinkCtx).sink->autoRetryStart(
         turn.retryAttempt, RetryPolicy::kMaxAttempts, delay,
         turn.lastCompletion.message.errorMessage);
      cancelled_ = false;
      auto const until = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds{delay};
      pool_.submit([this, until] {
         while (std::chrono::steady_clock::now() < until
                && !cancelled_.load()
                && !ROLE(TurnStateCtx).aborted())
            std::this_thread::sleep_for(
               std::chrono::milliseconds{RetryPolicy::kCancelSliceMs});
         // EventQueueCtx::post 是 loop 线程私有 —— 先 marshal 回去
         loop_.post([this] {
            ROLE(EventQueueCtx).post(EV_RETRY_DUE, TimerDoneMsg{});
         });
      });
   }

   // EV_RETRY_DUE 到达后: 正常到期 -> 标记链在飞, 下一次 completion 解出;
   // 被取消/中止 -> 终止链 + auto_retry_end{success:false}
   auto settle() -> void {
      auto& turn = ROLE(TurnStateCtx);
      if (turn.aborted() || cancelled_) {
         turn.retryCancelled = true;
         ROLE(EventSinkCtx).sink->autoRetryEnd(
            false, turn.retryAttempt, "Retry cancelled");
         return;
      }
      turn.retryInFlight = true;
   }

private:
   infra::ThreadPool& pool_;
   infra::EventLoop&  loop_;
   std::atomic<bool>  cancelled_{false};
};

} // namespace pi::domain
