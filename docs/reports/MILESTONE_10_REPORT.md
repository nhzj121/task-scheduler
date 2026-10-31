# Milestone 10 acceptance report

Delivery review: 2026-10-31 (Asia/Shanghai). Scope: M10-A through M10-D only.

## A. Git/baseline and workspace protection

- Accepted local/remote main: `8bffdcf302adf31a73aa01926004de217a6f2a46`.
- Accepted M9 implementation: `1a93ff9d66052a47627fa29d3dfd26651d3f8a35`, verified ancestor.
- Branch: `milestone-10-deployment-hardening-benchmark`, created from that main.
  Fetch on resumption confirmed the same mainline. No merge, reset, force-push,
  release/tag, package publication or production deployment is authorized/performed.
- Implementation/evidence commit: `8926dd0b753587f031019a105a9ab3a8abcf3add`.
  Initial report/review HEAD: `2d5594f56cdb42e58ef6a90a4d77156505e2fc89`.
  Independent review required the final TSan integration cleanup under G below.
  Cleanup implementation/evidence commit: `fe12ba077f282c30eb0cc9e5b83973ff0853482d`.
  The final acceptance-report commit is the delivery HEAD; its own exact SHA
  is provided in the delivery message and by `git rev-parse HEAD`. A report cannot
  embed its own commit hash. Delivery uses an ordinary branch-only push, followed
  by `git ls-remote` verification of that exact tip.
- Final expected/required comparison: main-to-M10 `0 4`, local/remote M10 `0 0`,
  main/origin/main `0 0`, all main refs still at the accepted SHA above. These
  post-push checks must succeed before the external PASS report is issued.
  Code diff and milestone commits are reviewed; `git diff --check main...HEAD`
  and staged whitespace checks pass. No credentials, database files, binary logs,
  private notes or downloaded toolchain packages are committed.
- The only original untracked file was the owner-supplied M10 authorization;
  its content is preserved, with header hard-break whitespace expressed as blank
  paragraphs for `git diff --check`.
- Protected SHA-256 before work and after validation:
  - `BUILD_COMMANDS.md`: `8bda49a0af4e612892bafdba48effb924573e02a6ccea33b3aa8bd5991d813ae`.
  - `.local-notes/reading-notes.md`: `4b2e5a53c77198e9278ebdd3cf40be93ec4468d5707435f4729d26b4dabbc882`.
- Ignored notes/build/Python cache and unrelated material are preserved. Cleaning
  used CMake targets; no user build directory or database was deleted.

## B. Implemented M10-A–D

| Checkpoint | Concrete implementation |
|---|---|
| A | GNUInstallDirs staging of three real runtime binaries, examples, public docs/operator tools; Controller/Worker/template units; real non-root systemd lifecycle validation. |
| B | Real invalid configuration, duplicate keys, permissions, symlink root, lock/bind, private diagnostics, shutdown/ownership/rerun/interruption tests; no daemon diagnostics redesign. |
| C | All-target ASan+UBSan/LSan and independent practical TSan runner; compatibility probe; pinned read-only CI; original regressions and repeated M10/M9 fault tests. |
| D | Durable-terminal binary/empty benchmark with raw task/accounting/resource evidence; README, operations/development/source map/interview navigation, local reproduction and this report. |

No subsequent milestone, authentication, TLS, sandbox, resource enforcement,
storage subsystem, automatic retry, work adoption, new wire field or state exists.

## C. File inventory and dependencies

Modified:

- `CMakeLists.txt`: sanitizer selection/instrumentation, install rules, seven M10
  CTest entries. Original target dependencies and warning flags remain intact.
- `src/protocol/messages.cpp`: the sole changed production C++ file. `read_object`
  stages decoded fields in owned optional tuple slots before constructing the
  message. This fixes LSan-observed retention of earlier allocated fields when a
  later field rejects. Field validation order, diagnostics, schemas/IDs and API
  remain unchanged. The new 1,000-rejection regression runs with LSan enabled.
