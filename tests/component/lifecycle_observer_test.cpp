#include "ts/common/diagnostic_field.h"
#include "ts/controller/task_manager.h"
#include <gtest/gtest.h>

namespace ts::controller {
namespace {
struct Ports final : PersistencePort, EffectPort {
    std::vector<storage::Envelope> commands;
    std::vector<Effect> effects;
    storage::Admission try_post(storage::Envelope value) override {
        commands.push_back(std::move(value)); return storage::Admission::Accepted;
    }
    EffectAdmission emit(Effect value) override {
        effects.push_back(std::move(value)); return EffectAdmission::Accepted;
    }
};
class LifecycleController : public ::testing::Test {
protected:
    Ports ports;
    std::vector<TaskLifecycleEvent> events;
    bool throwing{false};
    TaskManager manager{ports, ports, {}, [this](const TaskLifecycleEvent& event) {
        events.push_back(event);
        if (event.reservation_released) {
            const auto usage = manager.resources().inspect(
                domain::WorkerId::parse(event.attempt->worker_id).value());
            ASSERT_TRUE(usage);
            EXPECT_EQ(usage.value().committed.attempts, 0U);
        }
        if (throwing) throw std::runtime_error("diagnostic sink failed");
    }};
    domain::TaskId id{domain::TaskId::generate()};
    Registration registration{domain::WorkerId::parse("observer-worker").value(),
        domain::SessionId::generate(), {7, 1}, domain::ResourceCapacity::create(2, 100).value(), 2};
    void submit(const domain::TaskId& task) {
        storage::TaskSpec spec; spec.task_id = task.value(); spec.executable = "/bin/true";
        ASSERT_EQ(manager.submit(spec, {{10, 1}, 1}), Decision::Accepted);
    }
    storage::Completion completion(std::size_t affected = 1) {
        storage::Outcome out; out.committed = true; out.affected = affected;
        return {ports.commands.back().correlation, out};
    }
    void commit() { ASSERT_EQ(manager.complete(completion()), Decision::Accepted); }
    Effect assigned(const domain::TaskId& task) {
        submit(task); commit();
        if (!manager.workers().inspect(registration.worker_id)) {
            EXPECT_EQ(manager.register_worker(registration), Decision::Accepted);
        }
        EXPECT_EQ(manager.schedule(), Decision::Accepted); commit();
        const auto run = ports.effects.back();
        EXPECT_EQ(run.kind, EffectKind::RunEligible);
        EXPECT_EQ(manager.record_dispatch(run, DispatchFact::BytesMayHaveEscaped), Decision::Accepted);
        return run;
    }
    void running(const Effect& run) {
        ASSERT_EQ(manager.receive(*run.task_id, AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
        commit();
        ASSERT_EQ(manager.receive(*run.task_id, StartedEvent{*run.attempt_id, *run.session}), Decision::Accepted);
    }
    void finish(const Effect& run) {
        const auto a = manager.attempts(*run.task_id).back();
        FinishedEvent done{*run.attempt_id, *run.session, ObservedOutcome::Exited, 0, {}, {},
            a.stdout_path, a.stderr_path, 0, 0, 0, 0,
            {storage::LogEvidenceKind::FinalBarrier, a.attempt_id, true, true, true, true, true, false, false, false}, false};
        ASSERT_EQ(manager.receive(*run.task_id, done), Decision::Accepted);
    }
};
TEST_F(LifecycleController, EverySuccessWaitsForMatchingCommitAndSnapshotsAreValues) {
    submit(id); EXPECT_TRUE(events.empty()); commit();
    ASSERT_EQ(events.size(), 1U); const auto accepted = events.front();
    EXPECT_EQ(accepted.kind, TaskLifecycleKind::Accepted); EXPECT_FALSE(accepted.attempt);
    ASSERT_EQ(manager.register_worker(registration), Decision::Accepted);
    ASSERT_EQ(manager.schedule(), Decision::Accepted);
    EXPECT_EQ(events.size(), 1U); commit();
    ASSERT_EQ(events.size(), 2U); EXPECT_EQ(events.back().kind, TaskLifecycleKind::Assigned);
    const auto assignment = events.back();
    const auto run = ports.effects.back();
    ASSERT_EQ(manager.record_dispatch(run, DispatchFact::BytesMayHaveEscaped), Decision::Accepted);
    running(run); EXPECT_EQ(events.size(), 2U); commit();
    ASSERT_EQ(events.size(), 3U); EXPECT_EQ(events.back().kind, TaskLifecycleKind::Running);
    EXPECT_EQ(events.back().state, domain::TaskState::ACTIVE);
    EXPECT_EQ(events.back().attempt->state, domain::AttemptState::RUNNING);
    EXPECT_EQ(events.back().attempt->version, 2U);
    finish(run); EXPECT_EQ(events.size(), 3U); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    const auto terminal = completion(); commit();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events.back().state, domain::TaskState::SUCCEEDED);
    EXPECT_TRUE(events.back().attempt->logs_complete); EXPECT_TRUE(events.back().reservation_released);
    EXPECT_EQ(manager.resources().reservation_count(), 0U);
    EXPECT_EQ(manager.complete(terminal), Decision::Stale); EXPECT_EQ(events.size(), 4U);
    EXPECT_EQ(accepted.state, domain::TaskState::PENDING);
    EXPECT_EQ(assignment.attempt->state, domain::AttemptState::ASSIGNED);
    EXPECT_EQ(assignment.attempt->version, 0U);
}
TEST_F(LifecycleController, UncommittedWrongGenerationAndDuplicateCompletionsStaySilent) {
    submit(id); auto done = completion();
    done.outcome.committed = false; EXPECT_EQ(manager.complete(done), Decision::Stale);
    done = completion(); ++done.correlation.entity_generation;
    EXPECT_EQ(manager.complete(done), Decision::Stale); EXPECT_TRUE(events.empty());
    done = completion(); EXPECT_EQ(manager.complete(done), Decision::Accepted);
    EXPECT_EQ(manager.complete(done), Decision::Stale); EXPECT_EQ(events.size(), 1U);
}
TEST_F(LifecycleController, FailedAndUnknownSubmitNeverClaimAcceptance) {
    for (const auto code : {storage::ErrorCode::Constraint, storage::ErrorCode::UnknownCommit}) {
        Ports p; std::vector<TaskLifecycleEvent> observed;
        TaskManager m(p, p, {}, [&](const auto& e) { observed.push_back(e); });
        storage::TaskSpec s; s.task_id = domain::TaskId::generate().value(); s.executable = "/bin/true";
        ASSERT_EQ(m.submit(s, {{10, 1}, 1}), Decision::Accepted);
        storage::Outcome out; out.error = storage::Error{code, 0, code == storage::ErrorCode::UnknownCommit, "injected"};
        static_cast<void>(m.complete({p.commands.back().correlation, out}));
        EXPECT_TRUE(observed.empty());
        EXPECT_EQ(m.degraded(), code == storage::ErrorCode::UnknownCommit);
    }
}
TEST_F(LifecycleController, ThrowingObserverCannotChangeEffectsOrReservationRelease) {
    throwing = true;
    const auto run = assigned(id); running(run); commit(); finish(run); commit();
    ASSERT_EQ(events.size(), 4U); EXPECT_FALSE(manager.degraded());
    EXPECT_EQ(manager.task(id)->state, domain::TaskState::SUCCEEDED);
    EXPECT_EQ(manager.resources().reservation_count(), 0U);
    std::vector<EffectKind> kinds;
    for (const auto& e : ports.effects) kinds.push_back(e.kind);
    EXPECT_EQ(kinds, (std::vector<EffectKind>{EffectKind::SubmitAccepted, EffectKind::RegistrationAccepted,
                                            EffectKind::RunEligible, EffectKind::TerminalVisible}));
}
TEST_F(LifecycleController, PendingCancelHasNoAttemptAndRestoreDoesNotReplay) {
    submit(id); commit();
    ASSERT_EQ(manager.receive(id, CancelEvent{{{10, 1}, 2}}), Decision::Accepted);
    EXPECT_EQ(events.size(), 1U); commit();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events.back().kind, TaskLifecycleKind::Terminal);
    EXPECT_FALSE(events.back().attempt); EXPECT_FALSE(events.back().reservation_released);
    Ports p; std::vector<TaskLifecycleEvent> replay;
    TaskManager restored(p, p, {}, [&](const auto& e) { replay.push_back(e); });
    ASSERT_EQ(restored.restore({*manager.task(id)}, {}), Decision::Accepted);
    EXPECT_TRUE(replay.empty());
}
TEST_F(LifecycleController, BulkLostWaitsForCommitAndDoesNotRepeatHistoricalTerminal) {
    const auto first = assigned(id); running(first); commit(); finish(first); commit();
    const auto second_id = domain::TaskId::generate();
    const auto second = assigned(second_id); running(second); commit();
    const auto count = events.size();
    ASSERT_EQ(manager.invalidate(*second.session), Decision::Accepted);
    EXPECT_EQ(events.size(), count); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    commit();
    ASSERT_EQ(events.size(), count + 1);
    EXPECT_EQ(events.back().task_id, second_id.value());
    EXPECT_EQ(events.back().state, domain::TaskState::FAILED);
    EXPECT_EQ(events.back().attempt->state, domain::AttemptState::LOST);
    EXPECT_FALSE(events.back().attempt->logs_complete);
    EXPECT_TRUE(events.back().reservation_released);
    EXPECT_EQ(manager.resources().reservation_count(), 0U);
}
TEST_F(LifecycleController, AssignmentFailureCannotClaimAssignment) {
    submit(id); commit();
    ASSERT_EQ(manager.register_worker(registration), Decision::Accepted);
    ASSERT_EQ(manager.schedule(), Decision::Accepted);
    storage::Outcome out; out.error = storage::Error{storage::ErrorCode::Constraint, 0, false, "injected"};
    EXPECT_EQ(manager.complete({ports.commands.back().correlation, out}), Decision::Rejected);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, TaskLifecycleKind::Accepted);
}
TEST_F(LifecycleController, FailedRunningCommitCannotClaimRunning) {
    const auto run = assigned(id); running(run);
    const auto count = events.size();
    storage::Outcome out; out.error = storage::Error{storage::ErrorCode::UnknownCommit, 0, true, "injected"};
    EXPECT_EQ(manager.complete({ports.commands.back().correlation, out}), Decision::Degraded);
    EXPECT_EQ(events.size(), count);
    EXPECT_EQ(manager.resources().reservation_count(), 1U);
}
TEST_F(LifecycleController, UnknownTerminalCommitCannotClaimTerminalOrRelease) {
    const auto run = assigned(id); running(run); commit(); finish(run);
    const auto count = events.size();
    storage::Outcome out; out.error = storage::Error{storage::ErrorCode::UnknownCommit, 0, true, "injected"};
    EXPECT_EQ(manager.complete({ports.commands.back().correlation, out}), Decision::Degraded);
    EXPECT_EQ(events.size(), count);
    EXPECT_EQ(manager.resources().reservation_count(), 1U);
}
TEST_F(LifecycleController, PendingCancelAfterRetryOmitsHistoricalAttempt) {
    storage::TaskRow row; row.spec.task_id = id.value(); row.spec.executable = "/bin/true";
    row.state = domain::TaskState::FAILED; row.state_version = 2;
    row.created_at = row.updated_at = "2026-11-03T00:00:00.000000Z";
    storage::AttemptRow old; old.task_id = id.value(); old.attempt_id = domain::AttemptId::generate().value();
    old.state = domain::AttemptState::LOST; row.latest_attempt_id = old.attempt_id;
    ASSERT_EQ(manager.restore({row}, {old}), Decision::Accepted); EXPECT_TRUE(events.empty());
    ASSERT_EQ(manager.receive(id, RetryEvent{{{10, 1}, 1}}), Decision::Accepted); commit();
    ASSERT_EQ(manager.receive(id, CancelEvent{{{10, 1}, 2}}), Decision::Accepted); commit();
    ASSERT_EQ(events.size(), 1U); EXPECT_FALSE(events[0].attempt);
    EXPECT_EQ(manager.attempts(id)[0].attempt_id, old.attempt_id);
}
TEST(DiagnosticField, BoundsAndEscapesTerminalControlAndFormatCharacters) {
    EXPECT_EQ(common::diagnostic_field("a\n\033[31m\"\\{}"), "\"a\\x0a\\x1b[31m\\\"\\\\{}\"");
    EXPECT_EQ(common::diagnostic_field(std::string(300, 'a')), "\"" + std::string(256, 'a') + "...\"");
}
} // namespace
} // namespace ts::controller
