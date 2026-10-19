#pragma once
#include <array>
#include <string_view>
#include "ts/protocol/frame.h"

namespace ts::test {
// Independent literals transcribed from PROTOCOL.md, not produced by the codec under test.
struct Fixture { protocol::Type type; std::string_view json; };
inline const std::array<Fixture,26> fixtures{{
 {protocol::Type::ErrorResponse,R"({"code":"INVALID_ARGUMENT","message":"cpu_slots must be at least 1","retryable":false,"details":{}})"},
 {protocol::Type::SubmitTask,R"({"executable":"/usr/bin/python3","args":["demo.py"],"env":{"MODE":"test"},"cwd":"/srv/jobs","resources":{"cpu_slots":2,"memory_mb":512},"timeout_ms":30000})"},
 {protocol::Type::SubmitTaskResponse,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","state":"PENDING","created_at":"2026-09-26T08:15:30.123456Z"})"},
 {protocol::Type::GetTask,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878"})"},
 {protocol::Type::GetTaskResponse,R"({"task":{"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","state":"ACTIVE","executable":"/usr/bin/python3","args":["demo.py"],"env":{"MODE":"test"},"cwd":"/srv/jobs","resources":{"cpu_slots":2,"memory_mb":512},"timeout_ms":30000,"cancel_requested_at":null,"created_at":"2026-09-26T08:15:30.123456Z","updated_at":"2026-09-26T08:15:31.000000Z","latest_attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71"},"attempts":[{"attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_no":1,"state":"RUNNING","worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","assigned_at":"2026-09-26T08:15:31.000000Z","started_at":"2026-09-26T08:15:31.050000Z","finished_at":null,"exit_code":null,"term_signal":null,"reason":null,"stdout_bytes":128,"stderr_bytes":0,"logs_complete":false,"finalization_pending":false}]})"},
 {protocol::Type::ListTasks,R"({"page_size":100,"page_token":null})"},
 {protocol::Type::ListTasksResponse,R"({"tasks":[{"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","state":"ACTIVE","executable":"/usr/bin/python3","attempt_count":1,"created_at":"2026-09-26T08:15:30.123456Z","updated_at":"2026-09-26T08:15:31.000000Z"}],"next_page_token":"opaque-or-null"})"},
 {protocol::Type::GetLogs,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_no":1,"stream":"stdout","offset":0,"limit_bytes":262144,"snapshot_end":null})"},
 {protocol::Type::GetLogsResponse,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_no":1,"stream":"stdout","offset":0,"next_offset":6,"snapshot_end":6,"data_base64":"aGVsbG8K","eof_snapshot":true,"logs_complete":false})"},
 {protocol::Type::CancelTask,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878"})"},
 {protocol::Type::CancelTaskResponse,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","status":"accepted","task_state":"ACTIVE","cancel_requested_at":"2026-09-26T08:16:00.000000Z"})"},
 {protocol::Type::RetryTask,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878"})"},
 {protocol::Type::RetryTaskResponse,R"({"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","state":"PENDING","next_attempt_no":2})"},
 {protocol::Type::ListWorkers,R"({})"},
 {protocol::Type::ListWorkersResponse,R"({"workers":[{"worker_id":"worker-a","state":"HEALTHY","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","capacity":{"cpu_slots":8,"memory_mb":16384},"reserved":{"cpu_slots":2,"memory_mb":512},"running_attempts":1,"max_concurrent_tasks":8,"last_heartbeat_at":"2026-09-26T08:16:02.000000Z"}]})"},
 {protocol::Type::RegisterWorker,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","capacity":{"cpu_slots":8,"memory_mb":16384},"max_concurrent_tasks":8,"worker_version":"1.0.0"})"},
 {protocol::Type::RegisterAck,R"({"accepted":true,"heartbeat_interval_ms":3000,"offline_timeout_ms":10000,"termination_grace_ms":2000,"initial_log_window_bytes":1048576,"initial_log_window_chunks":64,"controller_instance_id":"8d2743fa-e5d1-4036-8731-bc377c181181"})"},
 {protocol::Type::Heartbeat,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","sequence":17,"running_attempt_ids":["c38a0520-2172-44c4-aaf5-d5ab4392bc71"]})"},
 {protocol::Type::RunTask,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","attempt_no":1,"executable":"/usr/bin/python3","args":["demo.py"],"env":{"MODE":"test"},"cwd":"/srv/jobs","resources":{"cpu_slots":2,"memory_mb":512},"timeout_ms":30000})"},
 {protocol::Type::RunTaskAck,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","accepted":true,"error_code":null,"message":null})"},
 {protocol::Type::TaskStarted,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","pid":24130})"},
 {protocol::Type::LogChunk,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","stream":"stdout","sequence":0,"raw_size":6,"data_base64":"aGVsbG8K"})"},
 {protocol::Type::LogWindowUpdate,R"({"grant_bytes":65536,"grant_chunks":1})"},
 {protocol::Type::TaskFinished,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","outcome":"EXITED","exit_code":0,"term_signal":null,"reason":null,"stdout_next_sequence":1,"stderr_next_sequence":0,"stdout_bytes":6,"stderr_bytes":0})"},
 {protocol::Type::WorkerCancelTask,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","grace_ms":2000})"},
 {protocol::Type::WorkerCancelTaskAck,R"({"worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","accepted":true,"already_terminal":false,"message":null})"}
}};
} // namespace ts::test
