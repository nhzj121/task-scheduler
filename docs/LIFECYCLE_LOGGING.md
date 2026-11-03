# Lifecycle diagnostics — first batch

Controller and Worker emit English, single-line lifecycle events at INFO through
their existing spdlog stderr/journald configuration. Set logging.level to warn,
error or off to suppress these INFO events. No taskctl option or wire message was
added. Logs are best-effort observations, not a durable event stream or audit log.

## Event meanings

| Owner | Event | Observation boundary |
| --- | --- | --- |
| Controller | TASK_ACCEPTED | Matching Submit commit applied as durable PENDING |
| Controller | TASK_ASSIGNED | Matching Assign commit and memory reservation commit applied |
| Controller | ATTEMPT_RUNNING | Matching RUNNING Advance commit applied; Task remains ACTIVE |
| Controller | TASK_TERMINAL | Matching terminal/cancel/bulk-LOST commit applied, required memory reservation release finished |
| Worker | EXEC_STARTED | Runtime confirmed exec success; includes real leader PID |
| Worker | EXEC_FINISHED | Local runtime completion after reap/group cleanup/output drain, or parent-side launch preparation failed |

Parent preparation failure has no PID. A child-side exec failure may have a real
PID but never produces EXEC_STARTED. Worker outcome names reuse the existing
FINISH mapping: EXITED, SIGNALED, TIMED_OUT, CANCELLED, LAUNCH_FAILED. EXITED alone
does not mean success: inspect exit_code. A local finish on a lost Session is still
observable, but does not claim FINISH transmission, complete durable logs or a
Controller terminal commit. Internal runtime invariants remain explicit errors;
diagnostics do not invent a protocol outcome for them.

Controller terminal events include LOST/FAILED and logs_complete=false when
appropriate. PENDING cancellation has no current Attempt: omit Attempt/Worker/
Session fields and use reservation=none, including cancellation after manual retry.
An existing terminal Attempt is not logged again by subsequent bulk Session loss.
Startup restore/recovery does not replay lifecycle events.

## Fields and correlation

Every line contains event and full task_id. When an Attempt exists, include full
attempt_id, attempt_no, worker_id and session_id. Controller adds task_state and
task_version, plus attempt_state/attempt_version when applicable. Assignment
includes cpu_slots/memory_mb. Terminal includes reservation=released or none and,
when an Attempt exists, logs_complete=true or false. Worker includes pid when
known, executable on STARTED, and outcome/exit_code/term_signal on FINISHED when
available. No argv, environment, failure text or task-output payload is copied.

```text
event=TASK_ASSIGNED task_id=<UUID> task_state=ACTIVE task_version=1 attempt_id=<UUID> attempt_no=1 worker_id="worker-a" session_id=<UUID> attempt_state=ASSIGNED attempt_version=0 cpu_slots=1 memory_mb=16
event=EXEC_STARTED task_id=<UUID> attempt_id=<UUID> attempt_no=1 worker_id="worker-a" session_id=<UUID> pid=4821 executable="/bin/sleep"
event=TASK_TERMINAL task_id=<UUID> task_state=SUCCEEDED task_version=2 attempt_id=<UUID> attempt_no=1 worker_id="worker-a" session_id=<UUID> attempt_state=SUCCEEDED attempt_version=3 logs_complete=true reservation=released
```

The actual lines retain the logger's timestamp/level prefix. Free text is quoted;
quotes/backslashes are escaped, bytes outside printable ASCII use explicit hex
escapes, and each scalar is limited to its first 256 source bytes with a trailing
ellipsis on truncation. Canonical UUID fields remain complete. This is a diagnostic
field format, not JSON or a new protocol. Use IDs to correlate processes; do not
infer a global ordering from timestamps or confuse attempt_no with Attempt UUID.

## Ownership and operational limits

TaskManager and ExecutionManager accept optional, default-empty observer callbacks.
They receive value snapshots synchronously on the owner thread and must not
reenter or make business decisions. Applications alone format/spdlog these
events; TaskManager has no spdlog dependency. No observer event is persisted,
queued to a background thread or introduced as a business Effect.

Snapshot construction, observer invocation and application formatting exceptions
are isolated from business decisions. This isolation does not make synchronous
stderr nonblocking: a slow terminal/pipe/journald consumer can delay the owner
thread. Keep INFO limited to these low-frequency events. No per-heartbeat,
per-LOG_CHUNK or per-GET polling logging was added. These changes provide neither
exactly-once diagnostic delivery nor a performance guarantee.

Use a seconds-long workload for a terminal demonstration; inspect task output via
taskctl logs. No sleeps or display synchronization were added to production paths.
The process runtime, child-after-fork path, protocol, schema, scheduling, durable
ordering and log-credit/data path remain unchanged.

## Validation

New CTest cases carry the observability label. Component tests use real production
decision/runtime logic and controlled ports; integration tests use actual binaries,
loopback TCP, SQLite readers and the existing M8 file/transaction gates.

```bash
ctest --test-dir BUILD -L observability --output-on-failure
ctest --test-dir BUILD -L observability --output-on-failure --repeat until-fail:20
```

See the [first-batch implementation report](reports/LIFECYCLE_OBSERVABILITY_REPORT.md) for the executed build commands,
complete regression inventory, sanitizer scope and results.
