# Milestone 9 Authorization / Codex Development Prompt
## Project: Linux C++ Distributed Task Scheduler

**Authorized milestone:** M9 — Cancellation, Timeout, Failure, Reconnect, and Recovery

**Repository:** `nhzj121/task-scheduler`

**Development branch:** `milestone-9-cancellation-recovery`

**Accepted `main` baseline (verified remotely):** `c18014eb474c5bcff9e9916fe648a760b0d631fa`

**Accepted M8 implementation HEAD:** `75677e5cca70e153891c1a5600e8000dea365101`

**Authorization:** Full M9-A through M9-D only. M10 is **not authorized**.

**Required outcome:** Implement the frozen M9 feature/failure matrix, validate it with actual Linux processes, TCP, SQLite, and durable logs, commit and push the M9 branch only, produce an A–L report, and **STOP before merging**.

---

## 0. Authority, requirements and conflict handling

This document authorizes implementation of all M9 checkpoints, not alteration of the architectural contract. The following remain authoritative:

- `AGENTS.md` and `docs/ARCHITECTURE_GATE.md`;
- `docs/spec/Codex_Full_Implementation_Prompt_v1.md`;
- `docs/PROJECT_SCOPE.md` and `docs/IMPLEMENTATION_PLAN.md`;
- `docs/ARCHITECTURE.md`, `docs/STATE_MACHINE.md`, `docs/OWNERSHIP.md`;
- `docs/FAILURE_MODEL.md`, `docs/PROTOCOL.md`, `docs/DECISIONS.md`;
- `docs/TEST_STRATEGY.md`, `docs/DEPLOYMENT.md`;
- the accepted M5–M8 source, formal tests, reports and local demo instructions.

For any contradiction, follow the frozen document hierarchy in `AGENTS.md`; do **not** invent a new state, packet, database column, retry rule, or session-lease guarantee. Identify the precise conflicting sections and STOP for review if a correct implementation cannot be obtained under the frozen contract. Do not mistake a prior milestone's intentional limitation for a frozen prohibition: M9 is explicitly authorized to enable cancellation, retry, reconnection and failure paths previously left unsupported in M7/M8.

**Invariants:** one EventLoop decision owner, one SQLite owner, one LogWriter owner; durable transition before its irrevocable external effect; Task and Attempt lifetimes are separate; at most one nonterminal Attempt per Task; terminal Attempt history is immutable; no blind replay or exactly-once claim; output never intentionally dropped; native Linux child process groups are explicitly owned and reaped; network identities and asynchronous completions are generation-fenced.

## 1. Git, source and build preflight

Before production changes:

1. Inspect `git status --short`, branch, HEAD, remotes, merge-base, and `git log --graph`.
2. `git fetch origin` without rewriting history. Confirm `main` and `origin/main` contain accepted M8 HEAD `75677e5cca70e153891c1a5600e8000dea365101`; expected accepted merge baseline is `c18014eb474c5bcff9e9916fe648a760b0d631fa`.
3. Create `milestone-9-cancellation-recovery` from the accepted mainline. If the remote mainline has legitimately advanced, explain whether those changes can be safely incorporated before proceeding. Do not silently rebase against unreviewed work.
4. Work exclusively on this branch. **Do not** merge, push, force-push or reset `main`; do not create a self-merging PR.
5. Preserve ignored `.local-notes/`, all personal documentation and unrelated user files. Preserve `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md` **byte-for-byte**; the build directory is intentionally outside the source tree.
6. The owner-provided `docs/spec/Milestone_9_Authorization_Codex_Development_Prompt.md` is the **single permitted pre-existing untracked/modified authorization artifact**. If it was copied into the working tree before branch creation, verify its content, carry it onto the M9 branch without overwriting unrelated files, and commit it as documentation. Never use `git clean -fdx`, a broad `git reset --hard`, or directory-wide removal of the WSL build tree.
7. Treat an unexpected dirty working tree or unknown remote history as a blocker; preserve evidence and explain it rather than destroying changes.

**Established environment:** Windows source `E:/Deck_all_need/projects/work/task_scheduler`, WSL source `/mnt/e/Deck_all_need/projects/work/task_scheduler`, dedicated WSL build `/home/nhzj121/build/task_scheduler`, Ubuntu 22.04.3 / GCC 11.4 / C++17 / CMake 3.22.1 / SQLite 3.37.2. Match actual environment if it differs; do not claim a tool/test ran without recorded output.

## 2. Existing production surface: read and reuse, do not duplicate

Read current implementations and tests before editing:

