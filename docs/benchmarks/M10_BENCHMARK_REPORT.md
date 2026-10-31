# M10 durable-terminal benchmark

Scope: the current M10 branch, Ubuntu 22.04 WSL2, one Controller and one/two
independent Workers on the same host. This is a small end-to-end observation
benchmark, not a saturation test, execution-time microbenchmark or scaling SLA.

## Reproduction

Build/stage the runtime-only Release configuration in [M10_LOCAL_DEMO](../M10_LOCAL_DEMO.md).
The recorded final command is:

```bash
SRC=/mnt/e/Deck_all_need/projects/work/task_scheduler
python3 "$SRC/scripts/benchmark.py" \
  --bin-dir /home/nhzj121/build/task_scheduler-m10-stage-final/opt/task-scheduler-m10/bin \
  --root /home/nhzj121/build/task_scheduler-m10-benchmark-delivery \
  --output /home/nhzj121/build/task_scheduler-m10-benchmark-delivery/results.json \
  --workers 1 2 --tasks 20 --repeats 3 --warmup 2 --bytes 32768 --build-type Release
```

Choose new root/output leaves when reproducing; the script refuses overwrites.
Only its verified owned services are stopped; databases/logs are retained locally,
not committed. The published raw JSON is `m10_results.json` beside this report.

## Workloads, timing and correctness

- Empty: direct `/bin/true`, zero-byte stdout and stderr, with the normal empty
  file creation, namespace sync, final log barrier and SQLite terminal commit.
- Binary: direct Python interpreter plus `scripts/workloads/binary_output.py`,
  exactly 32,768 bytes on each stream. stdout byte i is i mod 251; stderr XORs it
  with 0xa5. Writes are bounded 4,096-byte blocks; streams remain independent.
- Each of four scenarios has three fresh Controller/data-root/Worker repetitions,
  two completed warmups and twenty measured tasks. No database/cache reset occurs
  between warmup and measurement within a repetition. Warmups are excluded from
  reported task throughput. No machine-wide cache flush is performed.
- `time.perf_counter()` measures wall time. Submission includes process creation,
  taskctl, TCP and durable acceptance. Twenty tasks are submitted sequentially;
  afterward each is polled to first observed durable terminal success.
- End-to-end latency is issuance to that observation, including queueing,
  completion/barriers, other submissions, query process creation and polling delay.
  It is **not** the server's exact completion timestamp or workload execution time.
- Throughput is twenty verified-success Tasks divided by elapsed time from batch
  issuance through the last durable terminal observation. PENDING/ACTIVE/submit
  ACK/finalization_pending cannot count. Downloads are checked afterward and have
  a separately recorded total interval; they are excluded from the rate denominator.
- Every measured task must have one SUCCEEDED Attempt, logs_complete, no pending
  finalization, exact wire downloads, exact stored bytes and matching independently
  read SQLite counters/paths. The final live Worker query must be HEALTHY with
  zero running_attempts and zero CPU/memory reservations.
- Failure is explicit and retained. Startup/measurement/cleanup failure cannot
  yield PASS. There is no automatic retry or dropped failed sample.

## Environment and resource accounting

The raw environment records kernel/version, CPU model/count, WSL, available RAM,
ext4 mount options, compiler/CMake/SQLite versions and clock ticks. Hostname is
intentionally omitted. Formal data roots are under Linux `/home`, not DrvFs.
Release is built with GCC 11.4 and the unchanged strict project warnings.

Each Worker advertises CPU slots 2, memory 512 MiB and concurrency 2. This is
placement accounting; Workers share the actual host. Each repetition records all
Controller queue/heartbeat/dispatch limits and concrete dynamic loopback ports;
only the private absolute data-root value is replaced with a descriptive marker.
No remote network, CPU affinity, cgroup or resource enforcement is introduced.

`/proc` records Controller and each Worker CPU ticks, lifetime VmHWM, endpoint fd
counts and thread counts before/after the batch. CPU delta includes subsequent
log verification, excludes client/workload CPU, and has tick quantization. VmHWM
includes warmup/startup; it is not allocated-memory attribution. Endpoint fd
counts are not peak fd occupancy. No inference about infinite-run memory/disk
growth is made from this short sample; dedicated saturation tests provide separate
queue/credit evidence. Task logs have no retention/quota.

## Final results

Delivery run: PASS, 240/240 measured successes, zero failed/missing/timed-out
samples, plus 24 completed warmups. Each run ended with all registered Workers
HEALTHY, zero running Attempts and zero reservations; all owned services stopped.
Measured binary data totals 7,864,320 bytes, independently verified through wire
downloads, stored files and SQLite. Empty tasks also required complete barriers.