- `docs/INTERVIEW_NOTES.md`: implemented source/test tradeoff map and measured-claim
  caveats precede the unchanged original architecture-phase notes; no decision changes.
- Final TSan cleanup modifies only test/tooling/docs: the runner now requires
  actual application integration; CI retains those artifacts; a test-only
  `ts_tsan_thread_probe` target calibrates runtime overhead. M7 Harness's default
  Controller thread expectation remains exactly 3; M8's existing assertion reads
  that default, with exactly 4 used only by the calibrated TSan adapter. No
  production file changes were needed after the independent-review baseline.

Added:

- `README.md` and `docs/{OPERATIONS,DEVELOPMENT,ARCHITECTURE_IMPLEMENTED,M10_LOCAL_DEMO}.md`.
- `docs/spec/Milestone_10_Authorization_Codex_Development_Prompt.md` (owner supplied).
- `examples/deployment/{controller,worker,worker-a,worker-b}.json`.
- `packaging/{task-controller,task-worker,task-worker@}.service.in`.
- `scripts/{cluster_support,local_cluster,benchmark,run_validation,systemd_validation}.py`
  and `scripts/workloads/binary_output.py`.
- `.github/workflows/linux.yml`.
- `tests/component/m10_protocol_hardening_test.cpp`,
  `tests/helpers/{sanitizer_probe,tsan_clockwait_probe}.cpp`,
  `tests/integration/m10_deployment.py`.
- Final cleanup adds `tests/helpers/tsan_thread_probe.cpp`,
  `tests/integration/tsan_applications.py`, and
  `docs/validation/M10_TSAN_APPLICATIONS_EVIDENCE.json`; it modifies
  `tests/integration/{m7_loopback,m8_loopback}.py` as described above and updates
  the already-listed CMake, runner, CI, development guide and this report.
- `docs/benchmarks/M10_BENCHMARK_REPORT.md`, `docs/benchmarks/m10_results.json`,
  `docs/validation/M10_VALIDATION_EVIDENCE.json`, `docs/reports/MILESTONE_10_REPORT.md`.

No removals/renames. New Python code uses only the standard library; no production
C++ dependency is added. GCC 12 is a development-only TSan toolchain. SQLite owns
metadata on PersistenceThread; LogWriter owns task-log filesystem operations on
its separate thread; operator tools run outside those owners. No test/helper
binary, sanitizer runner or privileged test installer is packaged as a runtime.

## D. Packaging/build/deployment

Formal environment: Ubuntu 22.04 WSL2, GCC 11.4, CMake 3.22.1, SQLite 3.37.2;
data/temporary roots are Linux-local ext4, not `/mnt/e` DrvFs. Source remains the
approved mounted checkout. No claim of a newly provisioned OS/native host is made.

Actual runtime-only configure/build/install (new independent Release tree):

```bash
SRC=/mnt/e/Deck_all_need/projects/work/task_scheduler
PKG=/home/nhzj121/build/task_scheduler-m10-package
STAGE=/home/nhzj121/build/task_scheduler-m10-stage-final
cmake -S "$SRC" -B "$PKG" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON -DCMAKE_INSTALL_PREFIX=/opt/task-scheduler-m10
cmake --build "$PKG" -j2
DESTDIR="$STAGE" cmake --install "$PKG"
```

All returned zero. GTest discovery is disabled, demonstrating no runtime install
dependency on GTest. Only controller/worker/taskctl enter `bin`; operator tools
and configs enter `share/task-scheduler`; units enter `lib/systemd/system` and
guides enter the GNUInstallDirs doc directory. No accounts/live configs/services
are created by CMake. `ExecStart` reflects the configured final prefix; relocating
via DESTDIR does not silently change it. Reinstallation after tool fixes returned
zero. CTest repeatedly runs staged executables from a staging path with spaces.

## E. Real systemd lifecycle

Owner explicitly approved uniquely prefixed temporary services, non-root test
accounts and isolated roots, followed by cleanup. Permission was required by
authorization §4; it was not inferred from ordinary build permission.

The system manager was running (PID 1 systemd). Final complete run:

