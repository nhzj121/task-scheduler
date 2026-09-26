# Ownership and Lifetime

The rule is one resource, one owner. Raw pointers, references, pids, pgids, and fd integers are
non-owning observations unless this document says otherwise. Ownership is not made ambiguous with
ubiquitous `shared_ptr`.

## Resource ownership table

| Resource/state | Sole owner | Borrowers | End-of-life rule |
|---|---|---|---|
| epoll fd and registration table | each `EventLoop` | application orchestrator | Stop dispatch, remove registrations, close epoll fd |
| listening socket | Controller `ConnectionManager` | EventLoop registration | unregister, close, erase listener |
| connected socket | one `TcpConnection` owned by `ConnectionManager` | EventLoop via token; protocol handler during callback | mark closing, unregister, defer erase, `UniqueFd` closes |
| socket input/output buffers | owning `TcpConnection` | parser/writer during callback | destroyed with connection after callback |
| Controller Task/Attempt live objects | `TaskManager` on EventLoop thread | Scheduler snapshots, protocol serializers | retained for Controller lifetime or evicted only by a future explicit cache policy |
| Worker/WorkerSession live objects | `WorkerRegistry` on EventLoop thread | Scheduler/TaskManager by stable ID | invalidate session, terminalize bound Attempts, release resources, detach connection |
| resource reservations | `ResourceLedger` on EventLoop thread | Scheduler snapshot | rollback tentative on assignment failure; release committed only after terminal commit |
| SQLite connection/statements | `PersistenceThread` | none | drain/abort by shutdown policy, finalize statements, close on that thread |
| task-log filesystem namespace and open file/directory fds | `LogWriterThread` | none | final barrier/close; on shutdown close after queued operations resolve |
| log byte/command-slot reservations | EventLoop `LogCreditLedger` | WorkerSession and LogCoordinator by value | transfer free -> granted -> queued/in-flight -> free in both dimensions; reclaim unused grants on session loss |
| persistence/log cross-thread channel | top-level `ControllerRuntime` | producer and EventLoop borrow channel refs/fd | stop producers, join threads, unregister eventfd, destroy channel |
| Worker shared signalfd | top-level `WorkerRuntime` | EventLoop/ExecutionManager | unregister after all RunningTasks reaped, then close |
| heartbeat/reconnect timerfd | Worker runtime/session manager | EventLoop | unregister then close on mode change/shutdown |
| Controller maintenance timerfd | Controller runtime | EventLoop | unregister then close at shutdown |
| RunningTask | `ExecutionManager` map | EventLoop callbacks via `(attempt_id,generation)` | remove only after leader reap, same-pgid cleanup, pipe EOF/closure, timer cleanup, and finish dependency registration or session-loss cleanup |
| child stdout read fd | its `RunningTask` | EventLoop registration | drain EOF, unregister, close |
| child stderr read fd | its `RunningTask` | EventLoop registration | drain EOF, unregister, close |
| exec-status read fd | its `RunningTask` | EventLoop registration | read launch errno or EOF, unregister, close |
| timeout/grace timerfd | its `RunningTask` | EventLoop registration | disarm, unregister, close after lifecycle resolution |
| pid/pgid | its `RunningTask::ProcessHandle` | signal/reap logic | mark reaped from `waitpid`; object cannot disappear while direct child is unreaped |
| CLI socket | client library connection object | blocking single-threaded request code | close after response/error |

## `UniqueFd`

Every owned fd is held by a move-only RAII wrapper equivalent to:

```cpp
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept;
    ~UniqueFd();
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&&) noexcept;
    UniqueFd& operator=(UniqueFd&&) noexcept;
    int get() const noexcept;
    explicit operator bool() const noexcept;
    int release() noexcept;
    void reset(int fd = -1) noexcept;
};
```

`get()` never transfers ownership. `release()` is used only at a documented ownership transfer.
The wrapper retries/handles close semantics correctly without throwing from its destructor. A
registered fd must be removed from epoll before its `UniqueFd` resets or destructs; RAII alone
does not know about epoll.

## EventLoop registrations and callback safety

EventLoop owns a registration record, not the registered object or fd. A record contains:

```text
RegistrationToken = {fd, generation, handler_kind, owner_id}
```

