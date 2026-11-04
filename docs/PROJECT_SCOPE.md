# Project Scope — v1

## Definition

Linux C++ Distributed Task Scheduler v1 is a lightweight workload manager for a trusted Linux
environment. One Controller accepts requests from `taskctl`, places logical Tasks on multiple
Workers using accounted CPU/memory capacity, and records metadata in SQLite. Workers execute
non-interactive programs directly with `fork`/`exec`, stream stdout/stderr, and report lifecycle
results.

It is an educational systems project engineered for correctness and explainability. It is not a
production scheduler and does not claim feature parity with Slurm, Ray, Nomad, HTCondor, or
Kubernetes.

## Goals

- demonstrate C++17 RAII and explicit lifetime ownership;
- exercise Linux processes, fds, pipes, process groups, signals, timerfds, signalfd, eventfd, and
  epoll LT;
- implement correct framed non-blocking TCP, partial I/O, correlation, and bounded buffering;
- separate Task intent from Attempt execution and Worker identity from session/connection;
- make failure, uncertainty, cancellation, timeout, and retry semantics explicit;
- perform deterministic resource-aware placement without pretending to enforce resources;
- persist control-plane metadata and recover conservatively after Controller restart;
- persist lossless task logs through a bounded, backpressured data path;
- define unit, integration, fault, sanitizer, deployment, and benchmark practices before code.

## User workflow

```text
taskctl -> client TCP -> Controller -> persistent worker TCP -> Worker -> child process group
                              |                                  |
                              +-> SQLite metadata                +-> stdout/stderr pipes
                              +-> task log files <--- LOG_CHUNK -+
```

Required commands:

```bash
taskctl submit [--cpu N] [--memory MB] [--timeout SEC] [--cwd DIR] \
               [--env KEY=VALUE ...] <executable> [args...]
taskctl status <task-id>
taskctl list
taskctl logs <task-id> [--attempt N] [--stdout|--stderr|--both]
taskctl cancel <task-id>
taskctl retry <task-id>
taskctl wait <task-id> [--timeout SEC]
taskctl workers
```

`taskctl list` transparently consumes paginated responses. `taskctl logs` reads a finite snapshot;
v1 has no default follow mode. Execution is executable plus argv, never an implicit shell string.

## Included features

- one Controller and multiple Workers;
- separate client and Worker TCP listeners;
- persistent Worker sessions with heartbeat and replacement semantics;
- task submission, status, paginated list, log snapshots, cancel, manual retry, Worker list;
- exact Task/Attempt/Worker state machines documented in `STATE_MACHINE.md`;
- CPU-slot, memory-MB, and optional max-concurrency accounting;
- deterministic projected-dominant-utilization placement;
- process-group execution and termination;
- workload-leader result capture followed by cleanup of residual same-pgid descendants;
- pre-fork argv/envp/cwd/PATH preparation and a minimal async-signal-safe child path;
- separate non-blocking stdout/stderr capture;
- timeout and cancellation using `SIGTERM`, grace timer, then `SIGKILL`;
- explicit exec failure reporting and child reaping;
- framed JSON protocol with fixed binary header and global numeric IDs;
- application log credit, bounded queues, and end-to-end backpressure without intentional drops;
- per-Attempt wire ordering that prevents `TASK_FINISHED` overtaking earlier log chunks;
- SQLite Task/Attempt history and conservative startup recovery;
- separate `PersistenceThread` and `LogWriterThread`;
- file-backed logs with a final file-`fdatasync` and creation-namespace directory-`fsync`
  durability barrier;
- spdlog system logs;
- CMake, GoogleTest/CTest, sanitizers where practical, systemd artifacts, examples, and benchmark
  scripts in the later implementation phase.

## Technology baseline

| Area | v1 decision |
|---|---|
| OS | Linux |
| Language/compiler | C++17, GCC >= 11 |
| Build | CMake >= 3.20 |
| I/O | non-blocking POSIX fds, epoll LT |
| Linux event sources | socket, pipe, signalfd, timerfd, eventfd |
| Process runtime | fork/exec/waitpid and process groups |
| Protocol | explicit 20-byte binary header plus JSON |
| JSON | nlohmann/json |
| Metadata | SQLite3 |
| Task logs | append-only files, not DB blobs |
| System logs | spdlog |
| Tests | GoogleTest + CTest |
| Deployment | systemd intent and JSON configuration |

