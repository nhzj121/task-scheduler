# Milestone 7 Authorization / Codex Development Prompt
## Project: Linux C++ Distributed Task Scheduler

> **Authorized milestone:** M7 — Registration and First Real Vertical Slice.
>
> **Authority:** The explicitly approved frozen architecture, state, ownership, protocol and failure documents remain binding. This document authorizes the construction of the real application composition and a narrowly scoped first end-to-end path, not the remaining M8–M10 functionality. If a material architectural ambiguity cannot be resolved by the documented M7 deferral rules, STOP before implementing the conflicting behavior.

## 0. Authorization, baseline, and completion condition

- Milestones M0–M6 have passed independent review and were merged into `main`.
- Previously verified `main` / `origin/main`: **`daa15ebd03ddfb3945b015ed2dad774fac8e9c79`**.
- Verified M6 final commit: `785269701299767980e84d79716d40d1ddf84265`, ancestor of that mainline merge.
- M7 branch name: **`milestone-7-first-vertical-slice`**. Do not reuse an older milestone branch.
- Source on Windows: `E:\Deck_all_need\projects\work\task_scheduler`.
- WSL source: `/mnt/e/Deck_all_need/projects/work/task_scheduler`.
- WSL build tree: `/home/nhzj121/build/task_scheduler`.
- Preserve `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md` byte-for-byte. Never delete or recreate the entire build directory to clean it.
- Preserve ignored `.local-notes/`, existing uncommitted user data, and all unrelated files.
- M7 is **NOT** a claim that M8's durable log pipeline or M9's complete failure/reconnect handling exists.
- Do not merge to `main`, open an automatically merging PR, or start M8+ after M7 development. Commit, push, report, and STOP for independent review.

## 1. Mandatory Git preflight

Before production edits:

1. Inspect `git status --short`, active branch, `git branch -vv`, remotes, last 12 commits, and ignored/untracked notes.
2. `git fetch origin`; verify the actual `origin/main` SHA and ancestry of the accepted M6 tip.
3. Do not assume the verified baseline remains current if main advanced. Examine any additional commits and STOP if they change the approved baseline or require reconciliation.
4. Check that tracked/untracked personal work is safe. The authorization Markdown file is the only permitted pre-branch uncommitted exception when it was placed in the repository by the user for this authorization.
5. Create `milestone-7-first-vertical-slice` from verified `main` and work exclusively there.
6. If the branch exists or worktree is dirty in an unrelated manner, investigate; do not reset, overwrite, silently stash, or discard user work.
7. No direct commits, merges, or pushes to `main`.

## 2. Mandatory frozen-document reading

Read the complete documents, not merely the listed sections, before implementation:

1. `AGENTS.md`; `docs/ARCHITECTURE_GATE.md`.
2. `docs/spec/Codex_Full_Implementation_Prompt_v1.md` (its original M1-only authorization is superseded only as to current milestone authority).
3. **This full M7 authorization document.**
4. `docs/PROJECT_SCOPE.md`.
5. `docs/ARCHITECTURE.md`: Controller, Worker, client, process preparation, handoffs, module dependencies, event-loop rules, submission/dispatch/terminal sequence.
6. `docs/PROTOCOL.md`: all 26 numeric IDs, role directions, request IDs, strict schemas, register, RUN/ACK/START/FINISH, error messages, log credit.
7. `docs/OWNERSHIP.md`: TCP fd/generation, WorkerSession, process ownership, signalfd, eventfd, destruction sequence.
8. `docs/STATE_MACHINE.md`: Task/Attempt/Worker states, pending finalization, one transition in flight.
9. `docs/FAILURE_MODEL.md`: dispatch uncertainty, Worker disconnection, timeouts, Controller restart, persistence failures.
10. `docs/TEST_STRATEGY.md`; `docs/IMPLEMENTATION_PLAN.md`; `docs/DECISIONS.md`; `docs/DEPLOYMENT.md`.
11. All approved M1–M6 production interfaces and tests, particularly M3 `ConnectionManager`/framing, M4 `PreparedExec`/`ProcessRuntime`, M5 `Channel`, M6 `TaskManager` effects/ports.
12. Relevant project checklists/approved decisions already present in the repository.

Authority order: user-approved frozen contracts above implementation convenience; this M7 document controls implementation scope and acceptance without changing frozen semantics. Any irreconcilable contradiction must be reported with precise file/section references. Do not invent protocol v2 or new durable states.

## 3. Exact M7 goals and non-goals

Deliver **real Linux executables** and reusable modules for:

- `controller`: actual nonblocking listeners for client and Worker roles; decode/validate TCP protocol; own actual M6 TaskManager and WorkerRegistry/ResourceLedger; connect to actual M5 PersistenceThread/channel; perform event-driven orchestration.
- `worker`: actual TCP client registration and heartbeats, decode/run assigned Attempts through the **real M4 process runtime**, emit protocol ACK/START/FINISH observations; own local Attempt lifecycle.
- `taskctl`: real TCP client for `submit`, `get`, `list`, and `workers`, request/response correlation, errors, readable output and optional JSON mode as justified by the frozen client model.
- Component and multi-process loopback integration tests against the real binaries and real SQLite, plus a reproducible local manual multi-terminal demo.
- Strictly bounded, transparently non-durable M7 log-control staging ONLY if needed to satisfy frozen successful registration semantics; see Section 11.

Default local development uses multiple terminal processes on one WSL Linux instance with `127.0.0.1` separate client/Worker ports, not fake in-process RPC. Two Workers may run with distinct `worker_id` and configured resource capacity while sharing physical hardware. No cgroup enforcement is implied.

**M7 does not deliver:** real durable log files, LogWriterThread final sync, general log snapshot service, production-wide reconnect/cancel/timeout orchestration, exactly-once dispatch, multi-host security, HA, systemd packaging or benchmarks.

## 4. Suggested implementation checkpoints

| Stage | Deliverable | Required evidence |
|---|---|---|
| **M7-A** | Application configuration, binaries, startup/shutdown and real socket composition | Three binaries start, invalid configuration fails safely, loopback listeners, SQLite owner and clean shutdown |
| **M7-B** | Controller protocol handlers, real M5/M6 adapters, client queries and Worker registration/heartbeat | Real role/request validation; ACK, liveness; durable submit/get/list/workers; no response before commit |
| **M7-C** | Worker protocol/ExecutionManager, RUN/ACK/START/FINISH path via M4 | Real child exec/reap, request correlation, safe bounded output/read policy, no zombies |
| **M7-D** | End-to-end integration, restart, fault/uncertainty tests and architecture review | Independent Controller+Worker+taskctl processes; real SQLite; no premature terminal; full regression PASS |

Use reviewable commits with meaningful tests. Stage sequencing can change for dependencies, but all four stages must be complete before M7 PASS.

## 5. Dependency boundaries and existing component contracts

- Keep `scheduler -> domain`, `storage -> common/domain`, `protocol -> common/domain`, `net -> common/io`. Application-layer `controller`, `worker`, `client` compose existing lower modules; never reverse dependencies.
- Prefer dedicated `ts_controller`, `ts_worker`, `ts_client` library targets plus executable entrypoints, or an equivalent clean project-consistent arrangement.
- Use existing `ts_net` ConnectionManager and incremental framing, not a second ad hoc socket stack. Transport callbacks borrow input; they must never retain raw borrowed bytes or owning pointers beyond the callback.
- Use typed M3 protocol messages through `encode_message`/`decode_message`; avoid unchecked ad hoc JSON fields or invented message IDs.
- Use M5 `ts_persistence` `Channel::try_post`, `drain`, `status`, eventfd; never call SQLite on the EventLoop thread or reinterpret `eventfd` as proof of commit.
- Use real M6 TaskManager decisions and `PersistencePort`/`EffectPort` adapters. Preserve one metadata mutation in flight per Task; do not duplicate placement, reservation, session invalidation or retry decisions in the application adapter.
- Use real M4 `PreparedExec`/`ProcessRuntime`; do not implement a second fork/exec wrapper. If its current poll interface needs integration, choose a bounded, nonblocking adapter path that preserves event processing without busy-spin and without weakening M4 tests. An unavoidable architecture-changing M4 rewrite requires STOP/approval.
- The app may maintain transport correlation tables, output/buffer budgets, stable connection IDs/generations, and startup restore cursors; these are integration state, not new durable domain state.
- No thread pool. Controller domain decisions belong to one EventLoop thread; PersistenceThread remains the only SQLite owner. Worker process child concurrency is not a thread pool.

## 6. M7-A — Executables, local configuration and lifecycle

