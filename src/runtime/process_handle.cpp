#include "ts/runtime/process_handle.h"

#include <sys/wait.h>

namespace ts::runtime {

ProcessHandle::ProcessHandle(pid_t leader_pid, pid_t pgid,
                             bool group_established) noexcept
    : leader_pid_(leader_pid),
      pgid_(pgid),
      group_established_(group_established) {}

pid_t ProcessHandle::leader_pid() const noexcept {
    return leader_pid_;
}

pid_t ProcessHandle::pgid() const noexcept {
    return pgid_;
}

bool ProcessHandle::group_established() const noexcept {
    return group_established_;
}

void ProcessHandle::confirm_group_established() noexcept {
    group_established_ = true;
}

bool ProcessHandle::reaped() const noexcept {
    return reaped_;
}

const std::optional<LeaderOutcome>& ProcessHandle::leader_outcome() const noexcept {
    return leader_outcome_;
}

bool ProcessHandle::natural_outcome_frozen() const noexcept {
    return natural_outcome_frozen_;
}

TerminationCause ProcessHandle::termination_cause() const noexcept {
    return termination_cause_;
}

bool ProcessHandle::install_termination_cause(TerminationCause cause) noexcept {
    if (cause == TerminationCause::NONE || termination_cause_ != TerminationCause::NONE ||
        natural_outcome_frozen_) {
        return false;
    }
    termination_cause_ = cause;
    return true;
}

bool ProcessHandle::record_reap_status(int wait_status) noexcept {
    if (reaped_) {
        return false;
    }
    LeaderOutcome outcome;
    if (WIFEXITED(wait_status)) {
        outcome.exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        outcome.term_signal = WTERMSIG(wait_status);
    } else {
        return false;
    }
    leader_outcome_ = outcome;
    reaped_ = true;
    natural_outcome_frozen_ = termination_cause_ == TerminationCause::NONE;
    return true;
}

GroupCleanupPhase ProcessHandle::cleanup_phase() const noexcept {
    return cleanup_phase_;
}

void ProcessHandle::set_cleanup_phase(GroupCleanupPhase phase) noexcept {
    cleanup_phase_ = phase;
}

}  // namespace ts::runtime
