#include "ts/runtime/process_runtime.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace ts::runtime {

class ProcessRuntimeTestPeer {
public:
    static void fail_next_registrations(ProcessRuntime& runtime, std::uint32_t mask) {
        runtime.fail_next_registrations_for_testing(mask);
    }
    static int output_fd(ProcessRuntime& runtime, RuntimeProcessId id, OutputStream stream) {
        return runtime.output_fd_for_testing(id, stream);
    }
};

namespace {

using namespace std::chrono_literals;

struct CapturedOutput {
    std::vector<std::uint8_t> stdout_bytes;
    std::vector<std::uint8_t> stderr_bytes;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/task-scheduler-runtime-XXXXXX";
        path_ = ::mkdtemp(pattern.data());
        if (path_.empty()) {
            throw std::runtime_error("mkdtemp failed");
        }
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

private:
    std::string path_;
};

class RuntimeHarness final {
public:
    RuntimeHarness() {
        auto created = ProcessRuntime::create(
            [this](RuntimeProcessId id, OutputStream stream,
                   const std::uint8_t* bytes, std::size_t size) {
                auto& destination = stream == OutputStream::STDOUT
                                        ? output_[id].stdout_bytes
                                        : output_[id].stderr_bytes;
                if (capture_output) {
                    destination.insert(destination.end(), bytes, bytes + size);
                }
                if (sink_hook) {
                    sink_hook(id, stream, size);
                }
                return common::Result<void>::success();
            });
        if (!created) {
            throw std::runtime_error(created.error().message);
        }
        runtime_ = std::move(created).value();
    }

    ~RuntimeHarness() {
        if (runtime_ == nullptr || runtime_->running_count() == 0U) {
            return;
        }
        for (RuntimeProcessId id : launched_) {
            static_cast<void>(runtime_->cancel(id));
        }
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (runtime_->running_count() != 0U &&
               std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(runtime_->poll_once(20));
            collect();
        }
    }

    RuntimeProcessId launch(std::vector<std::string> arguments,
                            std::optional<std::chrono::nanoseconds> timeout = std::nullopt,
                            std::chrono::nanoseconds grace = 30ms,
                            std::string executable = TS_RUNTIME_HELPER_PATH,
                            std::vector<std::pair<std::string, std::string>> environment = {},
                            std::optional<std::string> cwd = std::nullopt) {
        LaunchOptions options;
        options.execution.executable = std::move(executable);
        options.execution.arguments = std::move(arguments);
        options.execution.environment_overrides = std::move(environment);
        options.execution.cwd = std::move(cwd);
        options.timeout = timeout;
        options.termination_grace = grace;
        auto result = runtime_->launch(options);
        if (!result) {
            throw std::runtime_error(result.error().message);
        }
        launched_.push_back(result.value());
        return result.value();
    }

    void collect() {
        auto started = runtime_->take_started();
        started_.insert(started_.end(), started.begin(), started.end());
        auto completions = runtime_->take_completions();
        for (auto& completion : completions) {
            completion_by_id_.emplace(completion.process_id, std::move(completion));
        }
    }

    bool poll_until(const std::function<bool()>& predicate,
                    std::chrono::milliseconds limit = 4s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            const auto polled = runtime_->poll_once(50);
            if (!polled) {
                failure_ = polled.error().message;
                return false;
            }
            collect();
        }
        return predicate();
    }

    bool wait_completed(RuntimeProcessId id,
                        std::chrono::milliseconds limit = 4s) {
        return poll_until([this, id] { return completion_by_id_.count(id) != 0U; },
                          limit);
    }

    bool wait_started(RuntimeProcessId id) {
        return poll_until([this, id] {
            return std::find(started_.begin(), started_.end(), id) != started_.end();
        });
    }

    ProcessRuntime& runtime() {
        return *runtime_;
    }

    const ProcessCompletion& completion(RuntimeProcessId id) const {
        return completion_by_id_.at(id);
    }

