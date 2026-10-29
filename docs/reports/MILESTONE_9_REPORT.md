# Milestone 9 acceptance report

Date: 2026-10-29 (Asia/Shanghai). Scope: M9-A through M9-D only.

## A. Git, baseline and protected material

- Accepted `main` and `origin/main`: `c18014eb474c5bcff9e9916fe648a760b0d631fa`.
- Accepted M8 implementation: `75677e5cca70e153891c1a5600e8000dea365101`, verified ancestor of both main refs.
- Work branch: `milestone-9-cancellation-recovery`, created from accepted main; no main changes, reset, force-push or merge.
- Implementation commit: `f68fc692d2e739c64dbc0acc57afdd0d161a01ba` (`Complete M9 cancellation, retry, session cleanup and recovery`). The subsequent `Record M9 acceptance evidence and tested local demo` commit is the delivery HEAD; its exact SHA and remote equality are recorded in the final delivery message and verifiable with `git log -2`/`git rev-parse HEAD origin/milestone-9-cancellation-recovery`. A report cannot contain its own commit hash.
- Delivery comparison: main-to-M9 ahead/behind is `0 2`; `main...origin/main` is `0 0`; local M9-to-origin M9 is `0 0` after the branch-only push. Both commits were reviewed; the tracked working tree is clean. No PR, merge or tag was created.
- The only original untracked file was the owner-supplied M9 authorization Markdown, carried onto this branch as authorized. All other changes listed below are M9 work.
- Protected SHA-256 values before work and after validation:
  - `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md`: `8bda49a0af4e612892bafdba48effb924573e02a6ccea33b3aa8bd5991d813ae`.
  - `.local-notes/reading-notes.md`: `4b2e5a53c77198e9278ebdd3cf40be93ec4468d5707435f4729d26b4dabbc882`.
- Ignored personal notes and build directory were preserved; build cleaning used CMake's `clean` target, never directory deletion.

## B. Implemented checkpoints

| Checkpoint | Implemented behavior |
|---|---|
| M9-A | Real CLI and Controller Cancel/Retry handling through M6 and M5; commit-captured response facts; exact pre-dispatch proof; bounded independent CANCEL request correlation; overflow-checked manual retry and retained history accounting. |
| M9-B | Worker validates current Task/Attempt/session, queues receipt ACK, and cancels only its owned RuntimeProcessId; explicit command grace; first-cause and frozen leader outcome preserved through real M4 cleanup and M8 finalization. |
| M9-C | Logical invalidation quarantines old input until LOST commit; Worker detects close/reset/rejection, terminates/reaps/drains all old work, clears session state, waits bounded exponential backoff, then registers with a fresh UUID. |
| M9-D | Actual SQLite gates/faults, crash/restart, binary logs, timeout/cancel races, partial sends, replacement/deadlines, process/fd/thread checks, and all accepted regressions. |

No frozen protocol, domain states, SQLite schema, placement algorithm, durability ordering or LogWriter barrier was changed.

## C. Files and dependency direction

Modified production/build files:

- `CMakeLists.txt`: M9 test/helper targets and labelled CTest cases; accepted targets/flags retained.
- `include/ts/controller/task_manager.h`, `src/controller/task_manager.cpp`: response fact snapshots and retry attempt-number overflow rejection before posting persistence.
- `src/controller/application.cpp`: client mutation handling, correlation, queued-RUN proof, control delivery, logical-invalidation quarantine.
- `include/ts/runtime/process_runtime.h`, `src/runtime/process_runtime.cpp`: parent-only optional cancellation grace, fixed-outcome observation, refusal to replace an already observed launch failure.
- `include/ts/worker/execution_manager.h`, `src/worker/execution_manager.cpp`: cancel validation/receipt, Attempt tombstones, cause mapping and cleanup-gated session reset.
- `src/worker/application.cpp`: cancel dispatch, cleanup/backoff/reconnect lifecycle and explicit transport-invariant failure handling.
- `src/apps/taskctl.cpp`: human Cancel/Retry responses and help; `src/apps/controller.cpp`, `src/apps/worker.cpp`: remove superseded M7 help limitations.

