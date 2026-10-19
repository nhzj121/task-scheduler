# Milestone 5 Authorization / Codex Development Prompt

## Project: Linux C++ Distributed Task Scheduler

**Authorized milestone:** Milestone 5 — SQLite StateStore and Persistence Channel

**Repository:** `nhzj121/task-scheduler`

**Required development branch:** `milestone-5-sqlite-persistence`

**Accepted GitHub `main` baseline at authorization:** `e2d83701ca0ea463db7146f1419c3258e3cf8a6e`

**Approval scope:** M5 production implementation, M5-only test fixtures, build wiring, and narrowly necessary lower-layer corrections.
**Review gate:** Implement → validate → commit/push M5 branch → report → **STOP**. Do not merge into `main`.

> **Authority and interpretation.** This document explicitly authorizes M5, superseding **only** the historical M1-only milestone-authorization boundary in `docs/spec/Codex_Full_Implementation_Prompt_v1.md`. It does **not** supersede any frozen architecture, ownership, transaction, state-machine, failure, recovery, protocol, or durability semantics. If an actual contradiction or missing architectural decision prevents a correct implementation, STOP and request the smallest explicit clarification. Never silently invent a new contract.

---

## 0. Authorization facts and prior accepted work

The Architecture Gate and Milestones 1, 2, 3 and 4 have passed. M2, M3 and M4 are merged into the accepted `main` baseline; accepted M1 work is already included. The upstream GitHub `main` ref was independently checked when this authorization was prepared:

- `main`: `e2d83701ca0ea463db7146f1419c3258e3cf8a6e`;
- its M3 parent: `37da79590b32bd54d08fec71ac4b4783e2b1b05c`;
- M3 is fully ancestral to `main`, with no unmerged M3 changes.

Milestone 5 is now explicitly authorized **as the only newly authorized production milestone**. Do not begin M6, M7, M8, M9 or M10 because M5 tests pass. No Controller networking/application runtime is authorized in M5.

## 1. Work locations and user data protection

Source tree (from WSL):

```text
/mnt/e/Deck_all_need/projects/work/task_scheduler
```

Formal WSL build tree:

```text
/home/nhzj121/build/task_scheduler
```

Important pre-existing personal build note:

```text
/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
```

**Never delete, reset or overwrite this note.** Never delete/recreate the entire build directory to achieve a clean build; use CMake clean targets. Preserve `.local-notes/` and every ignored or unrelated personal file. Do not run a broad `git clean`, destructive `git reset`, or automatic `stash` on unrelated changes.

Windows Codex Desktop editing and Git use are acceptable. Formal compiler, SQLite, CTest, integration, and runtime verification must use the supported **WSL Ubuntu 22.04 / GCC 11+ / CMake 3.20+** Linux environment.

## 2. Mandatory Git preflight — before production changes

1. Inspect `git status --short`, `git branch -vv`, `git remote -v`, `git log --graph --decorate --oneline -12`, local `main`, and `origin/main`.
2. `git fetch origin` without mutating `main`.
3. Verify local and remote `main` include accepted M3 HEAD `37da79590b32bd54d08fec71ac4b4783e2b1b05c`, accepted M4 HEAD `070ca5eda09ab2a3fc7c818410f44bf3dfacd524`, and the accepted M4 final-adjustment commit as merged. Confirm `main` is the correct post-M3 baseline; if it changed since the above SHA, inspect the change instead of assuming it is equivalent.
4. Do not perform M5 work on `main`, and do not rewrite any existing milestone branch.
5. If the worktree is clean, create `milestone-5-sqlite-persistence` from the accepted `main`. If an M5 branch already exists, inspect its ancestry and changes before reusing it.
6. **Permitted uncommitted exception:** If the *only* pre-existing uncommitted file is `docs/spec/Milestone_5_Authorization_Codex_Development_Prompt.md`, carry this new authorization file onto the M5 branch, then include it in the appropriate M5 commit. Do not commit the authorization file on `main`.
7. If unrelated tracked changes, conflicts, or another dirty state exist, STOP; explain precisely and preserve them for the user. Do not silently discard, overwrite, or stash user changes.
8. Report verified baseline SHA, branch name, and initial working-tree status.

## 3. Mandatory reading order — authoritative frozen contracts

Read these files in the repository, not summaries of them, before coding:

1. `AGENTS.md`;
2. `docs/ARCHITECTURE_GATE.md`;
3. `docs/spec/Codex_Full_Implementation_Prompt_v1.md`;
4. **this** `docs/spec/Milestone_5_Authorization_Codex_Development_Prompt.md`;
5. `docs/spec/Task_Scheduler_Final_Scope_v1.md`;
6. `docs/PROJECT_SCOPE.md`;
7. `docs/ARCHITECTURE.md`, particularly Controller concurrency/handoff, SQLite schema, transaction boundaries, Controller restart, dependency direction;
8. `docs/STATE_MACHINE.md`, all Task and Attempt transitions, serialization and terminal visibility;
9. `docs/OWNERSHIP.md`, especially PersistenceThread, channels, lifetime, shutdown;
10. `docs/FAILURE_MODEL.md`, particularly SQLite failure/fail-stop, LOST and recovery;
11. `docs/TEST_STRATEGY.md`, particularly SQLite StateStore/PersistenceThread and guarantee-to-test traceability;
12. `docs/IMPLEMENTATION_PLAN.md`, especially Milestone 5;
13. `docs/DECISIONS.md`, especially ADR-005, ADR-006, ADR-008, ADR-016, ADR-018, ADR-021, ADR-022, ADR-023;
14. `docs/PROTOCOL.md` for persisted fields needed by future handlers — **not** to implement networking;
15. `docs/DEPLOYMENT.md` for SQLite data-path, process-lock and operational constraints;
16. Accepted existing headers and tests: `ts/common`, `ts/domain`, `ts/io`, `ts/protocol`, `ts/net`, and `ts/runtime`, as applicable, plus CMake test conventions.

If a genuine conflict concerns state meanings, persisted columns, transaction boundaries, log completeness, external side-effect ordering, queue guarantees, or SQLite ownership, STOP and cite conflicting file/section names. Do not reinterpret the frozen design by choosing one side silently.

## 4. Exact M5 scope, dependency boundary, and deliverable

`docs/IMPLEMENTATION_PLAN.md` specifies:

> **Milestone 5 — SQLite StateStore and persistence channel.** Future modules: `storage`, PersistenceThread integration utility. Work: schema version 1, prepared statements, all transaction commands, state-version checks, startup recovery, bounded command/completion channel. **No Controller networking yet.** Acceptance: CRUD/constraint tests; atomic terminal/session-loss/recovery; rollback failpoints; stale generation; queue bounds; SQLite error produces explicit failure.

Build **real, reusable production** `storage` code and an independently tested bounded persistence worker/channel. Include data-only command/result types that future M6/M7 can use. Demonstrate the ordering guarantees through test-only EventLoop/operation-generation harnesses, not through fake production Controller/Worker orchestration.

Allowed changes:

- `include/ts/storage/`, `src/storage/`, necessary storage-only configuration/value interfaces;
- minimal `include/ts/io/` or `src/io/` changes **only** to fix a demonstrated generic lower-layer defect (review carefully; prefer reuse);
- `CMakeLists.txt`, M5 unit/component/integration tests, deterministic test helpers/fault injection;
- this authorization document and a narrowly approved contract clarification if explicitly necessary.

Required dependency direction from frozen architecture:

```text
storage -> common
storage -> domain
```

If M5's persistence channel needs `io` for the existing eventfd `WakeupChannel`, keep this as an explicitly separated generic integration utility or a clearly justified dependency consistent with the frozen module graph. **Do not silently add a forbidden `storage -> io`, `storage -> net`, `storage -> protocol`, or `storage -> controller` dependency.** Consult the frozen diagram and STOP if no conforming layering can satisfy the required channel semantics without approval.

Storage may use SQLite3 and JSON libraries as technical dependencies. It never directly owns TCP sockets, task processes, WorkerSessions, ResourceLedger, TaskManager, LogWriter, or the application's EventLoop objects.

## 5. Explicitly prohibited work

Do **not** implement:

- M6 Scheduler, ResourceLedger orchestration, TaskManager state-decision engine, or Controller live state;
- M7 Controller/Worker registration, actual TCP Task dispatch, taskctl application functionality, or end-to-end workload execution;
- M8 LogWriter, log namespace creation, log chunk persistence, log byte/slot credit allocation, final file/directory `fdatasync`/`fsync` implementation;
- M9 reconnection, distributed retry, actual session-liveness machinery, or app-level fail-stop shutdown;
- M10 packaging, benchmarks, migration beyond v1, sanitizer infrastructure overhaul, or operational tooling;
- new protocol messages/fields, new Task/Attempt states, automatic retry, transaction pooling, synchronous SQLite calls on the EventLoop, or cross-medium ACID claims;
- Task/Attempt data stored inside log files or stdout/stderr binary blobs stored in SQLite.

Test fixtures may *simulate* side effects or an owner applying commits by value to verify gates. Such fixtures must remain in `tests/`; do not ship fake production Controller or Worker behavior.

## 6. Implementation checkpoints and suggested commit sequence

Follow these internal stages within one M5 authorization; no additional user authorization is required between stages:

| Internal phase | Work | Gate |
|---|---|---|
| **M5-A** | Schema v1, DB creation/version verification, process lock, WAL/FULL/FK settings, prepared statements, reads | introspection/CRUD/constraints/version/ownership tests |
| **M5-B** | All required transactional commands, CAS versions, atomicity/rollback | transaction-success/conflict/failure-injection tests |
| **M5-C** | Single SQLite-owning PersistenceThread, bounded command/completion channels, eventfd notifications, generation-safe completion envelope | queue/ownership/wakeup/stale-generation tests |
| **M5-D** | Startup recovery, session-loss and fatal SQLite handling, restart/shutdown and stress tests | independent connection/commit/restart/fail-stop tests |