- Provide three executable entrypoints and documented command-line usage, `--help`, `--config` where appropriate, clear nonzero exit on invalid startup and read-only/help invocations without side effects.
- Reuse M1 `ControllerConfig`/`WorkerConfig` parsers and their frozen schema/defaults; reject unexpected fields, invalid queue parameters, unsafe endpoints and malformed IDs as already specified.
- Use loopback development defaults from `docs/DEPLOYMENT.md`: client listener `127.0.0.1:7420`, Worker listener `127.0.0.1:7421`. Ports must be configurable and tests use dynamically allocated or isolated ports.
- Do not bind a public interface by accident, nor add TLS/authentication not specified in v1. Note trusted-private-network security boundary.
- Data directory uses explicit trusted absolute local Linux path. Create/verify the directory safely at application startup before M5 StateStore opens its deterministic `metadata.sqlite3` and `controller.lock`. No Controller startup may trample another Controller's lock.
- Ensure signal handling and shutdown are explicit and nonblocking in event dispatch. Unregister listeners, connections, eventfd, signal/timer fds before destroying their owners. Never destroy `ConnectionManager` or EventLoop inside its own callback.
- Worker captures stable startup cwd/environment before launches, arranges `SIGCHLD` ownership with existing M4 behavior, and always reaps owned direct children at shutdown.
- Document local multi-terminal usage; don't require elevated privileges, Docker, remote hosts or Windows-specific binaries.
- Controller must not accept mutating requests or Worker registration as service-ready until M5 initialization/recovery succeeded and M6 recovered Task snapshots are actually loaded.

## 7. Controller startup restore and authoritative read path

- M5 opens SQLite under its own process lock and completes startup recovery **before** reporting Ready. Controller then loads persisted Task/Attempt records using M5 read commands, with bounded pages and operation correlation, **before** exposing a ready service.
- M6 `TaskManager::restore` receives recovered Task/Attempt values. Do not restore live WorkerSessions/reservations; old nonterminal Attempts have already become LOST under M5 recovery.
- Because M5 read commands have bounded result budgets, support read pagination/repeated bounded requests rather than assuming an entire database fits one completion. Handle empty datasets, large histories, stable cursors and injected read failure.
- For client `GET_TASK`, `LIST_TASKS` and `LIST_WORKERS`, distinguish authoritative committed Task/Attempt history and current volatile WorkerSession state. M6 in-memory snapshots are not a substitute for incomplete persisted history.
- Apply `finalization_pending` from the M6 live diagnostic where applicable, but never persist it as a new Attempt state.
- Client read response must not expose an uncommitted transition. If a read is from SQLite, use committed snapshot semantics and stable ordering; if a live snapshot is used, prove it has passed matching commit gates.
- Pagination follows the frozen `(created_at, task_id)` newest-first tuple order and opaque token semantics. A cursor is not a cross-page snapshot. Reject malformed/oversized tokens rather than silently restarting pages.
- If an in-memory M6 restore boundary cannot accept paginated full history consistently, STOP and document the smallest required M6 interface adaptation or safe bounded startup policy. Do not fake recovered rows or drop Task history.

## 8. M7-B — Listener roles, protocol handlers and request correlation

- Separate client and Worker listeners establish fixed roles at accept; never let clients send Worker messages, Worker sockets submit client messages, or an unregistered Worker send heartbeat/RUN responses.
- All input uses M3's exact 20-byte `TSCH` header, version 1, typed 26-message schemas, 4 MiB payload cap and incremental decoder. Do not parse socket chunks as whole frames or resynchronize a bad stream arbitrarily.
- Enforce request-id table semantics: requester nonzero outstanding IDs on a connection, matching response ID, event request ID zero. A `RUN_TASK_ACK` correlates to the actual outbound RUN request/Attempt; client replies correlate to the original connection generation and request ID.
- Produce frozen `ERROR_RESPONSE` codes and schema for parseable client errors; appropriate disconnect on unparseable/malicious wire or protocol-role violations. Do not mutate state on malformed messages.
- Bound per-connection input, output, outstanding request maps, deferred replies and any decoded metadata. Transport backpressure is not an excuse for unbounded queueing or blocking the EventLoop.
- `CLIENT.SUBMIT_TASK`: parse frozen defaults and validate resource/timeout; generate Controller-owned canonical Task UUID; submit to M6; reply `SUBMIT_TASK_RESPONSE` only after M5's matching durable PENDING commit. A lost client response never cancels a committed Task. Do not invent a submit-idempotency key.
- `CLIENT.GET_TASK`: committed Task and ordered Attempt history, including `finalization_pending` only as truthful diagnostic; `NOT_FOUND` for unknown IDs.
- `CLIENT.LIST_TASKS`: default 100, clamp valid large page size to 200, page tokens strict, stable tuple pagination, preserve Task summaries and attempt counts; no SQLite activity in request callback.
- `CLIENT.LIST_WORKERS`: current HEALTHY/OFFLINE data-only WorkerRegistry state, current reserved and running-attempt counts; OFFLINE `session_id=null`, nullable heartbeat and optional concurrency field as frozen.
- For `CLIENT.GET_LOGS` before M8: return a truthful `UNSUPPORTED` error rather than invented bytes, fake snapshots or durable-completeness claims.
- For cancel/retry requests not delivered as M7 application functionality: return truthful `UNSUPPORTED` and never claim cancellation/retry occurred. Do not silently implement all M9 failure semantics.
- Every async response checks the current connection token/generation before sending. Failed send/enqueue is explicit; do not keep a dangling responder or roll back a committed Task because a client departed.

