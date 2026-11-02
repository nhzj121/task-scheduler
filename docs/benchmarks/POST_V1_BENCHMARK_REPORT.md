# Post-v1 concurrent-task and failure-recovery measurements

Review date: 2026-11-02 (Asia/Shanghai). Measurement/evidence only; no production
feature, architecture, scheduler, persistence, protocol or Worker lifecycle change.
No CI workflow, merge, release or production deployment is part of this work.

## Baseline and reproducibility

- Accepted main/origin/main: `124e388665cc2b82db6d1aa212cfb093dc9e4428`, containing
  the accepted M10 cleanup `116952508fb040237ff76a2664be8d23be8556b9`.
- Dedicated branch: `post-v1-benchmark-evidence`; main is not modified.
- Captured measurement harness: `3767b88e95335adb36a7a413a50602fdb11e242b`.
  Each raw manifest also records all four harness-file SHA-256 values and binary
  hashes. The production tree matches accepted main. Large raw data stays outside Git.
- After collection, `d23915b62d83fda54d933fe17fd809ef13bff949` fixes only interrupted
  client-pool setup: waiting threads are released on abort even before the start
  event is set. A deterministic seventh harness test covers that window. The
  captured formal runs had no interruption; their code hashes match `3767b88…`
  exactly and their original timings/data are unchanged. A third complete pilot
  of the published guard-fixed harness passed as a separate validation, not pooled
  into the formal distributions. Reproduce the acquired version at `3767b88…`, or
  generate a fresh matching pilot when using the current scripts.
- Owner selected eight independent submit clients and 32 KiB per binary stream.
- Release build (all commands returned zero):

```bash
SRC=/mnt/e/Deck_all_need/projects/work/task_scheduler
BIN=/home/nhzj121/build/task_scheduler-post-v1-release
cmake -S "$SRC" -B "$BIN" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTS_SANITIZER=none
cmake --build "$BIN" -j2
```

The actual compiler/cache/flags, binary hashes, kernel, CPU, guest memory, clock,
filesystem/mount and full configs are recorded in raw manifests. This is WSL on a
shared Windows host, not a dedicated/native benchmark machine. Source and the
Python binary-output script are on `/mnt/e` (DrvFs); executables, SQLite/WAL,
task logs and measurement data are on guest ext4 under `/home`. Script/interpreter
startup therefore belongs to the binary workload, not just log transport cost.

## Execution sequence and raw locations

```bash
python3 "$SRC/scripts/benchmarks/concurrent_benchmark.py" --bin-dir "$BIN" \
  --root /home/nhzj121/build/post-v1-concurrent-pilot-2 --mode pilot \
  --tasks 20 --repeats 1 --clients 8 --bytes 32768
python3 "$SRC/scripts/benchmarks/recovery_benchmark.py" --bin-dir "$BIN" \
  --root /home/nhzj121/build/post-v1-recovery-pilot-2 --mode pilot --repeats 2

python3 "$SRC/scripts/benchmarks/concurrent_benchmark.py" --bin-dir "$BIN" \
  --root /home/nhzj121/build/post-v1-concurrent-formal --mode formal \
  --tasks 120 --repeats 5 --clients 8 --bytes 32768 \
  --pilot-evidence /home/nhzj121/build/post-v1-concurrent-pilot-2/results.json
python3 "$SRC/scripts/benchmarks/recovery_benchmark.py" --bin-dir "$BIN" \
  --root /home/nhzj121/build/post-v1-recovery-formal --mode formal --repeats 20 \
  --pilot-evidence /home/nhzj121/build/post-v1-recovery-pilot-2/results.json
```

Use new nonexistent roots for reproduction; scripts refuse overwrites and verify
ext4. A formal run requires a successful complete matching pilot, identical binary
and harness hashes, and identical measurement parameters except task/repetition
counts. Two pilot iterations are retained: both passed all nine 20-task concurrent
scenarios and six recovery samples. The second includes additional ownership,
wire-session and empty-child checks; no failed performance sample was discarded.
The six independent harness checks passed before formal measurement. Formal
concurrent and recovery runs execute sequentially, not concurrently with each other.

