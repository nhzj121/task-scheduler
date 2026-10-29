#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

#include "ts/common/result.h"
#include "ts/runtime/prepared_exec.h"
#include "ts/runtime/process_handle.h"

namespace ts::runtime {

using RuntimeProcessId = std::uint64_t;

enum class OutputStream {
    STDOUT,
    STDERR,
};

enum class ChildFailureStage : std::uint16_t {
    SETPGID = 1,
    STDIN_DUP = 2,
    STDOUT_DUP = 3,
    STDERR_DUP = 4,
    FCHDIR = 5,
    SIGNAL_MASK_RESTORE = 6,
    EXECVE = 7,
    CLOSE_FDS = 8,
    MALFORMED_STATUS = 9,
};

struct LaunchFailure {
    ChildFailureStage stage;
    int error_number;
};

struct LaunchOptions {
    ExecutionSpec execution;
    std::optional<std::chrono::nanoseconds> timeout;
    std::chrono::nanoseconds termination_grace{std::chrono::seconds(1)};
};

struct ProcessCompletion {
    RuntimeProcessId process_id;
    pid_t leader_pid;
    pid_t pgid;
    bool exec_succeeded;
    std::optional<LaunchFailure> launch_failure;
    TerminationCause termination_cause;
    std::optional<LeaderOutcome> leader_outcome;
    bool stdout_complete;
    bool stderr_complete;
};

using OutputSink = std::function<common::Result<void>(
    RuntimeProcessId, OutputStream, const std::uint8_t*, std::size_t)>;
// Parent/owner-thread policy, queried before EACH read, never in the child.
// Zero pauses that stream; the owner explicitly re-enables it when capacity is
// available. Default behavior retains the accepted M4 read/dispatch budgets.
using OutputReadLimit = std::function<std::size_t(RuntimeProcessId, OutputStream)>;
struct ProcessObservation {
    pid_t leader_pid;
    bool exec_resolved, exec_succeeded, stdout_complete, stderr_complete;
    std::optional<LaunchFailure> launch_failure;
    bool outcome_fixed{false};
};

class ProcessRuntime final {
public:
    static common::Result<std::unique_ptr<ProcessRuntime>> create(
        OutputSink output_sink, OutputReadLimit read_limit = {});

    ~ProcessRuntime();
    ProcessRuntime(const ProcessRuntime&) = delete;
    ProcessRuntime& operator=(const ProcessRuntime&) = delete;
    ProcessRuntime(ProcessRuntime&&) = delete;
    ProcessRuntime& operator=(ProcessRuntime&&) = delete;

    [[nodiscard]] common::Result<RuntimeProcessId> launch(
        const LaunchOptions& options);
    [[nodiscard]] common::Result<bool> cancel(RuntimeProcessId process_id,
        std::optional<std::chrono::nanoseconds> grace = std::nullopt);
    // Errors do not release owned children. Continue polling to finish explicit cleanup
    // before destruction; failed fd registrations are serviced nonblockingly here.
    [[nodiscard]] common::Result<std::size_t> poll_once(int timeout_ms);
    [[nodiscard]] common::Result<void> set_output_enabled(
        RuntimeProcessId process_id, OutputStream stream, bool enabled);

    [[nodiscard]] std::vector<RuntimeProcessId> take_started();
    [[nodiscard]] std::vector<ProcessCompletion> take_completions();
    [[nodiscard]] std::size_t running_count() const noexcept;
    [[nodiscard]] std::size_t registration_count() const noexcept;
    // Non-owning value snapshot, valid even before exec; callers must check
    // exec_succeeded before using its PID to announce TASK_STARTED.
    [[nodiscard]] std::optional<ProcessObservation> observe(RuntimeProcessId id) const;

private:
    friend class ProcessRuntimeTestPeer;
    void fail_next_registrations_for_testing(std::uint32_t mask);
    int output_fd_for_testing(RuntimeProcessId id, OutputStream stream);
    class Impl;
    explicit ProcessRuntime(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

}  // namespace ts::runtime