    const CapturedOutput& output(RuntimeProcessId id) const {
        return output_.at(id);
    }

    std::string stdout_text(RuntimeProcessId id) const {
        const auto iterator = output_.find(id);
        return iterator == output_.end()
                   ? std::string()
                   : std::string(iterator->second.stdout_bytes.begin(),
                                 iterator->second.stdout_bytes.end());
    }

    const std::string& failure() const noexcept {
        return failure_;
    }

    std::function<void(RuntimeProcessId, OutputStream, std::size_t)> sink_hook;
    bool capture_output{true};

    bool completed(RuntimeProcessId id) const {
        return completion_by_id_.count(id) != 0U;
    }

private:
    std::unique_ptr<ProcessRuntime> runtime_;
    std::vector<RuntimeProcessId> launched_;
    std::vector<RuntimeProcessId> started_;
    std::unordered_map<RuntimeProcessId, ProcessCompletion> completion_by_id_;
    std::unordered_map<RuntimeProcessId, CapturedOutput> output_;
    std::string failure_;
};

std::string bytes_to_string(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::size_t count_open_fds() {
    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        static_cast<void>(entry);
        ++count;
    }
    return count;
}

TEST(ProcessRuntime, DistinguishesExecSuccessExit127NonzeroAndSignalOutcomes) {
    RuntimeHarness harness;
    const RuntimeProcessId zero = harness.launch({"exit", "0"});
    const RuntimeProcessId natural_before_timer = harness.launch({"exit", "0"}, 500ms);
    const RuntimeProcessId one_twenty_seven = harness.launch({"exit", "127"});
    const RuntimeProcessId nonzero = harness.launch({"exit", "23"});
    const RuntimeProcessId signaled =
        harness.launch({"signal", std::to_string(SIGUSR1)});

    ASSERT_TRUE(harness.poll_until([&harness] {
        return harness.runtime().running_count() == 0U;
    })) << harness.failure();
    for (RuntimeProcessId id :
         {zero, natural_before_timer, one_twenty_seven, nonzero, signaled}) {
        EXPECT_TRUE(harness.completion(id).exec_succeeded);
        EXPECT_EQ(harness.completion(id).termination_cause, TerminationCause::NONE);
    }
    EXPECT_EQ(harness.completion(zero).leader_outcome->exit_code, 0);
    EXPECT_EQ(harness.completion(natural_before_timer).leader_outcome->exit_code, 0);
    EXPECT_EQ(harness.completion(natural_before_timer).termination_cause,
              TerminationCause::NONE);
    EXPECT_EQ(harness.completion(one_twenty_seven).leader_outcome->exit_code, 127);
    EXPECT_EQ(harness.completion(nonzero).leader_outcome->exit_code, 23);
    EXPECT_EQ(harness.completion(signaled).leader_outcome->term_signal, SIGUSR1);
    EXPECT_EQ(harness.runtime().registration_count(), 1U);
}

TEST(ProcessRuntime, ReportsExecFailureWithoutConfusingItWithExit127) {
    RuntimeHarness harness;
    const RuntimeProcessId id =
        harness.launch({}, std::nullopt, 20ms, "/definitely/missing/runtime-helper");
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    const ProcessCompletion& completion = harness.completion(id);
    EXPECT_FALSE(completion.exec_succeeded);
    ASSERT_TRUE(completion.launch_failure.has_value());
    EXPECT_EQ(completion.launch_failure->stage, ChildFailureStage::EXECVE);
    EXPECT_EQ(completion.launch_failure->error_number, ENOENT);
    ASSERT_TRUE(completion.leader_outcome.has_value());
    EXPECT_EQ(completion.leader_outcome->exit_code, 127);
}

