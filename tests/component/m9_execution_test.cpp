#include "ts/worker/execution_manager.h"
#include "ts/runtime/process_runtime.h"
#include <chrono>
#include <csignal>
#include <gtest/gtest.h>
namespace ts::worker {
namespace {
using Clock = std::chrono::steady_clock;
struct M9Execution : ::testing::Test {
    domain::WorkerConfig config = domain::WorkerConfig::parse({{"worker_id", "m9-worker"},
        {"controller", {{"address", "127.0.0.1"}, {"port", 7421}}},
        {"capacity", {{"cpu_slots", 2}, {"memory_mb", 100}}}}).value();
    domain::SessionId session{domain::SessionId::generate()};
    std::unique_ptr<ExecutionManager> manager;
    std::vector<protocol::Message> messages;
    void SetUp() override {
        auto created = ExecutionManager::create(config, session, [this](net::Outbound bytes) {
            protocol::FrameParser parser(protocol::Direction::WorkerToController);
            const auto fed = parser.feed(bytes.bytes, [this](protocol::Frame&& frame) {
                auto value = protocol::decode_message(frame, protocol::Direction::WorkerToController);
                if (!value) return false;
                messages.push_back(std::move(value).value()); return true;
            });
            return fed ? common::Result<void>::success() : common::Result<void>::failure(fed.error().message);
        }, [] { return std::optional<net::ConnectionSnapshot>{net::ConnectionSnapshot{}}; });
        ASSERT_TRUE(created); manager = std::move(created).value();
        ASSERT_TRUE(manager->activate({true, 50, 1000, 30, 65536, 64, domain::SessionId::generate()}));
    }
    void TearDown() override {
        if (!manager) return;
        EXPECT_TRUE(manager->stop());
        const auto until = Clock::now() + std::chrono::seconds(4);
        while (!manager->running().empty() && Clock::now() < until) EXPECT_TRUE(manager->tick());
        EXPECT_TRUE(manager->running().empty());
    }
    protocol::RunTask run(const std::string& executable, std::vector<std::string> args = {}) {
        return {config.worker_id, session, domain::TaskId::generate(), domain::AttemptId::generate(),
            domain::AttemptNumber::create(1).value(), executable, std::move(args), {}, {},
            domain::ResourceRequest::create(1, 0).value(), {}};
    }
    protocol::WorkerCancelTask cancel(const protocol::RunTask& task) {
        return {task.worker_id, task.session_id, task.task_id, task.attempt_id, 25};
    }
    void finish() {
        const auto until = Clock::now() + std::chrono::seconds(4);
        while (!manager->running().empty() && Clock::now() < until) ASSERT_TRUE(manager->tick());
        ASSERT_TRUE(manager->running().empty());
    }
};
TEST_F(M9Execution, CancelBeforeExecConfirmationWinsLaunchFailureWithoutStarted) {
    const auto task = run("/missing-m9-executable");
    ASSERT_TRUE(manager->accept(task, 1));
    ASSERT_TRUE(manager->cancel(cancel(task), 2)); // no runtime poll between fork and cancel
    finish();
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_TRUE(std::get<protocol::WorkerCancelTaskAck>(messages[1]).accepted);
    EXPECT_FALSE(std::get<protocol::WorkerCancelTaskAck>(messages[1]).already_terminal);
    EXPECT_EQ(std::get<protocol::TaskFinished>(messages[2]).outcome, protocol::Outcome::Cancelled);
}
TEST_F(M9Execution, CompletedLaunchFailureAndNaturalResultCannotBeOverwritten) {
    for (const auto* executable : {"/missing-m9-executable", "/bin/true"}) {
        const auto task = run(executable);
        ASSERT_TRUE(manager->accept(task, 1)); finish();
        const auto original = std::get<protocol::TaskFinished>(messages.back()).outcome;
        ASSERT_TRUE(manager->cancel(cancel(task), 2));
        const auto ack = std::get<protocol::WorkerCancelTaskAck>(messages.back());
        EXPECT_TRUE(ack.accepted); EXPECT_TRUE(ack.already_terminal);
        EXPECT_EQ(original, std::string(executable) == "/bin/true" ? protocol::Outcome::Exited : protocol::Outcome::LaunchFailed);
    }
}
TEST_F(M9Execution, WrongTaskSessionAndUnknownAttemptCannotSignalOwnedProcess) {
    const auto task = run("/bin/sleep", {"60"});
    ASSERT_TRUE(manager->accept(task, 1));
    auto command = cancel(task); command.task_id = domain::TaskId::generate();
    EXPECT_FALSE(manager->cancel(command, 2));
    command = cancel(task); command.session_id = domain::SessionId::generate();
    EXPECT_FALSE(manager->cancel(command, 3));
    command = cancel(task); command.attempt_id = domain::AttemptId::generate();
    ASSERT_TRUE(manager->cancel(command, 4));
    EXPECT_FALSE(std::get<protocol::WorkerCancelTaskAck>(messages.back()).accepted);
    ASSERT_EQ(manager->running().size(), 1U);
    ASSERT_TRUE(manager->cancel(cancel(task), 5)); finish();
    EXPECT_EQ(std::get<protocol::TaskFinished>(messages.back()).outcome, protocol::Outcome::Cancelled);
}
TEST_F(M9Execution, ResetRequiresCompleteCleanupAndDiscardsAllSessionTombstones) {
    const auto task = run("/bin/sleep", {"60"});
    ASSERT_TRUE(manager->accept(task, 1));
    const auto next = domain::SessionId::generate();
    EXPECT_FALSE(manager->reset_session(next));
    ASSERT_TRUE(manager->stop());
    EXPECT_FALSE(manager->reset_session(next));
    finish();
    ASSERT_TRUE(manager->reset_session(next));
    EXPECT_FALSE(manager->accept(task, 2)); // not activated, old session unusable
    ASSERT_TRUE(manager->activate({true, 50, 1000, 30, 65536, 64, domain::SessionId::generate()}));
    EXPECT_FALSE(manager->accept(task, 2));
    auto fresh = task; fresh.session_id = next;
    ASSERT_TRUE(manager->accept(fresh, 3)); // old seen-set and resource charge gone
    ASSERT_TRUE(std::get<protocol::RunTaskAck>(messages.back()).accepted);
    ASSERT_TRUE(manager->stop()); finish();
}
TEST_F(M9Execution, DuplicateCancelDoesNotReplaceCommandGraceOrRearmTermination) {
    const auto task = run(TS_RUNTIME_HELPER_PATH, {"ignore-term"});
    ASSERT_TRUE(manager->accept(task, 1));
    const auto until = Clock::now() + std::chrono::seconds(3);
    auto ready = [&] {
        for (const auto& message : messages) if (std::holds_alternative<protocol::LogChunk>(message)) return true;
        return false;
    };
    while (!ready() && Clock::now() < until) ASSERT_TRUE(manager->tick());
    ASSERT_TRUE(ready());
    auto command = cancel(task); command.grace_ms = 40;
    const auto start = Clock::now(); ASSERT_TRUE(manager->cancel(command, 2));
    while (Clock::now() - start < std::chrono::milliseconds(20)) ASSERT_TRUE(manager->tick());
    command.grace_ms = 2000; ASSERT_TRUE(manager->cancel(command, 3));
    finish();
    const auto elapsed = Clock::now() - start;
    EXPECT_GE(elapsed, std::chrono::milliseconds(30));
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
    const auto final = std::get<protocol::TaskFinished>(messages.back());
    EXPECT_EQ(final.outcome, protocol::Outcome::Cancelled); EXPECT_EQ(final.term_signal, SIGKILL);
}
} // namespace
} // namespace ts::worker