Modified test files: `tests/helpers/runtime_helper.cpp` (deterministic fail-until-file workload), `tests/integration/m7_loopback.py` (two legitimately superseded assertions, detailed below).

Added: `tests/component/m9_execution_test.cpp`, `tests/component/m9_state_test.cpp`, `tests/helpers/m9_fault_controller.cpp`, `tests/integration/m9_loopback.py`, the owner-supplied `docs/spec/Milestone_9_Authorization_Codex_Development_Prompt.md`, this report and `docs/M9_LOCAL_DEMO.md`. No removals or renames.

Dependency review: Controller composition remains above TaskManager/storage/net/protocol/logs; Worker composition remains above ExecutionManager/runtime/net/protocol. Runtime has no protocol, Controller, SQLite or log-file dependency. SQLite and LogWriter retain separate owners and queues. No dependency or production target was introduced beyond M9 composition.

Narrow lower-layer corrections: retry overflow is rejected before acceptance; M4 parent observation exposes already fixed results and supports the existing command's grace without changing child execution; invalidated Controller peers no longer close early merely because late input arrives before LOST commit.

## D. Client cancellation and retry

- PENDING cancellation commits CANCELLED first, creates no Attempt and sends no Worker command; timestamp is the actual nullable persisted value.
- ASSIGNED cancellation uses ProvenEmpty only when an adapter-owned current RUN effect has never entered transport, or M6 has retained the same explicit dispatch fact. Empty socket queues are never used as proof.
- Otherwise ACTIVE cancellation posts CancelIntent; accepted/ACTIVE and Worker CANCEL are emitted only from successful completion. Task remains ACTIVE and reservations remain held until real finalization commits.
- Repeated durable intents do not post again or send again. Requests received while an intent/terminal transaction is pending serialize through M6. CANCELLED returns already_cancelled; SUCCEEDED/FAILED returns CONFLICT; unknown UUID returns NOT_FOUND.
- Cancel/Retry response state, timestamp and next number are captured at emission, rather than read from a row that may have advanced before the effect drains. Client identity includes generation and request ID.
- Retry is explicit FAILED-to-PENDING only. It clears current intent and preserves every historical Attempt/result/path/log. New assignment uses a new UUID and exactly max-old-number + 1. UINT32_MAX history rejects before any Retry post. Concurrent retries cannot both succeed.
- Each admitted/deferred retry conservatively charges another 8 KiB against the existing retained metadata ceiling. Charges are lifetime-conservative, including requests later rejected or disconnected; there is no history eviction or storage quota subsystem.

## E. Controller command delivery

CANCEL uses the existing control output path and configured grace. A separate bounded map stores request ID, Task, Attempt, SessionKey and connection generation. RUN and CANCEL share monotonically allocated request IDs but never correlation entries. Delayed effects are revalidated immediately before enqueue. An ACK must match all current peer/request/Attempt identities; invalid fields, wrong UUID/request and duplicates invalidate the session.

ACK clears only receipt correlation. accepted=false/already_terminal=true cannot terminalize, release resources or suppress a later authoritative FINISH. Correlation remains available after terminalization for a legitimately late ACK, then is removed on receipt or session destruction. No new ACK lease/deadline was invented. Saturated correlation remains bounded; queued control effects wait for capacity without blocking the owner thread.

Failed or partially sent CANCEL is uncertain delivery and follows LOST. Tests capture a seven-byte partial command followed by close, not a fabricated complete send or CANCELLED result. Logical faults stop applying old input immediately, keep consuming bounded TCP input, wait for durable bulk LOST and only then intentionally close the old socket. Physical EOF/reset naturally precedes that logical cleanup.

## F. Worker termination, timeout and logs

ExecutionManager resolves current owned work by Attempt to RuntimeProcessId; tombstones retain Task identity without PIDs. Wrong Task/session fails before signalling; unknown Attempt receives an explicit negative receipt. Completed/frozen work ACKs already_terminal without rewriting the outcome. Duplicate cancel neither replaces the installed cause nor restarts grace.