```bash
# Invoked through WSL's root entry point after explicit owner permission.
python3 "$SRC/scripts/systemd_validation.py" --allow-temporary-system-services \
  --bin-dir "$STAGE/opt/task-scheduler-m10/bin" \
  --units-dir "$STAGE/opt/task-scheduler-m10/lib/systemd/system" \
  --artifacts /home/nhzj121/build/task_scheduler/m10-systemd-complete
```

Return 0 / PASS, prefix `ts-m10-14a2e0b3f5`. The packaged Controller, ordinary
Worker and Worker template all underwent actual service execution. Two template
instances were HEALTHY with distinct IDs/Sessions; an additional ordinary Worker
was started/restarted/stopped and its Session changed. UID checks proved non-root
operation. Temporary test-only overrides changed names/accounts/paths/configs,
not restart, signal, stop-timeout, fd-limit or mask policy.

Commands captured in `systemd.json`: `systemd-analyze verify`, daemon-reload,
start/status/stop/restart, MainPID/UID inspection and journal retrieval. A running
workload's Worker stop reaped its child and produced LOST; restart used a fresh
Session. Controller restart during RUNNING recovered LOST/FAILED, retained
infeasible PENDING and genuine terminal history, and replaced both Sessions.
Another Worker restart changed Session again. State root was owned/mode 0700,
metadata had no group/other access. Required command results were zero; polling a
deliberately stopped unit can correctly return `is-active` code 3.

All temporary units/accounts/groups and both marked test roots were independently
verified absent afterward; cleanup_errors was empty. Conventional service names
and existing `/var/lib/task-scheduler` were untouched. `systemd-analyze` also emitted
an unrelated installed snapd RestartMode warning; it did not reject our units.
Earlier template runs also passed; the final run includes ordinary Worker coverage.

Units use UMask 0077, non-root users, Controller StateDirectory, restart-on-failure
after 3s, NOFILE 16384 and 45s stop timeout. The timeout exceeds example 2s group
grace plus normal drain margin, not arbitrary storage hangs. Worker KillMode=mixed
allows normal main-process cleanup before systemd's final timeout enforcement;
it is not proof of log completion under forced killing or a resource sandbox.

## F. Operational hardening

`M10.operational_rejections_and_private_diagnostics` uses the actual parsers and
binaries: missing/invalid/duplicate/unknown config, unwritable/world-writable or
symlink root, duplicate Controller ownership, and occupied ports all fail
explicitly. A synthetic environment value appears exactly in task output and not
in system logs. Metadata/log permissions are checked. No arbitrary environment,
arguments or raw output were added to daemon diagnostics.

`M10.script_ownership_spaces_and_reruns` tests reused roots, duplicate start,
unmarked-directory preservation, tampered PID birth-time refusal before *any*
sibling receives a signal, interrupted demo cleanup, and strict argv verification.
The helper now bounds transient `/proc` argv rechecks to 50 reads; it never trusts
an empty/mismatching argv and rechecks PID/UID/executable identity each time.
Tests inject an empty snapshot followed by the exact argv, and persistent wrong
argv that must fail. No broad process-name kill, fallback SIGKILL or unverified
PID signalling is used. Failures retain artifacts; operator data is never removed.

One post-interruption benchmark startup initially hit the argv guard. Immediate
inspection found the exact recorded owner/argv; only then was that isolated
Controller stopped with the existing guarded tool. Its failed output/log remains
in `task_scheduler-m10-benchmark-final`. Startup and cleanup failures now record
FAILED/FAILED_CLEANUP explicitly instead of leaving ambiguous RUNNING output.
The regression also supplies a missing binary directory and checks a recorded
startup failure with no fabricated successes. Final measurements use a new root.

## G. Sanitizers and CI

All CMake project targets, including libraries, binaries, tests and helpers, receive instrumentation and
retain `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`.
ASan+UBSan and TSan are separate builds; instrumentation-only non-PIE flags do not
change production defaults. GTest discovery uses PRE_TEST in sanitizer trees.

