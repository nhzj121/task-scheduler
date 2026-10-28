# Milestone 8 acceptance report

## A. Git and baseline

Work is confined to `milestone-8-durable-task-logs`, created from verified
`main == origin/main == 04e5790075d6dfdee9096f19345856935e7c774f` after fetch.
Accepted M7 `e61230470ac026c91f18b61a005bfaa3ab9f54ed` is an ancestor of both.
The only pre-existing untracked change was the owner-provided M8 authorization;
it was carried and committed under its explicit exception. Ignored notes were
preserved. No reset, main commit, merge, branch deletion or force push was used.

Reviewed implementation commit: `b33fb406979541a2a51616cc2376d17c6717d472`.
Essential-priority follow-up: `d7e79477836bcf01335a5841c29a78a9252dc58d`.
Final reviewed production/test revision:
`2580279bc500f80b392d09018f0a9c3dc74db1c7` (read-error contract correction).
This separate acceptance artifact accompanies the final branch HEAD; the exact
final report-commit SHA and remote equality are recorded in the delivery report
and can be independently resolved with `git rev-parse HEAD origin/milestone-8-durable-task-logs`.
Final working-tree/remote verification is performed after committing this report;
the final delivery records that result. Only this M8 branch is pushed.

## B. Implemented scope

M8-A: dedicated task-log Writer thread; separate bounded write, essential and
snapshot admissions; owned correlated completions and distinct eventfd; safe
filesystem namespace, append/pread, exact visibility and real final barriers.

M8-B: one production LogCreditLedger and LogCoordinator; initial reservations,
byte/slot ownership transfers, successful-completion replenishment, generation
fences and Worker window updates on the existing TCP connection.

M8-C: immediate FINISH/accepted reconciliation; write completion, file and dirty
directory synchronization, actual M5 evidence and M6 terminal transaction/release;
known-outcome log errors and conservative earlier session loss.

M8-D: committed Attempt selection, Writer snapshot reads, finite paging and binary
CLI stdout/stderr/both; real subprocess, fault, crash-order and saturation tests.
No M9 retry/cancel CLI orchestration, automatic reconnect or M10 packaging was added.

## C. Files and dependencies

Added production files:

- `include/ts/logs/writer.h`, `src/logs/writer.cpp`.
- `include/ts/controller/log_credit_ledger.h`, `src/controller/log_credit_ledger.cpp`.
- `include/ts/controller/log_coordinator.h`, `src/controller/log_coordinator.cpp`.

Modified accepted production files:

- `include/ts/controller/application.h`, `src/controller/application.cpp`: Writer
  composition, separate wakeup, backed registration, append/FINISH adapters,
  terminal notification, GET_LOGS and shutdown ownership.
- `include/ts/worker/execution_manager.h`, `src/worker/execution_manager.cpp`:
  checked incremental window updates; existing pre-read allowance remains authoritative.
- `include/ts/worker/application.h`, `src/worker/application.cpp`: typed grant
  dispatch; optional existing M3 SocketIo seam for deterministic short-send tests.
  Normal worker binary supplies no fault options.
- `include/ts/client/client.h`, `src/client/client.cpp`, `src/apps/taskctl.cpp`:
  streamed snapshot paging and binary-safe CLI.

Moved the two M7 provisional-staging files from production to
`tests/legacy/include/ts/controller/provisional_staging.h` and
`tests/legacy/provisional_staging.cpp`. Only historical M7 unit tests link them;
production has one ledger and no volatile body staging path. Old test names and
labels are preserved. No replacement production facade or duplicate ledger remains.

Added tests/helpers:

- `tests/component/log_writer_test.cpp` (17 tests).
- `tests/component/log_coordinator_test.cpp` (3 tests).
- `tests/unit/log_credit_test.cpp` (3 tests).
- `tests/integration/m8_loopback.py` (18 externally launched cases).
- `tests/helpers/log_workload.cpp`, `tests/helpers/m8_fault_controller.cpp`,
  `tests/helpers/m8_partial_worker.cpp`.

Modified `tests/integration/m7_loopback.py` only where M8 supersedes the historical
hold/unsupported/two-thread expectations. The crash-recovery case now uses real
sleeping workloads so it still proves ACTIVE -> LOST; coalesced FINISH and launch
failure now assert actual durable terminal results. Other M1–M7 tests are unchanged.