The epoll user datum identifies this token. At dispatch, the owning manager resolves
`owner_id/generation`; if no current object matches, the event is ignored. Callbacks do not
capture owning raw pointers. Objects requested for deletion during a callback enter a deferred
destruction list processed after the current epoll batch.

Removal order is mandatory:

1. mark the logical resource closing so no new work is queued;
2. call `EPOLL_CTL_DEL` (treat `ENOENT`/already-closed cleanup as diagnosed idempotence only);
3. invalidate/remove the registration token;
4. close/reset the owning `UniqueFd`;
5. erase/destroy the owner after callback dispatch no longer references its ID.

Fd numbers can be reused by Linux; generation validation prevents an old event from being applied
to a new object with the same integer fd.

## TCP connection ownership

```text
ConnectionManager
  owns unordered_map<ConnectionId, unique_ptr<TcpConnection>>
    TcpConnection owns socket UniqueFd, InputBuffer, control OutputQueue, log OutputQueue
```

EventLoop and protocol handlers borrow the connection only for one callback. Domain objects store
`ConnectionId` plus session generation, never an owning connection pointer. Closing a Worker
connection notifies `WorkerRegistry` by value before deferred connection destruction. Closing a
client connection discards only that request transport; it never cancels a Task.

For writes, TcpConnection owns encoded frame bytes until fully sent. A partially sent frame
cannot be moved, interleaved, or freed. Control frames may be selected before new log frames, but
the current partial frame finishes first. `TASK_FINISHED` is not an unconstrained control frame:
it owns a dependency on the last generated same-Attempt log ordinal and cannot begin sending until
all earlier same-Attempt log frames are fully sent. Heartbeats/ACKs remain eligible while that
finish frame waits.

## Worker, WorkerSession, and Connection

```text
WorkerRecord (stable worker_id; owned by WorkerRegistry)
  -> optional current WorkerSession (fresh session_id + generation)
       -> non-owning ConnectionId referring to ConnectionManager
```

- Replacing a session first invalidates its generation and stops new traffic without yet actively
  closing the old socket.
- Old bound Attempts are durably moved to `LOST`; reservations are released after commit; then the
  old connection is closed.
- Only after that completion can a new session become `HEALTHY` and receive `REGISTER_ACK`.
- Destroying a TcpConnection does not directly destroy a WorkerRecord.
- Destroying/invalidation of a WorkerSession does not erase Worker history embedded in Attempts.

## RunningTask and process ownership

```text
ExecutionManager
  owns unordered_map<AttemptId, unique_ptr<RunningTask>>
    RunningTask owns ProcessHandle, stdout/stderr/exec-status UniqueFd,
                    timeout/grace timerfd, PreparedExec storage,
                    log buffers, sequence counters and finish-send dependency
```

`ProcessHandle` records leader pid, pgid, whether group establishment succeeded, termination
cause, immutable leader outcome once reaped, group-cleanup phase, and reap status. It is move-only.
A normal destructor never sends `SIGKILL`.
Business lifecycle methods explicitly initiate cancellation, timeout, session-loss cleanup, or
Worker shutdown. The owning ExecutionManager must not destroy a live/unreaped ProcessHandle.

### Spawn ownership transfer

1. After accepted dispatch, Worker parent snapshots its environment, applies validated Task
   overrides, resolves execution cwd, merges final PATH, resolves a no-slash executable against
   that PATH, and finalizes owned argv/env strings plus null-terminated pointer arrays. It also
   opens prepared cwd and `/dev/null` fds. `argv[0]` remains the submitted executable token.
2. Parent creates stdout, stderr, and exec-status pipes with close-on-exec policy before fork.
3. On preparation or fork failure, parent retains and closes every fd; no pid owner exists.
   Preparation failure is reported as `LAUNCH_FAILED` without entering `RUNNING`.
4. Child closes parent ends, establishes its process group, maps stdout/stderr/stdin with `dup2`,
   changes to the prepared cwd, restores the prepared signal mask, closes redundant descriptors,
   and calls `execve` with already stable executable/argv/envp storage.
5. Child constructs no C++ object/container and performs no PATH lookup, environment merge,
   logging, heap allocation, or exception handling. On setup/exec failure, it writes a fixed
   stage/errno record to the exec-status pipe using async-signal-safe calls and `_exit`s.