```bash
python3 "$SRC/scripts/run_validation.py" --mode asan --source "$SRC" \
  --build /home/nhzj121/build/task_scheduler-m10-asan --jobs 2
```

ASAN_OPTIONS=`detect_leaks=1:halt_on_error=1:abort_on_error=1`;
UBSAN_OPTIONS=`halt_on_error=1:print_stacktrace=1`. Probe actually entered main;
473/473 passed in 115.16s, including practical fork/exec and cluster tests. No
leak-disable/suppression. Expanded tool assertions subsequently ran under ASan too.

Initial instrumented execution (before the new regression) exposed six protocol
validation failures with real LSan allocation reports. `read_object`'s staged RAII
ownership fixes this exception cleanup path; the accepted tests plus new late-field
rejection test pass without suppressing any report. No frozen schema changed.

GCC 11.4 TSan first failed before main on this WSL mapping; process-local setarch
allowed its queue/wakeup tests, but it then reported double mutex lock during the
persistence fixture's standard timed condition-variable handshake. The standalone
`tsan_clockwait_probe.cpp` reproduces that diagnostic without any project code;
GCC 12.3 runs the same program cleanly. Tests/clock semantics were not weakened.

An isolated GCC 12.3 Ubuntu package extraction under the build tree supplied the
second TSan toolchain; no root installation or global ASLR setting was changed:

```bash
TC=/home/nhzj121/build/task_scheduler-m10-toolchain/root
CXX="$TC/usr/bin/g++-12" LD_LIBRARY_PATH="$TC/usr/lib/x86_64-linux-gnu" \
  python3 "$SRC/scripts/run_validation.py" --mode tsan --fixed-address-layout \
  --source "$SRC" --build /home/nhzj121/build/task_scheduler-m10-tsan-gcc12 --jobs 2
```

Return 0/PASS: 4 BoundedQueue, 3 channel/wakeup, 6 PersistenceThread/channel and
17 real LogWriter tests = 30. TSAN_OPTIONS=`halt_on_error=1`, no suppression.
Each process uses `setarch x86_64 -R`; the initial GCC 12 probe without that option
also exited before main, so it is retained as a failed diagnostic, not counted as
a successful test. Final probe and all 30 selected tests really executed.
This does not claim the entire fork-based cluster matrix ran under TSan.

### Final TSan acceptance cleanup

The initial 30-case selection omitted the frozen TEST_STRATEGY requirement for
selected Controller/Worker integration tests. Independent review identified that
coverage gap; the previous broad TSan gate claim was incomplete. The final runner
requires the original 30 cases **plus all three** of these existing workflows:

| Existing case | Actual instrumented workload and checked path |
|---|---|
| M8Loopback.test_empty_real_barrier_terminal_release_and_restart | Ordinary Controller + Worker, two direct /bin/true executions, complete empty files/barriers, SQLite terminal state, resource release and Controller restart/history. |
| M8Loopback.test_large_binary_multiple_replenishments_and_paged_cli_are_exact | Ordinary Controller + Worker, instrumented ts_log_workload; exact 4,194,443 stdout and 3,145,979 stderr bytes; credit replenishment, LogWriter write/completion wakeups, final barrier/SQLite commit, exact paginated downloads and fd check. |
| M9Loopback.test_retry_can_succeed_on_another_worker_preserving_exact_old_log_history | Ordinary Controller and Workers a/b, instrumented ts_runtime_test_helper fail-until-file; first FAILED then manual retry SUCCEEDED, exact old Attempt/log preservation and restart; 15 stdout/19 stderr bytes each Attempt. |

