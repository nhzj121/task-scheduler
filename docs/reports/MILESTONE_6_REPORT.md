# Milestone 6 implementation and acceptance report

## A. Git / baseline

Accepted `main` and fetched `origin/main`: `699ac82b2af67e58da301465c945e4a0f8240bb3`.
Accepted M3, M4 and M5 commits were verified as ancestors before development.
All work is on `milestone-6-scheduler-controller-state`; neither main nor its remote was
updated by this work. The only pre-existing uncommitted authorization file was carried
unchanged onto this branch. No PR or merge is performed.

Production/test commits, in dependency order:

- `b6ee7c1`: M6-A exact scheduler and authorization.
- `2b8e739`: M6-B resource ledger.
- `a29a0df`: standalone M6-D registry, needed by TaskManager.
- `3bf2b3e01d714e99b99ec85a5c0489fa57d999d3`: M6-C and combined M6-D gates/tests.

This report is a subsequent documentation-only commit. Its full delivery HEAD and verified
remote synchronization are recorded in the final handoff message (a report cannot embed its
own commit hash). The final branch is five commits ahead of the unchanged mainline.

Ignored `.local-notes/reading-notes.md` remains local and unchanged; SHA-256:
`4b2e5a53c77198e9278ebdd3cf40be93ec4468d5707435f4729d26b4dabbc882`.
`BUILD_COMMANDS.md` was not deleted or edited; before/after SHA-256:
`8bda49a0af4e612892bafdba48effb924573e02a6ccea33b3aa8bd5991d813ae`.

## B. Implementation summary

M6-A ships a pure immutable-snapshot Scheduler and deterministic pending ordering.
M6-B ships a single-owner ResourceLedger with tentative and committed reservations.
M6-C ships production TaskManager decisions, bounded retained metadata, actual M5 command/
completion types and nonblocking value-only persistence/effect ports.
M6-D ships WorkerRegistry identities/liveness/replacement and serialized bulk LOST decisions.

The core does not run an EventLoop, talk to SQLite, encode/send TCP messages, launch processes,
write logs or compose Controller/Worker applications. Deterministic test ports capture actual
M5 Envelopes and eligible effects; production state decisions are not faked.

## C. Files changed and dependencies

Modified:

- `CMakeLists.txt`.

Added:

- `docs/spec/Milestone_6_Authorization_Codex_Development_Prompt.md` (authorized pre-existing file).
- `docs/reports/MILESTONE_6_REPORT.md`.
- `include/ts/scheduler/scheduler.h`.
- `src/scheduler/scheduler.cpp`.
- `include/ts/controller/resource_ledger.h`.
- `src/controller/resource_ledger.cpp`.
- `include/ts/controller/worker_registry.h`.
- `src/controller/worker_registry.cpp`.
- `include/ts/controller/task_manager.h`.
- `src/controller/task_manager.cpp`.
- `tests/unit/scheduler_test.cpp`.
- `tests/unit/resource_ledger_test.cpp`.
- `tests/unit/worker_registry_test.cpp`.
- `tests/component/controller_state_test.cpp`.

No files removed. No accepted M1-M5 production/test file or frozen architecture file changed.
Targets: `ts_scheduler -> ts_domain`; `ts_controller_state -> ts_scheduler, ts_storage`;
`ts_milestone6_tests -> ts_controller_state, GTest::gtest_main`, CTest label `m6`.
Storage linkage uses M5 value helpers, not StateStore/PersistenceThread execution. Inspection
of unresolved M6 archive symbols found no SQLite, net, io or runtime calls; `ldd` on the M6
test binary contains only standard C/C++ runtime libraries. No dependencies added.

## D. Scheduler correctness

Only HEALTHY, current, non-invalidating sessions with enough CPU, memory and independent
optional concurrency capacity qualify. Usage includes tentative and committed reservations.
The projected dominant CPU/memory fraction is compared with a continued-fraction algorithm:
division/remainder only, no floating point, no overflowing cross products, no compiler-specific
integer extension. Feasibility uses subtraction before sums. Zero memory capacity only admits
zero memory demand; its memory term is zero. Exact ties choose lexicographic worker_id.
Pending tasks sort by `(created_at, task_id)` and scan once, skipping infeasible older tasks.
Large requests can starve under sustained smaller requests; there is no aging or priority.

