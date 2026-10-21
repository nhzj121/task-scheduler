# Milestone 7 — first real partial vertical slice

## A. Git and baseline

Authorized branch: `milestone-7-first-vertical-slice`, created from verified
`main == origin/main == daa15ebd03ddfb3945b015ed2dad774fac8e9c79` after fetch.
Accepted M6 tip `785269701299767980e84d79716d40d1ddf84265` is its ancestor.
The only pre-branch exception was the supplied M7 authorization document; it was
carried intact. No reset, stash, main commit, merge, PR or force push was used.
Tested implementation commit:
`d778bd42cb61e41b055ad953b42a556d4556ddec`. The subsequent documentation-only
delivery SHA and final tracking verification are provided in the handoff message.

Protected SHA256 values:

- Build note: `8bda49a0af4e612892bafdba48effb924573e02a6ccea33b3aa8bd5991d813ae`.
- Ignored personal reading notes:
  `4b2e5a53c77198e9278ebdd3cf40be93ec4468d5707435f4729d26b4dabbc882`.

Delivery checks: local/remote branch SHA equality, tracking ahead/behind `0/0`,
clean worktree, unchanged main/origin/main, unchanged protected hashes. These are
rechecked after the report commit/push; the exact delivery SHA is in the handoff.

## B. Implemented scope

M7-A: real `controller`, `worker`, `taskctl`, strict existing config parsers,
loopback defaults, help/usage/error exit codes, explicit startup/shutdown.

M7-B: actual TCP role-specific listeners, typed protocol/correlation, real M5
eventfd/channel/PersistenceThread, real M6 decisions and resource reservations,
durable submit response, committed paginated queries, registration/heartbeat/loss.

M7-C: actual Worker TCP registration and RUN/ACK/START/FINISH using M4 preparation,
fork/exec, real leader pid, reap/group cleanup/EOF, bounded read-interest adapter
and existing M3 finish ordering dependency. No second process or transport stack.

M7-D: isolated multi-process actual-binary/SQLite tests, independent external
readers, narrowly identified fault seams, restart/history paging, local demo and
source-level review. This remains a **partial vertical slice**, not final v1.

## C. Files and dependency direction

Added:

- `docs/spec/Milestone_7_Authorization_Codex_Development_Prompt.md` (user-supplied).
- `include/ts/client/client.h`, `src/client/client.cpp`.
- `include/ts/controller/application.h`, `src/controller/application.cpp`.
- `include/ts/controller/provisional_staging.h`, `src/controller/provisional_staging.cpp`.
- `include/ts/controller/views.h`, `src/controller/views.cpp`.
- `include/ts/worker/application.h`, `src/worker/application.cpp`.
- `include/ts/worker/execution_manager.h`, `src/worker/execution_manager.cpp`.
- `src/apps/config_file.h`, `src/apps/controller.cpp`, `src/apps/worker.cpp`,
  `src/apps/taskctl.cpp`.
- `tests/component/m7_runtime_support_test.cpp`, `tests/unit/m7_staging_test.cpp`.
- `tests/helpers/m7_fault_controller.cpp`, `tests/integration/m7_loopback.py`.
- `examples/m7/controller.json`, `examples/m7/worker-a.json`, `examples/m7/worker-b.json`.
- `docs/M7_LOCAL_DEMO.md`, this report.

Modified:

- `CMakeLists.txt`: three application libraries and binaries, M7 unit/component
  target, test-only fault executable, standard-library Python integration cases.
- `include/ts/runtime/process_runtime.h`, `src/runtime/process_runtime.cpp`:
  optional before-each-read allowance and non-owning parent observation snapshot.
  These are necessary to obey credit before reading and START only after exec.
  Default M4 behavior and the complete child path are unchanged.
- `include/ts/controller/worker_registry.h`, `src/controller/worker_registry.cpp`:
  retained volatile advertisement, owner-thread value enumeration, removal of a
  disconnected *pending replacement* without forgetting its UUID/old LOST duty.
- `include/ts/controller/task_manager.h`, `src/controller/task_manager.cpp`:
  generation-bound connection-close adapter handles both accepted and pending
  replacement connections through the existing invalidation/LOST gates.
