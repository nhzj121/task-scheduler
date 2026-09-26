#include "ts/common/bounded_queue.h"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace ts::common {
namespace {

TEST(BoundedQueue, RejectsZeroCapacityAndReportsFullEmpty) {
    EXPECT_THROW(BoundedQueue<int>(0U), std::invalid_argument);

    BoundedQueue<int> queue(2U);
    EXPECT_FALSE(queue.try_pop().has_value());
    EXPECT_EQ(queue.try_push(1), QueuePushResult::Pushed);
    EXPECT_EQ(queue.try_push(2), QueuePushResult::Pushed);
    EXPECT_EQ(queue.try_push(3), QueuePushResult::Full);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.try_pop(), 1);
    EXPECT_EQ(queue.try_pop(), 2);
    EXPECT_FALSE(queue.try_pop().has_value());
}

TEST(BoundedQueue, StopRejectsPushesAndDrainsAcceptedValues) {
    BoundedQueue<int> queue(2U);
    ASSERT_EQ(queue.try_push(7), QueuePushResult::Pushed);
    queue.stop();
    queue.stop();

    EXPECT_TRUE(queue.stopped());
    EXPECT_EQ(queue.try_push(8), QueuePushResult::Stopped);
    EXPECT_EQ(queue.wait_pop(), 7);
    EXPECT_FALSE(queue.wait_pop().has_value());
}

TEST(BoundedQueue, StopWakesBlockedConsumerWithoutPolling) {
    BoundedQueue<int> queue(1U);
    auto waiting = std::async(std::launch::async, [&queue] { return queue.wait_pop(); });
    queue.stop();
    EXPECT_EQ(waiting.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(waiting.get().has_value());
}

TEST(BoundedQueue, ConcurrentProducersAndConsumerPreserveEveryValue) {
    constexpr int producer_count = 4;
    constexpr int values_per_producer = 250;
    constexpr int total_values = producer_count * values_per_producer;

    BoundedQueue<int> queue(17U);
    std::vector<int> consumed;
    consumed.reserve(static_cast<std::size_t>(total_values));
    std::thread consumer([&queue, &consumed] {
        while (const auto value = queue.wait_pop()) {
            consumed.push_back(*value);
        }
    });

    std::vector<std::thread> producers;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([producer, &queue] {
            for (int offset = 0; offset < values_per_producer; ++offset) {
                const int value = producer * values_per_producer + offset;
                while (queue.try_push(value) == QueuePushResult::Full) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    queue.stop();
    consumer.join();

    ASSERT_EQ(consumed.size(), static_cast<std::size_t>(total_values));
    const std::unordered_set<int> unique(consumed.begin(), consumed.end());
    EXPECT_EQ(unique.size(), static_cast<std::size_t>(total_values));
    for (int value = 0; value < total_values; ++value) {
        EXPECT_EQ(unique.count(value), 1U);
    }
}

}  // namespace
}  // namespace ts::common