## E. ResourceLedger correctness

Reservation identity binds Attempt ID, operation ID, Worker/session generation and non-owning
connection ID/generation. Reserve consumes tentative usage immediately. Commit only changes
designation; rollback removes a definitely failed/unadmitted assignment; terminal/bulk LOST
commit authorizes committed release. Duplicate/wrong-phase operations return typed Conflict.
Capacity, concurrency and arithmetic boundaries are checked; audit recomputes conservation.

Seed 602: 5,000 steps per trace, checking an independent live-reservation model, every tentative/
committed counter and production audit after every transition. Identity inputs are deterministic.
UnknownCommit retains tentative ownership; known failed Assign rolls it back before fatal
fail-stop if necessary. No memory rollback pretends to undo a committed SQLite assignment.

## F. TaskManager serialization and durability gates

One inflight metadata transition per Task. Completions must exactly match M5 Correlation
(operation, entity/Task/Attempt/session IDs, expected versions and generations); pending context
also retains the complete connection/session binding. Successful mutation payload must have
`committed=true`, correct affected count and no unexpected read/page rows. A notification or
successful read is not mutation proof. Stale, duplicate and malformed results authorize nothing.

Snapshots and effects change only after matching durable completion: submit -> PENDING response;
Assign -> ACTIVE/ASSIGNED + committed reservation -> eligible RUN; accepted ACK -> STARTING;
START -> RUNNING; cancel intent -> response and eligible CANCEL, never completed cancellation
from a cancel ACK; retry -> PENDING, preserving all historical Attempts. A known-unsent ASSIGNED
cancel and explicit dispatch rejection are the only proven-empty shortcuts.

Finished observation without evidence retains private finalization metadata and nonterminal
state. Attempt-bound actual FinalBarrier evidence is required before issuing ordinary Terminalize;
both paths must match assignment. SQLite terminal commit precedes ledger release and visible
terminal effect. Log failure uses incomplete logs, FAILED/LOG_PERSISTENCE_ERROR and retained
observed outcome. M6 never creates or executes a log barrier.

Default bounds: 64 pending operations / 4 MiB owned-envelope bytes with four operations / 64 KiB
reserved from ordinary admission; 32 deferred events per Task, 1,024 globally / 4 MiB owned event
metadata; 64 event decisions per turn. Held final observations count against event bounds.
Ordinary Full/NotReady returns backpressure; irreplaceable event/essential admission exhaustion
fail-stops. These are pending metadata bounds, not a total RSS/history-cache bound. Triggers
coalesce; completion does not recursively run another placement pass.

## G. WorkerRegistry and session safety

Worker ID, fresh session UUID, monotonically advancing session generation and connection
ID/generation are distinct. Advancing valid heartbeat sequences update monotonic liveness;
duplicates, stale identities and expired sessions do not. UTC timestamps do not drive deadlines.
Defaults remain heartbeat 3 s, offline 10 s, ACK 5 s and START 10 s, configurable with validation.
Observed ACK/START/finish facts cease being classified as missing reports while metadata/log
completion is pending.

Invalidation immediately disables old-session placement/effects. It waits for all old-session
inflight operations, including Assign, to settle before posting one bulk LoseSession. A committed
stale Assign is recorded but never RUN; its reservation remains owned until LOST commits.
While bulk loss is pending, bound Tasks cannot start another transition. On bulk commit only
bound live Attempts become LOST; release precedes OFFLINE/old-close/new-ACK eligibility.
Already-closed sockets receive no second close effect. Replacement activation gets a fresh
deadline/generation; late old results cannot touch it. Live leases are never restored from DB.

## H. Failure model / integration limits

Ordinary assignment conflicts roll back tentative usage and request a later pass. Active
Advance/CancelIntent business conflicts use conservative serialized session-loss reconciliation,
not a false state jump or memory rollback. Fatal IO/FULL/BUSY/aborted work, uncertain commits,
required admission failures or effect-port failure produce explicit fail-stop values; further
mutation/scheduling stops. Committed reservations and prior visible state remain held until a
proven terminal/LOST commit; UnknownCommit never authorizes external success.

