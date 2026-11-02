#pragma once

#include <utility>

#include <pi/domain/llm/ILlmProvider.h>
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/async/ThreadPool.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 端口实现装饰器: 把同步阻塞的 provider (curl_easy_perform) 赶到 pool
// worker 线程上执行, 回调统一 marshal 回 EventLoop 的 owner 线程 ——
// 流增量与 done 经同一 FIFO 队列, 顺序与同步实现完全一致.
// 效果: complete() 立即返回, 领域态仍只在 loop 线程被触碰.
// ---------------------------------------------------------------------------
class AsyncLlmProvider : public domain::ILlmProvider {
public:
   AsyncLlmProvider(domain::ILlmProvider& inner,
                    infra::EventLoop&     loop,
                    infra::ThreadPool&    pool)
      : inner_(inner), loop_(loop), pool_(pool) {}

   void complete(domain::CompletionRequest       req,
                 domain::AssistantStreamHandler  onEvent,
                 domain::CompletionHandler       done) override {
      pool_.submit([this,
                    req     = std::move(req),
                    onEvent = std::move(onEvent),
                    done    = std::move(done)]() mutable {
         inner_.complete(
            std::move(req),
            [this, onEvent](domain::AssistantStreamEvent const& e) {
               auto copy = e;
               loop_.post([onEvent, e = std::move(copy)] { onEvent(e); });
            },
            [this, done](domain::CompletionResult r) {
               loop_.post([done, r = std::move(r)]() mutable {
                  done(std::move(r));
               });
            });
      });
   }

private:
   domain::ILlmProvider& inner_;
   infra::EventLoop&     loop_;
   infra::ThreadPool&    pool_;
};

} // namespace pi::iface::adapter