The exact invocation remains the GCC 12 runner command above. It now records and
runs `setarch x86_64 -R python3 tests/integration/tsan_applications.py --bin BUILD
--artifacts BUILD/tsan-applications-UNIQUE --fixed-address-layout`. These are real
instrumented applications, not an instrumented Python harness: `nm -u` verifies
TSan memory-access/prologue symbols, target flags include `-fsanitize=thread`,
and live `/proc/PID/exe`, libtsan mappings and personality `0x40000` are retained.
Nine application startups (five Controller, four Worker), all application exit
codes 0, and no race diagnostics are required. Skipped/zero-test cases cannot pass.
Final execution returned 0: the three cases took 0.816s, 20.283s and 0.756s
respectively; the complete application driver (including calibration/provenance)
took 23.363s. Final raw evidence directory:
`/home/nhzj121/build/task_scheduler-m10-tsan-gcc12/tsan-applications-hj43droj`.
The preceding adapted run also passed all three; neither that repeat nor the
native calibration binary is counted as an additional unique integration case.

Initial large-log execution reached its exact-output/persistence checks but failed
the native total-thread assertion (4 versus 3), without a sanitizer finding.
Those failed unittest logs and retained application roots remain as evidence.
A first main-only calibration attempt did not establish a stable constant overhead
and was not counted as passing. The final two-application-thread control measures
native counts [2,2,2,2,2] and TSan [3,3,3,3,3], both exits 0. The ordinary M8 case
still requires exactly 3; the TSan-only adapter requires exactly 4. This accounts
for the detector's additional thread without weakening the application owner
contract, allowing an arbitrary count, disabling a test or suppressing any race.
All three selected cases then passed, including the original large-log workflow.

Raw `applications.json`, per-case unittest/system/workload logs, configurations,
SQL terminal facts, binary hashes/flags and process outcomes are retained in the
unique artifact directory named by the final `validation.json`. The compact
[cleanup evidence](../validation/M10_TSAN_APPLICATIONS_EVIDENCE.json) records the
exact commands, final case durations, diagnostic history and regression results.
CI runs the same extended runner and uploads the retained directory. No selected
integration case remains unverified and no TSan/fork exclusion was used.

CI pins official checkout/upload-artifact action SHAs, read-only contents permission,
independent Debug/ASan/TSan trees and a 40-minute bound; TSan selects GCC 12. The
equivalent repo runner was executed locally. Hosted CI execution is not claimed,
and privileged systemd testing is deliberately separate from ordinary CI.

## H. Stress, failure and ownership traceability

| Requirement | Actually retained/executed evidence |
|---|---|
| Crash/restart, PENDING/ACTIVE/terminal | M9Loopback controller_crash_running, assignment/cancel commit crash gates; actual systemd Controller restart. |
| Worker stop/SIGKILL | Real systemd workload stop; M9Loopback killed_worker_lost (test subreaper owns intentionally orphaned child). |
| Reconnect after cleanup | M9Loopback running_disconnect_churn and idle_reconnect; checks pgid disappearance, no children, fixed fd count, one Worker thread and fresh UUIDs. |
| Metadata uncertainty and effect order | M5 transaction/failure tests; M9 commit gates/failed-unknown commits/held LOST replacement and negative CANCEL ACK tests. |
| High-output control responsiveness | M8Loopback stalled_writer and independent byte/slot pause; M9Loopback stalled_log_path_cancel_control_two_workers; exact durable tails. |
| Wire/log/namespace/SQLite order | M8Loopback partial_tcp_sends; every_file_directory_and_sql_gate; crash boundaries; LogWriter sync-failure/empty-barrier tests. |
| Snapshot/pagination/disconnect | M8 large_binary_multiple_replenishments_and_paged_cli, active_snapshot_is_fixed, clamped frame-limit paging; M9 disconnected correlation tests. |
| Queue/fd/thread lifetime | BoundedQueue/IoStress/StorageStress, LogWriter dedicated-thread/fd tests, Runtime fd/reap suite, M9 churn and M10 tracked teardown. |

The original 466 tests ran in both normal and ASan full suites. All 36 M9 cases
also passed 20 repetitions (720 executions, 514.74s). These cover actual TCP,
SQLite, process groups and fault gates, not mocks standing in for daemon work.
Bounded queue/credit observations are not a claim that arbitrary disk growth or
workload CPU/memory is constrained. Escaped setsid descendants and uninterruptible
processes remain outside finite cleanup guarantees. No physical power-loss or
remount validation is claimed.