- `src/storage/state_store.cpp`: validation fd closes **before** SQLite opens.
  On Linux, closing another fd for the same database inode after SQLite has
  acquired fcntl locks silently releases those process locks. Independent reader
  tests exposed an unlinked-WAL/visibility problem; commit `0fb701f` isolates this
  fd-lifetime repair. No schema, transaction order or evidence contract changed.

No files removed. No existing regression tests removed/disabled.

Direction: `controller -> controller_state/persistence/net/protocol`,
`worker -> runtime/net/protocol`, `client -> net/protocol`; executable entrypoints
compose these. Existing scheduler/storage/io/protocol/net layers never depend on
applications. Existing C++ dependencies only: Threads, nlohmann_json, SQLite3,
spdlog, GoogleTest for tests. Python3 is a test harness dependency only.

## D. Controller wiring

One EventLoop owns both role-specific ConnectionManagers and M6; each manager's
connection identity namespace is used only with its own role. Peer values retain
full stable connection/generation identities, never raw borrowed socket input.
EffectPort only appends bounded values, never reenters TaskManager. Actual sends
occur after effect consumption and current-generation revalidation.

M5 owns its SQLite connection/thread. Startup Ready follows its recovery commit;
Controller then pages all Tasks and every Attempt history, invokes M6 restore once,
and only then opens listeners. No historical Worker lease or reservation restores.
Queries use high-half operation IDs distinct from M6 mutation IDs; all completions
verify exact correlation. GET reads Task/history/verification, compares committed
versions, and retries a changing snapshot at most three times; no mixed history
or partial-history success. History >200 and tuple-tied task pages are tested.

Bounds: 32 sockets per role, 16 outstanding requests per connection, 64 application
queries, 4096 effect slots; 64 MiB startup/retained-metadata admission budget and
4 MiB reply accounting with independent frozen wire cap. New accepted submissions
charge four M5 owned-envelope amounts plus 8192 bytes per Task/Attempt. Charges
are conservative and not refunded within the process lifetime. At most 4096
successful registrations bound M6 UUID tombstones for that lifetime. Oversized
startup history fails explicitly; admission pressure does not evict history.

Submit responds only to the matching committed M6 effect. RUN records
BytesMayHaveEscaped *before* entering M3 output, and only after Assign commit.
Actual persistence degradation/unknown commit stops mutations/scheduling, closes
sessions and exits nonzero. No diagnostic-only fail-stop. Shutdown closes listeners,
stops admission, drains accepted metadata/loss decisions, then closes sockets and
joins outside dispatch. All channel fd/registrations outlive the joined thread.

## E. Client and actual CLI

See `docs/M7_LOCAL_DEMO.md` for exact four-terminal commands. Examples:

```bash
/home/nhzj121/build/task_scheduler/taskctl --port 7420 workers
/home/nhzj121/build/task_scheduler/taskctl --port 7420 submit /bin/true
/home/nhzj121/build/task_scheduler/taskctl --port 7420 --json get TASK_UUID
/home/nhzj121/build/task_scheduler/taskctl --port 7420 --json list
```

Numeric host/port configurable; one outstanding client request with exact expected
type/ID (or ERROR_RESPONSE), bounded timeout and no automatic resubmit. Lost submit
response is explicitly uncertain. JSON list is streaming NDJSON, one response per
page; taskctl automatically fetches all pages. GET/status, submit/list/workers use
real TCP. Logs/cancel/retry return UNSUPPORTED, exit 2; CLI/transport errors exit 1.

## F. Worker lifecycle

Fresh UUID per actual registration attempt, REGISTER first, correlated accepted
ACK required before RUN. No M7 reconnect loop. Advancing heartbeat sequence on a
real monotonic timer; wrong session/type/order stops the invalid connection.

ExecutionManager validates session/local resources/concurrency/finite ownership,
queues accepted ACK before preparation/launch, prepares all owner storage before
fork, and uses existing M4. Both output interests initially pause until actual
exec observation permits START. No START on parent preparation or child exec
failure. Each 10 ms main-loop timer services one nonblocking `runtime.poll_once(0)`;
there is no workload thread or busy-spin. M4 remains the sole SIGCHLD/reap owner.
Runtime captures its original signal mask before application INT/TERM blocking;
shutdown restores masks in reverse nesting order.

