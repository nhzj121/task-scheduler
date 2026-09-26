# Codex Architecture / Specification Prompt v1
## Project: Linux C++ Distributed Task Scheduler

> **Usage**: Give this entire document to Codex in the repository root.
>
> **This phase is architecture/specification only. Do not implement production source code yet.**
>
> The purpose of this phase is to freeze the system semantics, architecture contracts, state machines, protocol, ownership/lifetime rules, failure model, test plan, and implementation order before the implementation phase begins.

---

## 0. Your role

Act as a senior Linux/C++ systems architect and reviewer.

Your task is to design the project architecture from a clean repository baseline and produce a coherent architecture/specification package for a lightweight distributed task execution and resource scheduling system.

There is no existing production implementation that must be preserved.

The architecture should therefore be designed coherently from first principles while strictly following the scope and technical constraints defined in this specification.

The project is intentionally educational but must be engineered seriously enough to support:

1. Linux/C++ internship interview discussion in the short term;
2. later evolution toward Compute Infrastructure / AI Infrastructure;
3. detailed study of Linux system programming, networking, concurrency, distributed-system failure semantics, resource scheduling, persistence, and engineering practices.

Do **not** optimize for maximum feature count. Optimize for:
- correctness;
- clear semantics;
- explicit ownership;
- testability;
- explainability;
- future extensibility;
- consistency across modules.

Do not silently change requirements. If you find a contradiction, record it in `docs/DECISIONS.md`, propose the smallest coherent resolution, and keep the rest of the specification consistent with that resolution.

---

# 1. Project definition

The final v1 system is:

> A lightweight distributed task execution and resource scheduling system for a **trusted Linux environment**, implemented primarily in **C++17**. It consists of a single Controller, multiple Workers, and a CLI client (`taskctl`). The system uses a custom TCP protocol to support task submission, resource-aware scheduling, remote process execution, stdout/stderr streaming, task lifecycle management, timeout/cancellation, Worker heartbeat/failure detection, and metadata persistence. The Controller and Worker use epoll-based event-driven I/O. Workers execute workloads using Linux `fork/exec` and manage process output, process exit, timeout, and network events through file-descriptor-oriented Linux mechanisms.

The v1 system is **not** production-ready and must explicitly state that it is intended for a trusted localhost/private-network environment.

---

# 2. Primary goals

The architecture must make the following capabilities explicit and central:

- Linux system programming;
- C++17 RAII and ownership;
- POSIX process management;
- POSIX TCP sockets;
- non-blocking I/O;
- `epoll` in **Level Triggered (LT)** mode;
- `pipe2`, `dup2`, `waitpid`, process groups;
- `signalfd` for `SIGCHLD`;
- `timerfd` for task timeout and periodic timers where appropriate;
- `eventfd` if needed for cross-thread EventLoop wakeups;
- TCP stream framing and partial read/write handling;
- Controller/Worker persistent connections;
- resource-aware task placement;
- Worker heartbeat and failure detection;
- distributed failure semantics;
- SQLite-backed metadata persistence;
- system/task logging;
- CMake-based build;
- unit tests, integration tests, failure-injection tests;
- systemd deployment;
- benchmark scripts;
- architecture and interview-oriented documentation.

The design must prefer **explicit, understandable mechanisms** over framework-heavy abstractions.

---

# 3. Explicit non-goals for v1

Do **not** design or implement these in v1:

- Controller high availability;
- multiple active Controllers;
- Raft or Paxos;
- exactly-once execution guarantees;
- automatic distributed retries;
- container runtime integration;
- Docker execution backend;
- Linux namespace isolation;
- cgroup-based hard resource enforcement;
- Kubernetes integration;
- GPU scheduling;
- CUDA;
- RDMA;
- NCCL;
- gang scheduling;
- topology-aware scheduling;
- task preemption;
- multi-tenant security;
- authentication;
- authorization;
- TLS;
- sandboxing;
- web frontend;
- interactive task stdin;
- shell parsing as the default execution model;
- production-grade horizontal scalability.

If an implementation would require one of these, document it as a future extension instead of partially implementing it.

---

# 4. Final user-visible system

The architecture must support this final workflow.

## 4.1 Components

```text
                           taskctl
                              |
                              | TCP
                              v
                    +------------------+
                    |    Controller    |
                    |------------------|
                    | TaskManager      |
                    | WorkerRegistry   |
                    | Scheduler        |
                    | StateStore       |
                    | LogStore         |
                    +--------+---------+
                             |
                       persistent TCP
                     +-------+-------+
                     |               |
                     v               v
               +-----------+   +-----------+
               | Worker A  |   | Worker B  |
               |-----------|   |-----------|
               | EventLoop |   | EventLoop |
               | Executor  |   | Executor  |
               +-----+-----+   +-----+-----+
                     |               |
                 fork/exec       fork/exec
                     |               |
                  workload        workload
```

## 4.2 Required CLI operations

The architecture must support these operations:

```bash
taskctl submit [options] <executable> [args...]
taskctl status <task-id>
taskctl list
taskctl logs <task-id> [--attempt <n>] [--stdout|--stderr|--both]
taskctl cancel <task-id>
taskctl retry <task-id>
taskctl workers
```

Recommended `submit` options:

```text
--cpu <slots>
--memory <MB>
--timeout <seconds>
--cwd <directory>                optional
--env KEY=VALUE                  repeatable, optional
```

The default execution model is **executable + argv**, not `/bin/sh -c`.

Examples:

```bash
taskctl submit --cpu 2 --memory 512 --timeout 30 /usr/bin/python3 demo.py
taskctl submit /bin/bash script.sh
```

Do not add implicit shell evaluation.

Tasks are non-interactive. Unless a future extension changes this, child stdin should be closed or redirected from `/dev/null`.

---

# 5. High-level module architecture

The architecture must preserve clear dependency direction and avoid cyclic ownership.

Use the following conceptual modules. Exact class names may be refined, but responsibilities must remain equivalent.

```text
apps/
  controller/
  worker/
  taskctl/

library/application modules:
  common/
  domain/
  io/
  runtime/
  net/
  protocol/
  scheduler/
  storage/
  controller/
  worker/
  client/
```

## 5.1 `common`

Responsibilities:
- generic error/result helpers;
- time helpers;
- UUID/ID helpers if needed;
- small RAII primitives that are not domain-specific;
- thread-safe blocking queue if used by persistence;
- utility functions.

Must not contain:
- task scheduling logic;
- TCP protocol semantics;
- task lifecycle rules.

## 5.2 `domain`

Pure or near-pure data models:
- Task;
- Attempt;
- WorkerRecord;
- WorkerSession;
- ResourceCapacity;
- ResourceUsage;
- ResourceRequest;
- TaskState;
- AttemptState;
- WorkerState;
- IDs and timestamps.

Must not perform:
- socket I/O;
- SQLite operations;
- fork/exec;
- scheduling side effects.

## 5.3 `io`

Linux FD/event runtime:
- `UniqueFd`;
- epoll wrapper;
- EventLoop;
- fd registration/unregistration;
- timerfd wrapper;
- signalfd wrapper;
- optional eventfd-based cross-thread wakeup.

Important:
`io` is not a networking-only module. It must be usable for:
- sockets;
- stdout/stderr pipes;
- signalfd;
- timerfd;
- eventfd.

## 5.4 `runtime`

Local process execution:
- process creation;
- `fork`;
- `execve` using executable/argv/envp prepared in the parent, with parent-side PATH resolution;
- argv/environment construction;
- working directory;
- pipe setup;
- stdout/stderr redirection;
- non-blocking pipe reads;
- process group creation;
- wait/reap;
- timeout termination;
- cancellation termination;
- SIGTERM -> grace interval -> SIGKILL;
- process exit result mapping.

The runtime must **not** decide which Worker should execute a task.

## 5.5 `net`

TCP transport:
- listening sockets;
- outbound connections;
- non-blocking sockets;
- accept/connect lifecycle;
- input buffers;
- output buffers;
- partial read;
- partial write;
- connection close/error semantics.

Must not embed task-domain state transitions.

## 5.6 `protocol`

Message framing and serialization:
- binary fixed header;
- JSON payload;
- message type enum;
- request/response correlation;
- encode/decode;
- validation;
- maximum payload enforcement.

Must not call `send`, `recv`, `epoll`, SQLite, or scheduler logic directly.

## 5.7 `scheduler`

Prefer a near-pure decision component.

Input:
- task resource request;
- immutable/snapshot view of eligible Workers.

Output:
- a placement decision or no feasible Worker.

v1 scheduling:
1. filter unhealthy Workers;
2. filter Workers lacking CPU slots;
3. filter Workers lacking memory accounting capacity;
4. optionally enforce configured max concurrent tasks;
5. among feasible Workers minimize projected dominant utilization:
   `max((used_cpu + requested_cpu) / capacity_cpu,
        (used_memory + requested_memory) / capacity_memory)`;
6. compare ratios exactly and use lexicographically smallest Worker ID as the deterministic final
   tie-breaker;
7. a scheduling pass may skip an infeasible older Task and continue to later feasible Tasks; v1
   documents the resulting starvation risk rather than adding priority/reservation policy.

The Scheduler must not:
- own Workers;
- own Tasks;
- send TCP messages;
- write SQLite;
- fork processes.

## 5.8 `storage`

Define an abstraction equivalent to:

```text
StateStore
    ^
    |
SQLiteStateStore
```

Responsibilities:
- task metadata;
- attempt metadata;
- persisted terminal/nonterminal states;
- Worker metadata if needed for history;
- transaction boundaries;
- prepared statements;
- schema creation/migration strategy suitable for v1.

Do not store large stdout/stderr bodies in SQLite.

## 5.9 `controller`

Controller application logic:
- client request handling;
- TaskManager;
- WorkerRegistry;
- WorkerSession lifecycle;
- scheduling orchestration;
- dispatch;
- state transitions;
- heartbeat/failure detection;
- persistence coordination;
- task-log storage coordination.

## 5.10 `worker`

Worker application logic:
- Controller connection;
- registration;
- session management;
- heartbeat;
- ExecutionManager;
- RunningTask ownership;
- local process lifecycle;
- stdout/stderr capture;
- log chunk transmission;
- timeout;
- cancellation;
- task completion reporting.

## 5.11 `client`

Reusable client protocol wrapper used by `taskctl`.

The CLI application should remain thin.

