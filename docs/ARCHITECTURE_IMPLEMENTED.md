# Implemented architecture map

This is a source guide to the frozen architecture, not a replacement contract.

| Owner | Source | Responsibilities |
|---|---|---|
| Controller composition | `src/controller/application.cpp` | EventLoop, role-fixed TCP peers, request generations, effects, persistence/log completions, queries, shutdown |
| TaskManager | `src/controller/task_manager.cpp` | Task vs immutable Attempt history, one in-flight metadata transition, effect gates, deadlines, LOST/retry |
| WorkerRegistry/ResourceLedger | `src/controller` | stable Worker vs current session/connection, generations, tentative/committed reservations, release after commit |
| Scheduler | `src/scheduler/scheduler.cpp` | feasibility, exact projected dominant ratios, lexical ID tie-break, documented starvation |
| PersistenceThread/StateStore | `src/storage`, `src/persistence` | exclusive SQLite owner, bounded channels/eventfd, WAL FULL transactions, startup recovery |
| LogCoordinator/LogWriter | `src/controller/log_coordinator.cpp`, `src/logs/writer.cpp` | reserved byte/slot credit, exact counters, binary snapshots, single filesystem owner, final file/directory barrier |
| Worker Application/ExecutionManager | `src/worker` | shared session control/log path, per-Attempt runtime ownership, cancel ACK, credit-paced pipe reads, cleanup before new-session reconnect |
| ProcessRuntime | `src/runtime` | parent prepared exec, async-safe child setup, process groups, shared SIGCHLD signalfd, timerfd escalation, reap/drain |
| EventLoop/ConnectionManager | `src/io`, `src/net` | LT epoll, generation tokens, safe self-removal/deferred destruction, bounded buffers and partial-write ordering |
| protocol/client | `src/protocol`, `src/client`, `src/apps/taskctl.cpp` | frozen 20-byte/26-message protocol, strict JSON/base64 schemas, pagination/correlation and actual CLI |

```mermaid
sequenceDiagram
  participant CLI as taskctl
  participant C as Controller EventLoop
  participant DB as PersistenceThread
  participant W as Worker / ProcessRuntime
  participant L as LogWriterThread
  CLI->>C: SUBMIT
  C->>DB: persist PENDING
  DB-->>C: matching committed completion
  C-->>CLI: success
  C->>DB: ACTIVE + ASSIGNED + binding
  DB-->>C: commit
  C->>W: RUN_TASK (revalidated effect)
  W-->>C: ACK, exec-confirmed START
  C->>DB: intermediate Attempt transitions
  W->>C: credited LOG_CHUNK
  C->>L: reserved append
  L-->>C: write completion; credit may replenish
  W->>C: FINISH after reap/EOF and log wire barrier
  C->>L: final file and dirty-directory sync barrier
  L-->>C: exact Attempt-bound evidence
  C->>DB: atomic terminal Task + Attempt
  DB-->>C: commit
  Note over C: release memory reservation, expose terminal
```

SQLite + memory accounting + network + filesystem are not a distributed ACID
transaction. The owner serializes logical conversion after durable evidence.
Unknown commit halts new effects. Session invalidation batches old nonterminal
Attempts as LOST before release/replacement ACK. Startup never recovers sessions
as healthy or infers successful results from orphan log files.

Cancel persists intent first; positive ACK is receipt. A correlated refusal without
already accepted FINISH is a session consistency failure, not cancellation proof.
Known natural completion wins its finalization path. Worker cancel uses the owned
RuntimeProcessId/pgid, and first installed cause wins. Reconnect drains/reaps old
owners, clears session wire state, then registers a fresh UUID. Manual retry alone
creates a new Attempt through normal scheduling; historical log paths remain fixed.

M10 adds packaging, external operator/benchmark scripts and instrumentation.
These do not introduce daemon threads, new protocols, persisted states, adoption,
automatic retry, sandboxing or resource enforcement. Frozen ARCHITECTURE,
OWNERSHIP, STATE_MACHINE, PROTOCOL, FAILURE_MODEL and DECISIONS remain authoritative.