## 9. Worker registration, heartbeat and session replacement

- Worker establishes one real outbound TCP control session, generates a fresh UUID for each actual registration attempt, sends `REGISTER_WORKER` as its first frame, then waits for a real accepted `REGISTER_ACK` before admitting RUN.
- Registration verifies canonical `worker_id`, `session_id`, version, advertised resources and optional concurrency; associates the accepted generation with current `ConnectionId`. No reuse of an old session after reconnect.
- Controller delegates identity, liveness, replacement and bulk loss to M6 WorkerRegistry/TaskManager; do not put ad hoc parallel Worker state in handlers.
- Matching accepted registration sends the complete success-only `REGISTER_ACK` fields, with a genuinely allocated controller-instance diagnostic UUID and correctly configured timeouts/credit. Reject with frozen rejection-only schema when necessary.
- A replacement registration invalidates the old generation immediately. Do not treat replacement as HEALTHY or send accepted ACK until the old session's relevant persisted `LOST` transaction commits and reservations release, and the old connection is handled according to frozen ownership.
- Real heartbeat timer uses monotonic liveness with advancing sequence per session. Wire timestamps do not extend deadlines. Stale/duplicate sequence, wrong Worker/session or old connection cannot renew or mutate the replacement.
- TCP EOF/RST, output failure and heartbeat expiry drive the M6 invalidation decision, retaining reservations until the matching durable LOST completion.
- No invented adoption/reconciliation of old Worker processes and no automatic Task retry.
- M9 owns comprehensive reconnect/partition/cancel/timeout policy. In M7, any implemented retry of the transport itself must use a fresh session and the same conservative LOST guarantees; no false continuance of an old Attempt.

## 10. M7-C — Real Worker ExecutionManager and RUN/ACK/START/FINISH

- On a properly correlated `WORKER.RUN_TASK`, verify current accepted session and task/attempt/Worker identity, enforce local resource and concurrency limits without claiming OS cgroup enforcement, and create a real RunningTask owner keyed by Attempt ID.
- Queue a correctly correlated `RUN_TASK_ACK.accepted=true` in the bounded control output **before** starting launch. If accepted ACK cannot enqueue, fail/close the session and do not launch. A known pre-launch rejection sends `accepted=false` and never forks.
- After accepted ACK, use M4's real PreparedExec rules: environment override/PATH resolution, cwd/stdin preparation, stable argv/envp, no C++ allocation in child after fork.
- Launch with M4 ProcessRuntime, track process IDs, report `TASK_STARTED` only after the exec-status pipe confirms success, and send it before same-Attempt log output.
- Worker handles direct-child reaping, natural exit, same-pgid descendant cleanup, pipe EOF and completion using accepted M4 lifecycle; don't leak zombies or orphan owned subprocesses on orderly shutdown.
- Convert launch preparation/exec failure into `TASK_FINISHED` with `LAUNCH_FAILED` and no false `RUNNING` or success. Distinguish accepted ACK from launch success.
- Generate frozen `TASK_FINISHED` only after real completion and EOF; final counts/sequences must equal actual observed output. No fabricated `TASK_FINISHED`, even for `/bin/true`.
- For M7 no-output supported test path, final log counts/sequences really are zero. The Controller may retain that Finish as an internal pending observation but **may NOT** create fake FinalBarrier or mark Task `SUCCEEDED`/Attempt `SUCCEEDED` without M8 durability evidence.
- Map `RUN_TASK_ACK` -> M6 `AckEvent`, `TASK_STARTED` -> `StartedEvent`, `TASK_FINISHED` -> `FinishedEvent`, honoring one-in-flight persistence decisions and revalidation.
- Adapter dispatch path: on eligible RUN/CANCEL, call `TaskManager::effect_is_current()` immediately before trying transport; record exact dispatch facts through `record_dispatch()` **before queueing bytes** where required; treat ambiguous enqueue/send/partial output conservatively rather than calling `NoBytesQueued` after bytes may have escaped.
- Actual production Worker CANCEL_TASK processing and robust timeout/grace/failover scenarios are M9; if a narrow M7 shutdown requires termination, use M4 cleanup only, not an invented new external cancellation contract.