Suggested reviewable commits: schema/opening; transaction commands; persistence thread/channel; recovery/fault tests. Small refactors can span stages, but each commit must be self-consistent and buildable where practical.

## 7. Schema version 1 — exact `tasks` table

Implement the table described in `docs/ARCHITECTURE.md`, with actual SQLite `NOT NULL`, `CHECK`, foreign-key and index enforcement rather than solely in-memory checks:

| Column | Frozen contract |
|---|---|
| `task_id` | `TEXT PRIMARY KEY`; canonical Task UUID |
| `state` | `TEXT NOT NULL`; CHECK permits exactly `PENDING`, `ACTIVE`, `SUCCEEDED`, `FAILED`, `CANCELLED` |
| `state_version` | `INTEGER NOT NULL`; monotonically incremented on successful state/intent changes |
| `executable` | `TEXT NOT NULL` |
| `args_json`, `env_json` | `TEXT NOT NULL`; canonical JSON serialization |
| `cwd` | nullable text |
| `cpu_slots` | `INTEGER NOT NULL CHECK >= 1` |
| `memory_mb` | `INTEGER NOT NULL CHECK >= 0` |
| `timeout_ms` | nullable integer; `CHECK > 0` when present |
| `cancel_requested_at` | nullable timestamp text |
| `latest_attempt_id` | nullable Attempt ID text |
| `created_at`, `updated_at` | `TEXT NOT NULL` |

Required indexes, matching the frozen specification:

- `(state, created_at, task_id)` for pending scheduling;
- `(created_at DESC, task_id DESC)` for task listing.

Make any additional physical constraint/index only if necessary and consistent with frozen semantics; explain it explicitly. Do not change states, rename columns, add persistent Worker leases, or invent a non-v1 schema.

## 8. Schema version 1 — exact `attempts` table

| Column | Frozen contract |
|---|---|
| `attempt_id` | `TEXT PRIMARY KEY` |
| `task_id` | `TEXT NOT NULL REFERENCES tasks` |
| `attempt_no` | `INTEGER NOT NULL CHECK >= 1`; `UNIQUE(task_id, attempt_no)` |
| `state` | `TEXT NOT NULL`; CHECK permits exactly `ASSIGNED`, `STARTING`, `RUNNING`, `SUCCEEDED`, `FAILED`, `TIMED_OUT`, `CANCELLED`, `LOST` |
| `state_version` | `INTEGER NOT NULL`; monotonically incremented |
| `worker_id`, `session_id` | `TEXT NOT NULL` for assigned Attempts |
| `assigned_at`, `acknowledged_at`, `started_at`, `finished_at` | nullable lifecycle timestamp text |
| `exit_code`, `term_signal` | nullable integer |
| `reason` | nullable text |
| `observed_outcome_json` | nullable text; preserves observed workload facts if a log/infrastructure failure supersedes success |
| `stdout_path`, `stderr_path` | `TEXT NOT NULL` paths (metadata only) |
| `stdout_bytes`, `stderr_bytes` | `INTEGER NOT NULL DEFAULT 0` |
| `stdout_final_sequence`, `stderr_final_sequence` | nullable integer |
| `logs_complete` | `INTEGER NOT NULL DEFAULT 0 CHECK in (0,1)` |

Required indexes: `(task_id, attempt_no)`, `(session_id, state)`, and `(state)`.

Also enforce **at most one nonterminal Attempt per Task** through the frozen application transaction logic and tests. Do not introduce a new Attempt state for log-finalization-in-progress.

## 9. SQLite initialization, version, lock and connection ownership

