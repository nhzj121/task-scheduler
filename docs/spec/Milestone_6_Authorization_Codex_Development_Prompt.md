# Milestone 6 Authorization / Codex Development Prompt

## Project: Linux C++ Distributed Task Scheduler

**Authorization status:** M6-only production development authorized. This document is a standalone, executable development contract for Codex, to be placed at `docs/spec/Milestone_6_Authorization_Codex_Development_Prompt.md` on the M6 branch. It does not authorize M7 or later milestones.

**Verified GitHub baseline (2026-10-21):** `origin/main` = `699ac82b2af67e58da301465c945e4a0f8240bb3`. This merge commit has M5 final commit `3cce466eb058a279d78f6c333a77f07b3377abb3` as a parent. M0–M5 have passed their review gates and are merged.

**Development branch:** `milestone-6-scheduler-controller-state` (create from verified `main`, never from an older milestone branch).

**Normative M6 statement from `docs/IMPLEMENTATION_PLAN.md`:**

> **Milestone 6 — Pure scheduler and Controller state orchestration.** Future modules: `scheduler`, Controller `TaskManager`, `WorkerRegistry`, `ResourceLedger` using fake persistence and transport ports. Work: deterministic placement; tentative/committed reservations; one-transition-in-flight serialization; cancellation intent; session invalidation; durable-side-effect gates represented as interfaces. Acceptance: scheduler vectors; reserve/rollback/release; transition completion reordering; stale session; assert fake transport receives no RUN/CANCEL/response before fake commit; bulk LOST. Suggested commits: scheduler; ledger; TaskManager persistence state machine; WorkerRegistry and gating tests. Risk: accidental mixing of persisted domain state and pending orchestration phase. Keep pending operations separate from the frozen enums.

---

## 0. Authority, milestone boundary, and stop rule

This is the owner's **explicit authorization to implement all of M6**, not an approval to implement an end-to-end runtime. It supersedes *only* the historical Milestone-1-only authorization boundary in `docs/spec/Codex_Full_Implementation_Prompt_v1.md`. All frozen architecture, ownership, state, scheduling, failure, protocol, and persistence contracts remain authoritative.

Do not change frozen state enums, transition meaning, scheduler heuristic, Task/Attempt uniqueness or persistence ordering to fit an implementation. If a genuine conflict or an externally observable decision missing from the frozen documents prevents safe implementation, **STOP before choosing a new semantic rule**. Report the two relevant clauses, smallest clarification, affected tests, and your recommendation. Continue with unrelated M6 work only if it cannot prejudice that decision.

Architecture sources have priority over illustrative sample APIs in this document. You may choose class names and signatures appropriate to the existing C++17 codebase, provided the observable contracts below remain true.

## 1. Workspace and user-data protection

- Windows working copy: `E:\Deck_all_need\projects\work\task_scheduler`.
- WSL source location: `/mnt/e/Deck_all_need/projects/work/task_scheduler`.
- WSL out-of-tree build location: `/home/nhzj121/build/task_scheduler`.
- Preserve the existing `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md` exactly. Never delete or recreate the whole build tree to force a clean build; use CMake's clean target.
- Preserve ignored personal notes `.local-notes/`, all untracked user files, and unrelated tracked modifications. No broad `git clean -fdx`, destructive reset, forced branch checkout, or silent stashing.
- Formal acceptance runs in **WSL Ubuntu 22.04 / GCC 11.4 / C++17**. Windows tools may edit files and inspect Git, but Windows/MinGW builds are not Linux acceptance evidence.

## 2. Mandatory Git preflight, before editing production code

1. Inspect `git status --short`, current branch, `git branch -vv`, `git remote -v`, relevant recent history, and local/remote `main`.
2. Fetch remote refs. Confirm `main` and `origin/main` include accepted M3, M4 and the complete M5 merge, including `3cce466eb058a279d78f6c333a77f07b3377abb3`.
3. Expected remote base is `699ac82b2af67e58da301465c945e4a0f8240bb3`. If the remote has advanced, inspect the new commits and explain whether the accepted baseline remains valid; do not silently branch from stale `main`.
4. If `docs/spec/Milestone_6_Authorization_Codex_Development_Prompt.md` is the **only** pre-existing uncommitted change, it is the expressly permitted authorization-file exception: carry it intact onto the M6 development branch and commit it there. Do not commit it to `main` first.
5. Create or switch to `milestone-6-scheduler-controller-state` from the accepted base. Only this branch may receive M6 commits.
6. If any other dirty state, conflict, unknown baseline change, or conflicting M6 branch is present, do not overwrite or discard anything. STOP and report the precise condition.
7. Record exact baseline and initial branch HEAD for final review. The source tree must remain traceable to the accepted mainline.