Future adapters must outlive the owner, be bounded/nonblocking/non-reentrant, consume effects
after the current decision call, revalidate delayed RUN/CANCEL eligibility, and record actual
dispatch facts before queuing RUN bytes. An eligibility value is not proof of transmission;
explicit uncertain send invalidates the session. ClientGone drops only responses, never a Task.
M7 must integrate these ports and fail-stop shutdown. M8 must supply real log evidence and
namespace durability. There is no claim of end-to-end network/log correctness from M6 fakes.
Task/history snapshots and used-session UUID tombstones are retained for this core lifetime;
there is no eviction/fairness/fencing/exactly-once extension in M6.

## I. Complete acceptance matrix (authorization sections 14-18)

All rows PASS. Names below are actual GoogleTests. `S` = Scheduler, `R` = ResourceLedger,
`C` = ControllerState, `B` = ControllerBounds, `F` = ControllerFailures, `W` = WorkerRegistry.

| Contract | Exact test evidence |
|---|---|
| CPU/memory/projected ranking | S.CpuAndMemoryDominantPlacementUsesProjectedNotCurrentLoad |
| Exact equality, near ties, overflow | S.ExactRatiosDistinguishNearEqualityAtUint64Boundary; S.InvalidOverflowSnapshotsAreExplicitFailures |
| Permutation/tie stability; independent ratio oracle | S.TieBreakIsIndependentOfSeededInputPermutations; S.SeededExactComparatorAgreesWithIndependentBoundedCrossProductOracle |
| Zero memory; CPU/memory/concurrency boundaries | S.ZeroMemoryAndIndependentCapacityAndConcurrencyFilters |
| HEALTHY/current/invalidating filters | S.OfflineInvalidAndInvalidatingSessionsAreNeverCandidates |
| Pending order and skipping infeasible older | S.PendingOrderIsStableAndAllowsOnePassSkippingInfeasibleOldest; C.OldestInfeasibleTaskDoesNotBlockYoungerAndTentativePreventsOvercommit |
| Tentative usage, designation, rollback, duplicate release | R.TentativeCommitRollbackReleaseAndDuplicateGuards; R.BoundaryArithmeticAndTentativePlacementVisibility; C.RejectedAssignmentRollsBackOnlyTentativeReservation |
| Wrong reservation/session identity | R.WrongOperationConnectionOrSessionCannotStealReservation |
| Independent randomized conservation at each step | R.SeededReservationTracesConserveResourcesAtEveryStep |
| Submit after commit, not notification/read | C.SubmitVisibilityAndResponseRequireMatchingCommittedInsertion |
| Submit failure; client disconnect | C.BusinessSubmitFailureAndDisconnectedClientNeverCancelCommittedTask |
| Assignment gate and no delayed overcommit | C.AssignmentUsesTentativeCapacityAndNeverRunsBeforeCommit |
| Wrong operation/entity/versions/generation/data, duplicates | C.WrongOperationEntityVersionsAndCompletionPayloadCannotAuthorizeRun |
| Durable ACK then queued START; no jump | C.AckAndQueuedStartAreSerializedWithoutSkippingDurableStates; C.StartBeforeAcceptedAckCannotInventStartingTransition |
| PENDING cancellation | C.PendingCancelIsDurableWithoutCreatingAttemptOrWorkerCommand |
| Active cancel persistence gate/idempotent intent | C.ActiveCancelIntentMustCommitBeforeTransportAndIsNotTerminal |
| Cancel ACK not terminal; natural result wins | C.CancelAckIsOnlyReceiptAndNaturalResultCanStillWin |
| Cancel while Assign pending; no dispatch bytes proof | C.CancellationDuringPendingAssignmentRevokesUnsentRunAndUsesProvenEmpty; C.ExplicitAdapterUnsentFactAllowsProvenEmptyButUncertainEligibilityDoesNot |
| Uncertain eligibility not proven empty | C.UncertainDispatchCancellationUsesIntentNotProvenEmpty |
| Final barrier + terminal commit gate; invalid evidence/path | C.FinalLogEvidenceAndCommitBothGateTerminalVisibilityAndRelease; C.ObservedLeaderOutcomeCannotChangeWhileWaitingForBarrier |
| Log failure preserves outcome/incomplete logs | C.LogPersistenceFailureKeepsObservedOutcomeAndNeverClaimsCompleteLogs |
| Explicit rejection; retry preserves real production history | C.DispatchRejectionIsProvenEmptyAndManualRetryPreservesHistory |
| Retry only FAILED, no automatic retry | C.RetryIsRejectedForPendingActiveSucceededAndCancelled |
| Bounded per-Task/global events, bytes, pending operations | B.EventCountAndByteOverflowAreExplicitAndDoNotGrowQueues; B.PendingByteBoundRejectsOversizedMetadataBeforePortAdmission; C.RequiredWorkerEventOverflowFailStopsWithoutReleasingReservation |
| Ordinary pressure vs essential capacity | C.OrdinaryPressureRetainsWorkerEventAndEssentialTerminalUsesReservedCapacity; B.NormalPendingCountCannotConsumeEssentialReservation |
| Essential admission failure | C.EssentialAdmissionFailureNeverReleasesOrExposesTerminal; C.EssentialSessionLossAdmissionFailureDoesNotReuseOrAckNewSession |
| Fatal IO/FULL/BUSY/aborted/unknown | F.FatalIoFullBusyAbortedAndUnknownCommitNeverAuthorizeRunOrRelease; C.TerminalCommitFailureHoldsResourcesAndLastCommittedState |
| Conflict rollback/reconciliation, no recursive scheduling | C.AssignmentRollbackRequestsLaterPassWithoutRecursivePlacement; C.ActiveCasConflictReconcilesThroughDurableLostWithoutFakeRollback; B.NestedSchedulingTriggerCoalescesInsteadOfRecursing |
| Session/connection identities and generation | W.ReusedSessionOrConnectionAndUnboundedReplacementAreRejected; W.ReplacementIsNotHealthyUntilOldDurableCleanupCompletes |
| Heartbeat sequence, monotonic deadline and expiry | W.CurrentHeartbeatSequenceAndMonotonicDeadlineAreAuthoritative; C.HeartbeatExpiryUsesMonotonicTimeAndDoesNotExposeOfflineBeforeCommit |
| Whole bound session; no unrelated Attempt | C.SuccessfulBulkLossLeavesUnrelatedAttemptAndReservationIntact |
| Bulk loss gate, replacement during inflight Assigns | C.BulkLossWaitsForEveryInflightAssignmentAndGatesReplacementAck |
| Multiple Tasks, inflight Advance and Assign | C.MixedInflightAdvanceAndAssignmentSerializeBeforeOneBulkLoss |
| Lost failure holds resources and blocks replacement | C.BulkLossDoesNotTouchAnotherSessionAndUnknownCommitBlocksReplacement |
| Already-closed socket, retained heartbeat | W.ClosedSocketAndOfflineRecordRetainHeartbeatWithoutLiveLease; C.MixedInflightAdvanceAndAssignmentSerializeBeforeOneBulkLoss |
| Stale Worker facts cannot consume queue/mutate new session | C.InvalidatedSessionRejectsFactsWithoutConsumingDeferredCapacity; C.BulkLossWaitsForEveryInflightAssignmentAndGatesReplacementAck |
| Missing ACK / START and uncertain send become LOST | C.MissingAckStartDeadlineAndHeartbeatExpiryUseConservativeLost; C.MissingStartInvalidatesEvenWithAdvancingHeartbeats; C.ExplicitUncertainSendInvalidatesRatherThanPretendingDispatchRejection |
| Received reports aren't falsely timed out during persistence/log wait | C.ReceivedAckDuringPendingCancelDoesNotExpireAsMissingAck; C.LaunchResultAwaitingLogsDoesNotExpireAsMissingStart |
| Inflight terminal commit wins before later bulk loss | C.InflightTerminalCommitWinsBeforeDeferredLossWithoutDoubleRelease |
| Queued client event revalidates after terminal winner | C.DeferredClientRequestRevalidatesAfterFirstTerminalWinner |
| Frozen launch/timeout/cancel mapping | C.LaunchFailureAndTimeoutAndCancellationMapOnlyFrozenTerminalStates |
| Recovery snapshots, no restored live Workers | C.RecoveredPendingAndTerminalHistoryDoNotRestoreWorkerLeases |
| Combined RUN/ACK/START/terminal/loss churn and numeric connection reuse | C.SeededCombinedChurnMaintainsGenerationAndResourceConservation |
| Explicit effect-port failure; owner/limit guards | B.EffectPortFullIsExplicitFailStopNotSilentSuccess; B.OwnerAndLimitGuardsRejectWorkWithoutCallingPorts |
| Existing M1-M5 behavior | All 299 pre-existing CTest cases, unchanged |

