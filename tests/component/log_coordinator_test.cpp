#include "ts/controller/log_coordinator.h"
#include <chrono>
#include <filesystem>
#include <thread>
#include <gtest/gtest.h>
#include <unistd.h>
namespace {
using namespace ts;
using namespace controller;
struct Persistence final : PersistencePort {
    std::vector<storage::Envelope> commands;
    storage::Admission try_post(storage::Envelope command) override { commands.push_back(std::move(command)); return storage::Admission::Accepted; }
};
struct Effects final : EffectPort {
    std::vector<Effect> values;
    EffectAdmission emit(Effect e) override { values.push_back(std::move(e)); return EffectAdmission::Accepted; }
};
struct NotifyFailure final : logs::FilesystemHook {
    logs::FaultAction before(logs::Operation op, const std::string&, std::size_t) override {
        return op == logs::Operation::Notify ? logs::FaultAction{EIO, 0} : logs::FaultAction{};
    }
};
class LogCoordinatorTest : public ::testing::Test {
protected:
    Persistence persistence;
    Effects effects;
    TaskManager manager{persistence, effects};
    std::unique_ptr<LogCoordinator> coordinator;
    std::string root;
    domain::TaskId task{domain::TaskId::generate()};
    Registration registration{domain::WorkerId::parse("log-worker").value(), domain::SessionId::generate(), {1, 1}, domain::ResourceCapacity::create(1, 1).value(), 1};
    std::optional<SessionKey> key;
    std::optional<storage::AttemptRow> row;
    void commit() {
        storage::Outcome outcome; outcome.committed = true; outcome.affected = 1;
        ASSERT_EQ(manager.complete({persistence.commands.back().correlation, outcome}), Decision::Accepted);
    }
    void start(std::shared_ptr<logs::FilesystemHook> hook = {}) {
        char name[] = "/tmp/task-scheduler-coordinator-XXXXXX"; const char* made = ::mkdtemp(name);
        ASSERT_NE(made, nullptr); root = made;
        domain::QueueConfig config; config.log_bytes = 8; config.log_commands = 3; config.log_control_reserve = 1;
        config.initial_session_log_window_bytes = 8; config.initial_session_log_window_chunks = 2;
        auto result = LogCoordinator::create(root, config, manager, std::move(hook)); ASSERT_TRUE(result);
        coordinator = std::move(result).value();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (coordinator->status().health == logs::Health::Starting && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        ASSERT_EQ(coordinator->status().health, logs::Health::Ready);
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        ASSERT_EQ(manager.tick(now, common::Timestamp::from_unix_microseconds(0)), Decision::Accepted);
        ASSERT_TRUE(coordinator->reserve(registration.session_id));
        ASSERT_EQ(manager.register_worker(registration), Decision::Accepted);
        key = manager.workers().inspect(registration.worker_id)->current->key;
        ASSERT_TRUE(coordinator->activate(*key));
        storage::TaskSpec spec; spec.task_id = task.value(); spec.executable = "/bin/true";
        ASSERT_EQ(manager.submit(spec, {{2, 1}, 1}), Decision::Accepted); commit();
        ASSERT_EQ(manager.schedule(), Decision::Accepted); commit();
        const auto run = effects.values.back(); ASSERT_EQ(run.kind, EffectKind::RunEligible);
        ASSERT_EQ(manager.record_dispatch(run, DispatchFact::BytesMayHaveEscaped), Decision::Accepted);
        row = manager.attempts(task).back(); ASSERT_TRUE(coordinator->bind(*row, *key));
        ASSERT_EQ(manager.receive(task, AckEvent{domain::AttemptId::parse(row->attempt_id).value(), *key, true}), Decision::Accepted); commit();
        ASSERT_EQ(manager.receive(task, StartedEvent{domain::AttemptId::parse(row->attempt_id).value(), *key}), Decision::Accepted); commit();
    }
    protocol::LogChunk chunk(std::uint64_t sequence = 0) {
        return {registration.worker_id, registration.session_id, task, domain::AttemptId::parse(row->attempt_id).value(), protocol::Stream::Stdout, sequence, 1, "eA=="};
    }
    void TearDown() override { coordinator.reset(); if (!root.empty()) std::filesystem::remove_all(root); }
};
TEST_F(LogCoordinatorTest, CreditedEnqueueFailureIsAnExplicitInvariantFailureNotCongestion) {
    start(std::make_shared<NotifyFailure>()); ASSERT_TRUE(coordinator);
    protocol::GetLogs read{task, {}, protocol::Stream::Stdout, 0, 1, {}};
    ASSERT_TRUE(coordinator->read(*row, read, {{2, 1}, 2}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (coordinator->status().health != logs::Health::Failed && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    ASSERT_EQ(coordinator->status().health, logs::Health::Failed);
    // The session still owns an unused valid grant, but the actual Writer has
    // failed completion notification. Admission cannot become normal backpressure.
    EXPECT_EQ(coordinator->append(chunk()), LogAdmission::InvariantFailure);
    EXPECT_FALSE(coordinator->pump([](const auto&, auto) { ADD_FAILURE() << "grant after channel failure"; return true; }));
    EXPECT_EQ(manager.task(task)->state, domain::TaskState::ACTIVE);
}
TEST_F(LogCoordinatorTest, OneByteChunksExhaustSlotsAndAllCreditedChunksAdmitAtCapacity) {
    start(); ASSERT_TRUE(coordinator);
    ASSERT_EQ(coordinator->append(chunk(0)), LogAdmission::Accepted);
    ASSERT_EQ(coordinator->append(chunk(1)), LogAdmission::Accepted);
    EXPECT_EQ(coordinator->append(chunk(2)), LogAdmission::ProtocolViolation);
    EXPECT_EQ(coordinator->usage().writes, 2U); EXPECT_EQ(coordinator->usage().bytes, 2U);
}
TEST_F(LogCoordinatorTest, FinishMustMatchAcceptedCountsImmediatelyAndNoLateLogIsAccepted) {
    start(); ASSERT_TRUE(coordinator);
    ASSERT_EQ(coordinator->append(chunk()), LogAdmission::Accepted);
    FinishedEvent finish{domain::AttemptId::parse(row->attempt_id).value(), *key, ObservedOutcome::Exited, 0, {}, {}, row->stdout_path, row->stderr_path, 2, 0, 1, 0, {}, false};
    EXPECT_FALSE(coordinator->finish(finish)); EXPECT_FALSE(manager.finalization_pending(task));
    finish.stdout_bytes = 1; ASSERT_TRUE(coordinator->finish(finish));
    EXPECT_TRUE(manager.finalization_pending(task)); EXPECT_EQ(manager.task(task)->state, domain::TaskState::ACTIVE);
    EXPECT_EQ(coordinator->append(chunk(1)), LogAdmission::ProtocolViolation);
}
} // namespace
