# Milestone 10 — Deployment, Hardening, Documentation, and Benchmark

## Explicit owner authorization / Codex development contract

**Authorized milestone:** M10 only, from M10-A through M10-D.

**Status:** Authorized to implement on a dedicated branch; not authorized to merge.

**Repository:** `nhzj121/task-scheduler`.

**Verified accepted `main` baseline:** `8bffdcf302adf31a73aa01926004de217a6f2a46`.

**Accepted M9 implementation:** `1a93ff9d66052a47627fa29d3dfd26651d3f8a35` (ancestor of main).

**Authorized development branch:** `milestone-10-deployment-hardening-benchmark`.

**Source in established WSL workflow:** `/mnt/e/Deck_all_need/projects/work/task_scheduler`.

**Existing out-of-source build:** `/home/nhzj121/build/task_scheduler`.

**Protected build note:** `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md`.

**Review boundary:** implement, commit, push M10 branch, report, and STOP for independent source review. Do not merge or claim final product acceptance yourself.

This contract operationalizes **the accepted, frozen architecture**; it is not a license to alter its semantics. If a requirement conflicts with an actually frozen document, halt that affected work and report the exact conflicting passages and smallest possible alternatives. Do not invent an architecture decision in code or disguise test exclusions as success.

---

## 0. Authority and non-negotiable invariants

Read `AGENTS.md`, `docs/ARCHITECTURE_GATE.md`, `docs/IMPLEMENTATION_PLAN.md`, and this document first. Then read all of:

- `docs/ARCHITECTURE.md`, `docs/OWNERSHIP.md`, `docs/STATE_MACHINE.md`, `docs/FAILURE_MODEL.md`;
- `docs/PROTOCOL.md`, `docs/DECISIONS.md`, `docs/TEST_STRATEGY.md`, `docs/PROJECT_SCOPE.md`, `docs/DEPLOYMENT.md`;
- `docs/spec/Codex_Full_Implementation_Prompt_v1.md`, other accepted specs relevant to the change;
- `docs/reports/MILESTONE_9_REPORT.md`, `docs/M9_LOCAL_DEMO.md`, accepted M8 evidence and live source/test files.

The accepted core is already feature-complete through M9. Do not rewrite the Scheduler, TaskManager, ResourceLedger, protocol framing, StateStore, ProcessRuntime, ExecutionManager, LogWriter, LogCoordinator, or networking architecture for cosmetic cleanup. Necessary narrowly scoped hardening corrections are permitted with exact rationale, regression tests, and no semantic drift.

Preserve all frozen facts:

1. One Controller with one EventLoop decision owner, separate PersistenceThread and LogWriterThread; multiple independent Worker processes; no new general thread pool.
2. C++17, Linux, epoll level-triggered, signed/owned fd cleanup, fork/exec/process-group execution; Controller StateStore uses SQLite WAL and `synchronous=FULL`.
3. Durable assignment before possible RUN, durable cancel intent before CANCEL/accepted response, durable LOST before resource release/replacement ACK, complete log-file + namespace barrier before normal terminal SQLite commit.
4. Task states and Attempt states, Session IDs, Worker identity/generation fences, deterministic projected dominant-utilization placement, no automatic retry and no exactly-once guarantee remain unchanged.
5. One shared Worker session TCP channel for control/logs; M8 byte-and-slot credit is a true capacity reservation; final wire log-order barrier persists.
6. No TLS/authentication/authorization, sandbox, container, cgroups, HA, network-exposed untrusted service, quotas, remote log store, new wire fields, new SQLite schema or new persisted states.
7. Do not silently skip, disable, rename to hide, or weaken an accepted regression or strict warning flag.

M10 is **deployment, hardening, documentation, test tooling, and benchmark**, not a new scheduling or recovery milestone.

## 1. Git and workspace preflight

1. `git status --short --branch`, `git remote -v`, `git fetch origin` (network permission permitting).
2. Verify `main` and `origin/main` contain the accepted M9 implementation and review merge; compare to the above SHA. If main moved independently or is not on that ancestry, STOP and report instead of rebasing an unknown branch silently.
3. Inspect local untracked/ignored and modified files. Preserve `.local-notes/`, private material, all user-owned files and build documentation. **Do not use `git clean -fdx`, recursive deletion, hard reset, force push, or destructive cleanup.**
4. The owner-supplied authorization Markdown may be untracked at `docs/spec/Milestone_10_Authorization_Codex_Development_Prompt.md` on the starting workspace. It is the explicitly permitted preflight exception and may be committed on M10. Do not treat unrelated untracked artifacts as disposable.
5. Create the branch from the accepted mainline, named exactly `milestone-10-deployment-hardening-benchmark`; keep all production work on it. Do not merge to main or auto-merge PRs.
6. Before changes capture tracked regression inventory, full CTest count, build configuration, critical config/schema/protocol file hashes and protected notes hashes as appropriate; never expose secrets.
7. Keep installation staging and temporary service/benchmark data outside production/user directories. Never run a test against an operator's existing `/var/lib/task-scheduler` tree.

