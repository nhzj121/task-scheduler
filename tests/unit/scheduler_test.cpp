#include "ts/scheduler/scheduler.h"

#include <algorithm>
#include <limits>
#include <random>
#include <gtest/gtest.h>

namespace ts::scheduler {
namespace {
WorkerSnapshot worker(const std::string& name, std::uint32_t cpu, std::uint64_t memory,
                      std::uint32_t used_cpu = 0, std::uint64_t used_memory = 0) {
    return {domain::WorkerId::parse(name).value(), domain::ResourceCapacity::create(cpu, memory).value(),
            domain::ResourceUsage::from_values(used_cpu, used_memory), 0, std::nullopt};
}
std::string choose(std::uint32_t cpu, std::uint64_t memory, const std::vector<WorkerSnapshot>& workers) {
    auto result = select(domain::ResourceRequest::create(cpu, memory).value(), workers);
    EXPECT_TRUE(result);
    return result && result.value() ? result.value()->value() : "none";
}
TEST(Scheduler, CpuAndMemoryDominantPlacementUsesProjectedNotCurrentLoad) {
    const std::vector<WorkerSnapshot> workers{worker("a", 4, 100, 0, 50), worker("b", 8, 100, 1, 0)};
    EXPECT_EQ(choose(4, 0, workers), "b");
    EXPECT_EQ(choose(1, 50, workers), "b");
    EXPECT_EQ(choose(1, 0, {worker("a", 2, 100), worker("b", 8, 100, 1)}), "b");
}
TEST(Scheduler, ExactRatiosDistinguishNearEqualityAtUint64Boundary) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(choose(1, 1, {worker("a", 100, max, 0, max - 2),
                            worker("b", 100, max - 1, 0, max - 3)}), "b");
    EXPECT_EQ(choose(1, 0, {worker("z", 6, 0, 1), worker("a", 3, 0)}), "a");
}
TEST(Scheduler, TieBreakIsIndependentOfSeededInputPermutations) {
    std::vector<WorkerSnapshot> workers{worker("z", 8, 100), worker("a", 8, 100), worker("m", 8, 100)};
    std::mt19937 random(601);
    for (int i = 0; i < 500; ++i) {
        std::shuffle(workers.begin(), workers.end(), random); EXPECT_EQ(choose(1, 0, workers), "a");
    }
}
TEST(Scheduler, ZeroMemoryAndIndependentCapacityAndConcurrencyFilters) {
    auto w = worker("a", 2, 0);
    EXPECT_EQ(choose(2, 0, {w}), "a"); EXPECT_EQ(choose(1, 1, {w}), "none");
    EXPECT_EQ(choose(3, 0, {w}), "none");
    w.max_concurrent_tasks = 1; w.attempts = 1; EXPECT_EQ(choose(1, 0, {w}), "none");
    w.attempts = 0; EXPECT_EQ(choose(1, 0, {w}), "a");
    EXPECT_EQ(choose(2, 10, {worker("a", 2, 10)}), "a");
    EXPECT_EQ(choose(1, 11, {worker("a", 2, 10)}), "none");
}
TEST(Scheduler, OfflineInvalidAndInvalidatingSessionsAreNeverCandidates) {
    auto w = worker("a", 2, 10);
    w.state = domain::WorkerState::OFFLINE; EXPECT_EQ(choose(1, 0, {w}), "none");
    w.state = domain::WorkerState::HEALTHY; w.current_session = false; EXPECT_EQ(choose(1, 0, {w}), "none");
    w.current_session = true; w.invalidating = true; EXPECT_EQ(choose(1, 0, {w}), "none");
    EXPECT_EQ(choose(1, 0, {}), "none");
}
TEST(Scheduler, InvalidOverflowSnapshotsAreExplicitFailures) {
    const auto request = domain::ResourceRequest::create(1, 1).value();
    EXPECT_FALSE(select(request, {worker("a", 1, 0, UINT32_MAX, UINT64_MAX)}));
    EXPECT_FALSE(select(request, {worker("a", 1, 1), worker("a", 1, 1)}));
    EXPECT_EQ(choose(1, 1, {worker("a", UINT32_MAX, UINT64_MAX, UINT32_MAX - 1, UINT64_MAX - 1)}), "a");
}
TEST(Scheduler, PendingOrderIsStableAndAllowsOnePassSkippingInfeasibleOldest) {
    const auto at = common::Timestamp::parse("2026-10-21T00:00:00.000000Z").value();
    const auto a = domain::TaskId::parse("00000000-0000-4000-8000-000000000001").value();
    const auto b = domain::TaskId::parse("00000000-0000-4000-8000-000000000002").value();
    auto ordered = pending_order({{b, at, domain::ResourceRequest::create(1, 0).value()},
                                 {a, at, domain::ResourceRequest::create(9, 0).value()}});
    ASSERT_EQ(ordered.size(), 2U); EXPECT_EQ(ordered[0].task_id, a);
    EXPECT_EQ(choose(ordered[0].request.cpu_slots(), 0, {worker("a", 2, 0)}), "none");
    EXPECT_EQ(choose(ordered[1].request.cpu_slots(), 0, {worker("a", 2, 0)}), "a");
}
TEST(Scheduler, SeededExactComparatorAgreesWithIndependentBoundedCrossProductOracle) {
    std::mt19937 random(603);
    for (int sample = 0; sample < 1000; ++sample) {
        std::vector<WorkerSnapshot> workers;
        for (int i = 0; i < 5; ++i) {
            const auto cpu = static_cast<std::uint32_t>(random() % 999U + 1U); const auto mem = random() % 1000U;
            workers.push_back(worker(std::string(1, static_cast<char>('a' + i)), cpu, mem,
                                     static_cast<std::uint32_t>(random() % (cpu + 1)), random() % (mem + 1)));
        }
        const auto cpu = static_cast<std::uint32_t>(random() % 50U + 1U); const auto mem = random() % 50U;
        std::string expected = "none"; std::uint64_t bn = 0, bd = 1;
        for (const auto& w : workers) {
            if (cpu > w.capacity.cpu_slots() - w.used.cpu_slots() || mem > w.capacity.memory_mb() - w.used.memory_mb()) continue;
            std::uint64_t n = w.used.cpu_slots() + cpu, d = w.capacity.cpu_slots();
            const auto mn = w.used.memory_mb() + mem, md = w.capacity.memory_mb();
            if (md && mn * d > n * md) { n = mn; d = md; }
            if (expected == "none" || n * bd < bn * d || (n * bd == bn * d && w.worker_id.value() < expected)) {
                expected = w.worker_id.value(); bn = n; bd = d;
            }
        }
        std::shuffle(workers.begin(), workers.end(), random);
        EXPECT_EQ(choose(cpu, mem, workers), expected) << "seed=603 sample=" << sample;
    }
}
} // namespace
} // namespace ts::scheduler
