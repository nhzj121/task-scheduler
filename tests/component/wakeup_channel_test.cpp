#include "ts/io/wakeup_channel.h"

#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ts/common/bounded_queue.h"
#include "ts/io/event_loop.h"

namespace ts::io {
namespace {

std::unique_ptr<EventLoop> make_loop() {
    auto result = EventLoop::create();
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result).value();
}

TEST(WakeupChannel, CapacityFullAndStoppedStatesAreExplicit) {
    EXPECT_FALSE(WakeupChannel<int>::create(0U));
    auto created = WakeupChannel<int>::create(2U);
    ASSERT_TRUE(created);
    auto channel = std::move(created).value();

    const auto first = channel->try_post(1);
    const auto second = channel->try_post(2);
    const auto full = channel->try_post(3);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(full);
    EXPECT_EQ(first.value(), common::QueuePushResult::Pushed);
    EXPECT_EQ(second.value(), common::QueuePushResult::Pushed);
    EXPECT_EQ(full.value(), common::QueuePushResult::Full);
    EXPECT_EQ(channel->size(), 2U);

    ASSERT_TRUE(channel->stop());
    const auto stopped = channel->try_post(4);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped.value(), common::QueuePushResult::Stopped);
    const auto values = channel->drain();
    ASSERT_TRUE(values);
    EXPECT_EQ(values.value(), (std::vector<int>{1, 2}));
}

TEST(WakeupChannel, CrossThreadPostsWakeAndDrainWithoutBlockingWait) {
    auto loop = make_loop();
    auto created = WakeupChannel<int>::create(8U);
    ASSERT_TRUE(created);
    auto channel = std::move(created).value();
    std::vector<int> observed;
    auto registration = loop->add(
        channel->fd(), kReadable,
        [&channel, &observed](const RegistrationToken&, EventMask) {
            auto drained = channel->drain();
            EXPECT_TRUE(drained);
            if (drained) {
                for (int value : drained.value()) {
                    observed.push_back(value);
                }
            }
        });
    ASSERT_TRUE(registration);

    bool producer_succeeded = false;
    std::thread producer([&channel, &producer_succeeded] {
        const auto first = channel->try_post(10);
        const auto second = channel->try_post(20);
        producer_succeeded =
            first && second && first.value() == common::QueuePushResult::Pushed &&
            second.value() == common::QueuePushResult::Pushed;
    });
    producer.join();
    const auto dispatched = loop->poll_once(1'000);
    ASSERT_TRUE(dispatched);
    EXPECT_EQ(dispatched.value(), 1U);
    EXPECT_TRUE(producer_succeeded);
    EXPECT_EQ(observed, (std::vector<int>{10, 20}));
    EXPECT_EQ(channel->size(), 0U);
    EXPECT_TRUE(loop->remove(registration.value()));
}

}  // namespace
}  // namespace ts::io
