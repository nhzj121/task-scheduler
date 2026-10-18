#pragma once

#include <map>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>

#include "ts/common/timestamp.h"
#include "ts/domain/ids.h"
#include "ts/domain/resources.h"
#include "ts/domain/states.h"
#include "ts/protocol/frame.h"

namespace ts::protocol {
inline constexpr std::uint64_t kMaxLogPage = 3144960;
enum class Stream { Stdout, Stderr };
enum class ErrorCode { InvalidArgument, NotFound, Conflict, Unsupported, TemporarilyUnavailable, Internal };
enum class CancelStatus { Accepted, Completed, AlreadyCancelled };
enum class Outcome { Exited, Signaled, TimedOut, Cancelled, LaunchFailed };
using Environment = std::map<std::string, std::string>;
using Strings = std::vector<std::string>;
using Milliseconds = std::optional<std::uint64_t>;

struct SubmitTask {
    std::string executable; Strings args; Environment env; std::optional<std::string> cwd;
    domain::ResourceRequest resources; Milliseconds timeout_ms;
};
struct Task {
    domain::TaskId task_id; domain::TaskState state;
    std::string executable; Strings args; Environment env; std::optional<std::string> cwd;
    domain::ResourceRequest resources; Milliseconds timeout_ms;
    std::optional<common::Timestamp> cancel_requested_at;
    common::Timestamp created_at; common::Timestamp updated_at;
    std::optional<domain::AttemptId> latest_attempt_id;
};
struct Attempt {
    domain::AttemptId attempt_id; domain::TaskId task_id; domain::AttemptNumber attempt_no;
    domain::AttemptState state; std::optional<domain::WorkerId> worker_id;
    std::optional<domain::SessionId> session_id;
    std::optional<common::Timestamp> assigned_at, started_at, finished_at;
    std::optional<std::int32_t> exit_code, term_signal;
    std::optional<std::string> reason;
    std::uint64_t stdout_bytes, stderr_bytes; bool logs_complete, finalization_pending;
};
struct TaskSummary {
    domain::TaskId task_id; domain::TaskState state; std::string executable;
    std::uint32_t attempt_count; common::Timestamp created_at, updated_at;
};
struct Worker {
    domain::WorkerId worker_id; domain::WorkerState state;
    std::optional<domain::SessionId> session_id;
    domain::ResourceCapacity capacity; domain::ResourceUsage reserved;
    std::uint32_t running_attempts; std::optional<std::uint32_t> max_concurrent_tasks;
    std::optional<common::Timestamp> last_heartbeat_at;
};
struct ErrorResponse { ErrorCode code; std::string message; bool retryable; nlohmann::json details; };
struct SubmitTaskResponse { domain::TaskId task_id; domain::TaskState state; common::Timestamp created_at; };
struct GetTask { domain::TaskId task_id; };
struct GetTaskResponse { Task task; std::vector<Attempt> attempts; };
struct ListTasks { std::uint64_t page_size; std::optional<std::string> page_token; };
struct ListTasksResponse { std::vector<TaskSummary> tasks; std::optional<std::string> next_page_token; };
struct GetLogs {
    domain::TaskId task_id; std::optional<domain::AttemptNumber> attempt_no; Stream stream;
    std::uint64_t offset, limit_bytes; std::optional<std::uint64_t> snapshot_end;
};
struct GetLogsResponse {
    domain::TaskId task_id; domain::AttemptNumber attempt_no; Stream stream;
    std::uint64_t offset, next_offset, snapshot_end; std::string data_base64;
    bool eof_snapshot, logs_complete;
};
struct CancelTask { domain::TaskId task_id; };
struct CancelTaskResponse {
    domain::TaskId task_id; CancelStatus status; domain::TaskState task_state;
    std::optional<common::Timestamp> cancel_requested_at;
};
struct RetryTask { domain::TaskId task_id; };
struct RetryTaskResponse { domain::TaskId task_id; domain::TaskState state; domain::AttemptNumber next_attempt_no; };
struct ListWorkers {};
struct ListWorkersResponse { std::vector<Worker> workers; };
struct RegisterWorker {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::ResourceCapacity capacity;
    std::optional<std::uint32_t> max_concurrent_tasks; std::string worker_version;
};
struct RegistrationAccepted {
    bool accepted;
    std::uint64_t heartbeat_interval_ms, offline_timeout_ms, termination_grace_ms;
    std::uint64_t initial_log_window_bytes, initial_log_window_chunks;
    domain::SessionId controller_instance_id;
};
struct RegistrationRejected { bool accepted; std::string error_code, message; };
struct RegisterAck { std::variant<RegistrationAccepted, RegistrationRejected> result; };
struct Heartbeat {
    domain::WorkerId worker_id; domain::SessionId session_id; std::uint64_t sequence;
    std::vector<domain::AttemptId> running_attempt_ids;
};
struct RunTask {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::TaskId task_id;
    domain::AttemptId attempt_id; domain::AttemptNumber attempt_no;
    std::string executable; Strings args; Environment env; std::optional<std::string> cwd;
    domain::ResourceRequest resources; Milliseconds timeout_ms;
};
struct RunTaskAck {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::AttemptId attempt_id;
    bool accepted; std::optional<std::string> error_code, message;
};
struct TaskStarted {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::TaskId task_id;
    domain::AttemptId attempt_id; std::int32_t pid;
};
struct LogChunk {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::TaskId task_id;
    domain::AttemptId attempt_id; Stream stream; std::uint64_t sequence;
    std::uint32_t raw_size; std::string data_base64;
};
struct LogWindowUpdate { std::uint64_t grant_bytes, grant_chunks; };
struct TaskFinished {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::TaskId task_id;
    domain::AttemptId attempt_id; Outcome outcome;
    std::optional<std::int32_t> exit_code, term_signal; std::optional<std::string> reason;
    std::uint64_t stdout_next_sequence, stderr_next_sequence, stdout_bytes, stderr_bytes;
};
struct WorkerCancelTask {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::TaskId task_id;
    domain::AttemptId attempt_id; std::uint64_t grace_ms;
};
struct WorkerCancelTaskAck {
    domain::WorkerId worker_id; domain::SessionId session_id; domain::AttemptId attempt_id;
    bool accepted, already_terminal; std::optional<std::string> message;
};
using Message = std::variant<ErrorResponse, SubmitTask, SubmitTaskResponse, GetTask, GetTaskResponse,
    ListTasks, ListTasksResponse, GetLogs, GetLogsResponse, CancelTask, CancelTaskResponse,
    RetryTask, RetryTaskResponse, ListWorkers, ListWorkersResponse, RegisterWorker, RegisterAck,
    Heartbeat, RunTask, RunTaskAck, TaskStarted, LogChunk, LogWindowUpdate, TaskFinished,
    WorkerCancelTask, WorkerCancelTaskAck>;
struct ValidationOptions {
    std::uint64_t maximum_timeout_ms{9223372036854775807ULL};
};
[[nodiscard]] Type message_type(const Message& message);
[[nodiscard]] ProtocolResult<Message> decode_message(const Frame& frame, Direction expected, ValidationOptions options = {});
[[nodiscard]] ProtocolResult<Frame> encode_message(const Message& message, std::uint64_t request_id, Direction expected, ValidationOptions options = {});
[[nodiscard]] std::string base64_encode(std::string_view raw);
[[nodiscard]] ProtocolResult<std::string> base64_decode(std::string_view encoded, std::size_t maximum_raw);

// Explicit caller-provided context, no session ownership or credit admission policy.
struct StreamPosition { std::uint64_t next_sequence{0}, bytes{0}; };
struct LogContext { StreamPosition out, err; std::uint64_t credit_bytes, credit_chunks; };
[[nodiscard]] ProtocolResult<LogContext> check_log(const LogChunk& chunk, LogContext context);
[[nodiscard]] ProtocolResult<bool> check_finish(const TaskFinished& finish, const LogContext& context);
} // namespace ts::protocol