TEST(ProcessRuntime, CapturesSeparateBinaryStreamsLargeOutputTailAndDevNullStdin) {
    RuntimeHarness harness;
    const RuntimeProcessId binary = harness.launch({"emit"});
    const RuntimeProcessId large = harness.launch({"large", "524288"});
    const RuntimeProcessId stdin_check = harness.launch({"stdin-eof"});
    const RuntimeProcessId interleaved = harness.launch({"interleave"});
    ASSERT_TRUE(harness.poll_until([&harness] {
        return harness.runtime().running_count() == 0U;
    }, 8s)) << harness.failure();

    const std::vector<std::uint8_t> expected_stdout{'o', 'u', 't', 0U, 0xffU, '\n'};
    const std::vector<std::uint8_t> expected_stderr{'e', 'r', 'r', 0U, 0xfeU, '\n'};
    EXPECT_EQ(harness.output(binary).stdout_bytes, expected_stdout);
    EXPECT_EQ(harness.output(binary).stderr_bytes, expected_stderr);
    EXPECT_EQ(harness.output(large).stdout_bytes.size(), 524288U);
    EXPECT_EQ(harness.output(large).stderr_bytes.size(), 524288U);
    EXPECT_EQ(harness.output(large).stdout_bytes, harness.output(large).stderr_bytes);
    EXPECT_EQ(bytes_to_string(harness.output(stdin_check).stdout_bytes), "stdin-eof\n");
    EXPECT_TRUE(harness.completion(large).stdout_complete);
    EXPECT_TRUE(harness.completion(large).stderr_complete);
    EXPECT_EQ(harness.output(interleaved).stdout_bytes.size(), 32U);
    EXPECT_EQ(harness.output(interleaved).stderr_bytes.size(), 32U);
    EXPECT_NE(harness.output(interleaved).stdout_bytes,
              harness.output(interleaved).stderr_bytes);
}

TEST(ProcessRuntime, AppliesPreparedPathCwdEnvironmentAndExactArgv) {
    const std::filesystem::path helper(TS_RUNTIME_HELPER_PATH);
    const std::filesystem::path helper_directory = helper.parent_path();
    RuntimeHarness harness;
    const RuntimeProcessId id = harness.launch(
        {"describe", "payload"}, std::nullopt, 30ms, helper.filename().string(),
        {{"PATH", helper_directory.string()}, {"TS_RUNTIME_TEST_VALUE", "override"}},
        helper_directory.string());
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    const std::string output = bytes_to_string(harness.output(id).stdout_bytes);
    EXPECT_NE(output.find("cwd=" + helper_directory.string() + "\n"), std::string::npos);
    EXPECT_NE(output.find("env=override\n"), std::string::npos);
    EXPECT_NE(output.find("arg0=" + helper.filename().string() + "\n"),
              std::string::npos);
    EXPECT_NE(output.find("arg1=describe\n"), std::string::npos);
    EXPECT_NE(output.find("arg2=payload\n"), std::string::npos);
}

TEST(ProcessRuntime, RelativeSlashExecutableIsResolvedAfterPreparedCwd) {
    TemporaryDirectory directory;
    const std::filesystem::path link =
        std::filesystem::path(directory.path()) / "local-helper";
    ASSERT_NO_THROW(std::filesystem::create_symlink(TS_RUNTIME_HELPER_PATH, link));
    RuntimeHarness harness;
    const RuntimeProcessId id = harness.launch(
        {"describe"}, std::nullopt, 30ms, "./local-helper", {}, directory.path());
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    EXPECT_TRUE(harness.completion(id).exec_succeeded);
    const std::string output = harness.stdout_text(id);
    EXPECT_NE(output.find("cwd=" + directory.path() + "\n"), std::string::npos);
    EXPECT_NE(output.find("arg0=./local-helper\n"), std::string::npos);
}