## 2. Milestone structure and acceptance checkpoints

| Checkpoint | Authorized deliverables | Exit evidence |
|---|---|---|
| **M10-A — Packaging and deployment** | README, real config examples, `cmake --install` packaging if applicable, service units, start/stop/check scripts, safe loopback/private deployment recipe | Clean-machine build/install, live Controller and two distinct Workers, actual systemd start/stop/restart with independent data root and unprivileged accounts |
| **M10-B — Operational hardening and diagnostics** | Verify data-root/lock/perms, configuration and startup diagnostics, proper system logging/rotation instructions, shutdown/fd/child checks, negative deployment tests | No semantic changes; explicit failures, no double-owner data directory, logs and process cleanup verified |
| **M10-C — Sanitizer/CI/stress** | Reproducible ASan+UBSan/LSan and practical TSan presets/runners, CI or equivalent scripts, repeated real-process fault and bounded-memory validation | Concrete commands, results, environment exclusions justified individually, no unexplained project sanitizer issues |
| **M10-D — Benchmark, docs and project gate** | Reproducible performance harness, raw outputs and analysis, final operator/architecture/technical interview docs, full definition-of-done traceability | Measured throughput/latency/resource figures with experimental context and limitations; A–L acceptance report, no inflated claims |

Implement A–D without asking for intermediate routine approval; request architecture approval only when a frozen contract is genuinely contradictory or missing an essential decision.

## 3. M10-A: distribution and CMake integration

- Inspect existing `CMakeLists.txt`, target graph and compile flags first. Actual existing executables are `controller`, `worker`, `taskctl`; public CLI forms are `controller [--config FILE]`, `worker --config FILE`, `taskctl [--host ADDRESS] [--port PORT] [--json] COMMAND`. **Do not invent new argument flags in docs without implementing and testing them.**
- Provide a supported clean Linux/GCC >= 11/CMake >= 3.20 build/install path. Current required packages include threads, nlohmann_json, SQLite3, spdlog, and GTest for BUILD_TESTING. Document exact package names for supported Ubuntu and build commands, pin/record versions in benchmark and CI outputs.
- Prefer conventional CMake `install(TARGETS ...)` to `GNUInstallDirs` with a configurable `CMAKE_INSTALL_PREFIX` and `DESTDIR` staging. Package only runtime executables, approved config examples, service units and public documentation; never install test fixtures, private data, temp logs or user's build note.
- If adopting `CPack`, distro-specific packaging or extra install targets, keep it minimal, explicitly verify dependency behavior, and do not state that native `.deb`/`.rpm` installation is supported unless actually tested.
- Ensure `BUILD_TESTING=OFF` works in a clean environment without requiring GTest. Preserve all existing project compiler warnings as errors and normal test build behavior.
- Provide runnable example controller/worker JSON using the **already implemented config field names**, not hypothetical new ones. Default listener and Worker target `127.0.0.1:7420` and `127.0.0.1:7421`. Provide worker-a and worker-b with **different stable worker IDs** and practical resource capacities. Keep unique/isolated data roots; do not point an example at committed persistent state.
- Example configs must be syntactically valid, accepted by the actual config parser and usable with installed executables. Document that a same-host multi-Worker topology is independent processes sharing physical CPU/memory, not OS-enforced isolation.
- Explicit multi-host instructions may use RFC1918 private addresses as examples; they must state that authentication/TLS are absent and a strict trusted-network firewall is required. Do not publicly bind listeners by default.
- A minimal script suite should start, check, stop and run a **real** isolated local installation/demo using process IDs, trapping cleanup and checking for orphaned child processes. Never kill unrelated processes by name, blindly truncate databases, or silently ignore a failing return code. CLI argument safety, spaces in filesystem paths, signal interrupts, failed bind/lock and reruns must be tested.

## 4. M10-A: systemd unit correctness and honest validation