M4 supplies negative-pgid SIGTERM, timerfd grace/SIGKILL, shared SIGCHLD signalfd and nonblocking waitpid/reap/drain. Timeout still begins only at exec confirmation. Cancellation installed before exec-failure observation remains CANCELLED; an already observed launch failure remains LAUNCH_FAILED. Timeout installed first remains TIMED_OUT; reaped natural exit remains EXITED/SIGNALED even through residual-group cleanup.

START remains ahead of buffered logs; FINISH retains its last-log dependency and exact binary counters. Cancellation/timeout do not bypass byte/slot credit. Zero-credit tests keep receipt/heartbeat processing live, then replenish and verify exact tails. Both stream file syncs and required directory syncs still precede the SQLite terminal transaction and resource release.

Child-after-fork audit: `child_exec`, `child_failure`, `child_close`, `retry_dup2`, prepared argv/envp/path ownership, setpgid/dup2/fchdir/sigprocmask/execve/write/close/_exit path are unchanged from accepted M4. No C++ container construction, PATH transformation, allocator, logging, mutex, implicit shell or blocking wait was added there.

## G. Session loss and reconnect

Controller uses existing M6 liveness, missing ACK/START and replacement invalidation; all pending old-session metadata resolves before one M5 bulk LOST transaction. Reservations, OFFLINE visibility, deliberate socket close and replacement ACK wait for completion. A disconnected pending replacement is never later activated; stale generation/session packets and Writer completions cannot revive it.

Worker loss handling disables new admissions/transmission, starts termination of all owned groups, keeps ticking reap and incomplete local pipe drain, and closes the old TCP connection. Only when all per-Attempt owners are gone does it reset parser, credit, heartbeat sequence and bounded tombstones, generate a fresh UUID, back off, connect and await the new REGISTER_ACK. There is no replay/adoption.

The single Worker-wide ProcessRuntime/shared SIGCHLD owner is deliberately retained across sessions, with zero old running processes or per-Attempt registrations. Recreating it inside the still-blocked application SIGINT/SIGTERM mask would capture the wrong child mask; retaining the shared owner preserves accepted nesting. Destruction still restores application mask before destroying the runtime. This is owner reuse, not reuse of session work.

Reconnect uses configured initial/max backoff with overflow-safe doubling and Linux clock-range checks. Backoff resets only after accepted registration. TCP failure/rejection retries after cleanup; protocol, runtime, epoll and transport buffer/callback invariant failures remain explicit nonzero failures. Shutdown suppresses reconnect.

## H. Recovery and failure classification

Startup lock, WAL/FULL schema setup and atomic recovery remain M5. Before listeners open, old nonterminal Attempts become LOST/Tasks FAILED; PENDING and committed terminal histories survive. No old session/reservation is restored, and orphan files never imply success.

- BUSY/FULL/IOERR and unknown COMMIT do not authorize corresponding CANCEL, RUN, accepted reply or scheduling. Unknown post-commit facts are resolved only by restart/read, never blind retry.
- Log failure before reliable FINISH retains LOST semantics. Failure after reliable FINISH produces LOG_PERSISTENCE_ERROR, actual observed outcome diagnostic and completeness=false if SQLite is healthy.
- Cancelled-workload tests inject open, both file-sync and every dirty-directory sync failure. None returns complete logs.
- Notification/completion and reservation exhaustion retain explicit fail-stop paths. Accepted M5/M8 notification tests remain in the full suite.
- Crash gates independently inspect SQLite before/after assignment, cancel intent, terminal commit and memory release. File/namespace crash gates remain covered by accepted M8 tests. SIGKILL is process crash testing, not hardware power-loss simulation.

## I. Executable acceptance matrix

All `M9Loopback.*` cases below are in `tests/integration/m9_loopback.py`. They run real Controller/Worker/taskctl executables with TCP and independent SQLite readers. Raw Worker/Controller peers deliberately inject protocol observations (including synthetic START PID) for wire/commit tests; they do not claim actual workload execution. Actual child execution is separately exercised by runtime/Worker/cluster cases.