- `include/ts/controller/task_manager.h`, `src/controller/task_manager.cpp`: `CancelEvent`, `RetryEvent`, `CancelAckEvent`, `EffectKind::{CancelEligible,CancelAccepted,CancelCompleted,RetryAccepted}`, per-Task pending metadata command, correlation and generation validation, `record_dispatch`, `effect_is_current`, `tick`, session loss and completion gating.
- `include/ts/controller/worker_registry.h`, `src/controller/worker_registry.cpp`: current SessionKey, heartbeat monotonic deadline, `invalidating`, replacement registration, session tombstones, delayed replacement ACK.
- `src/controller/application.cpp`: current unsupported Cancel/Retry branch, current fatal-on-`CancelEligible` path, `Peer::runs`/`observations`, TCP `request_id`, generated Effects queue, M5/M8 eventfds, shutdown and startup restore.
- `include/ts/worker/execution_manager.h`, `src/worker/execution_manager.cpp`: accepted RUN lifecycle, Attempt→ProcessRuntime ownership, timeout, process-group/pipe cleanup, M3 output sequencing and M8 credit.
- `src/worker/application.cpp`: M7 single-session state machine, registration/heartbeat, current connection-loss fail-stop, M8 log grants.
- `include/ts/runtime/process_runtime.h`, `src/runtime/process_runtime.cpp`: `ProcessRuntime::cancel`, timeout beginning at exec-confirmed START, first-cause-wins behavior, group SIGTERM/grace/SIGKILL, pid ownership and reap.
- `include/ts/protocol/messages.h`, `src/protocol/messages.cpp`: all already-frozen Cancel/Retry/WorkerCancel schemas and response types; **no new protocol or version**.
- M5 `StateStore` and `PersistenceThread`, M6 `ResourceLedger`, M8 `LogCoordinator`, `LogCreditLedger`, `LogWriter` and network output dependency/priority support.
- `tests/integration/m7_loopback.py`, `tests/integration/m8_loopback.py`, existing M3–M8 unit/component/fault hooks and `docs/M8_LOCAL_DEMO.md`.

The existing production core already implements much of the M9 decision logic. The major work is correct application/transport wiring, Worker lifecycle and reconnect integration, exact response semantics, and a complete fault/race evidence matrix. Favor narrow, testable adjustments; do not rewrite accepted modules as convenient shortcuts.

## 3. Scope and disallowed expansions

**Allowed:** existing Controller/Worker/TaskManager/Registry/ProcessRuntime/StateStore/LogWriter composition, taskctl Cancel/Retry, generation-scoped Worker reconnect, deterministic fault hooks/tests, cluster integration, necessary configuration validation and M9 demo documentation.

**Forbidden:** M10 systemd packaging, new deployment/CI/benchmark layer, TLS/auth, cgroups, security sandbox, new task state such as `CANCELLING`, database schema v2, new wire message IDs, a new persistence owner, a second log connection, an unbounded queue or detached threads, automatic requeue/retry after `LOST`, Worker process adoption, arbitrary workload reconciling, exactly-once claims, live/follow logs, retention/quotas or remote storage. Do not alter Scheduler placement rules or weaken M8 data/namespace barriers.

No broad architectural refactoring is authorized. When a genuine M1–M8 contract defect prevents M9, repair it in the smallest justified diff and add a targeted regression; describe it explicitly in the A–L report.

## 4. Suggested M9-A through M9-D checkpoints

These are **implementation checkpoints, not intermediate merge approvals**:

- **M9-A — Client cancellation/retry and Controller command delivery:** typed CLI/client requests, durable-before-response and durable-before-CANCEL semantics, cancellation request/ACK correlation, deterministic controller tests.
- **M9-B — Worker cancellation/timeout and competing completion:** accept typed cancel, map exact Attempt to ProcessRuntime owner, first-cause-wins termination, ACK not completion, preserve FINISH/log barriers, process-group/race tests.
- **M9-C — Session failure, replacement and reconnect:** Controller liveness/deadline/uncertainty routing, old session LOST transaction/release, Worker local cleanup before new UUID registration, generation/late-message fencing, actual TCP fault tests.
- **M9-D — Restart/fail-stop/retry/fault matrix:** crash/restart and recovery, bounded persistence/log failures, repeated active/terminal histories, full cluster test matrix and formal regression.

Suggested commits: Cancel/retry adapter; Worker termination races; reconnect/session integration; recovery/fault test hardening; final report/demo. Commit names and count are not a fixed interface.

## 5. M9-A: frozen `CLIENT.CANCEL_TASK` response contract

Wire protocol:

- `CLIENT.CANCEL_TASK` (`0x0109`) carries `task_id`.
- `CLIENT.CANCEL_TASK_RESPONSE` (`0x010A`) carries `task_id`, `status` in `accepted|completed|already_cancelled`, `task_state`, `cancel_requested_at`.

Enforce every case with **real** M5 persistence:

1. **PENDING:** apply `CancelPending` atomically, return `completed/CANCELLED` only after commit; no Attempt, no resource reservation, no Worker command.
2. **ACTIVE with ASSIGNED and proved `NoBytesQueued`:** use M6's existing `ProvenEmpty` terminal transaction without emitting a Worker cancel. Do not claim no bytes queued merely because a TCP queue currently appears empty; account for previously queued/possibly escaped RUN bytes and the `RunEligible`/dispatch facts.
3. **Other ACTIVE:** persist `cancel_requested_at` via `CancelIntent` and keep Task `ACTIVE`. Only after successful commit return `accepted/ACTIVE` and authorize `WORKER.CANCEL_TASK` to the current Attempt/Session.
4. **ACTIVE with existing committed cancel intent:** return `accepted/ACTIVE` idempotently; do not issue a duplicate intent transaction or multiply sends. Be explicit about pending initial intent transaction: serialize subsequent requests and avoid precommit `accepted` responses.
5. **Already CANCELLED:** return frozen `already_cancelled` status; do not modify history, send another Worker command or fabricate another completion.
6. **SUCCEEDED/FAILED:** return `ERROR_RESPONSE/CONFLICT`; unknown Task returns `NOT_FOUND`; any other invalid/stale request is rejected using the frozen error vocabulary.
7. If the client disconnects after sending a request, the durable operation may proceed; never roll it back because the response cannot be delivered. On a lost response, client can query and retry/revalidate.

Maintain one authoritative in-flight metadata mutation per Task. Correlate callback effects with the correct client connection **generation and request ID**; a reused numeric fd or stale request ID must never receive a later response. Never use a synchronous SQLite call on EventLoop.

The existing `CancelAccepted` and `CancelCompleted` M6 Effects are output notifications, not permission to bypass commit ordering. Generate the response from the persisted/current Task row; avoid incorrect `cancel_requested_at` values for the proven-empty or pending paths. No ambiguous double response (e.g., accepted immediately and completed after terminalization) on a single request.

## 6. M9-A: frozen `CLIENT.RETRY_TASK` contract

Wire protocol: `CLIENT.RETRY_TASK` (`0x010B`) and `CLIENT.RETRY_TASK_RESPONSE` (`0x010C`).

- Only a **durably `FAILED`** Task may retry. `SUCCEEDED`, `CANCELLED`, `ACTIVE`, `PENDING`, unknown or inconsistent states must not be silently retried.
- Persist `FAILED -> PENDING` atomically; clear cancel intent; preserve all prior Attempt rows, old Attempt UUIDs, sessions, reasons, log paths and the historical `latest_attempt_id` until a new assignment commits.
- Return `PENDING` and the correct `next_attempt_no` only after the Retry commit. Derive that number from prior immutable Attempt history, checked for overflow; do not reuse an Attempt ID/number or silently narrow a uint32.
- Scheduling then creates a **new Attempt** only after a fresh healthy Worker is eligible and the new Assignment transaction commits. The previous Attempt remains terminal and immutable; its complete logs remain queryable by Attempt number.
- If the old Attempt was `LOST`, retry is user-initiated and may overlap a still-running old process during failure detection. Explicitly document the duplicate-execution risk. There is **no automatic retry** on loss, reconnect, restart, timeout or Worker rejection.
- Duplicate concurrent retries must serialize: once the first commit makes Task `PENDING`, a second must not fabricate a second acceptance or additional Attempt.
- On persistence error/unknown commit, do not report durable success, do not schedule based on ambiguous state, and follow the M5/M6 fail-stop model.

## 7. M9-A: actual Controller CANCEL delivery and ACK correlation

Implement `EffectKind::CancelEligible` in `src/controller/application.cpp` using the existing M3 control-output path and `WORKER.CANCEL_TASK` (`0x020A`): exact `worker_id`, current `session_id`, `task_id`, `attempt_id`, configured `grace_ms`.

- Recheck `TaskManager::effect_is_current(effect)` **immediately before enqueue**. Verify Task/Attempt binding, SessionKey generation, connection identity, live/registered peer, cancel intent committed, and terminal/finalization phase. A stale Effect sends nothing.
- Use a distinct, bounded per-connection cancel request-correlation map (`request_id -> AttemptId, SessionKey/generation, TaskId` or equivalent), not ad hoc reuse of RUN ACK identity. Respect reserved control-output and per-peer request capacity; reject duplicate pending commands or make repeat handling explicitly idempotent.
- Maintain a clear invariant: `CANCEL_TASK` must **not** enter the output queue before a matching `CancelIntent` transaction commits. For PENDING/local-ProvenEmpty cancel there must be **no** Worker command.
- Treat a failed/partial send, connection loss, or unknown delivery as uncertain remote execution; invalidate current Session and run frozen `LOST` handling. Do not reinterpret uncertain cancel delivery as `CANCELLED`.
- Validate `WORKER.CANCEL_TASK_ACK` (`0x020B`): header request ID, Worker ID, Session UUID, Attempt ID, accepted/already-terminal, exact current peer and request generation. Only a matching ACK may clear a pending cancel-control request; duplicate/stale/role-wrong ACK must not refresh a lease, alter state or release resources.
- `CancelAckEvent` documents command handling **only**; never makes a Task terminal. `TASK_FINISHED`, real M8 barrier and SQLite commit remain authoritative.
- ACK `accepted=false` or `already_terminal=true` must not be rewritten as proof of process cancellation. Await the real terminal observation or follow frozen session failure policy if the observation becomes unavailable. No unwarranted invented retry or new state.
- If a cancel arrives after FINISH is already authoritatively observed/finalizing, preserve the result that won EventLoop ordering even if SQLite terminal transaction is pending. Neither cancellation nor ACK may replace a frozen natural outcome.

