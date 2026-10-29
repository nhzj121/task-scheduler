# Milestone 8 Authorization / Codex Development Prompt

## Project: Linux C++ Distributed Task Scheduler

**Status:** Explicit owner authorization for the complete **M8 — Lossless Durable Task-Log Path** only.  **No authorization to merge `main` or start M9/M10.**

**Verified accepted baseline:** `main == 04e5790075d6dfdee9096f19345856935e7c774f`; accepted M7 commit `e61230470ac026c91f18b61a005bfaa3ab9f54ed` is its ancestor. Reverify at execution time.

**Development branch:** `milestone-8-durable-task-logs`.

**Intent:** Complete the existing M7 real TCP/process vertical slice with an independently owned, bounded and durable stdout/stderr path. Eliminate M7's deliberately incomplete `finalization_pending` hold **only when the full frozen barrier and terminal SQLite commit have actually succeeded**. Build real Controller LogCoordinator, LogCreditLedger, dedicated LogWriterThread, worker grant/replenishment handling, client fixed-end snapshot logs, and adversarial multi-process/fault tests.

## 0. Authority and conflict rule

1. Read `AGENTS.md`, `docs/ARCHITECTURE_GATE.md`, `docs/spec/Codex_Full_Implementation_Prompt_v1.md`, `docs/spec/Milestone_8_Authorization_Codex_Development_Prompt.md` and the frozen documents listed in section 2 **before production edits**.
2. The accepted `docs/IMPLEMENTATION_PLAN.md` defines M8, and `docs/ARCHITECTURE.md`, `docs/OWNERSHIP.md`, `docs/STATE_MACHINE.md`, `docs/FAILURE_MODEL.md`, `docs/PROTOCOL.md`, `docs/DECISIONS.md`, `docs/TEST_STRATEGY.md`, `docs/DEPLOYMENT.md` define its semantics. This authorization operationalizes them; it does not override them.
3. This document supersedes only the historical M1-only authorization limit in the Full Implementation Prompt. Preserve every frozen wire ID, schema v1, Task/Attempt state, scheduler heuristic, recovery policy, and consistency guarantee.
4. A real contradiction preventing a safe implementation is a **STOP** condition: report exact file/section, executable alternatives, and minimum required decision. Do not silently invent a new domain protocol or weaken durability to claim a PASS.
5. M8-A through M8-D are all authorized here. No intermediate approval is required for ordinary engineering progress. A final independent review, not the implementer's self-report, determines permission to merge.

## 1. Git and workspace preflight

- `git fetch origin`, inspect `git status --short`, `git branch -vv`, `git log --graph --oneline --decorate`, and the actual remote `main` SHA.
- Verify accepted M7 is in `main` and `origin/main`. If remote main advanced, inspect intervening commits; never automatically assume the previous SHA remains current.
- Start `milestone-8-durable-task-logs` **from the verified accepted main** and implement only on that branch.
- Exception: if the sole pre-existing uncommitted change is this owner-provided M8 authorization Markdown file at `docs/spec/Milestone_8_Authorization_Codex_Development_Prompt.md`, carry it onto the new branch and commit it there. Any other unrelated modifications require an explicit stop, not a reset/stash/overwrite.
- Preserve ignored `.local-notes/`, personal notes, local run data, all user files, and `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md` (verify its SHA-256 before/after). Do not remove/recreate the build directory.
- Do **not** modify/push `main`, force push, create an auto-merging PR, delete previous milestone branches, or begin M9+.

## 2. Mandatory architecture and source reading

Read in full, not merely headings:

- `docs/PROJECT_SCOPE.md`; `docs/ARCHITECTURE.md`, especially concurrency and handoff, the log data path, Controller/Worker architectures, Worker wire-output ordering, SQLite vs log-file separation and submission-to-completion sequence.
- `docs/OWNERSHIP.md`, especially LogWriter owner, file/directory fds, RunningTask, timers, fd registration and shutdown sequences.
- `docs/STATE_MACHINE.md`, especially internal finalization phase, Attempt terminal transitions and `LOST`/ProvenEmpty exceptions.
- `docs/FAILURE_MODEL.md`, especially log write/fsync/namespace errors, protocol invalidation, cancellation/failure precedence and fail-stop.
- `docs/PROTOCOL.md`, especially `LOG_CHUNK`, `LOG_WINDOW_UPDATE`, `TASK_FINISHED`, `GET_LOGS`, request IDs and 4 MiB limit.
- `docs/DECISIONS.md`: ADR-005, -006, -007, -009, -016, -017, -018, -019, -021, -022 and -023.
- `docs/TEST_STRATEGY.md` plus `docs/DEPLOYMENT.md`, `docs/IMPLEMENTATION_PLAN.md` and `docs/ARCHITECTURE_GATE.md`.
- Accepted M3 transport/framing and `OutputQueue` ordering dependency; M4 `ProcessRuntime`/read allowance; M5 `StateStore`, `LogEvidence`, `PersistenceThread` and eventfd channel; M6 `TaskManager`, `WorkerRegistry` and `ResourceLedger`; M7 Controller/Worker applications, `ExecutionManager`, `ProvisionalStaging`, `Client`, taskctl, M7 tests, `docs/reports/MILESTONE_7_REPORT.md`, and `docs/M7_LOCAL_DEMO.md`.
- Search the actual source for existing LogWriter/log-credit scaffolding before naming new files or assuming an interface. No second network/process stack or cloned state machine.