TEST(ProcessRuntime, CooperativeAndUncooperativeCancellationPreserveFirstCause) {
    RuntimeHarness harness;
    const RuntimeProcessId cooperative = harness.launch({"cooperative"}, 500ms, 30ms);
    ASSERT_TRUE(harness.wait_started(cooperative)) << harness.failure();
    ASSERT_TRUE(harness.poll_until([&harness, cooperative] {
        return harness.stdout_text(cooperative).find("ready\n") != std::string::npos;
    })) << harness.failure();
    ASSERT_TRUE(harness.runtime().cancel(cooperative));
    EXPECT_FALSE(harness.runtime().cancel(cooperative).value());
    ASSERT_TRUE(harness.wait_completed(cooperative)) << harness.failure();
    EXPECT_EQ(harness.completion(cooperative).termination_cause,
              TerminationCause::CANCELLED);

    const RuntimeProcessId uncooperative = harness.launch({"ignore-term"}, 500ms, 20ms);
    ASSERT_TRUE(harness.wait_started(uncooperative)) << harness.failure();
    ASSERT_TRUE(harness.poll_until([&harness, uncooperative] {
        return harness.stdout_text(uncooperative).find("ready\n") != std::string::npos;
    })) << harness.failure();
    ASSERT_TRUE(harness.runtime().cancel(uncooperative));
    ASSERT_TRUE(harness.wait_completed(uncooperative)) << harness.failure();
    EXPECT_EQ(harness.completion(uncooperative).termination_cause,
              TerminationCause::CANCELLED);
    EXPECT_EQ(harness.completion(uncooperative).leader_outcome->term_signal, SIGKILL);
}

TEST(ProcessRuntime, TimeoutStartsAfterExecSuccessAndEscalatesToSigkill) {
    RuntimeHarness harness;
    const RuntimeProcessId id = harness.launch({"ignore-term"}, 30ms, 20ms);
    ASSERT_TRUE(harness.wait_started(id)) << harness.failure();
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    EXPECT_EQ(harness.completion(id).termination_cause, TerminationCause::TIMEOUT);
    EXPECT_EQ(harness.completion(id).leader_outcome->term_signal, SIGKILL);
}

TEST(ProcessRuntime, CooperativeTimeoutRetainsTimeoutCauseWithoutSigkill) {
    RuntimeHarness harness;
    const RuntimeProcessId id = harness.launch({"cooperative"}, 30ms, 100ms);
    ASSERT_TRUE(harness.poll_until([&harness, id] {
        return harness.stdout_text(id).find("ready\n") != std::string::npos;
    })) << harness.failure();
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    EXPECT_EQ(harness.completion(id).termination_cause, TerminationCause::TIMEOUT);
    EXPECT_EQ(harness.completion(id).leader_outcome->exit_code, 0);
}

TEST(ProcessRuntime, CancellationSignalsAndCleansEntireSamePgidTree) {
    RuntimeHarness harness;
    const RuntimeProcessId id = harness.launch({"tree-ignore"}, 1s, 20ms);
    ASSERT_TRUE(harness.poll_until([&harness, id] {
        return harness.stdout_text(id).find("tree-ready\n") != std::string::npos;
    })) << harness.failure();
    ASSERT_TRUE(harness.runtime().cancel(id));
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    const ProcessCompletion& completion = harness.completion(id);
    EXPECT_EQ(completion.termination_cause, TerminationCause::CANCELLED);
    EXPECT_EQ(completion.leader_outcome->term_signal, SIGKILL);
    EXPECT_EQ(::kill(-completion.pgid, 0), -1);
    EXPECT_EQ(errno, ESRCH);
}