---

# 6. Required project structure

The architecture phase should validate or refine the following repository structure without introducing unnecessary layering:

```text
task-scheduler/
|
├── CMakeLists.txt
├── README.md
├── LICENSE
|
├── cmake/
|
├── apps/
│   ├── controller/
│   │   └── main.cpp
│   ├── worker/
│   │   └── main.cpp
│   └── taskctl/
│       └── main.cpp
|
├── include/
│   └── ts/
│       ├── common/
│       ├── domain/
│       ├── io/
│       ├── runtime/
│       ├── net/
│       ├── protocol/
│       ├── scheduler/
│       ├── storage/
│       ├── controller/
│       ├── worker/
│       └── client/
|
├── src/
│   ├── common/
│   ├── domain/
│   ├── io/
│   ├── runtime/
│   ├── net/
│   ├── protocol/
│   ├── scheduler/
│   ├── storage/
│   ├── controller/
│   ├── worker/
│   └── client/
|
├── tests/
│   ├── unit/
│   ├── integration/
│   └── fault/
|
├── examples/
│   └── workloads/
|
├── configs/
│   ├── controller.json
│   └── worker.json
|
├── scripts/
│   ├── start_cluster.sh
│   ├── stop_cluster.sh
│   └── benchmark.sh
|
├── systemd/
│   ├── task-controller.service
│   └── task-worker.service
|
└── docs/
    ├── PROJECT_SCOPE.md
    ├── ARCHITECTURE.md
    ├── STATE_MACHINE.md
    ├── PROTOCOL.md
    ├── OWNERSHIP.md
    ├── FAILURE_MODEL.md
    ├── TEST_STRATEGY.md
    ├── IMPLEMENTATION_PLAN.md
    ├── DEPLOYMENT.md
    ├── DECISIONS.md
    └── INTERVIEW_NOTES.md
```

Do not create empty placeholder source modules merely to match this tree. During this architecture phase, create/update documentation only.

---

# 7. Task and Attempt semantics

Separate logical user intent from concrete execution.

## 7.1 Task

A Task is a logical user submission.

Required Task state model:

```text
PENDING
ACTIVE
SUCCEEDED
FAILED
CANCELLED
```

Suggested enum:

```cpp
enum class TaskState {
    PENDING,
    ACTIVE,
    SUCCEEDED,
    FAILED,
    CANCELLED
};
```

Semantics:

- `PENDING`: accepted and queued, no active Attempt currently executing.
- `ACTIVE`: an Attempt has been created and is nonterminal.
- `SUCCEEDED`: latest/active Attempt completed successfully.
- `FAILED`: latest/active Attempt reached a failure terminal state.
- `CANCELLED`: user cancellation completed or a pending Task was cancelled.

## 7.2 Attempt

An Attempt is one concrete execution of a Task.

Required Attempt states:

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

Suggested enum:

```cpp
enum class AttemptState {
    ASSIGNED,
    STARTING,
    RUNNING,
    SUCCEEDED,
    FAILED,
    TIMED_OUT,
    CANCELLED,
    LOST
};
```

Required high-level transition:

```text
Task PENDING
    |
    | scheduler selects Worker
    v
Task ACTIVE
    |
    +--> Attempt ASSIGNED
              |
              | Worker accepts dispatch
              v
           STARTING
              |
              | task process successfully launched
              v
           RUNNING
          /   |    |      \
         v    v    v       v
  SUCCEEDED FAILED TIMED_OUT CANCELLED
                         \
                          LOST  (from any relevant nonterminal state on Worker/session loss)
```

Task terminal mapping:
- Attempt `SUCCEEDED` -> Task `SUCCEEDED`;
- Attempt `FAILED` -> Task `FAILED`;
- Attempt `TIMED_OUT` -> Task `FAILED`;
- Attempt `LOST` -> Task `FAILED`;
- Attempt `CANCELLED` -> Task `CANCELLED`.

## 7.3 Manual retry

v1 must not automatically retry distributed failures.

`taskctl retry <task-id>`:
1. only applies to an eligible terminal Task, normally `FAILED`;
2. changes the Task back to `PENDING`;
3. does not mutate the historical Attempt;
4. a later scheduling cycle creates a new Attempt number;
5. Attempt history remains queryable.

Example:

```text
Task 42
  Attempt 1 -> Worker A -> LOST
  Attempt 2 -> Worker B -> SUCCEEDED
```

Do not claim exactly-once semantics.

---

# 8. Worker semantics

Required Worker state:

```cpp
enum class WorkerState {
    HEALTHY,
    OFFLINE
};
```

A Worker has:
- stable `worker_id`;
- per-connection/per-process-start `session_id`;
- configured/advertised resource capacity;
- accounted resource usage;
- last heartbeat timestamp;
- current connection/session;
- running Attempt metadata.

Important distinction:

```text
Worker = logical identity
WorkerSession = one concrete Worker runtime session
Connection = one TCP connection
```

Do not collapse these into one lifetime.

On Worker reconnect:
- same `worker_id` is allowed;
- new `session_id` is required;
- old session is invalidated;
- nonterminal Attempts associated with the old session become `LOST`;
- the new session registers from a clean scheduling perspective.

---

# 9. Failure semantics

These semantics are authoritative for v1.

