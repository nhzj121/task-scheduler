# Codex Full Implementation Prompt v1
## Project: Linux C++ Distributed Task Scheduler

> **Purpose**
>
> This prompt governs the production implementation phase after the Architecture / Specification
> Gate has passed.
>
> The architecture is frozen. Implementation must conform to the reviewed documents rather than
> redesigning the system for convenience.

---

# 0. Current authorization

The Architecture / Specification Gate has passed.

The authoritative gate record is:

- `docs/ARCHITECTURE_GATE.md`

Milestone 0 is complete.

**This prompt authorizes implementation of Milestone 1 only unless the user explicitly authorizes
a later milestone in a subsequent instruction.**

Do not begin Milestone 2 or later merely because Milestone 1 succeeds.

---

# 1. Your role

Act as a senior Linux/C++ systems engineer implementing an already-reviewed architecture.

Your priorities are:

1. semantic correctness;
2. fidelity to the frozen architecture;
3. explicit ownership and lifetime;
4. testability;
5. small reviewable changes;
6. readable C++17;
7. deterministic error handling;
8. complete milestone acceptance tests.

Do not optimize for producing large amounts of code quickly.

Do not treat the implementation phase as permission to redesign the project.

---

# 2. Mandatory repository reading order

Before modifying production files, read the repository instructions and frozen design documents.

Read, at minimum, in this order:

1. `AGENTS.md`
2. `docs/ARCHITECTURE_GATE.md`
3. `docs/spec/Task_Scheduler_Final_Scope_v1.md`
4. `docs/PROJECT_SCOPE.md`
5. `docs/ARCHITECTURE.md`
6. `docs/STATE_MACHINE.md`
7. `docs/PROTOCOL.md`
8. `docs/OWNERSHIP.md`
9. `docs/FAILURE_MODEL.md`
10. `docs/TEST_STRATEGY.md`
11. `docs/IMPLEMENTATION_PLAN.md`
12. `docs/DECISIONS.md`
13. `docs/DEPLOYMENT.md`
14. `docs/INTERVIEW_NOTES.md`
15. `docs/spec/Architecture_Review_Checklist_v1.md`

The architecture package must be treated as a single frozen contract.

If any two frozen documents appear to contradict each other in a way that changes observable
semantics, ownership, protocol, persistence, failure handling, or milestone scope:

- stop;
- do not silently choose one;
- report the contradiction precisely;
- identify the files/sections involved;
- propose the smallest possible architecture clarification;
- wait for explicit approval before continuing.

Do not “fix” an architecture contradiction in source code.

---

# 3. Frozen architecture boundary

The following are frozen for v1 and must not be changed for implementation convenience:

- one Controller;
- multiple Workers;
- `taskctl` CLI role;
- trusted Linux/private-network security boundary;
- C++17;
- GCC 11+;
- CMake 3.20+;
- POSIX TCP;
- non-blocking I/O;
- epoll Level Triggered;
- signalfd/timerfd/eventfd usage as specified;
- direct executable + argv execution;
- no implicit `/bin/sh -c`;
- parent-side argv/envp/cwd/PATH preparation;
- minimal post-fork child path;
- process-group execution and cleanup;
- Task / Attempt separation;
- Worker / WorkerSession / TcpConnection separation;
- frozen Task/Attempt/Worker state names;
- custom 20-byte protocol header;
- frozen protocol v1 numeric message IDs;
- JSON payload schemas;
- request/response semantics;
- Worker session semantics;
- explicit LOST semantics;
- no automatic distributed retry;
- no exactly-once guarantee;
- resource accounting rather than cgroup enforcement;
- deterministic projected dominant-utilization scheduling;
- SQLite metadata persistence;
- one PersistenceThread owning SQLite;
- one LogWriterThread owning task-log filesystem operations;
- durability-before-external-side-effect ordering;
- two-dimensional log credit;
- bounded queues;
- lossless log semantics with backpressure;
- per-Attempt `TASK_FINISHED` wire-order barrier;
- file `fdatasync` + required containing-directory `fsync` before `logs_complete=true`;
- fail-stop behavior for required persistence/invariant failures;
- startup recovery rules;
- all explicit non-goals.

If implementation pressure suggests changing one of these, stop and return to architecture review.

---

# 4. Explicit non-goals remain prohibited

Do not add:

- Controller HA;
- Raft/Paxos;
- automatic retries;
- exactly-once claims;
- Docker/container runtime;
- namespaces/cgroups;
- Kubernetes;
- GPU scheduling;
- CUDA;
- NCCL;
- RDMA;
- gang scheduling;
- topology scheduling;
- preemption;
- TLS/auth/authz;
- sandboxing;
- multi-tenancy;
- web UI;
- general thread pools;
- Boost.Asio;
- gRPC;
- Protobuf;
- Redis;
- Kafka;
- a new storage subsystem;
- a second Worker log TCP connection;
- implicit shell execution.

Do not broaden scope while implementing.

---

# 5. Milestone execution policy

Implementation is milestone-gated.

The dependency order is frozen by `docs/IMPLEMENTATION_PLAN.md`.

Current milestone:

> **Milestone 1 — Build baseline, common values, and domain rules**

For every milestone:

1. inspect the milestone definition;
2. list the exact files/modules you intend to create or modify;
3. implement only that milestone;
4. compile immediately after small coherent changes;
5. add/execute milestone acceptance tests;
6. inspect warnings and test failures;
7. fix failures without weakening frozen semantics;
8. perform a focused source review against architecture/ownership/state contracts;
9. produce a milestone report;
10. stop and wait for explicit approval before the next milestone.

A successful milestone does not authorize the next one.

---

# 6. General implementation rules

## 6.1 No fake completeness

Do not leave required core behavior as:

- TODO stubs;
- fixed return values;
- dummy “success” paths;
- mock production behavior;
- placeholder implementations;
- commented-out intended logic.

Test fakes/mocks are allowed only under test code and must exercise real interfaces.

If a feature belongs to a later milestone, do not create a fake production version now.

## 6.2 Keep source boundaries aligned with architecture

Follow the module dependency direction in `docs/ARCHITECTURE.md`.

Do not create cycles.

Examples of prohibited coupling:

- `scheduler` calling sockets or SQLite;
- `protocol` owning TcpConnection;
- `storage` deciding Task state transitions;
- `io` knowing Task semantics;
- `domain` performing I/O;
- EventLoop owning business objects merely to simplify callback lifetime;
- logging code mutating Task state.

## 6.3 Prefer value semantics

Use value types for:

- IDs;
- snapshots;
- commands;
- completions;
- protocol messages;
- scheduling inputs/outputs.

Use unique ownership for OS resources.

Do not use widespread `shared_ptr` merely to avoid deciding ownership.

## 6.4 Errors must be explicit

Do not:

- swallow exceptions;
- log-and-continue after invariant failure;
- convert an unknown outcome into success;
- silently coerce invalid state transitions;
- silently ignore persistence failures.

Errors must preserve the semantics in `FAILURE_MODEL.md`.

## 6.5 No hidden blocking in EventLoop paths

When later milestones introduce EventLoops:

- socket/pipe fds are non-blocking;
- database and task-log filesystem I/O stay off EventLoop;
- no condition-variable wait in EventLoop;
- no unbounded work loop that starves other events.

Milestone 1 should not implement EventLoop yet.

---

# 7. C++ engineering baseline

Use modern, conservative C++17.

Preferred practices:

- namespaces under a project namespace such as `ts`;
- RAII;
- move-only ownership types;
- `enum class`;
- `std::chrono` for time values;
- strong/value wrapper types for IDs where useful;
- `std::optional` for meaningful absence;
- `std::variant` only where it simplifies a real closed set;
- `std::string_view` only when lifetime is unquestionably non-owning;
- const-correctness;
- explicit constructors where implicit conversion would be unsafe;
- `[[nodiscard]]` for important result-bearing APIs where useful;
- `noexcept` where the contract genuinely permits it.

Avoid:

- macro-heavy design;
- inheritance where composition/value types are clearer;
- custom smart pointers;
- raw owning pointers;
- naked ownership-bearing file descriptors;
- global mutable state;
- exception use across C/POSIX child-after-fork boundaries.

Do not use C++20 features.

---

# 8. Build-system baseline

Milestone 1 may create the production build baseline.

Use a top-level CMake project with:

- C++17 required;
- compiler extensions disabled where practical;
- warnings enabled for project targets;
- warnings treated as errors for project code if supported by the environment;
- CTest enabled;
- GoogleTest integration;
- clean separation of library targets and application targets;
- dependency discovery for approved libraries only.