## 3. Required source reading

Read and apply, in this order, before implementing core classes:

1. `AGENTS.md`.
2. `docs/ARCHITECTURE_GATE.md`.
3. `docs/spec/Codex_Full_Implementation_Prompt_v1.md`.
4. **This** `docs/spec/Milestone_6_Authorization_Codex_Development_Prompt.md`.
5. `docs/spec/Task_Scheduler_Final_Scope_v1.md` and `docs/PROJECT_SCOPE.md`.
6. `docs/IMPLEMENTATION_PLAN.md` — M6 scope, DAG, cross-cutting rules and milestone stop boundaries.
7. `docs/ARCHITECTURE.md` — Controller components/handoff; scheduling/resources; module dependencies; submission, cancellation, failure, and restart sequences.
8. `docs/STATE_MACHINE.md` — every Task/Attempt transition, Worker state, persistence serialization and finalization phase.
9. `docs/OWNERSHIP.md` — TaskManager, WorkerRegistry, WorkerSession/ConnectionId lifetimes; resource reservations; invalidation and cleanup order.
10. `docs/FAILURE_MODEL.md` — dispatch uncertainty, cancellation, LOST, heartbeat failure, persistence fail-stop, client uncertainty.
11. `docs/DECISIONS.md` — especially ADR-005, -006, -007, -008, -009, -011, -012, -013, -018, -021, -022, -023.
12. `docs/TEST_STRATEGY.md` — Scheduler/resources and relevant state/fault integration tests.
13. `docs/PROTOCOL.md` — typed behavior and request/session semantics to model without network integration.
14. Accepted M1 domain/value types, M2 event-loop/wakeup interfaces, M3 protocol/net interfaces and **M5 storage/persistence value contracts and tests**.

Before coding, extract a short M6 invariant-to-module-to-test checklist. Treat contradictions as architecture blockers, not choices left to an implementation convenience.

## 4. Exact M6 deliverable and module ownership

M6 is the last **standalone Controller decision-core** milestone before application integration. Production logic must be real; fake dependencies belong in tests, not as hardcoded production stubs.

### 4.1 Pure `scheduler` module

- Accept immutable Task requirements and Worker/resource snapshots by value/reference without mutable scheduler-owned state.
- Implement frozen feasible-set filtering and deterministic projected dominant-utilization choice.
- Determine no placement cleanly when the feasible set is empty.
- Operate on a single Task selection and support a deterministic full-pass helper/Controller orchestration that scans pending Tasks oldest first, skipping infeasible Tasks without blocking later eligible work.
- Must depend only on `domain`/`common` as permitted by `docs/ARCHITECTURE.md`; **no SQLite, EventLoop, socket, protocol, or process runtime dependencies**.

### 4.2 Controller `ResourceLedger`

- One EventLoop/Controller decision-owner thread owns mutable Worker resource accounting.
- Maintain separate **tentative** and **committed** reservations for CPU slots, memory MB, and optional max-concurrent-task slots, keyed by stable Attempt/operation/Worker-session identities so repeated operations cannot double-reserve or double-release.
- Provide explicit reserve, commit, rollback, release, and session-loss release operations with invariant checking and typed failure values.
- Tentative reservations **immediately reduce scheduling availability**; commit changes designation, not total allocated capacity; rollback restores tentative resources; terminal/session-loss release occurs **only after matching successful persistence completion**.
- Refuse overcommit, unsigned overflow/underflow, stale/duplicate release, wrong Worker/session identity, and inconsistent ownership. No cgroups or actual resource enforcement.

### 4.3 Controller `WorkerRegistry`

- Own stable Worker records and at most one current logical WorkerSession per `worker_id`.
- Track fresh `session_id`, monotonically generated session generation, stable *non-owning* connection identity if an M6 fake adapter uses one, heartbeat deadline/liveness, advertised capacity/limits, and reservation-relevant eligibility.
- Represent internally pending registration/replacement/invalidation without inventing a new persisted WorkerState or treating an old transport lifetime as a current session.
- M6 has only data-level registration, heartbeat and invalidation **decisions** supplied by test inputs/fake transport; do not implement real registration protocol, listener, timer, or network handler (M7).
- Invalidate the old generation and stop new placement immediately upon close/timeout/replacement. Perform durable bulk session loss through the persistence port before release/externally observable `OFFLINE` finalization, old-connection close if still applicable, or replacement `HEALTHY` + ACK eligibility.
- After replacement begins, the old session must never receive new work or update a new session. Preserve old Attempt history; no restoration of live Worker state from SQLite.

