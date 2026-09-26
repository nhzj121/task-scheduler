# Architecture Decisions

This file records the decisions that freeze v1 semantics. The specification files under
`docs/spec/` remain authoritative. Status `Accepted` means accepted for the architecture
baseline; production implementation still requires explicit human approval.

## ADR-001 — C++17 and Linux-only runtime

- **Status:** Accepted
- **Decision:** Use C++17, GCC 11 or newer, and Linux APIs directly.
- **Rationale:** This matches the learning goals and permits explicit use of `epoll`,
  `signalfd`, `timerfd`, `eventfd`, `fork`, and `exec`.
- **Rejected:** A portable abstraction layer; a newer language standard without a concrete
  need.
- **Consequences:** The worker and controller are not portable to non-Linux systems.
- **Revisit when:** Portability becomes a stated product goal.

## ADR-002 — One Controller, multiple Workers

- **Status:** Accepted
- **Decision:** v1 has one authoritative Controller process and any configured number of
  Workers.
- **Rationale:** It keeps ownership and recovery semantics understandable within the target
  project size.
- **Rejected:** Active/standby controllers, consensus, leader election.
- **Consequences:** The Controller is a single point of failure and availability.
- **Revisit when:** Controller high availability enters scope.

## ADR-003 — Epoll Level Triggered reactor

- **Status:** Accepted
- **Decision:** Controller and Worker each use one `epoll` Level Triggered EventLoop thread.
  Ready non-blocking descriptors are drained until `EAGAIN` where applicable.
- **Rationale:** LT is easier to reason about while still exposing the relevant Linux I/O
  mechanics.
- **Rejected:** Edge Triggered epoll, Boost.Asio, a thread per connection.
- **Consequences:** Callbacks must remain short and must never perform blocking socket, pipe,
  database, or task-log file I/O.
- **Revisit when:** Measured scale requires a different I/O architecture.

## ADR-004 — Custom 20-byte framing with JSON payloads

- **Status:** Accepted
- **Decision:** Use an explicitly encoded 20-byte binary header in network byte order and a
  bounded JSON payload. Protocol version is 1 and magic is `0x54534348` (`TSCH`).
- **Rationale:** The design teaches TCP framing and remains inspectable.
- **Rejected:** Raw C++ structs, line-delimited JSON, gRPC, Protobuf.
- **Consequences:** Framing, validation, 64-bit endian conversion, and schemas are maintained
  by this project.
- **Revisit when:** Interoperability or schema evolution outweighs the educational goal.

## ADR-005 — SQLite metadata store with one owner thread

- **Status:** Accepted
- **Decision:** A dedicated `PersistenceThread`, reached through a bounded
  `PersistenceQueue`, exclusively owns the SQLite connection. State changes for one logical
  operation use one SQLite transaction.
- **Rationale:** This removes cross-thread SQLite ownership and keeps the EventLoop
  non-blocking.
- **Rejected:** SQLite access on the EventLoop, a database connection pool, an external DB.
- **Consequences:** Metadata write throughput is serialized. Completion records are returned
  by ID and generation through an EventLoop wakeup, never by callbacks holding raw domain
  pointers.
- **Revisit when:** Measurements show the single metadata writer is the bottleneck.

## ADR-006 — Durable transition before irreversible side effect

- **Status:** Accepted; explicitly approved by the project owner
- **Decision:** A required durable state transition must commit before its corresponding
  irreversible external side effect:
  - submit commits `PENDING` before success is returned;
  - scheduling commits Task `ACTIVE`, Attempt `ASSIGNED`, and worker/session binding before
    any `RUN_TASK` byte is queued;
  - an active cancellation request commits `cancel_requested_at` before `WORKER.CANCEL_TASK`
    is queued;
  - a terminal SQLite transaction atomically commits Task and Attempt terminal state; after
    commit, the same EventLoop logical transition releases the in-memory resource reservation,
    then and only then exposes the terminal state.
- **Rationale:** Recovery never depends on an external action for which no durable intent
  exists.
- **Rejected:** Fire-and-forget dispatch followed by best-effort persistence.
- **Consequences:** Database latency is on the control path. In-memory resource accounting is
  not part of a cross-medium ACID transaction. A tentative reservation prevents concurrent
  overcommit while an assignment transaction is pending and is rolled back on failure.
- **Revisit when:** A replicated transactional control plane is introduced.

## ADR-007 — Task and Attempt are separate

- **Status:** Accepted
- **Decision:** A Task is logical user intent; every concrete execution is an immutable-history
  Attempt with its own state and identity.
- **Rationale:** Retry, loss, and duplicate-execution uncertainty cannot be represented safely
  by one state object.
