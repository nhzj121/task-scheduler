#include "ts/worker/execution_manager.h"
#include <chrono>
#include <thread>
#include <gtest/gtest.h>

namespace ts::worker {
namespace {
class LifecycleExecution : public ::testing::Test {
protected:
    domain::WorkerConfig config = domain::WorkerConfig::parse({{"worker_id", "observer-worker"},
        {"controller", {{"address", "127.0.0.1"}, {"port", 7421}}},
        {"capacity", {{"cpu_slots", 2}, {"memory_mb", 100}}}}).value();
    domain::SessionId session{domain::SessionId::generate()};
    std::unique_ptr<ExecutionManager> manager;
    std::vector<protocol::Message> messages;
    std::vector<ExecutionLifecycleEvent> events;
    bool throwing{false};
    void SetUp() override {
        auto made = ExecutionManager::create(config, session, [this](net::Outbound bytes) {
            protocol::FrameParser parser(protocol::Direction::WorkerToController);
            const auto fed = parser.feed(bytes.bytes, [this](protocol::Frame&& f) {
                auto decoded = protocol::decode_message(f, protocol::Direction::WorkerToController);
                if (!decoded) return false;
                messages.push_back(std::move(decoded).value()); return true;
            });
            return fed ? common::Result<void>::success() : common::Result<void>::failure(fed.error().message);
        }, [] { return std::optional<net::ConnectionSnapshot>{net::ConnectionSnapshot{}}; },
        [this](const auto& e) {
            events.push_back(e);
            if (throwing) throw std::runtime_error("diagnostic failure");
        });
        ASSERT_TRUE(made); manager = std::move(made).value();
        ASSERT_TRUE(manager->activate({true, 50, 1000, 20, 65536, 64, domain::SessionId::generate()}));
    }
    protocol::RunTask run(const std::string& exe, std::vector<std::string> args = {}) {
        return {config.worker_id, session, domain::TaskId::generate(), domain::AttemptId::generate(),
            domain::AttemptNumber::create(1).value(), exe, std::move(args), {}, {},
            domain::ResourceRequest::create(1, 0).value(), {}};
    }
    void pump(bool until_started = false) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!manager->running().empty() && std::chrono::steady_clock::now() < end) {
            ASSERT_TRUE(manager->tick());
            if (until_started && !events.empty()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!until_started) { EXPECT_TRUE(manager->running().empty()); }
    }
    void TearDown() override {
        if (!manager) return;
        EXPECT_TRUE(manager->stop()); pump();
    }
};
TEST_F(LifecycleExecution, NaturalCompletionAndWireOutcomeUseTheSameFacts) {
    auto task = run("/bin/true");
    ASSERT_TRUE(manager->accept(task, 1)); EXPECT_TRUE(events.empty()); pump();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].kind, ExecutionLifecycleKind::Started);
    ASSERT_TRUE(events[0].pid); EXPECT_GT(*events[0].pid, 0);
    EXPECT_EQ(events[0].task_id, task.task_id.value());
    EXPECT_EQ(events[0].session_id, session.value());
    EXPECT_EQ(events[0].executable, "/bin/true");
    const auto& finished = std::get<protocol::TaskFinished>(messages.back());
    EXPECT_EQ(events[1].outcome, finished.outcome);
    EXPECT_EQ(events[1].exit_code, finished.exit_code);
    EXPECT_EQ(events[1].pid, events[0].pid);
    EXPECT_EQ(events[1].exit_code, 0);
}
TEST_F(LifecycleExecution, ParentPreparationFailureHasNoStartedOrInventedPid) {
    auto task = run("/bin/true");
    task.cwd = "/missing-observer-directory";
    ASSERT_TRUE(manager->accept(task, 1)); pump();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, ExecutionLifecycleKind::Finished);
    EXPECT_EQ(events[0].outcome, protocol::Outcome::LaunchFailed);
    EXPECT_FALSE(events[0].pid);
    EXPECT_EQ(std::get<protocol::TaskFinished>(messages.back()).outcome, *events[0].outcome);
}
TEST_F(LifecycleExecution, ChildExecFailureHasKnownPidButNeverAnnouncesStarted) {
    ASSERT_TRUE(manager->accept(run("/missing-observer-executable"), 1)); pump();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, ExecutionLifecycleKind::Finished);
    EXPECT_EQ(events[0].outcome, protocol::Outcome::LaunchFailed);
    ASSERT_TRUE(events[0].pid); EXPECT_GT(*events[0].pid, 0);
}
TEST_F(LifecycleExecution, TimeoutAndCancellationKeepExistingWireCause) {
    for (const bool timeout : {true, false}) {
        events.clear(); messages.clear();
        auto task = run("/bin/sleep", {"30"});
        if (timeout) task.timeout_ms = 40;
        ASSERT_TRUE(manager->accept(task, timeout ? 1 : 2)); pump(true);
        if (!timeout) { ASSERT_TRUE(manager->cancel({task.worker_id, task.session_id, task.task_id, task.attempt_id, 20}, 3)); }
        pump();
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events.back().outcome, timeout ? protocol::Outcome::TimedOut : protocol::Outcome::Cancelled);
        const auto& done = std::get<protocol::TaskFinished>(messages.back());
        EXPECT_EQ(events.back().outcome, done.outcome);
        EXPECT_EQ(events.back().term_signal, done.term_signal);
    }
}
TEST_F(LifecycleExecution, SessionCleanupObservesLocalCompletionWithoutSendingFinish) {
    auto task = run("/bin/sleep", {"30"});
    ASSERT_TRUE(manager->accept(task, 1)); pump(true);
    ASSERT_TRUE(manager->stop()); pump();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events.back().outcome, protocol::Outcome::Cancelled);
    EXPECT_EQ(events.back().session_id, task.session_id.value());
    for (const auto& message : messages) EXPECT_FALSE(std::holds_alternative<protocol::TaskFinished>(message));
}
TEST_F(LifecycleExecution, ThrowingObserverDoesNotLoseOwnerOrSuppressMessages) {
    throwing = true;
    ASSERT_TRUE(manager->accept(run("/bin/true"), 1)); pump();
    ASSERT_EQ(events.size(), 2U);
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<protocol::RunTaskAck>(messages[0]));
    EXPECT_TRUE(std::holds_alternative<protocol::TaskStarted>(messages[1]));
    EXPECT_EQ(std::get<protocol::TaskFinished>(messages[2]).exit_code, 0);
    EXPECT_TRUE(manager->running().empty());
}
} // namespace
} // namespace ts::worker