## 8. M9-B: Worker `CANCEL_TASK` handling and lifecycle

Integrate `WORKER.CANCEL_TASK` reception in `src/worker/application.cpp` and `ExecutionManager` with M4's existing `ProcessRuntime::cancel(RuntimeProcessId)`. The Worker must validate current session identity, current Task/Attempt binding and phase before touching a child.

- Maintain an explicit Attempt→RuntimeProcessId association for only currently owned work; do not use PID alone as identity, do not signal a reused PID, and never act on a previous session's Attempt.
- For a valid active Attempt, queue `WORKER.CANCEL_TASK_ACK` before or as part of initiating termination in the frozen ACK-handled sense; an accepted ACK confirms handling, not completion.
- Invoke the existing runtime's process-group `SIGTERM -> grace -> SIGKILL`, exact reap, stdout/stderr final drain and `TASK_FINISHED` generation. No `kill(pid)` shortcut; do not leak zombies or group members.
- An Attempt already reaped/terminal or whose natural outcome is frozen must ACK with correct `accepted`/`already_terminal` semantics without changing that natural result. Duplicate CANCEL requests should be safely handled without restarting grace timers or signalling an unrelated group.
- Preserve the **first installed cause** among cancellation, timeout and natural leader outcome. A late cancel cannot replace `TIMED_OUT` or a frozen natural `EXITED/SIGNALED` outcome. A late timeout cannot rewrite a prior cancel or a leader that has already naturally exited.
- Preserve M4 child-after-fork async-signal-safety, exec-status confirmation, signal masks, and parent-side prepared-exec ownership; do not rebuild or duplicate the child runtime.
- Keep all M8 log credit, pause/resume, binary output, per-Attempt FINISH wire dependencies and LogWriter barrier semantics. Cancel/timeout may generate final logs; never discard credited output or report terminal status before the full finalization path.
- If the session is gone, terminate/reap children and diagnose/discard any undeliverable output as incomplete—never claim that it was transmitted, written or durably synchronized.
- No new network control channel and no blocking EventLoop/synchronous `waitpid` loop.

## 9. M9-B: timeout ownership and cancellation/timeout races

- `timeout_ms` begins at **exec success confirmation / TASK_STARTED**, never at assignment, accepted ACK or fork start.
- Deadlines use the existing monotonic Linux timer/fd-native runtime, not wall-clock timestamps or a periodic polling sleep that changes semantics.
- Timeout first cause -> `TASK_FINISHED` outcome `TIMED_OUT`; corresponding Attempt `TIMED_OUT`, Task `FAILED`, **after** real log barrier and atomic SQLite commit.
- Cancel first cause -> `CANCELLED`/`CANCELLED` after the same cleanup/barrier/commit. Natural exit first -> observed exit/signal result, regardless of late cancel/timeout. After natural leader exit, descendant group cleanup signals must not rewrite its result.
- Start/ACK missing deadline is a **session-loss decision**, not a workload timeout; mark its Attempt `LOST`/Task `FAILED` only after required session-loss transaction, not `TIMED_OUT`.
- Delayed/stalled log writes can keep `finalization_pending=true`, `logs_complete=false` and committed reservations held. Neither cancel ACK nor timer firing permits premature release.
- Test cancellation during `ASSIGNED`, `STARTING`, `RUNNING`, during exec failure, while stdout/stderr pipes are credit-paused, during final log barrier, and after terminal SQLite commit.

## 10. M9-C: Controller Session loss, deadline and replacement ordering

Use existing M6 `WorkerRegistry`, `TaskManager::tick`, `invalidate`/`connection_closed`, and M5 `LoseSession`. Close/reset, heartbeat expiry, missing RUN ACK, missing TASK_STARTED, protocol errors and replacement registration must converge to the documented **session invalidation** path:

1. Mark the old session invalid/stop placement and stop new RUN/CANCEL/log credit for it.
2. Fence old connection and generation (including already queued but not-yet-executed Effects).
3. Wait for previously accepted old-session metadata commands to resolve; do not race a batch LOST against a pending assignment/transition.
4. In one durable SQLite transaction terminalize all old nonterminal Attempts as `LOST`, Tasks as `FAILED`, with `logs_complete=false`.
5. **Only after commit** release reservations, expose OFFLINE/terminal results, close the old connection, and allow a replacement Session to receive `REGISTER_ACK`.
6. Reclaim unused old log credit and fence late log writes, file barriers, ACKs and completion notifications; none may promote LOST to SUCCEEDED or replenish a replacement's capacity.

A missing RUN ACK after possibly escaped bytes is not dispatch rejection; a missed start-report deadline is not proof that launch never occurred. `LOST` asserts lack of authoritative observation, not confirmed host/process death. A delayed heartbeat after invalidation cannot resurrect the session. An old `worker_id` may register with a **new UUID** while the prior TCP connection is open, but it must not become HEALTHY until old-session cleanup commits.