Per-tick reads cap at 64 KiB/eight chunks, each read <=16 KiB and remaining credit
and actual output room. Local ownership/resource release follows real M4 completion
and owning M3 FINISH enqueue, not Controller resource release. FINISH carries the
last cross-stream M3 emission ordinal as dependency. Partial socket writes cannot
overtake earlier logs. Session failure stops new launches and cancels/drains/reaps
local groups; undeliverable invalid-session bytes never become durability evidence.

## G. Exact pre-M8 boundary

Controller allocates each accepted session's raw byte vector and chunk metadata
vector *before* positive credits. Outstanding unused grants plus staged data remain
within global byte and slot capacity excluding essential-control slots. Default
window 1 MiB/64 chunks; global 64 MiB/(4096-128) slots. No replenishment, file API,
snapshot read or barrier exists. Valid credited admission failure is an internal
invariant failure/fail-stop, not suspension of Worker TCP reads.

Worker bounds **actual pipe reads**, not a later unsent buffer. Exhaustion pauses
pipes, may block workload writes indefinitely, while control/heartbeat stays live.
Even EOF of later quiet tasks can wait for window capacity. M7 output is volatile;
session invalidation reclaims it as incomplete.

Normal FINISH, including launched zero-output `/bin/true`, supplies only Incomplete
evidence. Task remains ACTIVE, Attempt RUNNING (or STARTING for launch failure),
`finalization_pending=true`, `logs_complete=false`; reservation remains held.
No fake ProvenEmpty, FinalBarrier or normal SUCCEEDED/FAILED. Explicit prelaunch
rejection retains the already-approved M6 ProvenEmpty rule; committed session loss
remains LOST/FAILED. A Controller crash recovers held nonterminal work as LOST.

## H. Real acceptance matrix

All following are `M7Loopback.*` cases in `tests/integration/m7_loopback.py`:

| Case suffix | Concrete evidence |
|---|---|
| real_vertical_slice_two_workers_and_crash_recovery | Real Controller + two Worker + taskctl processes; deterministic feasible placement, real FINISH/hold, actual SQLite nonterminal, crash/recovery PENDING/history/LOST, empty restored leases |
| help_invalid_configuration_lock_and_clean_cycles | Three help commands without files, invalid config/root/address/duplicate bind, second Controller lock denial, three start/stop cycles |
| fragmentation_roles_request_correlation_and_fd_churn | Single-byte and coalesced TCP frames, concurrent IDs, NOT_FOUND, 40 disconnected-submit/reused-socket cycles, wrong roles/malformed/oversized/unregistered Worker rejection |
| durable_submit_gate_and_disconnected_client | Real SQL-thread precommit gate, no response and external reader sees zero rows; independent workers query stays live; disconnected submit still commits |
| assignment_gate_reserves_before_run_and_reuses_capacity_after_loss | Actual held Assign, no RUN bytes precommit, tentative CPU prevents second placement, SQL bound session before observed RUN, wrong ACK ID -> committed LOST/OFFLINE/released CPU, real replacement Worker reuse |
| essential_failure_and_unknown_commit_fail_stop_without_false_effects | Real SQLite failpoint/AfterCommit uncertainty; nonzero Controller exit, no RUN/submit-success leak; independent reader distinguishes rollback and durable-but-unknown submit |
| pagination_history_restore_and_truthful_unsupported | Seeded fixture with 205 prior Attempts and 207 Tasks, actual paginated restore and GET, 100/100/7 CLI pages, tuple ties/no duplication, bad cursors, explicit unsupported commands |
| registration_replacement_heartbeat_and_staging_exhaustion | Positive window backed by capacity, excess session exact rejection shape, duplicate heartbeat does not renew, OFFLINE null session/retained heartbeat, wrong-session close |
| output_credit_backpressure_control_liveness_and_group_shutdown | Actual `/usr/bin/yes`, eight-byte initial budget, healthy beyond offline deadline, no false FINISH, group cancellation, child reaped and group absent |
| worker_wire_exec_failure_credit_and_finish_barrier | Test TCP peer with real Worker/M4; six true exec/pid/reap flows, parent cwd and exec failures without START, exact echo bytes/sequences and log-before-FINISH, steady Worker fd count |
| negative_missing_ack_and_session_replacement | Real negative ACK/proven prelaunch rejection; missing ACK -> LOST; replacement accepted only after old SQL LOST; coalesced ACK/START/FINISH arrives ahead of completion dispatch and still commits exact RUNNING version 2 with incomplete logs |
| startup_read_failure_never_opens_ready_listeners | Real M5 SELECT fault, no READY and both listener ports refused |
| real_launch_failures_hold_starting_without_fabricated_running | Full Controller/Worker/taskctl flow for parent preparation and child exec failures, actual SQLite STARTING without started timestamp or complete logs |
| connection_output_pressure_is_bounded_and_other_clients_survive | Non-reading client/repeated batches, other clients remain responsive, descriptor reuse and exactly two Controller threads |
| partial_run_transport_failure_keeps_uncertainty_and_commits_lost | Explicit test-only SocketIo seam sends seven *real TCP bytes*, then EPIPE; actual SQL LOST, no fabricated START/non-execution certainty, release after commit |
| retained_metadata_and_session_tombstones_have_lifetime_admission_bounds | Small test-only application limits prove metadata temporary-unavailable and registration rejection, no durable mutation/lease eviction to hide pressure |

