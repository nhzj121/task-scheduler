# TCP Protocol v1

This document freezes protocol version 1. Controller exposes separate configurable listeners
for client and Worker roles, so a connection has one role for its entire lifetime. Worker control
and log frames share one ordered Worker connection, with application-level log credit preventing
log traffic from monopolizing the control plane.

## Wire header

Every frame is exactly:

```text
byte offset  size  field
0            4     magic
4            2     version
6            2     message_type
8            8     request_id
16           4     payload_length
20           N     UTF-8 JSON payload bytes
```

| Field | Wire type | v1 rule |
|---|---|---|
| `magic` | unsigned 32-bit | `0x54534348`, ASCII `TSCH` |
| `version` | unsigned 16-bit | `1` |
| `message_type` | unsigned 16-bit | Frozen in the table below |
| `request_id` | unsigned 64-bit | Correlation rules below |
| `payload_length` | unsigned 32-bit | `0..4,194,304` bytes |

The fixed header length is 20 bytes. Every integer is explicitly encoded in network byte order
(most significant byte first). The implementation must provide tested 64-bit host/network-order
helpers; it must not cast, pack, or send a C++ `struct` as wire data. JSON payloads use UTF-8 and
must contain exactly one object. A zero-length payload is reserved and is not used by the v1
messages below.

## IDs and common scalar rules

- `task_id`, `attempt_id`, and `session_id` are canonical lowercase UUID strings.
- `worker_id` is a configured nonempty ASCII identifier, maximum 128 bytes, stable across Worker
  restarts.
- `attempt_no` starts at 1 for each Task and increases by one.
- Timestamps are UTC RFC 3339 strings with microseconds, for example
  `2026-09-26T08:15:30.123456Z`. They are informational on the wire; the Controller supplies
  authoritative persisted timestamps.
- Durations use integer milliseconds unless a field explicitly says otherwise.
- Resource values are integers: `cpu_slots >= 1` for requests and capacity, `memory_mb >= 0` for
  requests, and capacity memory greater than zero in normal configurations.
- Unknown JSON fields are rejected in v1 to catch version mismatch early. Required, optional, and
  nullable fields are explicit below.

## Message type IDs

Names are qualified where the source specification uses `CANCEL_TASK` in two directions.

| ID | Name | Direction | Correlation |
|---:|---|---|---|
| `0x0001` | `ERROR_RESPONSE` | Controller -> client | response |
| `0x0101` | `CLIENT.SUBMIT_TASK` | client -> Controller | request |
| `0x0102` | `CLIENT.SUBMIT_TASK_RESPONSE` | Controller -> client | response |
| `0x0103` | `CLIENT.GET_TASK` | client -> Controller | request |
| `0x0104` | `CLIENT.GET_TASK_RESPONSE` | Controller -> client | response |
| `0x0105` | `CLIENT.LIST_TASKS` | client -> Controller | request |
| `0x0106` | `CLIENT.LIST_TASKS_RESPONSE` | Controller -> client | response |
| `0x0107` | `CLIENT.GET_LOGS` | client -> Controller | request |
| `0x0108` | `CLIENT.GET_LOGS_RESPONSE` | Controller -> client | response |
| `0x0109` | `CLIENT.CANCEL_TASK` | client -> Controller | request |
| `0x010A` | `CLIENT.CANCEL_TASK_RESPONSE` | Controller -> client | response |
| `0x010B` | `CLIENT.RETRY_TASK` | client -> Controller | request |
| `0x010C` | `CLIENT.RETRY_TASK_RESPONSE` | Controller -> client | response |
| `0x010D` | `CLIENT.LIST_WORKERS` | client -> Controller | request |
| `0x010E` | `CLIENT.LIST_WORKERS_RESPONSE` | Controller -> client | response |
| `0x0201` | `WORKER.REGISTER_WORKER` | Worker -> Controller | request |
| `0x0202` | `WORKER.REGISTER_ACK` | Controller -> Worker | response |
| `0x0203` | `WORKER.HEARTBEAT` | Worker -> Controller | event |
| `0x0204` | `WORKER.RUN_TASK` | Controller -> Worker | request |
| `0x0205` | `WORKER.RUN_TASK_ACK` | Worker -> Controller | response |
| `0x0206` | `WORKER.TASK_STARTED` | Worker -> Controller | event |
| `0x0207` | `WORKER.LOG_CHUNK` | Worker -> Controller | event |
| `0x0208` | `WORKER.LOG_WINDOW_UPDATE` | Controller -> Worker | event |
| `0x0209` | `WORKER.TASK_FINISHED` | Worker -> Controller | event |
| `0x020A` | `WORKER.CANCEL_TASK` | Controller -> Worker | request |
| `0x020B` | `WORKER.CANCEL_TASK_ACK` | Worker -> Controller | response |