Other changes: `CMakeLists.txt` (ts_logs, test targets/registration), `.gitignore`
(generated Python cache only), `docs/M7_LOCAL_DEMO.md` (historical-guide pointer),
`docs/M8_LOCAL_DEMO.md`, this report, and the owner-provided M8 authorization file.
No other frozen architecture/specification file changed.

`ts_logs -> ts_common/ts_domain/ts_io/Threads`; it has no Controller, SQLite,
protocol, transport or process-runtime dependency. `ts_controller` composes it
with the existing M5/M6/M3 layers. Worker still uses M3/M4; client uses M3.
No new third-party dependency: C++17/Linux, Threads, nlohmann_json, SQLite,
spdlog/fmt and test-only GoogleTest remain the accepted dependencies. Python's
standard library is only an external test harness.

## D. Writer, paths and namespace durability

`src/logs/writer.cpp:23` verifies canonical UUIDs, positive Attempt number and
exact immutable assigned paths:
`tasks/<task-uuid>/attempt-<number>.stdout.log` / `.stderr.log`. No extra Attempt
directory or reassignment is introduced. Root is the existing canonical Controller
data root. Writer lazily creates tasks/ and task directories with restrictive
permissions; dirfd-relative no-follow opens, owned directories, regular single-link
files and non-writable-by-other checks prevent path escape/nonregular targets.
Existing files open without truncation; append requires exact known length.

All task-log mkdir/open/fstat/lseek/write/pread/sync/close operations run on Writer.
The EventLoop passes owned values and opaque IDs, never file fds, Peer pointers,
borrowed strings or owner callbacks across the thread boundary. Root/tasks handles
are retained by Writer; per-command task/file handles close before completion.

Writer advances visibility only after the entire chunk and owned-handle close
succeed. Short writes/EINTR preserve exact progress; a partially failed chunk
poisons that owner and is never re-appended or promoted to visible data. Safe
idempotent/offset-stable operations retry EINTR; Linux close is never retried.
Unexpected internal/allocation failure is explicit whole-Controller fail-stop,
not a per-command success or silent resumption.

Barrier at `src/logs/writer.cpp:244`: wait behind accepted writes, verify both
written counters, fdatasync stdout, fdatasync stderr, fsync dirty task directory,
then dirty tasks/, then dirty data root. Zero-output execution creates both files
and performs both file syncs. Namespace dirty flags are set immediately after
successful creation, before test after-syscall hooks. Sync obligations clear only
after actual successful sync. No per-chunk directory sync. On first encounter
after restart preexisting unsynchronized namespace is conservatively dirty;
subsequent same-task Attempt creation does not resync already clean ancestors.

Any open/write/close/file-sync/directory-sync failure forbids successful evidence.
Task-log root provisioning remains the existing trusted-local-directory startup
scope; no new storage subsystem or filesystem/device power-loss promise is made.

## E. Credit, queues and bounded ownership

For raw bytes and write slots separately:
`free + granted + queued_or_inflight = configured_grantable_capacity`.
Defaults: 64 MiB raw write ownership, 4096 total command slots with 128 essential
slots excluded from grants; initial per-session window 1 MiB/64 chunks.
Session ACK reserves both before transmission. Ledger and session tombstones are
bounded by existing registration limits. Each append requires 1..65536 bytes and
one slot; unused bytes cannot substitute for a slot.

`src/controller/log_credit_ledger.cpp` implements reserve -> grant -> accept ->
complete/reclaim. Reclaim releases only unused grants; queued commands retain
operation/binding/byte ownership. Duplicate or generation-mismatched completion
cannot free capacity or replenish a new lease. Raw capacity arithmetic is checked
by partition bounds including UINT64_MAX tests. Real application reserve, Writer
bytes and Writer write-slot limits are derived from the same QueueConfig.

At `src/controller/log_coordinator.cpp:118`, a valid credited transfer must admit
to Writer. Failure returns `InvariantFailure`, which Controller makes fatal
(`application.cpp:378`), rather than suspending socket input or dropping output.
Successful matching append completion frees its exact bytes/slot, advances written
position and records a replenishment debt. Round-robin current-session pump grants
only that debt, capped to the original window and actual free capacity. Grants are
reserved before M3 send; send failure closes/reclaims the session. Receipt and file
sync do not replenish. A session receiving an error is poisoned and receives no
further grants.

