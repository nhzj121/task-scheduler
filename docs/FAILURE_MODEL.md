# Failure Model

v1 prefers conservative, explainable outcomes over availability. It distinguishes observed
workload failure from infrastructure uncertainty and never converts uncertainty into success.

## Failure taxonomy

| Class | Examples | What can be known |
|---|---|---|
| Workload result | exit 0, nonzero exit, signal | Worker knows after `waitpid` |
| Requested termination | timeout, user cancellation | Worker knows the first installed termination cause and final reap result |
| Launch failure | pipe/fork/setpgid/chdir/exec error | Worker can report before `RUNNING`; exec-status pipe preserves errno |
| Transport/session failure | reset, close, heartbeat timeout, malformed Worker frame | Controller cannot prove whether dispatched code ran or is still running |
| Worker process/host failure | crash, kill, reboot | Detected by connection/liveness; remote process outcome is unknown |
| Controller failure | crash, restart, process kill | New Controller trusts only SQLite and no old session |
| Metadata storage failure | SQLite I/O/full/corruption/commit error | Required transition may not be durable; external side effect must stop |
| Log storage failure | mkdir/open/write/fdatasync/directory-fsync/read error | Output or its namespace may be partial and cannot be called complete |
| Client failure | disconnect, timeout, repeated request | Does not cancel work; submit outcome may be unknown to client |
| Protocol fault | invalid header, schema, sequence, credit violation | Peer/session cannot be trusted and is closed |

## Authoritative behavior table

| Event | Attempt result | Task result | Resource/log behavior |
|---|---|---|---|
| Workload leader exits 0 | `SUCCEEDED` | `SUCCEEDED` | Freeze leader result, clean remaining same-pgid members, drain logs, final barrier, commit, release |
| Workload leader exits nonzero | `FAILED` | `FAILED` | Freeze exit code, clean remaining same-pgid members without rewriting it, then drain/barrier/commit/release |
| Workload leader receives unexpected terminating signal | `FAILED` | `FAILED` | Freeze signal result, clean remaining same-pgid members without rewriting it, then drain/barrier/commit/release |
| Launch/setup/exec failure after accepted dispatch | `FAILED` | `FAILED` | Preserve stage/errno; barrier any emitted bytes; release after commit |
| Worker explicitly rejects dispatch before launch | `FAILED` (`DISPATCH_REJECTED`) | `FAILED` | Store proven-empty complete logs; no data barrier; release after terminal commit |
| Task timeout | `TIMED_OUT` | `FAILED` | SIGTERM group, grace, SIGKILL if needed, reap/drain/barrier/commit/release |
| Cancel `PENDING` | no Attempt | `CANCELLED` | Commit Task first; no reservation or Worker command |
| Cancel active, cancellation wins | `CANCELLED` | `CANCELLED` | Persist intent before command; terminate/reap/drain/barrier/commit/release |
| Cancel active, natural result wins | observed terminal result | mapped natural result | Cancellation was accepted, not guaranteed; first EventLoop terminal fact wins |
| Worker/session loss with nonterminal Attempt | `LOST` | `FAILED` | Bulk transaction first, then release; `logs_complete=false` |
| Controller restart with old nonterminal Attempt | `LOST` | `FAILED` | Startup recovery transaction; no old reservations restored |
| Client disconnect | unchanged | unchanged | Task continues; unsent client response is discarded |
| Final log barrier failure | `FAILED` (`LOG_PERSISTENCE_ERROR`) | `FAILED` | Retain observed child outcome and `logs_complete=false`; terminalize if SQLite works |
| Required SQLite transition failure | no uncommitted transition exposed | prior durable state | No corresponding network side effect; Controller enters fail-stop handling |

## Heartbeat and detection assumptions

Worker sends `HEARTBEAT` approximately every 3 seconds; Controller declares the current session
offline after approximately 10 seconds without a valid heartbeat. Both are configurable and the
offline timeout must be greater than the interval with room for scheduling jitter. Controller
uses a monotonic clock for deadlines; wire timestamps do not determine liveness.

TCP close/reset can detect a failure earlier. Heartbeat timeout is a failure detector, not proof
of host death: process pauses, network partitions, Controller overload, and host failure look
similar. Therefore the semantic action is session invalidation and `LOST`, not a claim about the
physical machine.

Log credit prevents ordinary log congestion from filling the Worker control output indefinitely.
Heartbeats have output priority at frame boundaries and do not consume log credit. If the
connection itself cannot transmit control traffic before the offline deadline, the session is
still failed; bounded memory takes priority over indefinite tolerance.

Dispatch ACK defaults to 5 seconds and start report to 10 seconds. A missed deadline invalidates
the session because the Controller cannot safely distinguish a slow Worker from a lost command.

## LOST semantics

`LOST` means the Controller has lost authoritative observation of a concrete Attempt before a
terminal result was durably committed. It does not mean the executable definitely stopped or
never started.

Session invalidation performs one SQLite transaction for all that session's nonterminal Attempts
and corresponding Tasks. Only after commit does EventLoop release reservations and expose the
Worker as `OFFLINE`. If this transaction fails, Controller follows persistence fail-stop; it does
not release and continue scheduling on unknown state.