- **Rejected:** Resetting a single Task execution record on retry.
- **Consequences:** Queries and storage retain all Attempts. At most one Attempt for a Task is
  nonterminal.
- **Revisit when:** Never; future retry policy still requires this separation.

## ADR-008 — Manual retry only; no exactly-once claim

- **Status:** Accepted
- **Decision:** Only a `FAILED` Task may be manually retried. Retry changes it to `PENDING`;
  later scheduling creates a new Attempt. There is no automatic distributed retry and no
  exactly-once guarantee.
- **Rationale:** After a connection or Controller failure, remote execution may have happened
  even when its result is unknown.
- **Rejected:** Silent automatic retry and claims of exactly-once execution.
- **Consequences:** A user retry can overlap with an old process during a failure-detection
  window. Workloads should be idempotent when this matters.
- **Revisit when:** Idempotency keys or a reconciliation protocol are in scope.

## ADR-009 — Worker identity, session, and connection are distinct

- **Status:** Accepted; replacement behavior explicitly approved by the project owner
- **Decision:** `worker_id` is stable configuration identity; `session_id` is a UUID for one
  registered runtime connection; `TcpConnection` is transport lifetime. A new session for the
  same worker logically invalidates the old session and stops new traffic. The old session's
  nonterminal Attempts become durably `LOST`, then the old connection is closed, before the new
  session receives `REGISTER_ACK`.
- **Rationale:** A TCP reconnect cannot prove continuity of process state.
- **Rejected:** Reusing a session across reconnect and dual active connections.
- **Consequences:** Reconnect is deliberately destructive to old scheduling state.
- **Revisit when:** Worker reconciliation is designed.

## ADR-010 — Worker kills workloads after control-session loss

- **Status:** Accepted; explicitly approved by the project owner
- **Decision:** When the Worker detects loss of its Controller connection, it stops accepting
  work and explicitly terminates every local nonterminal process group with
  `SIGTERM -> grace -> SIGKILL`, reaps children, and only then registers a new session.
- **Rationale:** This bounds orphan workload lifetime after the Controller has declared Attempts
  `LOST`.
- **Rejected:** Letting workloads run without a controlling session; reconnect reconciliation.
- **Consequences:** A transient network failure terminates work. There remains a finite
  duplicate-execution window before TCP failure is detected and termination completes.
- **Revisit when:** Disconnected execution and reconciliation enter scope.

## ADR-011 — Active cancellation is accepted, not immediately completed

- **Status:** Accepted; explicitly approved by the project owner
- **Decision:** Cancelling `PENDING` completes immediately without an Attempt. Cancelling an
  active Task durably records `cancel_requested_at` and returns `accepted`; the Task remains
  `ACTIVE` until the Worker reports the concrete result. A Worker/session loss still produces
  Attempt `LOST` and Task `FAILED`. A natural terminal result that wins the EventLoop ordering
  race is not rewritten as `CANCELLED`.
- **Rationale:** Sending a signal and observing process termination are different facts.
- **Rejected:** Marking an active Task `CANCELLED` as soon as a command is sent.
- **Consequences:** Cancellation is best effort and observable as an asynchronous operation.
- **Revisit when:** A richer `CANCELLING` Task state is approved.

## ADR-012 — Resource accounting, not enforcement

- **Status:** Accepted
- **Decision:** The Controller reserves advertised CPU slots, memory MB, and optional task
  concurrency in memory. It does not use cgroups or measure actual consumption.
- **Rationale:** This is sufficient for placement study without adding an isolation subsystem.
- **Rejected:** cgroups, preemption, oversubscription policy.
- **Consequences:** A task may consume more resources than requested. Accounting reconstructs
  from no active reservations after Controller restart because old active Attempts become
  `LOST`.
- **Revisit when:** Hard isolation or utilization-aware scheduling enters scope.

## ADR-013 — Projected dominant-utilization placement

- **Status:** Accepted; explicitly approved for v1 by the project owner
- **Decision:** Among feasible Workers, minimize projected dominant utilization:
  `max((used_cpu + requested_cpu)/capacity_cpu,
       (used_memory + requested_memory)/capacity_memory)`.
  A zero memory capacity is feasible only for a zero-memory request and contributes zero to the
  memory term. Ratios are compared exactly with checked widened arithmetic. Ties use
  lexicographically smallest `worker_id`. A scheduling pass skips an infeasible older Task and
  continues scanning later pending Tasks.
- **Rationale:** It accounts for both dimensions, is deterministic, and is straightforward to
  test.
- **Rejected:** Random choice and task-count-only load.
- **Consequences:** This is an accounting heuristic, not an optimizer. Skipping infeasible work
  improves utilization but can starve a large request; v1 has no priority or reservation policy.
- **Revisit when:** Weighted, priority, or topology-aware scheduling is added.