IDs not listed are reserved. Renaming or reusing an ID requires protocol version 2.

## `request_id`

- A requester chooses a nonzero ID unique among its outstanding requests on that connection.
- The direct response copies the request ID exactly.
- Worker events (`HEARTBEAT`, `TASK_STARTED`, `LOG_CHUNK`, `LOG_WINDOW_UPDATE`, and
  `TASK_FINISHED`) use zero.
- `ERROR_RESPONSE` copies the offending client request ID when the header/request can be parsed.
- IDs are scoped to one TCP connection and are not idempotency keys.
- A client that loses a `SUBMIT_TASK_RESPONSE` cannot infer whether submission committed. v1 has
  no submission idempotency key; blindly resubmitting can create another Task.

## Common objects

A full Task object has this shape:

```json
{
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "state": "ACTIVE",
  "executable": "/usr/bin/python3",
  "args": ["demo.py"],
  "env": {"MODE": "test"},
  "cwd": "/srv/jobs",
  "resources": {"cpu_slots": 2, "memory_mb": 512},
  "timeout_ms": 30000,
  "cancel_requested_at": null,
  "created_at": "2026-09-26T08:15:30.123456Z",
  "updated_at": "2026-09-26T08:15:31.000000Z",
  "latest_attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71"
}
```

`cwd` and `timeout_ms` may be null. The submitted environment is a set of overrides applied to
the Worker environment. An Attempt object has this shape:

```json
{
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_no": 1,
  "state": "RUNNING",
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "assigned_at": "2026-09-26T08:15:31.000000Z",
  "started_at": "2026-09-26T08:15:31.050000Z",
  "finished_at": null,
  "exit_code": null,
  "term_signal": null,
  "reason": null,
  "stdout_bytes": 128,
  "stderr_bytes": 0,
  "logs_complete": false,
  "finalization_pending": false
}
```

`worker_id`, `session_id`, timestamps, result fields, and paths may be null where their lifecycle
does not supply them. `finalization_pending` is computed live diagnostic metadata, not a stored
Attempt state.

## Client protocol

### Submit

`CLIENT.SUBMIT_TASK` (`0x0101`):

```json
{
  "executable": "/usr/bin/python3",
  "args": ["demo.py"],
  "env": {"MODE": "test"},
  "cwd": "/srv/jobs",
  "resources": {"cpu_slots": 2, "memory_mb": 512},
  "timeout_ms": 30000
}
```

`args` excludes argv[0]; the Worker constructs argv with `executable` as argv[0]. `env`, `cwd`,
`resources`, and `timeout_ms` are optional. Defaults are empty overrides, null cwd, one CPU slot,
zero accounted memory, and no timeout. A supplied timeout is positive and subject to a configured
operational maximum.

`CLIENT.SUBMIT_TASK_RESPONSE` (`0x0102`), sent only after Task `PENDING` commits:

```json
{
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "state": "PENDING",
  "created_at": "2026-09-26T08:15:30.123456Z"
}
```

### Get task

`CLIENT.GET_TASK` (`0x0103`):

```json
{"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878"}
```

`CLIENT.GET_TASK_RESPONSE` (`0x0104`):

```json
{
  "task": {"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","state":"ACTIVE","executable":"/usr/bin/python3","args":["demo.py"],"env":{"MODE":"test"},"cwd":"/srv/jobs","resources":{"cpu_slots":2,"memory_mb":512},"timeout_ms":30000,"cancel_requested_at":null,"created_at":"2026-09-26T08:15:30.123456Z","updated_at":"2026-09-26T08:15:31.000000Z","latest_attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71"},
  "attempts": [{"attempt_id":"c38a0520-2172-44c4-aaf5-d5ab4392bc71","task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","attempt_no":1,"state":"RUNNING","worker_id":"worker-a","session_id":"ee3388af-a34f-45ef-910e-e7953e89951d","assigned_at":"2026-09-26T08:15:31.000000Z","started_at":"2026-09-26T08:15:31.050000Z","finished_at":null,"exit_code":null,"term_signal":null,"reason":null,"stdout_bytes":128,"stderr_bytes":0,"logs_complete":false,"finalization_pending":false}]
}
```