Writer usage includes queued, executing and completed-undrained commands. Every
admission reserves a preallocated completion slot. Defaults additionally bound
reads to four slots/eight MiB charged raw replies, and record/namespace caches to
4096 owners. Snapshot reads never consume essential slots. Barrier selection has
priority once its prior writes finish; retirement waits for accepted snapshots
before deleting visibility state (`writer.cpp:253`). Selection skips blocked
retirements to run unrelated ready barriers. If a sole essential slot is held by
retirement waiting for a snapshot, that snapshot dependency runs before unrelated
writes, allowing the reserved slot to be released even under continuous output.
Pending Coordinator maps are
bounded by those live admissions, with 4096 Attempt entries.

Existing M3 input/output bounds, M7 request/query/effect/metadata limits and Worker
per-tick limits remain in force. Encoded base64 is independently bounded by M3's
8 MiB/128-slot log output and 8 MiB/128-slot control output per connection. Snapshot
encoding incurs bounded per-page temporary copies; capacity is not claimed to
equal exact allocator/RSS usage. There is no unbounded raw backlog or body cache.

Seeds 8, 813 and 20261028 each exercise 10,000 deterministic ledger operations,
asserting both partition identities after every step, with multiple sessions,
reclaim/re-reserve and late queued completions.

## F. Worker and transport

`ExecutionManager::allowance` (`src/worker/execution_manager.cpp:47`) remains the
M4 before-read bound: bytes, slots, encoded queue room and per-tick work budget.
`output` decrements at generation/enqueue. No post-read credit request exists.
`grant` at line 144 rejects wrong activation/zero/overflow and the normal paced
tick re-enables eligible pipe interest. Both credit dimensions independently pause
reads; timers, actual heartbeats, TCP input and child reaping continue. Extremely
high output may block the workload's write; this is expected flow control.

Window updates use existing typed message `0x0208`, request_id 0. No IDs, 26
schemas, framing or SQL schema changed. M3 remains the sole output ordering
implementation: cross-stream per-Attempt ordinal, pinned partial frame and FINISH
dependency (`execution_manager.cpp:108`). The short-send helper uses actual Worker
Application/M3 send with at most 97 bytes per syscall and EAGAIN every seventh
call. Two concurrent real binary-output Attempts complete exactly without a
FINISH overtaking their stdout/stderr. Existing M3 priority/dependency tests remain.
M4 child-after-fork code and process-group ownership are unchanged.

## G. FINISH, barrier and metadata commit

Coordinator retains accepted positions, Writer-reported written positions and
one immutable observed final result separately. FINISH immediately compares all
four accepted bytes/sequences; mismatch or a later chunk invalidates the session.
The first M6 observation has Incomplete evidence and cannot terminalize. After all
writes and matching real barrier completion, pump supplies Attempt-bound
FinalBarrier evidence to that same observed result. M5 validates immutable paths,
versions, session and evidence in its terminal transaction. Unchanged
`TaskManager::apply` (`src/controller/task_manager.cpp:446`) releases reservations
only on committed completion, before external terminal visibility. No cross-medium
ACID claim is made.

A write/barrier error after reliable FINISH produces LOG_PERSISTENCE_ERROR with
the original observed child facts and completeness false if SQLite works. The
poisoned session is invalidated after that terminal commit. A write error before
reliable FINISH follows session loss/LOST; no leader outcome is invented. Old
session writes/barriers may still physically finish but their fenced completions
cannot promote LOST to SUCCEEDED or grant a replacement's credits.

File errors are typed results; notification/internal channel errors fail-stop the
application with nonzero exit. Accepted Writer work is drained/joined outside
dispatch; fatal shutdown explicitly aborts remaining in-memory business
correlations and exposes no successful durability. Nonterminal durable rows are
recovered conservatively by M5 on restart. No live writer is detached. Blocked
kernel disk I/O can still delay join; no arbitrary disk-operation timeout is claimed.

## H. GET_LOGS and taskctl

Real client requests page committed Task/Attempt history and verify versions before
selecting an explicit Attempt number or latest. Coordinator sends Writer an owned
read with immutable path binding. Active reads use actual Writer visibility;
complete historical reads use committed final bounds. Incomplete historical rows
without surviving visible knowledge return a conservative zero-length incomplete
snapshot, never infer durable bytes from orphan file length.