The two gate tests also reproduce the repaired same-inode fd/SQLite external-reader
bug: before the fd-lifetime fix, independent readers could observe no committed
rows while the Controller's own query saw them. They now remain mutually visible.
Test fixture row seeding is not presented as production Task submission. Raw wire
peers/fault hooks are explicitly separate from the actual two-Worker end-to-end path.

## I. Source/ownership review and operational safety

Frozen authority checked: AGENTS; ARCHITECTURE_GATE; Full Implementation Prompt;
complete M7 authorization; PROJECT_SCOPE; ARCHITECTURE; PROTOCOL; OWNERSHIP;
STATE_MACHINE; FAILURE_MODEL; TEST_STRATEGY; IMPLEMENTATION_PLAN; DECISIONS;
DEPLOYMENT, source specification and architecture checklist. No frozen document,
state name/transition, protocol schema/ID, placement algorithm or evidence changed.

The 14 mandatory source-review questions have these evidence bindings:

| # | Source location | Tests in H / focused M7 cases |
|---|---|---|
| 1 | `src/controller/application.cpp:421`, `src/worker/application.cpp:62`, `src/client/client.cpp:58` | real_vertical_slice; fragmentation |
| 2 | `src/controller/application.cpp:421`, `:547` | real_vertical_slice; pagination_history; startup_read_failure |
| 3 | `src/controller/application.cpp:171`, `:199`, `:305`; `src/client/client.cpp:35` | fd_churn; assignment_gate wrong ACK; negative_missing_ack |
| 4 | `src/controller/application.cpp:568`, `:622` | assignment_gate; partial_run_transport_failure |
| 5 | existing `src/controller/task_manager.cpp:358`, `:664` (commit/ledger gates unchanged) | assignment_gate; essential_failure; original 65 M6 tests |
| 6 | `src/worker/execution_manager.cpp:144`, `:186`, `:83` | worker_wire_exec_failure; real_vertical_slice; real_launch_failures |
| 7 | `src/controller/application.cpp:389` (Incomplete FinishedEvent); `src/worker/execution_manager.cpp:83` | real_vertical_slice; real_launch_failures |
| 8 | `src/controller/provisional_staging.cpp:14`; `src/worker/execution_manager.cpp:47` | all M7Staging tests; staging_exhaustion; output_credit |
| 9 | `src/controller/application.cpp:184`, `:305`; `src/controller/task_manager.cpp:620` | replacement; heartbeat; M7Registry.ClosedPendingReplacementCannotBecomeHealthyAfterOldLossCommits |
| 10 | `src/controller/application.cpp:117`, `:667` | essential_failure; startup_read_failure; partial_run |
| 11 | `src/controller/application.cpp:104`, `:642`; `src/worker/application.cpp:44`, `:122` | clean_cycles; worker_wire steady fds; output_credit child/group; all M4 fd/reap regressions |
| 12 | `src/controller/application.cpp:247`, `:449`; `src/controller/views.cpp:29` | pagination_history; assignment_gate OFFLINE; malformed/NOT_FOUND |
| 13 | `CMakeLists.txt:112`; public headers and include inspection | All old targets retained; source scan and frozen-source diff |
| 14 | `tests/integration/m7_loopback.py:227` and CMake M7 labels | Actual real-process suite plus full original 364 regressions |