| Event | Required v1 behavior |
|---|---|
| Child exits 0 | Attempt `SUCCEEDED`, Task `SUCCEEDED` |
| Child exits nonzero | Attempt `FAILED`, Task `FAILED` |
| Child terminated by unexpected signal | Attempt `FAILED`, Task `FAILED` |
| Task timeout | SIGTERM process group -> grace period -> SIGKILL if necessary -> Attempt `TIMED_OUT`, Task `FAILED` |
| Cancel PENDING Task | Task `CANCELLED`, no Attempt required |
| Cancel RUNNING Task | Controller sends cancel -> Worker terminates process group -> Attempt `CANCELLED`, Task `CANCELLED` |
| Worker heartbeat timeout | Worker `OFFLINE` |
| Worker/session disappears with nonterminal Attempts | those Attempts `LOST`; corresponding Tasks `FAILED` |
| Client disconnect | submitted/running Task continues |
| Controller restart | persisted `PENDING` Tasks remain `PENDING`; persisted nonterminal active Attempts become `LOST`; corresponding Tasks become `FAILED` |
| Worker reconnect | new session; old session invalid |
| Dispatch uncertainty | do not pretend to know whether remote execution happened; use Attempt/LOST semantics |
| Retry | explicit user action; creates future new Attempt |

Heartbeat defaults:
- Worker heartbeat interval: approximately 3 seconds;
- Controller offline timeout: approximately 10 seconds.

These values should be configurable.

Do not implement automatic distributed retry in v1.

---

# 10. Process/runtime semantics

Workers execute tasks as child process groups.

Required behavior:
1. in the Worker parent, merge inherited environment with Task overrides;
2. in the parent, finalize stable argv/envp storage and cwd/stdin resources;
3. if executable contains `/`, retain it directly; otherwise resolve it in the parent against the
   final merged PATH, interpreting relative/empty PATH entries against execution cwd;
4. prepare stdout/stderr and exec-status pipes before fork;
5. use close-on-exec where appropriate;
6. fork;
7. child creates/joins its own process group;
8. child redirects stdin/stdout/stderr via `dup2`, applies prepared cwd and signal mask, closes
   redundant fds, and calls `execve` with prepared executable/argv/envp;
9. child performs no C++ container construction, PATH lookup, environment transformation,
   logging, heap allocation, or exception handling;
10. parent closes unused pipe ends;
11. parent marks relevant read ends non-blocking;
12. parent registers stdout/stderr fds and task timerfd with EventLoop;
13. worker-level signalfd receives `SIGCHLD`;
14. on `SIGCHLD`, reap with `waitpid(..., WNOHANG)` until exhausted;
15. treat the direct child as workload leader and freeze its natural exit/signal outcome;
16. if same-pgid descendants remain after leader exit, clean them with SIGTERM -> grace ->
    SIGKILL without overwriting the leader outcome;
17. drain remaining stdout/stderr to EOF after group cleanup;
18. close/unregister all fds deterministically.

Cancellation/timeout:
- send `SIGTERM` to the process group;
- wait configurable grace period, default approximately 2 seconds;
- if still alive, send `SIGKILL` to the process group;
- reap child processes;
- do not leave zombies.

Do not rely on `system()`.

---

# 11. I/O and EventLoop model

Use:
- non-blocking fd;
- epoll;
- **Level Triggered** mode;
- no EPOLLET requirement;
- no EPOLLONESHOT requirement unless architecture review proves it necessary.

Event sources include:
- TCP listening sockets;
- TCP client/Worker connections;
- task stdout pipes;
- task stderr pipes;
- signalfd;
- task timerfds;
- heartbeat/periodic timerfd;
- eventfd for cross-thread wakeup if persistence callbacks need to be posted to the loop.

The architecture must explain:
- fd registration;
- read readiness handling;
- write readiness handling;
- output buffering;
- interest-mask changes;
- deregistration before object destruction;
- EventLoop callback lifetime safety.

---

# 12. Concurrency model

Keep concurrency intentionally small and explicit.

## 12.1 Controller

Preferred model:
- one EventLoop thread owns/mutates live cluster state;
- one persistence thread owns the SQLite connection;
- EventLoop sends persistence commands through a thread-safe queue;
- persistence results return through a safe callback/post mechanism, preferably queue + eventfd/EventLoop wakeup;
- avoid a general-purpose thread pool in v1.

The EventLoop thread owns/mutates:
- active Task state;
- active Attempt state;
- WorkerRegistry;
- WorkerSession state;
- ConnectionManager;
- scheduling orchestration.

This should minimize shared mutable state and mutex use.

## 12.2 Worker

Preferred model:
- one EventLoop thread;
- child processes provide task execution concurrency;
- no task-execution thread pool;
- ExecutionManager owns RunningTask objects;
- signalfd/timerfd/process stdout/stderr are integrated with epoll.

## 12.3 taskctl

Single-threaded is sufficient.

---

# 13. Resource model

v1 resource model is accounting/placement only.

Required:

```cpp
struct ResourceCapacity {
    uint32_t cpu_slots;
    uint64_t memory_mb;
};

struct ResourceRequest {
    uint32_t cpu_slots;
    uint64_t memory_mb;
};
```

Workers advertise capacity.