Attempts are ordered by `attempt_no` ascending.

### List tasks

`CLIENT.LIST_TASKS` (`0x0105`):

```json
{"page_size":100,"page_token":null}
```

Page size defaults to 100 and is capped at 200. The opaque token encodes the last
`(created_at, task_id)` cursor. Results are ordered newest first. Pagination is stable with
respect to the cursor but is not a multi-page database snapshot.

`CLIENT.LIST_TASKS_RESPONSE` (`0x0106`):

```json
{
  "tasks": [
    {"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878","state":"ACTIVE","executable":"/usr/bin/python3","attempt_count":1,"created_at":"2026-09-26T08:15:30.123456Z","updated_at":"2026-09-26T08:15:31.000000Z"}
  ],
  "next_page_token": "opaque-or-null"
}
```

`taskctl list` automatically requests pages until the token is null.

### Get logs

`CLIENT.GET_LOGS` (`0x0107`) addresses one stream:

```json
{
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_no": 1,
  "stream": "stdout",
  "offset": 0,
  "limit_bytes": 262144,
  "snapshot_end": null
}
```

`attempt_no` may be null to select the latest Attempt. `stream` is `stdout` or `stderr`.
`limit_bytes` defaults to 256 KiB and is capped so the base64 response remains under 4 MiB. On
the first request, null `snapshot_end` captures the currently completed LogWriter byte count.
Later pages repeat that returned value.

`CLIENT.GET_LOGS_RESPONSE` (`0x0108`):

```json
{
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_no": 1,
  "stream": "stdout",
  "offset": 0,
  "next_offset": 6,
  "snapshot_end": 6,
  "data_base64": "aGVsbG8K",
  "eof_snapshot": true,
  "logs_complete": false
}
```

The snapshot contains bytes for which LogWriter `write` completion has been reported. For an
Attempt that could execute, terminal `logs_complete=true` guarantees its final barrier completed
both file `fdatasync`s and every required containing-directory `fsync` for newly created log
namespace entries before terminal metadata commit. `taskctl logs --both` reads each stream
separately.
The command does not follow later output in v1. For an explicit dispatch rejection or a
pre-dispatch local cancellation, zero-byte/zero-sequence metadata instead proves that execution
never produced logs; `GET_LOGS` returns an empty complete snapshot even if no physical zero-length
file was created.

### Cancel

`CLIENT.CANCEL_TASK` (`0x0109`):

```json
{"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878"}
```

`CLIENT.CANCEL_TASK_RESPONSE` (`0x010A`):

```json
{
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "status": "accepted",
  "task_state": "ACTIVE",
  "cancel_requested_at": "2026-09-26T08:16:00.000000Z"
}
```

`status` is `completed` for a pending Task now committed `CANCELLED`, `accepted` for an active
Task (including an idempotent repeat while cancellation is pending), or `already_cancelled`.
Cancelling `SUCCEEDED` or `FAILED` returns `ERROR_RESPONSE` with `CONFLICT`.

### Retry

`CLIENT.RETRY_TASK` (`0x010B`):

```json
{"task_id":"8e7f5e0c-11fe-4dc4-8437-fcab1b332878"}
```

`CLIENT.RETRY_TASK_RESPONSE` (`0x010C`), after `FAILED -> PENDING` commits:

```json
{
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "state": "PENDING",
  "next_attempt_no": 2
}
```

Only `FAILED` is retryable in v1.

### List Workers

`CLIENT.LIST_WORKERS` (`0x010D`):

```json
{}
```

`CLIENT.LIST_WORKERS_RESPONSE` (`0x010E`):

```json
{
  "workers": [
    {
      "worker_id": "worker-a",
      "state": "HEALTHY",
      "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
      "capacity": {"cpu_slots": 8, "memory_mb": 16384},
      "reserved": {"cpu_slots": 2, "memory_mb": 512},
      "running_attempts": 1,
      "max_concurrent_tasks": 8,
      "last_heartbeat_at": "2026-09-26T08:16:02.000000Z"
    }
  ]
}
```

This is current in-memory Controller state, not persistent Worker history.

### Error response

`ERROR_RESPONSE` (`0x0001`):

```json
{
  "code": "INVALID_ARGUMENT",
  "message": "cpu_slots must be at least 1",
  "retryable": false,
  "details": {}
}
```