| ID | Exact M9Loopback suffix | Evidence |
|---|---|---|
| 1 | pending_cancel_commit_gate_idempotence_and_missing_conflicts | held commit, two requests, no Attempt, completed/already/conflict/not-found |
| 2 | active_cancel_commit_gate_receipt_not_terminal_and_duplicate | zero precommit bytes/replies, exact CANCEL binding, one intent, ACK not terminal |
| 3 | cancel_unknown_failed_commit_never_escapes_on_wire | real BUSY/FULL/IOERR/unknown commit, rollback/second-reader visibility, fail-stop/recovery |
| 4 | no_dispatch_assigned_proven_empty_and_possible_bytes_cancel_intent | bounded RUN slot holds unsent effect; local complete empty vs possible-byte remote cancellation |
| 5 | retry_commit_preserves_history_logs_and_unique_attempt_number | held Retry, concurrent conflict, immutable full old row, distinct new UUID/paths/number |
| 6 | client_disconnect_and_reused_request_cannot_steal_cancel_retry_response | disconnected cancel and fresh request ID/generation |
| 7 | bad_cancel_ack_correlations_and_duplicates_invalidate_not_terminalize | request/Attempt/session/worker/field errors and duplicate ACK -> LOST |
| 8 | real_cancel_timeout_and_noncooperative_group_cleanup | real cooperative/ignored TERM, KILL, timeout, exact logs, no child/fd/thread growth |
| 9 | cancel_during_held_final_barrier_keeps_natural_outcome_and_reservation | finalization result frozen despite accepted cancel; real held file barrier |
| 10 | controller_crash_running_cleanup_before_fresh_registration_and_recovery | real running child reaped before fresh registration; PENDING/terminal preservation |
| 11 | deadlines_and_lost_commit_hold_reservations_and_replacement_ack | held bulk LOST, replacement disconnect, late heartbeat, ACK/START/heartbeat deadlines |
| 12 | zero_credit_cancel_duplicate_timeout_first_and_natural_first_wire | all three cause races, zero credit control liveness, exact binary tails, late already-terminal receipt |
| 13 | running_disconnect_churn_cleans_groups_before_register_and_bounds_fds | five real close/reset cycles, unique UUIDs, group absence before REGISTER, bounded fd/thread/children |
| 14 | retry_can_succeed_on_another_worker_preserving_exact_old_log_history | failed Attempt 1 then success on Worker b, exact old logs and restart history |
| 15 | disconnected_retry_and_failed_unknown_retry_commit_never_auto_schedule | disconnected response, retry rollback/unknown commit, immutable history and durable predecessor |
| 16 | crash_before_and_after_cancel_intent_commit_recovers_lost_never_replays | pre/post-commit process crash, no escaped command/response, LOST restart |
| 17 | killed_worker_lost_has_no_automatic_retry_and_new_session_is_empty | SIGKILL Worker -> LOST, reservation release, fresh empty session; orphan explicitly cleaned by harness |
| 18 | real_multiworker_projected_choice_and_pending_registration_trigger | actual persisted placement, projected utilization and lexical tie, pending-to-assigned trigger |
| 19 | launch_setup_cancel_race_and_stale_worker_command_are_fenced | real setup race and fatal wrong-Task cancel with complete local cleanup |
| 20 | partial_cancel_send_is_lost_not_cancelled_and_late_valid_ack_is_receipt | real seven-byte short send/EPIPE; legitimate ACK after terminal is accepted only as receipt |
| 21 | stalled_log_path_cancel_control_two_workers_and_exact_cancel_tail | Writer stalls, independent metadata cancellation, two Workers, exact Z prefix and binary other stream |
| 22 | protocol_invalidation_keeps_socket_until_lost_commit_and_fences_late_packets | held LOST, no early deliberate close, no stale result/heartbeat revival |
| 23 | cancelled_log_namespace_and_file_sync_failures_never_claim_complete | open, stdout/stderr sync and task/tasks/root directory sync failures after Cancel |
| 24 | idle_reconnect_registration_rejection_backoff_resets_heartbeat_and_credit | repeated rejection/backoff, fresh UUIDs, sequence 1 and no stale running work |
| 25 | assignment_before_after_commit_crash_has_no_run_and_exact_recovery | actual pre/post-assignment commit crash, no RUN, exact PENDING vs LOST recovery |
| 26 | cancel_terminal_before_after_commit_crash_does_not_infer_from_synced_files | synced logs, pre/post-terminal COMMIT crash, reservation held until completion, LOST vs CANCELLED recovery |