## I. Benchmarks

Final [benchmark report](../benchmarks/M10_BENCHMARK_REPORT.md) and
[raw JSON](../benchmarks/m10_results.json): PASS, 12 runs, 240 measured successes,
zero failures, plus 24 completed warmups. Four scenarios each use three fresh
repetitions of 20 measured tasks: `/bin/true` and direct deterministic Python binary
output, one/two same-host Workers. Every task has one durable SUCCEEDED Attempt,
complete non-finalizing logs, exact downloaded/stored bytes and matching SQLite
counters; final live reservations/running counts are zero. No ACK is completion.

Median terminal Tasks/s: 1 Worker empty 28.377, binary 19.525; 2 Workers empty
25.355, binary 27.595. These include sequential taskctl creation/query observation
delay and all normal durability barriers, exclude subsequent download checks, and
do not establish scalability. Raw per-task times, all run rates, configs, host/FS
context and server CPU/RSS/fd/thread samples are retained. Full exact command and
interval/quantile limitations are in the report. Original data/configs remain at
`/home/nhzj121/build/task_scheduler-m10-benchmark-delivery`; all its services stopped.
Earlier pilots and the zero-sample startup failure remain separate, unpooled and
disclosed; the final run followed all project validation processes.

## J. Formal build and tests

Completed pre-interruption clean validation:

```bash
BLD=/home/nhzj121/build/task_scheduler
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTS_SANITIZER=none
cmake --build "$BLD" --target clean
test -f "$BLD/BUILD_COMMANDS.md"
cmake --build "$BLD" -j2
ctest --test-dir "$BLD" --output-on-failure
ctest --test-dir "$BLD" -L m10 --output-on-failure --repeat until-fail:20
ctest --test-dir "$BLD" -L m9 --output-on-failure --repeat until-fail:20
```

473/473 PASS in 72.57s; 7 M10 cases ×20 PASS in 137.92s; 36 M9 cases ×20 PASS in
514.74s. Baseline before changes was 466/466 PASS in 70.73s. Inventory comparison
proved all 466 original names and labels unchanged. Initial delivery modified no
accepted test file; the final cleanup's equivalent native thread assertion and
strict TSan-only overhead adaptation are documented in C/G. The seven additions are:

1. `M10Protocol.RepeatedLateFieldRejectionRetainsDiagnosticsAndReleasesDecodedOwners`.
2. `M10.sanitizer_runtime_probe`.
3. `M10.staged_install_and_configurations`.
4. `M10.local_lifecycle_and_durable_workflows`.
5. `M10.operational_rejections_and_private_diagnostics`.
6. `M10.script_ownership_spaces_and_reruns`.
7. `M10.benchmark_terminal_accounting`.

After the external-tool correction and explicit live resource-accounting check:

```bash
cmake --build "$BLD" -j2
ctest --test-dir "$BLD" --output-on-failure
ctest --test-dir "$BLD" -L m10 --output-on-failure --repeat until-fail:20
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /home/nhzj121/build/task_scheduler-m10-asan -L m10 --output-on-failure
```

Final full **473/473 PASS, 70.48s**; M10 **7 ×20 =140 PASS, 131.85s**; updated
ASan M10 **7/7 PASS, 8.50s**. No production C++ changed on resumption. Intermediate
resumed full 473/473 (72.06s), M10 repeats (131.37s), and affected ASan checks
also passed before the final accounting assertion; they are not pooled as new cases.

Logs under `$BLD`: `m10-final-{configure,clean,build}.log`,
`m10-delivery-{ctest,repeat,asan-m10}.log`, `m10-m9-fault-repeat.log`, package logs,
systemd-complete evidence and benchmark-delivery log. Separate sanitizer trees
retain configure/build/probe/suite logs plus command/exit/duration `validation.json`.
The committed [compact evidence](../validation/M10_VALIDATION_EVIDENCE.json)
contains exact commands, result counts/durations, compatibility diagnostics,
protected hashes, inventory audit and independent cleanup results. Initial
discovery failures are disclosed in F/G; none is counted as a pass. No skipped
core case, sanitizer suppression, physical power-loss claim or hosted CI claim.
The initial pre-fix ASan ctest.log was replaced by the successful rerun; that
retention limitation is explicit in the evidence. The reproducible late-field
regression and final leak-enabled run remain, rather than an invented old log.
Final configure/build/install were also repeated successfully with the frozen
code before delivery (`m10-delivery-{configure,build,install}.log`).