### 4.4 Controller `TaskManager` and state orchestration

- Own EventLoop-side Task and Attempt state snapshots and transition decisions using stable value IDs and monotonic operation/generation tokens.
- Keep **persisted Task/Attempt states** separate from pending asynchronous operation/finalization phases. Never add artificial `PERSISTING`, `FINALIZING`, `INVALIDATING`, etc. to frozen domain or SQLite enums.
- Serialize at most **one outstanding metadata transition per Task**. Queue later relevant events by value and revalidate them against the newly committed state after each matching completion.
- Submit, assignment, ACK/START progression, pending cancellation, active cancel intent, terminalization, retry and session loss should be represented through M5-compatible typed commands/results where M6 legitimately controls decisions. Do not implement actual M7 protocol dispatch or M8 log barrier.
- Requests that require a database commit result cannot authorize a response, `RUN_TASK`, `CANCEL_TASK`, resource release, terminal visibility, or replacement registration before a **matching successful commit**.
- A stale completion is rejected using operation ID, entity ID, expected Task/Attempt version, WorkerSession/connection generation, and currently applicable transition context. `committed` and `error` in M5 outcomes are not interchangeable with a mere notification. Reject duplicate completion application.
- Model M5 `ErrorCode`/`Admission`/`Health` distinctions without masking storage errors. Business-invalid or rejected commands must not authorize effects; required fatal/unknown commits or loss of essential admission must invoke a value-only fail-stop/degraded decision for later M7 application handling. Never blindly retry an `UnknownCommit`.
- Side-effect decisions are emitted as values through an injected port/fake transport. Production M6 code must not send TCP bytes, open SQLite, join/stop the persistence worker, or synchronously wait for persistence.

### 4.5 Production port contracts

Use small typed interfaces or value-oriented adapters to represent:

- **Persistence submit:** nonblocking acceptance or rejection of the actual M5 `storage::Envelope`/`Command` data; correlation and reserved completion budget preserved.
- **Persistence completion input:** M5 `storage::Completion` values, explicitly delivered to the Controller decision owner.
- **External effects:** fake-observable actions (`submit response`, `RUN_TASK`, `CANCEL_TASK`, close old connection, registration ACK, terminal visibility/release) emitted only after the corresponding commit gate. Effects must distinguish an *eligible queued action* from confirmed network execution; M7 owns actual transport enqueue and subsequent uncertainty.
- **Clock/liveness:** deterministic monotonic time input for heartbeat/ACK/start deadlines, rather than sleeping real tests or accepting wall-clock wire timestamps.
- **Log-finalization prerequisite:** caller-provided typed `storage::LogEvidence`, bound to the exact Attempt and M5-established paths. M6 must never fabricate successful final barriers or pretend bytes were synchronized; M8 supplies the actual proof.

It is permitted to reuse M5 storage value types from the controller state core, but not to make `scheduler` depend on storage or make storage depend on controller. If production M6 can be built without linking actual SQLite/runtime operations, prefer that isolated testable arrangement.

## 5. Explicitly prohibited work

Do **not** begin M7, M8, M9 or M10:

- No production `apps/controller`, `apps/worker`, `apps/taskctl` orchestration or live complete cluster.
- No TCP listener/accept, real Worker registration/heartbeat network handlers, live connection/session adapter or `REGISTER_ACK` encoding/sending. Model only the M6 decisions and fake ports.
- No real `WORKER.RUN_TASK`, socket send, `TASK_STARTED` wire transport, child process launch or worker ExecutionManager integration.
- No LogWriter, directory creation/fsync, byte/slot credit issuance, durable log-file append, raw log capture, or filesystem-based log-finish proof.
- No complete cancellation/timeout/reconnect/kill/reap implementation, automatic retry, or multi-worker production service orchestration.
- No modifications to frozen wire fields/IDs, Task/Attempt/Worker state enums, v1 SQLite schema, retry guarantees, scheduler heuristic or failure mappings.
- No new persistent WorkerRegistry/session lease tables, no cgroups, preemption, quotas, job priorities, HA/replication, or external DB.
- No rewriting/refactoring accepted M1–M5 production modules to make M6 easier, except a demonstrably necessary small lower-layer defect explicitly documented and reviewed.
- No test-only fake Scheduler/TaskManager core in place of production algorithms; fakes are **ports**, not the system under test.

## 6. Recommended M6 implementation checkpoints