- Only the dedicated PersistenceThread opens the SQLite connection, owns every prepared `sqlite3_stmt*`, begins/commits/rolls back transactions, and closes the connection. No other production thread may issue DB operations on that handle.
- Initialize empty DBs as exact schema **version 1**. Detect and reject unsupported/unrecognized versions; never silently truncate or rewrite a pre-existing DB. Do not implement unrelated schema migration/version 2.
- Set and **verify** `PRAGMA foreign_keys=ON`, SQLite `journal_mode=WAL`, `synchronous=FULL`, and a **bounded** `busy_timeout`, following ADR-023. Note that some PRAGMAs are connection-scoped and must be set/verified on the actual owning connection.
- Enforce one-Controller data-directory process ownership with an explicit exclusive lifetime lock, consistent with the frozen deployment contract. Acquire it before the live database is exposed, retain it until orderly shutdown, and test contention. Do not substitute SQLite's internal transaction locks for the Controller process ownership rule.
- The precise file name and low-level lock implementation are implementation choices only if no frozen document defines them; use a deterministic, safe local-file policy and document it. Never conflate a test-only shared read connection with a second live Controller.
- Use RAII or equivalent explicit cleanup for lock, SQLite handle and prepared statements, all on the permitted owning thread. Set SQLite threading mode consistently with exclusive owner; do not leak DB objects into EventLoop callbacks.
- Check every relevant return code, including prepare/bind/step/reset/finalize/BEGIN/COMMIT/ROLLBACK/close and PRAGMAs. Do not assume a statement that returned `SQLITE_DONE` has committed.
- SQLite integers are signed 64-bit. Reject/validate unsigned domain values beyond representable SQLite range rather than silently truncating, wrapping or changing sign. SQLite NULL, text, ID and timestamp binding must preserve semantics.
- Canonicalize `args_json`, `env_json`, and `observed_outcome_json` on write with deterministic serialization. Preserve submitted meaning; do not introduce undocumented schema fields or deserialize malformed rows as valid state.
- Design read paths to support required snapshots, Task/Attempt history, pending Task listings and stable tuple-based pagination where specified, using the same SQLite owner thread. Do not implement network handlers.
- Use SQLite prepared statements for operations, not unsafe string concatenation. Protect identifier/path handling under the trusted-local-filesystem baseline.

## 10. Transaction command set — all commands, no placeholders

Implement typed, moveable, value-only commands/results covering the full `docs/ARCHITECTURE.md` transaction table and the `PENDING -> CANCELLED` path in `docs/STATE_MACHINE.md`:

1. **Submit:** insert one `PENDING` Task; no success is observable until commit.
2. **Assignment:** conditionally update Task `PENDING -> ACTIVE` **and** insert the new Attempt in `ASSIGNED` with Worker/session binding in **one transaction**, including Task latest Attempt ID and versions. A failed insert or CAS rolls back Task update.
3. **Intermediate Attempt transition:** conditional `ASSIGNED -> STARTING` or `STARTING -> RUNNING` and allowed analogous transitions, with `state_version` check; Task remains `ACTIVE` as appropriate.
4. **Active cancellation intent:** conditionally set persisted `cancel_requested_at` while Task remains `ACTIVE`; do not claim the Task is terminal or send Worker cancellation here.
5. **Pending cancellation:** conditionally transition a Task `PENDING -> CANCELLED` with no Attempt, inside a committed operation.
6. **Terminalization:** atomically update terminal Attempt and matching Task, with correct outcome/state/reason/final counters/paths/`logs_complete` metadata, only when caller supplies the prerequisite authorization/barrier evidence required by the frozen semantics. Storage must not fabricate or perform log durability.
7. **Retry:** conditional `FAILED -> PENDING`; clear cancellation intent; preserve historical `latest_attempt_id`; do not mutate historical Attempts or auto-retry.
8. **Session loss:** one transaction changing **all** bound nonterminal Attempts to `LOST` and their active Tasks to `FAILED`, with accurate recovery/loss reason, versions and `logs_complete=false`. Never treat `LOST` as proven-empty logs.
9. **Startup recovery:** one transaction converting all persisted nonterminal Attempts (`ASSIGNED`, `STARTING`, `RUNNING`) to `LOST`, matching `ACTIVE` Tasks to `FAILED`, recording recovery reason/time, and forcing `logs_complete=false`. Preserve `PENDING` Tasks and all terminal Task/Attempt history. No filesystem inference of successful logs.
10. **Reads:** typed metadata-only queries necessary for Task, Attempt, state/recovery verification, pending scheduling order and stable list pagination. All queries remain on the SQLite owner thread.

Each transaction must return an explicit committed success or a precise conflict/failure classification; **never** report a partially committed operation as success. Reset statements and rollback on any failed intermediate step. At the end of every command, SQLite must be out of an unexpected transaction.

### Transaction-specific corner cases

- The same Task cannot acquire two nonterminal Attempts, including concurrent queued assignment intents.
- `state_version` predicates must guard affected rows; `sqlite3_changes`/actual affected row counts must be checked. No success on zero-row CAS update.
- A stale request must not rewrite newer state, reset a terminal Attempt, release a resource reservation, or enable a network side effect.
- Task/Attempt terminal updates must be visible atomically from a **separate SQLite connection after commit**, never one ahead of the other.
- When one session owns multiple Attempts, one failure anywhere in session-loss transaction must roll back *all* associated Attempt and Task updates.
- Future log-finalization must not be represented by a new Task/Attempt state; existing `finalization_pending` is a computed live diagnostic only.
- A `FAILED` Task's retry leaves the prior Attempt records untouched and historical `latest_attempt_id` intact.
- Only a proven-not-executed dispatch rejection or pre-dispatch cancellation can set zero sizes/final counts and `logs_complete=true` **without** the log durability barrier; a LOST result never can.
- A normal runtime terminal completion with `logs_complete=true` requires a successfully supplied/logically verified final-barrier prerequisite; if this precondition cannot be modeled correctly as a value-only operation without inventing application policy, STOP for clarification rather than defaulting to true.
- For log persistence failure when SQLite is still available, atomically commit `FAILED` / `FAILED`, reason `LOG_PERSISTENCE_ERROR`, preserve observed child outcome in `observed_outcome_json`, and set `logs_complete=false` (do not claim success).
- Roll back on duplicate IDs, invalid FK, illegal state, invalid nullable data, constraint errors or failed statements; do not allow malformed persisted rows to escape the typed boundary undetected.

