#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <pi/domain/llm/ILlmProvider.h>
#include <pi/infra/log/Logging.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// Provider 多路复用器: ILlmProvider 的稳定门面 —— 内部持有每个 provider 的
// 真实实现, use(providerId) 切换转发目标. 切模型跨 provider 时无需重建
// service/impl: Completer 持有的 mux 引用不变, 下次 complete 即走新实现.
// 未知 provider -> use 返回 false (service.setModel 转成错误).
// ---------------------------------------------------------------------------
class LlmProviderMux : public domain::ILlmProvider {
public:
   auto add(std::string providerId,
            std::unique_ptr<domain::ILlmProvider> p) -> void {
      providers_[std::move(providerId)] = std::move(p);
      if (active_.empty()) active_ = providers_.begin()->first;
   }

   auto use(std::string_view providerId) -> bool {
      auto const it = providers_.find(std::string{providerId});
      if (it == providers_.end()) return false;
      if (it->first != active_) {
         LOG_INFO(pi::infra::log::logger(), "llm provider: {} -> {}",
                  active_, it->first);
         active_ = it->first;
      }
      return true;
   }

   auto active() const -> std::string const& { return active_; }

   auto has(std::string_view providerId) const -> bool {
      return providers_.contains(std::string{providerId});
   }

   void complete(domain::CompletionRequest req,
                 domain::AssistantStreamHandler onEvent,
                 domain::CompletionHandler done) override {
      auto const it = providers_.find(active_);
      if (it == providers_.end()) {
         domain::Message err{domain::Role::Assistant, {}, {}, {}};
         err.errorMessage = "llm: no provider registered for " + active_;
         done(domain::CompletionResult{std::move(err),
                                       domain::StopReason::Error});
         return;
      }
      it->second->complete(std::move(req), std::move(onEvent), std::move(done));
   }

private:
   std::unordered_map<std::string,
                      std::unique_ptr<domain::ILlmProvider>> providers_;
   std::string active_;
};

} // namespace pi::iface::adapter