Controller tracks accounted available/used resources.

Scheduler:
1. only considers `HEALTHY` Workers;
2. filters insufficient CPU;
3. filters insufficient memory;
4. optionally filters max-concurrency violations;
5. minimizes projected dominant utilization after placement across CPU and memory;
6. compares ratios exactly and uses lexicographic Worker ID as the deterministic final tie-break;
7. may skip an infeasible older Task during a scheduling pass, with the starvation limitation
   documented explicitly.

Important:
- v1 does **not** use cgroups to guarantee actual CPU/memory consumption;
- requested resources are scheduling-accounting values;
- README/docs must state this limitation clearly.

---

# 14. TCP protocol contract

Use a custom framed protocol.

## 14.1 Header

Conceptual header fields:

```cpp
struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t message_type;
    uint64_t request_id;
    uint32_t payload_length;
};
```

Do **not** serialize by sending the in-memory C++ struct directly.

Requirements:
- serialize each field explicitly;
- fixed wire order;
- network byte order;
- explicit support/helper for 64-bit endian conversion;
- protocol version starts at 1;
- define a stable magic value in `PROTOCOL.md`;
- header length is fixed and documented;
- validate payload length before allocation.

Recommended maximum JSON payload:
- 4 MiB, configurable/constant.

## 14.2 Payload

JSON using `nlohmann/json`.

JSON is chosen to keep v1 inspectable while still requiring correct TCP framing.

## 14.3 Required message families

Client -> Controller:
- `SUBMIT_TASK`
- `GET_TASK`
- `LIST_TASKS`
- `GET_LOGS`
- `CANCEL_TASK`
- `RETRY_TASK`
- `LIST_WORKERS`

Controller -> Client:
- corresponding success responses;
- `ERROR_RESPONSE`.

Worker -> Controller:
- `REGISTER_WORKER`
- `HEARTBEAT`
- `RUN_TASK_ACK`
- `TASK_STARTED`
- `LOG_CHUNK`
- `TASK_FINISHED`

Controller -> Worker:
- `REGISTER_ACK`
- `RUN_TASK`
- `CANCEL_TASK`

The architecture phase must assign explicit numeric message-type IDs and document them.

## 14.4 Required protocol robustness

The design must explicitly handle:
- frame split across multiple `recv` calls;
- multiple frames in one `recv`;
- partial `send`;
- queued output buffering;
- EAGAIN/EWOULDBLOCK;
- peer close;
- connection reset;
- invalid magic;
- unsupported version;
- unknown message type;
- oversized payload;
- malformed JSON;
- unexpected message for connection role.

`request_id` must correlate request/response flows where applicable.

## 14.5 Log chunks

Task logs are streamed in chunks.

Recommended raw log chunk maximum:
- 64 KiB.

Since arbitrary stdout/stderr bytes may not be valid UTF-8, specify an encoding strategy such as base64 in JSON.

Each log chunk should contain enough metadata to preserve:
- task ID;
- attempt ID;
- stream (`stdout` or `stderr`);
- sequence number;
- data.

The architecture must use credit whose grant already reserves matching Controller LogQueue raw-byte
and command-slot capacity. A correctly credited chunk transfers both reservations and must be admitted; inability to
admit it is an internal invariant failure, not normal socket backpressure. Normal backpressure
occurs before transmission when Worker disables pipe reads for lack of credit, while Controller
continues reading Worker TCP for control traffic.

`TASK_FINISHED` is a per-Attempt wire-ordering barrier. It must not begin sending until every
previously generated stdout/stderr `LOG_CHUNK` for that Attempt is fully sent, even when control
frames can otherwise bypass logs or a log frame is partially written.

---

# 15. Persistence model

Use SQLite3 for v1.

Reasons:
- self-contained deployment;
- sufficient transaction/index/schema learning;
- avoids introducing a separate DB server into a systems-focused project.

Define a `StateStore` abstraction and `SQLiteStateStore` implementation.

Architecture documentation must specify schema for at least:
- tasks;
- attempts;
- optional worker-history/session records if genuinely useful.

Persist:
- logical Task definition;
- resource request;
- timeout;
- cwd/environment metadata as appropriate;
- Task state;
- Attempt state/history;
- assigned Worker/session;
- timestamps;
- exit code/signal/result reason;
- stdout/stderr log paths.

Do not store large task-log contents in SQLite.

The architecture must define:
- transaction boundaries;
- uniqueness constraints;
- indexes;
- startup recovery procedure.

---

# 16. Logging

Separate system logs from task logs.

## 16.1 System logs

Use `spdlog`.

Examples:
- Controller startup;
- Worker registration;
- heartbeat timeout;
- connection errors;
- scheduling decision;
- database failure;
- runtime launch failure.

## 16.2 Task logs

Worker reads child stdout/stderr and sends `LOG_CHUNK`.

Controller stores log data under a deterministic directory, for example:

```text
data/
  tasks/
    <task-id>/
      attempt-<n>.stdout.log
      attempt-<n>.stderr.log
```

SQLite stores paths/metadata, not the full log body.

---

# 17. Ownership and lifetime contract

This is a hard architecture requirement.

Every resource must have one clear owner.

## 17.1 File descriptors

Provide or design a move-only RAII wrapper equivalent to:

```cpp
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1);
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&&) noexcept;
    UniqueFd& operator=(UniqueFd&&) noexcept;

    int get() const noexcept;
    int release() noexcept;
    void reset(int new_fd = -1) noexcept;
};
```

Rules:
- owned fds must not be represented by ambiguous raw `int` ownership;
- raw fd access is non-owning;
- unregister from epoll before destroying the fd owner.

## 17.2 TCP connections

Preferred relationship:

```text
ConnectionManager
    owns
TcpConnection
    owns
socket UniqueFd
InputBuffer
OutputBuffer
```

EventLoop does not own the TcpConnection object.

Architecture must describe how callbacks avoid dangling object references.

## 17.3 Worker execution

```text
ExecutionManager
    owns
unordered_map<attempt_id, RunningTask>
```

Each RunningTask owns:
- process handle metadata;
- stdout read fd;
- stderr read fd;
- timeout timerfd;
- execution metadata;
- buffers required for final drain.

Worker-level signalfd is owned at Worker/EventLoop level, not by every RunningTask.

## 17.4 Process lifetime

ProcessHandle tracks at least:
- pid;
- pgid;
- reaped/terminal state.

Important:
- do not make an ordinary destructor silently SIGKILL a live workload;
- termination must be explicit at the business/runtime level;
- destructor/RAII cleanup handles file descriptors and already-resolved OS resources;
- lifecycle logic guarantees reaping before terminal RunningTask removal.

## 17.5 Controller domain state

TaskManager owns mutable live Task/Attempt state.

WorkerRegistry owns WorkerRecord/WorkerSession state.

Scheduler owns neither.

## 17.6 Worker identity vs connection

Keep distinct:
- Worker logical identity;
- WorkerSession runtime identity;
- TCP Connection.

A Worker can have multiple sessions over time, but only one current valid session.

---

# 18. Controller restart semantics

On Controller startup:
- load persisted Tasks/Attempts;
- `PENDING` Tasks remain `PENDING`;
- any persisted nonterminal Attempt from the previous Controller lifetime becomes `LOST`;
- corresponding Task becomes `FAILED`;
- Workers must reconnect/register with new sessions.

Do not attempt distributed reconciliation with old in-flight processes in v1.

Document this as a deliberate limitation.

---

# 19. Security boundary

The system can remotely request executable launch and therefore represents remote-code-execution capability by design.

Hard requirements:
- trusted environment only;
- localhost/private trusted network;
- no claim of production readiness;
- no public Internet exposure recommendation;
- no authentication/TLS/multi-tenant security in v1;
- no implicit shell command evaluation;
- document security limitations prominently.

---

# 20. Technology choices

Freeze these unless a concrete repository constraint makes one impossible:

| Area | Choice |
|---|---|
| OS | Linux |
| Language | C++17 |
| Compiler | GCC >= 11 |
| Build | CMake >= 3.20 |
| Network | POSIX TCP sockets |
| I/O | non-blocking + epoll LT |
| Linux event sources | socket, pipe, signalfd, timerfd, optional eventfd |
| Protocol | fixed binary header + JSON payload |
| JSON | nlohmann/json |
| Persistence | SQLite3 |
| Logging | spdlog |
| Unit test | GoogleTest |
| Test integration | CTest |
| Deployment | systemd |
| Config | JSON |
| Version control | Git |

Do not introduce Boost.Asio, gRPC, Protobuf, Redis, Kafka, Docker, or Kubernetes into v1 unless explicitly approved later.

---

# 21. Quality constraints

The final implementation phase must target:

- no obvious fd leaks;
- no zombies;
- no dangling epoll registrations;
- no obvious data races;
- no blocking socket/pipe I/O in EventLoop;
- correct partial read/write handling;
- correct TCP framing;
- bounded protocol payload allocation;
- deterministic cleanup on errors;
- clear error propagation;
- no core TODO stubs pretending to be implemented;
- no fixed-value/mock implementation of required core features;
- no “catch all exceptions and continue” behavior that hides state corruption;
- no silent state transitions;
- no hidden shell invocation;
- no accidental use of `shared_ptr` everywhere to avoid ownership decisions.

Correctness and explainability take priority over extreme throughput.

---

# 22. Expected project size

Target, not a hard quota:

```text
core C++ source: roughly 6,000-9,000 LOC
tests:           roughly 2,000-4,000 LOC
total authored:  roughly 8,000-13,000 LOC
```

Exclude third-party and generated build artifacts.

If the architecture would clearly require much more code, simplify it instead of expanding scope.

---

# 23. Required test strategy

The architecture phase must specify tests before implementation.

At minimum:

## Unit tests
- UniqueFd move/reset/release;
- protocol header encode/decode;
- frame parser split-frame;
- frame parser multi-frame;
- malformed frame rejection;
- output buffer partial-write behavior;
- scheduler resource filtering;
- scheduler deterministic tie-break;
- Task/Attempt state-transition validity;
- resource accounting;
- config parsing;
- persistence CRUD/transactions.

## Runtime integration tests
- execute program exit 0;
- execute program nonzero exit;
- SIGSEGV/signal termination;
- capture stdout;
- capture stderr;
- large stdout/stderr without deadlock;
- timeout;
- cancellation;
- child process tree termination;
- no zombie left behind.