## 11. Durability-before-external-side-effect gate

ADR-006 is binding **even though M7's TCP dispatch is not being implemented yet**. Design/verify a commit-completion interface that future EventLoop code can gate on:

```text
request to persist
  -> bounded command accepted
  -> PersistenceThread runs SQLite transaction
  -> COMMIT succeeds
  -> completion value published + eventfd wakeup
  -> EventLoop validates ID/version/generation
  -> caller may perform corresponding external side effect
```

Required ordering demonstrations using test-only side-effect sinks:

- Submit response is not enabled before durable `PENDING` commit.
- RUN_TASK send authorization is not enabled before Task `ACTIVE` + Attempt `ASSIGNED` commit.
- Cancellation send authorization is not enabled before `cancel_requested_at` commit.
- Terminal state visibility, in-memory reservation release, and response are not enabled before terminal Task+Attempt commit; normal result also requires the external log final barrier prerequisite.
- A failure at any write, final prepare, or commit point cannot authorize that side effect.
- Do not implement actual Controller network dispatch or ResourceLedger mutation in production M5; tests can record a symbolic effect only after a successful matching completion.

## 12. Command ownership, bounded queues, and backpressure

- The EventLoop-facing side may create **immutable copied/movable metadata command values** and use **nonblocking** `try_push`/`try_post`. It must never block waiting for SQLite or a condition variable.
- M5 must bound queued commands by a configured finite **command count and owned metadata bytes**; do not bound count while allowing arbitrarily large strings/arrays to exhaust memory. Reject oversized items explicitly.
- Reserve usable capacity for essential terminal/session-loss/recovery commands as specified. Ordinary query and new-mutation pressure must not consume the essential reserve. Essential enqueue failure is a persistence availability fault and must report a fatal/degraded signal, not silently drop work.
- Use a single SQLite owner thread. `wait_pop` is acceptable **only in that worker thread**. Do not introduce a thread per query, connection, or transaction.
- Completions must also be bounded and lossless: reserve or account for each accepted command's eventual completion before accepting it, or provide an equivalent bounded mechanism. A completion must not be dropped because the consumer is temporarily slow.
- Completion delivery must not hold the SQLite transaction open while waiting for an EventLoop consumer. Apply explicit admission/backpressure rules.
- Include operation ID, relevant Task/Attempt IDs, expected state version, and session/entity generation in completion values as appropriate. No cross-thread callable closures capturing domain, socket or WorkerSession pointers.
- The **EventLoop** (not PersistenceThread) ultimately validates the received operation ID/version/generation against its current owners, then applies or discards the completion. Provide a reusable value-only validation interface or a test-side validator as appropriate to the frozen M5/M6 boundary.
- Use the accepted `io::WakeupChannel`/`EventFd` patterns **without** changing storage's frozen dependency direction; use a separable integration layer/test harness if needed. No independent general-purpose reactor redesign.
- `eventfd` wakeups are notifications, not transaction commits. Drain completion notifications and queue to empty safely; guard lost-wakeup/shutdown races.

## 13. SQLite error classification, fail-stop and bounded cleanup

On any required metadata transition failure, comply with `docs/FAILURE_MODEL.md` and ADR-021:

1. Do **not** enable any external side effect gated on that failed transition.
2. Do **not** expose uncommitted state as durable truth.
3. Report a **fatal/degraded persistence failure** through a typed completion/status path. Future Controller integration must reject new mutations and stop scheduling.
4. Future Controller integration must invalidate/close Worker sessions and terminate running work according to the frozen shutdown sequence; **M5 must not implement that network/process behavior itself**.
5. If SQLite remains operable, future Controller may perform only bounded required cleanup transactions, not unbounded blind retries.
6. Propagate a terminal nonzero/fail-stop decision to the future application owner via an explicit value-only signal; do not autonomously call `exit()` or `std::terminate()` to mask recoverable diagnostics.

Distinguish expected stale-version/constraint business conflicts from true SQLite infrastructure failures where the frozen architecture makes that distinction. Both are explicit failed operations, but do not arbitrarily characterize an ordinary conditional state conflict as disk corruption. Do not continue scheduling after a required persistence infrastructure error.

Cover `SQLITE_BUSY` under a bounded timeout, injected statement/commit failures, simulated `SQLITE_FULL` and `SQLITE_IOERR`, foreign key and constraint violations, and rollback errors. Failure injection must be deterministic and test-only or a narrowly scoped injectable SQLite operation boundary; do not monkey-patch production semantics or claim disk-full behavior was physically reproduced when only a failpoint was used.