Stable v1 codes are `INVALID_ARGUMENT`, `NOT_FOUND`, `CONFLICT`, `UNSUPPORTED`,
`TEMPORARILY_UNAVAILABLE`, and `INTERNAL`. Messages are diagnostic and not machine contracts.

## Worker protocol

### Register

`WORKER.REGISTER_WORKER` (`0x0201`) must be the first frame:

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "capacity": {"cpu_slots": 8, "memory_mb": 16384},
  "max_concurrent_tasks": 8,
  "worker_version": "1.0.0"
}
```

The Worker generates a fresh `session_id` for every connection attempt that reaches
registration. `max_concurrent_tasks` may be null to disable a separate count limit. Before
accepting a replacement, Controller durably marks the old session's nonterminal Attempts `LOST`,
then closes the old connection.

`WORKER.REGISTER_ACK` (`0x0202`):

```json
{
  "accepted": true,
  "heartbeat_interval_ms": 3000,
  "offline_timeout_ms": 10000,
  "termination_grace_ms": 2000,
  "initial_log_window_bytes": 1048576,
  "initial_log_window_chunks": 64,
  "controller_instance_id": "8d2743fa-e5d1-4036-8731-bc377c181181"
}
```

For rejection, `accepted` is false and fields `error_code` and `message` replace runtime
settings. The Controller instance ID is diagnostic only; it is not a reconciliation epoch.

### Heartbeat

`WORKER.HEARTBEAT` (`0x0203`):

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "sequence": 17,
  "running_attempt_ids": ["c38a0520-2172-44c4-aaf5-d5ab4392bc71"]
}
```

Sequence starts at 1 per session. Running IDs are diagnostic; Controller does not reconcile or
adopt work from them. Only a role-valid heartbeat for the current session advances liveness.

### Run task

`WORKER.RUN_TASK` (`0x0204`), sent only after assignment persistence commits:

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "attempt_no": 1,
  "executable": "/usr/bin/python3",
  "args": ["demo.py"],
  "env": {"MODE": "test"},
  "cwd": "/srv/jobs",
  "resources": {"cpu_slots": 2, "memory_mb": 512},
  "timeout_ms": 30000
}
```

`WORKER.RUN_TASK_ACK` (`0x0205`):

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "accepted": true,
  "error_code": null,
  "message": null
}
```

Accepted means the Worker created local lifecycle ownership and will attempt launch; it does not
mean `fork` or `exec` succeeded. An explicit rejection proves launch did not occur and maps to
`FAILED/DISPATCH_REJECTED`. Missing ACK or connection uncertainty maps to `LOST`; it never causes
automatic retry. The default ACK deadline is 5 seconds and the default start-report deadline is
10 seconds, both configurable. The Worker does not begin launch unless it has successfully queued
the accepted ACK in its bounded control-output path; inability to queue that ACK is a session
failure and no new workload is launched. After queuing accepted ACK, Worker performs the complete
parent-side `PreparedExec` construction (merged environment, cwd/stdin resources, argv/envp, and
PATH resolution) before fork. Preparation failure produces `TASK_FINISHED/LAUNCH_FAILED` and the
Attempt never reaches `RUNNING`.

### Started

`WORKER.TASK_STARTED` (`0x0206`):

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "pid": 24130
}
```

The Worker emits this only after the close-on-exec status pipe proves successful `exec`. It is
queued before any log chunk for this Attempt.

### Log chunk

`WORKER.LOG_CHUNK` (`0x0207`):

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "stream": "stdout",
  "sequence": 0,
  "raw_size": 6,
  "data_base64": "aGVsbG8K"
}
```

Arbitrary output is base64 encoded. Raw decoded data is 1..65,536 bytes; EOF produces no empty
chunk. Sequence numbers
start at zero independently for stdout and stderr, with no gaps. `raw_size` must equal decoded
length and consumes that many bytes of the session's granted log window.

`WORKER.LOG_WINDOW_UPDATE` (`0x0208`):

```json
{
  "grant_bytes": 65536,
  "grant_chunks": 1
}
```

Controller grants only capacity reserved in its bounded log path and replenishes it after
LogWriter reports corresponding writes complete. Credit is two-dimensional so small chunks cannot
exhaust an independently bounded command queue: each `LOG_CHUNK` consumes `raw_size` byte credit
and one chunk-slot credit. Granting first moves both byte and command-slot capacity from Controller
free LogQueue capacity into an outstanding reservation for that session. A valid chunk transfers
those reservations to its queued/in-flight LogWriter command; it does not compete for new
capacity. Capacity becomes free only after LogWriter completion or when unused session credit is
reclaimed. Controller must never over-grant either dimension across sessions.