Each root contains `results.json`, and each independent repetition has its own
directory with `run.json`, configs, `processes.json`, system diagnostics,
SQLite/WAL-related data and task logs. Concurrent manifests contain all per-task
request/send/receive/decoded-response timestamps and every polling observation.
Recovery manifests contain every fault/query observation and proxy event. Raw
artifacts are retained, including failures; no database/task-log files enter Git.

## Concurrent benchmark methodology

- Topologies: one Controller plus 1, 2 or 4 independent Workers on loopback.
  Each Worker advertises two CPU slots, 512 MiB and concurrency two. Workloads
  request one slot/zero accounted memory. Accounting is not CPU/memory enforcement.
- Concurrent runs retain heartbeat 3,000/10,000 ms, group grace 200 ms, ACK/start
  deadlines 5,000/10,000 ms, reconnect 100–500 ms; metadata queue 4,096 commands,
  log queue 64 MiB/4,096 commands with 128 control reserve, initial per-session
  log credit 1 MiB/64 chunks, and frozen 4 MiB frame maximum. Full per-run numeric
  ports/configs remain in raw evidence; no production defaults were rewritten.
- Workloads: direct `/bin/true`; direct `/bin/sleep 0.2`; direct Python executable
  plus the accepted deterministic workload, 32,768 stdout bytes and 32,768 stderr
  bytes. stdout byte i is i mod 251; stderr XORs it with 0xa5. No implicit shell.
- Five independent repetitions per topology/workload, 120 measured tasks each:
  45 repetitions and 5,400 measured Tasks. Each uses fresh application processes,
  database and data root; no Task warmup or machine-wide cache flush. Registration,
  health checks and TCP setup precede timing. Runs have fixed topology/workload
  order, not randomized host-load control.
- Eight bounded submit threads own eight independent persistent TCP clients.
  Two independent query clients observe disjoint task-index partitions while
  submissions continue. Each connection has one correlated request in flight;
  no submission retry/idempotency key or protocol extension. Client TCP_NODELAY
  is explicit; server socket policy is unchanged.
- All timestamps use `time.monotonic_ns()` in the external driver. Queued time,
  request encoding/start, completed send, received response and decoded response
  are distinct. Connection creation is recorded separately and excluded.
  Completed send means `sendall()` returned, not a physical packet-transmit time.
- Durable submission response latency: request start to decoded SUBMIT response;
  it includes JSON, TCP and the required durable PENDING commit, not an internal
  database-commit timestamp. It excludes client queue delay and TCP setup.
- Observed durable-terminal latency: submit request start to the first completed
  GET observing terminal Task/Attempt. It includes Python scheduling, submit work,
  execution/queueing, durable barriers and query/poll delay. **It is not internal
  scheduler latency or workload execution time.** Waiting in the external client's
  submission queue precedes request start and is excluded; its timestamp is retained.
  Polling pauses 10 ms per round.
- Successful observed durable-terminal Tasks/s: exact-byte/SQLite-verified
  successes divided by batch-start to last observed terminal response. Downloads,
  stored-file checks and final metadata inspection happen afterward and are
  excluded from that denominator. Missing/unknown/failed/incomplete samples and
  cleanup failures invalidate a run; they cannot be silently omitted.
- Verification requires exactly 120 unique Tasks/Attempts, SUCCEEDED, one Attempt
  each, logs_complete, no finalization_pending, exact wire/file bytes and SQLite
  paths/counters. All Workers must end HEALTHY with zero running_attempts and zero
  CPU/memory reservations, and no direct children before teardown.

Resource observations sample `/proc` every 50 ms: Controller/individual Worker
CPU user+system ticks, current RSS, lifetime VmHWM, fd and thread counts. CPU deltas
are between the recorded resource snapshots, not precisely the rate interval;
they exclude clients, the driver and workload child CPU. Sampled maxima may miss
brief peaks; VmHWM includes startup. More Workers share physical host resources.
Processes are read sequentially within each sampling pass, not as an atomic snapshot.
Post-timing verification/artifact writes can affect subsequent host cache/I/O;
this is not controlled saturation/profiling evidence.
CPU/RSS observations describe application parent processes, not workload descendants.

## Failure-recovery methodology