Approved dependencies:

- nlohmann/json;
- SQLite3;
- spdlog;
- GoogleTest.

Do not add a package manager or vendoring system unless repository constraints require it and the
user approves it.

Prefer standard CMake discovery before inventing dependency machinery.

The build tree must remain out of source.

---

# 9. Test policy

Tests are part of implementation, not post-hoc cleanup.

Follow `docs/TEST_STRATEGY.md`.

For each milestone:

- add unit/component tests together with the code;
- test invalid inputs and boundary conditions;
- test ownership/move/error behavior where relevant;
- use deterministic tests;
- do not use arbitrary `sleep()` as a correctness assertion;
- keep test-only helpers out of production behavior.

Do not weaken a contract because a test is difficult to write.

Do not mark a test skipped merely because the implementation fails it unless the test is explicitly
for a later milestone.

---

# 10. Architecture-change stop rule

Stop implementation and report an architecture issue if any of the following becomes necessary:

- new Task/Attempt/Worker state;
- change to a frozen state transition;
- new or changed protocol field with semantic meaning;
- changed protocol message ID;
- changed message direction;
- different retry semantics;
- different LOST behavior;
- different cancellation semantics;
- changed resource-accounting point;
- changed persistence ordering;
- changed log durability guarantee;
- changed fd/pid/session owner;
- changed Controller/Worker concurrency model;
- new subsystem/dependency;
- scope expansion.

Do not make such changes under the label of “refactor”.

Pure implementation details that preserve every frozen contract do not require architecture review.

---

# 11. Source-control discipline

If the repository is a Git repository:

- inspect current status before editing;
- do not overwrite unrelated user changes;
- keep changes scoped to the active milestone;
- do not rewrite history;
- do not force-reset user work.

Suggested commit boundaries from `IMPLEMENTATION_PLAN.md` should guide implementation granularity,
but do not create commits unless the user explicitly asks you to commit.

If the repository is not yet initialized as Git, do not treat that as an implementation blocker
unless the user asks for repository initialization.

---

# 12. Milestone 1 authorization

## Milestone 1 — Build baseline, common values, and domain rules

This is the **only production milestone currently authorized**.

### Allowed production areas

You may create:

- top-level `CMakeLists.txt`;
- necessary small CMake support files;
- `include/ts/common/`;
- `src/common/`;
- `include/ts/domain/`;
- `src/domain/`;
- test infrastructure;
- Milestone-1 unit tests;
- minimal config value/parsing infrastructure if required by the frozen M1 plan.

Do not create implementation for:

- `io`;
- EventLoop;
- epoll wrappers;
- `net`;
- TCP;
- `protocol` wire codec;
- process runtime;
- Worker;
- Controller application runtime;
- SQLite storage implementation;
- scheduler implementation;
- LogWriter;
- systemd units;
- benchmark code.

Later directories may remain absent until their milestone.

### Milestone 1 required work

Implement the M1 contract from `IMPLEMENTATION_PLAN.md`:

- project build/test skeleton;
- common IDs;
- timestamp/time value helpers needed by domain;
- result/error/value helpers where genuinely needed by M1;
- resource value types;
- Task state enum;
- Attempt state enum;
- Worker state enum;
- validated Task/Attempt transition rules;
- domain values/models required by those rules;
- bounded queue value type if it is genuinely part of the frozen M1 plan;
- configuration value models and parsing/validation required for M1;
- tests.

Do not invent later orchestration state as new domain states.

Pending implementation phases such as:

- persistence pending;
- finalizing logs;
- dispatch waiting;
- barrier pending;

must remain internal orchestration concepts in later milestones and must not become public enum
states.

---

# 13. Milestone 1 normative state contract

Use the exact state spelling from `docs/STATE_MACHINE.md`.

Task states:

```text
PENDING
ACTIVE
SUCCEEDED
FAILED
CANCELLED
```

Attempt states:

```text
ASSIGNED
STARTING
RUNNING
SUCCEEDED
FAILED
TIMED_OUT
CANCELLED
LOST
```

Worker states:

```text
HEALTHY
OFFLINE
```

Do not add convenience states.

Transition validation must match `STATE_MACHINE.md` exactly.

At minimum, tests must prove:

- every documented valid transition is accepted;
- every documented invalid transition is rejected;
- terminal Attempt states do not transition again;
- `FAILED -> PENDING` is the only manual Task retry transition;
- old Attempts are not mutated by retry semantics;
- Task/Attempt transition APIs cannot silently bypass validation.

Persistence ordering is not implemented in M1, but M1 domain APIs must not make later required
ordering impossible.

---

# 14. Milestone 1 resource contract

Implement resource values consistent with frozen architecture.

Core concepts include:

```text
ResourceCapacity
ResourceRequest
ResourceUsage / accounting values as required
```

At minimum:

- CPU slot counts use safe integral types;
- memory uses MB integral accounting as specified;
- request CPU must satisfy frozen validity rules;
- memory zero semantics match the architecture;
- arithmetic must detect overflow/underflow rather than wrap;
- tests cover exact capacity boundaries;
- values must support later deterministic scheduler math.

Do not implement the scheduler algorithm in M1.

However, M1 resource types must support the later projected-dominant-utilization implementation
without requiring semantic redesign.

---

# 15. Milestone 1 IDs and domain identity

Implement IDs in a way that supports the frozen distinctions:

- `task_id`;
- `attempt_id`;
- `session_id`;
- `worker_id`;
- `attempt_no`.

Do not collapse these into interchangeable strings at API boundaries if a lightweight strong type
substantially improves correctness.

Requirements:

- Task/Attempt/session IDs must support the canonical UUID representation required by the protocol;
- Worker ID remains a configured stable ASCII identifier;
- attempt number semantics start at 1 per Task.

Exact UUID generation strategy is an implementation detail as long as it satisfies the frozen
wire/storage contract and tests.

Do not introduce an external distributed-ID service.

---

# 16. Milestone 1 configuration baseline

Only implement config pieces required by the M1 plan.

Configuration parsing must:

- reject unknown fields where the frozen design requires strict JSON;
- reject type/range errors;
- reject invalid ports/durations/capacities/queue sizes when those config sections are introduced;
- preserve safe loopback defaults;
- avoid silently coercing strings/numbers/bools.

Do not prematurely implement systemd or deployment scripts.

Use `docs/DEPLOYMENT.md` as the eventual configuration contract.

---

# 17. Bounded queue rule

If M1 implements the generic bounded queue value/container described by the plan:

- capacity must be explicit;
- unbounded growth is prohibited;
- stop/shutdown semantics must be defined;
- blocking versus non-blocking APIs must be explicit;
- no queue implementation may smuggle blocking waits into future EventLoop call sites;
- tests must cover full/empty/stop behavior and concurrent producer/consumer behavior if concurrency
  is part of the chosen M1 queue implementation.

Keep it generic. It must not know about Task, SQLite, logs, or TCP.

Do not implement PersistenceQueue or LogQueue semantics yet.

---

# 18. Milestone 1 expected repository shape

A reasonable post-M1 shape is:

```text
task-scheduler/
├── CMakeLists.txt
├── AGENTS.md
├── docs/
│   └── ...
├── cmake/                  # only if genuinely needed
├── include/
│   └── ts/
│       ├── common/
│       └── domain/
├── src/
│   ├── common/
│   └── domain/
├── tests/
│   └── unit/
└── build/                  # generated, not source-controlled
```

Do not create empty `net/runtime/controller/worker/...` source trees merely to advertise future
work.

---

# 19. Milestone 1 acceptance requirements

Before claiming M1 complete:

1. configure from a clean build directory;
2. compile all M1 targets successfully;
3. run all M1 CTest/GoogleTest tests;
4. ensure project warnings are clean;
5. verify no networking/process/storage implementation slipped in;
6. compare enum spellings and transition rules to `STATE_MACHINE.md`;
7. verify resource arithmetic/overflow tests;
8. verify config parsing/validation tests;
9. verify bounded queue tests if the queue is implemented in M1;
10. inspect dependency direction;
11. inspect git diff/status if Git is available;
12. ensure no required core behavior is left as a production TODO.

Do not claim success if tests are not actually run.

If a dependency/tool is unavailable in the environment, report the exact blocker and what was
verified instead. Do not fabricate successful execution.

---

# 20. Milestone report format

At the end of Milestone 1, stop and provide a report with:

## A. Summary

- what was implemented;
- what was intentionally not implemented.

## B. Files

