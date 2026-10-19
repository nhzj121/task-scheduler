#include "../helpers/storage_fixture.h"

#include <limits>
#include "ts/io/event_loop.h"
#include "ts/io/timer_fd.h"

namespace ts::storage::test {
using AS = domain::AttemptState;

TEST_F(StoreTest, CountReserveIsUsableByEssentialWorkAndCompletionsAreLossless) {
    channel.reset(); ChannelLimits limits; limits.count = 6; limits.essential_count = 2; open(limits);
    hook->arm(SqlStage::Step, "INSERT INTO tasks", SQLITE_OK, 1, true);
    for (unsigned int i = 1; i <= 4; ++i)
        EXPECT_EQ(channel->try_post(envelope(Submit{spec(i), kAt}, 512)), Admission::Accepted);
    EXPECT_TRUE(hook->wait_entered());
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(5), kAt}, 512)), Admission::Full);
    EXPECT_EQ(channel->try_post(envelope(Recover{kLater}, 512)), Admission::Accepted);
    EXPECT_EQ(channel->try_post(envelope(LoseSession{uuid(1000), kLater, "TEST"}, 512)), Admission::Accepted);
    EXPECT_EQ(channel->usage().count, 6U); hook->release();
    channel->stop(); channel->join(); // consumer is stalled throughout execution
    EXPECT_EQ(channel->usage().count, 6U);
    const auto results = wait(6); ASSERT_EQ(results.size(), 6U);
    const std::vector<std::uint64_t> accepted_ids{1, 2, 3, 4, 6, 7};
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].outcome.committed); EXPECT_EQ(results[i].correlation.operation_id, accepted_ids[i]);
    }
    EXPECT_EQ(channel->usage().count, 0U); EXPECT_EQ(channel->usage().command_bytes, 0U);
    EXPECT_EQ(channel->usage().completion_bytes, 0U);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(6), kAt})), Admission::Stopped);
}

TEST_F(StoreTest, MetadataByteLimitCannotBeBypassedBySmallCommandCount) {
    channel.reset(); auto e = envelope(Submit{spec(1), kAt}, 512);
    const auto bytes = owned_bytes(e);
    ChannelLimits limits; limits.count = 20; limits.essential_count = 2;
    limits.essential_command_bytes = 4096; limits.command_bytes = bytes * 2 + 4096;
    open(limits);
    hook->arm(SqlStage::Step, "INSERT INTO tasks", SQLITE_OK, 1, true);
    EXPECT_EQ(channel->try_post(e), Admission::Accepted); EXPECT_TRUE(hook->wait_entered());
    EXPECT_EQ(channel->try_post(e), Admission::Accepted);
    EXPECT_EQ(channel->try_post(e), Admission::Full);
    EXPECT_LT(channel->usage().count, limits.count - limits.essential_count);
    EXPECT_EQ(channel->usage().command_bytes, bytes * 2);
    EXPECT_EQ(channel->try_post(envelope(Recover{kLater}, 512)), Admission::Accepted);
    hook->release(); auto results = wait(3); ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(results[0].outcome.committed); EXPECT_FALSE(results[1].outcome.ok());
    EXPECT_TRUE(results[2].outcome.committed);
}

TEST_F(StoreTest, CompletionByteReservationSurvivesConsumerPauseAndProtectsEssentialCredit) {
    channel.reset(); ChannelLimits limits;
    limits.completion_bytes = 2048; limits.essential_completion_bytes = 1024; open(limits);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(1), kAt}, 512)), Admission::Accepted);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(2), kAt}, 512)), Admission::Accepted);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(3), kAt}, 512)), Admission::Full);
    EXPECT_EQ(channel->try_post(envelope(Recover{kLater}, 512)), Admission::Accepted);
    EXPECT_EQ(channel->usage().completion_bytes, 1536U);
    auto results = wait(3); EXPECT_EQ(results.size(), 3U);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(3), kAt}, 512)), Admission::Accepted);
    EXPECT_TRUE(wait(1)[0].outcome.committed);
}