## 14. Recovery and no persisted live Worker registry

Startup recovery must precede any simulated listener-ready/dispatch-ready state. Test:

- Existing `PENDING` Tasks stay `PENDING` and retain scheduling order.
- All nonterminal persisted Attempts, irrespective of whether their Worker can reconnect, become `LOST` in **one transaction**, with matching previously `ACTIVE` Tasks becoming `FAILED`.
- All changed Attempts have `logs_complete=false`; partial filenames and sizes may remain historical metadata but are never inferred to represent complete logs.
- Terminal histories are immutable and retained; no terminal state is downgraded.
- No startup query of old Worker pids, sessions, log-file sizes, or filesystem directory existence.
- No Worker registry/session lease is restored as valid in SQLite. Worker records remain Controller-memory-only.
- No resource reservations are inferred from old persisted sessions; future in-memory reservation owner starts with zero at restart.
- A crash/restart test uses a reopened SQLite database and separate reader connection, not just reinitializing in-memory fixtures.
- Recovery failure must not signal readiness or accept new work.

## 15. Thread lifetime and deterministic shutdown

- A single owner thread creates and destroys the SQLite connection, statements and transaction state. Thread affinity is a reviewed invariant, not a comment-only promise.
- Identify the top-level component that owns the bounded command/completion channels, the worker thread and any `eventfd`, and the destruction order. Design the integration utility for future ControllerRuntime composition.
- Stop admission first; select a precise, documented **bounded drain or abort** policy consistent with frozen accepted-work guarantees. Never report a canceled, lost or discarded accepted command as committed success.
- Wake any blocked worker wait, resolve accepted commands to explicit outcomes, join worker before closing its DB resources, and prevent a worker from posting into a destroyed channel.
- Ensure no open SQLite transaction remains on teardown; finalize statements and release the data-directory process lock in the correct order.
- Test idle stop, stop with queued commands, stop while a command is executing, and repeated create/destroy, with no deadlock, lost completion, thread leak, fd leak or stale wakeup callback.
- Do not introduce a second SQLite owner to 'help' with shutdown.

## 16. Error boundaries, numeric/JSON fidelity, and stale generations

- C++ exceptions and SQLite error codes must be converted to explicit command/status results at the appropriate thread boundary; no uncaught worker-thread exception may terminate the process unexpectedly.
- SQLite `INTEGER`, JSON integer and unsigned domain ranges must be verified before binding and when reading. Explicitly test INT64_MAX and one-beyond cases if representable in the input API.
- Reject NUL-contaminated identifiers/paths where they would truncate C APIs; use length-aware bindings and explicit metadata validation.
- Untrusted persisted JSON/text must be validated against the frozen domain shape when read back. No silent coercion, omitted required field or permissive SQL-NULL-to-zero conversion.
- A stale completion bearing an old Task/Attempt state version, operation ID, or Worker/session generation cannot mutate the current EventLoop-side fixture, even if its SQL operation once committed. SQL CAS state and in-memory generation are **distinct protections**, both tested.
- Ensure duplicate completion delivery or repeated eventfd notifications do not apply the same side effect twice.
- Do not interpret an SQLite committed result as cross-medium atomicity with log files, resource accounting, or network output.

## 17. Schema and transaction tests — mandatory matrix

Use deterministic temporary SQLite databases, independent connections and fixture IDs. Tests must inspect **real SQLite schema** (`sqlite_master`, `PRAGMA table_info`, `PRAGMA foreign_key_list`, index metadata, constraint probes), not just compare source SQL strings.

### M5-A: Schema and open/close

- Creation of a fresh schema-v1 DB, idempotent reopening, correct version rejection for unknown newer/invalid versions, no destructive re-creation;
- `tasks` and `attempts` exact columns, types, state CHECK lists, nullability, PK/FK/unique constraints, required indexes;
- WAL, FULL synchronous mode, foreign keys, bounded busy timeout verified on the owning connection;
- two Controller-start contenders cannot simultaneously acquire the same data-directory ownership lock;
- prepared statements are finalized and file descriptors/lock are released on normal shutdown;
- canonical JSON text and exact round-trip of nullable columns, UUIDs, timestamps and integer bounds;
- independent read/pagination with stable ordered cursors, including same timestamp tie-break;
- no log bodies, Worker live registry, or additional persistent runtime state inserted.

### M5-B: Every transaction

