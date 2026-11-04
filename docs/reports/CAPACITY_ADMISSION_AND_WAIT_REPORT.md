# Capacity admission and taskctl wait — implementation report

Date: 2026-11-04 (Asia/Shanghai). Result: PASS, ready for independent review.

## Baseline, authorization and scope

Accepted main/origin/main after fetch:
73899442b139758112e25d8d2c2b3f9ca25b9aa4. Work is isolated on
codex/capacity-admission-and-wait under .local-notes/worktrees/capacity-admission-and-wait.
The original checkout stays on main with its existing README changes, Chinese
README and video_demo.sh. No push or merge is included.

The explicit development authorization approves a post-v1 admission policy
change: zero known Worker advertisements rejects, OFFLINE last activated
advertisements count, and FAILED retry is checked too. ADR-024/025 and relevant
scope/protocol/architecture documents record that change. Task/Attempt states,
transitions, durable ordering, placement and wire schemas/IDs remain unchanged.

## Implemented behavior and API

Controller checks a read-only TaskManager capacity_admission query before UUID
generation, retained Task budget charging or Submit persistence. A single known
node must fit total CPU and memory together. Occupancy, max concurrency and health
do not affect admission. Pending replacement advertisements do not contribute.
Internal owner/registry invariant failures remain internal errors.

NO_KNOWN_WORKERS and NO_KNOWN_WORKER_FITS use existing INVALID_ARGUMENT,
retryable=false and details.reason plus requested_cpu_slots, requested_memory_mb,
known_worker_count. Submit rejection creates no rows; retry checks Task existence
and FAILED first, then rejects without changing history/versions/cancel intent.
No existing/recovered PENDING Task is retroactively rejected or cancelled.

taskctl submit/retry return 3 only for a typed, recognized capacity rejection.
Plain failure has empty stdout and a stderr diagnostic; JSON keeps its existing
error-object output. Successful UUIDs and other command/error behavior remain.
An absent submit response still means outcome may be unknown, not a proved rejection.

taskctl wait TASK_ID [--timeout SEC] polls existing GET_TASK immediately then one
second after each nonterminal reply, reusing one connection. It verifies Task ID,
uses Task state rather than historical Attempt state, and prints one final result.
Text marks latest_recorded_attempt_id explicitly; JSON retains the full final
GET_TASK_RESPONSE. It downloads no logs and makes no mutation requests.

Wait exit codes: 0 SUCCEEDED; 1 FAILED/CANCELLED; 2 wait-local arguments/query/
protocol/transport errors; 124 whole observation deadline; normal shell SIGINT
status 130 (subprocess observes signal termination -SIGINT). Existing global
option parsing and other command exit handling remain unchanged.

Client connect/request gained a default-nullptr borrowed Deadline parameter.
Its monotonic at and expired flag are caller-owned only during synchronous calls,
never retained by callbacks. Old calls are source-compatible. Requests keep their
normal five-second timeout; the earlier timeout wins, including late OS wakeups.
Only true overall-budget expiry sets the flag. No reconnect/automatic retry,
background thread, subscription, cancellation or richer error framework was added.

See [usage and contracts](../ADMISSION_AND_WAIT.md).

## Files changed

Production and build:

- include/ts/controller/task_manager.h, src/controller/task_manager.cpp:
  read-only typed admission result, known-advertisement checks.
- src/controller/application.cpp: pre-submit/pre-retry client-boundary enforcement.
- include/ts/client/client.h, src/client/client.cpp: optional synchronous deadline.
- src/apps/taskctl.cpp: capacity-error classification, wait parsing/output/exit codes.
- scripts/local_cluster.py: specification-valid blockers demonstrate PENDING
  cancellation, then cancel/drain their managed tasks.
- CMakeLists.txt: new tests and installation of the new guide alongside existing guides.

Tests:

- New tests/component/capacity_admission_test.cpp (4 cases).
- New tests/component/client_deadline_test.cpp (6 cases).
- New tests/integration/admission_wait.py (10 cases).
- tests/integration/m7_loopback.py and m9_loopback.py: explicit accepted OFFLINE
  advertisement setup where old fault scenarios need PENDING without a live executor.
  Original durability/crash/history assertions and names remain; the Worker-list
  assertion reflects the added OFFLINE fixture rather than expecting an empty list.
- tests/integration/m10_deployment.py: assert the new guide is actually installed.

Docs: ARCHITECTURE.md, DECISIONS.md, DEVELOPMENT.md, PROJECT_SCOPE.md, PROTOCOL.md,
STATE_MACHINE.md, TEST_STRATEGY.md, new ADMISSION_AND_WAIT.md, and this report.
Twenty-three files changed/added, none removed. No changes to Worker, runtime,
storage, log data path, scheduler algorithm, protocol implementation or CI.

## Exact formal validation

Ubuntu 22.04 WSL2, kernel 6.18.54.1-microsoft-standard-WSL2; GCC 11.4.0,
CMake 3.22.1. Both build trees are under ext4-backed /home/nhzj121/build.
Commands were executed with explicit Python subprocess argument arrays and
verified CMakeCache source/build paths, avoiding shell path-variable expansion.
All strict GCC flags including -Werror, -Wconversion, -Wsign-conversion and
-Wshadow remain active.

```bash
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler/.local-notes/worktrees/capacity-admission-and-wait -B /home/nhzj121/build/task_scheduler-admission-wait -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTS_SANITIZER=none
cmake --build /home/nhzj121/build/task_scheduler-admission-wait --target clean
cmake --build /home/nhzj121/build/task_scheduler-admission-wait -j2
ctest --test-dir /home/nhzj121/build/task_scheduler-admission-wait --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler-admission-wait -L admission_wait --output-on-failure --repeat until-fail:20
```

| Formal step | Exit | Result / time |
| --- | ---: | --- |
| Configure | 0 | 2.49 s |
| Clean | 0 | 0.47 s |
| Strict build | 0 | 99.27 s |
| Full CTest | 0 | 516/516 PASS, CTest 89.80 s |
| New cases repeated | 0 | 20 cases x 20 = 400 PASS, CTest 344.25 s |

CTest JSON inventories confirm all accepted 496 names and labels remain, with
20 new admission_wait cases (10 component, 10 integration). Parsing the repeat
log confirms exactly twenty successful executions of every new case, not merely
twenty total tests. All 36 accepted M9 fault cases pass.

```bash
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler/.local-notes/worktrees/capacity-admission-and-wait -B /home/nhzj121/build/task_scheduler-admission-wait-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTS_SANITIZER=address
cmake --build /home/nhzj121/build/task_scheduler-admission-wait-asan -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /home/nhzj121/build/task_scheduler-admission-wait-asan --output-on-failure
```

All exits zero: sanitizer configure 4.58 s, build 148.15 s, full CTest
516/516 PASS in 131.67 s. Actual project libraries/applications/helpers are built
with address,undefined instrumentation; leak checking is enabled. No ASan, UBSan
or LSan diagnostics. No TSan or physical power-loss validation was performed.

After those runs, a documentation-only installation correction added the guide
referenced by DEVELOPMENT.md to the existing install list and strengthened its
staged-install assertion. Both trees were reconfigured with the same commands,
then the affected M10 suite reran:

```bash
ctest --test-dir /home/nhzj121/build/task_scheduler-admission-wait -L m10 --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /home/nhzj121/build/task_scheduler-admission-wait-asan -L m10 --output-on-failure
```

Both exits zero: Debug 7/7 in 7.59 s, ASan 7/7 in 9.64 s. No production source
changed after full/repeated/sanitizer validation.

## Acceptance and source review