- Provide two concrete units: `task-controller.service` and `task-worker.service`. Refer to executable installed paths and checked config paths. Document how multiple Worker instances are configured: e.g. distinct installed unit instances with distinct config files, or separately named units. A single service running two Workers is not a substitute for distinct process/Worker IDs.
- Dedicated unprivileged users are required. The Controller must own its persistent data directory; the Worker needs only the executable and cwd access needed for explicitly trusted tasks. Avoid granting `root`, broad filesystem write access, a false sandbox or a startup shell pipeline.
- Include a restrictive `UMask`, explicit `ExecStart`, appropriate `WorkingDirectory` (only if needed), `Restart=on-failure` and a bounded `RestartSec`; choose and justify a finite `TimeoutStopSec` greater than configured group termination grace plus drain/storage shutdown margin. Set reasonable `LimitNOFILE`, not infinity by default. `StateDirectory`/`LogsDirectory`/`RuntimeDirectory` directives must agree with the *actual* executable config paths and ownership; do not assume systemd creates a directory outside its unit context.
- Account for Controller/Worker SIGINT/SIGTERM handling, Worker cleanup-before-reconnect, Controller fail-stop nonzero exit and startup lock. Restarting Controller necessarily makes previously active Attempts LOST/FAILED, not seamlessly resumed.
- Avoid security unit directives that block arbitrary configured direct executables, cwd access, inherited environment, log/fsync, or process-group signals. Explain the security tradeoffs plainly.
- Provide installation commands and concrete `systemctl daemon-reload`, start, status, stop, restart, `journalctl` steps. Do not run privileged service installation in the user's real system without explicit privilege/environment permission; use an isolated supported test VM or temporary test host/service manager for actual validation.
- **A successful `systemd-analyze verify` is not proof of live start/stop/restart.** Execute real systemd lifecycle tests where systemd exists, including both units and at least two logically distinct Worker service instances/configs. If the present WSL instance lacks a functioning systemd manager or privileges, capture the exact blocker and run syntax/static validation plus an explicitly separate process-level integration test; report live systemd acceptance **NOT VERIFIED**, not PASS, and request owner/environment review rather than invent results.
- Test a stopped Controller's restart while a Worker is running, then verify new Worker Session IDs, no old Attempt adoption, proper FAILED/LOST recovery and no zombie direct child. Use an isolated database. Do not infer physical survival or at-most-once.

## 5. M10-B: data root, local storage and permissions

Review existing production implementations before changing anything. M5/M8/M9 may already meet these constraints; prefer tests and documentation rather than unnecessary rewrites.

- Controller data root must be a known, owned local Linux filesystem path and must not use symlink/path-traversal tricks to escape configured ownership. Validate unsafe paths/ownership/permissions per frozen docs, document canonicalization, restrictive file modes and lock semantics. Reject a competing second Controller before accepting work. SQLite locks and process-level directory ownership are distinct; preserve their correct order.
- Ensure SQLite WAL, FULL synchronous metadata and LogWriter file-plus-directory sync semantics remain intact after any installer/config change. The log namespace/permissions must allow LogWriter to create both empty and nonempty stdout/stderr files and durable directory entries.
- Verify start failure on invalid config JSON, missing config, duplicate keys, failed directory creation, unwritable data directory, unavailable network bind and conflicting Controller ownership. Distinguish expected user errors (clean nonzero) from unexpected internal failures. Do not hide environmental failures through permissive fallback directory choices.
- Run repeated start/stop/restart with fresh and reused isolated data root; assure no fd leak, thread leak, orphaned live Worker direct child or retained pipe registration. Preserve documented fail-stop for metadata errors; do not rewrite a failed transition as success.
- System logs should include operational IDs and useful lifecycle/error context without dumping submitted environment values or raw arbitrary task output. Task stdout/stderr remain separate durable task-log files. Existing `spdlog` stderr/journald sink is acceptable when documented and tested; don't add a file logger merely for appearance. If file output is implemented, specify rotation and lifetime without touching active task-log files.
- Error conditions and operational status should be legible to an operator, but do not add monitoring protocols/telemetry transport/prometheus/gRPC or modify wire schemas. Document disk exhaustion, offline Worker, log backpressure, unsupported unsafe exposure and backup/restore rules.
- Backup guidance must acknowledge SQLite WAL and task-log consistency: the simple supported method is STOP Controller, copy the whole owned data directory consistently, restart. Do not claim coordinated online backup, automatic cleanup, archive, retention or replication.

## 6. M10-B: lifecycle and resource fault cases

Use actual multiple processes where appropriate and existing deterministic fault seams for precise boundaries:

