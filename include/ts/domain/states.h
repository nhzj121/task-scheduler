#pragma once

#include <string_view>

#include "ts/common/result.h"

namespace ts::domain {

enum class TaskState {
    PENDING,
    ACTIVE,
    SUCCEEDED,
    FAILED,
    CANCELLED,
};

enum class AttemptState {
    ASSIGNED,
    STARTING,
    RUNNING,
    SUCCEEDED,
    FAILED,
    TIMED_OUT,
    CANCELLED,
    LOST,
};

enum class WorkerState {
    HEALTHY,
    OFFLINE,
};

[[nodiscard]] std::string_view to_string(TaskState state) noexcept;
[[nodiscard]] std::string_view to_string(AttemptState state) noexcept;
[[nodiscard]] std::string_view to_string(WorkerState state) noexcept;

[[nodiscard]] common::Result<TaskState> parse_task_state(std::string_view text);
[[nodiscard]] common::Result<AttemptState> parse_attempt_state(std::string_view text);
[[nodiscard]] common::Result<WorkerState> parse_worker_state(std::string_view text);

[[nodiscard]] bool is_terminal(TaskState state) noexcept;
[[nodiscard]] bool is_terminal(AttemptState state) noexcept;

[[nodiscard]] bool can_transition(TaskState from, TaskState to) noexcept;
[[nodiscard]] bool can_transition(AttemptState from, AttemptState to) noexcept;
[[nodiscard]] bool can_transition(WorkerState from, WorkerState to) noexcept;

[[nodiscard]] common::Result<void> validate_transition(TaskState from, TaskState to);
[[nodiscard]] common::Result<void> validate_transition(AttemptState from, AttemptState to);
[[nodiscard]] common::Result<void> validate_transition(WorkerState from, WorkerState to);

}  // namespace ts::domain
