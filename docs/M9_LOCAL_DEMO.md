# M9 local cancellation, retry and recovery demo

These workflows were exercised with actual Ubuntu 22.04.3 WSL Controller, two
Workers, taskctl, TCP, SQLite and Linux-local logs. The exact generated ports,
UUIDs, config paths, commands and responses are retained in
`/home/nhzj121/build/task_scheduler/m9-demo.log`. The acceptance suite additionally
uses deterministic commit/barrier gates; this interactive recipe is not a timing
proof.

The commands below use the same verified CLI forms with copied existing example
configs. `TASK_UUID`, `CONTROLLER_PID`, `WORKER_PID`, `WORKLOAD_PGID` and the demo
directory printed during setup are explicit runtime placeholders. Replace them
with the values from your run. Ports 7420/7421 are the example defaults; choose
unused ports in both copied configs if another instance already owns them.

## Build and prepare an isolated demo

```bash
cd /mnt/e/Deck_all_need/projects/work/task_scheduler
cmake -S . -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler -L m9 --output-on-failure

export TS_BIN=/home/nhzj121/build/task_scheduler
export TS_DEMO=$(mktemp -d /tmp/task-scheduler-m9-demo.XXXXXX)
python3 - "$PWD" "$TS_DEMO" <<'PY'
import json, sys
from pathlib import Path
source, target = map(Path, sys.argv[1:])
for name in ('controller', 'worker-a', 'worker-b'):
    value = json.loads((source / 'examples/m7' / (name + '.json')).read_text())
    if name == 'controller':
        value['data_dir'] = str(target / 'data')
    (target / (name + '.json')).write_text(json.dumps(value))
print('Use this TS_DEMO in every terminal:', target)
PY
```

Keep the build directory and `BUILD_COMMANDS.md`. Use Linux `/tmp` for metadata
and task logs; do not substitute a Windows/DrvFs data directory for durability
testing. Existing user demo data is not erased or reused by this setup.

## Start Controller and two Workers

In terminal 1:

```bash
export TS_BIN=/home/nhzj121/build/task_scheduler
export TS_DEMO=/tmp/task-scheduler-m9-demo.REPLACE_WITH_PRINTED_DIRECTORY
echo "Controller PID after exec: $$"
exec "$TS_BIN/controller" --config "$TS_DEMO/controller.json"
```

In terminals 2 and 3, set the same `TS_BIN`/`TS_DEMO`, then respectively:

```bash
# Terminal 2
echo "worker-a PID after exec: $$"
exec "$TS_BIN/worker" --config "$TS_DEMO/worker-a.json"

# Terminal 3
echo "worker-b PID after exec: $$"
exec "$TS_BIN/worker" --config "$TS_DEMO/worker-b.json"
```

Wait for Controller `READY` and both `REGISTERED` messages. In terminal 4, set the
same variables. Global `--host`, `--port` and `--json` precede the subcommand.

```bash
"$TS_BIN/taskctl" --host 127.0.0.1 --port 7420 workers
"$TS_BIN/taskctl" list
```

## Pending and active cancellation

The example Workers each have two CPU slots, so this request cannot be placed:

```bash
PENDING=$("$TS_BIN/taskctl" submit --cpu 100 /bin/true)
"$TS_BIN/taskctl" get "$PENDING"
"$TS_BIN/taskctl" cancel "$PENDING"
"$TS_BIN/taskctl" --json cancel "$PENDING"
```

The first cancel reports `completed CANCELLED`, after SQLite commit, with no
Attempt. The repeat reports `already_cancelled` and does not mutate history.

```bash
ACTIVE=$("$TS_BIN/taskctl" submit /bin/sleep 60)
"$TS_BIN/taskctl" get "$ACTIVE"
# Once status shows an active Attempt:
"$TS_BIN/taskctl" cancel "$ACTIVE"
"$TS_BIN/taskctl" get "$ACTIVE"
"$TS_BIN/taskctl" logs "$ACTIVE" --attempt 1 --both
```

An active cancel reports `accepted ACTIVE`. Repeat `get` until CANCELLED; receipt
ACK alone is not completion. The final state waits for group cleanup, reap, both
pipe EOFs, complete log/namespace barrier and terminal SQLite commit. A request
that wins before any RUN bytes were queued can instead report `completed` via the
documented ProvenEmpty path. A cancel that loses to a natural result preserves
that result. Cancelling SUCCEEDED/FAILED returns CONFLICT/nonzero.