## 3. Frozen invariants that must remain provable

**Three-thread Controller:** one EventLoop decision owner, one SQLite-only PersistenceThread, one task-log-only LogWriterThread. No general thread pool. SQLite owns metadata, paths, sizes, final sequences and completeness flags; never log bodies. LogWriter owns task-log `mkdir`/open/append/pread/fdatasync/directory fsync/close and file/directory-handle lifetime. EventLoop does **not** run filesystem operations, block on queue conditions or disk, or wait for durability synchronously.

**Credit conservation:** for raw bytes and write-command slots independently, `free + outstanding_granted + queued_or_inflight == configured_grantable_capacity`, with essential barrier slots separately reserved and non-grantable. Grant moves existing free capacity to session outstanding; a schema-valid credited chunk transfers ownership without fresh queue-capacity competition; write completion frees its exact raw bytes and one slot. Unsused grants reclaimed only on session invalidation. Session generation and Attempt identities fence stale transfers, notifications and grants. Correctly credited inability-to-admit is an internal invariant failure/fail-stop, never normal congestion or intentional output drop.

**End-to-end backpressure:** Worker checks both dimensions before reading actual child pipe bytes and before generating/queueing a chunk. Credit consumption occurs at chunk **generation/enqueue**, not at `send` completion. Exhaustion pauses pipe reads while socket control/heartbeat/timer/reaping remain serviced. Encoded base64 and bounded transport queues must not create unaccounted memory or an independent unbounded raw backlog. No output is intentionally lost on a healthy session.

**Independent streams:** stdout and stderr have separate starting-at-zero sequences, exact raw byte counters and 1..65,536 raw bytes/chunk. Binary including embedded NUL/non-UTF8 is opaque. M3's cross-stream per-Attempt emission ordinal and `TASK_FINISHED` dependency across partial TCP writes remain authoritative; a finish cannot begin send before all previous same-Attempt log frames have fully left M3's queue. Unrelated Attempts need not block the dependency.

**Completion ordering:** `TASK_FINISHED` counts/sequences must equal **accepted** chunks immediately upon receipt, or it is a Worker protocol fault/session invalidation; never wait for post-FINISH chunks. Accepted is different from written. After accepted writes for that Attempt complete, a LogWriter barrier must `fdatasync` **both** streams including empty files, then `fsync` **every still-dirty containing-directory namespace** from deepest to the task-log root. Only the matching barrier success can supply M5 `FinalBarrier` evidence and unlock M6 `Terminalize`/resource release. A terminal SQLite commit is the last gate, **not** part of a claimed SQLite/filesystem ACID transaction.

**Durability exceptions:** explicit rejected RUN_ACK or verified no dispatch bytes may use the frozen `ProvenEmpty` shortcut (zero counters and no failed namespace creation). `LOST` never uses it. Log write/file-data sync/directory sync errors never produce `logs_complete=true`; if the Task is known completed and SQLite still writable, terminalize as `FAILED`/`LOG_PERSISTENCE_ERROR`, retain observed outcome; session failure before reliable outcome follows conservative `LOST`. Unknown SQLite commit triggers M5/M6 fail-stop; do not invent retries.