6. Parent closes child ends immediately, creates ProcessHandle ownership, sets read ends
   non-blocking, and registers them. Its copy of PreparedExec may be released after the fork result
   is known; the child has an independent address-space copy.
7. Parent and child both attempt the benign `setpgid(pid,pid)` pattern to close the race; the
   parent records a usable pgid only after accepted outcomes.

`PreparedExec` is move-only and parent-owned. Its string vectors are fully populated before any
pointer arrays are built, so vector/string reallocation cannot invalidate argv/envp. If submitted
executable contains `/`, its string is passed directly to `execve`; otherwise its resolved parent
path is used. The merged environment is emitted in lexicographic key order after Task overrides.
Relative/empty PATH entries are resolved against the absolute prepared execution cwd, and the first
non-directory `X_OK` candidate under effective Worker credentials is retained. Missing PATH or no
candidate for a no-slash executable is a preparation error. The prepared cwd directory fd and
stdin `/dev/null` fd are also uniquely owned until fork/setup transfer.

### Reaping and final drain

The shared signalfd reports that child state changed; it does not identify ownership by itself.
ExecutionManager drains `waitpid(-1, &status, WNOHANG)` and maps each returned pid to exactly one
RunningTask. When the workload leader is reaped without an existing timeout/cancel/session-loss
cause, its exit code or signal becomes the immutable workload outcome. Worker probes the known
process group with `kill(-pgid, 0)` (`0` or `EPERM` means members may remain). Remaining same-group
processes receive SIGTERM, then SIGKILL after the normal grace timer. Those cleanup signals never
change the recorded leader outcome. Reap/outcome state is retained while stdout/stderr continue to
drain. Completion requires:

- direct child reaped;
- no known remaining member of the workload process group after cleanup (`ESRCH`), subject to the
  documented escaped-descendant limitation;
- stdout EOF observed or fd closed on diagnosed error;
- stderr EOF observed or fd closed on diagnosed error;
- all bytes converted into sequenced log frames;
- every earlier same-Attempt log frame registered as a dependency of `TASK_FINISHED`, so the
  finish frame cannot begin sending until those frames are fully sent;
- task timers disarmed/unregistered;
- `TASK_FINISHED` generated/queued with that dependency when a Controller session remains valid.

Only then may ExecutionManager erase the RunningTask. During session loss it still explicitly
terminates/reaps and closes pipes, but cannot report completion to the invalid session.

## Timer, signal, and wakeup ownership

- Worker runtime blocks `SIGCHLD` before any child is created. Its single signalfd is owned by
  WorkerRuntime, registered once, and shared logically by all RunningTasks.
- Each RunningTask owns its task timeout timerfd. When termination begins, the same owner may
  rearm it for a cancellation, timeout, session-loss, or post-leader group-cleanup grace deadline;
  there is never a free-floating timer referring to a destroyed task.
- Controller owns one maintenance timerfd for heartbeat/dispatch deadline scans rather than one
  timerfd per Worker.
- Each cross-thread channel has one eventfd owned by ControllerRuntime. Producers borrow the fd
  only while ControllerRuntime guarantees lifetime. EventLoop drains its counter and completion
  queue to `EAGAIN`.

## Persistence ownership

Persistence commands and results are values. `PersistenceThread` alone opens SQLite, creates and
finalizes prepared statements, begins/commits/rolls back transactions, and closes the handle.
`StateStore` is a command-oriented abstraction; it cannot invoke TaskManager callbacks on its
thread. EventLoop applies a successful result only if operation ID, expected state version, and
entity/session generation still match.

The persistence queue contains metadata only, never log bodies. SQLite transactions establish
durability, but in-memory resource reservations are adjusted separately in EventLoop order:
tentative reserve before assignment request, rollback on failure, commit designation after DB
success, and release only after terminal/session-loss DB success.

## LogWriter ownership

LogWriterThread owns its file/directory-handle cache and all task-log `mkdir`, `open`/`openat`,
`write`, `pread`, `fdatasync`, directory `fsync`, and `close` calls. No other Controller thread
creates, renames, unlinks, opens, or synchronizes task-log directories or files. EventLoop never
touches those fds. Commands identify a path by Task/Attempt/stream and carry owned bytes or
snapshot ranges. Paths are constructed and validated under the configured log root; user input is
never concatenated as a path component.

