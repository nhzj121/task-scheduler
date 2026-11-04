# Linux distributed task scheduler

[English](README.md) | [简体中文](README.zh-CN.md)

A C++17/Linux learning project: one Controller, multiple Workers and `taskctl`.
Workers execute trusted executable/argv requests directly; the Controller places
tasks using advertised CPU/memory and deterministic projected dominant utilization.
SQLite stores metadata; a separate log writer stores binary stdout/stderr.

The v1 implementation is complete. It targets trusted, small Linux clusters for
batch commands such as build/test jobs and offline scripts, with cancellation,
timeouts, manual retry and conservative failure recovery. It is an engineering
prototype, not a production cluster platform.

**Trusted operators/private networks only. There is no authentication, TLS,
sandbox or cgroup resource enforcement. Never expose the listeners publicly.**
Multiple Workers on one host share its physical resources.
Listeners default to `127.0.0.1`. Multi-host use requires operators to provision
executables, dependencies and input files on Workers; artifact/data distribution
is outside this project.

```mermaid
flowchart LR
  CLI[taskctl] --> C[Controller EventLoop]
  C <--> P[PersistenceThread / SQLite WAL FULL]
  C <--> L[LogWriterThread / files and namespace sync]
  C <--> W[Worker EventLoop]
  W --> R[ProcessRuntime / process groups]
```

## Build and test

Supported baseline: Ubuntu 22.04, GCC 11+, CMake 3.20+, Python 3.10+ for tooling.
Recorded native validation used GCC 11.4 and CMake 3.22.1 under Ubuntu 22.04/WSL2;
TSan uses a separate GCC 12 toolchain (see the development guide).

```bash
sudo apt-get install build-essential cmake libsqlite3-dev libspdlog-dev \
  nlohmann-json3-dev libgtest-dev python3 pkg-config
cmake -S . -B "$HOME/build/task-scheduler" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$HOME/build/task-scheduler" -j4
ctest --test-dir "$HOME/build/task-scheduler" --output-on-failure
```

Warnings are errors. Build outside the source tree. Do not delete a user's build
directory or `BUILD_COMMANDS.md` to obtain a clean build: use CMake's `clean` target.

## Install and run an isolated two-Worker demo

```bash
cmake -S . -B "$HOME/build/task-scheduler-runtime" -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/opt/task-scheduler"
cmake --build "$HOME/build/task-scheduler-runtime" -j4
cmake --install "$HOME/build/task-scheduler-runtime"
DEMO=$(mktemp -d /tmp/task-scheduler-demo.XXXXXX)
python3 "$HOME/opt/task-scheduler/share/task-scheduler/tools/local_cluster.py" demo \
  --bin-dir "$HOME/opt/task-scheduler/bin" --root "$DEMO" --workers 2
```

The demo verifies durable success, pending/active cancellation, timeout and manual
retry history; it stops only its own processes and keeps diagnostics/data. For
interactive use, replace `demo` with `start`, read `cluster.json` for the chosen
loopback client port, and use `status`/`stop --root "$DEMO"`. Use a new root for
each `demo`; `start` may reuse a stopped tool-owned root while preserving history.

## CLI

Global `--host`, `--port`, `--json` options precede the command.
The examples below use the default loopback client port, 7420. For the demo's
automatically selected port, pass `--port` from its `cluster.json` on every call.

```bash
taskctl --host 127.0.0.1 --port 7420 submit --cpu 1 --memory 16 --timeout 5 /bin/echo hello
taskctl get TASK_UUID
taskctl list
taskctl workers
taskctl logs TASK_UUID --attempt 1 --both
taskctl cancel TASK_UUID
taskctl retry TASK_UUID
```

`TASK_UUID` is the submit response. Task execution does not implicitly interpret
shell syntax; `--timeout` is in seconds. `--json` emits typed JSON; in that mode,
lists/logs emit one page per line.
`logs` fixes finite snapshots, automatically pages, and preserves arbitrary bytes.
`--both` concatenates stdout then stderr snapshots; it is not an interleaved or
live-follow stream.
Wait for terminal state before expecting complete logs; redirect binary logs to
a file rather than a terminal when appropriate.

## Validation and measurements

Recorded regression validation passed **473/473 CTest cases**, covering protocol
framing, fd/callback lifetime, scheduling/accounting, SQLite rollback and commit
ordering, real process execution, binary logs, cancellation/timeouts, reconnection
and injected failures. This is recorded evidence, not a claim that every host or
configuration has been tested.

