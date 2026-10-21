#include "ts/controller/task_manager.h"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>

#include <gtest/gtest.h>

namespace ts::controller {
namespace {
using TS = domain::TaskState;
using AS = domain::AttemptState;
std::string uuid(std::uint64_t n) {
    std::ostringstream out; out << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << n;
    return out.str();
}
domain::TaskId task_id(std::uint64_t n = 1) { return domain::TaskId::parse(uuid(n)).value(); }
ClientRequest client(std::uint64_t n = 1) { return {{100, 1}, n}; }
storage::TaskSpec spec(std::uint64_t n = 1, std::uint32_t cpu = 1, std::uint64_t memory = 10) {
    storage::TaskSpec s; s.task_id = uuid(n); s.executable = "/bin/true"; s.cpu_slots = cpu; s.memory_mb = memory; return s;
}
Registration reg(std::uint64_t n = 1000, const std::string& worker = "worker", std::uint64_t generation = 1) {
    return {domain::WorkerId::parse(worker).value(), domain::SessionId::parse(uuid(n)).value(), {7, generation},
            domain::ResourceCapacity::create(4, 100).value(), 4};
}
struct Persistence final : PersistencePort {
    std::vector<storage::Envelope> commands;
    storage::Admission admission{storage::Admission::Accepted};
    bool ordinary_full{false};
    storage::Admission try_post(storage::Envelope envelope) override {
        if (admission != storage::Admission::Accepted) return admission;
        if (ordinary_full && !storage::is_essential(envelope.command)) return storage::Admission::Full;
        commands.push_back(std::move(envelope)); return storage::Admission::Accepted;
    }
};
struct Effects final : EffectPort {
    std::vector<Effect> values;
    EffectAdmission admission{EffectAdmission::Accepted};
    bool client_gone{false};
    EffectAdmission emit(Effect value) override {
        if (client_gone && value.client) return EffectAdmission::ClientGone;
        if (admission == EffectAdmission::Accepted) values.push_back(std::move(value));
        return admission;
    }
    std::size_t count(EffectKind kind) const {
        return static_cast<std::size_t>(std::count_if(values.begin(), values.end(), [&](const auto& e) { return e.kind == kind; }));
    }
    Effect last(EffectKind kind) const {
        for (auto it = values.rbegin(); it != values.rend(); ++it) if (it->kind == kind) return *it;
        throw std::logic_error("expected effect absent");
    }
};
storage::Completion committed(const storage::Envelope& e, std::size_t affected = 1) {
    storage::Outcome o; o.committed = true; o.affected = affected; return {e.correlation, o};
}
storage::Completion failed(const storage::Envelope& e, storage::ErrorCode code, bool fatal = false) {
    storage::Outcome o; o.error = storage::Error{code, 0, fatal, "injected"}; return {e.correlation, o};
}
class ControllerState : public ::testing::Test {
protected:
    Persistence persistence;
    Effects effects;
    TaskManager manager{persistence, effects};
    void submit(std::uint64_t n = 1, std::uint32_t cpu = 1) {
        ASSERT_EQ(manager.submit(spec(n, cpu), client(n)), Decision::Accepted);
        ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    }
    SessionKey worker(Registration registration = reg()) {
        EXPECT_EQ(manager.register_worker(registration), Decision::Accepted);
        return manager.workers().inspect(registration.worker_id)->current->key;
    }
    // All transitions remain explicit: these helpers submit then return real
    // captured M5 commands to the production core, never mutate its snapshots.
    storage::Envelope assign() {
        EXPECT_EQ(manager.schedule(), Decision::Accepted);
        EXPECT_TRUE(std::holds_alternative<storage::Assign>(persistence.commands.back().command));
        return persistence.commands.back();
    }
    Effect active(std::uint64_t n = 1) {
        submit(n); if (!manager.workers().inspect(reg().worker_id)) static_cast<void>(worker());
        auto assignment = assign(); EXPECT_EQ(manager.complete(committed(assignment)), Decision::Accepted);
        auto run = effects.last(EffectKind::RunEligible);
        EXPECT_EQ(manager.record_dispatch(run, DispatchFact::BytesMayHaveEscaped), Decision::Accepted);
        return run;
    }
    void running(const Effect& run) {
        ASSERT_EQ(manager.receive(*run.task_id, AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
        ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
        ASSERT_EQ(manager.receive(*run.task_id, StartedEvent{*run.attempt_id, *run.session}), Decision::Accepted);
        ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    }
    FinishedEvent finish(const Effect& run, bool barrier = true) {
        const auto row = manager.attempts(*run.task_id).back();
        FinishedEvent f{*run.attempt_id, *run.session, ObservedOutcome::Exited, 0, {}, {},
                        row.stdout_path, row.stderr_path, 12, 0, 1, 0, {}, false};
        if (barrier) f.evidence = {storage::LogEvidenceKind::FinalBarrier, row.attempt_id, true, true, true, true, true, false, false, false};
        return f;
    }
};

TEST_F(ControllerState, SubmitVisibilityAndResponseRequireMatchingCommittedInsertion) {
    ASSERT_EQ(manager.submit(spec(), client()), Decision::Accepted);
    const auto command = persistence.commands.back();
    EXPECT_FALSE(manager.task(task_id())); EXPECT_EQ(effects.count(EffectKind::SubmitAccepted), 0U);
    auto notification = committed(command); notification.outcome.committed = false;
    EXPECT_EQ(manager.complete(notification), Decision::Stale);
    EXPECT_FALSE(manager.task(task_id()));
    EXPECT_EQ(manager.complete(committed(command)), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::PENDING); EXPECT_EQ(effects.count(EffectKind::SubmitAccepted), 1U);
    EXPECT_EQ(manager.complete(committed(command)), Decision::Stale);
}
TEST_F(ControllerState, BusinessSubmitFailureAndDisconnectedClientNeverCancelCommittedTask) {
    ASSERT_EQ(manager.submit(spec(), client()), Decision::Accepted);
    EXPECT_EQ(manager.complete(failed(persistence.commands.back(), storage::ErrorCode::Constraint)), Decision::Rejected);
    EXPECT_FALSE(manager.task(task_id())); EXPECT_EQ(effects.count(EffectKind::SubmitAccepted), 0U);
    effects.client_gone = true; submit(2);
    EXPECT_EQ(manager.task(task_id(2))->state, TS::PENDING); EXPECT_FALSE(manager.degraded());
}
TEST_F(ControllerState, AssignmentUsesTentativeCapacityAndNeverRunsBeforeCommit) {
    submit(); submit(2); static_cast<void>(worker()); static_cast<void>(assign());
    ASSERT_EQ(persistence.commands.size(), 4U); // two Submit, two Assign
    const auto one = persistence.commands[2]; const auto two = persistence.commands.back();
    EXPECT_EQ(effects.count(EffectKind::RunEligible), 0U);
    auto usage = manager.resources().inspect(reg().worker_id).value();
    EXPECT_EQ(usage.tentative.attempts, 2U); EXPECT_EQ(usage.committed.attempts, 0U);
    EXPECT_EQ(manager.schedule(), Decision::Accepted); EXPECT_EQ(persistence.commands.size(), 4U);
    EXPECT_EQ(manager.complete(committed(two)), Decision::Accepted);
    usage = manager.resources().inspect(reg().worker_id).value();
    EXPECT_EQ(usage.tentative.attempts, 1U); EXPECT_EQ(usage.committed.attempts, 1U);
    EXPECT_EQ(manager.complete(committed(one)), Decision::Accepted);
    EXPECT_EQ(manager.complete(committed(one)), Decision::Stale);
    EXPECT_EQ(manager.resources().audit(), LedgerStatus::Ok);
}
TEST_F(ControllerState, WrongOperationEntityVersionsAndCompletionPayloadCannotAuthorizeRun) {
    submit(); static_cast<void>(worker()); auto command = assign();
    for (int field = 0; field != 8; ++field) {
        auto completion = committed(command);
        switch (field) {
            case 0: ++completion.correlation.operation_id; break;
            case 1: completion.correlation.task_id = uuid(999); break;
            case 2: completion.correlation.attempt_id = uuid(998); break;
            case 3: ++completion.correlation.expected_task_version; break;
            case 4: ++completion.correlation.expected_attempt_version; break;
            case 5: ++completion.correlation.entity_generation; break;
            case 6: ++completion.correlation.session_generation; break;
            case 7: completion.correlation.session_id = uuid(997); break;
        }
        EXPECT_EQ(manager.complete(completion), Decision::Stale);
    }
    auto malformed = committed(command); malformed.outcome.affected = 2;
    EXPECT_EQ(manager.complete(malformed), Decision::Stale);
    malformed = committed(command); malformed.outcome.tasks.emplace_back();
    EXPECT_EQ(manager.complete(malformed), Decision::Stale);
    EXPECT_EQ(effects.count(EffectKind::RunEligible), 0U);
    EXPECT_EQ(manager.complete(committed(command)), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::RunEligible), 1U);
}
TEST_F(ControllerState, RejectedAssignmentRollsBackOnlyTentativeReservation) {
    submit(); static_cast<void>(worker()); const auto command = assign();
    EXPECT_EQ(manager.complete(failed(command, storage::ErrorCode::Conflict)), Decision::Rejected);
    EXPECT_EQ(manager.resources().reservation_count(), 0U); EXPECT_EQ(manager.task(task_id())->state, TS::PENDING);
    EXPECT_TRUE(manager.attempts(task_id()).empty()); EXPECT_EQ(effects.count(EffectKind::RunEligible), 0U);
    EXPECT_EQ(manager.schedule(), Decision::Accepted); EXPECT_EQ(manager.resources().reservation_count(), 1U);
}
TEST_F(ControllerState, OldestInfeasibleTaskDoesNotBlockYoungerAndTentativePreventsOvercommit) {
    submit(1, 8); submit(2, 3); submit(3, 2); static_cast<void>(worker());
    const auto count = persistence.commands.size(); EXPECT_EQ(manager.schedule(), Decision::Accepted);
    ASSERT_EQ(persistence.commands.size(), count + 1);
    EXPECT_EQ(std::get<storage::Assign>(persistence.commands.back().command).expected.task_id, uuid(2));
    EXPECT_EQ(manager.schedule(), Decision::Accepted); EXPECT_EQ(persistence.commands.size(), count + 1);
}
TEST_F(ControllerState, AckAndQueuedStartAreSerializedWithoutSkippingDurableStates) {
    const auto run = active();
    ASSERT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
    const auto ack = persistence.commands.back(); const auto count = persistence.commands.size();
    EXPECT_EQ(manager.receive(task_id(), StartedEvent{*run.attempt_id, *run.session}), Decision::Deferred);
    EXPECT_EQ(persistence.commands.size(), count); EXPECT_EQ(manager.attempts(task_id()).back().state, AS::ASSIGNED);
    EXPECT_EQ(manager.complete(committed(ack)), Decision::Accepted);
    EXPECT_EQ(manager.attempts(task_id()).back().state, AS::STARTING);
    const auto start = persistence.commands.back(); ASSERT_TRUE(std::holds_alternative<storage::Advance>(start.command));
    EXPECT_EQ(std::get<storage::Advance>(start.command).expected.version, 1U);
    EXPECT_EQ(manager.complete(committed(start)), Decision::Accepted);
    EXPECT_EQ(manager.attempts(task_id()).back().state, AS::RUNNING);
    EXPECT_EQ(manager.task(task_id())->state_version, 1U); // M5 Advance changes Attempt only
    EXPECT_EQ(manager.receive(task_id(), StartedEvent{*run.attempt_id, *run.session}), Decision::Stale);
}
TEST_F(ControllerState, StartBeforeAcceptedAckCannotInventStartingTransition) {
    const auto run = active(); const auto count = persistence.commands.size();
    EXPECT_EQ(manager.receive(task_id(), StartedEvent{*run.attempt_id, *run.session}), Decision::Stale);
    EXPECT_EQ(persistence.commands.size(), count); EXPECT_EQ(manager.attempts(task_id()).back().state, AS::ASSIGNED);
}
TEST_F(ControllerState, PendingCancelIsDurableWithoutCreatingAttemptOrWorkerCommand) {
    submit(); ASSERT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::PENDING); EXPECT_EQ(effects.count(EffectKind::CancelCompleted), 0U);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::CANCELLED); EXPECT_TRUE(manager.attempts(task_id()).empty());
    EXPECT_EQ(effects.count(EffectKind::CancelCompleted), 1U); EXPECT_EQ(effects.count(EffectKind::CancelEligible), 0U);
}
TEST_F(ControllerState, CancellationDuringPendingAssignmentRevokesUnsentRunAndUsesProvenEmpty) {
    submit(); static_cast<void>(worker()); const auto command = assign();
    EXPECT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Deferred);
    EXPECT_EQ(manager.complete(committed(command)), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::RunEligible), 0U); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    const auto terminal = persistence.commands.back(); ASSERT_TRUE(std::holds_alternative<storage::Terminalize>(terminal.command));
    const auto& c = std::get<storage::Terminalize>(terminal.command);
    EXPECT_EQ(c.logs.kind, storage::LogEvidenceKind::ProvenEmpty); EXPECT_TRUE(c.logs.no_dispatch_bytes);
    EXPECT_EQ(c.to, AS::CANCELLED); EXPECT_EQ(manager.task(task_id())->state, TS::ACTIVE);
    EXPECT_EQ(manager.complete(committed(terminal)), Decision::Accepted);
    EXPECT_EQ(manager.resources().reservation_count(), 0U); EXPECT_TRUE(manager.attempts(task_id()).back().logs_complete);
}
TEST_F(ControllerState, ExplicitAdapterUnsentFactAllowsProvenEmptyButUncertainEligibilityDoesNot) {
    submit(); static_cast<void>(worker()); auto command = assign();
    ASSERT_EQ(manager.complete(committed(command)), Decision::Accepted);
    const auto run = effects.last(EffectKind::RunEligible);
    EXPECT_EQ(manager.record_dispatch(run, DispatchFact::NoBytesQueued), Decision::Accepted);
    EXPECT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Accepted);
    EXPECT_FALSE(manager.effect_is_current(run)); EXPECT_EQ(manager.record_dispatch(run, DispatchFact::BytesMayHaveEscaped), Decision::Stale);
    EXPECT_TRUE(std::holds_alternative<storage::Terminalize>(persistence.commands.back().command));
}
TEST_F(ControllerState, ActiveCancelIntentMustCommitBeforeTransportAndIsNotTerminal) {
    const auto run = active(); running(run);
    ASSERT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::CancelEligible), 0U); EXPECT_FALSE(manager.task(task_id())->cancel_requested_at);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::ACTIVE); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    ASSERT_EQ(effects.count(EffectKind::CancelEligible), 1U); EXPECT_TRUE(manager.effect_is_current(effects.last(EffectKind::CancelEligible)));
    const auto count = persistence.commands.size();
    EXPECT_EQ(manager.receive(task_id(), CancelEvent{client(3)}), Decision::Accepted);
    EXPECT_EQ(persistence.commands.size(), count); EXPECT_EQ(effects.count(EffectKind::CancelAccepted), 2U);
}
TEST_F(ControllerState, UncertainDispatchCancellationUsesIntentNotProvenEmpty) {
    submit(); static_cast<void>(worker()); auto assignment = assign(); ASSERT_EQ(manager.complete(committed(assignment)), Decision::Accepted);
    EXPECT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Accepted);
    EXPECT_TRUE(std::holds_alternative<storage::CancelIntent>(persistence.commands.back().command));
    EXPECT_EQ(manager.resources().reservation_count(), 1U);
}
TEST_F(ControllerState, FinalLogEvidenceAndCommitBothGateTerminalVisibilityAndRelease) {
    const auto run = active(); running(run); auto f = finish(run, false); const auto count = persistence.commands.size();
    EXPECT_EQ(manager.receive(task_id(), f), Decision::WaitingForLogs); EXPECT_TRUE(manager.finalization_pending(task_id()));
    EXPECT_EQ(persistence.commands.size(), count); EXPECT_EQ(manager.task(task_id())->state, TS::ACTIVE);
    auto wrong = finish(run); wrong.evidence.attempt_id = uuid(999);
    EXPECT_EQ(manager.receive(task_id(), wrong), Decision::Rejected);
    wrong = finish(run); wrong.evidence.namespace_synced = false;
    EXPECT_EQ(manager.receive(task_id(), wrong), Decision::Rejected);
    f = finish(run); EXPECT_EQ(manager.receive(task_id(), f), Decision::Accepted);
    const auto terminal = persistence.commands.back(); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    EXPECT_EQ(effects.count(EffectKind::TerminalVisible), 0U); EXPECT_FALSE(manager.attempts(task_id()).back().logs_complete);
    EXPECT_EQ(manager.complete(committed(terminal)), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::SUCCEEDED); EXPECT_TRUE(manager.attempts(task_id()).back().logs_complete);
    EXPECT_EQ(manager.resources().reservation_count(), 0U); EXPECT_FALSE(manager.finalization_pending(task_id()));
    EXPECT_EQ(manager.complete(committed(terminal)), Decision::Stale); EXPECT_EQ(effects.count(EffectKind::TerminalVisible), 1U);
}
TEST_F(ControllerState, ObservedLeaderOutcomeCannotChangeWhileWaitingForBarrier) {
    const auto run = active(); running(run); EXPECT_EQ(manager.receive(task_id(), finish(run, false)), Decision::WaitingForLogs);
    auto changed = finish(run); changed.exit_code = 1;
    EXPECT_EQ(manager.receive(task_id(), changed), Decision::Rejected);
    changed = finish(run); changed.stdout_path += "wrong";
    EXPECT_EQ(manager.receive(task_id(), changed), Decision::Rejected);
    EXPECT_EQ(manager.receive(task_id(), finish(run)), Decision::Accepted);
}
TEST_F(ControllerState, LogPersistenceFailureKeepsObservedOutcomeAndNeverClaimsCompleteLogs) {
    const auto run = active(); running(run); auto f = finish(run, false); f.log_persistence_failed = true;
    ASSERT_EQ(manager.receive(task_id(), f), Decision::Accepted);
    const auto& c = std::get<storage::Terminalize>(persistence.commands.back().command);
    EXPECT_EQ(c.to, AS::FAILED); EXPECT_EQ(c.reason, "LOG_PERSISTENCE_ERROR"); ASSERT_TRUE(c.observed_outcome_json);
    EXPECT_EQ(c.logs.kind, storage::LogEvidenceKind::Incomplete);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::FAILED); EXPECT_FALSE(manager.attempts(task_id()).back().logs_complete);
}
TEST_F(ControllerState, DispatchRejectionIsProvenEmptyAndManualRetryPreservesHistory) {
    const auto run = active(); ASSERT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, false}), Decision::Accepted);
    const auto& c = std::get<storage::Terminalize>(persistence.commands.back().command);
    EXPECT_TRUE(c.logs.explicit_dispatch_rejection); EXPECT_EQ(c.reason, "DISPATCH_REJECTED");
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    const auto history = manager.attempts(task_id()); ASSERT_EQ(history.size(), 1U);
    EXPECT_EQ(manager.pump(), Decision::Accepted); EXPECT_EQ(manager.attempts(task_id()).size(), 1U); // no automatic retry
    EXPECT_EQ(manager.receive(task_id(), RetryEvent{client(3)}), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::RetryAccepted), 0U);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::PENDING); EXPECT_FALSE(manager.task(task_id())->cancel_requested_at);
    EXPECT_EQ(manager.attempts(task_id()).back().state_version, history.back().state_version);
    auto next = assign(); EXPECT_EQ(manager.complete(committed(next)), Decision::Accepted);
    ASSERT_EQ(manager.attempts(task_id()).size(), 2U); EXPECT_EQ(manager.attempts(task_id()).back().attempt_no, 2U);
    EXPECT_EQ(manager.attempts(task_id()).front().attempt_id, history.front().attempt_id);
    EXPECT_EQ(manager.attempts(task_id()).front().state, AS::FAILED);
}
TEST_F(ControllerState, RetryIsRejectedForPendingActiveSucceededAndCancelled) {
    submit(); EXPECT_EQ(manager.receive(task_id(), RetryEvent{client(2)}), Decision::Rejected);
    static_cast<void>(worker()); auto assignment = assign(); ASSERT_EQ(manager.complete(committed(assignment)), Decision::Accepted);
    const auto run = effects.last(EffectKind::RunEligible); EXPECT_EQ(manager.receive(task_id(), RetryEvent{client(3)}), Decision::Rejected);
    running(run); EXPECT_EQ(manager.receive(task_id(), finish(run)), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.receive(task_id(), RetryEvent{client(4)}), Decision::Rejected);
    submit(2); EXPECT_EQ(manager.receive(task_id(2), CancelEvent{client(5)}), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.receive(task_id(2), RetryEvent{client(6)}), Decision::Rejected);
}
TEST_F(ControllerState, CancelAckIsOnlyReceiptAndNaturalResultCanStillWin) {
    const auto run = active(); running(run);
    ASSERT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    const auto count = persistence.commands.size();
    EXPECT_EQ(manager.receive(task_id(), CancelAckEvent{*run.attempt_id, *run.session}), Decision::Accepted);
    EXPECT_EQ(persistence.commands.size(), count); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    EXPECT_EQ(manager.task(task_id())->state, TS::ACTIVE);
    EXPECT_EQ(manager.receive(task_id(), finish(run)), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::SUCCEEDED);
}
TEST_F(ControllerState, OrdinaryPressureRetainsWorkerEventAndEssentialTerminalUsesReservedCapacity) {
    const auto run = active(); persistence.ordinary_full = true;
    EXPECT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, true}), Decision::Deferred);
    EXPECT_EQ(manager.retained().event_count, 1U); EXPECT_EQ(manager.attempts(task_id()).back().state, AS::ASSIGNED);
    persistence.ordinary_full = false;
    ASSERT_EQ(manager.pump(), Decision::Accepted); ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    ASSERT_EQ(manager.receive(task_id(), StartedEvent{*run.attempt_id, *run.session}), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    persistence.ordinary_full = true;
    EXPECT_EQ(manager.receive(task_id(), finish(run)), Decision::Accepted);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::SUCCEEDED);
}
TEST(ControllerBounds, NormalPendingCountCannotConsumeEssentialReservation) {
    Persistence p; Effects e; DecisionLimits limits; limits.pending_count = 3; limits.essential_count = 1;
    TaskManager m(p, e, limits);
    ASSERT_EQ(m.submit(spec(), client()), Decision::Accepted); ASSERT_EQ(m.complete(committed(p.commands.back())), Decision::Accepted);
    ASSERT_EQ(m.submit(spec(2), client(2)), Decision::Accepted); ASSERT_EQ(m.submit(spec(3), client(3)), Decision::Accepted);
    EXPECT_EQ(m.submit(spec(4), client(4)), Decision::Backpressure);
    EXPECT_EQ(m.receive(task_id(), CancelEvent{client(5)}), Decision::Backpressure); // ordinary cancellation, no reservation claim
    EXPECT_EQ(m.register_worker(reg()), Decision::Accepted);
    const auto key = m.workers().inspect(reg().worker_id)->current->key;
    EXPECT_EQ(m.invalidate(key), Decision::Accepted);
    EXPECT_EQ(m.retained().pending_count, 3U); EXPECT_TRUE(std::holds_alternative<storage::LoseSession>(p.commands.back().command));
    EXPECT_EQ(m.complete(committed(p.commands.back(), 0)), Decision::Accepted);
    EXPECT_EQ(m.workers().inspect(reg().worker_id)->state, domain::WorkerState::OFFLINE);
}
TEST(ControllerBounds, EventCountAndByteOverflowAreExplicitAndDoNotGrowQueues) {
    Persistence p; Effects e; DecisionLimits l; l.events_per_task = 1; l.event_count = 1;
    TaskManager m(p, e, l);
    ASSERT_EQ(m.submit(spec(), client()), Decision::Accepted);
    EXPECT_EQ(m.receive(task_id(), CancelEvent{client(2)}), Decision::Deferred);
    EXPECT_EQ(m.receive(task_id(), CancelEvent{client(3)}), Decision::Backpressure);
    EXPECT_EQ(m.retained().event_count, 1U);
    EXPECT_EQ(m.complete(committed(p.commands.front())), Decision::Accepted);
    EXPECT_EQ(m.retained().event_count, 0U);
    Persistence p2; Effects e2; l.event_bytes = 1; TaskManager m2(p2, e2, l);
    ASSERT_EQ(m2.submit(spec(), client()), Decision::Accepted);
    EXPECT_EQ(m2.receive(task_id(), CancelEvent{client(2)}), Decision::Backpressure);
    EXPECT_EQ(m2.retained().event_count, 0U);
}
TEST(ControllerBounds, PendingByteBoundRejectsOversizedMetadataBeforePortAdmission) {
    Persistence p; Effects e; DecisionLimits l; l.pending_bytes = 1000; l.essential_bytes = 500;
    TaskManager m(p, e, l); auto large = spec(); large.args.push_back(std::string(2000, 'x'));
    EXPECT_EQ(m.submit(large, client()), Decision::Backpressure); EXPECT_TRUE(p.commands.empty());
    EXPECT_EQ(m.retained().pending_bytes, 0U);
}
TEST_F(ControllerState, RequiredWorkerEventOverflowFailStopsWithoutReleasingReservation) {
    const auto run = active(); running(run); auto huge = finish(run, false);
    huge.reason = std::string(5U * 1024U * 1024U, 'x');
    EXPECT_EQ(manager.receive(task_id(), huge), Decision::Degraded); EXPECT_TRUE(manager.degraded());
    EXPECT_EQ(manager.resources().reservation_count(), 1U); EXPECT_EQ(manager.retained().event_count, 0U);
    EXPECT_EQ(effects.count(EffectKind::TerminalVisible), 0U);
}
TEST_F(ControllerState, EssentialAdmissionFailureNeverReleasesOrExposesTerminal) {
    const auto run = active(); running(run); persistence.admission = storage::Admission::Full;
    EXPECT_EQ(manager.receive(task_id(), finish(run)), Decision::Degraded);
    EXPECT_EQ(manager.task(task_id())->state, TS::ACTIVE); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    EXPECT_EQ(effects.count(EffectKind::TerminalVisible), 0U); EXPECT_EQ(effects.count(EffectKind::FailStop), 1U);
    EXPECT_EQ(manager.schedule(), Decision::Degraded);
}
TEST(ControllerFailures, FatalIoFullBusyAbortedAndUnknownCommitNeverAuthorizeRunOrRelease) {
    for (const auto code : {storage::ErrorCode::Io, storage::ErrorCode::Full, storage::ErrorCode::Busy,
                           storage::ErrorCode::Aborted, storage::ErrorCode::UnknownCommit}) {
        Persistence p; Effects e; TaskManager m(p, e);
        ASSERT_EQ(m.submit(spec(), client()), Decision::Accepted); ASSERT_EQ(m.complete(committed(p.commands.back())), Decision::Accepted);
        ASSERT_EQ(m.register_worker(reg()), Decision::Accepted); ASSERT_EQ(m.schedule(), Decision::Accepted);
        const auto assignment = p.commands.back();
        EXPECT_EQ(m.complete(failed(assignment, code, code != storage::ErrorCode::UnknownCommit)), Decision::Degraded);
        EXPECT_EQ(m.resources().reservation_count(), code == storage::ErrorCode::UnknownCommit ? 1U : 0U);
        EXPECT_EQ(m.task(task_id())->state, TS::PENDING);
        EXPECT_EQ(e.count(EffectKind::RunEligible), 0U); EXPECT_EQ(e.count(EffectKind::FailStop), 1U);
        EXPECT_EQ(m.complete(committed(assignment)), Decision::Degraded); EXPECT_EQ(m.schedule(), Decision::Degraded);
    }
}
TEST_F(ControllerState, TerminalCommitFailureHoldsResourcesAndLastCommittedState) {
    const auto run = active(); running(run); ASSERT_EQ(manager.receive(task_id(), finish(run)), Decision::Accepted);
    EXPECT_EQ(manager.complete(failed(persistence.commands.back(), storage::ErrorCode::UnknownCommit)), Decision::Degraded);
    EXPECT_EQ(manager.task(task_id())->state, TS::ACTIVE); EXPECT_EQ(manager.attempts(task_id()).back().state, AS::RUNNING);
    EXPECT_EQ(manager.resources().reservation_count(), 1U); EXPECT_EQ(effects.count(EffectKind::TerminalVisible), 0U);
}
TEST_F(ControllerState, BulkLossWaitsForEveryInflightAssignmentAndGatesReplacementAck) {
    submit(); submit(2); const auto old = worker(); static_cast<void>(assign());
    const auto first = persistence.commands[2]; const auto second = persistence.commands[3];
    auto replacement = reg(1001, "worker", 2);
    const auto count = persistence.commands.size(); EXPECT_EQ(manager.register_worker(replacement), Decision::Accepted);
    EXPECT_EQ(persistence.commands.size(), count); EXPECT_EQ(effects.count(EffectKind::RegistrationAccepted), 1U);
    EXPECT_EQ(manager.resources().reservation_count(), 2U); EXPECT_FALSE(manager.workers().valid(old, 0));
    EXPECT_EQ(manager.workers().inspect(old.worker_id)->state, domain::WorkerState::HEALTHY);
    ASSERT_EQ(manager.complete(committed(second)), Decision::Accepted); EXPECT_EQ(persistence.commands.size(), count);
    ASSERT_EQ(manager.complete(committed(first)), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::RunEligible), 0U); ASSERT_EQ(persistence.commands.size(), count + 1);
    const auto loss = persistence.commands.back(); ASSERT_TRUE(std::holds_alternative<storage::LoseSession>(loss.command));
    EXPECT_EQ(manager.resources().reservation_count(), 2U); EXPECT_EQ(effects.count(EffectKind::CloseConnection), 0U);
    EXPECT_EQ(manager.heartbeat(old, 1), Decision::Stale);
    ASSERT_EQ(manager.complete(committed(loss, 2)), Decision::Accepted);
    EXPECT_EQ(manager.resources().reservation_count(), 0U); EXPECT_EQ(manager.attempts(task_id()).back().state, AS::LOST);
    EXPECT_FALSE(manager.attempts(task_id()).back().logs_complete); EXPECT_EQ(manager.task(task_id(2))->state, TS::FAILED);
    EXPECT_EQ(effects.count(EffectKind::WorkerOffline), 1U); EXPECT_EQ(effects.count(EffectKind::CloseConnection), 1U);
    ASSERT_EQ(effects.count(EffectKind::RegistrationAccepted), 2U);
    const auto next = manager.workers().inspect(old.worker_id)->current->key;
    EXPECT_GT(next.generation, old.generation); EXPECT_EQ(next.connection.value, old.connection.value);
    const auto accepted = effects.last(EffectKind::RegistrationAccepted);
    const auto close_it = std::find_if(effects.values.begin(), effects.values.end(), [](const auto& e) { return e.kind == EffectKind::CloseConnection; });
    EXPECT_LT(static_cast<std::size_t>(close_it - effects.values.begin()), effects.values.size() - 1);
    EXPECT_EQ(accepted.session->session_id, replacement.session_id);
    EXPECT_EQ(manager.complete(committed(first)), Decision::Stale); EXPECT_EQ(manager.invalidate(old), Decision::Stale);
    EXPECT_TRUE(manager.workers().valid(next, 0)); EXPECT_EQ(manager.resources().audit(), LedgerStatus::Ok);
}
TEST_F(ControllerState, MixedInflightAdvanceAndAssignmentSerializeBeforeOneBulkLoss) {
    const auto run = active();
    ASSERT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
    const auto ack = persistence.commands.back(); submit(2); const auto assignment = assign();
    ASSERT_EQ(manager.invalidate(*run.session, true), Decision::Accepted);
    const auto count = persistence.commands.size();
    EXPECT_EQ(manager.complete(committed(assignment)), Decision::Accepted); EXPECT_EQ(persistence.commands.size(), count);
    EXPECT_EQ(manager.complete(committed(ack)), Decision::Accepted); EXPECT_EQ(persistence.commands.size(), count + 1);
    const auto loss = persistence.commands.back(); EXPECT_EQ(manager.resources().reservation_count(), 2U);
    EXPECT_EQ(manager.complete(committed(loss, 2)), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::CloseConnection), 0U); EXPECT_EQ(effects.count(EffectKind::RunEligible), 1U);
    EXPECT_EQ(manager.resources().reservation_count(), 0U);
}
TEST_F(ControllerState, BulkLossDoesNotTouchAnotherSessionAndUnknownCommitBlocksReplacement) {
    const auto run = active(); running(run);
    auto other = reg(2000, "other", 1); other.connection.value = 8; static_cast<void>(worker(other));
    submit(2); const auto assignment = assign(); ASSERT_EQ(manager.complete(committed(assignment)), Decision::Accepted);
    const auto other_run = effects.last(EffectKind::RunEligible); ASSERT_EQ(other_run.session->worker_id, other.worker_id);
    ASSERT_EQ(manager.register_worker(reg(1001, "worker", 2)), Decision::Accepted);
    const auto loss = persistence.commands.back();
    EXPECT_EQ(manager.complete(failed(loss, storage::ErrorCode::UnknownCommit)), Decision::Degraded);
    EXPECT_EQ(manager.resources().reservation_count(), 2U); EXPECT_EQ(effects.count(EffectKind::RegistrationAccepted), 2U);
    EXPECT_EQ(manager.task(task_id())->state, TS::ACTIVE); EXPECT_EQ(manager.task(task_id(2))->state, TS::ACTIVE);
}
TEST_F(ControllerState, SuccessfulBulkLossLeavesUnrelatedAttemptAndReservationIntact) {
    const auto run = active(); running(run);
    auto other = reg(2000, "other"); other.connection.value = 8; static_cast<void>(worker(other));
    submit(2); const auto command = assign(); ASSERT_EQ(manager.complete(committed(command)), Decision::Accepted);
    ASSERT_EQ(manager.invalidate(*run.session), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::FAILED); EXPECT_EQ(manager.task(task_id(2))->state, TS::ACTIVE);
    EXPECT_EQ(manager.resources().reservation_count(), 1U); EXPECT_EQ(manager.resources().audit(), LedgerStatus::Ok);
}
TEST_F(ControllerState, EssentialSessionLossAdmissionFailureDoesNotReuseOrAckNewSession) {
    const auto run = active(); persistence.admission = storage::Admission::Full;
    EXPECT_EQ(manager.register_worker(reg(1001, "worker", 2)), Decision::Degraded);
    EXPECT_EQ(effects.count(EffectKind::RegistrationAccepted), 1U); EXPECT_EQ(effects.count(EffectKind::CloseConnection), 0U);
    EXPECT_EQ(manager.resources().reservation_count(), 1U); EXPECT_FALSE(manager.workers().valid(*run.session, 0));
}
TEST_F(ControllerState, MissingAckStartDeadlineAndHeartbeatExpiryUseConservativeLost) {
    const auto run = active();
    EXPECT_EQ(manager.tick(4999, common::Timestamp::from_unix_microseconds(4999000)), Decision::Accepted);
    EXPECT_TRUE(manager.workers().valid(*run.session, 4999));
    EXPECT_EQ(manager.tick(5000, common::Timestamp::from_unix_microseconds(5000000)), Decision::Accepted);
    EXPECT_FALSE(manager.workers().valid(*run.session, 5000)); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.attempts(task_id()).back().state, AS::LOST);
}
TEST_F(ControllerState, MissingStartInvalidatesEvenWithAdvancingHeartbeats) {
    const auto run = active();
    ASSERT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    ASSERT_EQ(manager.tick(9000, common::Timestamp::from_unix_microseconds(9000000)), Decision::Accepted);
    ASSERT_EQ(manager.heartbeat(*run.session, 1), Decision::Accepted);
    EXPECT_EQ(manager.tick(10000, common::Timestamp::from_unix_microseconds(10000000)), Decision::Accepted);
    EXPECT_FALSE(manager.workers().valid(*run.session, 10000));
    EXPECT_TRUE(std::holds_alternative<storage::LoseSession>(persistence.commands.back().command));
}
TEST_F(ControllerState, ExplicitUncertainSendInvalidatesRatherThanPretendingDispatchRejection) {
    submit(); static_cast<void>(worker()); auto assignment = assign(); ASSERT_EQ(manager.complete(committed(assignment)), Decision::Accepted);
    const auto run = effects.last(EffectKind::RunEligible);
    EXPECT_EQ(manager.record_dispatch(run, DispatchFact::Uncertain), Decision::Accepted);
    EXPECT_TRUE(std::holds_alternative<storage::LoseSession>(persistence.commands.back().command));
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.attempts(task_id()).back().state, AS::LOST); EXPECT_FALSE(manager.attempts(task_id()).back().logs_complete);
}
TEST_F(ControllerState, RecoveredPendingAndTerminalHistoryDoNotRestoreWorkerLeases) {
    storage::TaskRow row; row.spec = spec(); row.state = TS::FAILED; row.state_version = 2;
    row.created_at = row.updated_at = common::Timestamp::from_unix_microseconds(0).to_string(); row.latest_attempt_id = uuid(3);
    storage::AttemptRow a; a.task_id = uuid(1); a.attempt_id = uuid(3); a.state = AS::LOST; a.reason = "CONTROLLER_RESTART";
    a.worker_id = "worker"; a.session_id = uuid(1000); a.stdout_path = "out"; a.stderr_path = "err";
    ASSERT_EQ(manager.restore({row}, {a}), Decision::Accepted);
    EXPECT_EQ(manager.resources().reservation_count(), 0U); EXPECT_FALSE(manager.workers().inspect(reg().worker_id));
    EXPECT_EQ(manager.receive(task_id(), RetryEvent{client()}), Decision::Accepted);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.attempts(task_id()).back().state, AS::LOST); EXPECT_EQ(manager.task(task_id())->state, TS::PENDING);
}
TEST_F(ControllerState, SeededCombinedChurnMaintainsGenerationAndResourceConservation) {
    std::mt19937 random(604); std::uint64_t old_generation = 0;
    for (std::uint64_t cycle = 1; cycle <= 80; ++cycle) {
        const auto key = worker(reg(1000 + cycle, "worker", cycle));
        ASSERT_GT(key.generation, old_generation); old_generation = key.generation;
        submit(cycle * 2); submit(cycle * 2 + 1);
        const auto index = persistence.commands.size(); static_cast<void>(assign());
        auto first = persistence.commands[index]; auto second = persistence.commands[index + 1];
        if (random() % 2) std::swap(first, second);
        ASSERT_EQ(manager.complete(committed(first)), Decision::Accepted);
        ASSERT_EQ(manager.complete(committed(second)), Decision::Accepted);
        ASSERT_EQ(manager.resources().reservation_count(), 2U); ASSERT_EQ(manager.resources().audit(), LedgerStatus::Ok);
        std::vector<Effect> runs;
        for (const auto& effect : effects.values) if (effect.kind == EffectKind::RunEligible && effect.session && *effect.session == key)
            runs.push_back(effect);
        ASSERT_EQ(runs.size(), 2U);
        for (const auto& run : runs) { ASSERT_EQ(manager.record_dispatch(run, DispatchFact::BytesMayHaveEscaped), Decision::Accepted); running(run); }
        if (random() % 2) {
            ASSERT_EQ(manager.receive(*runs.front().task_id, finish(runs.front())), Decision::Accepted);
            ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
        }
        const auto remaining = manager.resources().reservation_count();
        ASSERT_EQ(manager.invalidate(key, true), Decision::Accepted);
        ASSERT_EQ(manager.complete(committed(persistence.commands.back(), remaining)), Decision::Accepted);
        ASSERT_EQ(manager.resources().reservation_count(), 0U); ASSERT_EQ(manager.resources().audit(), LedgerStatus::Ok);
        ASSERT_FALSE(manager.workers().valid(key, 0));
        for (const auto& run : runs) EXPECT_FALSE(manager.effect_is_current(run));
        EXPECT_EQ(manager.complete(committed(first)), Decision::Stale);
    }
}
TEST_F(ControllerState, HeartbeatExpiryUsesMonotonicTimeAndDoesNotExposeOfflineBeforeCommit) {
    const auto key = worker();
    EXPECT_EQ(manager.tick(3000, common::Timestamp::from_unix_microseconds(3000000)), Decision::Accepted);
    EXPECT_EQ(manager.heartbeat(key, 1), Decision::Accepted);
    EXPECT_EQ(manager.tick(6000, common::Timestamp::from_unix_microseconds(1000000)), Decision::Accepted); // UTC may go backwards
    EXPECT_EQ(manager.heartbeat(key, 1), Decision::Stale);
    EXPECT_EQ(manager.tick(12999, common::Timestamp::from_unix_microseconds(0)), Decision::Accepted);
    EXPECT_TRUE(manager.workers().valid(key, 12999));
    EXPECT_EQ(manager.tick(13000, common::Timestamp::from_unix_microseconds(0)), Decision::Accepted);
    EXPECT_EQ(manager.heartbeat(key, 2), Decision::Stale);
    EXPECT_EQ(manager.workers().inspect(key.worker_id)->state, domain::WorkerState::HEALTHY);
    EXPECT_EQ(manager.complete(committed(persistence.commands.back(), 0)), Decision::Accepted);
    const auto record = manager.workers().inspect(key.worker_id); EXPECT_EQ(record->state, domain::WorkerState::OFFLINE);
    EXPECT_FALSE(record->current); EXPECT_EQ(record->last_heartbeat_at->unix_microseconds(), 3000000);
    EXPECT_EQ(manager.tick(12999, common::Timestamp::from_unix_microseconds(0)), Decision::Rejected);
}
TEST_F(ControllerState, InflightTerminalCommitWinsBeforeDeferredLossWithoutDoubleRelease) {
    const auto run = active(); running(run);
    ASSERT_EQ(manager.receive(task_id(), finish(run)), Decision::Accepted); const auto terminal = persistence.commands.back();
    const auto size = persistence.commands.size(); ASSERT_EQ(manager.invalidate(*run.session), Decision::Accepted);
    EXPECT_EQ(persistence.commands.size(), size); EXPECT_EQ(manager.resources().reservation_count(), 1U);
    ASSERT_EQ(manager.complete(committed(terminal)), Decision::Accepted);
    EXPECT_EQ(manager.task(task_id())->state, TS::SUCCEEDED); EXPECT_EQ(manager.resources().reservation_count(), 0U);
    ASSERT_TRUE(std::holds_alternative<storage::LoseSession>(persistence.commands.back().command));
    ASSERT_EQ(manager.complete(committed(persistence.commands.back(), 0)), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::TerminalVisible), 1U); EXPECT_TRUE(manager.attempts(task_id()).back().logs_complete);
}
TEST_F(ControllerState, InvalidatedSessionRejectsFactsWithoutConsumingDeferredCapacity) {
    const auto run = active(); ASSERT_EQ(manager.invalidate(*run.session), Decision::Accepted);
    const auto count = manager.retained().event_count;
    EXPECT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, false}), Decision::Stale);
    EXPECT_EQ(manager.receive(task_id(), StartedEvent{*run.attempt_id, *run.session}), Decision::Stale);
    EXPECT_EQ(manager.retained().event_count, count);
    auto wrong_connection = *run.session; ++wrong_connection.connection.generation;
    EXPECT_EQ(manager.heartbeat(wrong_connection, 1), Decision::Stale);
    EXPECT_EQ(manager.invalidate(wrong_connection), Decision::Stale);
}
TEST_F(ControllerState, DeferredClientRequestRevalidatesAfterFirstTerminalWinner) {
    const auto run = active(); running(run); ASSERT_EQ(manager.receive(task_id(), finish(run)), Decision::Accepted);
    const auto terminal = persistence.commands.back();
    EXPECT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Deferred);
    EXPECT_EQ(manager.complete(committed(terminal)), Decision::Accepted);
    EXPECT_EQ(effects.count(EffectKind::Rejected), 1U); EXPECT_EQ(effects.count(EffectKind::CancelAccepted), 0U);
    EXPECT_EQ(manager.task(task_id())->state, TS::SUCCEEDED);
}
TEST_F(ControllerState, LaunchFailureAndTimeoutAndCancellationMapOnlyFrozenTerminalStates) {
    for (std::uint64_t n = 1; n <= 3; ++n) {
        const auto run = active(n);
        ASSERT_EQ(manager.receive(*run.task_id, AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
        ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
        if (n != 1) {
            ASSERT_EQ(manager.receive(*run.task_id, StartedEvent{*run.attempt_id, *run.session}), Decision::Accepted);
            ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
        }
        auto f = finish(run); f.exit_code.reset(); f.reason = "observed runtime result";
        f.outcome = n == 1 ? ObservedOutcome::LaunchFailed : n == 2 ? ObservedOutcome::TimedOut : ObservedOutcome::Cancelled;
        ASSERT_EQ(manager.receive(*run.task_id, f), Decision::Accepted);
        ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
        EXPECT_EQ(manager.task(*run.task_id)->state, n == 3 ? TS::CANCELLED : TS::FAILED);
        EXPECT_EQ(manager.attempts(*run.task_id).back().state, n == 1 ? AS::FAILED : n == 2 ? AS::TIMED_OUT : AS::CANCELLED);
        EXPECT_EQ(manager.resources().reservation_count(), 0U);
    }
}
TEST(ControllerBounds, EffectPortFullIsExplicitFailStopNotSilentSuccess) {
    Persistence p; Effects e; TaskManager m(p, e); ASSERT_EQ(m.submit(spec(), client()), Decision::Accepted);
    e.admission = EffectAdmission::Full;
    EXPECT_EQ(m.complete(committed(p.commands.back())), Decision::Degraded);
    EXPECT_TRUE(m.failure()); EXPECT_EQ(e.count(EffectKind::SubmitAccepted), 0U);
    EXPECT_EQ(m.task(task_id())->state, TS::PENDING); // actual committed DB fact is not rolled back
}
TEST(ControllerBounds, NestedSchedulingTriggerCoalescesInsteadOfRecursing) {
    struct Trigger final : EffectPort {
        TaskManager* manager{nullptr};
        std::size_t calls{0};
        EffectAdmission emit(Effect effect) override {
            ++calls;
            if (effect.kind == EffectKind::SubmitAccepted) { EXPECT_EQ(manager->schedule(), Decision::WrongOwner); }
            return EffectAdmission::Accepted;
        }
    } e;
    Persistence p; TaskManager m(p, e); e.manager = &m;
    ASSERT_EQ(m.register_worker(reg()), Decision::Accepted); ASSERT_EQ(m.submit(spec(), client()), Decision::Accepted);
    ASSERT_EQ(m.complete(committed(p.commands.back())), Decision::Accepted);
    EXPECT_EQ(p.commands.size(), 1U); EXPECT_EQ(m.pump(), Decision::Accepted);
    EXPECT_EQ(p.commands.size(), 2U); EXPECT_EQ(m.pump(), Decision::Accepted); EXPECT_EQ(p.commands.size(), 2U);
    EXPECT_EQ(e.calls, 2U);
}
TEST_F(ControllerState, AssignmentRollbackRequestsLaterPassWithoutRecursivePlacement) {
    submit(); static_cast<void>(worker()); const auto assignment = assign(); const auto size = persistence.commands.size();
    EXPECT_EQ(manager.complete(failed(assignment, storage::ErrorCode::Conflict)), Decision::Rejected);
    EXPECT_EQ(persistence.commands.size(), size); EXPECT_EQ(manager.resources().reservation_count(), 0U);
    EXPECT_EQ(manager.pump(), Decision::Accepted); EXPECT_EQ(persistence.commands.size(), size + 1);
    EXPECT_EQ(manager.resources().reservation_count(), 1U);
}
TEST_F(ControllerState, ReceivedAckDuringPendingCancelDoesNotExpireAsMissingAck) {
    const auto run = active(); ASSERT_EQ(manager.receive(task_id(), CancelEvent{client(2)}), Decision::Accepted);
    const auto intent = persistence.commands.back();
    EXPECT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, true}), Decision::Deferred);
    EXPECT_EQ(manager.tick(6000, common::Timestamp::from_unix_microseconds(6000000)), Decision::Accepted);
    EXPECT_TRUE(manager.workers().valid(*run.session, 6000));
    EXPECT_EQ(manager.complete(committed(intent)), Decision::Accepted);
    EXPECT_TRUE(std::holds_alternative<storage::Advance>(persistence.commands.back().command));
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.attempts(task_id()).back().state, AS::STARTING);
}
TEST_F(ControllerState, LaunchResultAwaitingLogsDoesNotExpireAsMissingStart) {
    const auto run = active(); ASSERT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
    ASSERT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    auto f = finish(run, false); f.outcome = ObservedOutcome::LaunchFailed; f.exit_code.reset(); f.reason = "execve failed";
    ASSERT_EQ(manager.receive(task_id(), f), Decision::WaitingForLogs);
    ASSERT_EQ(manager.tick(9000, common::Timestamp::from_unix_microseconds(9000000)), Decision::Accepted);
    ASSERT_EQ(manager.heartbeat(*run.session, 1), Decision::Accepted);
    EXPECT_EQ(manager.tick(10000, common::Timestamp::from_unix_microseconds(10000000)), Decision::Accepted);
    EXPECT_TRUE(manager.workers().valid(*run.session, 10000)); EXPECT_TRUE(manager.finalization_pending(task_id()));
    EXPECT_EQ(manager.resources().reservation_count(), 1U);
}
TEST(ControllerBounds, OwnerAndLimitGuardsRejectWorkWithoutCallingPorts) {
    Persistence p; Effects e; TaskManager m(p, e);
    Decision result = Decision::Accepted;
    std::thread other([&] { result = m.submit(spec(), client()); }); other.join();
    EXPECT_EQ(result, Decision::WrongOwner); EXPECT_TRUE(p.commands.empty()); EXPECT_TRUE(e.values.empty());
    DecisionLimits limits; limits.essential_count = 0;
    EXPECT_THROW(TaskManager(p, e, limits), std::invalid_argument);
    limits = {}; limits.offline_timeout_ms = limits.heartbeat_interval_ms;
    EXPECT_THROW(TaskManager(p, e, limits), std::invalid_argument);
}
TEST_F(ControllerState, ActiveCasConflictReconcilesThroughDurableLostWithoutFakeRollback) {
    const auto run = active();
    ASSERT_EQ(manager.receive(task_id(), AckEvent{*run.attempt_id, *run.session, true}), Decision::Accepted);
    const auto ack = persistence.commands.back();
    EXPECT_EQ(manager.complete(failed(ack, storage::ErrorCode::Conflict)), Decision::Rejected);
    EXPECT_FALSE(manager.degraded()); EXPECT_EQ(manager.attempts(task_id()).back().state, AS::ASSIGNED);
    EXPECT_EQ(manager.resources().reservation_count(), 1U); EXPECT_FALSE(manager.workers().valid(*run.session, 0));
    EXPECT_TRUE(std::holds_alternative<storage::LoseSession>(persistence.commands.back().command));
    EXPECT_EQ(manager.complete(committed(persistence.commands.back())), Decision::Accepted);
    EXPECT_EQ(manager.attempts(task_id()).back().state, AS::LOST); EXPECT_EQ(manager.resources().reservation_count(), 0U);
}
} // namespace
} // namespace ts::controller