TEST_F(StoreTest, OversizedMetadataAndForgedCorrelationAreExplicitRejections) {
    auto s = spec(1); s.args.push_back(std::string(5U * 1024U * 1024U, 'x'));
    EXPECT_EQ(channel->try_post(envelope(Submit{s, kAt})), Admission::Oversized);
    auto e = envelope(Submit{spec(1), kAt}); e.response_bytes = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(channel->try_post(e), Admission::Oversized);
    e = envelope(Submit{spec(1), kAt}); e.correlation.task_id = uuid(99);
    EXPECT_NE(channel->try_post(e), Admission::Accepted);
    EXPECT_EQ(channel->usage().count, 0U);
    EXPECT_EQ(channel->status().health, Health::Ready);
}

TEST_F(StoreTest, EssentialAdmissionExhaustionIsFatalNotSilentNormalBackpressure) {
    channel.reset(); ChannelLimits limits; limits.count = 2; limits.essential_count = 1; open(limits);
    hook->arm(SqlStage::Step, "INSERT INTO tasks", SQLITE_OK, 1, true);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(1), kAt}, 512)), Admission::Accepted);
    EXPECT_TRUE(hook->wait_entered());
    EXPECT_EQ(channel->try_post(envelope(Recover{kLater}, 512)), Admission::Accepted);
    EXPECT_EQ(channel->try_post(envelope(Recover{kLater}, 512)), Admission::Degraded);
    ASSERT_TRUE(channel->status().error); EXPECT_EQ(channel->status().error->code, ErrorCode::Capacity);
    hook->release(); const auto results = wait(2); ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(results[0].outcome.committed); ASSERT_TRUE(results[1].outcome.error);
    EXPECT_EQ(results[1].outcome.error->code, ErrorCode::Aborted);
}

TEST_F(StoreTest, SlowSqliteDoesNotBlockEventLoopTimerOrCompletionOwner) {
    auto created = io::EventLoop::create(); ASSERT_TRUE(created); auto loop = std::move(created).value();
    auto timer_result = io::TimerFd::create(); ASSERT_TRUE(timer_result); auto timer = std::move(timer_result).value();
    bool timer_fired = false;
    auto registration = loop->add(timer.fd(), io::kReadable, [&](const auto&, auto) {
        EXPECT_TRUE(timer.drain()); timer_fired = true;
    }); ASSERT_TRUE(registration);
    hook->arm(SqlStage::Step, "INSERT INTO tasks", SQLITE_OK, 1, true);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(1), kAt})), Admission::Accepted);
    EXPECT_TRUE(hook->wait_entered());
    ASSERT_TRUE(timer.arm_once(std::chrono::milliseconds(1)));
    ASSERT_TRUE(loop->poll_once(1000)); EXPECT_TRUE(timer_fired);
    EXPECT_TRUE(channel->drain().value().empty());
    hook->release(); EXPECT_TRUE(wait(1)[0].outcome.committed);
    EXPECT_TRUE(loop->remove(registration.value()));
}

TEST_F(StoreTest, EventfdDrainHandlesStaleNotificationsAndResumeWithoutLostWakeups) {
    auto created = io::EventLoop::create(); ASSERT_TRUE(created); auto loop = std::move(created).value();
    std::vector<std::uint64_t> operations;
    auto registration = loop->add(channel->fd(), io::kReadable, [&](const auto&, auto) {
        auto values = channel->drain(); ASSERT_TRUE(values);
        for (const auto& c : values.value()) operations.push_back(c.correlation.operation_id);
    }); ASSERT_TRUE(registration);
    for (unsigned int i = 1; i <= 40; ++i) {
        const auto e = envelope(Submit{spec(i), kAt}, 512);
        EXPECT_EQ(channel->try_post(e), Admission::Accepted);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (operations.size() < i && std::chrono::steady_clock::now() < end) EXPECT_TRUE(loop->poll_once(100));
        ASSERT_EQ(operations.size(), i); EXPECT_EQ(operations.back(), e.correlation.operation_id);
        EXPECT_TRUE(channel->drain().value().empty());
    }
    EXPECT_EQ(channel->usage().count, 0U); EXPECT_TRUE(loop->remove(registration.value()));
}