## Security boundary

The system deliberately launches remote-requested executables. v1 is safe only for a single
trusted operator/security domain on loopback or a trusted private network. It provides no TLS,
authentication, authorization, sandbox, container isolation, multi-tenancy, or protection from a
malicious Worker/client/workload. It must not be exposed to the public Internet. Submitted
environment and paths must be treated as trusted input. Direct argv execution reduces shell
injection exposure but does not make remote execution safe for untrusted users.

## Explicit non-goals

- Controller HA, replicas, consensus, leader election, or seamless failover;
- exactly-once execution or automatic distributed retry;
- adoption/reconciliation of processes from an old Worker session;
- Docker, containers, namespaces, cgroups, hard resource enforcement, or sandboxing;
- Kubernetes integration, GPU/CUDA, RDMA/NCCL, gang/topology scheduling;
- priorities, preemption, dependencies/DAGs, recurring jobs, or workflow orchestration;
- authentication, authorization, TLS, quotas, or multi-tenant isolation;
- interactive stdin, terminal allocation, shell parsing by default, or file staging;
- a web UI, external log service, Redis/Kafka, gRPC/Protobuf, Boost.Asio;
- production-scale throughput, unbounded result sets, or public cloud deployment.

## Authoritative semantic summary

- Tasks: `PENDING`, `ACTIVE`, `SUCCEEDED`, `FAILED`, `CANCELLED`.
- Attempts: `ASSIGNED`, `STARTING`, `RUNNING`, `SUCCEEDED`, `FAILED`, `TIMED_OUT`,
  `CANCELLED`, `LOST`.
- Workers: `HEALTHY`, `OFFLINE`.
- Submit success follows durable `PENDING` insertion.
- Approved post-v1 admission (ADR-024): submit and FAILED retry require a known
  single-Worker total-capacity fit, including OFFLINE advertisements; zero known
  capacity rejects without mutation. Previously accepted PENDING work is retained.
- Client-only wait (ADR-025) polls existing GET_TASK with an optional monotonic
  observation deadline; it does not cancel tasks or introduce protocol events.
- Dispatch follows durable Task `ACTIVE` + Attempt `ASSIGNED` + session binding.
- Terminal Task+Attempt state commits atomically after required final-log durability.
- Memory resource release follows terminal commit; resource accounting is not part of SQLite
  ACID and is not OS enforcement.
- Active cancellation is a persisted request; completion remains asynchronous.
- Session loss makes bound nonterminal Attempts `LOST` and Tasks `FAILED`.
- Workers terminate local workloads when their Controller session is lost.
- A natural leader result remains authoritative while residual same-pgid processes are terminated;
  escaped descendants remain outside the guarantee.
- Granted log credit already reserves Controller LogQueue raw-byte and command-slot capacity;
  normal backpressure happens at Worker pipe reads, while Controller continues reading control
  traffic.
- Controller restart keeps `PENDING`, marks old nonterminal Attempts `LOST`, and requires new
  Worker sessions.
- Manual retry is allowed only from `FAILED` and preserves Attempt history.

## Definition of done for the eventual implementation

A clean supported Linux environment can:

1. configure and build all targets;
2. start one Controller and multiple Workers;
3. observe registered Workers become `HEALTHY`;
4. submit a direct executable/argv Task and receive a durable ID;
5. schedule it according to accounted resources and deterministic placement;
6. remotely launch it and observe `STARTING`/`RUNNING`;
7. capture arbitrary stdout/stderr without deadlock or intentional loss;
8. expose a terminal result only after final logs satisfy the documented barrier;
9. retrieve paginated task data and finite log snapshots;
10. cancel pending and running work with documented asynchronous semantics;
11. time out a process group and leave no direct-child zombie;
12. detect Worker failure and mark in-flight Attempts `LOST`;
13. reconnect a Worker using a new session and no old-state adoption;
14. manually retry a failed Task while retaining Attempt history;
15. restart Controller and execute the documented recovery transaction;
16. run unit, runtime, cluster, failure, and sanitizer test suites;
17. install/run documented systemd units in a trusted environment;
18. run repeatable benchmark scripts without making production-performance claims.

## Architecture-phase completion boundary

This phase produces documentation only. It does not create source/header placeholders, CMake
targets, configs, scripts, service units, or fake implementations. Implementation starts only
after explicit human approval of the reviewed architecture package.
