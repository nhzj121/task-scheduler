# Deployment Intent

This is the architecture-phase deployment contract. No configuration files, scripts, binaries,
or systemd units are claimed to exist yet.

## Supported topology

- Linux hosts with one Controller process and zero or more Worker processes.
- `taskctl` may run on the Controller host or another trusted private-network host.
- Default development topology runs every process on one machine over loopback.
- Multi-host deployment requires routable trusted private addresses and firewall rules limited to
  known hosts.

There is no TLS or authentication. Public Internet exposure, untrusted clients, shared
multi-tenant hosts, and privilege-bearing Worker service accounts are unsupported and unsafe.

## Example network defaults

| Listener | Example default | Purpose |
|---|---|---|
| Controller client | `127.0.0.1:7420` | `taskctl` request/response |
| Controller Worker | `127.0.0.1:7421` | persistent Worker control/log session |

Ports and bind addresses are configurable. Separate listeners give each accepted connection a
fixed protocol role. Multi-host operators explicitly change the Worker listener to a trusted
private interface; they should not use `0.0.0.0` without firewalling. One Worker uses one TCP
connection per session; log credit keeps the shared connection bounded.

## Intended paths

A conventional packaged deployment may use:

```text
/etc/task-scheduler/controller.json
/etc/task-scheduler/worker.json
/var/lib/task-scheduler/controller/state.db
/var/lib/task-scheduler/controller/tasks/<task-id>/attempt-<n>.stdout.log
/var/lib/task-scheduler/controller/tasks/<task-id>/attempt-<n>.stderr.log
/var/log/task-scheduler/                 # optional spdlog files; journald is also suitable
```

Development configs may use repository-local or temporary paths, but tests always use isolated
temporary directories. The Controller must canonicalize and own its data root, reject unsafe
paths, create files with restrictive permissions, and acquire a process lock so two Controllers
cannot write the same SQLite/log directory.

Task IDs and attempt numbers are generated/validated values, not raw user path components.

The configured log root must reside on a trusted local Linux filesystem that supports file
`fdatasync` and directory `fsync`. `LogWriterThread` alone creates task-log namespace entries and
synchronizes newly dirtied containing directories before reporting a successful final barrier.
Network filesystems and storage that do not honor those sync semantics are outside v1 support.

## Controller configuration model

The eventual `controller.json` should contain these groups; exact JSON spelling must be frozen
with implementation config tests:

```json
{
  "client_listen": {"address": "127.0.0.1", "port": 7420},
  "worker_listen": {"address": "127.0.0.1", "port": 7421},
  "data_dir": "/var/lib/task-scheduler/controller",
  "heartbeat": {"interval_ms": 3000, "offline_timeout_ms": 10000},
  "dispatch": {"ack_timeout_ms": 5000, "start_timeout_ms": 10000},
  "termination_grace_ms": 2000,
  "protocol": {"max_payload_bytes": 4194304},
  "queues": {
    "persistence_commands": 4096,
    "log_bytes": 67108864,
    "log_commands": 4096,
    "log_control_reserve": 128,
    "initial_session_log_window_bytes": 1048576,
    "initial_session_log_window_chunks": 64
  },
  "logging": {"level": "info", "destination": "journald"}
}
```

Queue numbers are example tuning values, not extra wire guarantees. Validation enforces positive
bounds, at least one maximum frame where needed, log byte/chunk credit no larger than its grantable
capacity after the control reserve, and `offline_timeout_ms` comfortably greater than heartbeat
interval.

Controller opens SQLite with foreign keys enabled, WAL journaling, `synchronous=FULL`, and a
bounded busy timeout. Failure beyond the bounded policy is explicit fail-stop, not indefinite
EventLoop blocking. Schema migration is versioned; v1 supports initialization of an empty DB and
the exact v1 schema, not arbitrary downgrade.

## Worker configuration model

The eventual `worker.json` should contain:

```json
{
  "worker_id": "worker-a",
  "controller": {"address": "127.0.0.1", "port": 7421},
  "capacity": {"cpu_slots": 8, "memory_mb": 16384},
  "max_concurrent_tasks": 8,
  "reconnect": {"initial_backoff_ms": 250, "max_backoff_ms": 30000},
  "logging": {"level": "info", "destination": "journald"}
}
```