Each scenario has a two-repetition pilot and twenty independent formal samples.
One real Controller, one real Worker, and an external loopback proxy are used per
sample. The accepted M9 test patterns supply RUNNING/session/LOST/history/group
checks; accepted `cluster_support.owned` supplies PID birth-time/UID/executable/
argv ownership verification. The proxy passes complete v1 frames unchanged except
for the explicit fault; it is not a replacement Controller or fake Worker.

Dedicated settings: heartbeat 100 ms, offline timeout 1,500 ms, group grace 200 ms,
Worker reconnect backoff 100–500 ms, dispatch ACK 5 s/start 10 s. The workload is
direct `/bin/sleep 60`, requesting CPU 1/memory 16 MiB. RUNNING must be durable and
the real leader PID/pgid/parent identity verified before injection; two valid
heartbeats must first be forwarded. This cooperative workload does not measure
uncooperative SIGKILL escalation.

1. **TCP disconnect:** close/reset both ends of the old proxy connection. The
   same live Worker cleans/reaps its old group and reconnects with a fresh UUID.
2. **Graceful Worker SIGTERM:** signal only the verified Worker. Poll loss/release
   and local group cleanup. Explicitly start a new Worker process only after the
   old process exits zero, old pgid disappears and LOST/release are observed.
3. **Heartbeat timeout with TCP open:** drop only old-session HEARTBEAT frames;
   all other frame bytes pass. No intentional close occurs. The first close must
   be Controller EOF after the detector acts; dropped heartbeat counts/events
   and subsequent real Worker registration are retained.

Record injection begin/end, every GET/Worker query interval, first durable
LOST/FAILED observation, first zero-reservation observation, first old-pgid
absence observation, old/new Session UUIDs, optional Worker exit/restart begin/end,
and first HEALTHY observation with a different Session. New registration must also
be seen in the proxy's actual Worker wire stream; old Attempt session/identity
remain unchanged, one Attempt only, logs incomplete, no adoption/automatic retry.
Independent SQLite readers verify the terminal facts. Local group disappearance
is checked separately; **LOST never proves physical Worker death**.

All timings are client/proxy upper observations, not exact SQLite commit times.
Task and Worker queries are sequential, so polling can observe release or new
HEALTHY before its next Task query observes LOST even though the frozen internal
commit-before-release/activation order is preserved. Injection is bracketed;
heartbeat timeout is relative to the last valid heartbeat, not necessarily the
moment dropping was enabled. Proxy forwarding/thread scheduling and reconnect
backoff are included in recovery observations. P50/P95 are descriptive lower
empirical quantiles floor((n-1)*q), without interpolation, not confidence bounds.

## Formal concurrent results

**45/45 repetitions PASS; 5,400/5,400 Tasks SUCCEEDED**, with one Attempt each,
complete exact logs and matching SQLite evidence. No missing/unknown response,
timeout, failed Task, incomplete successful-task log or cleanup error occurred.
All final reservations/running counts were zero and all owned application exits
were zero. Binary output verified totals 117,964,800 bytes across both streams.

Each row has five independent repetitions and 600 latency observations. Rates
summarize the five run rates; latency quantiles pool the 600 tasks, not the five
already-rounded per-run quantiles. Full values and all repetitions are retained.

| Workers | Workload | Median Tasks/s | Min–max Tasks/s | Submit response P50 / P95 ms | Observed completion P50 / P95 ms |
|---|---|---:|---:|---:|---:|
| 1 | /bin/true | 22.936 | 22.129–24.038 | 67.9 / 84.4 | 2767.6 / 4175.7 |
| 1 | sleep 0.2 | 8.656 | 8.633–8.666 | 65.5 / 72.6 | 6802.0 / 12386.2 |
| 1 | binary | 18.294 | 17.729–18.495 | 66.0 / 78.6 | 3235.4 / 5405.2 |
| 2 | /bin/true | 32.782 | 31.631–33.666 | 85.5 / 108.2 | 2008.1 / 2462.6 |
| 2 | sleep 0.2 | 15.971 | 15.723–16.096 | 78.8 / 100.4 | 3649.9 / 6172.3 |
| 2 | binary | 29.406 | 27.717–30.834 | 79.9 / 102.4 | 2004.0 / 2992.8 |
| 4 | /bin/true | 32.275 | 31.816–34.661 | 107.6 / 155.2 | 1831.5 / 2131.1 |
| 4 | sleep 0.2 | 27.845 | 26.621–28.378 | 97.0 / 154.6 | 2034.9 / 2807.8 |
| 4 | binary | 34.238 | 33.473–36.080 | 107.5 / 140.6 | 1705.6 / 2013.7 |

