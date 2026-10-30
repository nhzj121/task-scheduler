#include "ts/controller/task_manager.h"
#include <gtest/gtest.h>
namespace ts::controller {
namespace {
struct Port final : PersistencePort, EffectPort {
    std::vector<storage::Envelope> commands;
    std::vector<Effect> effects;
    storage::Admission try_post(storage::Envelope e) override { commands.push_back(std::move(e)); return storage::Admission::Accepted; }
    EffectAdmission emit(Effect e) override { effects.push_back(std::move(e)); return EffectAdmission::Accepted; }
};
TEST(M9State, RetryRejectsExhaustedAttemptNumberBeforePersistenceOrSuccess) {
    Port port; TaskManager manager(port, port);
    const auto id = domain::TaskId::generate(); const auto attempt = domain::AttemptId::generate();
    storage::TaskRow task; task.spec.task_id = id.value(); task.spec.executable = "/bin/true";
    task.state = domain::TaskState::FAILED; task.state_version = 2;
    task.created_at = task.updated_at = "2026-10-29T00:00:00.000000Z"; task.latest_attempt_id = attempt.value();
    storage::AttemptRow row; row.task_id = id.value(); row.attempt_id = attempt.value(); row.attempt_no = UINT32_MAX;
    row.state = domain::AttemptState::LOST;
    ASSERT_EQ(manager.restore({task}, {row}), Decision::Accepted);
    EXPECT_EQ(manager.receive(id, RetryEvent{{{1, 1}, 1}}), Decision::Rejected);
    EXPECT_TRUE(port.commands.empty()); EXPECT_TRUE(port.effects.empty());
    EXPECT_EQ(manager.task(id)->state, domain::TaskState::FAILED);
    EXPECT_FALSE(manager.degraded());
}
TEST(M9State, RetryResponseFactsSurviveLaterInMemoryMutation) {
    Port port; TaskManager manager(port, port);
    const auto id = domain::TaskId::generate(); const auto attempt = domain::AttemptId::generate();
    storage::TaskRow task; task.spec.task_id = id.value(); task.spec.executable = "/bin/true";
    task.state = domain::TaskState::FAILED; task.state_version = 2;
    task.created_at = task.updated_at = "2026-10-29T00:00:00.000000Z"; task.latest_attempt_id = attempt.value();
    storage::AttemptRow row; row.task_id = id.value(); row.attempt_id = attempt.value(); row.attempt_no = 7;
    row.state = domain::AttemptState::LOST;
    ASSERT_EQ(manager.restore({task}, {row}), Decision::Accepted);
    ASSERT_EQ(manager.receive(id, RetryEvent{{{1, 1}, 1}}), Decision::Accepted);
    storage::Outcome committed; committed.committed = true; committed.affected = 1;
    ASSERT_EQ(manager.complete({port.commands.back().correlation, committed}), Decision::Accepted);
    ASSERT_EQ(port.effects.size(), 1U); const auto response = port.effects.front();
    ASSERT_EQ(response.kind, EffectKind::RetryAccepted); EXPECT_EQ(response.next_attempt_no, 8U);
    ASSERT_EQ(manager.receive(id, CancelEvent{{{1, 1}, 2}}), Decision::Accepted);
    ASSERT_EQ(manager.complete({port.commands.back().correlation, committed}), Decision::Accepted);
    EXPECT_EQ(manager.task(id)->state, domain::TaskState::CANCELLED);
    EXPECT_EQ(response.next_attempt_no, 8U); // adapter sends the captured PENDING response
}
} // namespace
} // namespace ts::controller