| Workers | Workload | Median Tasks/s | Min–max Tasks/s (3 runs) | Observed p50 / p95 (ms, 60 samples) |
|---|---|---:|---:|---:|
| 1 | empty | 28.377 | 27.430–29.467 | 366.6 / 423.1 |
| 1 | binary | 19.525 | 18.783–20.169 | 457.8 / 782.7 |
| 2 | empty | 25.355 | 24.601–25.881 | 514.1 / 713.9 |
| 2 | binary | 27.595 | 27.150–27.930 | 471.6 / 609.6 |

All individual per-run rates, without omitted outliers:

| Workers / workload | Repetition 0 | Repetition 1 | Repetition 2 |
|---|---:|---:|---:|
| 1 / empty | 29.467 | 27.430 | 28.377 |
| 1 / binary | 20.169 | 19.525 | 18.783 |
| 2 / empty | 24.601 | 25.355 | 25.881 |
| 2 / binary | 27.150 | 27.595 | 27.930 |

Resource observations (actual raw units retained):

| Process / scenario | VmHWM KiB range | End fd count | Threads |
|---|---:|---:|---:|
| Controller, 1 Worker / empty | 17,216–17,256 | 17 | 3 |
| Controller, 1 Worker / binary | 21,436–21,508 | 17 | 3 |
| Controller, 2 Workers / empty | 21,292–21,360 | 18 | 3 |
| Controller, 2 Workers / binary | 21,892–25,608 | 18 | 3 |
| Individual Worker, empty | 9,568–9,636 | 9 | 1 |
| Individual Worker, binary | 9,784–9,864 | 9 | 1 |

At 100 ticks/s, Controller CPU deltas were 0.11–0.20 seconds per run and each
Worker 0.01–0.03 seconds, over the interval including later log verification.
Workload/client CPU is excluded. These are not utilization or total-work figures.

Measured environment: x86_64 WSL2 kernel `6.18.54.1-microsoft-standard-WSL2`,
Intel i7-13650HX, 20 guest logical CPUs, MemTotal 12,105,596 kB. Data resides on
the guest `/` mount (`/dev/sdd`, ext4, relatime/discard/data=ordered). Compiler
GCC 11.4.0, CMake 3.22.1, SQLite 3.37.2; hostname omitted. The virtual device
name is environmental, not a portable deployment requirement.

Raw [m10_results.json](m10_results.json), SHA-256 of original LF bytes:
`0790f168fcb82df54f50f00b39573bc39181e8684760998e96d169f90ecd6445`.
The original data/configs remain at the command's Linux-local root. Compact
[validation evidence](../validation/M10_VALIDATION_EVIDENCE.json) records test
commands/results and independent teardown checks. No SQLite/log files are shipped.

To inspect the summaries without rerunning:

```bash
python3 -m json.tool docs/benchmarks/m10_results.json
```

The two-Worker empty median is lower than the one-Worker median, while the binary
median is higher. This mixed result is retained; it does not establish either
general scaling or a server-side cause. Client observation cost and host variation
remain part of every measurement.

## Earlier executions and limits

The earlier `task_scheduler-m10-benchmark-primary` pilot passed all four scenarios
with three repetitions, but overlapped acceptance activity. A later
`task_scheduler-m10-benchmark-final` attempt failed during startup ownership
inspection before any sample; its output/log and guarded cleanup are retained.
`task_scheduler-m10-benchmark-reviewed` completed after the guard correction; the
delivery run adds explicit live zero-reservation evidence. These are disclosed
rather than silently pooled or relabelled as primary measurements.

The published delivery run follows completion of the project validation processes.
The shared Windows/WSL host remains uncontrolled; other OS activity and filesystem
cache can affect results. Three repeats and sixty pooled latency observations per
scenario are small samples. Quantiles use the lower empirical index
floor((n-1)*q), without interpolation; throughput summaries use three per-run
rates, not a pooled synthetic run. Outliers remain in the raw JSON.

One EventLoop, one SQLite writer, one LogWriter, synchronous durable barriers,
fork/exec, JSON/base64 and client process/query overhead are possible costs. This
experiment does not isolate them or establish causality. A difference between
one and two same-host Workers is not evidence of general distributed scaling.
No scheduler/durability optimization was made to improve the numbers. Native
Linux, WAN, physical power failure, authentication, workload isolation and
production deployment readiness were not measured or claimed.