[POST_V1_CONCURRENT_REPETITIONS.csv](POST_V1_CONCURRENT_REPETITIONS.csv) contains
**every one of the 45 measured repetitions**, including outcome counts, rate,
latencies, CPU/RSS/fd/thread observations and release/cleanup status.

Resource ranges over the fifteen runs at each Worker count (KiB is not MiB):

| Workers | Controller CPU seconds/run | All Workers CPU seconds/run | Controller sampled peak RSS KiB | All Workers peak RSS sum per pass KiB | Controller max observed fds | Individual Worker max observed fds |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 2.20–8.59 | 0.12–0.19 | 54,496–55,052 | 9,540–9,856 | 30 | 16–24 |
| 2 | 0.89–4.03 | 0.12–0.23 | 58,616–59,272 | 19,100–19,524 | 31 | 13–23 |
| 4 | 0.76–1.77 | 0.11–0.20 | 66,860–67,680 | 38,100–39,136 | 33 | 13–23 |

Observed application thread counts remained Controller 3 / each Worker 1.
CPU ticks have 10 ms granularity (SC_CLK_TCK=100); workload child and client/driver
CPU are excluded. The CSV also retains lifetime VmHWM at observation end and
resource-snapshot interval lengths. These samples do not establish infinite-run
memory bounds, precise transient fd peaks, or whole-host CPU utilization.

## Formal failure-recovery results

**60/60 measurement samples PASS**, twenty per scenario. Task outcomes were
**60 FAILED / Attempt LOST / logs_complete=false**, as required after session
loss. Those expected incomplete logs are explicitly retained, not silently treated
as complete or omitted. There were no missing timings, validation failures,
automatic retries/adoption, unreleased reservations or cleanup errors. Old groups
were verified absent in all samples. TCP/heartbeat cases kept the same live Worker
PID; SIGTERM cases explicitly restarted a new process after verified exit/cleanup.

| Fault | LOST observation P50 / P95 ms | Reservation release P50 / P95 ms | New HEALTHY observation P50 / P95 ms | Old pgid absent P50 / P95 ms |
|---|---:|---:|---:|---:|
| TCP disconnect | 9.625 / 11.068 | 9.998 / 11.547 | 114.110 / 121.834 | 11.345 / 12.694 |
| Worker SIGTERM | 10.443 / 11.562 | 10.710 / 12.015 | 25.712 / 29.342 | 11.878 / 17.334 |
| Heartbeat dropped, TCP open | 1492.307 / 1517.170 | 1492.772 / 1517.449 | 1606.142 / 1631.947 | 1504.693 / 1530.709 |

For SIGTERM, explicit restart-begin to observed HEALTHY was P50 **13.994 ms**,
P95 **14.697 ms**, min **11.810 ms**, max **14.719 ms**. All other table timings
start at injection begin. Automatic reconnection's 100 ms initial backoff is part
of TCP/heartbeat measurements; immediate explicit process restart is a different
policy, so these HEALTHY values are not interchangeable restart-speed claims.
Heartbeat numbers use the dedicated **100/1,500 ms** settings; default v1
3,000/10,000 ms heartbeat recovery was not measured by these fault samples.

[POST_V1_RECOVERY_REPETITIONS.csv](POST_V1_RECOVERY_REPETITIONS.csv) contains
**every one of the 60 samples**: exact old/new Session UUID, Task/Attempt IDs,
old/new PID, injection brackets, absolute monotonic observations, optional
exit/restart times, relative durations, states, log completeness and cleanup status.
The raw proxy events prove heartbeat drops with no deliberate close and first
closure from the Controller; raw SQL views preserve the old Attempt binding.

## Interpretation and limits