- Submit succeeds only with one committed `PENDING` row; duplicate ID fails without mutation;
- Assignment atomically creates `ACTIVE` Task and `ASSIGNED` Attempt, binding Worker/session and incrementing versions, with `latest_attempt_id` correct;
- intermediate `ASSIGNED -> STARTING -> RUNNING` versions/CAS and invalid-transition rejection;
- active cancellation intent persistence and pending cancellation with no Attempt;
- terminal success, failure, timeout, cancellation, dispatch rejection, pre-dispatch cancellation, LOST, and log-persistence-failure metadata rules;
- Task+Attempt terminal atomicity observed using a *second* connection after commit;
- Retry `FAILED -> PENDING` with Attempt history untouched, cancel intent cleared and last Attempt history retained;
- session-loss atomic multi-Attempt update and matching Tasks, unbound sessions unchanged;
- one nonterminal Attempt per Task; duplicate `(task_id, attempt_no)` constraint;
- exact state-version CAS conflict produces no modifications or external authorization;
- transaction rollback after failure at each meaningful prepare/bind/step/commit phase, no lingering partial transaction;
- no committed terminal `logs_complete=true` without valid documented prerequisites.

### M5-C: Channels and ownership

- Only one actual SQLite-owning thread; EventLoop/test producer never touches its connection;
- multiple ordered commands and corresponding operation IDs/versions/generations delivered through bounded values;
- queue limits for **count and bytes**; ordinary admission rejection and reserved essential capacity behavior;
- completion delivery stays bounded and lossless under temporarily stalled EventLoop draining;
- `eventfd` readiness/wakeup, drain-to-empty, stale notifications, resume after pause, shutdown without deadlocks;
- a test-side EventLoop owner rejects stale ID/version/generation after replacing an entity/session;
- timeout/slow-storage injection never blocks an EventLoop callback waiting for a condition variable or SQLite mutex;
- no raw Task/Attempt/WorkerSession/TcpConnection pointers cross threads.

### M5-D: Recovery and failures

- Reopen seeded database after simulated crash; PENDING unchanged; ASSIGNED/STARTING/RUNNING all LOST; matching Tasks FAILED;
- all terminal rows and Attempt history preserved, `logs_complete` not inferred from files;
- session invalidation updates the entire session as one atomic operation; failure rolls everything back;
- SQLite BUSY/FULL/IOERR/commit failure inject or simulated locked writer; explicit fatal/degraded result for required transitions;
- failed submit/assignment/cancel/terminal side-effect gate remains closed;
- startup recovery failure prevents readiness; bounded retry/cleanup policy, no silent infinite loop;
- duplicate/stale completions do not re-enable effects or resurrect sessions;
- repeated create/execute/close/reopen cycles with no fd, prepared-statement, thread, lock or eventfd growth.

## 18. Controlled fault injection guidance

Provide deterministic, M5-only fault injection for SQLite statements, transaction stages and worker/channel lifecycle. Injection must not require privileged disk faults or non-reproducible `sleep()`-based timing. Use test-only hooks with explicit arm/disarm and counters; production defaults remain real SQLite calls.

For each tested command, inject failures at representative transition-sensitive checkpoints: before work, after the first of multiple updates, during dependent insert/update, on commit, and on rollback if appropriate. Verify no successful external gate, no wrong partial durable state and no leaked transaction afterward. If a simulated commit failure has an *ambiguous* durability outcome, report it as unknown/fatal rather than treating the transaction as definitely rolled back and retrying blindly.

Test setup and teardown may use temporary independent SQLite connections for inspection. These must not be misrepresented as additional production writer threads.

## 19. CMake and existing-regression obligations

- Add a clearly named storage library (e.g., `ts_storage`) with only allowed dependency edges, using the already-discovered `SQLite3` package rather than introducing an external database adapter.
- Add M5 unit/component/recovery/failure test targets, discoverable in CTest under a dedicated `m5` (or `storage`) label. Use existing GTest/CTest conventions and warnings-as-errors flags.
- Retain every existing M1, M2, M3 and M4 build/test target. Do not weaken assertions, skip tests, alter architecture docs for convenience, or make the ordinary acceptance path depend on live Controller/Worker binaries.
- Run formal WSL configure, a clean warnings-as-errors build, full CTest, focused M5 repeats/stress and resource checks.
- If ASan/UBSan tooling is already present and appropriate, it may be used and actual results reported. If not configured, do not implement M10 sanitizer infrastructure and do not claim sanitizer passes.