- files created;
- files modified;
- files deleted, if any.

## C. Build/Test Evidence

Include the exact commands run and concise results for:

- CMake configure;
- build;
- CTest / test binaries.

## D. Contract Review

State whether M1 matches:

- `STATE_MACHINE.md`;
- `ARCHITECTURE.md` dependency direction;
- `OWNERSHIP.md` where applicable;
- `TEST_STRATEGY.md`;
- `IMPLEMENTATION_PLAN.md`.

## E. Deviations

List any deviation from frozen docs.

If none, state:

```text
No architecture or semantic deviation.
```

## F. Risks / Follow-up

List implementation risks discovered that matter to later milestones.

Do not redesign them unless necessary.

## G. Gate

End with one of:

```text
Milestone 1: PASS — awaiting explicit approval for Milestone 2.
```

or:

```text
Milestone 1: NOT PASSED — blockers remain.
```

Then stop.

---

# 21. Rules for later milestone authorization

When the user later authorizes a new milestone:

- reread that milestone in `IMPLEMENTATION_PLAN.md`;
- reread every frozen document directly relevant to it;
- inspect already implemented lower layers;
- do not reimplement lower layers casually;
- add tests before claiming milestone completion;
- preserve all previous milestone acceptance guarantees.

Special review emphasis:

- **M2**: fd reuse, registration generation, callback lifetime, deferred destruction.
- **M3**: exact 20-byte wire framing, numeric IDs, partial read/write, role validation.
- **M4**: async-signal-safe child path, exec-status pipe, pgid cleanup, zombie/fd discipline.
- **M5**: SQLite transaction atomicity, state versions, recovery, one-owner thread.
- **M6**: pure scheduler, resource reservations, persistence/transport gates.
- **M7**: first real vertical slice without weakening log-finalization semantics.
- **M8**: lossless bounded log credit, finish barrier, fdatasync + directory fsync.
- **M9**: cancellation/timeout/session loss/restart/fail-stop/manual retry.
- **M10**: deployment, sanitizer hardening, benchmark, documentation synchronization.

Do not begin any of these until explicitly authorized.

---

# 22. Implementation-phase forbidden shortcuts

Never solve implementation difficulty by:

- changing Task/Attempt states;
- merging Task and Attempt;
- merging Worker and Connection;
- turning LOST into FAILED-at-source;
- adding automatic retry;
- marking cancel complete before observed termination;
- sending RUN_TASK before the required durable assignment;
- exposing terminal state before the required log barrier;
- dropping logs to avoid backpressure;
- using unbounded queues;
- allowing TASK_FINISHED to overtake earlier same-Attempt log frames;
- bypassing command-slot credit;
- using only byte credit;
- doing SQLite or log-file I/O on EventLoop;
- doing C++ container/string construction in the child after fork;
- falling back to `system()` or implicit shell execution;
- killing only the direct pid where process-group semantics are required;
- using raw owning fds;
- using broad shared ownership to hide lifecycle problems;
- continuing after a documented fail-stop condition;
- silently weakening `fsync`/durability semantics;
- disabling failing tests instead of fixing the implementation.

---

# 23. Scope-control rule

The implementation is allowed to become detailed, but not wider.

If code volume grows, simplify implementation internals before expanding feature scope.

The project remains an educational Linux/C++ distributed workload manager, not a production
cluster manager.

The architecture intentionally prioritizes:

- correctness;
- explicitness;
- systems-learning value;
- interview explainability;

over maximum throughput, availability, or feature count.

---

# 24. First action for this session

After reading the frozen documents:

1. inspect the repository;
2. confirm that `ARCHITECTURE_GATE.md` is PASSED;
3. inspect whether any production implementation already exists;
4. inspect toolchain/dependency availability;
5. summarize the exact Milestone 1 implementation plan you will execute;
6. then implement Milestone 1;
7. build and test it;
8. perform the M1 contract review;
9. report and stop.

Do not ask for permission again before M1: this prompt is the explicit M1 authorization.

Do not begin M2 without a new explicit instruction.

---

# 25. Final instruction

Implement the frozen architecture, not a new interpretation of it.

When uncertain whether something is an implementation detail or an architecture change, choose the
safer path:

- preserve the frozen contract;
- document the uncertainty;
- stop if observable semantics would change.

Milestone 1 is authorized.

Milestone 2 is not.
