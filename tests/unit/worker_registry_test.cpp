#include "ts/controller/worker_registry.h"

#include <gtest/gtest.h>

namespace ts::controller {
namespace {
Registration registration(const std::string& id, std::uint64_t connection_generation = 1) {
    return {domain::WorkerId::parse("worker").value(), domain::SessionId::parse(id).value(), {7, connection_generation},
            domain::ResourceCapacity::create(4, 100).value(), 2};
}
constexpr const char* a = "00000000-0000-4000-8000-000000000001";
constexpr const char* b = "00000000-0000-4000-8000-000000000002";
TEST(WorkerRegistry, CurrentHeartbeatSequenceAndMonotonicDeadlineAreAuthoritative) {
    WorkerRegistry registry;
    const auto first = registry.register_worker(registration(a), 0); ASSERT_TRUE(first); ASSERT_TRUE(first.value().accepted);
    const auto key = *first.value().accepted; const auto wall = common::Timestamp::from_unix_microseconds(0);
    EXPECT_TRUE(registry.valid(key, 9999)); EXPECT_FALSE(registry.valid(key, 10000));
    EXPECT_TRUE(registry.heartbeat(key, 1, 3000, wall));
    EXPECT_FALSE(registry.heartbeat(key, 1, 6000, wall));
    EXPECT_EQ(registry.inspect(key.worker_id)->current->heartbeat_deadline, 13000U);
    EXPECT_FALSE(registry.heartbeat(key, 2, 13000, wall));
    EXPECT_EQ(registry.expired(13000).size(), 1U);
}
TEST(WorkerRegistry, ReplacementIsNotHealthyUntilOldDurableCleanupCompletes) {
    WorkerRegistry registry; auto first = registry.register_worker(registration(a), 0); ASSERT_TRUE(first);
    const auto old = *first.value().accepted;
    const auto replacement = registry.register_worker(registration(b, 2), 1); ASSERT_TRUE(replacement);
    EXPECT_FALSE(replacement.value().accepted); EXPECT_EQ(*replacement.value().invalidate, old);
    EXPECT_FALSE(registry.valid(old, 1));
    EXPECT_EQ(registry.inspect(old.worker_id)->state, domain::WorkerState::HEALTHY);
    EXPECT_TRUE(registry.inspect(old.worker_id)->invalidating);
    auto finished = registry.finish_invalidation(old, 50000); ASSERT_TRUE(finished); ASSERT_TRUE(finished.value());
    const auto next = *finished.value(); EXPECT_GT(next.generation, old.generation);
    EXPECT_EQ(next.connection.value, old.connection.value); EXPECT_NE(next.connection.generation, old.connection.generation);
    EXPECT_TRUE(registry.valid(next, 50000)); EXPECT_FALSE(registry.valid(old, 50000));
    EXPECT_FALSE(registry.finish_invalidation(old, 50000));
    EXPECT_FALSE(registry.heartbeat(old, 99, 50001, common::Timestamp::from_unix_microseconds(0)));
    EXPECT_TRUE(registry.valid(next, 50001));
}
TEST(WorkerRegistry, ReusedSessionOrConnectionAndUnboundedReplacementAreRejected) {
    WorkerRegistry registry; ASSERT_TRUE(registry.register_worker(registration(a), 0));
    EXPECT_FALSE(registry.register_worker(registration(a, 2), 1));
    EXPECT_FALSE(registry.register_worker(registration(b), 1));
    ASSERT_TRUE(registry.register_worker(registration(b, 2), 1));
    EXPECT_FALSE(registry.register_worker(registration("00000000-0000-4000-8000-000000000003", 3), 2));
}
TEST(WorkerRegistry, ClosedSocketAndOfflineRecordRetainHeartbeatWithoutLiveLease) {
    WorkerRegistry registry; const auto key = *registry.register_worker(registration(a), 0).value().accepted;
    const auto wall = common::Timestamp::from_unix_microseconds(1);
    EXPECT_TRUE(registry.heartbeat(key, 1, 100, wall));
    EXPECT_TRUE(registry.invalidate(key, true)); EXPECT_TRUE(registry.inspect(key.worker_id)->socket_closed);
    auto result = registry.finish_invalidation(key, 200); ASSERT_TRUE(result); EXPECT_FALSE(result.value());
    const auto record = registry.inspect(key.worker_id); ASSERT_TRUE(record);
    EXPECT_EQ(record->state, domain::WorkerState::OFFLINE); EXPECT_FALSE(record->current);
    EXPECT_EQ(record->last_heartbeat_at, wall);
}
} // namespace
} // namespace ts::controller