Section 18 source review answers:

1. PASS: Scheduler is pure, exact, deterministic; zero-memory term explicit.
2. PASS: ledger includes tentative usage, independently checked counters and phase conflicts.
3. PASS: one pending mutation, finite metadata budgets and exact correlation.
4. PASS: required persistence gates precede eligible effects and committed resource release.
5. PASS: old-session commits are accounted but cannot dispatch or mutate replacement state.
6. PASS: loss failure holds ownership and fail-stops; no replacement acceptance.
7. PASS: no M7+ integration or direct DB/socket/EventLoop/process execution in M6 core.
8. PASS: pending/finalization are private orchestration metadata; domain enums unchanged.
9. PASS: fakes deliberately reorder, reject, corrupt, duplicate and delay completions.
10. PASS: all existing regressions pass unchanged.

## J. WSL build / tests / stress

Formal environment: WSL `Ubuntu22.04.3`, Ubuntu 22.04, GCC 11.4.0, CMake 3.22.1, C++17.
Actual M6 target flags:
`-g -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror -std=c++17`.

Final exact Linux commands (invoked using `wsl.exe -d Ubuntu22.04.3 -- bash -lc`):

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -L m6 \
  --repeat until-fail:20 --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -R Seeded \
  --repeat until-fail:20 --output-on-failure