## Cluster integration tests
- Controller + 1 Worker + submit;
- Controller + multiple Workers;
- resource-based placement;
- task log retrieval;
- task cancellation;
- manual retry;
- Worker disconnect while idle;
- Worker disconnect while running;
- Worker reconnect with new session;
- heartbeat timeout;
- Controller restart recovery.

## Fault injection
- kill Worker process;
- drop TCP connection;
- kill child task;
- send malformed protocol frame;
- force exec failure;
- force SQLite failure where practical.

## Tooling
The later implementation phase should support sanitizer-friendly builds:
- ASan;
- UBSan;
- TSan where meaningful and practical.

---

# 24. Benchmark scope

Do not promise production performance.

Define repeatable measurements such as:
- task submission round-trip latency;
- scheduling latency;
- dispatch-to-start latency;
- concurrent short-task throughput;
- log-stream throughput;
- Controller behavior with multiple idle Worker connections.

Provide scripts and methodology, not inflated claims.

---

# 25. Learning/documentation requirements

The architecture/specification package must explain these topics in sufficient depth for later study and interview preparation:

- fork/exec/waitpid;
- pipe/dup2/fd lifecycle;
- process groups;
- signal vs signalfd;
- timerfd;
- eventfd if used;
- non-blocking I/O;
- epoll LT;
- Reactor/EventLoop;
- TCP byte stream semantics;
- framing;
- partial read/write;
- connection lifecycle;
- RAII ownership;
- Task vs Attempt;
- Worker session semantics;
- heartbeat and failure detection;
- timeout != failure;
- retry and duplicate-execution risk;
- resource accounting vs resource enforcement;
- scheduler responsibilities;
- thread synchronization;
- SQLite transaction boundaries;
- why v1 does not claim exactly-once.

Mechanical implementation topics may be less emphasized:
- CLI argument parsing;
- JSON boilerplate;
- CMake boilerplate;
- systemd unit syntax;
- SQLite CRUD boilerplate;
- logging wrappers;
- test fixtures;
- shell scripts.

---

# 26. Reference-system positioning

Use systems such as Slurm, Ray, Nomad, HTCondor, and Kubernetes Scheduler only as conceptual references.

Do not:
- copy their architecture blindly;
- copy source code;
- mimic large-system complexity;
- claim feature parity.

The intended v1 is best understood as:
- a small workload manager / distributed execution system;
- conceptually close to a tiny subset of Slurm + distributed runtime ideas;
- deliberately scoped for learning and interview discussion.

---

# 27. What to do in this architecture phase

## Step 1: Establish the clean architecture baseline

Confirm that this repository is starting from an architecture-first baseline.

Read:

- `AGENTS.md`;
- `docs/spec/Task_Scheduler_Final_Scope_v1.md`;
- this architecture specification;
- `docs/spec/Architecture_Review_Checklist_v1.md`.

Do not create production source code.

At this stage, reason about the complete target architecture before introducing implementation artifacts.

Identify:

- missing architectural decisions;
- semantic inconsistencies;
- conflicting requirements;
- ambiguous ownership;
- unclear failure behavior;
- unnecessary complexity;
- decisions that could cause the implementation to exceed the intended project scope.

## Step 2: Specification consistency analysis

Before finalizing the architecture documents, identify:

- requirement contradictions;
- architecture risks;
- missing decisions;
- ambiguous state transitions;
- ambiguous ownership;
- protocol ambiguities;
- failure-model ambiguities;
- unnecessary complexity;
- likely implementation risks.

Since there is no legacy implementation, optimize for a clean and internally consistent design rather than backward compatibility.

Resolve only issues that can be resolved without materially changing the intended v1 scope.

Record significant decisions in `docs/DECISIONS.md`.

## Step 3: Produce/finalize documentation

Create or update:

```text
docs/PROJECT_SCOPE.md
docs/ARCHITECTURE.md
docs/STATE_MACHINE.md
docs/PROTOCOL.md
docs/OWNERSHIP.md
docs/FAILURE_MODEL.md
docs/TEST_STRATEGY.md
docs/IMPLEMENTATION_PLAN.md
docs/DEPLOYMENT.md
docs/DECISIONS.md
docs/INTERVIEW_NOTES.md
```

Do not create production `.cpp/.h` implementation in this phase.

## Step 4: Cross-document consistency pass

Before finishing, verify:
- state names match everywhere;
- message names match everywhere;
- module names match everywhere;
- ownership agrees with architecture;
- failure transitions agree with state machine;
- persistence schema supports required recovery semantics;
- implementation order respects dependency direction;
- tests cover stated guarantees;
- non-goals are not accidentally implemented by the design.

## Step 5: Final architecture report

At the end, provide a concise report containing:

1. files created;
2. final architecture summary;
3. key design decisions;
4. specification ambiguities or architecture risks discovered;
5. explicit limitations;
6. unresolved issues, if any;
7. proposed implementation milestone order;
8. architecture review checklist result;
9. any issue requiring human approval before implementation.

---

# 28. Required contents of each document

## `PROJECT_SCOPE.md`
Must contain:
- project definition;
- goals;
- final user workflow;
- features;
- non-goals;
- technology choices;
- security boundary;
- definition of done.