A replacement peer may disconnect while waiting. Maintain generation/session tombstones; never activate an already disconnected replacement or reuse its old UUID. The `WorkerRegistry` lives in memory, not in SQLite; startup restores **no** live sessions or reservations.

## 11. M9-C: actual Worker local cleanup-before-reconnect

M8 Worker currently stops on connection loss. M9 authorizes reconnection **only after actual old-session local cleanup**. The Worker must:

1. Detect Controller TCP close/reset, registration rejection, or session invalidation as appropriate to the frozen protocol; stop accepting RUN/grants/cancel from that old session.
2. Explicitly initiate termination of every old RunningTask process group, continue nonblocking reaping, pipe draining or diagnostic incomplete-output handling, and close every owned fd/registration. Do not detach an owned runtime or leave an old child group running.
3. Drop old TCP output queues, per-session credits, pending RUN and CANCEL response correlation, parser state, and old Session UUID/Controller instance binding. Never replay a previously sent Task or log frame into the new session.
4. **Only after cleanup has finished**, generate a **fresh session UUID**, establish a fresh TCP connection, issue `REGISTER_WORKER`, and wait for a new correlated `REGISTER_ACK` before new work.
5. Reset heartbeat sequence and valid session-scoped log credit; install newly advertised credit only after the new registration ACK. Do not trust a prior Controller instance ID or reuse an old Session ID.
6. Handle repeated short disconnects, eventual Controller restoration, and bounded connection-admission/backoff without busy spinning or unbounded memory. Do not invent a new persisted lease or adoption RPC. Any terminal unrecoverable condition exits nonzero with explicit cleanup rather than silently continuing in an ambiguous half-registered state.

Carefully review existing `ExecutionManager` and `ProcessRuntime` destruction order and SIGCHLD / SIGINT / SIGTERM masks. The old runtime's owner must finish cleanup before replacement runtime/session registration; use only the accepted three-thread Controller architecture (the Worker is its own EventLoop process). If a clean in-process reconnect lifecycle is incompatible with the accepted owners, STOP with exact evidence instead of launching a second session while old children still exist.

Controller restart must be equally conservative: a running Worker detects loss, cleans up, then may register to the new Controller instance; the new Controller never asks for or adopts old PIDs or Attempt results.

## 12. M9-D: startup recovery and post-restart behavior

Preserve existing startup order before opening listeners:

- exclusive Controller data-directory ownership, SQLite schema version and WAL/FULL settings;
- one recovery transaction converting all persisted nonterminal `ASSIGNED/STARTING/RUNNING` Attempts to `LOST`, corresponding ACTIVE Tasks to FAILED and `logs_complete=false`;
- no modification of committed terminal Task/Attempt rows; preserve PENDING Tasks and historical retry/terminal logs;
- rebuilding TaskManager pending order without restoring Worker sessions, old reservations or uncommitted finalization facts;
- opening listeners only after successful recovery and state restoration; new registration uses fresh generation and session ID.

Never infer terminal success from orphan log file tails, a partially written final barrier, a prior Worker ACK, or a successful file sync lacking the corresponding terminal metadata commit. Log snapshot paths and proven-empty rules stay exactly those accepted in M8.

Test restart at before/after assignment commit, possible RUN byte enqueue, cancel-intent commit, log barrier, terminal metadata COMMIT and resource release; verify durable predecessor facts rather than manufacturing remote certainty. Include client disconnect during pending Submit/Cancel/Retry responses and ensure no double mutation.

## 13. M9-D: failure classification and fail-stop guarantees

Preserve M5/M6/M8 distinctions:

- Metadata transaction failed, `BUSY/FULL/IOERR`, unclear COMMIT/`UnknownCommit`, required persistence channel degradation: no corresponding irreversible RUN/CANCEL/client-accepted response; stop mutating scheduling, invalidate/close sessions, perform only bounded safe cleanup if DB is still usable, and exit nonzero. Never blindly retry a possibly committed transaction.
- Log data write/open/directory creation failure **before** reliable FINISH: invalidate session, commit LOST if possible; do not invent an observed child result.
- Log append/barrier failure **after** reliable FINISH: if SQLite is healthy, terminalize `FAILED`, reason `LOG_PERSISTENCE_ERROR`, keep the actual observed outcome diagnostic and `logs_complete=false`; no false success.
- Failure of stdout or stderr `fdatasync`, or required containing-directory `fsync`: same no-success log failure semantics; preserve dirty-directory and log-path ownership guarantees.
- Read-only log snapshot failure: `INTERNAL` response, no Task state rewrite.
- Malformed Worker frame, wrong Session, wrong request correlation, FINISH count mismatch or late LogChunk: reject and invalidate the affected session, follow LOST semantics, rather than pretending a task was completed.
- No uncontrolled retries, no queue spin, no `sleep` on Controller EventLoop, no unbounded completion backlog, no dropped credited output, and no release of a tentative/committed reservation before its allowed durable event.

Avoid claiming SQLite + task-log filesystem + network form one ACID transaction. Check exact ownership and completion generation, including old log writer results arriving after session replacement and late terminal COMMIT effects.