**Historical paths:** M6/M5 already persist paths of the form `tasks/<canonical-task-uuid>/attempt-<positive-attempt-no>.stdout.log` and corresponding stderr (relative to Controller's canonical data root). The directory levels **actually present in these persisted paths** determine the namespace fsync chain. Do **not** silently introduce an extra `attempt/` directory or rewrite assigned paths to match diagrams. Reuse the exact paths, require Attempt/path equality, and verify they remain beneath the trusted root. The frozen ownership rule (sync dirty parents) applies even when there is no separate attempt directory.

## 4. Strict M8 boundaries and excluded later work

Implement all log-related integration in M8, including normal terminal outcomes and log-persistence-failure mapping. **Do not start** M9's complete active cancellation command orchestration, retry CLI, Worker automatic reconnect, failure/timeout orchestration redesign, or M10's packaging/systemd/deployment hardening.

In particular:

- Do not invent authentication, TLS, public-internet exposure, sandboxing, cgroups, quotas, retention, remote log storage, replication, upload, log-follow streaming, extra TCP log socket, or schema v2.
- Do not change the meaning of resource accounting, session identity, Worker child leader outcomes, M3 output scheduling, Attempt terminal states, or `GET_LOGS` wire fields.
- Do not turn an output failure into a silent truncation, synthetic `ProvenEmpty`, successful `FinalBarrier`, or a fake terminal Task state.
- Do not move log filesystem I/O onto EventLoop, SQLite thread, an extra worker pool, or the Worker disk; Controller LogWriter remains sole task-log I/O owner.
- Do not add automatic task retry, make Worker reattach old processes, adopt sessions after Controller restart, or claim exactly-once execution.
- A narrowly necessary change in M1–M7 code must be justified as a real integration defect or missing adapter, minimal, reviewable and tested; preserve regression behavior. Remove/replace M7's *provisional volatile staging only once* the full durable path replaces its credits safely; do not leave two conflicting ledgers.

## 5. Suggested implementation checkpoints

| Checkpoint | Production deliverable | Evidence required before moving on |
|---|---|---|
| **M8-A** | Bounded LogQueue/LogWriterThread, safe namespace + handles, raw append/pread, actual dual-file and dirty-directory barrier, completion/eventfd | Real file lifecycle, binary fidelity, dirty-fsync trace, error injection, fd cleanup |
| **M8-B** | Controller LogCreditLedger + LogCoordinator and Worker `LOG_WINDOW_UPDATE` integration; eliminate provisional credit split | Independent byte/slot conservation, no premature replenish, bounded Worker pipe reads, multi-session saturation |
| **M8-C** | Match FINISH to accepted/written counters; wait/barrier/typed evidence; M6 terminal commit and M5 failure path | Delayed write/barrier proves ACTIVE until commit, then terminal+release; no fake proof on error |
| **M8-D** | GET_LOGS snapshot + pagination + taskctl stdout/stderr/both; real multi-process, crash-order, stress/fault acceptance | Binary/large logs, frozen snapshots, restart/history, all M1–M7 regressions, repeat stress |

Suggested commits: Writer/paths; credit+Worker; FINISH/barrier/terminal; snapshot CLI; fault/crash-order/acceptance. Do not wait for a new authorization between checkpoints.

## 6. M8-A — Writer thread and owned LogQueue

- Add a single dedicated LogWriterThread owning all task-log descriptors, namespace mutations and disk syscalls. Expose commands/results as **owned values**: append (exact Attempt/session/stream/sequence/raw payload and stable operation identity), per-Attempt barrier (expected final sizes/seqs and matching persisted paths), bounded snapshot read, and possibly a narrowly scoped namespace-creation operation only if necessary. No lambdas retaining EventLoop/TaskManager/Peer pointers across threads.
- Implement a separate bounded LogQueue for write data, write-command slots and an **ungrantable essential command-slot reserve** for barriers. Lower-priority snapshot reads must not consume essential capacity. Bound completion results and pre-reserve delivery capacity before issuing operations; never lose an accepted write/barrier/snapshot result after disk side effects. Wake EventLoop via a distinct eventfd; drain until EAGAIN as required, unregister before teardown, join before destroying notifications.
- Maintain strict per-Attempt append/write ordering, stream offsets and independently advancing stream counters. A Writer completion reports exactly which chunk was written (and raw bytes), its owner/session generation, successful written length and `visible_bytes`, or typed failure. Progress after `write` short counts and EINTR without double-writing an already accepted prefix. Do not treat an incomplete write as a complete chunk, leak credit, or reissue non-idempotent append blindly.
- Implement namespace creation *inside Writer only*: relative paths derived from the persisted canonical Task/Attempt identity and stored paths, no user-controlled path joins; use a canonical trusted root and directory-fd-relative, no-follow operations where applicable. Reject traversal, symlink escape, nonregular log-file targets, different expected paths, and unsafe data roots. File permissions restrictive; directories owned and safe. Existing files/history must not be truncated, overwritten or silently treated as fresh empty logs.
- For each new directory, mark its parent dirty; for each new stream file entry, mark that containing directory dirty. Track real dirty state for all involved ancestor directories beneath the configured root; successful fsync discharges only the current namespace obligation, and later creates re-dirty it. Newly created task directory under `tasks/` requires sync of its containing `tasks/` directory; newly created `tasks/` requires sync of `data_dir`. If a path layout contains no attempt directory, don't fabricate one. Make this logic testable by recording actual syscalls/dirty transitions.
- Per-Attempt final barrier waits behind its previously accepted write commands, creates/opens both files if needed (including no-output `/bin/true`), verifies exact written counters, performs `fdatasync(stdout)` and `fdatasync(stderr)`, then `fsync` each **remaining dirty** containing directory deepest-to-root, and only then returns `FinalBarrier`-eligible success. A successful barrier must never be returned after an interrupted/failed required sync; retry only under a documented safe policy, never assume a sync happened.
- Use only real Linux task-log filesystem operations; do not replace actual durability with an in-memory staged vector or just call `fflush`, `close`, `fsync` on the DB, or `fdatasync` one stream.
- Provide deterministic fault seams for mkdir/open/write/partial-write/fdatasync on either stream/fsync on each dirty ancestor/close and completion delivery. Avoid production fault flags and permission-unsafe test shortcuts. Failure must be surfaced without reporting a successful barrier.
- Fail/abort/shutdown must ensure no new writes are granted, all accepted command owners have explicit results or a documented fail-stop abort, descriptors are unregistered/closed by their owner and threads joined. Never detach a live writer or block EventLoop waiting for disk.

## 7. M8-B — Controller LogCreditLedger and LogCoordinator

- Replace M7 `ProvisionalStaging` with **one authoritative** Controller EventLoop LogCreditLedger backed by the real queue. For each resource independently, track `free`, `outstanding_granted_by_session`, and `queued_or_inflight`; account exact raw bytes and one slot for **each chunk**, never base64 length or a raw-only byte window. Keep dedicated essential barrier reserve (slot counts) outside Worker grantable resources.
- Registration ACK initial bytes/chunks must be genuinely reserved before the ACK can be queued, or reject registration with the exact frozen rejected ACK shape. Avoid assigning the same global capacity to two sessions. Preserve nonzero accepted ACK contract and capacity/connection generation. On replacement, bind grants to the new UUID/generation only after the old session's required durable LOST path. On invalidation reclaim **unused** grant; previously accepted queued writes retain ownership until completion/abort. No credit transfer across sessions.
- For each valid incoming `LOG_CHUNK`, check Worker/Session/Attempt, stream sequence, raw_size, decoded length, credit, per-Attempt finalization status, path binding and applicable limits **before** ownership transfer. Accepted sequence and byte totals advance only once for correct chunks. Transfer both dimensions atomically to queued/in-flight Writer command without a new ordinary admission choice. On impossible post-transfer enqueue failure, assert/declare invariant failure, halt grants and fail-stop explicitly; do not quietly drop or disable TCP input as recovery.
- On matching successful Writer append completion, update only that Attempt's written/visible stream counters, release exactly the recorded byte and slot reservation, then grant newly available credit to appropriate valid sessions (fairness bounded; avoid single noisy Worker monopolizing all replenishment). *Write completion, not file sync*, allows a later grant. An error must not be mistaken for a successful completion or replenish capacity while still retaining an unowned chunk. Bound every pending completion/grant/command and reconcile loss paths.
- Maintain a **nonblocking** Controller socket path under saturation: always receive role-valid control/heartbeats, ACK, START and FINISH while pipe-based Worker output is backpressured. No `epoll` busy-spin, no blocking EventLoop disk call, no unbounded deferred frame/decode buffer.
- `LOG_WINDOW_UPDATE` (`0x0208`, Controller->Worker event, request_id=0) sends typed incremental byte and chunk credit; at least one positive, both nonnegative. Only announce a grant whose capacity is already reserved and can be safely transmitted to the current generation; if the output queue cannot admit the frame, do not silently leak promised capacity or strand it indefinitely. Session close releases undelivered and unused grants consistently.
- Implement credit replenishment within the original single Worker session TCP connection. Do not modify the M3 frame priority/dependency semantics to send FINISH ahead of log frames. Maintain the snapshot of cross-stream same-Attempt emission ordinal. Test partial sends, EAGAIN, control-frame priority, one-byte chunks, saturated log slots and several concurrent Attempts/sessions.

## 8. Worker credit, pipe reads and frame order

- Extend the M7 Worker application to accept current-session, role-valid `LOG_WINDOW_UPDATE` events after registration; reject wrong phase/direction/session where applicable. Check all addition overflow; never interpret a grant as an independent session or a new command.
- Preserve the M4 parent-only preparation/fork ownership, SIGCHLD, process-group cleanup, EOF detection and the existing M7 START ordering. Read no more than the currently available raw-byte credit, one chunk-slot per generated chunk, and actual M3 log-frame output-room limit. A zero in either credit dimension suspends **both stdout and stderr pipe read interest** appropriately while heartbeats, timers, control processing, reaping and cancellation-required cleanup remain live.
- A `LOG_CHUNK` is an owned encoded frame; raw byte and chunk credit are decremented exactly at generation/admission, with checked counters. Do not buffer unbounded bytes read from pipes and try to obtain credit afterward. Transport failure follows conservative Worker session-loss cleanup, not byte discard and success.
- Ensure a Worker waking after grant re-enables the appropriate pipe-read registrations and makes bounded progress without spinning. Log write completion/grants allow an output-heavy workload to finish, rather than permanent exhaustion as in M7. Ensure exit/EOF and no-output paths still finish promptly.
- M3 output queue priority and per-Attempt ordinal dependency remain the single wire-ordering implementation. Explicitly test a FINISH queued behind partially sent stdout/stderr chunks while unrelated heartbeats remain eligible. Do not send a FINISH first merely because it is a control frame.
- Preserve the current M7 per-process/Attempt ownership and resource accounting. No Worker per-task threads or second TCP connection. M9's automatic reconnect and complete cancellation orchestration remain excluded.

## 9. M8-C — FINISH reconciliation, durable evidence and terminal state

- Keep **three distinct positions** per stream: accepted from TCP, successfully written/visible by Writer, and final declared by `TASK_FINISHED`. Do not conflate accepted with written or written with synced. Bind them to exact Task ID, Attempt ID, Session UUID/generation and immutable persisted stdout/stderr paths.
- On FINISH reception verify the frozen declared `stdout_next_sequence`, `stderr_next_sequence`, `stdout_bytes`, `stderr_bytes` **immediately against accepted counters**. Mismatch is protocol violation and conservative session invalidation; never block waiting for impossible post-FINISH chunks.
- Record immutable observed outcome once via M6 `FinishedEvent` with `Incomplete` evidence, leaving current persisted nonterminal state and reservation untouched. If write completions for this Attempt remain, defer its barrier; once all prior writes are resolved and written counters equal final counters, submit an essential LogWriter barrier for exactly that Attempt. Never let snapshot reads consume barrier-reserved slots.
- On **genuine** matching success (two file-data syncs, required namespace syncs, all earlier writes resolved and counter checks), construct typed M5 `LogEvidence{FinalBarrier, exact attempt ID, prior_writes_completed=true, final_counters_verified=true, stdout_synced=true, stderr_synced=true, namespace_synced=true, namespace_failed=false, ...}`. Re-deliver the *same immutable observed outcome* through M6 `TaskManager::receive` with evidence. M6 validates version/session and issues real M5 `Terminalize` and only matching SQLite COMMIT permits Task terminal visibility, resource release and subsequent scheduling. `SUCCEEDED` for `/bin/true` is the expected happy-path result after these gates.
- On Writer append/open/file-sync/required-directory-sync failure, do **not** send `FinalBarrier`. When a reliable finished observation is known and SQLite is healthy, supply M6's log-persistence-failure path: `FAILED`, reason `LOG_PERSISTENCE_ERROR`, observed process outcome preserved, `logs_complete=false`, release only after terminal COMMIT. On loss before an authoritative FINISH, use conservative session invalidation/LOST; do not manufacture a child result. Distinguish read-only snapshot errors (client INTERNAL) from write/barrier health faults.
- When SQLite is degraded or returns `UnknownCommit`, stop scheduling/grants and follow existing fail-stop; never release reservations optimistically, emit success, or invent cross-medium atomicity. A Controller SIGKILL before terminal metadata commit must recover as LOST even if files happened to sync; a committed terminal record requires earlier successful barrier.
- Preserve the two frozen proven-empty exemptions exactly: explicit RUN rejection and pre-dispatch local cancellation, with no LogWriter creation/sync error on their paths. A genuinely `LOST` Attempt or an ordinary zero-output executed child **cannot** substitute ProvenEmpty.
- Session invalidation while writes/barrier are inflight must fence late success/credit/terminal completions via IDs and generations; do not turn a new session's Attempt terminal, release an unrelated resource or create false completeness.

## 10. M8-D — GET_LOGS, snapshot paging, taskctl integration

- Enable existing frozen `CLIENT.GET_LOGS`/`GET_LOGS_RESPONSE` using real Controller client handler and Writer-thread-owned `pread`/snapshot operations. No Controller/EventLoop file `open`, `pread`, `stat`, `fstat` or sync for task-log data. Resolve a specified Attempt number against committed history; null `attempt_no` selects latest. Match the canonical immutable stored path and safe root; do not trust client-supplied path components.
- First page with `snapshot_end=null` captures **writer-reported `visible_bytes` at that snapshot**, not current file length while another write is in flight. All subsequent pages preserve that exact end; do not include later appended bytes. For historical persisted terminal records, respect their durable final counters, handle verified existing files and failures explicitly without inventing snapshot progress from unsynced orphan tails. For incomplete/crash-recovered LOST history, do not infer logs_complete or durable data solely from physical file length.
- Limit raw page bytes to `min(requested_limit, 3,144,960)` with the exact frozen default 262,144; the wire encoder must still enforce 4,194,304-byte maximum with fixed-field JSON overhead. Reject zero/negative/type/overflow per frozen protocol; check byte-offset arithmetic, EOF/next_offset, no more than snapshot_end and bounded memory accounting. Responses are binary-safe base64, streams independent.
- Explicit `ProvenEmpty` with zero bytes/sequences and no file legitimately returns empty `logs_complete=true`; missing file on an ordinary genuinely completed durable Attempt is **not** silently considered empty. A log read failure returns protocol `INTERNAL`, not a Task state change. File read completion must be correlated to still-live requester/generation and request ID; stale client completion is discarded without side effects.
- Implement `taskctl logs` with stdout and stderr selection plus frozen `--both` behavior (two separate fixed-end snapshots). Fetch multiple pages without materializing an unbounded full log in memory; decode and write exact raw bytes to stdout/binary-safe stream; diagnostics belong on stderr. No follow mode. Avoid accidentally using JSON/Unicode rendering that changes binary bytes. If `--json` is supported for logs, follow the frozen response schema and don't mix encoded payload with raw stdout.
- GET_LOGS reads are lower priority/rejectable ahead of essential barriers. A client issuing large or repeated snapshot reads cannot delay active tasks' barrier completion indefinitely. Document the user-facing command in the local demo, including zero-output and historical cases.

## 11. Queue, memory, identity and shutdown contracts

- Write-command bytes, grant bytes, chunk slots, essential slots, encoded frame buffers, retained event/completion metadata and snapshot replies must each have explicit finite capacity. Audit the *combined* owned memory rather than claiming queue-size boundedness while accumulating unbounded metadata or completions elsewhere.
- All transfers are value-owned and stable-ID correlated; no fd or borrowed `string_view` crosses an async handoff. No raw pointer to Task, Attempt, WorkerSession, Peer, TcpConnection or file descriptor kept in a worker command/callback.
- LogWriter and PersistenceThread are separate; use distinct eventfds and queues; EventLoop callback drains to EAGAIN, bounded per turn; completion producer cannot discard successfully accepted effects. Sequence overflow, size conversion and credit addition must be checked, not wrap.
- Startup: create/canonicalize trusted Controller root with existing M7 ownership rules, start SQLite recovery and log writer safely, fully restore Task/Attempt history, then enable listeners; do not reconstruct dead Worker leases or assume incomplete historical logs became complete merely because files exist. `metadata.sqlite3` `fcntl` locks must not be broken by stray same-inode fd closes (preserve M7's M5 integration fix).
- Shutdown: stop grants/new log admission, conservatively invalidate sessions, allow bounded accepted write/barrier/finalization cleanup where possible, stop and join worker outside EventLoop dispatch, drain/abort all accepted completions explicitly, unregister eventfd before resource destruction, finalize fd ownership. Fatal failure must cause nonzero Controller termination, not a hidden success or detached file thread.
- Safe root and symlink rules must be tested on real local Linux filesystem; do not use `/mnt/e` for formal durability assertions if its filesystem semantics differ. The existing WSL Linux build location is acceptable for the build; isolated Linux `/tmp` or an ext4-backed WSL location is appropriate for test data, with the actual filesystem reported.

## 12. M8-A unit/component acceptance

- Deterministic real path creation for Task UUID and assigned Attempt naming; noncanonical IDs/traversal/symlink/regular-file mismatch fail without leaving unsafe outside-root files.
- Both stdout/stderr files created by Writer even for no output and final barrier; existing terminal history not truncated on reopening; no false creation or overwriting.
- Binary bytes with NUL, invalid UTF-8, interleaved streams, many chunks, short-write/EINTR and independent visible offsets; no duplicate/omitted bytes.
- Writer sole file and directory fd ownership, directory dirty-chain tracking and exact sync order for each newly created entry; two fdatasyncs before deepest-to-root parent directory fsync; no per-chunk directory sync.
- Repeated subsequent Attempt with same preexisting task directory does not redundantly resync an already clean parent. New stream/Attempt files correctly re-dirty containing directory.
- Every injected mkdir/open/write/fdatasync-stdout/fdatasync-stderr/dir-fsync level/partial-write/read fault produces a typed result and never a false successful final barrier.
- Writer capacity and reserved barrier slots; completion-delivery and eventfd under many commands and close/reopen cycles; steady fds/threads/handles, bounded shutdown.

## 13. M8-B credit, transport and Worker acceptance

- Independent exact byte and slot partition identities at every operation: grant, accept, pending writer, write complete, reclaim, replacement and abort. Randomized deterministic traces with invariant assertion **at every step**; test UINT64/size_t bounds and zero/mixed grants.
- One-byte chunks fully consume slots despite unused byte capacity. A chunk that fits credit always admits at global capacity limit. Inject impossible mismatched reservation to prove explicit fail-stop rather than dropped output.
- Worker refuses pipe read and chunk generation with no bytes **or** no slots, never reads ahead of credit, resumes on grant; output frame count/bytes bounded under stalled socket. No artificial data truncation or busy-spin.
- Multiple Workers/sessions and concurrent Attempts: aggregate outstanding credit never exceeds either grantable dimension; release of unused session grants does not release accepted queued work prematurely. A disconnected session cannot receive late grant.
- Controller keeps accepting heartbeats and other control while log path or TCP output is saturated; M3 ensures cross-stream same-Attempt chunk ordinal/FINISH ordering even on partial sends.
- Write completion replenishes, file fdatasync does not gate normal incremental replenishment; never credit merely for accepted-but-unwritten bytes. Verify `LOG_WINDOW_UPDATE` encoding/request ID/sequence and actual new worker pipe progress.

## 14. M8-C terminal and crash-order acceptance

- Real M7 `controller + worker + taskctl` `/bin/true` yields `PENDING -> ACTIVE/ASSIGNED -> STARTING -> RUNNING -> SUCCEEDED` and `logs_complete=true` **only after** both file-data syncs, all dirty-directory syncs, matching M5 terminal COMMIT and resource release. The two zero-byte files and namespace are real, not ProvenEmpty.
- Real nonzero exit, signaled outcome and parent/child exec failure preserve frozen Task/Attempt mapping and observed reason; normal terminal outcome waits for barrier. Do not broaden M9 timeout/cancel handling beyond existing runtime/TaskManager mapping.
- Delay/pause write completion, per-stream file fdatasync, each required directory fsync, and terminal SQLite COMMIT independently. Verify external readers see nonterminal + reservation until the right gate, then terminal and exact counters. No premature `FinalBarrier` or early release.
- Simulate append, file-data sync and directory sync error with SQLite healthy: `FAILED`/`LOG_PERSISTENCE_ERROR`, observed child facts retained and `logs_complete=false`; no success proof. Include error before authoritative FINISH, which must use conservative session loss rather than inventing a terminal observation.
- Crash/SIGKILL after namespace create, after each file fdatasync, before/after each required directory fsync and before/after terminal SQLite commit; restart reflects only previously durable metadata. Test syscall ordering deterministically with fault hooks, and do honest real-filesystem crash/restart probes where supported. **Do not claim real power-loss durability from SIGKILL alone.** No unprivileged test harness may claim to simulate a block-device power failure.
- M3 FINISH wire dependency tested on partial writes and multiple streams; accepted count mismatch is immediate protocol violation/LOST, not a waiting finalization.
- Session replacement/disconnect during inflight write/barrier, stale duplicate completion and file-error cleanup: never make a LOST Attempt SUCCEEDED or release a new Session's capacity.

## 15. M8-D snapshot and cluster acceptance

- Actual arbitrary binary stdout/stderr, non-UTF8, NUL, multiple chunks, interleaving, output larger than initial credit; exact bytes on disk and via GET_LOGS/taskctl, with full completion and bounded execution. Include at least one high-volume scenario that forces **multiple replenishments**.
- Snapshot fixed-end first page while workload continues writing; later append must not change earlier pages' end. A new independent invocation sees later bytes. Test large paging near 4 MiB JSON cap and default/clamped limits.
- A no-output executed workload returns two actual durable zero-length file streams; a ProvenEmpty never-executed Attempt returns empty complete logs without requiring files; an incomplete/LOST Attempt never masquerades as complete.
- One/no/multiple Workers, resource release leading to follow-on Task scheduling, historical attempts and restart. Query all retained records without treating Controller's memory as the authoritative log body.
- Output/queue/slow-log-writer saturation: ensure exact no-loss results, capacity/credit invariants, no spurious heartbeat timeout, no unbounded RAM, no starvation of essential barrier and no event-loop blocking.
- Client cancellation/retry commands not implemented by M7 remain excluded except when necessary to satisfy the already frozen log/error path; do not prematurely self-authorize M9.

## 16. Complete formal WSL build and test procedure

Formal environment: established WSL Ubuntu 22.04.3, GCC 11.4, CMake 3.22, C++17 and existing project warnings as errors. Source and build locations:

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler \
  -L m8 --output-on-failure --repeat until-fail:20
git diff --check main...HEAD
sha256sum /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
```

- Preserve all **389 existing M1–M7 tests** and their labels. Add a dedicated `m8` CTest label, separately identifiable unit/component and **actual external-process** integration tests. If baseline count has legitimately changed, explain and reconcile rather than assume 389.
- Perform a full clean `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror` build. Do not weaken flags or lower-layer tests.
- Repeat full M8 focused suite at least 20 times; repeat seeded randomized capacity conservation and key crash/partial-send scenarios; show exact commands, seeds and count. Report any flaky failures, retries and timeouts, not just final success.
- Confirm all tests still use the real M3/4/5/6/7 production components; test fakes are fault gates and synthetic transport/FS injections, not fake domain logic. At least one end-to-end runs **three actual binaries**, true Linux child processes, TCP, SQLite and actual filesystem logs.
- Audit `/proc` threads, fds and children after repeated start/stop, data-directory locks, eventfd registrations, writer-owner cleanup and no indefinite hangs. Preserve `BUILD_COMMANDS.md`, user notes, existing local data and frozen-doc SHA where appropriate.
- Check include/link dependency direction, no protocol/SQLite/schema drift, no path changes, no M9+, `git status`, `git diff --stat`, `git diff --check`. Report sanitizers only if actually executed; never assert power-loss protection from process crash alone.

## 17. Reviewer-focused invariants / explicit STOP triggers

**Must be answered with specific source and test evidence:**

1. Does LogWriterThread alone own *every* task-log file and directory syscall, including `pread`, and is there exactly one Writer thread in addition to EventLoop/PersistenceThread?
2. Can every byte and chunk-slot grant be traced to reserved global capacity, and is conservation maintained after write failure, partial write, disconnect, replacement and closure?
3. Is a credited chunk guaranteed capacity at receipt, or does any path quietly drop it or block the EventLoop?
4. Are credited Worker pipe reads bounded **before** `read()` and resumed by real `LOG_WINDOW_UPDATE` without starving heartbeat/control?
5. Are accepted/written/final stream counters separate and exactly matched to same Attempt and generation?
6. Can FINISH pass partially transmitted same-Attempt logs, or can an unrecognized late log be accepted after FINISH?
7. Does the barrier always include two file `fdatasync`s and *all* required dirty parent-directory `fsync`s in correct order, including empty output? Is directory sync absent from ordinary chunks?
8. Can any ordinary Task become terminal or release its ResourceLedger reservation before both barrier completion and matching M5 COMMIT?
9. Do log-failure and missing-files cases preserve explicit error, observed outcome when known, and `logs_complete=false` rather than false completeness?
10. Do GET_LOGS snapshots fix their end on the first read, correctly base64/paginate, handle historical and proven-empty cases, and stay below the full 4 MiB frame cap?
11. Does the code honor M5/M6 immutable stdout/stderr paths, old Session fencing and M7's POSIX SQLite lock fix?
12. Are log queue, essential barrier reserve, completion delivery, effect queues, client replies and fd/thread cleanup finite and nonblocking?
13. Do tests include real fault ordering for the required containing-directory sync levels and accurately describe what SIGKILL does and does not prove?
14. Are all M1–M7 regressions retained, with actual full-suite + 20-repeat M8 evidence and no scope creep to M9/M10?

**STOP for owner architecture review** if correctness would require a new wire message, changed SQL schema/states, weakening lossless or durable-log guarantees, using unbacked log credit, changing immutable paths, allowing EventLoop file I/O, treating `SIGKILL` as proof of physical power-loss durability, or adopting an unapproved retry/failure policy. Ordinary implementation bugs within this authorization should be fixed and re-tested rather than escalated.

## 18. Git commit, push and STOP

1. Finish all M8-A–D tests, review full source diff, ensure documentation describes implemented (not aspirational) behavior, and commit reviewable milestone commits **only** on `milestone-8-durable-task-logs`.
2. Push that branch to `origin` and verify local/remote branch SHA equality and tracking `ahead/behind = 0/0`; verify `main` and `origin/main` remain at the same accepted baseline, barring separately reviewed concurrent changes.
3. Preserve the WSL build note and ignored personal files. Do not merge, create auto-merging PR, delete development branch or begin M9/M10.
4. Produce `docs/reports/MILESTONE_8_REPORT.md` as a tracked branch artifact (or explain any existing report location), with mandatory sections A–L below. Include the updated local demo usage for durable stdout/stderr and `taskctl logs`.
5. STOP and await independent source review. Self-reported passing tests are evidence, not merge authorization.

## 19. Required final A–L acceptance report

- **A. Git / baseline:** verified main, branch, commit ancestry, final SHA, clean working tree, push equality.
- **B. Implemented scope:** M8-A/B/C/D modules, actual integration and intentionally deferred work.
- **C. Files/dependencies:** every added/modified/deleted file; CMake targets and dependency direction; accepted-layer adaptations.
- **D. LogWriter/namespace:** owner, safe paths, exact on-disk layout, write/pread, dirty-dir chain, fsync syscall ordering and fault behavior.
- **E. Credit ledger/queue:** byte and slot capacities/reserves, transfer invariants, initial and replenished windows, session reclaim, completion notifications, seeded traces.
- **F. Worker/transport:** actual pipe read bounds, credit updates, request IDs, stream sequences, FINISH dependency/partial sends, process owner.
- **G. Barrier and terminal commit:** accepted vs visible/final counters, two file syncs and directory sync, evidence binding, M5 CAS, M6 release and log-failure mapping.
- **H. GET_LOGS/taskctl:** historical and active reads, fixed snapshot end, paged binary output, cap handling, proven-empty/LOST semantics.
- **I. Acceptance matrix:** exact test names for every section 12–15 invariant, with gaps called out.
- **J. WSL verification:** build/CTest/repeats/stress/fault commands, exact counts, environment, fd/thread/process leak checks, no fake sanitizer or power-loss claims.
- **K. Risks/deviations/M9 boundary:** truthful remaining incomplete functionality, actual architecture exceptions and source-level review findings.
- **L. Gate:** `Milestone 8: PASS — awaiting explicit independent review; Milestone 9 and later milestones have not started.` **only** if all requirements genuinely met; otherwise `Milestone 8: NOT PASSED — blockers remain.`

## 20. Final instruction

**Implement the entire M8-A through M8-D scope on the verified `milestone-8-durable-task-logs` branch. Run formal WSL acceptance. Commit and push only that branch. Provide the full A–L report. Then STOP. Do not merge or begin M9+.**
