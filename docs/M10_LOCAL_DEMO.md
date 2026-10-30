# M10 local reproduction

The M10 report records the actual WSL commands/results. These tools execute only
trusted programs on loopback. Their root must be a fresh Linux-local directory;
they preserve data and stop only PID/UID/start-time/executable/config-verified
processes. Do not substitute an existing operator database directory.

## Build, stage, inspect

```bash
SRC=/mnt/e/Deck_all_need/projects/work/task_scheduler
PKG=/home/nhzj121/build/task_scheduler-m10-package
STAGE=/home/nhzj121/build/task_scheduler-m10-stage-final
cmake -S "$SRC" -B "$PKG" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON -DCMAKE_INSTALL_PREFIX=/opt/task-scheduler-m10
cmake --build "$PKG" -j2
DESTDIR="$STAGE" cmake --install "$PKG"
BIN="$STAGE/opt/task-scheduler-m10/bin"
TOOLS="$STAGE/opt/task-scheduler-m10/share/task-scheduler/tools"
"$BIN/controller" --help
"$BIN/worker" --help
"$BIN/taskctl" --help
```

Stage paths above belong to this validation. On another machine choose new build
and staging leaves. No privileged installation occurs here. Configure the final
prefix before building; DESTDIR does not rewrite unit ExecStart paths.

## Automated two-Worker workflow

```bash
DEMO=$(mktemp -d /tmp/task-scheduler-m10-demo.XXXXXX)
python3 "$TOOLS/local_cluster.py" demo --root "$DEMO" --bin-dir "$BIN" --workers 2
```

The JSON result contains real IDs for durable empty success, infeasible PENDING
cancellation, active cancellation, timeout and FAILED-only manual retry. The demo
checks immutable first Attempt/logs and then stops all its services. It leaves
metadata and diagnostics under DEMO. It does not claim that observing a cancel ACK
alone proves termination. Use a new root to rerun the demo.

## Interactive commands

```bash
LOCAL=$(mktemp -d /tmp/task-scheduler-m10-interactive.XXXXXX)
python3 "$TOOLS/local_cluster.py" start --root "$LOCAL" --bin-dir "$BIN" --workers 2
PORT=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["client_port"])' "$LOCAL/cluster.json")
python3 "$TOOLS/local_cluster.py" status --root "$LOCAL"
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" --json workers
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" --json submit /bin/true
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" --json submit --timeout 1 /bin/sleep 30
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" --json submit /bin/sleep 30
# Copy the actual ID from the preceding submit response:
TASK=REPLACE_WITH_SUBMITTED_UUID
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" --json cancel "$TASK"
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" --json get "$TASK"
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" list
"$BIN/taskctl" --host 127.0.0.1 --port "$PORT" logs "$TASK" --attempt 1 --both
python3 "$TOOLS/local_cluster.py" stop --root "$LOCAL"
```

`get` must eventually show CANCELLED and logs_complete, not just an accepted
response. Timeout maps Attempt TIMED_OUT to Task FAILED. Retry accepts only a
FAILED Task: `taskctl ... retry TASK_UUID`; see the automated deterministic
precondition example in `scripts/local_cluster.py` rather than retrying a workload
whose outcome is unknown. `--both` concatenates finite stdout/stderr snapshots;
there is no live follow or interleaved ordering guarantee.

## Systemd and recovery

Conventional service provisioning is an explicit operator action described in
[OPERATIONS](OPERATIONS.md). The actually executed privileged test instead used
uniquely named temporary accounts/units and independent roots, after owner
permission. It stopped a running Worker, verified LOST and reaping, started a new
session, restarted Controller during RUNNING, verified PENDING/terminal history
and LOST recovery, then restarted Worker again and verified another new session.

The development-only command is:

```bash
# Requires root plus explicit permission; choose a new artifact leaf.
python3 "$SRC/scripts/systemd_validation.py" --allow-temporary-system-services \
  --bin-dir "$BIN" --units-dir "$STAGE/opt/task-scheduler-m10/lib/systemd/system" \
  --artifacts /home/nhzj121/build/task_scheduler/m10-systemd-NEW
```

It is not installed as an operator tool and never deploys the conventional
service names. The final recorded run and cleanup identity are in the M10 report.
Physical power failure, a native host, public networking, authentication,
resource enforcement and production deployment were not validated or introduced.