### Regression validation after final TSan cleanup

Reconfigured and warning-as-error rebuilt the established Debug tree using the
same configure/build commands above. Exact final tests:

```bash
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -L m10 --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /home/nhzj121/build/task_scheduler-m10-asan \
  -R M8Loopback.large_binary_multiple_replenishments_and_paged_cli_are_exact --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /home/nhzj121/build/task_scheduler-m10-asan -L m10 --output-on-failure
```

All returned 0: **473/473 in 74.21s**, **7/7 M10 in 7.84s**, **1/1 affected ASan
M8 in 8.10s**, **7/7 ASan M10 in 9.54s**. All 473 accepted case names and labels
match the pre-cleanup inventory; the new calibration target is a helper, not a
new CTest case. All 36 accepted M9 cases are retained and passed in the full run.
GCC 12's original 30 selected tests also returned 0, followed by the three actual
application cases above. No case was skipped. No production code, frozen
semantics or warning flag changed for this cleanup. Full branch and cleanup
diffs, ownership of temporary artifacts and protected-file hashes were reviewed.

Logs are `m10-tsan-cleanup-final-{configure,build,ctest,m10}.log`,
`m10-tsan-cleanup-asan-{m8,m10}.log` under the established build, and the TSan
runner/applications evidence under the separate GCC 12 tree. Earlier failed
thread-count/calibration attempts are described in G and retained in the cleanup
evidence, not counted as successful integration coverage.

## K. Documentation and contract review

Read/reviewed authority: AGENTS, Architecture Gate, Full Implementation Prompt,
M10 authorization, ARCHITECTURE, OWNERSHIP, STATE_MACHINE, PROTOCOL, FAILURE_MODEL,
DECISIONS, TEST_STRATEGY, PROJECT_SCOPE, DEPLOYMENT, IMPLEMENTATION_PLAN, accepted
M8/M9 reports/demo and relevant live source/tests. The M10 authorization supersedes
the Full Prompt's historical M1-only starting boundary.

README and operator/development/source map/demo/benchmark guides document actual
CLI, finite snapshots, manual retry and security limits. Interview notes retain
frozen tradeoffs and link measured implementation evidence. No frozen decision,
state/transition, message ID/schema, scheduler formula, SQLite schema, process
runtime child path, durability ordering or log-credit/wire barrier was changed.

### Authorization §16 adversarial review

