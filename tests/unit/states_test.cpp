#include "ts/domain/states.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace ts::domain {
namespace {

TEST(States, ExactSpellingsRoundTrip) {
    constexpr std::array task_states{TaskState::PENDING, TaskState::ACTIVE, TaskState::SUCCEEDED,
                                     TaskState::FAILED, TaskState::CANCELLED};
    constexpr std::array task_names{"PENDING", "ACTIVE", "SUCCEEDED", "FAILED", "CANCELLED"};
    for (std::size_t index = 0U; index < task_states.size(); ++index) {
        EXPECT_EQ(to_string(task_states[index]), task_names[index]);
        const auto parsed = parse_task_state(task_names[index]);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed.value(), task_states[index]);
    }

    constexpr std::array attempt_states{
        AttemptState::ASSIGNED, AttemptState::STARTING, AttemptState::RUNNING,
        AttemptState::SUCCEEDED, AttemptState::FAILED, AttemptState::TIMED_OUT,
        AttemptState::CANCELLED, AttemptState::LOST};
    constexpr std::array attempt_names{"ASSIGNED", "STARTING", "RUNNING", "SUCCEEDED",
                                       "FAILED", "TIMED_OUT", "CANCELLED", "LOST"};
    for (std::size_t index = 0U; index < attempt_states.size(); ++index) {
        EXPECT_EQ(to_string(attempt_states[index]), attempt_names[index]);
        const auto parsed = parse_attempt_state(attempt_names[index]);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed.value(), attempt_states[index]);
    }

    EXPECT_EQ(to_string(WorkerState::HEALTHY), "HEALTHY");
    EXPECT_EQ(to_string(WorkerState::OFFLINE), "OFFLINE");
    EXPECT_FALSE(parse_task_state("FINALIZING"));
    EXPECT_FALSE(parse_attempt_state("PENDING"));
    EXPECT_FALSE(parse_worker_state("STARTING"));
}

TEST(States, TaskTransitionTableIsExact) {
    constexpr std::array states{TaskState::PENDING, TaskState::ACTIVE, TaskState::SUCCEEDED,
                                TaskState::FAILED, TaskState::CANCELLED};
    const std::vector<std::pair<TaskState, TaskState>> valid{
        {TaskState::PENDING, TaskState::ACTIVE},
        {TaskState::PENDING, TaskState::CANCELLED},
        {TaskState::ACTIVE, TaskState::SUCCEEDED},
        {TaskState::ACTIVE, TaskState::FAILED},
        {TaskState::ACTIVE, TaskState::CANCELLED},
        {TaskState::FAILED, TaskState::PENDING},
    };

    for (const auto from : states) {
        for (const auto to : states) {
            const bool expected =
                std::find(valid.begin(), valid.end(), std::pair{from, to}) != valid.end();
            EXPECT_EQ(can_transition(from, to), expected);
            EXPECT_EQ(static_cast<bool>(validate_transition(from, to)), expected);
        }
    }
}

TEST(States, AttemptTransitionTableIsExactAndTerminalStatesNeverChange) {
    constexpr std::array states{
        AttemptState::ASSIGNED, AttemptState::STARTING, AttemptState::RUNNING,
        AttemptState::SUCCEEDED, AttemptState::FAILED, AttemptState::TIMED_OUT,
        AttemptState::CANCELLED, AttemptState::LOST};
    const std::vector<std::pair<AttemptState, AttemptState>> valid{
        {AttemptState::ASSIGNED, AttemptState::STARTING},
        {AttemptState::ASSIGNED, AttemptState::FAILED},
        {AttemptState::ASSIGNED, AttemptState::CANCELLED},
        {AttemptState::ASSIGNED, AttemptState::LOST},
        {AttemptState::STARTING, AttemptState::RUNNING},
        {AttemptState::STARTING, AttemptState::FAILED},
        {AttemptState::STARTING, AttemptState::CANCELLED},
        {AttemptState::STARTING, AttemptState::LOST},
        {AttemptState::RUNNING, AttemptState::SUCCEEDED},
        {AttemptState::RUNNING, AttemptState::FAILED},
        {AttemptState::RUNNING, AttemptState::TIMED_OUT},
        {AttemptState::RUNNING, AttemptState::CANCELLED},
        {AttemptState::RUNNING, AttemptState::LOST},
    };

    for (const auto from : states) {
        for (const auto to : states) {
            const bool expected =
                std::find(valid.begin(), valid.end(), std::pair{from, to}) != valid.end();
            EXPECT_EQ(can_transition(from, to), expected);
            EXPECT_EQ(static_cast<bool>(validate_transition(from, to)), expected);
        }
    }

    for (const auto terminal : {AttemptState::SUCCEEDED, AttemptState::FAILED,
                                AttemptState::TIMED_OUT, AttemptState::CANCELLED,
                                AttemptState::LOST}) {
        EXPECT_TRUE(is_terminal(terminal));
        for (const auto target : states) {
            EXPECT_FALSE(can_transition(terminal, target));
        }
    }
}

TEST(States, FailedToPendingIsTheOnlyTaskRetryTransition) {
    EXPECT_TRUE(validate_transition(TaskState::FAILED, TaskState::PENDING));
    for (const auto state : {TaskState::PENDING, TaskState::ACTIVE, TaskState::SUCCEEDED,
                             TaskState::CANCELLED}) {
        EXPECT_FALSE(validate_transition(state, TaskState::PENDING));
    }
}

TEST(States, WorkerTransitionTableIsExact) {
    EXPECT_TRUE(validate_transition(WorkerState::OFFLINE, WorkerState::HEALTHY));
    EXPECT_TRUE(validate_transition(WorkerState::HEALTHY, WorkerState::OFFLINE));
    EXPECT_FALSE(validate_transition(WorkerState::OFFLINE, WorkerState::OFFLINE));
    EXPECT_FALSE(validate_transition(WorkerState::HEALTHY, WorkerState::HEALTHY));
}

}  // namespace
}  // namespace ts::domain