TEST(ProcessRuntime, NaturalLeaderOutcomeSurvivesResidualGroupCleanupAndTailDrain) {
    RuntimeHarness harness;
    const RuntimeProcessId success = harness.launch({"residual", "0"}, std::nullopt, 20ms);
    const RuntimeProcessId failure = harness.launch({"residual", "17"}, std::nullopt, 20ms);
    const RuntimeProcessId signaled =
        harness.launch({"residual-signal"}, std::nullopt, 20ms);
    ASSERT_TRUE(harness.wait_completed(success)) << harness.failure();
    ASSERT_TRUE(harness.wait_completed(failure)) << harness.failure();
    ASSERT_TRUE(harness.wait_completed(signaled)) << harness.failure();
    EXPECT_EQ(harness.completion(success).termination_cause, TerminationCause::NONE);
    EXPECT_EQ(harness.completion(success).leader_outcome->exit_code, 0);
    EXPECT_EQ(harness.completion(failure).termination_cause, TerminationCause::NONE);
    EXPECT_EQ(harness.completion(failure).leader_outcome->exit_code, 17);
    EXPECT_EQ(harness.completion(signaled).termination_cause, TerminationCause::NONE);
    EXPECT_EQ(harness.completion(signaled).leader_outcome->term_signal, SIGUSR2);
    EXPECT_EQ(bytes_to_string(harness.output(success).stdout_bytes), "residual-tail\n");
    EXPECT_EQ(bytes_to_string(harness.output(failure).stdout_bytes), "residual-tail\n");
    EXPECT_EQ(bytes_to_string(harness.output(signaled).stdout_bytes), "signal-tail\n");
    for (RuntimeProcessId id : {success, failure, signaled}) {
        EXPECT_EQ(::kill(-harness.completion(id).pgid, 0), -1);
        EXPECT_EQ(errno, ESRCH);
    }
}

TEST(ProcessRuntime, EscapedSetsidDescendantCanDelayEofUntilFixtureIsCleanedExternally) {
    RuntimeHarness harness;
    const RuntimeProcessId id = harness.launch({"escape"}, std::nullopt, 20ms);
    ASSERT_TRUE(harness.poll_until([&harness, id] {
        return harness.stdout_text(id).find('\n') != std::string::npos;
    })) << harness.failure();
    const std::string output = bytes_to_string(harness.output(id).stdout_bytes);
    const std::size_t beginning = output.find("escaped=");
    ASSERT_NE(beginning, std::string::npos);
    const std::size_t end = output.find('\n', beginning);
    ASSERT_NE(end, std::string::npos);
    const pid_t escaped_pid = static_cast<pid_t>(
        std::stol(output.substr(beginning + std::string("escaped=").size(),
                                end - beginning - std::string("escaped=").size())));

    for (int attempt = 0; attempt < 3; ++attempt) {
        ASSERT_TRUE(harness.runtime().poll_once(10)) << harness.failure();
        harness.collect();
    }
    EXPECT_EQ(harness.runtime().running_count(), 1U);
    ASSERT_EQ(::kill(escaped_pid, SIGKILL), 0);
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    EXPECT_EQ(harness.completion(id).leader_outcome->exit_code, 0);
}

TEST(ProcessRuntime, RepeatedRapidConcurrentExitLeavesNoFdsRegistrationsOrZombies) {
    const std::size_t baseline_fds = count_open_fds();
    {
        RuntimeHarness harness;
        for (int index = 0; index < 40; ++index) {
            static_cast<void>(harness.launch({"exit", std::to_string(index % 5)}));
        }
        ASSERT_TRUE(harness.poll_until([&harness] {
            return harness.runtime().running_count() == 0U;
        }, 8s)) << harness.failure();
        EXPECT_EQ(harness.runtime().registration_count(), 1U);
        int status = 0;
        EXPECT_EQ(::waitpid(-1, &status, WNOHANG), -1);
        EXPECT_EQ(errno, ECHILD);
    }
    EXPECT_EQ(count_open_fds(), baseline_fds);
}