`grant_bytes` and `grant_chunks` are nonnegative integers and at least one must be positive; Worker
accumulates both counters. Registration supplies the initial values using the same semantics.

Worker must never send unless it has sufficient raw-byte credit and at least one chunk slot. When
either prevents the next chunk, it disables stdout/stderr pipe read interest but continues
processing socket, signal, and timer events. Controller continues
reading the Worker socket during normal application backpressure because every correctly credited
chunk already has admission capacity. Inability to admit such a chunk is an internal Controller
invariant failure, not normal backpressure and not a reason to suspend socket reads.

Credit is consumed at Worker chunk generation/enqueue time, not at socket-send completion. Worker
never reads more raw pipe bytes than its current byte credit and requires a slot before reading a
new chunk. This bounds its unsent log-frame queue as well as the reserved Controller path.

Control frames use a separate bounded priority queue and may be selected before queued log frames
at frame boundaries where this does not violate an Attempt ordering dependency. An already
partially sent frame is always completed first. Thus sustained log pressure eventually blocks the
workload's writes instead of dropping bytes or intentionally hiding heartbeats.

### Finished

`WORKER.TASK_FINISHED` (`0x0209`):

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "outcome": "EXITED",
  "exit_code": 0,
  "term_signal": null,
  "reason": null,
  "stdout_next_sequence": 1,
  "stderr_next_sequence": 0,
  "stdout_bytes": 6,
  "stderr_bytes": 0
}
```

`outcome` is one of `EXITED`, `SIGNALED`, `TIMED_OUT`, `CANCELLED`, or `LAUNCH_FAILED`.
`EXITED` requires an exit code; `SIGNALED` requires `term_signal`; the other result fields are
nullable as appropriate. Final sequence values are one past the last chunk.

The direct child is the workload leader. On its natural exit, Worker records its exit/signal
outcome. If members remain in the same process group, Worker performs SIGTERM, grace, then SIGKILL
cleanup without changing that recorded leader outcome. `outcome`, `exit_code`, and `term_signal`
describe the leader (or a previously installed cancel/timeout cause), never the cleanup signals.
Worker continues pipe draining until both EOFs.

`TASK_FINISHED` is a per-Attempt wire-ordering barrier. When Worker generates it, it captures the
last previously generated stdout/stderr `LOG_CHUNK` ordinal for that Attempt. The output scheduler
must not pass the first `TASK_FINISHED` byte to `send` until the last byte of every such earlier
chunk has been accepted by the socket in wire order. This rule applies even if those chunks are
queued, another control frame is eligible for priority, or a chunk has only been partially sent.
Logs for another Attempt do not constrain this barrier. Worker therefore emits `TASK_FINISHED`
only after leader/group cleanup, both pipe EOFs, generation of all chunks, and completion of this
wire dependency. TCP order then guarantees Controller receives those chunks before finish.

On receipt, Controller's already accepted per-stream sequences and byte totals must exactly equal
the declared final values. A mismatch is a Worker protocol violation; Controller does not wait for
post-finish chunks. Receipt is still not terminal visibility: Controller waits for already accepted
writes to complete, executes the LogWriter file-data and directory-namespace durability barrier,
commits terminal metadata, then releases resources and exposes the result.

### Cancel active Attempt

`WORKER.CANCEL_TASK` (`0x020A`), sent only after cancellation intent persists:

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "task_id": "8e7f5e0c-11fe-4dc4-8437-fcab1b332878",
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "grace_ms": 2000
}
```

`WORKER.CANCEL_TASK_ACK` (`0x020B`):

```json
{
  "worker_id": "worker-a",
  "session_id": "ee3388af-a34f-45ef-910e-e7953e89951d",
  "attempt_id": "c38a0520-2172-44c4-aaf5-d5ab4392bc71",
  "accepted": true,
  "already_terminal": false,
  "message": null
}
```

The ACK confirms command handling, not cancellation completion. `TASK_FINISHED` carries the
observed result. If natural completion wins before the cancel cause is installed, its natural
result remains authoritative.

## Request/response matrix