The Worker is required to terminate its local process groups once it detects control-session
loss. This bounds orphan lifetime but does not remove uncertainty: detection and SIGTERM/SIGKILL
take time, and a disconnected Worker may itself be partitioned or stopped.

## Dispatch uncertainty

The durable assignment precedes dispatch. The cases are:

- assignment commit fails: rollback tentative reservation; no `RUN_TASK`; Task remains
  `PENDING`;
- cancellation arrives before any dispatch byte enters the connection path: terminalize locally
  as `CANCELLED`;
- session becomes invalid before dispatch is queued: committed assignment is subsequently marked
  `LOST`; no automatic retry;
- any dispatch byte is queued/sent and ACK is missing: execution is possible, so invalidate the
  session and use `LOST`;
- explicit `RUN_TASK_ACK.accepted=false`: launch is known not to have occurred, so use `FAILED`
  with `DISPATCH_REJECTED`, not `LOST`; zero logs are complete by proof;
- cancellation before any dispatch byte enters the output path likewise has proven-empty complete
  logs. Once dispatch may have escaped, only Worker finalization can prove log completeness.

This conservative distinction is why durable assignment does not imply execution and why v1
cannot claim exactly-once.

## Cancellation

Pending cancellation is synchronous after its Task transaction. Active cancellation is an
asynchronous request:

1. persist `cancel_requested_at`;
2. return `accepted` and then queue `WORKER.CANCEL_TASK`;
3. Worker records cancellation as the termination cause if no terminal cause already won;
4. send SIGTERM to the process group and arm the grace timer;
5. send SIGKILL if it remains alive; reap and drain logs;
6. report `CANCELLED`; Controller completes log barrier and terminal transaction.

An ACK means the command was handled, not that termination completed. If the child naturally
exited before the cancel cause won, the natural outcome is authoritative. If the session is lost,
the result is `LOST`/`FAILED`, even if the user requested cancellation.

## Timeout

The workload timeout starts when exec success is confirmed and the Worker emits `TASK_STARTED`.
It is based on a monotonic timerfd. Timeout and cancellation share termination mechanics but have
different terminal mapping. The first installed termination cause wins; a later cancel does not
rewrite `TIMED_OUT`, and a timer firing after natural reap is ignored.

The Worker signals `-pgid`, never just the leader pid. After SIGKILL it continues reaping and
draining. A descendant that deliberately calls `setsid`/leaves the process group can escape v1;
cgroups/namespaces are out of scope.

## Natural workload-leader exit and residual group cleanup

The direct child is the workload leader and its observed `waitpid` result is the workload result.
When it exits without a previously installed timeout/cancel/session-loss cause, Worker freezes its
exit code or signal before cleanup. It then probes the known pgid. If same-group members remain,
Worker sends SIGTERM, waits the normal grace period, and sends SIGKILL if necessary. Cleanup signals
are lifecycle hygiene and never turn leader exit 0 into failure or replace a nonzero/signal result.

Worker waits for same-group disappearance and continues reading stdout/stderr until EOF before
generating `TASK_FINISHED`. A descendant that deliberately calls `setsid` or otherwise leaves the
group is outside the termination guarantee. If such a descendant keeps an inherited pipe write end
open, v1 completion can remain pending because the required EOF never arrives. An uninterruptible
same-group process can likewise delay cleanup despite SIGKILL; v1 does not fabricate completion.
A timeout or cancellation arriving after the leader outcome was frozen cannot rewrite it; the
Worker may acknowledge that the execution outcome is already fixed while cleanup/finalization
continues.

## Executable preparation failure

The Worker parent, not the post-fork child, merges environment overrides, resolves cwd, constructs
stable argv/envp storage, and resolves a no-slash executable using the final merged PATH. Missing
PATH, no executable candidate, invalid environment input, or inability to prepare cwd/stdin is a
pre-fork `LAUNCH_FAILED` result. If executable contains `/`, no PATH search occurs; child passes the
submitted path directly to `execve` after applying cwd. The post-fork child performs only the
documented async-signal-safe fd/pgid/cwd/mask setup and `execve`; its failure stage/errno is reported
through the exec-status pipe.

## Worker crash, disconnect, and reconnect

- Immediate connection error or heartbeat timeout invalidates the session.
- All its nonterminal Attempts become `LOST`; Tasks become `FAILED`; resources release after DB
  commit.
- A new connection must present a new UUID session, even from the same Worker process.
- A new session for an already connected `worker_id` explicitly replaces the old one and waits
  for old-session invalidation before `REGISTER_ACK`.
- The new session starts with no Controller-accounted Attempts. It cannot report or adopt old
  work.
- Worker local reconnect begins only after it terminates/reaps all RunningTasks from the old
  session.

There is no automatic retry after reconnect.

## Controller crash and restart

On restart, before accepting connections, Controller atomically converts persisted `ASSIGNED`,
`STARTING`, and `RUNNING` Attempts to `LOST`, with corresponding `ACTIVE` Tasks to `FAILED`.
`PENDING` remains queued and terminal history remains unchanged. It starts with no healthy Workers
and zero reservations. Workers reconnect with new sessions.