Four Workers improved these sleep/binary measurements relative to one; /bin/true
did not improve from two to four. Submission latency rose with Worker count.
These are observations, not a proof of a particular bottleneck or linear scaling.
Possible costs include the single decision loop, one SQLite writer, one LogWriter,
FULL commits/file-and-directory barriers, fork/exec, JSON/base64, and observer
traffic. No production optimization, profiler, without-observer control, kernel
network tuning or CPU affinity change was used to attribute causality.

M10's earlier small benchmark used twenty sequential taskctl submissions and
subsequent observation. This dataset uses 120 preconnected concurrent TCP
submissions with continuous query traffic and no Task warmup. The figures are not
an apples-to-apples before/after optimization or regression comparison.

GET_TASK median observed round trips ranged about 0.55–4.23 ms across scenarios,
with observed tails up to 1.36 seconds; repeated polling itself creates load.
Only 15–184 of 600 tasks per scenario were already SUCCEEDED on their first
poll. Queueing, query timing and observer scheduling therefore remain part of the
reported completion latency and rate. None is renamed internal scheduler latency.

The guest reports Intel i7-13650HX, 20 logical CPUs and MemTotal 12,105,608 kB;
kernel `6.18.54.1-microsoft-standard-WSL2`, x86_64; GCC 11.4.0, CMake 3.22.1,
SQLite 3.37.2, Python 3.10.12. Data is on the guest `/` ext4 mount with
relatime/discard/data=ordered. Release flags include `-O3 -DNDEBUG` and all accepted
strict warnings as errors. Hostname is omitted from published evidence. Worker
counts describe shared-host processes, not separate physical machines. Hardware
heterogeneity, desktop/WSL load, cache, fixed ordering, filesystem writeback and
polling/proxy overhead limit generalization. Nanosecond timestamp representation
does not imply nanosecond measurement accuracy. No physical-death, exact commit,
power-loss, security/isolation, exactly-once or production-SLA claim is made.

## Evidence, validation and delivery

[POST_V1_SUMMARY.json](POST_V1_SUMMARY.json) contains exact unrounded statistics,
environment/binary/harness hashes, all outcome totals and raw manifest locations:

- Concurrent: `/home/nhzj121/build/post-v1-concurrent-formal/results.json`
  (97,826,981 bytes), SHA-256
  `86e531afdc9e027e891eb064479b927f9ad26aaa87e4408078ca894c0832be51`.
- Recovery: `/home/nhzj121/build/post-v1-recovery-formal/results.json`
  (3,545,174 bytes), SHA-256
  `768ad9049c25c6c8f262c48c7e44488091b944cb4b320c455ca1cf6060fa7c64`.

Every root/repetition's raw JSON, log and SQLite files remain outside Git. The
published CSVs are 45/60 rows, not selected examples. No measured repetition was
removed or silently replaced. Pilots and the failure-path-only fix validation are
separate; they are not pooled into formal percentiles or throughput.

Standalone harness checks: **7/7 PASS**, 0.282 s, including fragmented/wrong-ID/
oversized/error responses, selective heartbeat dropping without socket closure,
interrupt between spawn/registry publication, and interrupt before pool release.
The post-fix pilot passed all nine concurrent scenarios and six fault samples.
Debug regression configure/build retained warnings-as-errors; **473/473 CTest
PASS**, 81.84 s, including all accepted M9 faults. Commands/logs:

```bash
python3 "$SRC/tests/integration/post_v1_measurement_test.py" --bin-dir "$BIN" \
  --root /home/nhzj121/build/post-v1-harness-tests-2
cmake -S "$SRC" -B /home/nhzj121/build/task_scheduler \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTS_SANITIZER=none
cmake --build /home/nhzj121/build/task_scheduler -j2
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
```

Only measurement scripts, one standalone harness test, this report and the three
small summary artifacts changed. No production source/header, frozen document,
CMake target or GitHub Actions workflow changed. Process ownership uses exact
PID/start-time/UID/executable/argv checks; cleanup verifies exits and child/groups,
uses no broad process-name kill, retains failed artifacts and deletes no operator
data/build directory. `.local-notes/` and `BUILD_COMMANDS.md` are preserved.

Measurement/evidence collection is complete. Branch-only commits/push and final
remote/main equality are reported in the delivery message. Independent review is
required; no merge, release, deployment or further feature work is authorized.