`worker_id` must be unique within the Controller's trust domain and stable for that logical
Worker. Each connection registration still uses a fresh runtime `session_id`. Advertised capacity
is accounting input only: v1 does not discover or enforce CPU/memory limits with cgroups.
`max_concurrent_tasks` is optional/null when no separate count cap is wanted.

The Worker service environment becomes the base environment of submitted executables, with task
overrides applied. Operators must keep PATH and other inherited values controlled. The Worker
parent resolves no-slash executables against that final PATH before fork; missing PATH is a launch
preparation failure. The Worker should run as a dedicated unprivileged account with access only to
intended executables and cwd paths; this reduces impact but is not a sandbox.

## Startup order

1. Controller validates config, creates/locks data directory, opens/migrates SQLite, and performs
   startup recovery before opening listeners.
2. Controller starts PersistenceThread, LogWriterThread, and EventLoop with completion channels.
3. Workers connect and register fresh sessions; they retry with bounded exponential backoff.
4. `taskctl` connects per command.

Workers may start before Controller; they remain disconnected and execute no tasks until
registration succeeds.

## Shutdown and restart

### Controller graceful stop

Stop listeners and scheduling, logically invalidate Worker sessions and stop their traffic,
durably mark their active Attempts `LOST` where storage is healthy, then close connections so
Workers terminate workloads, drain bounded storage completions, join LogWriter/PersistenceThread,
and exit. systemd `TimeoutStopSec` must exceed workload termination grace plus storage cleanup
margin; it is still bounded.

### Controller crash/restart

Workers terminate workloads after detecting connection loss. The restarted Controller marks all
persisted nonterminal Attempts `LOST` and Tasks `FAILED`, leaves `PENDING` queued, and accepts only
fresh Worker sessions. It never adopts old processes.

### Worker graceful stop

Stop heartbeat/reconnect, terminate every process group with the configured grace, reap/drain,
close the session, then exit. A crash is detected by Controller connection/heartbeat failure.

## systemd intent

Later implementation provides `task-controller.service` and `task-worker.service` with:

- dedicated non-root service users;
- explicit config path arguments;
- `Restart=on-failure` with bounded delay;
- appropriate state/log directory directives and restrictive umask;
- high enough `LimitNOFILE` for configured Workers/tasks, but not unlimited by default;
- signal-based graceful stop and a bounded stop timeout;
- no false sandbox claim: hardening directives must be compatible with executing configured
  workloads and accessing their cwd/executables.

Worker units may declare network ordering but must not require Controller to be on the same host.
Controller restart can fail running Tasks by design, so automatic service restart improves
availability but does not preserve execution continuity.

## Filesystem capacity and operations

Task logs are lossless and can consume substantial disk. v1 has no retention, quota, compression,
or external log sink. Operators must monitor free space and manually archive/remove data only
while Controller is stopped and with metadata consistency understood. Disk-full is an execution
infrastructure failure, not a reason to discard output.

Directory synchronization is performed when creation dirties the log namespace and is required by
the later final barrier; it is not performed for every log chunk. A directory-sync error is a log
persistence failure and prevents `logs_complete=true`.

Backups must capture SQLite and task log files consistently. v1 does not ship online backup
coordination; the simple supported procedure is stop Controller, copy the locked data directory,
then restart. Restoring a partial SQLite/log set can yield `logs_complete=false` or missing files
and is not repaired automatically.

## System and task logs

spdlog system logs include instance/session/task/attempt IDs, state transitions, persistence
failures, network errors, scheduling decisions, and process failures without dumping arbitrary
task environment values or output. Workload stdout/stderr remain task-log data, not system logs.

Log rotation of system logs is a deployment concern. Task logs are not rotated while an Attempt
is active because byte offsets and completion barriers refer to fixed files.

## Deployment validation

Before calling a deployment usable:

- verify both listeners are limited to the intended trust boundary;
- verify service users and data permissions;
- verify unique Worker IDs and realistic accounting capacities;
- run one success, nonzero exit, cancel, timeout, Worker-kill, reconnect, and Controller-restart
  scenario;
- verify SQLite recovery and final logs after restart;
- verify disk/fd capacity and configured queue bounds under a high-log test;
- display the no-auth/no-TLS/no-sandbox warning in operator documentation.