## 11. Critical M7/M8 boundary: credits, provisional output and terminal visibility

Frozen constraints jointly require:

- Successful `REGISTER_ACK` includes an initial log window; M3 schema rejects a success ACK whose byte and chunk window are **both zero**.
- Log credit is a **capacity reservation**, not a number filled in merely to satisfy schema. Each granted raw byte and chunk slot must be backed by actual reserved Controller-side capacity; zero or exhausted credit must pause Worker stdout/stderr pipe reads rather than discard bytes.
- M8 alone owns the full LogCoordinator/LogWriterThread, durable file append, replenishment from completed writes, two-file `fdatasync`, dirty-directory `fsync`, snapshot logs and complete terminal visibility.

**Permitted narrow M7 integration choice:** For first-slice testing, the Controller may allocate a strictly bounded **volatile, non-durable** staging buffer and byte+chunk slot reservations for the initial credit window. The Worker must enforce BOTH available byte credit and chunk-slot credit before reading/generating a log chunk. The Controller may receive/store credited small-output chunks in that genuinely reserved capacity, validate stream ordering/raw size, and **must not replenish credit without a real completed write to the future M8 data path**. Capacity accounts for all sessions globally (including unconsumed grants and staged bytes/chunks), reserves essential control entries, and rejects further registration/grants if the advertised window cannot actually be reserved. Session invalidation reclaims credits/volatile data only with truthful incomplete-log semantics. This staging is not durable storage or a LogWriter substitute.

For M7's mandatory `/bin/true` flow, no log chunks are generated, and the initial reserved credit remains unused; the payload truthfully reports zero bytes and sequences. High-output tasks may backpressure indefinitely until M8; expose/document this limitation and keep heartbeats/control operational. Do not start general output-workload acceptance under this deliberately limited policy.

**Explicitly forbidden M7 shortcuts:**

- Granting initial credits without backing Controller capacity.
- Allowing Worker to read unlimited stdout/stderr while unable to send; dropping or truncating data; silently counting received bytes as written.
- Hardcoding a positive credit in REGISTER_ACK while retaining no actual credit accounting.
- Fabricating a synced empty-log proof because a workload is named `/bin/true` or emitted zero output.
- Creating `logs_complete=true`, `SUCCEEDED`, `FAILED` for normal observed exit solely from `TASK_FINISHED` when the frozen barrier is absent.
- Using `LogEvidenceKind::ProvenEmpty` for a launched no-output process; it is reserved for explicitly rejected/undispatched Attempt paths.
- Claiming M7 has full lossless-durable logs, GET_LOGS or normal terminal visibility.

M7 may expose `ACTIVE`/`RUNNING` plus `finalization_pending=true` after a real `TASK_FINISHED` observation. Keep that marker private to Controller memory, never a new domain state or SQLite field. Controller crash/restart then conservatively recovers the nonterminal Attempt as LOST. Explicitly document that this is a deliberate **partial vertical slice**, and that terminal completion will be wired to M8.

If exact M3 ACK constraints, M1 config constraints, M6 event handling or global credit reservation cannot be satisfied by the narrow bounded staging approach without violating frozen rules, **STOP with exact conflicting clauses and ask for approval**. Do not silently weaken protocol validation or implement an unapproved replacement log system.

## 12. Resource, fault and operational safety

- Bounded control/log output and input, metadata command/completion admissions, session maps, response maps, startup history paging and provisional log staging. Never block Controller EventLoop on SQLite, child process wait, DNS, disk I/O or uncontrolled sleeps.
- Use existing M3 generation-safe close/unregister/deferred destruction; fd reuse must never revive an old session or client request.
- Adapter port callbacks must be nonblocking and non-reentrant; do not call into TaskManager recursively from EffectPort::emit.
- If persistence status degrades or an essential transaction cannot be admitted: stop new mutations/scheduling, invalidate/close Worker sessions as safely as possible, do bounded cleanup and terminate Controller nonzero under ADR-021. Never silently continue with ambiguous durability.
- `UnknownCommit` never authorizes RUN, a response, terminal state or resource release. A known failed Assign rolls back only tentative reservation; a previously committed assignment is not undone in memory.
- Worker disconnect or uncertain dispatch never implies non-execution; follow M6 `LOST` state and no auto-retry.
- A client disconnect after committed submit does not cancel the Task. Correlation and socket ownership remain generation safe.
- On shutdown join/stop threads outside EventLoop callbacks; no use-after-free on pending persistence results or socket notifications; kill/reap owned child groups and close all fds.
- Tests should explicitly distinguish test-only fault seams from production code and refrain from calling test fakes a real networking guarantee.

