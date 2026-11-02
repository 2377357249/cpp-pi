#include <gtest/gtest.h>

#include <pi/infra/messaging/PubSub.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using pi::infra::BusMessage;
using pi::infra::InMemoryPubSub;
using pi::infra::subscribe;

TEST(PubSub, DeliversToAllSubscribersOnMatchingTopic) {
   InMemoryPubSub bus;
   std::vector<std::string> received;
   auto first = subscribe(bus, "orders.created", [&](BusMessage const& m) {
      received.push_back("first:" + m.payload);
   });
   auto second = subscribe(bus, "orders.created", [&](BusMessage const& m) {
      received.push_back("second:" + m.payload);
   });
   auto other = subscribe(bus, "orders.updated", [&](BusMessage const& m) {
      received.push_back("other:" + m.payload);
   });

   bus.publish({"orders.created", "42"});

   EXPECT_EQ(received.size(), 2);
   EXPECT_NE(std::find(received.begin(), received.end(), "first:42"), received.end());
   EXPECT_NE(std::find(received.begin(), received.end(), "second:42"), received.end());
}

TEST(PubSub, ResetStopsFutureDelivery) {
   InMemoryPubSub bus;
   auto calls = 0;
   auto sub = subscribe(bus, "events", [&](BusMessage const&) { ++calls; });

   bus.publish({"events", "one"});
   sub.reset();
   bus.publish({"events", "two"});

   EXPECT_EQ(calls, 1);
}

TEST(PubSub, SubscriptionCanMove) {
   InMemoryPubSub bus;
   auto calls = 0;
   auto subscription = subscribe(bus, "events", [&](BusMessage const&) { ++calls; });
   auto moved = std::move(subscription);
   bus.publish({"events", "payload"});
   EXPECT_EQ(calls, 1);
   moved.reset();
   bus.publish({"events", "payload"});
   EXPECT_EQ(calls, 1);
}

TEST(PubSub, CallbackMayPublishRecursively) {
   InMemoryPubSub bus;
   std::vector<std::string> received;
   auto nested = subscribe(bus, "nested", [&](BusMessage const& m) {
      received.push_back(m.payload);
   });
   auto outer = subscribe(bus, "outer", [&](BusMessage const&) {
      bus.publish({"nested", "inner"});
   });

   bus.publish({"outer", "start"});

   ASSERT_EQ(received.size(), 1);
   EXPECT_EQ(received.front(), "inner");
}