## `ARCHITECTURE.md`
Must contain:
- system context diagram;
- Controller component diagram;
- Worker component diagram;
- module dependency diagram;
- Controller concurrency model;
- Worker concurrency model;
- submit-to-completion sequence;
- cancel sequence;
- Worker failure sequence;
- Controller restart behavior;
- data flow for stdout/stderr.

Use Mermaid where useful, plus explanatory prose.

## `STATE_MACHINE.md`
Must contain:
- Task state machine;
- Attempt state machine;
- Worker state machine;
- valid transition table;
- invalid transitions;
- event causing each transition;
- owner of each transition;
- persistence requirements per transition.

## `PROTOCOL.md`
Must contain:
- wire header;
- endian rules;
- magic/version;
- message numeric IDs;
- request/response matrix;
- JSON schemas/examples for every message;
- framing algorithm;
- parser behavior;
- partial read/write;
- max sizes;
- malformed-message handling;
- Worker registration/session semantics;
- log chunk encoding.

## `OWNERSHIP.md`
Must contain:
- resource ownership table;
- fd ownership;
- TcpConnection ownership;
- EventLoop registration lifetime;
- RunningTask ownership;
- pid/pgid ownership;
- Task/Attempt ownership;
- Worker/WorkerSession/Connection distinction;
- persistence-thread ownership;
- move/copy policy for key classes;
- cleanup order for every major object.

## `FAILURE_MODEL.md`
Must contain:
- failure taxonomy;
- authoritative behavior table;
- failure detection assumptions;
- heartbeat semantics;
- LOST semantics;
- retry semantics;
- duplicate-execution uncertainty;
- Controller crash recovery;
- Worker crash/reconnect;
- task timeout/cancel;
- explicit non-guarantees.

## `TEST_STRATEGY.md`
Must contain:
- unit/integration/fault matrix;
- test environment;
- sanitizer plan;
- leak/zombie checks;
- protocol fuzz-ish negative cases;
- reproducible integration scenarios;
- definition of pass/fail.

## `IMPLEMENTATION_PLAN.md`
Must contain:
- dependency-ordered milestones;
- each milestone’s source modules;
- acceptance tests;
- no giant “implement everything” milestone;
- suggested commit boundaries;
- architecture checkpoints;
- risks.

The implementation plan should preserve vertical slices where possible.

## `DEPLOYMENT.md`
Architecture-phase version only:
- intended local/multi-process deployment;
- config files;
- ports;
- data directories;
- systemd intent;
- trusted-network warning.

Do not pretend final deployment scripts already exist.

## `DECISIONS.md`
Record architecture decisions in concise ADR-like entries:
- decision;
- rationale;
- alternatives rejected;
- consequences;
- future revisit trigger.

At minimum record:
- C++17;
- epoll LT;
- custom TCP framing;
- JSON payload;
- SQLite;
- no automatic retry;
- no exactly-once;
- single Controller;
- resource accounting not enforcement;
- signalfd/timerfd;
- no general thread pool;
- trusted environment;
- explicit executable+argv.

## `INTERVIEW_NOTES.md`
Architecture-phase version:
- map modules to Linux/C++/OS/CN/distributed-system concepts;
- list likely interview questions;
- explain what the final implementation is intended to demonstrate;
- distinguish “implemented guarantee” from “future idea”.

---

# 29. Forbidden shortcuts in this phase

Do not:
- write production implementation code;
- leave architecture contradictions as “TODO” without discussion;
- use vague ownership such as “shared by several modules”;
- collapse Task and Attempt;
- collapse Worker, WorkerSession, and TCP Connection;
- replace custom networking with gRPC/Boost.Asio;
- add Kubernetes/Docker/CUDA/GPU features;
- claim exactly-once;
- silently auto-retry;
- use “thread pool” merely to appear concurrent;
- use `/bin/sh -c` as the default task execution path;
- move task logs into SQLite blobs;
- make Scheduler responsible for network/storage;
- make EventLoop own domain objects;
- overdesign for millions of nodes.

---

# 30. Architecture acceptance criteria

The architecture phase is complete only if all of the following are true:

- the final CLI workflow is fully supported by the design;
- a task can be traced from submit -> persistence -> scheduling -> dispatch -> Worker launch -> RUNNING -> logs -> completion -> persisted terminal state;
- cancellation has a complete trace;
- timeout has a complete trace;
- Worker failure has a complete trace;
- Controller restart semantics are explicit;
- Task and Attempt are distinct;
- Worker and WorkerSession are distinct;
- every fd has a clear owner;
- every pid/pgid lifecycle has a clear owner;
- no module has contradictory responsibilities;
- Scheduler is side-effect-light/near-pure;
- TCP framing is completely specified;
- backpressure/partial writes are addressed;
- persistence supports recovery semantics;
- test plan verifies the claimed behavior;
- v1 non-goals remain out of scope;
- the implementation plan can be executed without redesigning core semantics.

If any criterion is not satisfied, continue refining the architecture documents rather than starting implementation.

---

# 31. Final instruction

Treat these requirements as the authoritative architecture baseline.

Your output from this phase should leave the repository in a state where a separate implementation agent can implement the system without inventing core semantics on its own.

Do not optimize for producing many documents quickly. Optimize for cross-document consistency and explicit engineering decisions.