| Checkpoint | Implementation | Required evidence before advancing |
|---|---|---|
| **M6-A** | Pure Scheduler snapshot API, eligibility, projected dominant-utilization ranking, deterministic pending scan | Exact rational comparisons, ties, infeasibility, zero memory, skip-oldest vector tests |
| **M6-B** | ResourceLedger with tentative vs committed reservations and idempotence guards | Double-placement protection, rollback, post-commit release, randomized invariant traces |
| **M6-C** | TaskManager value-only command/completion state machine and persistence/effects ports | At-most-one transition, reordered/stale completions, no effect before commit, CAS/queue failures |
| **M6-D** | WorkerRegistry identity/session invalidation, heartbeat/timeout/replacement gates, combined fake-port flows | Old generation exclusion, bulk LOST, delayed release, replacement ACK gate, M1–M5 regressions |

Suggested reviewable commit progression: scheduler and vectors; ResourceLedger and invariants; TaskManager transaction serialization; WorkerRegistry/combined gates and fault tests. Use actual meaningful production implementations at every checkpoint; no placeholder success paths.

## 7. Scheduler: normative deterministic placement

For each Task ordered by `(created_at, task_id)` ascending:

1. Consider only Worker snapshots whose state is `HEALTHY`, whose current session is valid and not logically invalidating, and whose advertised CPU/memory plus optional concurrency limits admit the request **after both committed and tentative reservations**.
2. CPU must remain within CPU capacity. Memory must remain within advertised memory capacity. A Worker with `capacity_memory=0` is feasible only for a zero-memory Task; its memory utilization term is defined as **zero** in that case.
3. For each feasible Worker compute projected dominant utilization **exactly**:

   `max((used_cpu + requested_cpu)/capacity_cpu, (used_memory + requested_memory)/capacity_memory)`.

   Here `used` includes already committed and tentative reservations. Use checked widened arithmetic/exact integer-rational comparison, **not float/double or rounded percentages**. Avoid overflow for legitimate representable capacities/usages, including near-boundary values.
4. Choose the smallest projected dominant utilization. If mathematically tied, choose lexicographically smallest `worker_id` independent of input iteration order.
5. If an older Task is infeasible, leave it `PENDING` and consider later Tasks within the **same pass**. A single pass scans the pending order once; no busy-loop retry of infeasible work. New submit, healthy Worker, assignment rollback, or committed resource release may request a later pass.
6. The scheduler returns only a candidate/decision; it may not mutate the ledger, persist assignment, or dispatch. The Controller makes a tentative reservation and submits the transactional command.

Required vectors: CPU-bound vs memory-bound projected choice; zero-memory request and zero-memory capacity; exact equality and near-equality where floating-point would drift; lexicographic ties; input permutations; CPU/memory/concurrency limits exactly full; OFFLINE/stale/invalidating session; unsupported demand; older infeasible with younger feasible; tentative reservation altering the next choice; overflow candidates rejected explicitly.

## 8. ResourceLedger: reservation ownership and conservation

- All resource values must be finite, nonnegative, and checked before addition/subtraction; CPU slots and memory MB use accepted domain invariants and representable integer ranges.
- An owned reservation has a stable Attempt/operation identity, owning Worker and current Session generation, request dimensions, and state `tentative` or `committed` **inside the ledger only**. This is not a new Task/Attempt persistence state.
- A second reserve with the same ID is not a second reservation. Repeated commit/rollback/release is either explicitly idempotent with a proven unchanged state or returns a typed conflict; it must never double-change totals. State the policy in API/test comments rather than silently treating errors as success.
- `reserve` after selection is synchronous in EventLoop memory and blocks concurrent tentative placement. `rollback` happens for rejected/failed assignment before any dispatch. `commit` after a verified assignment SQLite commit designates the existing reservation committed. `release` after terminal/session-loss SQLite commit frees committed resources exactly once.
- If the original Worker session becomes stale while assignment is in flight, do not dispatch after commit. Conservatively issue an appropriate follow-up LOST persistence transition and delay release/visibility until it commits. The ledger must not silently discard the reservation on session generation mismatch.
- Track optional `max_concurrent_tasks` independently from CPU/memory; the optional limit does not override CPU/memory capacity feasibility.
- Check invariant at every transition: for each Worker, tentative + committed = usage; usage <= capacity in every dimension; no negative usage; no overflow; each live reservation contributes exactly once.
- Tests should use deterministic seeded state-machine traces as well as hand-authored adversarial cases. Do not require real socket or DB latency.

## 9. TaskManager: one-transition-in-flight serialization