First page fixes end; subsequent pages enforce end/offset/Attempt identity. Retained
reads cannot lose visibility to owner retirement. `Client::logs`
(`src/client/client.cpp:96`) pages without whole-log accumulation; raw CLI uses
`cout.write`, while --json emits typed base64 NDJSON. --both takes separate stdout
and stderr snapshots, concatenated in that order; no reconstructed interleaving.

Default 262144 raw bytes, capped 3144960; M3 still rejects full encoded payloads
above 4194304 bytes. Valid large limit clamps; zero/negative/type/overflow errors
retain existing schema rules. Offset arithmetic is bounded by known signed file
sizes. ProvenEmpty is recognized only from frozen persisted rejection/no-dispatch
facts with zero counts/sequences; ordinary executed empty work requires actual
files/barrier. Missing completed files/read errors return INTERNAL and do not
rewrite Task history. Only invalid snapshot bounds map to INVALID_ARGUMENT;
filesystem safety/namespace failures map to INTERNAL as required by the frozen
read-error contract. Replies remain request-ID and client-generation fenced.
No follow mode, retention, uploads or extra log socket was added.

## I. Acceptance matrix and reviewer questions

All names below are exact CTest names; repeated parameter loops are subcases, not
inflated independent test counts. Writer/coordinator/ledger cases are native
unit/components; every M8Loopback case launches real external binaries. Raw peers
are adversarial protocol instrumentation, never fake production decision logic.

| Requirement / reviewer questions | Source evidence | Executable evidence |
|---|---|---|
| Sole file owner, three threads (1) | writer.cpp:271; application.cpp:149 | `LogWriter.FilesystemHooksRunOnOneDedicatedThreadAndShutdownHasNoFdGrowth`; `M8Loopback.large_binary_multiple_replenishments_and_paged_cli_are_exact` |
| Exact independent partitions, reclaim, stale/duplicate (2) | log_credit_ledger.cpp; log_coordinator.cpp:193 | `LogCreditLedger.ReclaimNeverReleasesInflightOwnershipAndDuplicateCompletionCannotMintCredit`; `LogCreditLedger.IndependentDimensionsAndUint64CapacityCannotOverflow`; `LogCreditLedger.SeededRandomTransfersConserveBothPartitionsAtEveryStep` |
| Credited admission/full slot boundary (3) | log_coordinator.cpp:118; application.cpp:378 | `LogCoordinatorTest.CreditedEnqueueFailureIsAnExplicitInvariantFailureNotCongestion`; `LogCoordinatorTest.OneByteChunksExhaustSlotsAndAllCreditedChunksAdmitAtCapacity` |
| Before-read bounds, independent exhaustion/resume (4) | execution_manager.cpp:47,144 | `M8Loopback.worker_pauses_independently_for_missing_bytes_or_slots_and_resumes_exactly`; `M8Loopback.stalled_writer_backpressures_multiple_workers_without_hiding_heartbeats` |
| Accepted/written/final identities (5) | log_coordinator.cpp:136,164,227 | `LogCoordinatorTest.FinishMustMatchAcceptedCountsImmediatelyAndNoLateLogIsAccepted`; `M8Loopback.completion_gate_prevents_replenishment_until_success_is_delivered` |
| FINISH partial-write barrier/late faults (6) | execution_manager.cpp:108; unchanged M3 output_queue | `M8Loopback.partial_tcp_sends_keep_both_streams_before_finish`; `M8Loopback.finish_count_mismatch_and_late_log_are_protocol_faults` |
| Exact dirty sync chain/empty files (7) | writer.cpp:102,244 | `LogWriter.EmptyBarrierSyncsBothFilesThenExactlyTheDirtyNamespaceChain`; `LogWriter.RepeatedAttemptOnlySyncsTheDirectoryWhoseFilesWereCreated`; `LogWriter.SafeSyncAndReadEintrRetryWithoutDuplicatingData` |
| Terminal visibility/release waits for disk+SQL (8) | log_coordinator.cpp:227; unchanged M5/M6 terminal CAS | `M8Loopback.every_file_directory_and_sql_gate_keeps_terminal_and_reservation_hidden`; `M8Loopback.empty_real_barrier_terminal_release_and_restart` |
| Typed log failure/observed facts/incomplete (9) | LogCoordinator::drain / pump in log_coordinator.cpp | `M8Loopback.sync_and_open_failures_preserve_observed_outcome_as_log_persistence_error`; `M8Loopback.delayed_write_failure_after_finish_retains_outcome_and_never_syncs_success`; `M8Loopback.write_error_before_finish_is_lost_not_a_fabricated_result` |
| Snapshot/paging/history/proof/frame bound (10) | writer.cpp:187; log_coordinator.cpp:149; client.cpp:96 | `M8Loopback.active_snapshot_is_fixed_while_later_append_completes`; `M8Loopback.default_clamped_paging_and_binary_payload_stay_below_frame_limit`; `M8Loopback.rejected_run_and_lost_and_missing_history_never_fabricate_logs`; `LogWriter.SnapshotEndDoesNotGrowAndReadErrorsAreExplicit`; `LogWriter.RetirementCannotEraseVisiblePositionsBeforeAnAcceptedSnapshot` |
| Immutable namespace/session fences/DB locks (11) | writer.cpp:23; unchanged M5 path CAS and SQLite lock | `LogWriter.TraversalSymlinksAndNonregularFilesCannotEscapeTheTrustedRoot`; `LogWriter.UnsafeRootAndSymlinkDirectoryAreExplicitFailures`; `M8Loopback.disconnect_and_replacement_during_write_or_barrier_fence_late_success`; retained M5/M7 lock tests |
| Bounded command/completion/barrier/lifetimes (12) | writer.cpp:260 and Writer::try_post/drain; application shutdown | `LogWriter.WriteSlotsAndBytesRemainOwnedUntilDrainAndBarrierReserveStaysAvailable`; `LogWriter.EssentialBarrierRunsBeforeSnapshotsAfterItsPriorWriteAndStopDrainsOwners`; `LogWriter.BlockedRetirementCannotHideAnotherAttemptsReadyFinalBarrier`; `LogWriter.SnapshotDependencyReleasesASingleEssentialSlotBeforeUnrelatedWrites`; `LogWriter.NotificationFailureRetainsTheAlreadyPublishedCompletionAndStopsAdmission`; `M8Loopback.completion_notification_failure_fail_stops_and_restart_is_conservative` |
| Every syscall/fault/crash boundary, honest limits (13) | writer.cpp; m8_fault_controller.cpp | `LogWriter.MkdirOpenWriteAndCloseFaultsAreTypedAndDoNotSucceed`; `LogWriter.EachFileAndDirectorySyncFailureNeverProducesSuccessfulEvidence`; `LogWriter.PartialWriteFailureNeverPublishesAnIncompleteChunkOrOverwritesItsPrefix`; `M8Loopback.crash_at_namespace_data_and_metadata_boundaries_never_infers_success` |
| Binary, retained regression scope/repeat (14) | unchanged lower layers, CMake M8 cases | `LogWriter.BinaryShortWritesAndEintrHaveExactIndependentVisibleCounters`; `M8Loopback.large_binary_multiple_replenishments_and_paged_cli_are_exact`; `M8Loopback.nonzero_and_signal_results_are_committed_only_with_complete_logs`; full/repeat results below |