1. CTRL unclean exit, restart and startup recovery with PENDING, ACTIVE, terminal history and logs.
2. Worker clean stop and SIGKILL; distinguish normal child cleanup vs orphan after SIGKILL (test harness owns orphan cleanup).
3. Worker connection loss/reconnect and Session UUID replacement after all owned process groups are cleaned.
4. Simultaneous stop/restart, failed bind, missing/invalid config, wrong user/file permissions, duplicate data root lock; avoid flaky timing assumptions.
5. High-output Worker with bounded Controller queue/log credit; heartbeats, control responses and graceful stop stay serviceable when TCP/log writes stall.
6. `TASK_FINISHED` and final file/directory barrier before SQLite terminal success and reservation release; no premature release when barrier/commit is held.
7. No process-signal handler issuing unsafe direct cleanup on a signal stack; signal-driven shutdown must run on the approved normal owner/event loop.
8. Explicitly explain same-pgid cleanup limits, escaped setsid descendant behavior, unverifiable remote liveness and lack of hard CPU/memory enforcement.

Where an existing accepted M1–M9 test covers a requirement, retain it and provide traceability. Add tests for actual M10 modifications and operator-facing fixtures. Never count reruns of the same test as independent coverage of a missing scenario.

## 7. M10-C: reproducible sanitizer build matrices

Add isolated build configurations/presets or scripts; **do not use the existing Debug build tree for sanitizer output** and never destroy `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md`.

- ASan + UBSan: build **project sources and tests** with `-fsanitize=address,undefined`, suitable frame pointers/debug symbols and no disabling of strict warnings; ensure instrumented executables really run. Run all practical unit, component and process-based tests; record exact tests executed, failure logs, ASAN/UBSAN options, toolchain and elapsed time. Root-cause and fix project findings before declaring a PASS.
- LSan: report whether built into ASan and usable in the fork/exec/integration environment. Distinguish leaks in project code, deliberate still-reachable globals, and tool/runtime limitations. Do not globally suppress leak checks for all tests to achieve nominal green status.
- TSan: use a **separate non-ASan build** with `-fsanitize=thread`; target queue/channel, PersistenceThread, LogWriter and appropriate integrated concurrent cases. TSan should exercise real cross-thread handoffs and eventfd notifications. A Linux runtime incompatible with TSan (e.g. unexpected mapping, supported-on-paper but failing before running test code) requires recorded minimal reproduction and environment diagnostics, not a silent skip. Prefer rerunning in a practical supported Linux host/VM if available.
- Suppressions: strictly scoped, documented third-party/runtime suppressions only; no suppression of project races, no wildcard bypass. Avoid mixing ASan and TSan in a single executable.
- Never claim sanitizer PASS if the test executable did not execute. Report number attempted/passed/failed/skipped separately for each matrix, with explicit exclusions for incompatible fork-based harnesses.
- If a required sanitizer cannot execute in the available WSL kernel or due to missing privileges/dependencies, retain the gate as **environment-limited / pending independent review**. Preserve all baseline tests and continue feasible deliverables, but do not self-certify full M10 acceptance.
- CMake sanitizer flags must propagate consistently to relevant link targets and test helpers; avoid silently using non-instrumented prebuilt project libraries.

## 8. M10-C: CI and repeatability

- Add CI workflow(s) for supported Linux, or equivalently runnable repo scripts where actual hosted CI cannot be configured; prefer GitHub Actions with modern pinned versions or SHA-locked actions if project norms require. Use a C++17/GCC supported matrix and install known CMake/SQLite/spdlog/nlohmann_json/GTest dependencies.
- CI must use clean independent build trees; warnings-as-errors; full CTest; isolated ASan/UBSan and practical TSan jobs; bounded test timeouts. Avoid exposing listeners outside loopback and avoid making privileged `systemd` tests run on unsupported hosted runner images as though they were ordinary integration tests.
- Prefer deterministic test seeds, per-test isolated `/tmp`/Linux-local paths, unique dynamic loopback ports, and faithful failure artifacts. CI should not execute destructive `git clean` or delete user data from mounted workspaces.
- Establish a repeat/stress suite for actual task submission/terminal/logs plus failure/reconnect, covering 20 consecutive M10-labelled runs where practical. Record exact invocations, number of cases per run, aggregate failures and duration. Large-scale/high-load tests should be bounded and opt-in.

## 9. M10-D: benchmark harness and interpretation

**The benchmark measures the existing architecture; it must not add an optimization that changes scheduler, persistence, protocol or durability semantics.**

