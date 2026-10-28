# M8 local durable-log demo

Run on Ubuntu 22.04.3 under WSL. These are real Controller, Worker and taskctl
processes using TCP, the existing Linux child runtime, SQLite metadata and local
log files. No deployment packaging is provided by M8.

## Build and start

```bash
cd /mnt/e/Deck_all_need/projects/work/task_scheduler
cmake -S . -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
```

Keep the build directory and its `BUILD_COMMANDS.md`. In separate terminals, from
the same source directory:

```bash
/home/nhzj121/build/task_scheduler/controller --config examples/m7/controller.json
/home/nhzj121/build/task_scheduler/worker --config examples/m7/worker-a.json
# Optional second Worker:
/home/nhzj121/build/task_scheduler/worker --config examples/m7/worker-b.json
```

The existing sample configurations remain valid. Controller uses
`/tmp/task-scheduler-m7-demo`; use a separate owned Linux-local directory in a
copy of the configuration to isolate a new demo. Existing history is preserved.
Wait for `READY` and `REGISTERED`. Default taskctl endpoint is `127.0.0.1:7420`.
Global `--host`, `--port` and `--json` options precede the command.

## Submit and read a finite snapshot

```bash
/home/nhzj121/build/task_scheduler/taskctl submit /bin/true
# Replace TASK_UUID with the returned canonical UUID:
/home/nhzj121/build/task_scheduler/taskctl get TASK_UUID
/home/nhzj121/build/task_scheduler/taskctl logs TASK_UUID --both

/home/nhzj121/build/task_scheduler/taskctl submit /usr/bin/printf 'hello\0world\n'
/home/nhzj121/build/task_scheduler/taskctl logs TASK_UUID --stdout > snapshot.bin
/home/nhzj121/build/task_scheduler/taskctl logs TASK_UUID --stdout --limit-bytes 2 > paged.bin
/home/nhzj121/build/task_scheduler/taskctl --json logs TASK_UUID --attempt 1

# A real nonzero result with stderr:
/home/nhzj121/build/task_scheduler/taskctl submit /usr/bin/ls /no-such-m8-example
/home/nhzj121/build/task_scheduler/taskctl logs TASK_UUID --stderr
```

Each submit prints a different UUID; use the corresponding UUID for later
commands. Executable/arguments are direct argv, with parent-prepared environment,
PATH and cwd. Shell syntax is not implicitly executed.

`logs` defaults to stdout. `--stderr` selects stderr; `--both` fetches two
independent fixed-end snapshots and writes stdout bytes followed by stderr bytes.
It does not reconstruct interleaving. Raw output preserves NUL/non-UTF8 and may
be redirected to a binary file. Diagnostics go to stderr. `--json logs` emits one
typed response object per page as NDJSON, containing base64 instead of raw bytes.
All pages are fetched automatically, with bounded per-page memory. Default raw
page size is 262,144 bytes; positive larger requests cap at 3,144,960. Full encoded
JSON still must fit the frozen 4 MiB frame maximum. No follow mode exists.

The first page fixes `snapshot_end` to Writer-visible data. Later writes do not
extend that invocation; a new invocation can see more data. `logs_complete=false`
means this is a partial snapshot, even when `eof_snapshot=true`. A pending Task
without an Attempt returns `NOT_FOUND`. Missing ordinary completed log files
return `INTERNAL`; a genuinely never-executed ProvenEmpty Attempt may return
empty complete logs without files. Incomplete LOST history after restart uses
only known visibility bounds, conservatively zero when none survived; physical
orphan tails are not evidence of durability. Ordinary commands return server
errors with exit 2; the paged logs adapter reports a failed fetch on stderr with
exit 1. Successful commands exit 0.

## What completion guarantees

During internal finalization, status may show `ACTIVE` and
`finalization_pending=true`; resources remain reserved. Normal terminal visibility
requires all prior stdout/stderr writes, both file `fdatasync`s (including empty
files), required dirty-directory `fsync`s, then the matching SQLite terminal
transaction. `/bin/true` now reaches `SUCCEEDED`, with two real zero-byte files and
`logs_complete=true`. Resources are released after metadata commit and may admit
the next Task. Paths remain `tasks/<task-uuid>/attempt-1.stdout.log` and the stderr
equivalent under the configured data root. SQLite stores metadata, never bodies.

Controller has exactly three threads: EventLoop, SQLite PersistenceThread and
LogWriterThread. Log bytes and command slots are independently bounded. Initial
session credits reserve capacity; successful write completions replenish it.
If Writer or TCP cannot make progress, Worker pauses pipe reads. High log rates
can block workload stdout/stderr writes; control/heartbeat/reaping stay live.
Known final log-persistence failures produce `FAILED`/`LOG_PERSISTENCE_ERROR`
with observed outcome retained and completeness false. Earlier session loss
uses conservative `LOST`.

## Stop, restart and remaining boundaries

Stop workloads/Workers with Ctrl-C, then Controller. Group cleanup and nonblocking
reap/drain remain the existing M4 owner contract. A Controller crash recovers
previously nonterminal Attempts as LOST/Tasks FAILED. Previously committed normal
terminal history remains readable. Worker sessions/reservations are not restored;
restart Workers manually. An uncertain submit response is not evidence of failure
or permission to safely resubmit.

M9 cancel/retry CLI orchestration and automatic reconnect remain unsupported.
No log retention, follow, upload, sandbox, authentication, TLS, cgroup enforcement
or systemd packaging was added. Trusted local filesystem/private network scope
and the existing finite metadata/session admission bounds still apply. Process
SIGKILL tests demonstrate ordering and restart behavior, not physical power-loss
simulation or guarantees beyond Linux filesystem/device sync semantics.

```bash
ctest --test-dir /home/nhzj121/build/task_scheduler -L m8 --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -L m8 --output-on-failure --repeat until-fail:20
```
