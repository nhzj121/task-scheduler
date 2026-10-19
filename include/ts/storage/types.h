#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ts/domain/states.h"

namespace ts::storage {

// All messages own metadata, never application objects, SQLite handles or log bytes.
struct TaskSpec {
    std::string task_id;
    std::string executable;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::optional<std::string> cwd;
    std::uint32_t cpu_slots{1};
    std::uint64_t memory_mb{0};
    std::optional<std::uint64_t> timeout_ms;
};
struct TaskRow {
    TaskSpec spec;
    domain::TaskState state{domain::TaskState::PENDING};
    std::uint64_t state_version{0};
    std::optional<std::string> cancel_requested_at;
    std::optional<std::string> latest_attempt_id;
    std::string created_at;
    std::string updated_at;
};
struct AttemptRow {
    std::string attempt_id, task_id;
    std::uint32_t attempt_no{1};
    domain::AttemptState state{domain::AttemptState::ASSIGNED};
    std::uint64_t state_version{0};
    std::string worker_id, session_id;
    std::optional<std::string> assigned_at, acknowledged_at, started_at, finished_at;
    std::optional<std::int32_t> exit_code, term_signal;
    std::optional<std::string> reason, observed_outcome_json;
    std::string stdout_path, stderr_path;
    std::uint64_t stdout_bytes{0}, stderr_bytes{0};
    std::optional<std::uint64_t> stdout_final_sequence, stderr_final_sequence;
    bool logs_complete{false};
};
struct TaskVersion {
    std::string task_id;
    std::uint64_t version{0};
};
struct AttemptVersion {
    TaskVersion task;
    std::string attempt_id;
    std::uint64_t version{0};
    std::string session_id;
};
struct Submit { TaskSpec task; std::string at; };
struct Assign {
    TaskVersion expected;
    std::string attempt_id, worker_id, session_id, stdout_path, stderr_path, at;
};
struct Advance {
    AttemptVersion expected;
    domain::AttemptState to{domain::AttemptState::STARTING};
    std::string at;
};
struct CancelIntent { TaskVersion expected; std::string at; };
struct CancelPending { TaskVersion expected; std::string at; };
struct Retry { TaskVersion expected; std::string at; };

// These are attestations supplied by the future EventLoop/LogCoordinator, not proof
// manufactured by SQLite. M5 neither opens log files nor implements a log barrier.
enum class LogEvidenceKind { FinalBarrier, ProvenEmpty, Incomplete };
struct LogEvidence {
    LogEvidenceKind kind{LogEvidenceKind::Incomplete};
    // FinalBarrier attests to THIS Attempt and to the paths/counters carried by
    // Terminalize: all earlier writes completed, final sequences/bytes reconciled,
    // then both files and every required containing-directory entry synced.
    std::string attempt_id;
    bool prior_writes_completed{false}, final_counters_verified{false};
    bool stdout_synced{false}, stderr_synced{false}, namespace_synced{false};
    bool explicit_dispatch_rejection{false}, no_dispatch_bytes{false};
    bool namespace_failed{false};
};
struct Terminalize {
    AttemptVersion expected;
    domain::AttemptState to{domain::AttemptState::FAILED};
    std::string at;
    std::optional<std::int32_t> exit_code, term_signal;
    std::optional<std::string> reason, observed_outcome_json;
    std::string stdout_path, stderr_path;
    std::uint64_t stdout_bytes{0}, stderr_bytes{0};
    std::optional<std::uint64_t> stdout_final_sequence, stderr_final_sequence;
    LogEvidence logs;
};
struct LoseSession { std::string session_id, at, reason; };
struct Recover { std::string at; };
struct ReadTask { std::string task_id; };
struct ReadAttempts {
    std::string task_id;
    std::uint32_t after_attempt_no{0};
    std::uint32_t limit{100};
};
struct TaskCursor { std::string created_at, task_id; };
struct ListTasks {
    std::optional<domain::TaskState> state;
    bool oldest_first{false};
    std::optional<TaskCursor> after;
    std::uint32_t limit{100};
};
using Command = std::variant<Submit, Assign, Advance, CancelIntent, CancelPending,
                             Terminalize, Retry, LoseSession, Recover,
                             ReadTask, ReadAttempts, ListTasks>;

enum class ErrorCode {
    Invalid, Conflict, Constraint, Busy, Full, Io, Corrupt, Unsupported,
    LockUnavailable, Aborted, Internal, UnknownCommit, Capacity
};
struct Error {
    ErrorCode code{ErrorCode::Internal};
    int sqlite_code{0};
    bool fatal{false};
    std::string message;
};
struct Outcome {
    bool committed{false}; // false for reads, including successful reads
    std::optional<Error> error;
    std::vector<TaskRow> tasks;
    std::vector<AttemptRow> attempts;
    std::size_t affected{0};
    bool has_more{false};
    std::optional<TaskCursor> next;
    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};
struct Correlation {
    std::uint64_t operation_id{0};
    std::string task_id, attempt_id, session_id;
    std::uint64_t expected_task_version{0}, expected_attempt_version{0};
    std::uint64_t entity_generation{0}, session_generation{0};
    friend bool operator==(const Correlation& a, const Correlation& b) noexcept;
};
struct Envelope {
    Correlation correlation;
    Command command;
    std::size_t response_bytes{4096};
};
struct Completion { Correlation correlation; Outcome outcome; };

[[nodiscard]] Correlation correlate(const Command& command, std::uint64_t operation_id,
                                    std::uint64_t entity_generation = 0,
                                    std::uint64_t session_generation = 0);

[[nodiscard]] bool is_essential(const Command& command) noexcept;
[[nodiscard]] bool is_read(const Command& command) noexcept;
// Conservative owned-allocation accounting (including container capacities).
[[nodiscard]] std::size_t owned_bytes(const Envelope& envelope);
[[nodiscard]] std::size_t owned_bytes(const Outcome& outcome);

} // namespace ts::storage