- Provide an executable reproducible script/driver and separate benchmark-specific isolated Controller/Worker configurations. Use a controlled Linux local filesystem for SQLite/WAL/logs; do not benchmark with an unrelated real data directory. Report `uname -a`, CPU model/logical cores, memory, kernel, filesystem type and mount, WSL/native/VM, GCC/CMake/SQLite, build type, thread counts, Worker capacities, queue config, network topology and task program.
- Define workload(s) precisely: `/bin/true` zero-log end-to-end baseline; deterministic fixed-size stdout/stderr binary workload with exact log checks; optional bounded short-sleep/concurrency workload; optionally compare 1, 2 and N Workers **with shared-host caveat**. No shell invocation is implied unless explicitly part of a measured task.
- Define measured phases unambiguously. For example, submission latency = start of taskctl/TCP request to durable submit response; end-to-end latency = request issuance to observed terminal durable Task state; throughput = number of successfully terminalized tasks divided by measured elapsed wall time including applicable barriers. Separate client process startup overhead from server-side measurement or state clearly when it is included. `PENDING/ACTIVE` response alone is not completed throughput.
- Warm-up and reset policy must be explicit. Perform at least three independent measured repetitions per primary scenario (more if cheap), fixed task counts and bounded run duration, report each run, median, min/max and optionally p50/p95 with sample-size caveat. Never fabricate measurements or round away outliers without disclosure.
- Collect bounded resource indicators: Controller and Workers CPU time, peak RSS (from `/proc`/`time -v` with caveats), fd counts, threads, bytes of log data, successes/failures, tail counters, and any baseline backpressure/restart effects. Avoid intrusive tracing unless reproducibly measured.
- Ensure response and task ID correlation. Verify exact Task terminal distribution, Attempt history and log bytes, report missing results and timeouts as failures, not silently discard. Be explicit about manual retry and loss ambiguity.
- Include a benchmark report (`docs/benchmarks/M10_BENCHMARK_REPORT.md` or clear equivalent) with raw CSV/JSON or reproducible log paths, scripts and example plotting/summary commands. Do not commit large generated datasets, SQLite database files, debug logs or host-identifying secrets. If actual benchmark cannot run, deliver scripts and mark numbers **not measured**.
- Explain architectural bottlenecks based on evidence: single EventLoop, single SQLite writer, single LogWriter, synchronous durable commit/fdatasync+fsync, JSON/base64 and per-task fork/exec. Correlation is not causation; don't advertise throughput or resource isolation as industry-grade.
- Compare one Controller + one Worker against multi-Worker and workload shape only if actually measured and reproducible; avoid irrelevant outside-system performance comparisons.

## 10. M10-D: final project documentation

Create or substantially improve a root `README.md` (missing in the inspected accepted mainline) and supporting docs. The deliverable should let a new developer/operator reproduce the system without reading the entire milestone history first. Suggested docs (names can adapt to repository layout):

1. `README.md` — concise v1 purpose, architecture diagram, supported platform, build/test, first local demo, CLI, project structure, safety boundary, guarantees/non-guarantees and links.
2. `docs/OPERATIONS.md` — safe deployment with two Workers, systemd lifecycle, config schema/validation, required dependencies, local filesystem data root, diagnostics, backup/restore, disk/fd capacity, failure-mode troubleshooting and privilege considerations.
3. `docs/DEVELOPMENT.md` — CMake layout, clean/off-tree builds, important interfaces, Test Strategy labels and how to run deterministic fault-injection and sanitizers, strict build flags.
4. `docs/ARCHITECTURE_IMPLEMENTED.md` or verified section — production ownership/dataflow map from client request through Controller state, SQLite, Worker ProcessRuntime, log credit and barrier to terminal visibility. Diagram must match actual source and not override frozen design.
5. `docs/INTERVIEW_NOTES.md` (or equivalent) — precise engineering decisions and their tradeoffs: one owner EventLoop, effect gates, metadata/log split durability, eventfd notifications, exact ratio placement, process group cleanup, cancellation races, LOST vs FAILED and retry, backpressure, security boundaries.
6. `docs/benchmarks/M10_BENCHMARK_REPORT.md` — collected measurements with methodology and configuration (see §9).
7. `docs/reports/MILESTONE_10_REPORT.md` — final A–L acceptance evidence with exact local/remote commit SHAs and explicitly unverified criteria.

