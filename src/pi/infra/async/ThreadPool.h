#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 技术设施: 定长线程池 —— 异步适配器的"阻塞原语收容所".
//   同步阻塞体 (curl_easy_perform / spawnMerged / 文件扫描) 整体丢进 worker,
//   完成后由适配器把结果 post 回 EventLoop —— 池子只装 IO, 不碰领域态.
//   析构: 置停止旗标, 排干已排队任务后 join 全部 worker.
// ---------------------------------------------------------------------------
class ThreadPool {
public:
   explicit ThreadPool(size_t workers) {
      workers_.reserve(workers);
      for (size_t i = 0; i < workers; ++i)
         workers_.emplace_back([this](std::stop_token st) { loop(st); });
   }

   ~ThreadPool() {
      {
         std::lock_guard lk{mu_};
         stop_ = true;
      }
      cv_.notify_all();   // jthread 析构自动 join
   }

   ThreadPool(ThreadPool const&)            = delete;
   auto operator=(ThreadPool const&) -> ThreadPool& = delete;

   auto submit(std::function<void()> task) -> void {
      {
         std::lock_guard lk{mu_};
         tasks_.push_back(std::move(task));
      }
      cv_.notify_one();
   }

private:
   auto loop(std::stop_token st) -> void {
      for (;;) {
         std::function<void()> task;
         {
            std::unique_lock lk{mu_};
            cv_.wait(lk, [&] { return stop_ || !tasks_.empty(); });
            if (tasks_.empty()) return;   // stop_ && 队列空
            task = std::move(tasks_.front());
            tasks_.pop_front();
         }
         task();
      }
   }

   std::mutex                        mu_;
   std::condition_variable           cv_;
   std::deque<std::function<void()>> tasks_;
   std::vector<std::jthread>         workers_;
   bool                              stop_ = false;
};

} // namespace pi::infra