ASan/UBSan/LSan validation and GCC 12 TSan coverage are documented in
[Development](docs/DEVELOPMENT.md) and the [M10 report](docs/reports/MILESTONE_10_REPORT.md).
TSan covers 30 component tests and three selected real Controller/Worker integration
cases, not the entire integration suite. The existing
[Linux CI workflow](.github/workflows/linux.yml) contains the automated jobs.

Post-v1 Release measurements used 1/2/4 Workers, eight independent submit clients,
and `/bin/true`, `/bin/sleep 0.2`, or deterministic binary output (32 KiB per stream).
All **45 runs / 5,400 Tasks succeeded**, with exact logs and final reservations
released. All **60 recovery samples** met the expected LOST/FAILED, incomplete-log,
old-group cleanup and fresh-Session checks; they are not successful workload outcomes.

| Measured scenario | Client/proxy-observed interval | P95 |
| --- | --- | ---: |
| 4 Workers, binary workload | Submit request start to durable submission response | 140.6 ms |
| Same, 120-task batches | Submit request start to first durable-terminal observation | 2013.7 ms |
| Worker TCP disconnect | Fault injection to first LOST/FAILED observation | 11.1 ms |
| Same, automatic reconnect | Fault injection to first new HEALTHY Session observation | 121.8 ms |

Concurrency figures pool 600 tasks (five 120-task runs); disconnect figures use
20 samples. Completion includes task queueing and polling delay: **these are not
internal scheduler timings or exact database commit timestamps**. Reconnect uses
100 ms initial backoff and does not retry the old Attempt. Heartbeat-drop recovery
was separately measured at 100/1500 ms heartbeat/offline settings, not the default
3000/10000 ms. Measurements use WSL2, same-host loopback and ext4 data roots;
they establish neither multi-machine scalability nor production SLAs.

The [Post-v1 report](https://github.com/nhzj121/task-scheduler/blob/c117663970f0e35d3fe560f3149853db27e7bbd9/docs/benchmarks/POST_V1_BENCHMARK_REPORT.md)
and [summary evidence](https://github.com/nhzj121/task-scheduler/tree/c117663970f0e35d3fe560f3149853db27e7bbd9/docs/benchmarks)
are pinned to the measurement commit on `post-v1-benchmark-evidence`; they include
all repetitions, exact commands, raw-data locations and limitations.

## Guarantees and limits

- Durable submit precedes acceptance; durable assignment precedes RUN; committed
  cancel intent precedes active cancel acceptance/command.
- Active cancel is asynchronous: ACK is receipt, final termination is observed.
  First installed termination cause and frozen natural leader result win.
- Normal terminal visibility waits for reap/EOF, all log chunks, both file syncs,
  required directory syncs and the atomic Task/Attempt SQLite transaction.
- Session loss/restart yields LOST/FAILED, not proof of host death. Resource
  release/replacement follows the LOST commit. Worker reconnect follows cleanup.
- Retry is manual and FAILED-only; old Attempt/log history is immutable. Unknown
  remote execution may overlap a manual retry. No exactly-once claim is made.
- CPU/memory are accounting inputs. Logs have no retention/quota. Backpressure
  can block workload writes. Escaped setsid descendants and uninterruptible tasks
  have no finite termination guarantee. Local durability is not replication or
  cross-medium ACID.

## Navigation

These links use the source-checkout layout. Installed guide copies are flat files
under the GNUInstallDirs documentation directory; they do not include the source
tree or historical specification/report archive.

- [Operations and systemd](docs/OPERATIONS.md)
- [Development, tests and instrumentation](docs/DEVELOPMENT.md)
- [Implemented ownership/dataflow](docs/ARCHITECTURE_IMPLEMENTED.md)
- [Frozen architecture](docs/ARCHITECTURE.md), [protocol](docs/PROTOCOL.md),
  [failure model](docs/FAILURE_MODEL.md), [interview notes](docs/INTERVIEW_NOTES.md)
- [Reproducible benchmark](scripts/benchmark.py)
- Source: `include/ts`, `src/{controller,worker,runtime,storage,logs,net,io}`;
  tests: `tests/{unit,component,integration}`; tools: `scripts`; services: `packaging`.

v1 completion and local validation do not constitute production deployment certification.