Synchronize all usage commands with **actual** `taskctl` syntax. Current command family includes `submit`, `get`/`status`, `list`, `workers`, `logs`, `cancel`, `retry`. In particular, `logs ... --both` concatenates independently retrieved stdout/stderr snapshots; it does not implement a merged timestamp-ordered live stream. Document `--json` response behavior and explicit no follow mode.

Do not claim trusted local Linux file `fdatasync`/directory `fsync` ordering gives cross-machine HA. Distinguish task-log data from spdlog system diagnostics. Explain lack of authorization/TLS/sandbox/cgroups and risk of executing arbitrary trusted user commands.

## 11. Mandatory real definition-of-done demonstration

On a supported Linux host/WSL environment using isolated configurations and local Linux filesystem:

1. Configure/build from a clean checkout and stage/install executables.
2. Start Controller and **two uniquely identified** Workers; verify both HEALTHY using `taskctl workers` and by real Session state.
3. Submit `/bin/true` and inspect real durable PENDING/ACTIVE/STARTING/RUNNING/terminal observations where timings permit (short stages may be too brief for polling), then read both zero-length durable logs.
4. Submit a deterministic binary stdout/stderr workload and compare downloaded bytes, counters and stored files; check no terminal visibility before barrier.
5. Submit CPU request infeasible for registered Workers, verify PENDING; cancel it durably with no Attempt.
6. Submit long-running workload, cancel, confirm `accepted/ACTIVE` then later terminal CANCELLED after process cleanup and log barrier; add timeout case mapping TIMED_OUT/FAILED.
7. Force failure/nonzero task; manual retry after correcting deterministic precondition; verify new Attempt identity and immutable prior log/history.
8. Kill or disconnect Worker during RUNNING, verify LOST/FAILED and no auto retry; on fresh session reconnect verify no old process adoption. Distinguish SIGKILL orphan test harness ownership.
9. Restart Controller while ACTIVE; verify restart LOST transaction, PENDING and old terminal history, no restoration of old session/reservations.
10. Verify pagination and fixed-end log snapshots with exact bytes, client disconnect behavior and no obvious fd/zombie leaks.
11. Demonstrate separate actual systemd stop/start/restart as required by M10-A on a functioning manager, not just foreground processes.
12. Repeat with fixed seeds where applicable and report tested commands, return codes and environment.

Do not make a scripted demo rely on arbitrary sleeps as the only proof. Use polling with bounded deadlines and inspect actual SQLite/wire/task facts where needed. Run in a clean temporary directory; teardown must stop only owned processes, preserve failure artifacts and not delete user data.

## 12. Security and operational truthfulness checklist

- Default bind: loopback only. Never present it as safe for public Internet.
- Credentials: none. Do not add fake token/password or imply encryption.
- Worker execution: arbitrary trusted direct executables/argv; inherited service environment matters. Non-root reduces privileges, not a sandbox.
- Resources: advertised accounting and max concurrency, not enforced actual per-process CPU/memory.
- Failure: LOST does not mean the remote OS process definitely stopped. No exactly-once or automatic retry; manual retry may overlap unknowable remote work.
- Capacity: bounded in-memory queues; task logs have no retention/quota; storage-full is a failure, not permission to drop data.
- Durability: SQLite WAL FULL metadata plus synchronized local task-log files and containing directories. No cross-medium ACID, replication, online backup guarantee or power-loss test by default.
- Network: one Controller, private network, dedicated workers; cgroup isolation, Kubernetes, metrics services and public cloud scaling explicitly outside v1.

## 13. Acceptance testing matrix (must report exact commands and outcomes)

| Evidence category | Required checks |
|---|---|
| M0–M9 regressions | Maintain all **466** accepted tests from M9 and labels; no renaming/skipping to hide failure |
| M10 unit/component | Installed config paths, executable discovery, parse failures, lock/path checks, defaults/permissions, logging and new code paths |
| M10 real-process integration | Fresh staged install, two workers, start/stop/restart, pending/success/cancel/timeout/retry/reconnect, logs and SQLite recovery |
| systemd | Real system manager start, stop and restart of Controller and Worker service(s); independent non-root users, exact paths and permissions |
| ASan + UBSan (+ LSan where usable) | Instrumented project tests and practical live integration; no unexplained project errors |
| TSan | Instrumented queue/channel/owner-thread cases and practical multi-threaded integration; genuine runtime compatibility assessment |
| Fault/hardening | Locked data root, invalid bind/config/perms, fault/shutdown, no fd/zombie leaks, no premature terminal release |
| Repeat | All M10-labelled tests at least 20 successful consecutive runs or explicitly bounded practical subset with objective reason; report separate stress matrix |
| Benchmark | Real commands, raw data, workload correctness, configuration, warmup/repeats, percentiles with limitations, measured failure rates |
| Final documentation | Accurate CLI instructions, security/non-goals, tested service instructions, source/dataflow map and full v1 definition-of-done checklist |