sha256sum /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
```

Logs were redirected to `/tmp/task_scheduler_m6_build.log`, `task_scheduler_m6_full.log`,
`task_scheduler_m6_repeat.log`, `task_scheduler_m6_seeded.log`. Final results:

- Full clean build: PASS; 364/364 CTest, zero failures (12.07 s).
- M6: 65 distinct tests; 20 successes each = 1,300 successful executions (3.37 s).
- Extra seeded run: four tests, 20 successes each = 80 executions (1.43 s).
- Seed 601: 500 shuffled tie-order vectors per run.
- Seed 602: 5,000 ledger transitions per run with independent counters at each step.
- Seed 603: 1,000 comparisons per run against an independent bounded cross-product oracle.
- Seed 604: 80 combined two-Task session churn cycles per run, randomized commit order/result.
- No sanitizer run or new FD/RSS benchmark is claimed. M6 does not own sockets/processes;
  all existing networking/runtime ownership regression tests remain green.
- `nm -C --undefined-only libts_controller_state.a`, source/include inspection and `ldd`
  confirmed the dependency boundary. Git checks use the established Windows Git CRLF
  configuration; Linux Git's different line-ending settings were not used to rewrite files.
- `git diff --check main...HEAD`, full path/stat inspection and commit inspection passed.
  CMake SHA-256 before/after splitting the commits is identical:
  `bc9e13baef97d88e04f60e6642276853f17ca373784ab325cdb4f05a8a936edb`.

## K. Architecture review and deviations

Checked AGENTS, Architecture Gate, Full Implementation Prompt, complete M6 Authorization,
PROJECT_SCOPE, ARCHITECTURE, OWNERSHIP, STATE_MACHINE, FAILURE_MODEL, PROTOCOL,
TEST_STRATEGY, IMPLEMENTATION_PLAN, DECISIONS, DEPLOYMENT, INTERVIEW_NOTES,
Final Scope and Architecture Review Checklist.

No frozen state name/transition, protocol ID/schema, persistence ordering, placement algorithm,
ownership or log durability rule changed. No production placeholders/TODO paths or accepted
test weakening. No architecture contradiction requiring human clarification was found.
Completion serialization and reconciliation implement the existing conservative LOST policy;
there is no additional architectural decision or scope deviation.

## L. Gate

Implementation and acceptance checks PASS, pending explicit independent review. The final
handoff message supplies the verified pushed HEAD and clean branch status. No merge and no
M7, M8 or M9 work is authorized or started by this delivery.

Milestone 6: PASS — awaiting explicit independent review; Milestone 7 and later milestones have not started.
