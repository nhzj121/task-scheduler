# Task Scheduler Final Scope v1

## 1. Positioning

**Project**: Linux C++ Distributed Task Scheduler

**Short-term goal**: serve as a substantial Linux/C++ systems project for internship interview preparation, with enough depth to support discussion of C++, Linux system programming, networking, operating systems, concurrency, distributed-system semantics, scheduling, persistence, testing, debugging, and deployment.

**Long-term goal**: provide a clean architectural base for later study of Compute Infrastructure / AI Infrastructure, especially distributed execution, cluster scheduling, resource management, training/inference workload orchestration, and later GPU-aware scheduling.

This project is not intended to imitate all of Slurm, Ray, Kubernetes, Nomad, or HTCondor. It is a deliberately scoped workload manager / distributed execution system.

---

## 2. Final v1 definition

v1 is a lightweight distributed task execution and resource scheduling system for a **trusted Linux environment**.

It contains:

- one Controller;
- multiple Workers;
- one CLI client, `taskctl`;
- persistent Controller <-> Worker TCP connections;
- task submission and query;
- resource-aware placement;
- remote process execution;
- stdout/stderr streaming;
- process exit collection;
- timeout;
- cancellation;
- heartbeat;
- Worker failure detection;
- explicit LOST semantics;
- manual retry;
- SQLite metadata persistence;
- file-backed task logs;
- system logging;
- unit/integration/fault tests;
- CMake build;
- systemd deployment artifacts;
- benchmark scripts;
- architecture and interview documentation.

---

## 3. Authoritative final workflow

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
                    +--------+--------+
                    |                 |
                    v                 v
               +----------+      +----------+
               | Worker A |      | Worker B |
               +----+-----+      +----+-----+
                    |                 |
                fork/exec         fork/exec
                    |                 |
                workload          workload