TEST(ProcessRuntime, SinkCanPauseWithBufferedDataAndResumeWithoutByteLoss) {
    RuntimeHarness harness;
    bool paused = false;
    std::size_t stdout_deliveries = 0U;
    harness.sink_hook = [&](RuntimeProcessId id, OutputStream stream, std::size_t) {
        if (stream != OutputStream::STDOUT) {
            return;
        }
        ++stdout_deliveries;
        if (!paused) {
            // The helper is blocked writing stdout until we drain it. Wait until another
            // chunk is ready, so a second read in this same callback would be observable.
            pollfd source{ProcessRuntimeTestPeer::output_fd(harness.runtime(), id, stream),
                          POLLIN, 0};
            ASSERT_EQ(::poll(&source, 1U, 1000), 1);
            ASSERT_NE(source.revents & POLLIN, 0);
            ASSERT_TRUE(harness.runtime().set_output_enabled(id, stream, false));
            paused = true;
        }
    };
    const RuntimeProcessId id = harness.launch({"large", "524288"});
    ASSERT_TRUE(harness.poll_until([&] { return paused; })) << harness.failure();
    EXPECT_EQ(stdout_deliveries, 1U);
    const std::size_t paused_size = harness.output(id).stdout_bytes.size();
    for (int iteration = 0; iteration < 12; ++iteration) {
        ASSERT_TRUE(harness.runtime().poll_once(0));
        harness.collect();
        EXPECT_EQ(harness.output(id).stdout_bytes.size(), paused_size);
        EXPECT_EQ(stdout_deliveries, 1U);
    }
    ASSERT_TRUE(harness.runtime().set_output_enabled(id, OutputStream::STDOUT, true));
    ASSERT_TRUE(harness.wait_completed(id)) << harness.failure();
    std::vector<std::uint8_t> expected(524288U);
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        expected[index] = static_cast<std::uint8_t>((index % 4096U) % 251U);
    }
    EXPECT_EQ(harness.output(id).stdout_bytes, expected);
    EXPECT_EQ(harness.output(id).stderr_bytes, expected);
}

TEST(ProcessRuntime, ContinuousOutputHasBoundedDispatchAndDoesNotStarveTimerOrReap) {
    RuntimeHarness harness;
    harness.capture_output = false;
    std::size_t bytes_this_poll = 0U;
    std::size_t total_bytes = 0U;
    harness.sink_hook = [&](RuntimeProcessId, OutputStream, std::size_t size) {
        bytes_this_poll += size;
        total_bytes += size;
    };
    const RuntimeProcessId flood = harness.launch({"continuous"}, 100ms, 20ms);
    const RuntimeProcessId quick = harness.launch({"exit", "0"});
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (harness.runtime().running_count() != 0U &&
           std::chrono::steady_clock::now() < deadline) {
        bytes_this_poll = 0U;
        ASSERT_TRUE(harness.runtime().poll_once(50));
        harness.collect();
        EXPECT_LE(bytes_this_poll, 64U * 1024U);
    }
    ASSERT_TRUE(harness.completed(quick));
    ASSERT_TRUE(harness.completed(flood));
    EXPECT_GT(total_bytes, 0U);
    EXPECT_EQ(harness.completion(quick).leader_outcome->exit_code, 0);
    EXPECT_EQ(harness.completion(flood).termination_cause, TerminationCause::TIMEOUT);
    EXPECT_EQ(harness.runtime().registration_count(), 1U);
}

