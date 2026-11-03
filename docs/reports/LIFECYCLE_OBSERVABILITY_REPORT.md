# Lifecycle observability — first-batch implementation report

Date: 2026-11-03 (Asia/Shanghai). Result: PASS, ready for independent review.

## Baseline and scope

- Branch: codex/lifecycle-observability, based on accepted main/origin/main
  124e388665cc2b82db6d1aa212cfb093dc9e4428, verified after fetch.
- Isolated worktree: repository .local-notes/worktrees/lifecycle-observability.
  The original post-v1-benchmark-evidence checkout and its uncommitted README,
  Chinese README and video_demo.sh are not included in this change.
- First batch only: four Controller commit-boundary events, two Worker local
  execution events, tests and event documentation. No Client verbose/watch,
  new Session events, scheduler scoring display, protocol/schema change or new
  logging thread/queue. No runtime/child-after-fork code changed.

## Implementation and files

Modified:

- include/ts/controller/task_manager.h and src/controller/task_manager.cpp:
  optional default-empty observer, typed value snapshots and commit-application
  hooks. Bulk LOST uses a transient diagnostic marker to notify only rows changed
  by that transaction, after reservation release; business Effects remain unchanged.
- include/ts/worker/execution_manager.h and src/worker/execution_manager.cpp:
  optional default-empty observer, confirmed exec/local completion snapshots.
  The existing FINISH result mapping is factored into one helper shared with
  local Session-loss completion diagnostics; invalid Sessions send no new frames.
- src/controller/application.cpp and src/worker/application.cpp:
  existing spdlog INFO output, English single-line fields, full canonical IDs.
- CMakeLists.txt: one component-test executable and six integration registrations;
  every new test carries observability, existing registrations remain intact.

Added:

- include/ts/common/diagnostic_field.h: bounded terminal-safe text scalar encoding.
- tests/component/lifecycle_observer_test.cpp: ten Controller cases and one
  scalar-format case.
- tests/component/execution_observer_test.cpp: six real-runtime observer cases.
- tests/integration/lifecycle_logging.py: six real-application cases using accepted
  M7/M8 helpers, actual loopback TCP, SQLite and existing durability gates.
- docs/LIFECYCLE_LOGGING.md: event boundaries, fields, ownership and limitations.
- This report.

No files removed. These thirteen files are the complete first-batch change.

## Validation commands and results

Environment: Ubuntu 22.04 WSL2, kernel 6.18.54.1-microsoft-standard-WSL2,
GCC 11.4.0, CMake 3.22.1. Formal Debug and ASan builds use separate ext4-backed
directories under /home/nhzj121/build. CMakeCache was checked for the exact isolated
source/build paths. All existing strict GCC flags, including -Werror, remain active.

The following command arguments were executed via Python subprocess arrays to
avoid Windows/WSL shell-variable expansion. They can also be run in a Linux shell:

```bash
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler/.local-notes/worktrees/lifecycle-observability -B /home/nhzj121/build/task_scheduler-lifecycle -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTS_SANITIZER=none
cmake --build /home/nhzj121/build/task_scheduler-lifecycle --target clean
cmake --build /home/nhzj121/build/task_scheduler-lifecycle -j2
ctest --test-dir /home/nhzj121/build/task_scheduler-lifecycle --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler-lifecycle -L observability --output-on-failure --repeat until-fail:20
```

| Step | Exit | Result / wall time |
| --- | ---: | --- |
| Configure | 0 | 2.59 s |
| Clean target | 0 | 0.39 s |
| Warnings-as-errors build | 0 | 103.71 s |
| Full CTest | 0 | 496/496 PASS, CTest 88.12 s |
| Repeated new tests | 0 | 23 cases x 20 = 460 PASS, CTest 109.40 s |

Independent CTest JSON inventories verify all 473 accepted names and labels are
preserved. There are 17 new component cases and six new integration cases. All 36
accepted M9 cases remain present and pass in the full run. Repeat-log parsing
verified exactly twenty passing executions for every new case.

Affected-path instrumentation commands (all exits zero):

```bash
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler/.local-notes/worktrees/lifecycle-observability -B /home/nhzj121/build/task_scheduler-lifecycle-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTS_SANITIZER=address
cmake --build /home/nhzj121/build/task_scheduler-lifecycle-asan -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /home/nhzj121/build/task_scheduler-lifecycle-asan -R '^(M10.sanitizer_runtime_probe|Lifecycle|DiagnosticField)' --output-on-failure
```

24/24 PASS, 7.06 s: all 23 new cases plus runtime probe. Compiler flags confirm
address,undefined instrumentation, with leak detection enabled; actual Controller,
Worker and workload binaries execute in the integration cases. No AddressSanitizer,
UBSan or LeakSanitizer diagnostics. This is affected-path coverage, not a new full
sanitizer matrix. No TSan or physical power-loss validation was performed.

Raw logs/evidence are outside Git under /home/nhzj121/build:

- lifecycle-formal-validation.json: exact argv, per-step exit/status durations,
  correct source/build paths and protected-note hashes.
- lifecycle-formal-{configure,clean,build,full,repeat}.log.
- lifecycle-asan-{configure,build,tests}.log.
- lifecycle-inventory.json: preserved inventory and every new case name.
- lifecycle-final-audit.json: repeat counts, instrumentation totals and final
  process scan. No executable from either lifecycle build remained running.
- Earlier lifecycle-{build,focused}.log retain development checks and corrections.

## Ownership, semantics and acceptance review

| Requirement | Evidence / conclusion |
| --- | --- |
| No success before durable commit | Component cases cover Submit, Assignment, RUNNING, terminal; real file barrier and terminal commit gates retain ACTIVE/reservation and suppress terminal log until released |
| Identity/generation and failure handling | Wrong generation, duplicate/uncommitted notifications, failed assignment, unknown RUNNING/terminal commit; no success event or premature release |
| Terminal visibility/resource release | Observer examines released ledger; real gate test checks reservation before/after commit; bulk LOST does not repeat old terminal rows |
| Snapshots and exceptions | Retained event values survive later mutation; throwing observers preserve Effects, FINISH/START frames, owner cleanup and original final state |
| Process outcomes | Parent cwd preparation failure has no PID; child exec failure has real PID but no START; natural exit, cancel and timeout match existing wire outcome mapping |
| Session loss/recovery | Real Controller disconnect causes Worker local cleanup with old binding; new Controller restores LOST/FAILED/incomplete logs without replaying diagnostic events |
| CLI/task-output compatibility | JSON remains parseable, submit stdout remains a UUID, binary bytes remain exact, private output marker absent from service logs; off suppresses new INFO |
| Lifetime/dependency direction | Synchronous owner-thread callbacks; no references retained by applications, no new fd/thread/queue; TaskManager has no spdlog dependency; no child-path logging |
| Frozen scope | No changes to runtime, protocol, SQLite/storage, log data path, scheduler, Client, business EffectKind, frozen documents or CI |

Reviewed against AGENTS, ARCHITECTURE_GATE, ARCHITECTURE_IMPLEMENTED, OWNERSHIP,
STATE_MACHINE, FAILURE_MODEL, PROTOCOL and applicable TEST_STRATEGY contracts.
Windows Git diff/whitespace review covers tracked changes and new files; the
accepted main remains unchanged. No architecture/semantic deviation identified.

Observer defaults preserve source compatibility. A synchronous slow log consumer
can still delay the owner thread; exceptions are isolated but blocking is not.
Events are best-effort and may be absent; no exactly-once, durable audit or
cross-process total ordering is claimed. Internal runtime failures retain their
existing error path rather than acquiring a fabricated diagnostic outcome.

## Corrections and preserved local data

- Initial new-test compilation found missing braces around GoogleTest macros under
  -Werror=dangling-else; corrected the tests, without weakening warning flags.
- An initial observer test incorrectly expected a nonexistent absolute executable
  to fail before fork. The accepted runtime correctly reports child-side exec
  failure with a PID. Split parent missing-cwd and child missing-executable tests;
  production behavior was not changed to satisfy the erroneous assertion.
- One attempted formal command lost shell-variable path arguments in the
  Windows/WSL handoff and generated an in-source baseline build in the original
  checkout. It was stopped and is not counted as validation. After checking no
  matching build process remained and validating every target path, moved the 66
  newly generated top-level artifacts into the ignored
  .local-notes/build-incident-lifecycle-20261103 directory. Nothing was deleted.
  The original checkout returned to its original three-file uncommitted set.
  Explicit argv arrays and cache-path assertions were used for the complete valid
  clean/build/test/repeat run above. The independently configured ASan run already
  used absolute paths and was unaffected.
- Windows-created worktree metadata uses Windows paths; WSL Git cannot resolve
  that metadata. Repository review uses native Windows Git; all formal compiles
  and tests use Linux GCC and the isolated Linux source path.

Verified unchanged SHA-256 values:

| Protected file | SHA-256 |
| --- | --- |
| Existing build BUILD_COMMANDS.md | 8bda49a0af4e612892bafdba48effb924573e02a6ccea33b3aa8bd5991d813ae |
| Original README.md worktree copy | 6e9c661466d9808d8cf8a122c49a64d942eed60c67b62cd42146cd3163c54f65 |
| Original README.zh-CN.md | f3f362beeb2725bcdfaef8d8c0fd2d2bdb88b018032e3363d4beb35b87c9bcab |
| Original scripts/video_demo.sh | 573ae542b999762f8eef204282dabb93c48d6c9ac9fad545ec4fa5b657f02a75 |
| Existing private reading-notes.md | 4b2e5a53c77198e9278ebdd3cf40be93ec4468d5707435f4729d26b4dabbc882 |

All first-batch acceptance criteria are satisfied. Save this work on its local
feature branch for independent review; do not merge main, push or start the next
batch as part of this delivery.