// Test-only live owner. Persistence never dereferences this object. A real M6
// TaskManager must also serialize one outstanding metadata operation per Task.
struct Gate {
    std::optional<Correlation> pending;
    std::size_t effects{0};
    bool begin(const Correlation& c) { if (pending) return false; pending = c; return true; }
    bool apply(const Completion& c) {
        if (!pending || !(c.correlation == *pending)) return false;
        pending.reset();
        if (!c.outcome.ok() || !c.outcome.committed) return false;
        ++effects; return true;
    }
};
TEST_F(StoreTest, OwnerRejectsStaleIdsVersionsGenerationsAndDuplicateCompletion) {
    auto e = envelope(Submit{spec(1), kAt}); Gate owner;
    ASSERT_TRUE(owner.begin(e.correlation)); EXPECT_FALSE(owner.begin(e.correlation));
    ASSERT_EQ(channel->try_post(e), Admission::Accepted); auto results = wait(1); ASSERT_EQ(results.size(), 1U);
    const auto committed = results[0];
    for (int which = 0; which < 8; ++which) {
        auto stale = committed;
        if (which == 0) ++stale.correlation.operation_id;
        if (which == 1) stale.correlation.task_id = uuid(99);
        if (which == 2) stale.correlation.attempt_id = uuid(99);
        if (which == 3) stale.correlation.session_id = uuid(99);
        if (which == 4) ++stale.correlation.expected_task_version;
        if (which == 5) ++stale.correlation.expected_attempt_version;
        if (which == 6) ++stale.correlation.entity_generation;
        if (which == 7) ++stale.correlation.session_generation;
        EXPECT_FALSE(owner.apply(stale)); EXPECT_EQ(owner.effects, 0U);
    }
    EXPECT_TRUE(owner.apply(committed)); EXPECT_FALSE(owner.apply(committed)); EXPECT_EQ(owner.effects, 1U);
}

class SideEffectGate : public StoreTest, public ::testing::WithParamInterface<int> {};
TEST_P(SideEffectGate, SubmitRunCancelAndTerminalEffectsWaitForCommitCompletion) {
    Command command;
    switch (GetParam()) {
        case 0: command = Submit{spec(1), kAt}; break;
        case 1: submit(1); command = assignment(1); break;
        case 2: running(1); command = CancelIntent{{uuid(1), 1}, kLater}; break;
        case 3: running(1); command = finish(1); break;
        default: throw std::logic_error("invalid gate");
    }
    auto e = envelope(command); Gate owner; ASSERT_TRUE(owner.begin(e.correlation));
    hook->arm(SqlStage::Commit, "COMMIT", SQLITE_OK, 1, true);
    EXPECT_EQ(channel->try_post(e), Admission::Accepted); EXPECT_TRUE(hook->wait_entered());
    auto early = channel->drain(); ASSERT_TRUE(early); EXPECT_TRUE(early.value().empty()); EXPECT_EQ(owner.effects, 0U);
    hook->release(); auto results = wait(1); ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(owner.apply(results[0])); EXPECT_EQ(owner.effects, 1U);
}
INSTANTIATE_TEST_SUITE_P(AllIrreversibleEffects, SideEffectGate, ::testing::Range(0, 4));

TEST_F(StoreTest, ShutdownWithExecutingAndQueuedCommandsDrainsWithoutConsumerDependency) {
    hook->arm(SqlStage::Commit, "COMMIT", SQLITE_OK, 1, true);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(1), kAt}, 512)), Admission::Accepted);
    EXPECT_TRUE(hook->wait_entered());
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(2), kAt}, 512)), Admission::Accepted);
    channel->stop(); EXPECT_EQ(channel->try_post(envelope(Submit{spec(3), kAt}, 512)), Admission::Stopped);
    hook->release(); channel->join(); auto results = wait(2); ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(results[0].outcome.committed); EXPECT_TRUE(results[1].outcome.committed);
    EXPECT_EQ(channel->status().health, Health::Stopped);
}