- The persisted snapshot is changed on **matching successful** M5 persistence completion, not optimistically upon issuing a command.
- Track an outstanding transition per Task by stable Task ID, operation ID, expected Task/Attempt versions and relevant session generation. Queue later inbound events by value rather than invoking a nested conflicting transition; revalidate queued events after commit or failure.
- Cross-Task operations can complete in different orders. Within a Task, older incoming events may become invalid once newer durable facts exist and must not be replayed blindly.
- A committed assignment updates the Task to `ACTIVE`, creates/binds the Attempt `ASSIGNED`, designates the tentative reservation committed, and **only then** emits an eligible `RUN_TASK` action for the current matching session.
- A rejected/failed assignment rolls back the tentative reservation without sending dispatch; an actual fatal persistence failure also requests fail-stop, not a normal scheduling continuation.
- `RUN_TASK_ACK.accepted=true` maps to `ASSIGNED -> STARTING` after its persistence commit. `TASK_STARTED` maps to `STARTING -> RUNNING` only after its own commit. If START arrives during the ACK transition, retain/revalidate it instead of falsely rejecting or skipping STARTING.
- An explicit rejected RUN_TASK_ACK proves no execution and follows the frozen `FAILED/DISPATCH_REJECTED`/proven-empty path; a missing ACK or uncertain partial send may not be interpreted as the same thing.
- `PENDING` cancellation commits `PENDING -> CANCELLED` and sends a completion response only afterward, creating no Attempt. `ACTIVE` cancellation commits `cancel_requested_at`, keeps Task `ACTIVE`, responds `accepted` and makes Worker cancel eligible **only after commit**. If Assigned but no dispatch byte was queued, the proven-empty undispatched cancellation path may be selected; the outbound dispatch boundary is represented by a fake-port fact, not guessed.
- A terminal Attempt and matching Task become externally terminal only after the required LogWriter evidence and one atomic M5 `Terminalize` commit. A separate pending-finalization marker is local diagnostic, not a new state. Do not invent log evidence in M6.
- Manual retry commits `FAILED -> PENDING`, clears cancellation intent, preserves prior Attempts and historical latest reference, and does not auto-run a new Attempt before scheduling conditions are met.
- On conflicting/rejected/out-of-date completion, retain authoritative state and avoid side effects. **Do not assume a result with matching operation ID is current without verifying versions and session generation.**
- The future application-level fail-stop sequence is represented by emitted decisions: stop mutation admission and scheduling, invalidate/close sessions as required, bounded cleanup and terminal nonzero. M6 must not directly `exit()` or manipulate live TCP.

## 10. WorkerRegistry: liveness, replacement and durable LOST

- Stable `worker_id` is **not** a session or a connection. A fresh session UUID and monotonically advancing session generation identify each registered runtime lifetime; never accept a stale old-generation update as current.
- `HEALTHY` requires current registration completed, liveness deadline valid, and no pending invalidation; the registry must not treat mere socket connection as registration.
- Use monotonic time inputs for heartbeat and ACK/start report deadlines. Allow configuration of the accepted defaults (heartbeat ~3 s, offline timeout ~10 s, dispatch ACK ~5 s, start report ~10 s) subject to frozen config validation; don't use wall clock wire fields for liveness.
- Duplicate heartbeat sequence or stale session/connection must not refresh current liveness. If the original document does not define a precise duplicate-sequence policy, avoid inventing a new wire-level error response: implement conservative current-session validity checks and escalate true ambiguity to architecture review.
- On peer closure, heartbeat timeout, protocol failure or replacement request: invalidate old generation immediately; stop placement and old-session side effects; submit one value-level session-loss persistence action covering **all** its nonterminal Attempts atomically.
- Do not release any committed reservations, mark external `OFFLINE`, close an otherwise-still-open old connection, or accept replacement as `HEALTHY`/ACK-eligible until the matching session-loss commit is confirmed; if the socket has already physically closed, do not resurrect it.
- If the old session is replaced while an assignment commit is pending, handle assignment-result reordering conservatively: after assignment commit, no old-session dispatch, and the stale Attempt must durably become `LOST` (possibly requiring another serialized transaction) before resources become reusable. No fake synchronous database action is permitted as a shortcut.
- On an old session-loss completion arriving after a newer session exists, compare session identity and generation before updating live state; do not turn the newer session OFFLINE or release its reservations.
- Worker live registry remains memory-only. Startup recovery is M5; M6 may accept a recovered snapshot by value, but must not open SQLite or reconstruct old Worker live leases.

## 11. Boundaries at persistence and external effects

M5 exposes typed `storage::Envelope`, `Correlation`, `Completion`, `Outcome`, `Admission`, and `Health`. Build adapters against their real definitions; do not invent a subtly incompatible parallel persistence protocol.

**Required commitment gate:**