## 13. M7-A acceptance: executable composition and configuration

Mandatory assertions:

- Build real `controller`, `worker`, `taskctl` binaries (namespaced variants are acceptable if clearly documented) against the accepted module dependencies.
- `--help` and invalid CLI/config return deterministic exit codes; no listener/SQLite files on help-only invocation.
- Controller binds client and Worker listeners separately to loopback; same data-dir second Controller cannot acquire SQLite ownership.
- Invalid listener address/port, duplicate bind, malformed Worker config, unsafe data directory produce explicit errors and no lingering process.
- Startup recovery and full Task/Attempt history restore precede ready listener behavior. Restart preserves committed PENDING and terminal historical metadata and yields LOST for previously running Attempts without restoring old live sessions.
- Full tear-down closes eventfds, listeners, active sockets, SQLite worker, RunningTasks and signal/timer fds in correct order. One clean start-stop and repeated cycles have bounded fd/thread/process counts.

## 14. M7-B acceptance: Controller and client/Worker TCP

Mandatory real socket tests:

- Client `submit` over TCP with typed frame, response only after M5 committed insert; failpoint-held commit must delay the response.
- GET_TASK returns exact state/history/nullable fields and no fabricated terminal result; unknown IDs return NOT_FOUND.
- LIST_TASKS cursor handles ties, multiple pages and malformed token rejection; taskctl auto-pagination uses actual TCP requests.
- LIST_WORKERS reflects registered HEALTHY session/current reservations, and OFFLINE `session_id:null`; repeated connections/heartbeats do not create fake persisted Worker leases.
- Worker REGISTER is first frame; valid registration produces correctly typed REGISTER_ACK and control identity; duplicate/wrong session or role-invalid message cannot mutate Registry.
- Monotonic heartbeat advances valid current session; stale/wrong sequence/Worker cannot renew it. Heartbeat timer and TCP close trigger conservative session handling.
- Request IDs: original client request survives M5 async completion; concurrent client requests and fd reuse do not cross responses; Worker RUN ACK is matched by outstanding dispatch request ID as well as Attempt/session.
- TCP is truly incremental; single-byte fragmented header/payload and coalesced frames work over loopback; malformed or oversized input fails safely.
- Actual backpressure saturations remain bounded; no socket callback synchronously waits for SQLite.

## 15. M7-C acceptance: Worker execution, wire observations and log boundary

Mandatory real OS tests:

- Controller commits assignment before first RUN_TASK byte is queued to Worker socket; tentative reservation blocks a competing assignment.
- Worker queues accepted RUN_TASK_ACK before launch, then M4 actually launches `/bin/true`; real pid is reported only after exec confirmation.
- Controller commits `ASSIGNED -> STARTING -> RUNNING` in correct order, even when messages arrive ahead of persistence completions.
- Worker reaps the child, observes EOF on both pipes, emits real FINISH with `EXITED`, `exit_code=0`, zero final raw bytes/sequences for no-output task.
- Controller receives and retains FINISH observation but does not persist SUCCESS without M8 log barrier; `finalization_pending` is visible truthfully, while SQLite remains on last committed nonterminal state.
- Inject failed parent preparation and failed child exec: no spurious TASK_STARTED/RUNNING and no zombie/fd leak; reasons and metadata remain truthful.
- Missing/negative RUN ACK, stale session, worker stop, ambiguous send and late observations do not produce fake execution certainty or premature resource release.
- Positive initial credit is actually reserved for every session; no global overgrant; never allow LOG_CHUNK without both reserved dimensions; unsupported high-output workloads are deliberately backpressured rather than dropped.
- Real worker closure and process-group cleanup are bounded and leave no owned direct-child zombie.

## 16. M7-D: automated multi-process and manual local validation

Provide a repeatable integration harness using actual executables (not just in-process fake ports):