class RegistrationFailureTest : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(RegistrationFailureTest, RetainsAndCleansAllPostForkResources) {
    const std::size_t baseline = count_open_fds();
    {
        RuntimeHarness harness;
        // Inherit SIGTERM-ignore through exec, forcing the recovery path to service
        // its grace timer and SIGKILL even if the helper has not reached main yet.
        struct sigaction ignored {};
        struct sigaction previous {};
        ignored.sa_handler = SIG_IGN;
        ASSERT_EQ(::sigemptyset(&ignored.sa_mask), 0);
        ASSERT_EQ(::sigaction(SIGTERM, &ignored, &previous), 0);
        ProcessRuntimeTestPeer::fail_next_registrations(harness.runtime(), GetParam());
        const RuntimeProcessId id = harness.launch({"ignore-term"}, std::nullopt, 20ms);
        ASSERT_EQ(::sigaction(SIGTERM, &previous, nullptr), 0);

        bool explicit_failure = false;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!harness.completed(id) && std::chrono::steady_clock::now() < deadline) {
            const auto polled = harness.runtime().poll_once(-1);
            if (!polled) {
                EXPECT_NE(polled.error().message.find("registration failed"),
                          std::string::npos);
                explicit_failure = true;
            }
            harness.collect();
        }
        ASSERT_TRUE(harness.completed(id));
        EXPECT_TRUE(explicit_failure);
        const auto& result = harness.completion(id);
        EXPECT_TRUE(result.exec_succeeded);
        EXPECT_EQ(result.termination_cause, TerminationCause::INTERNAL_ERROR);
        EXPECT_EQ(result.leader_outcome->term_signal, SIGKILL);
        EXPECT_TRUE(result.stdout_complete);
        EXPECT_TRUE(result.stderr_complete);
        EXPECT_EQ(::kill(-result.pgid, 0), -1);
        EXPECT_EQ(errno, ESRCH);
        int status = 0;
        EXPECT_EQ(::waitpid(result.leader_pid, &status, WNOHANG), -1);
        EXPECT_EQ(errno, ECHILD);
        EXPECT_EQ(harness.runtime().running_count(), 0U);
        EXPECT_EQ(harness.runtime().registration_count(), 1U);
    }
    EXPECT_EQ(count_open_fds(), baseline);
}

TEST_P(RegistrationFailureTest, HandlesRapidExitAndExecFailureDuringRecovery) {
    const std::size_t baseline = count_open_fds();
    {
        RuntimeHarness harness;
        for (const bool missing_executable : {false, true}) {
            ProcessRuntimeTestPeer::fail_next_registrations(harness.runtime(), GetParam());
            const RuntimeProcessId id = harness.launch(
                {"exit", "0"}, std::nullopt, 20ms,
                missing_executable ? "/definitely/missing/registration-failure-helper"
                                   : TS_RUNTIME_HELPER_PATH);
            bool reported = false;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!harness.completed(id) && std::chrono::steady_clock::now() < deadline) {
                const auto polled = harness.runtime().poll_once(20);
                if (!polled) {
                    EXPECT_NE(polled.error().message.find("registration failed"),
                              std::string::npos);
                    reported = true;
                }
                harness.collect();
            }
            ASSERT_TRUE(harness.completed(id));
            EXPECT_TRUE(reported);
            const auto& completion = harness.completion(id);
            EXPECT_EQ(completion.termination_cause, TerminationCause::INTERNAL_ERROR);
            if (missing_executable) {
                EXPECT_FALSE(completion.exec_succeeded);
                ASSERT_TRUE(completion.launch_failure.has_value());
                EXPECT_EQ(completion.launch_failure->stage, ChildFailureStage::EXECVE);
                EXPECT_EQ(completion.launch_failure->error_number, ENOENT);
            } else {
                EXPECT_TRUE(completion.exec_succeeded);
            }
            EXPECT_EQ(::kill(-completion.pgid, 0), -1);
            EXPECT_EQ(errno, ESRCH);
            EXPECT_EQ(harness.runtime().registration_count(), 1U);
            int status = 0;
            EXPECT_EQ(::waitpid(-1, &status, WNOHANG), -1);
            EXPECT_EQ(errno, ECHILD);
        }
    }
    EXPECT_EQ(count_open_fds(), baseline);
}

INSTANTIATE_TEST_SUITE_P(PostFork, RegistrationFailureTest,
                        ::testing::Values(1U << 2U, 1U << 3U, 1U << 4U,
                                          1U << 5U, 0x3cU));

}  // namespace
}  // namespace ts::runtime