Callback inputs are synchronous borrows, typed owned values cross async boundaries.
Peer deletion is deferred; Application owners are destroyed outside callbacks.
The M4 child-after-fork function `src/runtime/process_runtime.cpp:158` is unchanged:
close/dup2/setpgid/fchdir/sigprocmask/execve/fixed status write/_exit only, errno/EINTR
handling, no C++ allocation/PATH/environment transformation or new business signal
handler. Runtime allowance/observe additions run only in the parent.

No TODO/stub/fake success path, second networking/process stack, EventLoop SQL,
thread per workload, LogWriter/filesystem log operation, fsync barrier, replenished
credit, schema version change, live Worker persistence, M8/M9 orchestration or new
production dependency was introduced. `git diff --check` and frozen-source diff
are part of final validation.

Security remains trusted local filesystem/private network, default loopback. No
sandbox/cgroup/auth/TLS guarantee. Explicit numeric listener config is operator
controlled. Shutdown deadlines diagnose cleanup failure, never abandon/relabel a
live owned child as reaped; escaping groups/uninterruptible processes retain v1's
documented limits, not a hard universal termination bound.

## J. Formal WSL validation

Environment: Ubuntu22.04.3, GCC 11.4.0, CMake 3.22.1, C++17; all new targets inherit
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`.

```bash
set -euo pipefail
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler --target clean
test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -L m7 \
  --repeat until-fail:20 --output-on-failure
sha256sum /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
```

Final results: clean configure/build **PASS**; complete clean-build CTest
**389/389 PASS**, 22.37 s. A further full post-implementation-commit regression
also passed **389/389**, 24.03 s. Complete M7 repeat **25 cases x 20 = 500 passing
test executions**, zero failures, 211.63 s; this includes **320 external-process
case executions** (all sixteen integration cases x 20), not only a selected smoke
subset. The updated coalesced-observation assertion was exercised in all twenty
repeats of its case and the final complete postcommit regression.

Original baseline 364/364 was run before M7 changes; final suite contains
389 tests = 364 + 25 M7 (nine GoogleTest cases and sixteen external process cases).
Harness timeouts/teardown passed, `pgrep -x controller` and `pgrep -x worker` found
no lingering owners after final validation. The yes workload test checked its
specific child PID/PGID absent, the real Worker wire test checked stable fd count
and no owned children, Controller pressure tested bounded fd/thread counts, and
the original M4 cleanup tests all passed. Both protected SHA256 values matched.
`git diff --check main...HEAD` passed; frozen architecture/domain/protocol/
scheduler source diff was empty. No random seed is needed:
fixed deterministic scenarios, no randomized harness. Dynamic OS scheduling/ports
are intentional stress variation, not a claim of exhaustive interleaving coverage.
ASan/UBSan/TSan: not run; no sanitizer evidence is claimed.

## K. Limits and later work

No architecture/semantic deviation. The three narrow accepted-layer adaptations
(M4 read/observation, M6 volatile views/closed pending replacement, M5 POSIX fd
lifetime) are integration support, not a redesign. Startup/read/admission ceilings
are explicit implementation bounds and failures, not silent history truncation.

M8 still must implement sole-owner durable LogWriter, file/directory sync barriers,
backed replenished windows, safe terminal evidence and GET_LOGS. M9 still must
complete distributed cancel/retry/reconnect/failure handling. Normal pre-M8 FINISH
does not release Controller reservations or expose durable terminal success.
No exactly-once, high-output durable acceptance, multi-host security or production
deployment packaging is claimed. Manual demo is documented; automated equivalents
were exercised, not a claim that four interactive human terminals were operated.

## L. Gate

All documented M7-A through M7-D acceptance criteria are satisfied with the
above automated evidence and source review. No unresolved architecture
contradiction was found; the known M7 limitations are deliberate authorization
boundaries, not implemented future subsystems. Independent review remains the
next gate. No main merge or M8+ work is authorized or started.

**Milestone 7: PASS — awaiting explicit independent review; Milestone 8 and later milestones have not started.**