1. Create an isolated temporary Controller data root, two local Worker configurations with distinct `worker_id`s, and dynamically assigned ports.
2. Start Controller subprocess, wait for explicit readiness, then start Worker A and Worker B as separate processes. Both should register via real loopback TCP.
3. Use actual `taskctl` subprocess to query Worker listing, submit one or more `/bin/true` tasks and query their IDs; preserve request-ID correlations.
4. Prove deterministic feasible placement, tentative reservation blocking and M5 durability-before-RUN against actual SQLite and TCP observations.
5. Observe real Worker ACK, exec-confirmed START, OS process completion, final FINISH and Controller's truthful pre-M8 finalization hold. **Do not require SUCCEEDED as an M7 acceptance outcome.**
6. Restart Controller with the same DB root. Verify recovered committed PENDING and previously nonterminal ACTIVE Attempt -> LOST/FAILED, no current Worker lease restored; ensure all prior processes have completed/been cleaned up.
7. Exercise malformed role/wire, stale registration, client disconnect during submit, backpressure and an injected failing persistence transaction in isolated subprocess tests; verify explicit response/fail-stop rather than hanging.
8. Terminate every launched process reliably with test timeouts, identify any leaked process group, ensure no fd/thread growth across repeated runs and no test artifacts outside isolated root.
9. Repeat integration scenarios with fixed seeds and report exact iteration counts and duration; explicitly separate simulated fault seams from real TCP/process/SQLite cases.

In addition include a developer manual walkthrough for four WSL terminals: Controller, Worker A, optional Worker B and taskctl; give the real final CLI invocations/config paths implemented, not guessed examples. Include known limits: one physical host, shared CPU/memory, no cgroup enforcement, no M8 terminal success or GET_LOGS.

## 17. Scope exclusions to enforce in diffs

Do NOT implement M8–M10 features as M7 production work:

- Real LogWriterThread, task-log namespace creation, fdatasync/fsync directory chain, snapshot pagination/read, replenished LogWindowUpdate credit, sustained high-volume lossless durable logs, terminal barrier.
- Full production cancellation/timeout/pid-grace/reconnect and failure-recovery matrix, automatic retry, reconciliation of old process execution, exactly-once claims.
- Changing SQLite schema version, persisting Worker leases, new Task/Attempt/Worker state, new 26-message protocol fields/IDs or reinterpretation of response/ACK semantics.
- New queueing architecture or general thread pool; unnecessary M1–M6 rewrite; client HTTP/Web UI; Kubernetes, cgroups, authentication/TLS or public-interface deployment.

If a necessary narrow supporting fix in an accepted module is unavoidable, isolate it, explain the frozen-contract necessity, retain full prior regression tests and request clarification where the architectural boundary would change. No silent scope creep.

## 18. Build, complete regression and stress protocol