```

Required CLI:

```bash
taskctl submit [options] <executable> [args...]
taskctl status <task-id>
taskctl list
taskctl logs <task-id>
taskctl cancel <task-id>
taskctl retry <task-id>
taskctl workers
```

Recommended submit options:

```text
--cpu <slots>
--memory <MB>
--timeout <seconds>
--cwd <directory>
--env KEY=VALUE
```

Default execution is direct executable + argv. No implicit shell evaluation.

---

## 4. Fixed architecture modules

- `common`: generic helpers and RAII primitives.
- `domain`: Task/Attempt/Worker/Resource data models.
- `io`: UniqueFd, epoll, EventLoop, timerfd, signalfd, optional eventfd.
- `runtime`: process execution, pipe capture, wait/reap, timeout, cancellation.
- `net`: TCP server/client connection and buffering.
- `protocol`: framed wire protocol and JSON serialization.
- `scheduler`: pure/near-pure placement decision.
- `storage`: StateStore abstraction and SQLite implementation.
- `controller`: cluster/application orchestration.
- `worker`: Controller communication and local ExecutionManager.
- `client`: reusable client for taskctl.

---

## 5. State model

### Task

```text
PENDING
ACTIVE
SUCCEEDED
FAILED
CANCELLED
```

### Attempt

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

### Worker

```text
HEALTHY
OFFLINE
```

A logical Task and a concrete Attempt are separate concepts.

Manual retry preserves old Attempt history and creates a new Attempt later.

v1 does not automatically retry distributed failures.

---

## 6. Failure semantics

- exit 0 -> Attempt SUCCEEDED -> Task SUCCEEDED;
- nonzero exit -> Attempt FAILED -> Task FAILED;
- unexpected signal -> Attempt FAILED -> Task FAILED;
- timeout -> SIGTERM group -> grace -> SIGKILL if required -> TIMED_OUT -> Task FAILED;
- pending cancel -> Task CANCELLED;
- running cancel -> process-group termination -> Attempt CANCELLED -> Task CANCELLED;
- Worker heartbeat timeout -> Worker OFFLINE;
- nonterminal Attempts on a lost Worker/session -> LOST -> Task FAILED;
- Controller restart -> PENDING remains PENDING; old active attempts -> LOST; tasks -> FAILED;
- Worker reconnect -> same worker_id permitted but new session_id;
- client disconnect does not cancel the task;
- no exactly-once guarantee;
- retry is explicit/manual.

Default heartbeat design:
- approximately every 3 s;
- offline timeout approximately 10 s;
- configurable.

---

## 7. Resource model

Scheduling resources:

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

The system performs **resource accounting and placement**, not hard Linux resource enforcement.

No cgroups in v1.

Scheduling:
1. HEALTHY Workers only;
2. sufficient CPU;
3. sufficient memory;
4. optional max-concurrency filter;
5. among feasible Workers, minimize projected dominant utilization:
   `max((used_cpu + requested_cpu) / capacity_cpu,
        (used_memory + requested_memory) / capacity_memory)`;
6. compare ratios exactly and use lexicographically smallest `worker_id` as the deterministic
   final tie-break;
7. a scheduling pass may skip an infeasible older Task and continue to later feasible Tasks; v1
   accepts the documented starvation risk and adds no priority/reservation policy.

---

## 8. Linux runtime depth

v1 intentionally exposes Linux primitives rather than hiding them behind a large framework.

Expected mechanisms:

```text
fork
execve (with parent-side PATH resolution when needed)
pipe2
dup2
fcntl / O_NONBLOCK
waitpid
kill
setpgid
SIGTERM
SIGKILL
signalfd
timerfd
epoll
optional eventfd
```

Before `fork`, Worker parent prepares the merged inherited environment plus Task overrides,
stable argv/envp storage, cwd resources, and executable resolution. If executable contains `/`,
use it directly; otherwise resolve it in the parent against the final merged PATH. The post-fork
child performs only minimal async-signal-safe setup and `execve` using prepared storage.

The direct child is the workload leader. After a natural leader exit, retain its observed outcome.
If same-process-group descendants remain, clean them with SIGTERM -> grace -> SIGKILL without
overwriting the leader outcome, then drain stdout/stderr to EOF. Descendants that deliberately
leave the process group remain outside the v1 guarantee.

Worker I/O is EventLoop-driven.

Use epoll Level Triggered mode.

Task execution concurrency comes primarily from child processes, not a task thread pool.

---

## 9. Networking/protocol

Transport:
- POSIX TCP;
- non-blocking sockets;
- epoll LT;
- persistent Worker connections.

Wire framing:
- fixed binary header;
- JSON payload.

Conceptual header:

```text
magic:          uint32
version:        uint16
message_type:   uint16
request_id:     uint64
payload_length: uint32
```

Do not serialize raw C++ struct memory.

Protocol must handle:
- partial reads;
- multiple frames/read;
- partial writes;
- EAGAIN;
- output buffering;
- malformed frames;
- max payload;
- version mismatch;
- peer close.

Task logs use chunked messages. Granted log credit must pre-reserve matching Controller LogQueue
raw-byte and command-slot capacity, so normal backpressure occurs before transmission by disabling Worker pipe reads while
Controller continues reading the TCP connection. `TASK_FINISHED` is a per-Attempt wire-ordering
barrier and must not overtake any previously generated same-Attempt `LOG_CHUNK`, including during
partial socket writes and control-frame prioritization.

---

## 10. Persistence/logging

Persistence:
- SQLite3;
- StateStore abstraction;
- tasks;
- attempts;
- state/timestamps/results;
- log paths;
- startup recovery.

System log:
- spdlog.

Task logs:
- stdout/stderr stored as files;
- not large SQLite blobs.

---

## 11. Concurrency

Controller:
- one EventLoop thread for live cluster state;
- one persistence thread for SQLite;
- queue-based handoff;
- eventfd/EventLoop posting if needed;
- no general thread pool by default.

Worker:
- one EventLoop thread;
- multiple child processes;
- ExecutionManager owns RunningTask instances.

taskctl:
- single thread is sufficient.

---

## 12. Ownership principles

- all owned fds use explicit RAII;
- `UniqueFd` is move-only;
- unregister fd from epoll before fd owner destruction;
- ConnectionManager owns TcpConnection;
- EventLoop does not own TcpConnection;
- ExecutionManager owns RunningTask;
- RunningTask owns process-local execution resources;
- Worker-level signalfd is shared at Worker/EventLoop level;
- TaskManager owns mutable Task/Attempt state;
- WorkerRegistry owns Worker/WorkerSession state;
- Scheduler owns neither;
- Worker identity, WorkerSession, and TCP Connection are separate lifetimes;
- process termination is explicit; destructors must not silently SIGKILL normal live workloads.

---

## 13. Security boundary

v1 is for a trusted environment only.

Not implemented:
- auth;
- TLS;
- authorization;
- sandbox;
- container isolation;
- multi-tenancy.

Do not recommend public Internet exposure.

No implicit `/bin/sh -c`.

---

## 14. Technology stack

| Area | Decision |
|---|---|
| OS | Linux |
| Language | C++17 |
| Compiler | GCC >= 11 |
| Build | CMake >= 3.20 |
| Network | POSIX TCP sockets |
| I/O | non-blocking + epoll LT |
| Linux event sources | socket/pipe/signalfd/timerfd/optional eventfd |
| Protocol | fixed binary header + JSON payload |
| JSON | nlohmann/json |
| Persistence | SQLite3 |
| Logging | spdlog |
| Tests | GoogleTest + CTest |
| Deployment | systemd |
| Config | JSON |
| VCS | Git |

No Boost.Asio/gRPC/Protobuf/Redis/Kafka/Docker/Kubernetes in v1.

---

## 15. Explicit non-goals

- Controller HA;
- Raft/Paxos;
- exactly-once;
- automatic distributed retry;
- Docker/container runtime;
- namespaces/cgroups;
- Kubernetes integration;
- GPU scheduling;
- CUDA;
- RDMA;
- NCCL;
- gang scheduling;
- topology-aware scheduling;
- preemption;
- multi-tenant security;
- TLS/auth;
- web frontend;
- interactive stdin;
- production-scale performance.

---

## 16. Quality bar

Require:
- no obvious fd leaks;
- no zombies;
- no dangling epoll registration;
- no blocking socket/pipe I/O in EventLoop;
- no obvious data races;
- correct framing;
- correct partial I/O;
- deterministic cleanup;
- bounded payload allocation;
- no fake core TODOs;
- no mock core behavior;
- clear state transitions;
- clear ownership.

Correctness and explainability > extreme performance.

---

## 17. Target project size

Guideline:

```text
core C++: 6,000-9,000 LOC
tests:    2,000-4,000 LOC
total:    8,000-13,000 authored LOC
```

If architecture substantially exceeds this, reduce scope.

---

## 18. Final deliverables

- source code;
- CMake build;
- README;
- `PROJECT_SCOPE.md`;
- `ARCHITECTURE.md`;
- `STATE_MACHINE.md`;
- `PROTOCOL.md`;
- `OWNERSHIP.md`;
- `FAILURE_MODEL.md`;
- `TEST_STRATEGY.md`;
- `IMPLEMENTATION_PLAN.md`;
- `DEPLOYMENT.md`;
- `DECISIONS.md`;
- `INTERVIEW_NOTES.md`;
- unit tests;
- integration tests;
- failure-injection tests;
- benchmark scripts;
- systemd units;
- sample configs;
- example workloads.

---

## 19. Definition of done

v1 is complete only when a clean environment can:

1. build the project;
2. start Controller;
3. start multiple Workers;
4. see Workers become HEALTHY;
5. submit a task;
6. schedule it based on accounting resources;
7. remotely start the process;
8. capture stdout/stderr;
9. observe RUNNING and terminal state;
10. retrieve logs;
11. cancel a running task;
12. time out a long-running task;
13. detect Worker failure;
14. mark in-flight Attempt LOST;
15. manually retry a failed task;
16. restart Controller and perform documented recovery;
17. run the test suite;
18. run a repeatable basic benchmark.

The system must clearly state all limitations and non-guarantees.