```text
Controller decision + expected version/session generation
    -> (if assignment) tentative ResourceLedger reservation
    -> value-owned persistence command admitted through fake port
    -> no network/client/resource-release effects while pending
    -> matching successful M5-style committed completion
    -> revalidate operation, Task/Attempt versions, session generation
    -> apply in-memory transition and reservation phase change
    -> emit eligible external action through fake transport/effect port
```

For completion that is stale/duplicate, unsuccessful, aborted, unknown-commit, or from an invalidated session, assert **zero incorrectly authorized effects**. A stale result may still describe a durable SQLite operation: do not pretend to undo it with memory-only changes. Route required reconciliation into a new correctly serialized durable operation (not a hidden fake rollback).

The `Outcome` `committed` bit is **false for successful reads**; do not require `committed=true` for read-only results or confuse it with eventfd notification. Correctly distinguish business state/version conflicts from fatal SQLite unavailability and uncertain commits; if their exact orchestration mapping changes frozen semantics, STOP for clarification instead of guessing.

Completion ordering should also model `CANCEL_TASK` arriving while an assignment is pending, `TASK_STARTED` arriving while STARTING is pending, session loss while several Tasks have transitions in flight, and same operation ID with mismatched expected state or generation.

## 12. No hidden asynchronous deadlocks, unbounded queues or priority inversions

- M6 production logic is single-owner EventLoop-compatible and **nonblocking**. No mutex wait, condition-variable wait, SQLite query, socket send, or process spawning on its decision path.
- If M6 retains pending Task events or side-effect intentions while a persistence command is inflight, impose a finite configurable bound for retained command/event metadata; overflow has an explicit backpressure/fail-stop policy consistent with the frozen architecture. Do not hide an unbounded `std::deque` or unlimited callback recursion.
- Essential session-loss and terminal commands must remain able to make progress under ordinary query/submit pressure. Respect M5's essential admission reservation, or report explicit degraded/fail-stop when admission is impossible.
- When a scheduling trigger arrives during an existing pass, coalesce it into a later bounded pass; avoid recursively scheduling from completion callbacks or busy looping over infeasible oldest Tasks.
- Tests should prove no second scheduling pass overcommits while the first assignment completion is delayed, and no essential state transition becomes impossible solely because ordinary traffic occupies every slot.

## 13. Failure and lifetime invariants

- Persistence failure/`UnknownCommit` does not release resources, expose terminal state or emit a successful response. The later Controller application must fail-stop; M6 emits an explicit value-only degraded/fail-stop decision.
- Ordinary CAS conflict cannot silently enable an irreversible effect. Review whether a particular conflict requires rollback, revalidation, conservative LOST, or fatal treatment under the existing docs; do not mark all errors interchangeable.
- Session close/loss means uncertain execution, not that the child is certainly stopped. No auto-retry or exactly-once claim.
- Client disconnection never cancels an already committed Task; a lost submit response is an unknown outcome to the client, not permission to create a replacement Task automatically.
- Domain objects belong to the Controller decision owner; Scheduler only borrows immutable snapshots. No cross-thread lambda owns raw Task, Attempt, WorkerSession or TcpConnection pointers. Completion values are matched by stable IDs and generations.
- Worker connection IDs are non-owning references; M3 owns actual TCP sockets. The M6 fake transport port only records *eligible* effects and exposes sent/unsent/uncertain dispatch facts in tests.

## 14. Mandatory scheduler and ledger test matrix (M6-A/B)

- Exact frozen projected dominant-utilization ranking: CPU-dominant, memory-dominant, equal ratios and near-ties.
- Cross-worker candidate order shuffled/randomized; result identical, worker-ID tie break exact.
- Zero advertised memory + zero request, zero advertised memory + nonzero request, exact capacity boundaries.
- `HEALTHY` vs `OFFLINE`, invalidating vs current valid sessions, optional `max_concurrent_tasks`, CPU and memory insufficiency independent of each other.
- Oldest `PENDING` Task blocked, younger eligible scheduled same pass; no scan restart/forever spin; stable `(created_at, task_id)` ties.
- Tentative reservation consumes available capacity immediately, assignment commit does not double count, assignment rejection restores capacity, release happens only after durable terminal/LOST.
- Duplicate/mismatched Attempt or session reservation cannot steal resources; no negative usage, overflow, leak or double-release.
- Deterministic seeded randomized reserve/commit/rollback/release traces with invariant assertions at **every** transition.

## 15. Mandatory TaskManager gate tests (M6-C)

Use a deterministic fake persistence port that **captures actual M5 command/envelope values** and can deliver M5-compatible completion results later, in arbitrary order. Fake transport/effects may record actions but never perform real socket calls.

