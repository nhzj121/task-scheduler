#pragma once

#include <optional>

#include <sys/types.h>

namespace ts::runtime {

enum class TerminationCause {
    NONE,
    TIMEOUT,
    CANCELLED,
    INTERNAL_ERROR,
};

enum class GroupCleanupPhase {
    NONE,
    TERM_SENT,
    KILL_SENT,
    ABSENT,
};

struct LeaderOutcome {
    std::optional<int> exit_code;
    std::optional<int> term_signal;
};

class ProcessHandle final {
public:
    ProcessHandle(pid_t leader_pid, pid_t pgid, bool group_established) noexcept;
    ~ProcessHandle() = default;

    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&&) noexcept = default;
    ProcessHandle& operator=(ProcessHandle&&) noexcept = default;

    [[nodiscard]] pid_t leader_pid() const noexcept;
    [[nodiscard]] pid_t pgid() const noexcept;
    [[nodiscard]] bool group_established() const noexcept;
    void confirm_group_established() noexcept;

    [[nodiscard]] bool reaped() const noexcept;
    [[nodiscard]] const std::optional<LeaderOutcome>& leader_outcome() const noexcept;
    [[nodiscard]] bool natural_outcome_frozen() const noexcept;
    [[nodiscard]] TerminationCause termination_cause() const noexcept;
    [[nodiscard]] bool install_termination_cause(TerminationCause cause) noexcept;
    [[nodiscard]] bool record_reap_status(int wait_status) noexcept;

    [[nodiscard]] GroupCleanupPhase cleanup_phase() const noexcept;
    void set_cleanup_phase(GroupCleanupPhase phase) noexcept;

private:
    pid_t leader_pid_;
    pid_t pgid_;
    bool group_established_;
    bool reaped_{false};
    std::optional<LeaderOutcome> leader_outcome_;
    bool natural_outcome_frozen_{false};
    TerminationCause termination_cause_{TerminationCause::NONE};
    GroupCleanupPhase cleanup_phase_{GroupCleanupPhase::NONE};
};

}  // namespace ts::runtime