The 13 crash subcases are after file creation; before/after stdout fdatasync;
before/after stderr fdatasync; before/after task, tasks and root fsync; and
before/after terminal SQLite COMMIT. Five held sync boundaries plus terminal
COMMIT independently prove external SQL remains nonterminal and reservations held.
The held-write/completion scenarios additionally demonstrate control liveness and
no premature replenishment; tiny-byte/tiny-slot scenarios directly observe Worker
pipe progress rather than only a model ledger.

## J. Formal WSL verification

Ubuntu 22.04.3 WSL, GCC 11.4, CMake 3.22.1, C++17. All targets retain
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`.
Data roots are isolated Linux `/tmp` directories, not `/mnt/e`.
`findmnt -n -o FSTYPE -T /tmp` confirms `ext4`; `stat -f -c %T /tmp` reports the
compatible `ext2/ext3` filesystem magic under WSL. The Windows-mounted source is
only code/build input.

Exact clean acceptance commands:

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4 \
  > /home/nhzj121/build/task_scheduler/m8-clean-build.log 2>&1
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure \
  > /home/nhzj121/build/task_scheduler/m8-final-ctest.log 2>&1
ctest --test-dir /home/nhzj121/build/task_scheduler -L m8 \
  --output-on-failure --repeat until-fail:20 \
  > /home/nhzj121/build/task_scheduler/m8-repeat20.log 2>&1
sha256sum /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
```

