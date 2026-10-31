# Operator guide

This implements the frozen deployment intent without changing its semantics.
Use owned local Linux storage supporting file `fdatasync` and directory `fsync`.
Do not use DrvFs/network filesystems as formal durable-log storage. Defaults are
loopback client 7420 and Worker 7421. Remote use requires a trusted private interface
and firewall rules restricted to known hosts; there is no authentication or TLS.

## Installation

Install dependencies/build as in README. Configure the actual final prefix before
building: systemd ExecStart paths are generated from `CMAKE_INSTALL_PREFIX`.

```bash
cmake -S . -B "$HOME/build/task-scheduler-release" -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build "$HOME/build/task-scheduler-release" -j4
# Inspect staging before any privileged install:
STAGE=$(mktemp -d /tmp/task-scheduler-stage.XXXXXX)
DESTDIR="$STAGE" cmake --install "$HOME/build/task-scheduler-release"
"$STAGE/usr/local/bin/controller" --help
```

Staging installs only three executables, public docs, tooling, examples and unit
files. It does not create users, install live configs, enable services, or touch a
database. `BUILD_TESTING=OFF` needs no GTest. Python is required only for tools.
Installed README/operations/development/source-map guides are flat copies under
the GNUInstallDirs documentation directory; repository navigation links refer to
the checkout, not an installed source tree.
Units use prefix-relative `lib/systemd/system` (configurable via
`TS_SYSTEMD_UNIT_DIR`), never a multiarch library directory. Do not use
`cmake --install --prefix` to relocate already-generated service ExecStart paths;
reconfigure with the intended prefix instead.

## Privileged service provisioning (explicit operator action)

The following is an installation recipe, not an instruction that the build runs.
Inspect existing accounts/configs first; never overwrite an existing deployment.
For a fresh host after review:

```bash
(
set -eu
# Fail before provisioning if any destination configuration already exists.
test ! -e /etc/task-scheduler/controller.json
test ! -e /etc/task-scheduler/worker-a.json
test ! -e /etc/task-scheduler/worker-b.json
sudo cmake --install "$HOME/build/task-scheduler-release"
sudo useradd --system --user-group --no-create-home --shell /usr/sbin/nologin task-controller
sudo useradd --system --user-group --no-create-home --shell /usr/sbin/nologin task-worker
sudo install -d -m 0755 /etc/task-scheduler
# These guards deliberately fail if operator configs already exist.
test ! -e /etc/task-scheduler/controller.json
sudo install -m 0640 -o root -g task-controller \
  /usr/local/share/task-scheduler/examples/controller.json /etc/task-scheduler/controller.json
for name in a b; do
  test ! -e "/etc/task-scheduler/worker-$name.json" || exit 1
  sudo install -m 0640 -o root -g task-worker \
    "/usr/local/share/task-scheduler/examples/worker-$name.json" "/etc/task-scheduler/worker-$name.json"
done
sudo systemctl daemon-reload
sudo systemctl start task-controller.service task-worker@a.service task-worker@b.service
sudo systemctl status task-controller.service task-worker@a.service task-worker@b.service
sudo journalctl -u task-controller.service -u task-worker@a.service -u task-worker@b.service --since today
)
```

Use the template for two independent Workers, with distinct stable IDs. The
single `task-worker.service` instead reads `worker.json`; do not run it alongside
worker@a with the same worker-a ID. Accounts are non-root; both Worker instances
share their dedicated Worker account and physical host resources. This is not
tenant isolation. StateDirectory creates `/var/lib/task-scheduler/controller`
for the Controller account, mode 0700; UMask 0077 protects metadata/log files.

Units use Restart=on-failure, RestartSec=3s, LimitNOFILE=16384 and a finite 45s stop
timeout. This exceeds the example 2s group grace plus normal drain/storage shutdown
margin; permanently blocked storage or uninterruptible processes can still exceed
it. Worker KillMode=mixed gives the main process its normal TERM cleanup first;
systemd's final timeout enforcement is not a guarantee of successful log drain.
No restrictive filesystem/sandbox directive is added that would silently prevent
trusted executable/cwd access. The inherited service PATH is explicit; Task env
overrides remain supported. Non-root reduces privilege but is not a sandbox.

```bash
sudo systemctl restart task-controller.service
taskctl workers
sudo systemctl stop task-worker@a.service task-worker@b.service
sudo systemctl stop task-controller.service
```

Controller restart makes old ACTIVE Attempts LOST/Tasks FAILED; Workers clean up
old groups before registering fresh Session UUIDs. It does not resume old PIDs or
auto retry. SIGTERM/INT use signalfd/owner loops, not asynchronous cleanup handlers.
SIGKILL Worker cannot run cleanup; a test harness/operator must own any deliberate
orphan cleanup. Escaped setsid descendants remain outside v1 guarantees.

## Configuration and diagnostics

Examples are in `examples/deployment`; exact defaults/range/unknown-field checks
are implemented in `src/domain/config.cpp`. Config files are bounded to 1 MiB and
reject duplicate JSON keys. Roles/listeners are separate, and worker_id must be
unique in a Controller trust domain. Advertised CPU/memory/max concurrency are
placement accounting, not kernel limits. The process lock protects the whole data
directory; SQLite WAL writer locking is a separate layer.

Expected nonzero startup errors include missing/invalid config, bad permissions,
symlink data-root leaf, non-directory/unwritable root, conflicting Controller lock
and unavailable bind. Do not point failures at an alternate data directory to
pretend a restart succeeded. Inspect service journal and the known data root.

System logs use spdlog stderr captured by journald. Workload bytes live only in
task-log files, not system logs; don't log submitted environment values or raw
arguments. Configure journald retention through host policy. There is no active
task-log rotation, retention, quota or compression. Watch disk space/inodes/fds.

- OFFLINE/LOST: inspect connectivity, identity and heartbeat; never infer host death.
- PENDING: check feasibility and healthy Worker capacities; resources don't combine
  across Workers for one task.
- ACTIVE/finalization_pending: logs may still be draining/syncing; do not manually
  release reservations or mark success.
- Slow high-output jobs: finite credit/queues propagate pipe backpressure; no logs
  are intentionally dropped during a valid session.
- SQLite uncertainty/full/I/O: Controller fails stop; inspect storage before restart.
- LOG_PERSISTENCE_ERROR: logs are incomplete even if the workload exited zero.

## Backup/restore

Supported simple backup: stop Controller (and allow Workers to clean up), copy the
entire owned data directory including SQLite/WAL-related files and task logs
consistently, then restart. Keep owner/modes correct. No coordinated online backup,
replication, archive API or automatic orphan-file reconciliation is provided.
Never delete log files/metadata independently while active. Partial restores can
yield missing or incomplete logs; files alone cannot establish terminal success.

## Verified test environment

M10's separate service validation uses uniquely prefixed temporary units/accounts
and a distinct StateDirectory, after explicit owner permission. It does not deploy
these conventional service names over an operator's installation. Exact commands
and verified/limited gates are recorded in the M10 acceptance report.