`tests/component/m9_execution_test.cpp` adds these actual Linux-runtime tests through transport value ports (not fake durability):

- `M9Execution.CancelBeforeExecConfirmationWinsLaunchFailureWithoutStarted`.
- `M9Execution.CompletedLaunchFailureAndNaturalResultCannotBeOverwritten`.
- `M9Execution.WrongTaskSessionAndUnknownAttemptCannotSignalOwnedProcess`.
- `M9Execution.ResetRequiresCompleteCleanupAndDiscardsAllSessionTombstones`.
- `M9Execution.DuplicateCancelDoesNotReplaceCommandGraceOrRearmTermination`.

`tests/component/m9_state_test.cpp` adds pure production-core decision tests with fake ports, without claiming SQLite persistence:

- `M9State.RetryRejectsExhaustedAttemptNumberBeforePersistenceOrSuccess`.
- `M9State.RetryResponseFactsSurviveLaterInMemoryMutation`.

### Complete frozen cluster/fault matrix

The following accepted tests also run in the full final suite, not merely source assertions. Names with `M7Loopback`/`M8Loopback` live in the corresponding `tests/integration/m7_loopback.py` / `m8_loopback.py`.

| TEST_STRATEGY cluster scenario | Executable evidence |
|---|---|
| 1 happy path/restart | M9 10/14; `M8Loopback.empty_real_barrier_terminal_release_and_restart` |
| 2 multiple Workers/exact choice | M9 18; retained scheduler rational-comparison/property tests |
| 3 pending/no feasible Worker | M9 1/10/18/25; `M7Loopback.real_vertical_slice_two_workers_and_crash_recovery` |
| 4 >two task pages | `M7Loopback.pagination_history_restore_and_truthful_unsupported` (retained name) |
| 5 finite running log snapshot | `M8Loopback.active_snapshot_is_fixed_while_later_append_completes` |
| 6 terminal/log barrier | M9 9/26; `M8Loopback.every_file_directory_and_sql_gate_keeps_terminal_and_reservation_hidden` |
| 7–10 pending/active/race/timeout | M9 1/2/4/8/9/12/19/26 and M9Execution tests |
| 11 manual retry/history | M9 5/14/15, both M9State tests |
| 12 client disconnect/unknown response | M9 6/15; `M7Loopback.durable_submit_gate_and_disconnected_client` |
| 13–17 idle/running disconnect, replacement, heartbeat, restart | M9 10/11/13/16/17/22/24/25/26; retained WorkerRegistry/controller-core tests |
| 18 backpressure isolation | M9 12/21; `M8Loopback.stalled_writer_backpressures_multiple_workers_without_hiding_heartbeats` |
| 19 FINISH short-write barrier | `M8Loopback.partial_tcp_sends_keep_both_streams_before_finish`, `M7Loopback.worker_wire_exec_failure_credit_and_finish_barrier`, M3 output dependency tests |