On every newly created task/attempt directory, LogWriter marks the containing directory dirty; on
every newly created stdout/stderr file, it marks that file's containing directory dirty. Successful
directory `fsync` transfers that namespace-durability obligation to satisfied state. The final
barrier is ordered after prior writes for an Attempt, calls `fdatasync` for both streams (including
empty files), and then satisfies any remaining directory obligations from the deepest containing
directory through the log root. Previously satisfied directories are not synced again unless a
later creation dirties them. File and directory handles may then close. A failed required
directory sync is reported as barrier failure and ownership is cleaned up without reporting
`logs_complete=true`.

Directory synchronization is only for namespace creation durability, never per `LOG_CHUNK`.
Snapshot reads are lower priority and bounded. EventLoop `LogCreditLedger` owns two partitions:
raw-byte capacity and log-command slots, each divided into free, per-session granted-but-unspent,
and queued/in-flight capacity. A chunk consumes its raw size plus one slot. Grant, chunk receipt,
LogWriter completion, and session reclaim are conservation-preserving ownership transfers.
Therefore EventLoop never blocks or suspends socket reads to enqueue valid credited data. An
admission failure after a valid transfer is a fatal internal invariant violation, not normal
backpressure. Slots reserved for essential barriers are owned by LogCoordinator and are never
converted into Worker credit; optional snapshot reads yield before that reserve.

## Copy/move policy

| Type category | Copy | Move | Reason |
|---|---|---|---|
| `UniqueFd`, `TcpConnection`, listeners, timer/signalfd wrappers | deleted | allowed where stable registration is not active | unique OS ownership |
| `RunningTask`, `ProcessHandle` | deleted | limited/controlled; map normally owns stable heap object | pid/fd lifecycle identity |
| Task/Attempt/Worker live aggregates | deleted outside owner; snapshots copyable | owner-controlled | single-thread mutation |
| scheduler input snapshots and decisions | allowed | allowed | pure value exchange |
| protocol messages | allowed/movable values | allowed | no resource ownership |
| persistence/log commands and completions | movable values | allowed | cross-thread queue transfer |
| EventLoop | deleted | deleted after construction | registrations refer to stable owner |
| PersistenceThread/LogWriterThread | deleted | deleted | thread-affine resources |

## Cleanup sequences

### TcpConnection

Stop new frames -> notify role owner -> invalidate session if needed -> unregister socket ->
discard/diagnose unsent buffers -> close fd -> deferred object erase.

### Normal RunningTask

Observe/reap leader and freeze its result -> probe pgid -> if members remain perform
SIGTERM/grace/SIGKILL without changing the result -> drain both pipes to EOF -> generate all log
frames -> generate `TASK_FINISHED` with a dependency on the final same-Attempt log ordinal -> send
all dependent log bytes before any finish byte -> unregister/close pipes and timers -> erase
RunningTask after TcpConnection owns any remaining encoded bytes. Controller-side log namespace
and file/directory fds remain owned by LogWriter until the complete data/namespace barrier and
closure.

### Cancel/timeout RunningTask

Record first termination cause -> send `SIGTERM` to negative pgid -> arm grace timer -> if still
alive send `SIGKILL` -> reap -> drain -> report result -> normal cleanup. `ESRCH` causes a reap
check, not an assumption of completion.

### Worker session loss

Worker side: invalidate protocol session -> stop launches -> explicitly terminate every process
group -> reap/drain/close -> destroy RunningTasks -> close old connection -> reconnect with fresh
session. Controller side: invalidate generation and stop placement -> durably mark bound Attempts
`LOST`/Tasks `FAILED` -> release reservations -> mark Worker `OFFLINE` -> destroy connection.

### Controller shutdown

Stop listeners and new scheduling -> logically invalidate Worker sessions and stop their traffic
-> commit required LOST transitions while persistence is healthy -> release reservations and close
Worker connections so they terminate work -> drain completion channels -> stop and join LogWriter
-> stop and join PersistenceThread -> unregister/destroy eventfds and remaining connections ->
destroy EventLoop. On abrupt crash, startup recovery supplies the conservative state transition
instead.

### Worker shutdown

Stop reconnect/heartbeat -> stop accepting commands -> explicitly terminate/reap RunningTasks ->
drain or diagnose undeliverable logs -> unregister/close per-task fds -> unregister/close shared
signalfd and timers -> close Controller connection -> destroy EventLoop.
