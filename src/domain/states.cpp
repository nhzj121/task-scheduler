#include "ts/domain/states.h"

#include <string>

namespace ts::domain {
namespace {

template <typename State>
common::Result<void> validate(State from, State to, std::string_view kind) {
    if (can_transition(from, to)) {
        return common::Result<void>::success();
    }
    return common::Result<void>::failure(
        "invalid " + std::string(kind) + " transition from " + std::string(to_string(from)) +
        " to " + std::string(to_string(to)));
}

}  // namespace

std::string_view to_string(TaskState state) noexcept {
    switch (state) {
        case TaskState::PENDING:
            return "PENDING";
        case TaskState::ACTIVE:
            return "ACTIVE";
        case TaskState::SUCCEEDED:
            return "SUCCEEDED";
        case TaskState::FAILED:
            return "FAILED";
        case TaskState::CANCELLED:
            return "CANCELLED";
    }
    return "<invalid TaskState>";
}

std::string_view to_string(AttemptState state) noexcept {
    switch (state) {
        case AttemptState::ASSIGNED:
            return "ASSIGNED";
        case AttemptState::STARTING:
            return "STARTING";
        case AttemptState::RUNNING:
            return "RUNNING";
        case AttemptState::SUCCEEDED:
            return "SUCCEEDED";
        case AttemptState::FAILED:
            return "FAILED";
        case AttemptState::TIMED_OUT:
            return "TIMED_OUT";
        case AttemptState::CANCELLED:
            return "CANCELLED";
        case AttemptState::LOST:
            return "LOST";
    }
    return "<invalid AttemptState>";
}

std::string_view to_string(WorkerState state) noexcept {
    switch (state) {
        case WorkerState::HEALTHY:
            return "HEALTHY";
        case WorkerState::OFFLINE:
            return "OFFLINE";
    }
    return "<invalid WorkerState>";
}

common::Result<TaskState> parse_task_state(std::string_view text) {
    for (const TaskState state : {TaskState::PENDING, TaskState::ACTIVE, TaskState::SUCCEEDED,
                                  TaskState::FAILED, TaskState::CANCELLED}) {
        if (text == to_string(state)) {
            return common::Result<TaskState>::success(state);
        }
    }
    return common::Result<TaskState>::failure("unknown Task state");
}

common::Result<AttemptState> parse_attempt_state(std::string_view text) {
    for (const AttemptState state :
         {AttemptState::ASSIGNED, AttemptState::STARTING, AttemptState::RUNNING,
          AttemptState::SUCCEEDED, AttemptState::FAILED, AttemptState::TIMED_OUT,
          AttemptState::CANCELLED, AttemptState::LOST}) {
        if (text == to_string(state)) {
            return common::Result<AttemptState>::success(state);
        }
    }
    return common::Result<AttemptState>::failure("unknown Attempt state");
}

common::Result<WorkerState> parse_worker_state(std::string_view text) {
    for (const WorkerState state : {WorkerState::HEALTHY, WorkerState::OFFLINE}) {
        if (text == to_string(state)) {
            return common::Result<WorkerState>::success(state);
        }
    }
    return common::Result<WorkerState>::failure("unknown Worker state");
}

bool is_terminal(TaskState state) noexcept {
    return state == TaskState::SUCCEEDED || state == TaskState::FAILED ||
           state == TaskState::CANCELLED;
}

bool is_terminal(AttemptState state) noexcept {
    return state == AttemptState::SUCCEEDED || state == AttemptState::FAILED ||
           state == AttemptState::TIMED_OUT || state == AttemptState::CANCELLED ||
           state == AttemptState::LOST;
}

bool can_transition(TaskState from, TaskState to) noexcept {
    switch (from) {
        case TaskState::PENDING:
            return to == TaskState::ACTIVE || to == TaskState::CANCELLED;
        case TaskState::ACTIVE:
            return to == TaskState::SUCCEEDED || to == TaskState::FAILED ||
                   to == TaskState::CANCELLED;
        case TaskState::FAILED:
            return to == TaskState::PENDING;
        case TaskState::SUCCEEDED:
        case TaskState::CANCELLED:
            return false;
    }
    return false;
}

bool can_transition(AttemptState from, AttemptState to) noexcept {
    switch (from) {
        case AttemptState::ASSIGNED:
            return to == AttemptState::STARTING || to == AttemptState::FAILED ||
                   to == AttemptState::CANCELLED || to == AttemptState::LOST;
        case AttemptState::STARTING:
            return to == AttemptState::RUNNING || to == AttemptState::FAILED ||
                   to == AttemptState::CANCELLED || to == AttemptState::LOST;
        case AttemptState::RUNNING:
            return to == AttemptState::SUCCEEDED || to == AttemptState::FAILED ||
                   to == AttemptState::TIMED_OUT || to == AttemptState::CANCELLED ||
                   to == AttemptState::LOST;
        case AttemptState::SUCCEEDED:
        case AttemptState::FAILED:
        case AttemptState::TIMED_OUT:
        case AttemptState::CANCELLED:
        case AttemptState::LOST:
            return false;
    }
    return false;
}

bool can_transition(WorkerState from, WorkerState to) noexcept {
    return (from == WorkerState::OFFLINE && to == WorkerState::HEALTHY) ||
           (from == WorkerState::HEALTHY && to == WorkerState::OFFLINE);
}

common::Result<void> validate_transition(TaskState from, TaskState to) {
    return validate(from, to, "Task");
}

common::Result<void> validate_transition(AttemptState from, AttemptState to) {
    return validate(from, to, "Attempt");
}

common::Result<void> validate_transition(WorkerState from, WorkerState to) {
    return validate(from, to, "Worker");
}

}  // namespace ts::domain
