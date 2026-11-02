#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 技术设施: 单线程事件循环 —— 唯一的事务/领域线程 ("loop 线程").
//   post() 线程安全, 任意线程可投递; runUntil() 只能由 owner 线程调用.
//   异步适配器 (LLM/工具/stdin reader) 把回调都 marshal 回 loop 线程执行,
//   领域态 (session/role/context) 因此永远只被 loop 线程触碰, 无需锁.
// ---------------------------------------------------------------------------
class EventLoop {
public:
   auto post(std::function<void()> task) -> void {
      {
         std::lock_guard lk{mu_};
         tasks_.push_back(std::move(task));
      }
      cv_.notify_one();
   }

   // owner 线程驱动: 反复 { done 检查 -> 取任务执行 } 直到 done() 为真.
   // done 检查先于每次等待 —— 任务队列非空也不会漏看退出条件.
   auto runUntil(std::function<bool()> done) -> void {
      for (;;) {
         std::function<void()> task;
         {
            std::unique_lock lk{mu_};
            cv_.wait(lk, [&] { return !tasks_.empty() || done(); });
            if (done()) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
         }
         task();
      }
   }

private:
   std::mutex               mu_;
   std::condition_variable  cv_;
   std::deque<std::function<void()>> tasks_;
};

} // namespace pi::infra
