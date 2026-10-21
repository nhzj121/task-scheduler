#include "ts/runtime/process_runtime.h"
#include "ts/controller/worker_registry.h"
#include "ts/controller/task_manager.h"

#include <chrono>
#include <gtest/gtest.h>

namespace {
using namespace ts;
using Clock = std::chrono::steady_clock;
struct RuntimeGuard {
    std::unique_ptr<runtime::ProcessRuntime> runtime;
    runtime::RuntimeProcessId id{0};
    bool cleanup{false};
    ~RuntimeGuard() {
        if (!runtime || !runtime->running_count()) return;
        cleanup = true; static_cast<void>(runtime->cancel(id));
        if (const auto snapshot = runtime->observe(id)) {
            if (!snapshot->stdout_complete) static_cast<void>(runtime->set_output_enabled(id, runtime::OutputStream::STDOUT, true));
            if (!snapshot->stderr_complete) static_cast<void>(runtime->set_output_enabled(id, runtime::OutputStream::STDERR, true));
        }
        const auto deadline = Clock::now() + std::chrono::seconds(4);
        while (runtime->running_count() && Clock::now() < deadline) static_cast<void>(runtime->poll_once(10));
    }
};
TEST(M7Runtime, RemainingAllowanceBoundsActualReadAndZeroPausesUntilExplicitResume) {
    RuntimeGuard guard; std::size_t credit = 3; std::string output;
    auto created = runtime::ProcessRuntime::create(
        [&](auto, auto stream, const std::uint8_t* data, std::size_t size) {
            if (guard.cleanup) return common::Result<void>::success();
            EXPECT_LE(size, credit); credit -= size;
            if (stream == runtime::OutputStream::STDOUT) output.append(reinterpret_cast<const char*>(data), size);
            return common::Result<void>::success();
        }, [&](auto, auto) { return guard.cleanup ? std::size_t{65536} : credit; });
    ASSERT_TRUE(created); guard.runtime = std::move(created).value();
    runtime::LaunchOptions options; options.execution.executable = "/bin/echo"; options.execution.arguments = {"abcdef"};
    auto launched = guard.runtime->launch(options); ASSERT_TRUE(launched); guard.id = launched.value();
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (output.size() < 3 && Clock::now() < deadline) ASSERT_TRUE(guard.runtime->poll_once(10));
    ASSERT_EQ(output, "abc");
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(guard.runtime->poll_once(0));
    EXPECT_EQ(output, "abc"); ASSERT_TRUE(guard.runtime->observe(guard.id));
    EXPECT_GT(guard.runtime->observe(guard.id)->leader_pid, 0);
    credit = 1024;
    ASSERT_TRUE(guard.runtime->set_output_enabled(guard.id, runtime::OutputStream::STDOUT, true));
    const auto observation = guard.runtime->observe(guard.id);
    if (!observation->stderr_complete) { ASSERT_TRUE(guard.runtime->set_output_enabled(guard.id, runtime::OutputStream::STDERR, true)); }
    while (guard.runtime->running_count() && Clock::now() < deadline) ASSERT_TRUE(guard.runtime->poll_once(10));
    EXPECT_EQ(output, "abcdef\n"); EXPECT_EQ(guard.runtime->running_count(), 0U);
    const auto completions = guard.runtime->take_completions(); ASSERT_EQ(completions.size(), 1U);
    EXPECT_TRUE(completions[0].stdout_complete); EXPECT_TRUE(completions[0].stderr_complete);
    EXPECT_FALSE(guard.runtime->observe(guard.id));
}
TEST(M7Runtime, ExecObservationReportsRealPidOrFailureBeforePipeDrain) {
    for (const auto* executable : {"/bin/true", "/this-m7-executable-does-not-exist"}) {
        RuntimeGuard guard; bool enabled = false;
        auto created = runtime::ProcessRuntime::create(
            [](auto, auto, const auto*, auto) { return common::Result<void>::success(); },
            [&](auto, auto) { return enabled || guard.cleanup ? std::size_t{16384} : 0U; });
        ASSERT_TRUE(created); guard.runtime = std::move(created).value();
        runtime::LaunchOptions options; options.execution.executable = executable;
        const auto launched = guard.runtime->launch(options); ASSERT_TRUE(launched); guard.id = launched.value();
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!guard.runtime->observe(guard.id)->exec_resolved && Clock::now() < deadline) ASSERT_TRUE(guard.runtime->poll_once(10));
        const auto observed = guard.runtime->observe(guard.id); ASSERT_TRUE(observed); EXPECT_GT(observed->leader_pid, 0);
        const bool success = std::string(executable) == "/bin/true";
        EXPECT_EQ(observed->exec_succeeded, success); EXPECT_EQ(observed->launch_failure.has_value(), !success);
        EXPECT_EQ(guard.runtime->take_started().size(), success ? 1U : 0U);
        enabled = true;
        ASSERT_TRUE(guard.runtime->set_output_enabled(guard.id, runtime::OutputStream::STDOUT, true));
        ASSERT_TRUE(guard.runtime->set_output_enabled(guard.id, runtime::OutputStream::STDERR, true));
        while (guard.runtime->running_count() && Clock::now() < deadline) ASSERT_TRUE(guard.runtime->poll_once(10));
        const auto completed = guard.runtime->take_completions(); ASSERT_EQ(completed.size(), 1U);
        EXPECT_EQ(completed[0].leader_pid, observed->leader_pid); EXPECT_EQ(completed[0].exec_succeeded, success);
    }
}
TEST(M7Registry, OfflineAdvertisementSurvivesWithoutAStoredOrCurrentLease) {
    controller::WorkerRegistry registry;
    controller::Registration first{domain::WorkerId::parse("worker-a").value(), domain::SessionId::generate(),
        {7, 1}, domain::ResourceCapacity::create(4, 100).value(), 2};
    const auto old = *registry.register_worker(first, 0).value().accepted;
    auto next = first; next.session_id = domain::SessionId::generate(); next.connection.generation = 2;
    next.capacity = domain::ResourceCapacity::create(8, 200).value(); next.maximum.reset();
    ASSERT_TRUE(registry.register_worker(next, 0)); EXPECT_EQ(registry.inspect(first.worker_id)->capacity->cpu_slots(), 4U);
    const auto current = *registry.finish_invalidation(old, 0).value();
    ASSERT_TRUE(registry.invalidate(current, true)); ASSERT_TRUE(registry.finish_invalidation(current, 1));
    const auto records = registry.records(); ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].first, first.worker_id); EXPECT_EQ(records[0].second.state, domain::WorkerState::OFFLINE);
    EXPECT_FALSE(records[0].second.current); ASSERT_TRUE(records[0].second.capacity);
    EXPECT_EQ(records[0].second.capacity->cpu_slots(), 8U); EXPECT_FALSE(records[0].second.maximum);
}
TEST(M7Registry, ClosedPendingReplacementCannotBecomeHealthyAfterOldLossCommits) {
    struct Persistence final : controller::PersistencePort {
        std::vector<storage::Envelope> queued;
        storage::Admission try_post(storage::Envelope command) override {
            queued.push_back(std::move(command)); return storage::Admission::Accepted;
        }
    } persistence;
    struct Effects final : controller::EffectPort {
        std::vector<controller::Effect> values;
        controller::EffectAdmission emit(controller::Effect effect) override {
            values.push_back(std::move(effect)); return controller::EffectAdmission::Accepted;
        }
    } effects;
    controller::TaskManager manager(persistence, effects);
    controller::Registration first{domain::WorkerId::parse("worker-a").value(), domain::SessionId::generate(),
        {7, 1}, domain::ResourceCapacity::create(1, 100).value(), 1};
    ASSERT_EQ(manager.register_worker(first), controller::Decision::Accepted);
    auto replacement = first; replacement.session_id = domain::SessionId::generate(); replacement.connection.generation = 2;
    ASSERT_EQ(manager.register_worker(replacement), controller::Decision::Accepted);
    ASSERT_EQ(persistence.queued.size(), 1U);
    EXPECT_EQ(manager.connection_closed({7, 3}), controller::Decision::Stale); // fd identity alone is insufficient
    EXPECT_EQ(manager.connection_closed(replacement.connection), controller::Decision::Accepted);
    storage::Outcome outcome; outcome.committed = true; outcome.affected = 0;
    ASSERT_EQ(manager.complete({persistence.queued[0].correlation, outcome}), controller::Decision::Accepted);
    const auto record = manager.workers().inspect(first.worker_id);
    ASSERT_TRUE(record); EXPECT_FALSE(record->current); EXPECT_FALSE(record->replacement);
    EXPECT_EQ(record->state, domain::WorkerState::OFFLINE);
    EXPECT_EQ(manager.connection_closed(first.connection), controller::Decision::Stale);
    std::size_t accepted = 0;
    for (const auto& e : effects.values) if (e.kind == controller::EffectKind::RegistrationAccepted) ++accepted;
    EXPECT_EQ(accepted, 1U);
    // Discarding a disconnected replacement does not make its UUID reusable.
    EXPECT_EQ(manager.register_worker(replacement), controller::Decision::Rejected);
}
} // namespace