## Timeout and manual retry

```bash
TIMED=$("$TS_BIN/taskctl" submit --timeout 1 /bin/sleep 60)
"$TS_BIN/taskctl" get "$TIMED"
# Repeat get after completion: Task FAILED, Attempt TIMED_OUT.

FAILED=$("$TS_BIN/taskctl" submit /usr/bin/ls "$TS_DEMO/missing-until-retry")
"$TS_BIN/taskctl" get "$FAILED"
# Wait for FAILED before retry; preserve this first Attempt's stderr:
"$TS_BIN/taskctl" logs "$FAILED" --attempt 1 --both
mkdir "$TS_DEMO/missing-until-retry"
"$TS_BIN/taskctl" retry "$FAILED"
"$TS_BIN/taskctl" get "$FAILED"
"$TS_BIN/taskctl" logs "$FAILED" --attempt 1 --both
"$TS_BIN/taskctl" logs "$FAILED" --attempt 2 --both
```

Retry returns `PENDING next_attempt=2` only after commit. New assignment has a new
Attempt UUID/path and may choose either healthy Worker. The first Attempt and its
logs are unchanged; a second invocation succeeds because the directory now
exists. There is no automatic retry after timeout, loss or reconnect. Raw logs
are finite binary-safe snapshots; `--both` concatenates independently fetched
stdout and stderr snapshots. Use `--json logs` for base64 NDJSON pages; there is
no follow mode.

## Worker kill and restart

Submit another sleep and use `get` to identify the selected Worker. The foreground
terminal printed its PID above. For a normal local shutdown:

```bash
"$TS_BIN/taskctl" submit /bin/sleep 60
"$TS_BIN/taskctl" get TASK_UUID
kill -TERM WORKER_PID
"$TS_BIN/taskctl" get TASK_UUID
"$TS_BIN/taskctl" workers
# In that Worker's terminal, restart the same config:
"$TS_BIN/worker" --config "$TS_DEMO/worker-a.json"  # or worker-b.json as selected
```

The disconnected Attempt becomes LOST/Task FAILED, not automatically PENDING.
The restarted Worker registers a fresh Session UUID without adopting old work.

SIGKILL was also actually tested in `M9Loopback.killed_worker_lost_has_no_automatic_retry_and_new_session_is_empty`
and the recorded demo transcript. A killed Worker cannot execute cleanup. The
test explicitly owns, kills and reaps its orphan workload group using the
previously captured pgid; the test result must not be read as a guarantee that
`kill -KILL WORKER_PID` alone terminates all workloads. Use the TERM recipe above
for an interactive demo that does not deliberately create that orphan.

## Controller crash and automatic Worker reconnect

Keep Workers alive; submit a sleep, wait for RUNNING and record its UUID:

```bash
"$TS_BIN/taskctl" submit /bin/sleep 60
"$TS_BIN/taskctl" get TASK_UUID
kill -KILL CONTROLLER_PID
# Restart Controller in terminal 1 with the SAME data directory:
"$TS_BIN/controller" --config "$TS_DEMO/controller.json"
"$TS_BIN/taskctl" workers
"$TS_BIN/taskctl" get TASK_UUID
```

Workers detect the lost connection, stop admissions, clean/reap/drain all old
groups and close per-Attempt resources before a fresh UUID registration. Bounded
backoff retries while Controller is unavailable. Startup recovers persisted
nonterminal Attempts as LOST/Tasks FAILED; pending and previously committed
terminal history survive. Old sessions and reservations are not restored. No
old task/log frame is replayed. Manual retry after LOST remains the user's
decision and can overlap unknown execution on an unreachable host.

For final cleanup, TERM both Workers, then Controller. Do not remove another
instance's data directory. Escaped `setsid` descendants, uninterruptible tasks,
host power loss and hostile networks are outside this demo's guarantees.

## Reproduce formal acceptance

```bash
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler \
  -L m9 --output-on-failure --repeat until-fail:20
```

The complete A–L matrix, precise results and known boundaries are in
`reports/MILESTONE_9_REPORT.md`. M9 must await independent review; this demo does
not authorize merging or M10.