## 14. Client CLI and local demo

Enable existing CLI commands rather than inventing a second client:

```text
 taskctl cancel TASK_UUID
 taskctl retry TASK_UUID
 taskctl get TASK_UUID
 taskctl list
 taskctl workers
 taskctl logs TASK_UUID --attempt N --both
```

Match the current CLI global `--host`, `--port`, `--json` convention; keep shell-free argv task execution. Human output should clearly differentiate cancel accepted vs completed vs already cancelled; JSON output must use the frozen typed response and canonical enum strings. `retry` should display the next Attempt number. Errors go to stderr/nonzero exit according to existing taskctl conventions.

Update the existing M8 demo with a separate `docs/M9_LOCAL_DEMO.md` showing actual Controller + two Workers + taskctl in multiple WSL terminals, plus commands for pending cancel, active cancel, timeout, manual retry, Worker kill/restart and Controller restart. Mark all commands as **actually tested** or identify illustrative placeholders; use example configs as they exist rather than fabricating flags.

## 15. Acceptance matrix — M9-A client, commands and persistence

Tests must use real production logic, not an in-memory pseudo database to claim durability. Include at least:

- PENDING cancel commits first and creates no Attempt; no Worker CANCEL and no reservation.
- No-dispatch ASSIGNED cancellation with true `NoBytesQueued` proves empty streams; if any byte may escape, cancel intent is persisted instead and remote execution remains possible.
- ACTIVE cancel sends `accepted/ACTIVE` only after durable intent; Task stays ACTIVE, Attempt nonterminal and reservation retained until final log barrier + metadata transaction.
- Repeat cancel while intent already durable is idempotent, no duplicate mutation/response; a second client cancel during first pending intent is serialized correctly.
- Existing CANCELLED -> `already_cancelled`; SUCCEEDED/FAILED -> `CONFLICT`; missing Task -> `NOT_FOUND`.
- Client close and reused fd/request ID cannot receive stale Cancel/Retry response, yet durable operations remain unaffected.
- Captured Controller wire trace proves zero `WORKER.CANCEL_TASK` bytes before cancel intent COMMIT, including held SQLite hook, `UnknownCommit` and failed-commit cases.
- `WORKER.CANCEL_TASK` has exact current Task/Attempt/session/generation and a valid `grace_ms`; stale Effects issue no traffic.
- ACK request ID/Attempt/worker/session correlation, duplicates, wrong generation, already-terminal and invalid ACK fields are rejected; ACK never authorizes Task terminal visibility.
- Retry only FAILED -> PENDING after COMMIT; preserve old Attempt ID/number, path, result and log history; next assigned Attempt is exactly `max_old + 1`; duplicate retry never creates two Attempts.
- Rejection/unknown durability/constraint errors never produce success or permit scheduling/side effects.

## 16. Acceptance matrix — M9-B termination, timeout and races

Use real Linux child processes and process-group observations:

- Worker executes long-lived process; active Cancel ACK occurs, process group SIGTERM then grace/SIGKILL as needed, child reaped and logs complete before `CANCELLED` terminal commit.
- Noncooperative TERM workload forces SIGKILL after grace, with bounded timer-based progress and no direct-child zombies.
- Natural exit wins before cancel: original `SUCCEEDED/FAILED` and leader exit/signal facts are retained, even if a later cancel was accepted or ACKed. No phantom CANCELLED.
- Timeout starts on exec confirmation; before START, a missing start report maps to session LOST rather than task TIMED_OUT. After START, runtime timeout produces Attempt TIMED_OUT / Task FAILED.
- Race matrix: cancel-before-timeout, timeout-before-cancel, natural-exit-before-cancel, cancellation during launch/setup failure, completion during a held LogWriter barrier, and cancel after durable terminalization.
- Logs produced before cancellation/timeout are exact, binary-safe and synchronized before observed terminal state. Under zero credit, control packets/heartbeat/cancel are still serviced, while child pipe pressure is bounded.
- Worker rejects stale/wrong-task/wrong-session commands without signalling another Attempt or a reused PID.
- Parent/child and descendant pgid cleanup behaves as frozen; escaped `setsid` descendants and uninterruptible processes remain documented non-guarantees, not invented success claims.

## 17. Acceptance matrix — M9-C liveness and reconnection

Use real TCP, Worker process and separate Controller processes where possible:

- Idle Worker disconnect -> OFFLINE after authoritative session loss handling -> Worker reconnect with a fresh UUID -> HEALTHY, without old session adoption.
- Running Worker forced TCP reset/close -> local group SIGTERM/grace/SIGKILL and reap **before** new registration; old Attempt LOST/Task FAILED; reservation release only after durable SQLite session-loss commit.
- Registration of same `worker_id` while old session still exists invalidates the old Session, waits for one bulk LOST commit and closes old socket before sending the new REGISTER_ACK.
- Replacement disconnect while awaiting old invalidation cannot later become HEALTHY; old/new generation and UUID tombstones stay fenced.
- Frozen/late heartbeat or old ACK/START/FINISH/log completion never revives an invalidated Session or mutates terminal history.
- Pause heartbeats while TCP stays open and verify monotonic deadline invalidation. Missing RUN_ACK and missing STARTED deadlines each cause LOST and session invalidation; explicit RUN rejection causes FAILED with ProvenEmpty logs instead.
- Kill Worker during RUNNING; verify Controller LOST/FAILED and reservation accounting. Start a new Worker or reconnect safely; never auto retry the old Task.
- Kill Controller while a task is running; Worker cleans up and reconnects after Controller restart; new Controller recovers old active Attempt as LOST/FAILED, leaves PENDING/terminal Tasks intact and does not restore old sessions.
- Repeated disconnect/reconnect under churn uses bounded memory/fds/threads; all old direct children reaped; no port/socket reuse miscorrelation.

## 18. Acceptance matrix — M9-D crash, persistence, log and regression

The acceptance suite must cover **every applicable** scenario in `docs/TEST_STRATEGY.md` cluster integration and fault matrix, including the accepted M7/M8 scenarios. M9-specific new evidence is required for:

- One Worker happy path, multi-Worker resource choice, pending scheduling with no Worker, status/list/log pagination and stable finite snapshots.
- PENDING cancel, ACTIVE cancel and repeat, cancel-vs-exit/timeout races, explicit timeout mapping, failed Task manual retry preserving Attempt history.
- Client disconnect/unknown response, idle/running Worker disconnect/reconnect, replacement and heartbeat/ACK/START deadlines.
- Controller crash/startup recovery; failure before/after assignment, cancel intent, log barrier, terminal SQL commit, reservation release; no claim that SIGKILL simulates hardware power loss.
- M5 fault hooks (`BUSY`, `FULL`, `IOERR`, unknown commit), M8 log write/open/fdatasync/fsync faults, eventfd notify or completion failures, frame malformation and wrong-correlation faults.
- Backpressure isolation: sustained large stdout/stderr + multiple Workers under finite credit does not hide control events or lose raw log bytes.
- FINISH partial TCP write dependency: no final frame byte can overtake an earlier chunk; final counters match Writer-visible and SQLite-stored values after barrier.
- No unintended worker process group surviving cleanup, no zombie, fd/thread growth or leaked EventLoop registration after repeated init/shutdown/reconnect.
- Incomplete/LOST logs remain incomplete; ProvenEmpty only for actual nonexecution; complete history and retry numbering survive restart.

Always distinguish actual runtime/fault evidence from source assertions and Fake Port tests. Add dedicated deterministic seams for difficult races rather than timing-sensitive sleeps inside production code.

## 19. Mandatory adversarial review checklist

Review the final source diff with all of the following questions answered explicitly:

1. Can `CANCEL_TASK` bytes or an accepted client cancel response escape **before** `CancelIntent`/terminal commit?
2. Can an active cancel response misreport `CANCELLED` before the real Worker result and M8 barrier?
3. Can a duplicate Cancel/Retry result be sent to an fd-reused connection or cause a second SQLite transition?
4. Can Cancel ACK be mistaken for terminal proof, or does `already_terminal` suppress an authoritative pending FINISH?
5. Can late cancel/timeout change a frozen natural leader outcome or previously installed termination cause?
6. Can the Controller signal a child by stale PID rather than by currently owned Attempt→RuntimeProcessId?
7. Can missing RUN_ACK/START be mistaken for explicit rejection or task timeout instead of LOST?
8. Can an old session/new replacement be HEALTHY at the same time or release resources before bulk LOST commits?
9. Can Worker reconnect and register before **all** old groups are gone and fd/child owners are cleaned?
10. Can old session credit, queued LOG_CHUNK, callback, generation or LogWriter barrier result affect the new session?
11. Can Worker/Controller restart infer success from orphan log files or accept stale worker results?
12. Can manual retry reuse old Attempt IDs/sequence/path or accidentally auto-retry on failure?
13. Can log open/write/file-sync/directory-sync failure expose false `logs_complete=true`?
14. Can unknown SQLite COMMIT result authorize RUN, CANCEL or accepted client reply?
15. Can queue saturation/eventfd failure cause busy-loop, dropped credited log, unbounded backlog or deadlock the EventLoop?
16. Can a cancelled workload leave a zombie, live process-group member or inherited pipe that is silently treated as complete?
17. Are all call paths compatible with the frozen ownership graph, strict C++17 warnings and source/module dependency direction?
18. Are M1–M8 tests all retained rather than disabled/relabelled to manufacture the expected count?

A significant unmitigated defect in any question is a **NOT PASSED** gate. Do not rationalize incorrect behavior as a later M10 hardening task.

## 20. Build, formal tests and evidence

Run from WSL, preserving the existing build-note file:

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler \
  -L m9 --output-on-failure --repeat until-fail:20