- Submit success response strictly after committed insertion; failure emits no success.
- Assignment: tentative reserve before `Assign`, no `RUN_TASK` before successful matching completion, no dispatch after rollback/stale session.
- ACK accepted and START events arriving out of order while STARTING transaction is pending; durable `ASSIGNED -> STARTING -> RUNNING` progression, no artificial state jump.
- `PENDING` cancellation vs active cancellation intent; no cancellation transport effect before commit, no fake terminal completion from cancel ACK.
- Terminalization: no terminal visibility/resource release before valid barrier evidence plus committed Task/Attempt transaction; log-barrier failure never claims complete logs.
- Explicit dispatch rejection vs uncertain/missing ACK vs no-dispatch cancellation. Proven-empty limited to frozen cases.
- Manual retry only for `FAILED`; no historical Attempt mutation, no auto retry.
- At most one outstanding metadata transition per Task; later events queued/revalidated by ID; bounded queue behavior.
- Cross-Task completion reordering, duplicate completions, wrong operation ID, wrong Task/Attempt ID, wrong state version, wrong session generation, mismatched completion data.
- Storage ordinary admission pressure vs essential admission failure; fatal IO/FULL/BUSY and UnknownCommit trigger explicit safe degraded decisions and **no external success**.
- `eventfd` notification without a matching committed completion is never taken as a commit proof.

## 16. Mandatory WorkerRegistry and combined tests (M6-D)

- Distinct worker_id, session_id, connection ID, and session generation; replacement gets new generation and cannot accidentally retain currentness of old session.
- Valid current heartbeat updates liveness; stale/invalid session or past deadline does not renew it; timeout uses monotonic fake time.
- Session-loss action selects all bound nonterminal Attempts and no unrelated session; bulk LOST is one persistence command; no early reservation release or OFFLINE exposure.
- Old connection close and new registration/ACK eligibility occur only after the required old-session LOST commit. Existing socket-already-closed case remains safe.
- Replacement while assignment persistence is pending; committed stale assignment becomes conservative LOST and never RUN; later stale completion must not invalidate a new session.
- Multiple bound Tasks where one transition is already in flight: per-Task serialization and deferred LOST reconciliation; no premature release while late in-flight assignment exists.
- Session-loss persistence failure or essential admission exhaustion: fail-stop decision, no resource reuse or new-session acceptance.
- Combined deterministic path: Worker healthy -> two Tasks scheduled -> tentative reservations -> commits -> fake RUN -> ACK/START -> terminal or session loss -> matching completion -> exact single release.
- Resource conservation and generation safety after repeated churn, including identical fake numeric connection/fd reuse.

## 17. Build, verification and regression policy