| Contract | Evidence |
| --- | --- |
| Single-node bounds / no resource summing | Joint CPU/memory counterexamples, exact boundaries, busy tentative/committed capacity and full concurrency cases |
| OFFLINE / replacement / restart | Last activated advertisements remain admissible offline; uncommitted replacements excluded; restart has no known advertisements; old PENDING survives and executes after registration |
| No rejection side effects | Repeated rejected submit leaves Task/Attempt counts zero and reservations zero; source gate precedes UUID/budget/persistence; rejected retry compares every persisted Task/Attempt field |
| Error/CLI compatibility | Plain/JSON errors, both stable reasons, reason type safety, dedicated status, and raw submit disconnect remaining unknown/status 1 |
| Wait truthfulness | Real success, nonzero exit, timeout, cancel, LOST/incomplete logs; PENDING ignores old FAILED history; wrong Task ID rejected |
| Durability ordering | Existing actual file and terminal-commit gates keep Task ACTIVE/reservation allocated and wait alive until release |
| Timeout / interruption | Past connect/request budgets send no request; pending query budget, normal timeout and transport error distinguished; real wait timeout/SIGINT do not cancel; repeated expired calls do not grow fds |
| Lifetime / concurrency | Synchronous borrowed deadline, one outstanding request, no new production threads/fds/queues, registry access on owner thread |
| Frozen contracts | No state/transition enum, numeric message ID, wire schema, SQLite schema, scheduling comparator, log-credit/barrier, Worker/process lifetime changes |

Reviewed AGENTS, STATE_MACHINE, ARCHITECTURE, OWNERSHIP, PROTOCOL, FAILURE_MODEL,
TEST_STRATEGY and accepted M7-M10 interfaces. The intentional admission revision
is documented, with no additional architecture deviation or unresolved blocker.
Native Windows Git handles repository review because the Windows worktree metadata
contains Windows paths; compilation and all acceptance tests run on Linux GCC.

## Development corrections and retained evidence

Initial compile checks corrected a Result error-message accessor and a test using
operator!= on an ID type that exposes operator== only; warnings were not weakened.
The first full development run had ten failing old tests whose submissions
preceded any advertisement; one assignment subtest also continued into a follow-on
bind failure after its first setup failed. Explicit real OFFLINE advertisements
replace those incompatible assumptions. No original fault assertions were removed.
The next full run passed 515/515 before the additional fd-lifetime case and final
deadline review. Final source handles late wakeups by choosing the earlier
deadline. These development runs are not substituted for final acceptance.

Logs and machine-readable evidence remain outside Git under /home/nhzj121/build:

- admission-formal-validation.json and admission-formal-{configure,clean,build,full,repeat}.log.
- admission-asan-validation.json and admission-asan-{configure,build,full}.log.
- admission-doc-validation.json and admission-doc-{debug,asan}-{configure,tests}.log.
- admission-inventory.json: all added names and preservation check.
- admission-final-audit.json: per-case repeat counts and no-live-application scan.
- admission-initial-full.log, admission-second-full.log and admission-focused.log:
  development results/corrections retained.

No application executable from either new build tree remained live after
validation. Existing BUILD_COMMANDS.md SHA-256 before/after:
8bda49a0af4e612892bafdba48effb924573e02a6ccea33b3aa8bd5991d813ae.
Original checkout files remain byte-identical:

| File | SHA-256 |
| --- | --- |
| README.md | 6e9c661466d9808d8cf8a122c49a64d942eed60c67b62cd42146cd3163c54f65 |
| README.zh-CN.md | f3f362beeb2725bcdfaef8d8c0fd2d2bdb88b018032e3363d4beb35b87c9bcab |
| scripts/video_demo.sh | 573ae542b999762f8eef204282dabb93c48d6c9ac9fad545ec4fa5b657f02a75 |
| .local-notes/reading-notes.md | 4b2e5a53c77198e9278ebdd3cf40be93ec4468d5707435f4729d26b4dabbc882 |

Known limits are intentional: admission uses in-memory advertisements, not OS
resource enforcement or future availability guarantees; restart temporarily
rejects new work until registration; wait fails on communication errors without
reconnect; polling can miss a FAILED-and-retry interval; deadlines are not
hard-real-time guarantees. All approved acceptance criteria are satisfied.

Delivery is one local feature-branch commit for independent review. No push,
main merge, release or subsequent feature work is authorized by this report.