| Request | Direct response | Later events |
|---|---|---|
| `CLIENT.SUBMIT_TASK` | `CLIENT.SUBMIT_TASK_RESPONSE` or `ERROR_RESPONSE` | none on client connection |
| `CLIENT.GET_TASK` | `CLIENT.GET_TASK_RESPONSE` or `ERROR_RESPONSE` | none |
| `CLIENT.LIST_TASKS` | `CLIENT.LIST_TASKS_RESPONSE` or `ERROR_RESPONSE` | none |
| `CLIENT.GET_LOGS` | `CLIENT.GET_LOGS_RESPONSE` or `ERROR_RESPONSE` | none |
| `CLIENT.CANCEL_TASK` | `CLIENT.CANCEL_TASK_RESPONSE` or `ERROR_RESPONSE` | query status for completion |
| `CLIENT.RETRY_TASK` | `CLIENT.RETRY_TASK_RESPONSE` or `ERROR_RESPONSE` | none |
| `CLIENT.LIST_WORKERS` | `CLIENT.LIST_WORKERS_RESPONSE` or `ERROR_RESPONSE` | none |
| `WORKER.REGISTER_WORKER` | `WORKER.REGISTER_ACK` | heartbeat/commands/events |
| `WORKER.RUN_TASK` | `WORKER.RUN_TASK_ACK` | `TASK_STARTED`, `LOG_CHUNK`, `TASK_FINISHED` |
| `WORKER.CANCEL_TASK` | `WORKER.CANCEL_TASK_ACK` | `TASK_FINISHED` |

## Frame parsing and non-blocking I/O

### Receive

1. On readable readiness, call `recv` repeatedly until `EAGAIN/EWOULDBLOCK`, peer close, or a
   fatal error.
2. Append into a bounded input buffer; never allocate from an unvalidated payload length.
3. When at least 20 bytes exist, decode header fields individually.
4. Validate magic, version, type, role, and payload length.
5. Wait until exactly `20 + payload_length` bytes are buffered.
6. Parse one JSON object, validate its complete schema, dispatch it, remove it, and repeat so one
   read can yield multiple frames.
7. A correctly credited log frame transfers its pre-reserved capacity and must enter LogQueue
   immediately. Admission failure is a fatal internal invariant violation handled by Controller
   fail-stop policy; normal log backpressure never suspends Worker socket reads.

### Send

Frames are encoded once into owned buffers. `send` advances an offset until complete or
`EAGAIN`. Unsent bytes remain owned by `TcpConnection`. `EPOLLOUT` is enabled only while output
exists and removed immediately when drained. A partial frame cannot be interleaved with another
frame. The scheduler may bypass queued log frames with unrelated heartbeat/ACK/control traffic,
but a `TASK_FINISHED` frame is ineligible while any earlier same-Attempt log frame is queued or
partially sent. Control and log queue sizes have configurable hard limits; breach invalidates the
session rather than allocating without bound.

## Malformed and failure handling

| Condition | Behavior |
|---|---|
| Invalid magic, unsupported version, or payload length over 4 MiB | Close immediately without payload allocation or resynchronization. |
| Unknown message type or wrong connection role | Client: send `ERROR_RESPONSE/UNSUPPORTED` if safe, then close. Worker: close and invalidate session. |
| Malformed UTF-8/JSON or schema/type error | Client semantic request errors receive `INVALID_ARGUMENT`; syntactically malformed frames close. Worker errors close/invalidate. |
| Log sequence gap, bad base64, byte-count mismatch, or credit violation | Protocol violation; close Worker session and apply `LOST`. |
| Stale/incorrect `session_id` | Close the sending Worker connection; it cannot mutate state. |
| Peer orderly close, reset, timeout, or fatal send/recv error | Destroy connection through ConnectionManager; Worker role also invalidates session and marks bound attempts `LOST`. |
| `EINTR` | Retry the syscall where appropriate. |
| `EAGAIN/EWOULDBLOCK` | Preserve state and await readiness. |

There is no byte-stream resynchronization after a corrupt header. This avoids interpreting
attacker- or fault-controlled payload bytes as a new frame, even though v1 assumes a trusted
network.

## Size and buffering limits

| Item | v1 value/contract |
|---|---|
| Header | 20 bytes |
| JSON payload | maximum 4 MiB |
| Raw log chunk | maximum 64 KiB |
| Client log page | default 256 KiB raw; capped below frame maximum after base64 |
| Task list page | default 100, maximum 200 |
| Per-connection input/output high-water marks | Configured finite values, at least one maximum frame; exact tuning is deployment configuration |
| LogQueue | Independently bounded by raw bytes and command slots; both are pre-reserved by log credit |
| PersistenceQueue | Bounded by command count and owned metadata bytes as configured |

No configured limit may silently change the wire maximum or permit unbounded memory growth.