| TEST_STRATEGY fault | Executable evidence |
|---|---|
| Worker SIGKILL | M9 17; orphan cleanup is harness-owned, not falsely attributed to killed Worker |
| close/reset | M9 10/13/20/22 |
| ACK/START missing | M9 11; `M7Loopback.negative_missing_ack_and_session_replacement` |
| externally signalled child | `M8Loopback.nonzero_and_signal_results_are_committed_only_with_complete_logs` |
| malformed client/worker/log | M9 7/19/22; `M7Loopback.fragmentation_roles_request_correlation_and_fd_churn`; `M8Loopback.finish_count_mismatch_and_late_log_are_protocol_faults` |
| SQLite statement/commit/rollback/unknown | M9 3/15/16/25/26; `AllNineTransactions/TransactionFault.SqlFailureRollsBackWholeOperationAndNeverEnablesEffect/*`; `StoreTest.RollbackFailureRetiresConnectionAndNoCommandIsReportedCommitted` |
| log append before/after FINISH | `M8Loopback.write_error_before_finish_is_lost_not_a_fabricated_result`; `M8Loopback.delayed_write_failure_after_finish_retains_outcome_and_never_syncs_success` |
| both file syncs/all dirty-directory syncs | M9 23; `LogWriter.EachFileAndDirectorySyncFailureNeverProducesSuccessfulEvidence` |
| namespace/file-sync/terminal crash boundaries | M9 25/26; `M8Loopback.crash_at_namespace_data_and_metadata_boundaries_never_infers_success` |
| output stalls/byte and slot exhaustion | M9 12/21; `M8Loopback.worker_pauses_independently_for_missing_bytes_or_slots_and_resumes_exactly`; retained LogCoordinator/credit conservation tests |
| notify/completion failures | `StorageChannel.NotificationFailureIsExplicitAndNeverDropsCommittedResult`; `LogWriter.NotificationFailureRetainsTheAlreadyPublishedCompletionAndStopsAdmission`; `M8Loopback.completion_notification_failure_fail_stops_and_restart_is_conservative` |
| fd/zombie/group cleanup, escaped descendants | M9 8/10/13/17/24; `ProcessRuntime.CancellationSignalsAndCleansEntireSamePgidTree`, `NaturalLeaderOutcomeSurvivesResidualGroupCleanupAndTailDrain`, `EscapedSetsidDescendantCanDelayEofUntilFixtureIsCleanedExternally`, `RepeatedRapidConcurrentExitLeavesNoFdsRegistrationsOrZombies` |

### Mandatory adversarial source review (authorization §19)

| Question | Result and evidence |
|---|---|
| 1 precommit CANCEL/reply? | No; completion-generated facts/effects, M9 1–4/16. |
| 2 early CANCELLED? | No; accepted state captured ACTIVE; M9 2/9/26. |
| 3 duplicate/fd-reused mutation? | No stale response; generation/request fences and serialized transitions, M9 1/2/5/6/15. |
| 4 receipt mistaken for FINISH? | No; distinct map/event, M9 2/7/20. |
| 5 late cause overwrite? | No; RuntimeProcessId owner, fixed outcome and first cause, M9 8/9/12 plus execution tests. |
| 6 stale PID signalling? | No Controller kill path; Worker uses owned RuntimeProcessId/pgid, M9 19 and execution identity tests. |
| 7 missing ACK/START mistaken for timeout/rejection? | No; LOST, M9 11 plus retained negative-ACK tests. |
| 8 replacement/release before LOST? | No; M9 11/22, core pending-mutation tests. |
| 9 reconnect before cleanup? | No; reset gate checks empty owners/runtime, M9 10/13/24. |
| 10 old credit/callback/Writer completion reused? | No; old connection/parser/session credit reset and completion generation fences, M9 11/13/22/24; retained M8 replacement/barrier test. |
| 11 restart orphan-file success? | No; M9 10/16/25/26 and retained namespace crash cases. |
| 12 retry reuses history/automatic retry? | No; M9 5/14/15/17, overflow test. |
| 13 log fault complete=true? | No; M9 23 and retained M8 append/sync faults. |
| 14 unknown commit authorizes effect? | No; M9 3/15; M5 fault matrix. |
| 15 saturation/notify drops or spin? | Bounded owners/credit/effect maps; explicit owner failure, M9 4/12/21 and retained persistence/Writer notify tests. |
| 16 silent group/zombie completion? | No; unchanged M4 reap/group/EOF gate; M9 8/10/13 and retained tree/escape tests. |
| 17 ownership/dependencies/C++17 warnings? | Reviewed full diff; strict clean GCC build required below; no new runtime child functions or module back edges. |
| 18 regressions hidden? | All 430 accepted entries/labels retained; only two superseded M7 assertions changed with stronger domain checks. |

## J. Formal WSL validation

Environment: Ubuntu 22.04.3 WSL, GCC 11.4.0, CMake 3.22.1, SQLite 3.37.2. Durable logs/databases use Linux-local ext4 `/tmp`; source is on `/mnt/e`. Flags remain `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`, C++17.

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