Windows Git review commands (preserve repository autocrlf behavior):

```text
git diff --check
git diff --check main...HEAD
git diff --name-status main...HEAD
git log --oneline main..HEAD
git status --short
```

Final clean configure/build: PASS. Full CTest: **430/430 PASS, 39.55 seconds**,
including **389 retained M1–M7 and 41 M8**. Baseline before M8 edits was 389/389
PASS. Complete final M8 20-repeat result: **41/41 PASS at 20 consecutive runs each,
820 executions, 362.92 seconds**, with no flaky failure, retry or timeout in this run.
The three seeds therefore cover 600,000 ledger steps in the repeated run, with
both conservation identities asserted at each step. All 13 process-crash points,
partial-send, fault and saturation cases are included in that same repeated suite.

Development failures were reported and corrected: a -Werror unbraced assertion
macro in a ledger test; M7 provisional-hold/unsupported/two-thread expectations;
new raw-peer GET_LOGS fixtures missing required nullable fields; and fd sampling
before the second child had reached RUNNING. The final clean run has no retries,
disabled tests or lowered flags. A source review additionally caught the accepted
snapshot/retirement race and essential queue head-of-line blocking; these were
corrected and have deterministic regressions, including a one-slot reserve.
Read-error review also corrected unsafe filesystem entries being misclassified
as client argument errors; the historical-file test now checks this explicitly.

Leak evidence: 10 Writer start/stop cycles return `/proc/self/fd` to baseline;
Controller shows exactly three threads; bounded concurrent Workers retain steady
fds once running; repeated active/complete reads close per-Attempt fds (only the
one tasks/ directory may remain cached). External harness waits every owner and
checks absent owner process groups; retained M4/M7 tests verify child reaping/group
cleanup. End-of-repeat `ps -eo pid,ppid,stat,comm` inspection found no surviving
Controller, Worker or ts_log_workload processes, including zombies.

Protected BUILD_COMMANDS SHA-256 before/after clean build:
`8bda49a0af4e612892bafdba48effb924573e02a6ccea33b3aa8bd5991d813ae`.
Ignored reading notes SHA-256 remains
`4b2e5a53c77198e9278ebdd3cf40be93ec4468d5707435f4729d26b4dabbc882`.
No sanitizer or block-device power-loss test was run or claimed. SIGKILL provides
process-crash ordering/recovery evidence only; hardware/filesystem sync assumptions
remain the frozen trusted-local-filesystem contract.

## K. Source review, risks and M9 boundary

Reviewed AGENTS, Architecture Gate, Full Implementation Prompt, full M8
authorization, PROJECT_SCOPE, ARCHITECTURE, OWNERSHIP, STATE_MACHINE, FAILURE_MODEL,
PROTOCOL, DECISIONS, TEST_STRATEGY, DEPLOYMENT, IMPLEMENTATION_PLAN and the original
scope/review checklist, plus accepted M3–M7 source contracts/report/demo.

Reviewed all new production files and accepted-layer diffs for owner/thread/fd
lifetime, precise correlation, arithmetic, queue bounds, sync order and failure
cleanup. `git diff` confirms frozen documents, protocol, SQL schema/M5 operations,
TaskManager, scheduling heuristic, state enums and post-fork runtime unchanged.
No TODO/fake success/placeholder/detached-thread path added. Controller root
provisioning/SQLite locking remains accepted M7 behavior. All 14 reviewer questions
have source/test evidence in section I. No core architectural contradiction or
semantic deviation was found.

Intentional implementation limits: serial disk owner; finite 4096 owner cache and
existing lifetime metadata/session bounds; lower-priority snapshot reads may face
admission pressure; trusted namespace/no concurrent hostile local writer; failed
or interrupted chunks can leave physical unexposed partial tails; incomplete
historical visibility conservatively zero if not durably known. No repair,
retention, orphan scavenging, new retry policy or inferred complete data exists.
Uninterruptible disk operations can delay shutdown. These do not broaden scope or
claim atomicity between SQLite and the filesystem.

M9 and later have not started. No active cancel/retry CLI, reconnect, old-session
adoption, exactly-once execution, auth/TLS, public-network hardening, sandbox,
cgroups, uploads/follow, retention or systemd packaging was introduced. Independent
review and explicit merge authorization are still required.

## L. Gate

Milestone 8: PASS — awaiting explicit independent review; Milestone 9 and later milestones have not started.