```

- Enforce existing `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`; do not lower warnings or disable regression tests.
- Preserve **all 430 accepted M1–M8 CTest entries**, adjusting only legitimately superseded M7/M8 assertions with explicit explanation and equal-or-better regression coverage; report any baseline change rather than silently hiding it.
- Verify exact actual full-suite count, new M9 count, M9 repeated execution count and wall time from logs. Baseline at start of M9 is **430/430**, not a promised final total.
- Run genuine real-process chaos and subprocess harnesses with deterministic gates; record each scenario's name and evidence. Use Linux-local `/tmp` or another local filesystem supporting required directory sync, never treat DrvFs as equivalent for durable-log testing.
- Record `git diff --check main...HEAD`, `git diff --name-status main...HEAD`, dependency audit, exit statuses, test log paths and protected-note SHA-256 before/after.
- Inspect `/proc` fd/thread/children and process-group cleanup; ensure subprocess harness tearDown kills its own test groups and does not silently leave zombies.
- Consider ASan/UBSan/TSan when genuinely supported and feasible, but **do not claim sanitizer PASS** unless run. M10 owns the comprehensive sanitizer CI preset and deployment/benchmark work; absence here must be reported honestly.
- SIGKILL/crash-order tests do not simulate power loss; no claims beyond Linux local filesystem and SQLite sync assumptions.

## 21. Explicit STOP conditions

STOP and report evidence rather than continuing if:

- The working tree would overwrite unrelated user material, or the remote baseline is not verified.
- Correct M9 behavior would require altering frozen states/protocol messages/database version or inventing a lease/reconciliation protocol.
- The Worker would need to reconnect while still owning old tasks or without demonstrable cleanup-before-new-REGISTER semantics.
- Cancellation might mutate terminal Task/Attempt history, or any side effect must precede a required durable commit.
- A credited LogChunk would need to be dropped, or a log barrier would need to be skipped to pass a test.
- The test infrastructure cannot exercise a key mandatory fault without a truthful explanation; do not mark that criterion PASS from a fake claim.
- A required Core/Owner failure remains unclassified; never hide it behind generic retry, catch-all continue or fabricated ACK.

## 22. Git delivery and independent-review boundary

After satisfying all mandatory acceptance criteria:

1. Review **entire** `main...HEAD` including earlier M9 commits, authorization document, tests and scripts.
2. Commit only the authorized branch changes; push only `milestone-9-cancellation-recovery`.
3. Verify `HEAD` equals remote branch HEAD, working tree is clean, and the accepted `main`/`origin/main` baseline remains unchanged.
4. Add `docs/reports/MILESTONE_9_REPORT.md`, update or add `docs/M9_LOCAL_DEMO.md` with exact verified commands, and include any narrow cross-milestone fixes in the report.
5. Do **not** merge into `main`, make an automatically merging PR, start M10, tag a release or change frozen contracts.
6. **STOP and await explicit independent source review.**

## 23. Required M9 A–L acceptance report

Every section is mandatory. Exact implementation SHAs, test output and unresolved limitations should be provided rather than generic success language.

- **A. Git / baseline:** exact full original main SHA, final M9 HEAD, ancestry/ahead-behind, commits, remote equality, workspace and protected file checks.
- **B. M9-A–D implementation summary:** features and what each checkpoint actually implemented, not architectural aspirations.
- **C. Files changed and dependency direction:** full additions/modifications/deletions/renames and legitimate lower-layer fixes.
- **D. Client cancellation and retry:** all state/response cases, commit-before-response and command dispatch, no-auto-retry proof.
- **E. Controller command delivery:** current-effect fences, request correlation, CANCEL ACK semantics, backpressure and partial send handling.
- **F. Worker termination/timeout:** cause precedence, process-group signals/reap, concurrent logs, exact FINISH outcome.
- **G. Session loss and reconnect:** UUID/session/generation lifecycle, cleanup-before-registration, replacement, deadlines and stale messages.
- **H. Crash/restart/persistence/log failure semantics:** exact LOST/recovery results, `UnknownCommit`, failure classifications, barriers and no premature reservation release.
- **I. Formal acceptance matrix:** map every mandated M9 and applicable `TEST_STRATEGY` scenario to named executable tests and source paths; call out unsupported cases honestly.
- **J. WSL build, regression and stress evidence:** exact commands, GCC/CMake/SQLite versions, full test counts, repeat counts/durations, resource checks and sanitizer status.
- **K. Deviations, risks and future M10 boundaries:** anything deferred, known limitations, no exactly-once/auth/isolation claims; explain contract preservation.
- **L. Final Gate:** `PASS` only when all mandatory criteria have passed with evidence; otherwise `NOT PASSED` with concrete blockers.

## 24. Final Codex instruction

**M9-A, M9-B, M9-C and M9-D are explicitly authorized as one milestone.** Follow the document in full, reusing accepted M1–M8 code. Do not ask for routine checkpoint approvals. Conduct the full warning-clean build, complete regressions and real multi-process fault testing. Commit and push only the M9 branch, deliver the complete A–L report, and STOP before independent review and merge.

If every required Gate passes, end with exactly:

**Milestone 9: PASS — awaiting explicit independent review; Milestone 10 has not started.**

Otherwise end with:

**Milestone 9: NOT PASSED — blockers remain.**
