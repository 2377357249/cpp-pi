#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pi::infra {

// Payload encoding belongs to callers, keeping the envelope transport-neutral.
struct BusMessage {
   std::string topic;
   std::string payload;
};

class IPubSub {
public:
   using Handler = std::function<void(BusMessage const&)>;

   virtual ~IPubSub() = default;
   virtual auto publish(BusMessage message) -> void = 0;
   virtual auto subscribe(std::string topic, Handler handler) -> uint64_t = 0;
   virtual auto unsubscribe(std::string_view topic, uint64_t id) -> void = 0;
};

// Synchronous process-local implementation. Network transports can implement
// IPubSub while preserving the same topic/payload contract.
class InMemoryPubSub final : public IPubSub {
public:
   auto publish(BusMessage message) -> void override {
      std::vector<Handler> handlers;
      {
         std::lock_guard lock{mutex_};
         auto const it = handlers_.find(message.topic);
         if (it == handlers_.end()) return;
         handlers.reserve(it->second.size());
         for (auto const& [id, handler] : it->second) {
            static_cast<void>(id);
            handlers.push_back(handler);
         }
      }
      for (auto const& handler : handlers) handler(message);
   }

   auto subscribe(std::string topic, Handler handler) -> uint64_t override {
      std::lock_guard lock{mutex_};
      auto const id = nextId_++;
      handlers_[std::move(topic)].emplace(id, std::move(handler));
      return id;
   }

   auto unsubscribe(std::string_view topic, uint64_t id) -> void override {
      std::lock_guard lock{mutex_};
      auto const it = handlers_.find(std::string{topic});
      if (it == handlers_.end()) return;
      it->second.erase(id);
      if (it->second.empty()) handlers_.erase(it);
   }

private:
   std::mutex mutex_;
   std::unordered_map<std::string,
      std::unordered_map<uint64_t, Handler>> handlers_;
   uint64_t nextId_ = 1;
};

// RAII subscription. Destroying or resetting it unregisters the callback.
class Subscription {
public:
   Subscription() = default;
   Subscription(IPubSub& bus, std::string topic, uint64_t id)
      : bus_{&bus}, topic_{std::move(topic)}, id_{id} {}
   ~Subscription() { reset(); }

   Subscription(Subscription const&) = delete;
   auto operator=(Subscription const&) -> Subscription& = delete;
   Subscription(Subscription&& other) noexcept
      : bus_{std::exchange(other.bus_, nullptr)},
        topic_{std::move(other.topic_)}, id_{other.id_} {}
   auto operator=(Subscription&& other) noexcept -> Subscription& {
      if (this != &other) {
         reset();
         bus_ = std::exchange(other.bus_, nullptr);
         topic_ = std::move(other.topic_);
         id_ = other.id_;
      }
      return *this;
   }

   auto reset() -> void {
      if (bus_) {
         bus_->unsubscribe(topic_, id_);
         bus_ = nullptr;
      }
   }

private:
   IPubSub* bus_ = nullptr;
   std::string topic_;
   uint64_t id_ = 0;
};

inline auto subscribe(IPubSub& bus, std::string topic,
                      IPubSub::Handler handler) -> Subscription {
   auto const id = bus.subscribe(topic, std::move(handler));
   return Subscription{bus, std::move(topic), id};
}

} // namespace pi::infra
