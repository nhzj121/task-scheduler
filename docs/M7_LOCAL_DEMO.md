# M7 local partial vertical slice

This guide describes the implemented M7 binaries, not the final v1 service.
Use Ubuntu 22.04.3 under WSL. No Docker, systemd, root privileges or remote host
is needed. These are distinct OS processes exchanging real framed TCP packets.

## Build

```bash
cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
```

All targets use existing project warnings including `-Werror`. Never remove the
build directory: it contains the user's `BUILD_COMMANDS.md`.

In each terminal:

```bash
cd /mnt/e/Deck_all_need/projects/work/task_scheduler
```

## Four terminals

Terminal 1 — Controller (wait for `READY client=`):

```bash
/home/nhzj121/build/task_scheduler/controller --config examples/m7/controller.json
```

Startup creates/validates the owned, non-symlink data directory
`/tmp/task-scheduler-m7-demo`, then SQLite takes its exclusive Controller lock,
recovers nonterminal metadata and loads paginated history before listeners open.
Do not run this sample against valuable data. Reusing it intentionally preserves
earlier Tasks. A second Controller with the same root fails explicitly.

Terminal 2 — Worker A (wait for `REGISTERED worker=`):

```bash
/home/nhzj121/build/task_scheduler/worker --config examples/m7/worker-a.json
```

Terminal 3 — optional Worker B:

```bash
/home/nhzj121/build/task_scheduler/worker --config examples/m7/worker-b.json
```

Terminal 4 — actual client:

```bash
/home/nhzj121/build/task_scheduler/taskctl workers
/home/nhzj121/build/task_scheduler/taskctl submit /bin/true
# Replace TASK_UUID below with the printed canonical UUID.
/home/nhzj121/build/task_scheduler/taskctl get TASK_UUID
/home/nhzj121/build/task_scheduler/taskctl --json get TASK_UUID
/home/nhzj121/build/task_scheduler/taskctl list
/home/nhzj121/build/task_scheduler/taskctl --json workers
```

`taskctl --host NUMERIC_ADDRESS --port PORT` overrides its default client endpoint
`127.0.0.1:7420`. Global options precede the command. `status` aliases `get`.
`--json list` streams **one response object per page (NDJSON)**; the client
automatically requests every page, default 100 entries per request, without
retaining an entire database-sized result. Other JSON commands print one response
object. Server errors exit 2; transport/configuration/usage failures exit 1.

Supported submit options are `--cpu N`, `--memory MB`, `--timeout SEC`, `--cwd DIR`,
and repeated `--env KEY=VALUE`. Use `--` before an executable beginning with `--`.
Arguments are direct argv, not shell syntax; there is no implicit shell. The
existing M4 parent-side merged PATH/environment/cwd preparation remains in use.
`--help` on all three binaries does not create listeners or database files.

## Expected observations and explicit limits

Submit returns only after PENDING metadata commits. Controller uses the actual M6
projected-dominant-utilization placement and resource ledger. A tie selects the
lexicographically smaller Worker. Two sample Workers share physical CPU/memory:
advertisements are accounting limits, **not cgroup enforcement**.

After real accepted ACK, exec-confirmed START (actual pid), reap/pipe EOF and
FINISH from `/bin/true`, GET shows **ACTIVE / RUNNING** and
`finalization_pending=true`, `logs_complete=false`. SQLite remains nonterminal;
Controller reservations remain held. This is the correct M7 result, **not
SUCCEEDED**. Launch preparation/exec failures similarly have no invented START;
they may remain STARTING with a pending finalization observation. Explicit
prelaunch rejection and committed session LOST retain their existing M6 rules.

Initial successful registration credits are backed by allocated volatile
Controller memory. Default global raw capacity is 64 MiB and grantable chunk
slots are 4096 minus 128 essential-control entries. Each session reserves 1 MiB
and 64 slots, so slot capacity admits at most 62 default windows (the application
also limits simultaneous connections to 32 per role). There is **no replenishment,
LogWriter, log file, fsync barrier or durable log snapshot**. Staged bytes stay
until session invalidation; they are never called written/synced bytes.

Worker checks remaining byte **and** chunk credit, encoded output queue capacity
and per-tick budget before each pipe read. Exhaustion disables pipe read interest,
eventually blocking workload writes without growing raw queues or dropping bytes
on a valid session. Heartbeats/control continue. Do not use this M7 demo as a
general output-workload service. An exhausted session window may also delay EOF
observations of later quiet Tasks. Small-output diagnostic tests do not establish
durable log acceptance. On invalidated-session shutdown, undeliverable output is
drained only to clean owned processes; no completeness guarantee is exposed.

`logs`, `cancel`, and `retry` return truthful UNSUPPORTED in M7. No automatic
reconnect/retry, old-process adoption or exactly-once execution is implemented.
Restart Workers manually after a Controller restart. A lost submit response means
the Task may have committed; no automatic resubmit/idempotency key is invented.

Startup restore is bounded to 64 MiB of conservatively accounted history;
larger snapshots fail startup explicitly rather than dropping history. GET must
fit the complete bounded response (4 MiB accounting and frozen wire cap), otherwise
it returns an explicit temporary-unavailable error. Persistent histories are read
in pages, not reconstructed from a partial cache. Normal config uses the frozen
M1 schema; M7 application logging supports stderr/journald capture only. A metadata
command capacity below 2 is unusable because ordinary and essential admissions
must both exist, and is rejected explicitly at startup.

The same retained-metadata budget conservatively charges new accepted submissions
(four M5 `owned_bytes` Submit-envelope charges, including container capacities,
plus 8192 bytes of per-Task/Attempt overhead), without
history eviction or refund within a process lifetime. Admission pressure returns
TEMPORARILY_UNAVAILABLE. Session UUID tombstones are bounded by at most 4096
successful registration admissions per Controller lifetime; further registration
is rejected, not forgotten/reused. These are local integration capacity limits,
not new domain states or persistent leases.

## Stop/restart

Use Ctrl-C for each Worker; its existing M4 group termination/grace/KILL and
nonblocking reap/drain complete before owners are destroyed. Controller observes
connection loss and commits LOST before resource release. Then stop Controller.
Stopping Controller first closes/invalidates sessions; Workers exit nonzero for
lost sessions after local process cleanup, without reconnecting.

A crashed Controller conservatively recovers previously ACTIVE Attempts as LOST
and their Tasks as FAILED on restart. PENDING/terminal history survives; live
Worker leases and reservations are never restored. Do not infer non-execution
from an uncertain send or disconnection. Descendants deliberately escaping the
workload process group and uninterruptible kernel processes retain the frozen
v1 limitations.

Security boundary remains trusted local storage/private network. These binaries
execute submitted programs as the Worker user: no sandbox, auth, TLS or public
listener deployment has been added. Sample listeners use loopback; explicit
numeric config changes are the operator's responsibility.

## Reproduce M7 acceptance

```bash
ctest --test-dir /home/nhzj121/build/task_scheduler -L m7 --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -L m7 \
  --repeat until-fail:20 --output-on-failure
```

The external Python standard-library harness runs actual binaries in isolated
temporary roots and dynamically selected loopback ports, with timeouts and tracked
process teardown. Raw TCP peers are test instrumentation for wire ordering and
malformed input, not another production transport. `ts_m7_fault_controller` is a
test-only composition using the real Controller and M5 SQLite hook; it gates the
PersistenceThread with inherited pipes to prove commit ordering. It is not a
production CLI mode and does not replace the real three-binary/two-Worker smoke.