## ADR-014 — Linux process groups and fd-native lifecycle

- **Status:** Accepted
- **Decision:** Each workload is a new process group. `signalfd` handles blocked `SIGCHLD`,
  task `timerfd`s drive timeout/grace deadlines, and process-group signals implement timeout
  and cancellation. The direct child is the workload leader. Its natural exit result is frozen;
  remaining same-pgid processes receive SIGTERM/grace/SIGKILL cleanup without overwriting that
  result. Output is then drained to EOF. An exec-status pipe distinguishes successful `exec` from
  launch failure.
- **Rationale:** This gives explicit, testable process semantics without hidden runtime layers.
- **Rejected:** `system()`, implicit shell use, per-child signal handlers.
- **Consequences:** Descendants that deliberately leave the process group with `setsid` are not
  controlled by v1 and can delay EOF if they retain pipe fds. An uninterruptible same-group process
  can delay completion even after SIGKILL.
- **Revisit when:** cgroups or container isolation are introduced.

## ADR-015 — Executable plus argv, inherited environment with overrides

- **Status:** Accepted
- **Decision:** Execute directly without `/bin/sh -c`. The Worker inherits its environment and
  applies submitted key/value overrides. Before fork, the parent finalizes merged environment,
  argv/envp pointer storage, cwd resources, `/dev/null`, and executable resolution against the
  final merged PATH. A submitted executable containing `/` bypasses PATH lookup; otherwise the
  parent resolves it against PATH, with relative/empty entries based on execution cwd. The child
  only performs async-signal-safe fd/pgid/cwd/signal-mask setup and calls `execve` with prepared
  storage. Child stdin is `/dev/null`.
- **Rationale:** It avoids shell-injection semantics while preserving normal PATH-based command
  lookup.
- **Rejected:** Shell command strings and an empty-by-default environment.
- **Consequences:** Worker environment affects executable lookup and child behavior; deployment
  must keep that environment controlled. Parent lookup followed by `execve` has a trusted-environment
  TOCTOU window. Missing PATH for a no-slash executable is a preparation failure.
- **Revisit when:** Fully hermetic execution enters scope.

## ADR-016 — Separate metadata persistence and task-log data path

- **Status:** Accepted; explicitly directed by the project owner
- **Decision:** Controller uses three threads only: EventLoop, dedicated PersistenceThread, and
  dedicated LogWriterThread. The LogWriter owns every task-log filesystem operation, including
  namespace creation, open file/directory fds, I/O, and synchronization, and receives commands
  through a distinct bounded `LogQueue`. It never uses the SQLite connection.
- **Rationale:** High-volume task output must not starve control-plane metadata transactions.
- **Rejected:** One shared storage queue/thread; task logs in SQLite; a general thread pool.
- **Consequences:** Two independent completion paths wake the EventLoop. Cross-path ordering is
  explicit at terminalization rather than implied by queue order.
- **Revisit when:** Logs move to an external durable service.

## ADR-017 — Lossless bounded log flow control

- **Status:** Accepted
- **Decision:** Logs are never intentionally dropped. A two-dimensional credit window limits
  Worker log traffic independently of control messages. Credit has raw-byte and chunk-slot
  dimensions. Every
  granted raw byte and chunk slot already reserves matching Controller LogQueue/in-flight capacity;
  receipt transfers rather than newly allocates those reservations. Controller replenishes credit
  only after LogWriter completes the corresponding write. When either credit dimension prevents a
  chunk, Worker pauses pipe reads; kernel pipe pressure can ultimately block the workload's
  stdout/stderr writes. Controller continues reading the TCP connection.
- **Rationale:** It creates bounded end-to-end backpressure without allowing log traffic to hide
  heartbeats indefinitely in the same TCP session.
- **Rejected:** Unbounded queues, log dropping, blocking the Controller EventLoop, a second v1
  log TCP connection.
- **Consequences:** Very high log rates can slow or block a workload. The protocol includes
  `LOG_WINDOW_UPDATE` beyond the minimum message list in the source specification. Failure to
  admit a correctly credited chunk is an internal invariant failure and triggers fail-stop; it is
  not normal backpressure.
- **Revisit when:** Separate log transport or loss-tolerant logging is approved.

## ADR-018 — Terminal visibility waits for the log durability barrier

- **Status:** Accepted; explicitly requested by the project owner
- **Decision:** Worker generates `TASK_FINISHED` only after leader/group cleanup, both pipes reach
  EOF, and all log chunks are generated. The finish frame is a per-Attempt wire-ordering barrier:
  it cannot begin sending until every previously generated same-Attempt `LOG_CHUNK` is fully sent,
  regardless of control priority or partial writes. Controller then waits until every declared
  final sequence is written, asks LogWriter to `fdatasync` both log files and `fsync` every
  containing directory whose namespace was dirtied by creation, and only after that barrier
  commits the Task+Attempt terminal transaction. Directory sync runs for namespace durability,
  not per log chunk. After commit it releases resources and exposes terminal state with
  `logs_complete=true`.