If environmental constraints prevent a mandatory live systemd or sanitizer test, produce honest **NOT VERIFIED** evidence instead of fabricating PASS. A milestone that misses a required acceptance cannot be declared unconditional PASS; report exact outstanding gate and request independent environment review. This does not authorize changing the frozen Test Strategy to force a green label.

## 14. Approved WSL build commands (non-destructive)

```bash
set -euo pipefail
SRC=/mnt/e/Deck_all_need/projects/work/task_scheduler
BLD=/home/nhzj121/build/task_scheduler
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$BLD" --target clean
# Clean the CMake targets only; NEVER delete this user-owned note.
test -f "$BLD/BUILD_COMMANDS.md"
cmake --build "$BLD" -j4
ctest --test-dir "$BLD" --output-on-failure
ctest --test-dir "$BLD" -L m10 --output-on-failure --repeat until-fail:20
```

The final `-L m10` command is valid only when the M10 label is actually populated by real tests; a zero-test run is NOT PASS. Record `ctest -N`, count, labels, elapsed time, test logs and any failures. New sanitizer build directories must be created separately; show commands from the committed presets/scripts and preserve all configured warning flags.

Check `cmake --install` into temporary staging, then actually run staged executables, not just build-tree binaries. Test without GTest via `BUILD_TESTING=OFF` for runtime installation. Avoid using `/mnt/e`/DrvFs as Controller durability storage in the formal validation; use verified Linux-local filesystem.

## 15. Benchmark publication gate

Before accepting any performance result, independently inspect:

- benchmark script(s), workload generator and original result data;
- per-run task counts, terminal successes, failures, timeouts and tail verification;
- time source and start/stop timing boundaries; wall-time throughput denominator;
- warmup, distinct repetitions and statistical treatment; client spawn overhead where relevant;
- machine/kernel/FS/CPU/context and Worker logical vs real resource sharing;
- whether measured `SUCCEEDED` includes complete log barrier and SQLite commit;
- whether pressure changes memory usage/latency or merely stalls completion;
- whether process/fd cleanup, Controller resource accounting and stop are correct after the run.

Use report tables/charts if useful, but preserve raw values. If benchmarks show poor results, report them without changing semantics or omitting a failed scenario.

## 16. Mandatory pre-merge adversarial source review

Answer, with filenames/functions/tests, at least these questions:

1. Do installed, service and example configs use the real parser and actual CLI, with consistent paths/users/ports?
2. Can any default bind or example inadvertently expose unauthenticated remote execution beyond loopback?
3. Can stale installation scripts remove, overwrite, chmod or leak an operator's existing database/log directory?
4. Does the lock, root canonicalization and file-permission setup preserve existing SQLite/log durability and single-Controller ownership?
5. Do systemd `ExecStart`, directory directives, masks, signals and stop timers work in a real manager without running as root?
6. On Controller restart, is active work still LOST and is terminal history never fabricated from filesystem artifacts?
7. During Worker stop/reconnect, does an old process group remain owned until reaped, and are new sessions unique and clean?
8. Do LogWriter credit, snapshots, FINISH wire ordering and namespace sync remain untouched/correct under installed configuration?
9. Can a benchmark/test falsely count submission ACK or a `finalization_pending` task as a completed successful Task?
10. Does a system log format accidentally print arbitrary task env secrets, arguments, raw output or authentication-like data? (No authentication exists.)
11. Are sanitizer builds actually instrumenting all relevant project targets, and do suppressions conceal only third-party limitations?
12. Is TSan/LSan execution genuinely verified or explicitly environment-limited, without reporting a skipped matrix as PASS?
13. Do CI/test scripts preserve warnings-as-errors, run tests with real labels, and avoid destructive cleanup or privileged/Internet-exposed services?
14. Do memory, fd, thread and child limits remain bounded under high log/network pressure and shutdown?
15. Are benchmark metrics honest about WSL, shared-host Workers, synchronous SQLite/log durability, and untrusted workloads?
16. Are README, operator docs, interview notes and benchmark claims faithful to current production code and frozen non-goals?
17. Did any M10 convenience code introduce a new durable state, wire field, auto-retry, work adoption, plugin/network stack or scheduler behavior?
18. Have the original 466 tests and their labels been preserved, with modifications explicitly justified and compared?