Formal example commands (run inside WSL Ubuntu 22.04):

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -L m5 --output-on-failure --repeat until-fail:20
```

Adapt exact labels/repeat counts to the actual test names and time budget, but document the commands actually executed. Don't report imaginary success, including unexecuted sanitizers.

## 20. Mandatory architecture/code review before completion

Explicitly inspect and report:

- actual schema/version/index/constraint consistency with `ARCHITECTURE.md`;
- all eight frozen transaction boundaries plus pending-cancellation path;
- complete Worker/session loss and restart LOST semantics;
- `state_version` and one outstanding Task metadata operation requirements;
- Task vs Attempt ownership and terminal-state mapping;
- full SQLite connection/statement/thread/process-lock lifecycle;
- queues bounded in count and metadata bytes, essential reservation and completion capacity;
- `eventfd` notification, stale-generation handling and safe shutdown order;
- durability-before-side-effect tests with no premature network/Task side effects;
- bounded SQLite BUSY/IOERR/FULL paths and fatal/degraded failure status;
- absence of log file I/O, networking and M6/M7/M8+ production logic;
- exact module dependency direction, including any io integration utility;
- retained M1/M2/M3/M4 regression behavior and clean warning/error build.

Do not mark source review PASS based only on test counts; inspect the actual ownership, SQLite call paths, transaction exit states and rollback cases.

## 21. Stop conditions requiring architecture clarification

**STOP immediately** if correct M5 implementation would require any of the following without explicit approval:

- changing frozen `tasks`/`attempts` columns, state names, index semantics or transaction grouping;
- adding a new durable WorkerRegistry/session table, scheduler state, log blobs or other new schema;
- inventing a new persisted Task/Attempt finalization state or weakening log barriers;
- permitting external irreversible actions before required SQLite commit;
- allowing multiple production SQLite owners or placing SQLite calls on EventLoop;
- routing application callbacks/raw owning pointers across persistence thread boundary;
- unbounded work queues/completion queues, arbitrary infinite busy retries or dropped accepted commands;
- a non-conforming new module dependency;
- claiming lost work is definitely stopped, or that sqlite+log files+EventLoop are one ACID transaction;
- a genuine unresolved rule for session-loss/terminalization/recovery where choosing one changes externally observable semantics.

When blocked, identify exact documents/sections, competing interpretations, test cases whose results differ, and the minimal candidate clarification. Do not continue the affected production implementation until authorized.

## 22. Git completion instructions

When the **entire** M5 scope is implemented and verified:

1. Inspect `git status`, branch, baseline and working-tree cleanliness; preserve unrelated local/ignored files.
2. Use small, reviewable M5 commits; commit only authorized M5 production files, tests, CMake and the authorization document to `milestone-5-sqlite-persistence`.
3. Confirm `git diff --check main...HEAD` is clean and audit complete `main...HEAD` scope.
4. Reconfirm `main` and `origin/main` were not modified by M5 work.
5. Push `milestone-5-sqlite-persistence` to `origin` and verify remote HEAD equals local HEAD.
6. **Do not open or merge a PR automatically. Do not merge to `main`.**
7. **Do not start M6 or later.** Stop with the report below and await independent review.

## 23. Required Milestone 5 final report

Provide the following headings with real evidence:

### A. Git / baseline

Branch, accepted `main` full SHA, final M5 HEAD full SHA, local/remote sync, commit history, worktree state, main untouched, ignored notes preserved.

### B. Implementation summary

List M5-A, M5-B, M5-C, M5-D deliverables; identify production files/targets and technical design.

### C. Files changed

Every added/modified/deleted file; confirm no unrelated changes.

### D. Schema, constraints, PRAGMAs and process lock

Real SQLite introspection evidence for both tables, version, indices, FK/CHECK, canonical JSON, WAL, synchronous FULL, bounded busy timeout and exclusive data directory ownership.

### E. Transaction and version review

Each transaction type (submit, assignment, intermediate, cancellation intent, pending cancellation, terminalization, retry, session loss, recovery), its atomicity, version predicates, rollback, and log-completeness prerequisites.

### F. Channel / ownership review

Connection/statement/thread ownership, command/completion count+byte limits, essential reserve, eventfd wakeups, shutdown, operation/generation validation, no raw pointers.

### G. Failure/recovery and side-effect ordering

SQLite error classification, bounded failure behavior, recovery results, test-only side-effect gate verification, no false completeness/exactly-once claim.

### H. Full acceptance matrix

Each requirement in sections 17–20, named test(s), outcome, any untested environment-dependent case explicitly noted.

### I. Build/test/stress evidence

Exact WSL commands, toolchain versions, warnings-as-errors flags, full CTest count, focused M5 repeated count, deterministic fault injection, fd/thread/lock checks. Explicitly disclose if no sanitizer run.

### J. Deviations, risks and future integration

Any approved clarifications or unresolved issues; M6/M7 ownership integration boundaries and M8 log barrier prerequisites. No semantics changed without approval.

### K. Gate

If and **only if** all required tests, source audits, and contracts passed:

```text
Milestone 5: PASS — awaiting explicit review; Milestone 6 and later milestones have not started.
```

Otherwise:

```text
Milestone 5: NOT PASSED — blockers remain.
```

In either case **STOP**; do not self-authorize the next milestone or merge `main`.

## 24. Final instruction

Begin M5 only on the verified `milestone-5-sqlite-persistence` branch. Implement the frozen SQLite StateStore and bounded single-owner persistence channel faithfully, keep transaction and durability guarantees testable, protect all user files, and produce the exact evidence report before stopping. The existence of accepted M3 protocol and M4 process-runtime code is not permission to integrate them into new Controller/Worker applications during M5.