The Controller does not contact Workers to discover old pids or results. Log files may contain
bytes beyond metadata after a crash, and newly created names may be absent if their containing
directories were not synchronized. Without a successful file-data plus directory-namespace final
barrier and terminal transaction, logs are treated as partial and `logs_complete=false`. Startup
does not infer success from files.

## Persistence failure

Every SQLite command returns explicit success/failure. For a required transition failure:

1. do not emit the network side effect gated by that transition;
2. do not expose the uncommitted state;
3. reject new mutating client operations and stop scheduling;
4. close Worker sessions, causing Workers to terminate local work;
5. while SQLite remains capable, attempt only the bounded required cleanup transactions;
6. exit nonzero rather than continuing with ambiguous durable state.

The next process startup runs normal recovery. There is no infinite retry loop or catch-all
continue behavior. A read-only query may complete only if its source and consistency are still
known good.

SQLite commit establishes control-plane durability under the configured SQLite synchronous mode.
It does not atomically include EventLoop memory or log files. The documented order, stable IDs,
and recovery rules bridge those boundaries without claiming cross-medium ACID.

## Log failure and backpressure

The data path intentionally does not drop output:

```text
child pipe -> Worker bounded chunking -> log credit -> TCP -> Controller LogQueue
           -> LogWriter write -> credit replenishment
```

Controller grants credit only after reserving matching raw LogQueue bytes and chunk-command slots.
Outstanding credit and queued/in-flight log work are states of the same two-dimensional capacity
reservation. A valid chunk transfers its raw size plus one slot; LogWriter completion frees both.
If either credit dimension is exhausted, Worker pauses pipe reads. Kernel pipe buffers eventually
fill and block a workload's stdout/stderr
`write`; this is expected v1 flow control, not deadlock in the EventLoop. Controller continues
reading the Worker socket, and signals, timers, socket control frames, and reaping continue.

Inability to enqueue a correctly credited, otherwise valid chunk contradicts Controller capacity
accounting. It is an internal invariant failure, not ordinary congestion: do not drop the chunk or
pause socket reads as recovery. Stop grants and scheduling, enter fail-stop, durably mark
nonterminal Attempts bound to affected sessions `LOST` where persistence is healthy, close those
sessions, and exit nonzero for normal startup recovery.

Failure behavior:

- mkdir/open/append failure: stop credit for affected data, invalidate/terminate affected session as
  needed, and terminalize known completed work as `LOG_PERSISTENCE_ERROR` if metadata remains
  writable;
- final file `fdatasync` or required containing-directory `fsync` failure: never set
  `logs_complete=true`; terminalize `FAILED` with observed child outcome retained;
- log snapshot read failure: return `INTERNAL`; it does not change task execution state;
- Controller crash before final metadata commit: startup uses `LOST`, regardless of file tail.

The guarantee covers bytes up to both successful file `fdatasync`s, the required successful
directory `fsync`s for newly created namespace entries, and the later terminal metadata commit.
Directory synchronization is not performed per chunk. The guarantee assumes the configured
trusted local filesystem honors Linux file and directory sync semantics; it does not promise
survival of storage-device lies, filesystem corruption, or loss of the whole data directory.

## Protocol faults

Client semantic errors receive a bounded `ERROR_RESPONSE`. Corrupt framing closes the connection.
A Worker protocol violation invalidates the entire session because messages on a multiplexed
session can no longer be trusted. Bound Attempts become `LOST`. The frame parser never scans for a
new magic value inside corrupt payload data. `TASK_FINISHED` whose declared final sequences/bytes
do not exactly match already accepted chunks is such a violation; Controller never waits for log
chunks after finish.

## Client uncertainty

Client disconnect never cancels a Task. If the client loses a response:

- query/cancel/retry can be retried and revalidated against current state;
- a repeat cancel while intent is pending is accepted idempotently;
- submit has no idempotency key, so the client must treat a lost response as unknown and inspect
  task listings rather than assume failure; resubmitting may create a duplicate logical Task.

## Duplicate-execution risk

Manual retry creates a new Attempt after an old Attempt is already durably `LOST`/`FAILED`. The old
process can still run during connection-loss detection and termination. Thus two executions may
overlap. The system neither fences external side effects nor deduplicates workload behavior.
Users must design idempotent workloads or external fencing when duplicates are harmful.

## Explicit non-guarantees

- no exactly-once, at-most-once, or automatic retry guarantee across failures;
- no Controller availability during restart/failure;
- no Worker process reconciliation;
- no hard CPU/memory limit or protection from misleading resource requests;
- no isolation from malicious programs or users;
- no task-log replication or remote storage durability;
- no survival guarantee after data-directory loss/corruption;
- no termination guarantee for descendants that escape the process group;
- no finite completion guarantee for an escaped descendant that retains a task pipe or a process
  stuck uninterruptibly despite SIGKILL;
- no real-time heartbeat or scheduling guarantee;
- no globally consistent snapshot across paginated task-list pages;
- no live/follow log stream in v1.
