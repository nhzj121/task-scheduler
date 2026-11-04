#include "ts/controller/task_manager.h"
#include <gtest/gtest.h>
#include <thread>

namespace ts::controller {
namespace {
struct Ports final : PersistencePort, EffectPort {
    std::vector<storage::Envelope> commands;
    storage::Admission try_post(storage::Envelope e) override { commands.push_back(std::move(e)); return storage::Admission::Accepted; }
    EffectAdmission emit(Effect) override { return EffectAdmission::Accepted; }
};
struct CapacityAdmissionTest : ::testing::Test {
    Ports ports;
    TaskManager manager{ports, ports};
    Registration registration(std::string worker, std::uint32_t cpu, std::uint64_t memory, std::uint64_t connection) {
        return {domain::WorkerId::parse(worker).value(), domain::SessionId::generate(), {connection, 1},
                domain::ResourceCapacity::create(cpu, memory).value(), 1};
    }
    CapacityAdmission check(std::uint32_t cpu, std::uint64_t memory) {
        auto result = manager.capacity_admission(domain::ResourceRequest::create(cpu, memory).value());
        EXPECT_TRUE(result); return result.value();
    }
};
TEST_F(CapacityAdmissionTest, EmptyAndJointSingleNodeBoundsAreReadOnly) {
    EXPECT_EQ(check(1, 0).kind, CapacityAdmissionKind::NoKnownWorkers);
    ASSERT_EQ(manager.register_worker(registration("cpu", 8, 1, 1)), Decision::Accepted);
    ASSERT_EQ(manager.register_worker(registration("memory", 2, 100, 2)), Decision::Accepted);
    EXPECT_EQ(check(4, 50).kind, CapacityAdmissionKind::NoWorkerFits);
    EXPECT_EQ(check(8, 1).kind, CapacityAdmissionKind::Allowed);
    EXPECT_EQ(check(2, 100).kind, CapacityAdmissionKind::Allowed);
    EXPECT_EQ(check(9, 0).kind, CapacityAdmissionKind::NoWorkerFits);
    EXPECT_EQ(check(1, 101).kind, CapacityAdmissionKind::NoWorkerFits);
    EXPECT_EQ(check(1, 0).known_worker_count, 2U);
    EXPECT_TRUE(ports.commands.empty()); EXPECT_EQ(manager.resources().reservation_count(), 0U);
}
TEST_F(CapacityAdmissionTest, OfflineAdvertisementRemainsButPendingReplacementDoesNotCount) {
    auto old = registration("node", 2, 10, 1);
    ASSERT_EQ(manager.register_worker(old), Decision::Accepted);
    const auto key = manager.workers().inspect(old.worker_id)->current->key;
    auto replacement = registration("node", 8, 100, 2);
    ASSERT_EQ(manager.register_worker(replacement), Decision::Accepted);
    EXPECT_EQ(check(8, 100).kind, CapacityAdmissionKind::NoWorkerFits);
    ASSERT_FALSE(ports.commands.empty());
    storage::Outcome out; out.committed = true; out.affected = 0;
    ASSERT_EQ(manager.complete({ports.commands.back().correlation, out}), Decision::Accepted);
    EXPECT_EQ(check(8, 100).kind, CapacityAdmissionKind::Allowed);
    const auto current = manager.workers().inspect(old.worker_id)->current->key;
    ASSERT_EQ(manager.invalidate(current), Decision::Accepted);
    ASSERT_EQ(manager.complete({ports.commands.back().correlation, out}), Decision::Accepted);
    EXPECT_EQ(manager.workers().inspect(old.worker_id)->state, domain::WorkerState::OFFLINE);
    EXPECT_EQ(check(8, 100).kind, CapacityAdmissionKind::Allowed);
    EXPECT_EQ(check(9, 100).kind, CapacityAdmissionKind::NoWorkerFits);
    EXPECT_FALSE(key.session_id == current.session_id);
}
TEST_F(CapacityAdmissionTest, OccupiedTentativeCapacityAndConcurrencyDoNotRejectSpecification) {
    const auto reg = registration("busy", 2, 10, 1);
    ASSERT_EQ(manager.register_worker(reg), Decision::Accepted);
    storage::TaskSpec task; task.task_id = domain::TaskId::generate().value(); task.executable = "/bin/true";
    task.cpu_slots = 2; task.memory_mb = 10;
    ASSERT_EQ(manager.submit(task, {{8, 1}, 1}), Decision::Accepted);
    storage::Outcome out; out.committed = true; out.affected = 1;
    ASSERT_EQ(manager.complete({ports.commands.back().correlation, out}), Decision::Accepted);
    ASSERT_EQ(manager.schedule(), Decision::Accepted);
    EXPECT_EQ(manager.resources().inspect(reg.worker_id).value().tentative.attempts, 1U);
    EXPECT_EQ(check(2, 10).kind, CapacityAdmissionKind::Allowed);
    ASSERT_EQ(manager.complete({ports.commands.back().correlation, out}), Decision::Accepted);
    EXPECT_EQ(manager.resources().inspect(reg.worker_id).value().committed.attempts, 1U);
    EXPECT_EQ(check(2, 10).kind, CapacityAdmissionKind::Allowed);
}
TEST_F(CapacityAdmissionTest, RestoreDoesNotInventCapacityAndWrongOwnerIsAnError) {
    ASSERT_EQ(manager.restore({}, {}), Decision::Accepted);
    EXPECT_EQ(check(1, 0).kind, CapacityAdmissionKind::NoKnownWorkers);
    bool rejected = false;
    std::thread reader([&] {
        rejected = !manager.capacity_admission(domain::ResourceRequest::create(1, 0).value());
    });
    reader.join(); EXPECT_TRUE(rejected);
}
} // namespace
} // namespace ts::controller