| # | Finding and evidence |
|---|---|
| 1 | Config/CLI/unit paths use real parsers and binaries: examples, CMake configure_file, M10 staged test and real systemd run. |
| 2 | Both default listeners/examples/tools bind loopback; README/OPERATIONS explicitly prohibit untrusted/public use. |
| 3 | CMake never deploys live configs; tools reject unmarked roots; root test removes only exact marked unique leaves/accounts/units. |
| 4 | StateStore/LogWriter root/lock/canonicalization/modes unchanged; M5 and M10 negative tests cover ownership. |
| 5 | All three packaged unit forms ran in real systemd with non-root UID checks, actual state root/masks/signals/timeouts. |
| 6 | Systemd and M9 restart tests prove LOST/FAILED plus retained PENDING/terminal history; no filesystem inference. |
| 7 | Runtime/Worker code unchanged; M9 disconnect churn proves reap/pgid cleanup before new REGISTER and fixed fds/threads. |
| 8 | LogWriter/LogCoordinator code unchanged; M8 barriers/credit/snapshot/partial-wire tests and installed binary output checks pass. |
| 9 | benchmark.measured and cluster.terminal require durable SUCCEEDED, one Attempt, logs_complete and no finalization_pending; verify exact disk/SQLite/downloads. |
| 10 | No daemon log additions; synthetic environment secret stays out of system logs; task data remains separate. |
| 11 | Top-level CMake instrumentation precedes every target; strict flags preserved; no sanitizer suppression or global leak-disable. |
| 12 | ASan/UBSan/LSan full suite plus affected cleanup checks; GCC 12 TSan 30 component and three actual application cases pass; initial failed executions are separately disclosed. |
| 13 | Read-only pinned CI + bounded scripts, real test labels, owned temp teardown; no privileged services or deployment in CI. |
| 14 | Queue/credit saturation, M9 fd/thread/group churn and runtime reaping checks pass; logs have no disk quota and accounting is not enforcement. |
| 15 | Benchmark explicitly includes taskctl/poll delay, same-host Workers, WSL and durable barriers; no claimed scaling causality/SLA. |
| 16 | Source map and CLI inspected; normal/LOST semantics distinguished; no auth/TLS/sandbox/exactly-once/power-loss claim. |
| 17 | Only production change is exception ownership in protocol decoding; no new states, fields, schema, adoption or scheduler logic. |
| 18 | CTest JSON inventory preserves all 473 accepted names/labels; no new CTest case/skip. Native M8 thread assertion stays exactly 3 through a default Harness attribute; TSan-only count is independently calibrated. |

### Authorization §11 definition-of-done

| Item | Evidence |
|---|---|
| 1 clean build/stage | Separate runtime-only Release configure/build/DESTDIR and clean Debug targets; no fresh-OS claim. |
| 2 two Workers | Installed examples, local tool and systemd template instances prove distinct HEALTHY Sessions. |
| 3 empty success | local demo and systemd /bin/true, both durable empty stream files; held M7/M9 states provide intermediate observations too brief to poll otherwise. |
| 4 binary exactness | M10 65,537-byte streams; benchmark exact stdout/stderr downloads, stored files, counters and SQLite; M8 held barriers. |
| 5 infeasible cancellation | local demo CPU100 PENDING cancel with no Attempt; M9 commit gate. |
| 6 active cancel/timeout | local demo, M9 noncooperative groups/first-cause and held terminal barriers; ACK never terminal proof. |
| 7 retry | deterministic missing-directory failure then corrected precondition; exact old Attempt/log history retained. |
| 8 Worker loss/reconnect | systemd running stop/restart; M9 SIGKILL/disconnect, no adoption/automatic retry. |
| 9 Controller ACTIVE restart | real systemd and M9 crash tests, old ACTIVE LOST, PENDING/terminal preserved, new Sessions. |
| 10 paging/snapshots/leaks | M8 paging/fixed-end/partial sends, M9 disconnect/churn and M10 guarded teardown. |
| 11 actual systemd | final complete run described in E, ordinary Worker plus two template instances. |
| 12 repeats/evidence | 140 M10 and 720 M9 repeat executions; sanitizer logs, final raw benchmark and compact evidence. |

## L. Final gate

**M10 implementation/acceptance gates: PASS — awaiting independent review.**

Runtime-only staged install, actual non-root
systemd lifecycle of all unit forms, full 473 regressions/additions, 140 M10
repetitions, 720 M9 fault repetitions, full ASan/UBSan/LSan, GCC 12 TSan's 30
component and three actual Controller/Worker integration cases,
240 correct measured benchmark tasks, documentation and source/ownership review.
No core semantic contradiction or unexplained sanitizer finding remains.

Unperformed/non-guarantees: hosted CI itself, fresh native-machine provisioning,
physical power loss/remount testing, WAN/general scaling, production deployment,
security/isolation and exactly-once are not claimed. These do not replace or
weaken any executed mandatory local gate. The GCC 11 TSan compatibility failure
is disclosed and resolved by actual GCC 12 execution, not suppression/exclusion.

Final delivery requires the branch-only commit/push and remote/main
comparison recorded in A; the final delivery message confirms its observed SHA.
Independent source review remains required; this report
is not authorization to merge, release, deploy or begin subsequent work.