Formal environment: WSL Ubuntu 22.04.3, GCC 11.4.0, CMake 3.22.1, C++17, existing strict warning flags including `-Werror`.

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
```

- Retain all accepted M1–M6 test targets. Before any M7 additions, baseline full CTest count was **364**; never treat an altered/skipped old case as passing regression.
- Add dedicated `m7` unit/component/integration CTest labels, meaningful timeouts and isolated process fixtures.
- Run `ctest --test-dir /home/nhzj121/build/task_scheduler -L m7 --repeat until-fail:20 --output-on-failure` after passing full suite; if real process tests are expensive, report exact repeated subset and run required minimum core smoke repeated, do not falsely claim the full set repeated.
- Run actual binaries under external harness, capture logs and exit status; verify no lingering Controller, Worker or child processes.
- Preserve `BUILD_COMMANDS.md`; compare hash before/after. Run `git diff --check main...HEAD` and inspect every modified path and dependency direction.
- Sanitize only if actually installed/runnable; report **not run** rather than inventing ASan/UBSan evidence. Avoid privileged network or filesystem tools.
- No source-only mock of the end-to-end test. Component mocks/fault injection can complement, never replace, a real multi-process run.

## 19. Focused source-review checklist before Gate

Answer with file/line and tests, not only with test counts:

1. Do real controller/worker/taskctl binaries exchange correctly framed role-valid packets via TCP rather than direct fake calls?
2. Does Controller wait for M5 Ready/recovery+M6 restore before serving mutations or Workers?
3. Is every client response and Worker command correlated to the correct stable connection/session/request ID after fd reuse?
4. Does actual assignment commit precede queueing RUN; is dispatch fact recorded before the irreversible boundary?
5. Are M6 tentative reservations released only on known failed assignment, durable terminal or committed bulk LOST?
6. Does Worker ACK before actual M4 launch and START only after exec confirmation, with actual reap/EOF and true FINISH?
7. Does M7 avoid fake final-log barrier, normal `SUCCEEDED`, and fake proven-empty logs for a launched zero-output process?
8. Are REGISTER_ACK positive initial byte/chunk credits backed by real bounded staging reservations, not invented unlimited credits?
9. Are worker session replacement, heartbeat expiry, old messages and late completions generation safe?
10. Does a failed essential persistence command cause actual application fail-stop rather than just a diagnostic M6 Effect?
11. Are all eventfd/epoll/socket/process owners destroyed in safe order without callback UAF, zombies or fd leaks?
12. Are pagination, GET_TASK history, ListWorkers OFFLINE nullable and client errors strict to frozen schemas?
13. Is every new module dependency directional; are M8+ production subsystems absent?
14. Did real multi-process loopback tests pass alongside all 364 prior regressions?

## 20. Explicit STOP conditions and permissible clarifications

STOP for owner review if:

- You cannot provide a valid RegisterAck initial log window without faking credit reservation or implementing an unapproved persistent LogWriter.
- A real `/bin/true` terminal success is being requested without the file+directory barrier; use the documented M7 finalization hold instead.
- Current M4 event-loop/polling APIs cannot be composed without a new blocking thread, busy-spin, losing child SIGCHLD ownership or unsafe fd lifecycle.
- Actual M5 history paging/M6 restore contracts cannot produce consistent startup snapshots without an architectural/interface decision.
- Actual protocol role/response binding requires changing any frozen ID, JSON schema or TCP framing semantics.
- Controller cannot uphold M6 gate/dispatch uncertainty or failure fail-stop when connected to real network and persistence.
- Existing frozen docs genuinely contradict, rather than merely leaving a fixable implementation detail unspecified.

On STOP, report exact conflicting documents, observed interface or reproduction, minimal alternatives and recommendation; do not continue with a guess. Do not ask reauthorization between routine M7-A through M7-D checkpoints.

## 21. Git commit, push and explicit STOP

After all M7 acceptance items pass:

1. Inspect complete branch diff, module dependencies, old regression targets, new tests and actual executable behavior.
2. Use reviewable commits for M7-A/B/C/D as appropriate and add this authorization file and reproducible developer demo documentation.
3. Push **only** `milestone-7-first-vertical-slice`.
4. Confirm local SHA equals remote branch SHA, ahead/behind relative to the M7 tracking branch `0/0`, and `main`/`origin/main` unchanged.
5. Preserve user notes and build-directory file. Do not use force push, auto-merge, main rewrite or broad cleanup of unrelated data.
6. Produce a detailed final report with reproducible tests and source-level evidence, then STOP awaiting independent review.

## 22. Required final M7 acceptance report (A–L)

### A. Git / baseline

Main full SHA, M7 head full SHA, ancestry, branch sync, clean worktree, user data preservation, merge/PR status.

### B. Implementation summary

M7-A/B/C/D exact production capabilities and real executable names, not future desired features.

### C. Files and dependencies

Every added/modified/deleted file; CMake targets and layering; any accepted lower-module fix isolated and justified.

### D. Controller application wiring

Listeners/roles, eventfd, M5 Ready+restore, M6 bridge, completion timing, fail-stop and shutdown.

### E. Client and CLI

Actual command examples, request/response correlation, submit, get, pagination, workers, truthful unsupported operations.

### F. Worker runtime and wire flow

Registration, heartbeat, RUN/ACK/START/FINISH, real pid/exec/reap/EOF, group cleanup and session ownership.

### G. Pre-M8 durability boundary

Exact initial credit reservation scheme, memory/slot limits, log byte/sequence accounting, unsupported higher-output limitations, absence of fake barriers, finalization hold.

### H. End-to-end acceptance matrix

Each mandatory real multi-process assertion and the precise code/test proving it.

### I. Failure, security and operational cases

Wrong roles, malformed input, port/bind failures, lost clients, session loss, persistence degraded, exit/shutdown, default loopback risk boundary.

### J. Formal WSL build, tests and stress

Exact commands, total CTest count, preexisting 364 baseline, M7 test count, repeated tests, real-process harness iterations, any sanitizer outcome, fd/child leak evidence.

### K. Deviations, known limits and deferred work

Document pre-M8 terminal finalization restriction, non-durable staging, absent GET_LOGS/full cancel/retry/reconnect and M8/M9 integration requirements; no broad guarantees inferred from smoke tests.

### L. Gate

If and only if **all documented M7 criteria** hold, conclude:

**Milestone 7: PASS — awaiting explicit independent review; Milestone 8 and later milestones have not started.**

Otherwise:

**Milestone 7: NOT PASSED — blockers remain.**

Then **STOP**. Never merge `main`, start M8+, or promise to finish later in the background.

## 23. Final instruction

M7-A through M7-D are expressly authorized in this document. Begin on the verified `milestone-7-first-vertical-slice` branch, build the first truthful real loopback Controller/Worker/taskctl slice, carry forward the frozen commit/ownership/log guarantees, and stop after committing/pushing the M7 branch for independent review.