## 17. STOP conditions and escalation

STOP the affected implementation and report rather than silently improvise if any of the following occurs:

- Git main is not the accepted M9 merge ancestry, or protected/unrelated changes cannot be safely preserved;
- a real conflict exists between frozen architecture/ownership and proposed packaging/hardening behavior;
- an implementation would require TLS, auth, cgroups, new protocol/schema/states or other explicitly excluded feature;
- an essential required service/sanitizer test cannot actually execute in the present environment; mark it as unresolved acceptance rather than invent success;
- a sanitizer finds project memory safety, race or leak errors that remain unexplained;
- a clean build/regression fails and cannot be fixed within approved M10 scope;
- the only feasible service setup would modify the user's non-isolated real system without permission;
- benchmark cannot verify real terminal/log correctness or doesn't retain trustworthy timing inputs.

For an environmental limitation, complete all safe unaffected M10 work; report the missing criterion precisely. Do not announce an unconditional PASS unless the contract is satisfied.

## 18. Suggested commit plan

- `m10: add reproducible deployment configuration and staged install`
- `m10: add systemd units and lifecycle validation`
- `m10: harden runtime operational diagnostics and negative deploy tests`
- `m10: add sanitizer and CI/stress presets`
- `m10: add benchmark harness and reproducible evidence`
- `docs: publish final operator, architecture and project acceptance documentation`

These are suggestions, not permission to commit failing or misleading work. Record the exact before/after file inventory and dependencies. A changed lower-layer production file requires a specific explanation and regression.

## 19. M10 final A–L acceptance report contract

Create `docs/reports/MILESTONE_10_REPORT.md`, with explicit sections:

- **A. Git/baseline and workspace protection:** exact main/M9/M10 SHAs, branch ancestry, behind/ahead, protected-file integrity, no main changes.
- **B. Implemented M10-A–D:** concrete deliverables and limitations, no M11 invented.
- **C. File inventory/dependency review:** full adds/modifies/deletes; any lower-layer changes justified.
- **D. Packaging/build/deployment:** precise install paths, staged clean-build/install and app smoke tests.
- **E. systemd lifecycle:** actual service commands, environment, dedicated accounts, stop/restart observation, limitations.
- **F. Operational hardening:** root/lock/perms, signal shutdown, diagnostic and negative fault evidence.
- **G. Sanitizers/CI:** ASan/UBSan/LSan/TSan instrumented build commands, exact counts, results and justified exceptions.
- **H. Stress/failure/ownership:** full real-process fault and fd/zombie/queue matrix.
- **I. Benchmarks:** raw data refs, methodology, workloads, measured distributions and non-guarantees.
- **J. Formal build and test execution:** full CTest inventory including 466 regressions, new cases, repetitions, timing, failure/correction history.
- **K. Documentation/architecture audit:** README, operator/interview guides, frozen contract consistency and unresolved limitations.
- **L. Final gate:** PASS only if all mandatory checks are genuinely met; otherwise NOT PASSED / ENVIRONMENT-LIMITED with exact blockers and explicit independent review needed.

Report a measured fact only when source or execution evidence actually supports it. Do not claim production deployment readiness, security, SLA, power-loss durability or exactly-once guarantees.

## 20. Delivery and authorization close

After validating the complete branch:

1. Run `git diff --check main...HEAD` and inspect every added/modified file. Verify no large generated databases/build files, proprietary information or credentials are committed.
2. Verify every test name/label, CMake warning flag and frozen contract. Check exact local/remote main SHA and M10 HEAD.
3. Commit authorized M10 code/docs/tests only on `milestone-10-deployment-hardening-benchmark`, push branch and verify remote SHA equality.
4. Preserve `.local-notes/`, `/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md`, existing data and unrelated git state.
5. Produce the full A–L report, verified clean-machine installation and real systemd evidence, sanitizer/stress/benchmark artifacts, and a concrete copy/paste local demo.
6. **STOP**. Do not merge M10 into main, tag a release, publish packages, trigger deployment, or begin new scope without explicit independent review and user approval.

If all required acceptance criteria have actually passed, conclude exactly:

**Milestone 10: PASS — awaiting explicit independent review; no subsequent milestone has been started.**

If one or more mandatory gates were not verified, conclude:

**Milestone 10: NOT PASSED / ENVIRONMENT-LIMITED — precise outstanding gates listed; awaiting independent review.**

**This document is explicit authorization to implement M10-A through M10-D on the M10 branch, and nothing beyond that.**