std::size_t entries(const char* directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) { static_cast<void>(entry); ++count; }
    return count;
}
TEST(StorageStress, RepeatedOpenExecuteRecoverCloseHasNoFdThreadStatementOrLockGrowth) {
    const auto fds = entries("/proc/self/fd");
    const auto threads = entries("/proc/self/task");
    TempDirectory directory;
    for (unsigned int i = 1; i <= 25; ++i) {
        auto created = persistence::Channel::start({directory.path, 50, kLater, nullptr}); ASSERT_TRUE(created);
        auto c = std::move(created).value(); ASSERT_TRUE(startup(*c));
        Command command = Submit{spec(i), kAt}; Envelope e{correlate(command, i), command, 512};
        ASSERT_EQ(c->try_post(e), Admission::Accepted);
        c->stop(); c->join(); auto completions = c->drain(); ASSERT_TRUE(completions);
        EXPECT_EQ(c->status().health, Health::Stopped); EXPECT_FALSE(c->status().error);
        ASSERT_EQ(completions.value().size(), 1U); EXPECT_TRUE(completions.value()[0].outcome.committed);
        c.reset(); Reader db(directory.path);
        EXPECT_EQ(sqlite3_next_stmt(db.db, nullptr), nullptr);
        db.exec("BEGIN IMMEDIATE"); db.exec("ROLLBACK");
    }
    EXPECT_EQ(entries("/proc/self/fd"), fds); EXPECT_EQ(entries("/proc/self/task"), threads);
}

TEST(StorageChannel, InvalidLimitsFailBeforeCreatingWorkerOrDatabase) {
    TempDirectory directory; ChannelLimits limits; limits.count = limits.essential_count;
    EXPECT_FALSE(persistence::Channel::start({directory.path, 50, kAt, nullptr}, limits));
    EXPECT_FALSE(std::filesystem::exists(directory.path + "/metadata.sqlite3"));
}

TEST(StorageChannel, StartupIsNonblockingAndIdleOrStartingShutdownIsDeterministic) {
    for (const bool stop_early : {false, true}) {
        TempDirectory directory; auto hook = std::make_shared<Hook>();
        hook->arm(SqlStage::Prepare, "PRAGMA user_version", SQLITE_OK, 1, true);
        auto created = persistence::Channel::start({directory.path, 50, kAt, hook}); ASSERT_TRUE(created);
        auto c = std::move(created).value(); EXPECT_TRUE(hook->wait_entered());
        Command command = Submit{spec(1), kAt}; Envelope e{correlate(command, 1), command, 512};
        EXPECT_EQ(c->status().health, Health::Starting); EXPECT_EQ(c->try_post(e), Admission::NotReady);
        if (stop_early) c->stop();
        hook->release();
        if (!stop_early) { ASSERT_TRUE(startup(*c)); }
        c->stop(); c->join(); EXPECT_EQ(c->status().health, Health::Stopped);
        EXPECT_TRUE(c->drain().value().empty());
        EXPECT_EQ(c->try_post(e), Admission::Stopped);
    }
}

class FailingNotification final : public CompletionNotification {
public:
    bool notify() noexcept override { return ++calls != 2; }
    std::atomic<int> calls{0};
};
TEST(StorageChannel, NotificationFailureIsExplicitAndNeverDropsCommittedResult) {
    TempDirectory directory; auto notification = std::make_shared<FailingNotification>();
    auto worker = PersistenceThread::start({directory.path, 50, kAt, nullptr}, {}, notification);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (notification->calls == 0 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    ASSERT_EQ(worker->status().health, Health::Ready);
    Command command = Submit{spec(1), kAt};
    EXPECT_EQ(worker->try_post({correlate(command, 1), command, 512}), Admission::Accepted);
    while (notification->calls < 2 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    worker->stop(); worker->join(); ASSERT_TRUE(worker->status().error); EXPECT_TRUE(worker->status().error->fatal);
    const auto completions = worker->drain(); ASSERT_EQ(completions.size(), 1U);
    EXPECT_TRUE(completions[0].outcome.committed);
    EXPECT_EQ(worker->try_post({correlate(command, 2), command, 512}), Admission::Degraded);
}
} // namespace ts::storage::test