Use the established formal Linux configuration; do not depend on live network/controller/worker binaries for M6 unit/component acceptance.

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
```

- Preserve all M1–M5 test targets. Add a dedicated M6 test target/CTest label `m6` and independently named scheduler, ledger, TaskManager and WorkerRegistry tests; follow existing GoogleTest/CMake conventions and `-Werror` project warnings.
- Repeatedly run `ctest --test-dir /home/nhzj121/build/task_scheduler -L m6 --repeat until-fail:20 --output-on-failure` after success. Run selected deterministic randomized traces/combined churn repeatedly and report count/seed.
- Inspect actual tests and source; no TODO/mock success for the production decision core. Do not claim tests succeeded merely because a fake unconditionally commits.
- For M6, fake-port harnesses are **required**; real M5 SQLite tests remain regressions but are not substituted for M6 delayed/reordered completion tests.
- Check `git diff --check`, dependency graph, Git history, memory bounds and owner/lifetime assumptions. If sanitizer checks were actually run, report the commands and results; otherwise explicitly report not run.
- Do not modify M1–M5 tests to bypass a failing M6 case or weaken `-Werror`.

## 18. Focused pre-merge source review questions

Answer each from actual code and tests:

1. Is Scheduler pure and deterministic, with exact rational comparisons and a documented zero-memory rule?
2. Does ResourceLedger include tentative usage and prevent both capacity overcommit and double release?
3. Does TaskManager hold only one metadata transition per Task, with bounded deferred events and strictly matching completion correlation?
4. Can any response, RUN, CANCEL, terminal visibility, resource release or replacement ACK happen before required persistence success?
5. Can an old session completion, heartbeat or dispatch effect mutate a new session after generation change?
6. Does session-loss failure leave reservations owned and trigger fail-stop rather than continuing to schedule?
7. Does the M6 production core avoid real SQLite/socket/EventLoop blocking and M7/M8/M9 implementation?
8. Are pending orchestration phases private and distinct from frozen domain state enums, including finalization?
9. Are fake tests adversarial (late/stale/double/out-of-order completions), not just happy-path mock calls?
10. Does the full suite remain green, including M3/M4/M5 ownership and durability regressions?

## 19. Explicit STOP conditions

**Stop for architecture review** if correct M6 behavior demands any of the following:

- a new v1 Scheduler metric, task priority/fairness mechanism or floating-point approximation replacing exact ratios;
- a new persistent Task/Attempt/Worker state, Session table or claim that live workers survive restart;
- an unapproved policy for whether a possibly committed assignment may dispatch to a stale session;
- treating a failed SQLite transaction as an authorized response or reversible external action;
- releasing reservations on Session invalidation **before** required LOST durability;
- claiming a valid final-log durability barrier without M8 evidence;
- arbitrary policy changes to cancellation/retry, dual current Worker sessions, or missing-ACK uncertainty;
- a truly unresolved interaction among per-Task serialization, bulk session-loss transaction and in-flight assignments where the existing documents do not determine safe ordering;
- any forced reversal of frozen module dependency direction.

Clearly distinguish a fixable implementation bug from a real missing architectural decision. Do not ask to re-authorize normal M6-A through M6-D progress.

## 20. Commit, push, STOP

When **all** M6 acceptance items are proven:

1. Ensure `milestone-6-scheduler-controller-state` contains only authorized changes; preserve user files and the existing build note.
2. Create small commits covering M6-A/B/C/D and any necessary focused cleanup.
3. Push only the M6 branch and verify remote HEAD against local HEAD. Do not merge, force-push or create/merge a PR without further instruction.
4. Verify `main` / `origin/main` are unchanged by M6 development and M6 branch descends from accepted mainline.
5. Run full `git diff --check main...HEAD` and inspect the actual diff and commit list.
6. Produce the final report below. Then **STOP**. Do not start M7, M8 or M9.

## 21. Required M6 completion report format

### A. Git / baseline

Accepted main SHA, M6 final full SHA, commits, ahead/behind, remote sync, clean status, personal-note and build-note preservation, no merge.

### B. Implementation summary

What actually ships in M6-A through M6-D, module and ownership boundaries, and which downstream responsibilities remain only as fake ports.

### C. Files changed

Every added/modified/deleted path, production and tests, CMake targets and module dependencies.

### D. Scheduler correctness

Feasibility, exact ratios and overflow strategy, tie-break, zero-memory, stable pending ordering and skip-infeasible behavior, with tests.

### E. ResourceLedger correctness

Tentative/committed/concurrency counters, reservation identities, rollback/commit/release, session-loss handling, randomized invariant evidence.

### F. TaskManager serialization and durability gates

One transition per Task, deferred event bounds, M5 command and completion correlation, submit/assignment/ACK/START/cancel/retry/terminal gates, stale/duplicate/out-of-order results, no side effects precommit.

### G. WorkerRegistry and session safety

Generation/liveness/heartbeat/replacement, bulk LOST, no premature release, correct close/ACK eligibility, stale session behavior.

### H. Failure model and known integration limits

Distinguish ordinary conflict/backpressure from fatal/unknown persistence outcome, fail-stop decisions; explicit limits of fake port and absence of real M7/M8 integration.

### I. Complete acceptance matrix

PASS/FAIL with exact tests for every mandatory case in Sections 14–18. Do not hide untested or blocked rows.

### J. WSL build / tests / stress

Exact shell commands, GCC/CMake version, `-Werror` evidence, total CTest pass/fail, M6 count and 20x repeats, deterministic seeds, any FD/RSS/concurrency diagnostics, sanitizer status and preserved `BUILD_COMMANDS.md`.

### K. Architecture review and deviations

Relevant frozen documents checked; module dependencies, state/ownership/persistence/failure compliance; every approved clarification or unresolved concern. No claiming M7+ behavior from fake-port tests.

### L. Gate

If and **only if** implementation and all M6 acceptance requirements pass:

**Milestone 6: PASS — awaiting explicit independent review; Milestone 7 and later milestones have not started.**

Otherwise:

**Milestone 6: NOT PASSED — blockers remain.**

## 22. Final instruction

**Begin M6 only after the Git preflight succeeds. Implement the complete deterministic Scheduler, ResourceLedger, TaskManager, and WorkerRegistry state-decision core on `milestone-6-scheduler-controller-state`, using actual M5-compatible value ports and adversarial fakes for deferred completions and effects. Preserve every frozen contract, validate all tests, push the M6 branch, report, then STOP.**