Clean configure/build and full CTest exited 0. Full suite: **463/463 PASS**, **79.45 seconds**, comprising all **430** accepted regressions plus **33** new M9 entries (7 component/core tests and 26 real-TCP integration cases). Inventory review confirmed no disabled entries and exact registration of every M9 case. Repetition exited 0: **33 tests × 20 = 660 successful executions**, **0 failures**, **566.13 seconds**. An independent parse checked every test ID appeared exactly twenty times; `m9-repeat-review.json` records the result. After repetition, `/proc/*/exe` inspection found no residual executable from the build tree; protected-note hashes still matched. Kernel: `6.18.54.1-microsoft-standard-WSL2`, x86_64.

Logs under the build directory: `m9-final-configure.log`, `m9-final-clean.log`, `m9-final-build.log`, `m9-final-ctest.log`, `m9-final-repeat.log`, `m9-final-inventory.json`, `m9-inventory-review.json`, `m9-demo.log`. The independent two-Worker CLI demo exited 0 and exercised human accepted/ACTIVE output, JSON responses, pending/active cancellation, timeout, failed-then-successful retry/history logs, Worker SIGKILL with explicit harness orphan cleanup, Controller SIGKILL and fresh reconnect. `/tmp` was confirmed as `/dev/sdd` ext4 with `findmnt`; `statfs`'s ext2/ext3 family name alone is not used to infer the mount type. Earlier evidence: `m9-baseline-ctest.log`, `m9-dev-full.log`, `m9-resume-tests.log`, `m9-crash-tests.log` (development failures retained rather than represented as final PASS).

The baseline passed 430/430 before changes. Development corrections included an invalid test expectation about internal diagnostic JSON (its outcome uses the accepted internal numeric enum), a test reader initially opening the old data directory, and crash subcases initially sharing a recovered pending Task. Those fixtures were corrected, without changing production semantics. Resumption confirmed the quota-interrupted command had not executed and reran it. The owner-supplied authorization header's trailing Markdown hard-break spaces were replaced with blank paragraph separators to satisfy `git diff --check`; its authorization content is unchanged.

Two accepted M7 assertions were legitimately superseded, while retaining their exact CTest names/labels: Cancel/Retry no longer return UNSUPPORTED; and `worker_id="bad worker"` is valid frozen ASCII, so its old exit was connection refusal rather than invalid configuration. The invalid-config test now uses zero CPU (truly invalid); retry checks next number 206 and old history retention. No disabled, removed, skipped or relabelled regression was used.

No ASan/UBSan/TSan run or hardware power-loss/remount validation is claimed. Process crash and Linux filesystem sync-order evidence is the supported acceptance scope.

## K. Deviations, limits and M10 boundary

No frozen semantic deviation or unresolved architectural contradiction was found. Parent API additions and logical-invalidation cleanup correction above implement existing contracts; no wire/schema/state change.

Limits remain: trusted private/loopback network and trusted local filesystem; no auth/TLS, cgroup/resource enforcement, sandbox, adoption, follow/retention, exactly-once or distributed ACID. LOST cannot prove remote death. Manual retry after loss can overlap remote work on an unreachable host. SIGKILL Worker cannot run cleanup; its orphan in the fault test is explicitly cleaned by the test harness. Escaped setsid descendants and uninterruptible processes remain outside finite termination guarantees. Existing bounded retained metadata/session/tombstone admission can reject more work; there is no unbounded history cache. Unacknowledged CANCEL correlation remains bounded until receipt/session loss, without inventing a new lease.

Comprehensive sanitizer/CI packaging, systemd/deployment, benchmarking and any M10 work have not started.

## L. Gate

**PASS — M9 implementation acceptance.** All mandatory M9-A–D criteria and the applicable frozen cluster/fault matrix have executable evidence above; clean GCC build, 463/463 full tests and 660/660 repeated M9 executions passed. No unresolved core semantic inconsistency remains. Independent source review is still required before merge. M10 has not started.

Delivery review commands:

```text
git diff --check main...HEAD
git diff --name-status main...HEAD
git log --oneline main..HEAD
git rev-list --left-right --count main...HEAD
git rev-list --left-right --count main...origin/main
git push -u origin milestone-9-cancellation-recovery
git rev-parse HEAD origin/milestone-9-cancellation-recovery main origin/main
git status --short --branch
```

The final delivery verifies local/remote equality and unchanged main refs after push. Stop for independent review; do not merge or begin M10.