- **Rationale:** A visible completed Task must not claim complete logs while its tail is still in
  an unsafe queue.
- **Rejected:** Treating finish as ordinary priority control traffic; terminalizing on receipt of
  `TASK_FINISHED`; file or directory sync per chunk; assuming file `fdatasync` alone durably creates
  its directory entry.
- **Consequences:** Slow disks delay terminal visibility. New task/attempt directories and log-file
  entries make their containing directories namespace-dirty until successful directory `fsync`;
  finalization synchronizes any remaining dirty chain deepest-to-root. If any file or required
  directory sync fails, the Attempt is terminalized as `FAILED` with reason
  `LOG_PERSISTENCE_ERROR` and `logs_complete=false` when SQLite remains available; the observed
  child result is retained for diagnosis.
- **Revisit when:** A different durability contract is chosen.

## ADR-019 — Paginated list and snapshot log reads

- **Status:** Accepted
- **Decision:** Task lists use cursor pagination and `taskctl list` automatically fetches all
  pages. Log reads address one stream by byte offset, capture a fixed end offset on the first
  page, and continue only to that snapshot. `--both` reads the streams separately. No continuous
  follow mode exists in v1.
- **Rationale:** Responses remain below the frame limit and a busy task cannot make a read
  unbounded.
- **Rejected:** One unbounded response; default follow mode.
- **Consequences:** List pagination is not a database-wide snapshot across pages. A log command
  shows a stable snapshot, not bytes written after it began.
- **Revisit when:** Streaming query APIs enter scope.

## ADR-020 — Trusted private network only

- **Status:** Accepted
- **Decision:** No TLS, authentication, authorization, sandbox, or multi-tenant isolation is
  provided. Default example listeners bind loopback; multi-host configuration must use a trusted
  private network.
- **Rationale:** Remote execution is intentionally in scope while production security is not.
- **Rejected:** Partial home-grown security.
- **Consequences:** Public Internet exposure is unsafe and unsupported.
- **Revisit when:** Any untrusted principal or network is in scope.

## ADR-021 — Persistence failure is fail-stop for mutation

- **Status:** Accepted
- **Decision:** A required metadata transaction failure prevents its network side effect and
  moves the Controller to a fail-stop/degraded operational mode: reject new mutations, stop
  scheduling, close Worker sessions so Workers terminate workloads, and exit nonzero after
  bounded cleanup. Recovery uses the normal startup procedure.
- **Rationale:** Continuing after unknown control-plane durability would corrupt the state
  contract.
- **Rejected:** Logging the error and continuing; retrying arbitrary transactions forever.
- **Consequences:** A transient SQLite error can make the Controller unavailable. Read-only
  responses may be served only while their required data source remains known-good.
- **Revisit when:** A formal retry/idempotent transaction layer is added.

## ADR-022 — No persisted live Worker registry in v1

- **Status:** Accepted
- **Decision:** Worker and WorkerSession live state is memory-only. Attempts persist their
  worker/session binding. After restart, every Worker must register a new session.
- **Rationale:** Persisting a live lease cannot make it valid across Controller process lifetime.
- **Rejected:** Restoring Workers as healthy from SQLite.
- **Consequences:** `taskctl workers` shows the current Controller lifetime only.
- **Revisit when:** Auditable Worker history becomes a requirement.

## ADR-023 — SQLite WAL with full synchronous durability

- **Status:** Accepted
- **Decision:** Controller configures SQLite foreign keys on, WAL journaling, `synchronous=FULL`,
  and a bounded busy timeout. The data directory is protected by a single-Controller process lock.
- **Rationale:** WAL permits read activity around the one writer while FULL synchronous mode gives
  the control-plane commit a concrete local-durability meaning.
- **Rejected:** `synchronous=OFF/NORMAL` for control state; indefinite busy retry; two Controllers
  sharing one DB.
- **Consequences:** Commit latency is intentionally on the control path and storage failure can
  make the Controller fail-stop. This is local durability, not replication.
- **Revisit when:** A different metadata store or replicated Controller is approved.

## Unresolved decisions

There is no unresolved core semantic decision in the v1 architecture baseline. Numeric tuning
(queue byte limits, page size within documented maxima, reconnect backoff, and operational
timeouts) remains configuration work, not a semantic blocker. The Architecture Gate review result
and approval boundary are recorded in `ARCHITECTURE_GATE.md`.
