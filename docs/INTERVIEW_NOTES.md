# Interview Notes

This file maps the planned implementation to systems concepts. It describes the approved v1
design, not features already implemented during the architecture phase.

## What the project is intended to demonstrate

| Area | Concrete v1 evidence |
|---|---|
| Modern C++ | move-only fd ownership, value-based cross-thread messages, explicit object lifetime, bounded containers |
| Linux process model | fork/exec status channel, pipe/dup2 close discipline, pgid signaling, waitpid reaping |
| Linux event I/O | epoll LT over sockets, pipes, signalfd, timerfd, eventfd |
| Networking | TCP byte stream framing, partial read/write, buffering, endian conversion, role/session lifecycle |
| Concurrency | single-writer EventLoop plus dedicated SQLite and log threads with bounded queues |
| Distributed semantics | Task/Attempt split, sessions, heartbeat suspicion, LOST, dispatch uncertainty, manual retry |
| Scheduling | multi-resource feasibility, deterministic projected dominant utilization, reservation lifecycle |
| Persistence | explicit SQLite transactions, state versions, startup recovery, durability-before-side-effect |
| Data plane | binary-safe sequenced logs, credit flow control, backpressure, final durability barrier |
| Testing | state/protocol unit tests, fork/runtime tests, multi-process fault injection, sanitizers |

## Core explanations

### Why Task and Attempt are separate

A Task represents the user's logical request. An Attempt represents one concrete execution bound
to one WorkerSession. Failure can make execution outcome unknown; manual retry must create another
execution without erasing the first. One object cannot accurately represent both the stable user
intent and multiple immutable execution histories.

### Why Worker, WorkerSession, and TcpConnection are separate

`worker_id` survives restarts. `session_id` identifies one registered runtime continuity. A TCP
connection is only a transport object and can disappear independently. Treating reconnect as the
same execution authority would allow stale messages and resources to mutate current state. v1
invalidates the old session and uses `LOST` instead of attempting reconciliation.

### Why no exactly-once guarantee

After Controller sends dispatch, the connection can fail before ACK. The executable might not
have started, might be running, or might have completed. SQLite can record intent before sending,
but it cannot atomically commit with fork/exec on another host. The honest result is uncertainty
(`LOST`). Manual retry can duplicate external side effects, so idempotency belongs to the workload
or a future fencing design.

### Why durable state precedes external side effects

If dispatch happened before assignment persistence, a Controller crash could leave a remote
process with no durable Attempt. Persisting first guarantees recovery has a record to mark LOST.
The same principle applies to submit responses and cancel commands. It reduces ambiguity but does
not create a distributed transaction or exactly-once execution.

### Why resource accounting is not enforcement

Controller subtracts requested CPU slots/memory to avoid intentional oversubscription. Without
cgroups it cannot stop a task from consuming more than requested. Adding cgroups would introduce
privilege, hierarchy, measurement, and cleanup semantics outside v1. The architecture states the
limitation instead of presenting bookkeeping as isolation.

### Why epoll Level Triggered

LT is feasible for the target scale and easier to validate: readiness remains visible until the
condition is drained. Callbacks still read/write until `EAGAIN`, which handles throughput and
prepares for ET concepts without its missed-edge failure mode. A thread per fd would obscure the
fd-centric ownership/concurrency lesson.

### Why signalfd

Traditional asynchronous signal handlers can call only a small async-signal-safe set and make
shared C++ state dangerous. Blocking `SIGCHLD` and consuming it through signalfd turns notification
into normal EventLoop input. signalfd does not reap; `waitpid(-1, WNOHANG)` must still drain all
exited children.

### Why timerfd

timerfd makes timeouts and grace deadlines normal fd events with monotonic-clock semantics. Each
RunningTask owns its task timer, preventing a timer callback from outliving process ownership.

### Why process groups

Signaling only the leader can leave descendants running. A new pgid permits SIGTERM/SIGKILL to the
group. This is still weaker than cgroups: a process that deliberately creates a new session can
escape, which v1 documents. On natural leader exit, v1 freezes the leader result, cleans any
remaining same-pgid processes with TERM/grace/KILL, then drains pipes to EOF; cleanup signals do
not rewrite success or failure.

### Why an exec-status pipe

Fork success is not exec success. A close-on-exec pipe gives a precise handshake: successful exec
closes it automatically, while child setup/exec failure writes errno before `_exit`. This prevents
reporting `RUNNING` for a process that never execed. Merged environment, argv/envp, cwd resources,
and no-slash PATH lookup are built in the parent before fork. The child uses prepared storage and
only async-signal-safe setup syscalls plus `execve`.

### Why output must drain after process exit

`waitpid` can report the leader exit while bytes remain buffered in stdout/stderr pipes or are
still held by group descendants. Removing RunningTask immediately truncates logs. Completion waits
for reap plus both EOFs, then queues `TASK_FINISHED` after all chunks.

### Why logs can block a workload

Lossless logs, bounded memory, and an arbitrarily fast producer cannot all coexist without
backpressure. Credits cap the Controller data path; when exhausted, Worker stops reading pipes and
the kernel eventually blocks the producer. Each granted byte already reserves Controller
LogQueue capacity, so the Controller continues reading control traffic; failure to admit a valid
credited chunk is a bug/invariant failure. Dropping data or allocating unbounded memory would
violate stronger correctness/safety requirements.

`TASK_FINISHED` is not ordinary priority control traffic. It carries a per-Attempt dependency on
the final generated log ordinal and cannot begin sending until every earlier same-Attempt log
frame is fully sent, including partial-write tails. This is what makes TCP ordering usable by the
Controller's final log barrier.

### Why metadata and log threads are separate

Task/Attempt transitions are small latency-sensitive control operations. Logs are a potentially
large sequential data stream. Sharing one queue/thread allows a noisy task to delay assignment,
cancellation, heartbeat-loss persistence, and terminal state. Separate owner threads isolate
those workloads while the EventLoop explicitly joins them at finalization.

### Why terminal visibility waits for the log barrier

TCP ordering only proves `TASK_FINISHED` arrived after log frames; it does not prove the Controller
wrote their tail or durably created the directory entries naming new files. Controller validates
final counts, waits for all writes, calls `fdatasync` on both files, completes required containing-
directory `fsync`s for namespace creation, then commits terminal metadata. Directory sync is not
done per chunk. This makes `logs_complete=true` meaningful on the trusted local filesystem; it
does not claim survival of disk corruption or loss of the data directory.

### Why one EventLoop writer plus queues

Keeping live cluster state on one thread removes most mutex/data-race problems and gives events a
clear order. SQLite and file I/O cannot safely block that thread, so dedicated owners receive
values through bounded queues and return generation-tagged results through eventfd wakeups. A
general pool would complicate ordering without helping the v1 workload.

### Why SQLite

SQLite is deployable as one file, teaches schema/transaction/recovery work, and is adequate for a
single Controller. A network DB adds operations and distributed dependencies without solving
Worker execution atomicity. One owner thread also avoids accidental concurrent connection use.

## Likely interview questions

### C++ and ownership

1. Why is `UniqueFd` move-only, and why is closing an fd not enough when it is in epoll?
2. How do you prevent a stale epoll event from targeting a reused fd number?
3. Why not store every object in `shared_ptr`?
4. Which objects are thread-affine, and how are results safely returned?
5. Why must a ProcessHandle destructor not silently kill a live workload?

### Processes and signals

1. What is copied across fork, and what is safe before exec in a multithreaded process?
2. Why create pipes before fork and close unused ends immediately?
3. How does `dup2` establish stdout/stderr, and why preserve separate streams?
4. How does the exec-status pipe distinguish exec success?
5. Why loop `waitpid(..., WNOHANG)` after one SIGCHLD notification?
6. What race exists around `setpgid`, and how is it handled?
7. Why can EOF occur after the direct child is reaped?
8. Why are PATH lookup, environment merge, and argv/envp construction forbidden in the child?
9. Why do residual-group cleanup signals not change the leader result?

### Networking/EventLoop

1. Why does one `recv` not equal one message?
2. How are split frames and multiple frames per read handled?
3. Why serialize every header field rather than send a C++ struct?
4. What happens when `send` writes only part of a frame?
5. When is `EPOLLOUT` enabled, and why not keep it enabled permanently?
6. How do log credit and control-frame priority avoid unbounded memory and heartbeat starvation?
7. Why must `TASK_FINISHED` have a dependency despite control-frame priority?
8. What happens if a correctly credited chunk cannot enter LogQueue?

### Distributed semantics

1. What exactly does `LOST` mean?
2. Why does heartbeat timeout not prove a Worker is dead?
3. Why terminate workloads when the Controller connection is lost?
4. Can retry execute work twice? Give the timing window.
5. Why does new registration with the same worker ID invalidate the old session?
6. What does Controller restart do with PENDING versus ACTIVE Tasks?

### Persistence and ordering

1. Which transitions use one SQLite transaction?
2. Why persist assignment before `RUN_TASK`?
3. Is resource reservation atomically committed with SQLite? Why not?
4. How do tentative reservations prevent overcommit during async persistence?
5. How do state versions and operation generations reject late completions?
6. What happens on SQLite commit failure?
7. Why is the log barrier separate from the metadata transaction?

### Scheduling

1. What makes a Worker feasible?
2. Define projected dominant utilization and the deterministic tie-break.
3. When are resources reserved and released?
4. Why is Scheduler near-pure and why does that improve tests?
5. What cannot this accounting model guarantee without cgroups?

## Trade-off alternatives

| Topic | v1 choice | Plausible alternative | Why not v1 |
|---|---|---|---|
| I/O | epoll LT | ET, io_uring, thread/connection | more subtle or outside learning scope |
| Protocol | fixed header + JSON | Protobuf/gRPC | hides framing; extra tooling |
| DB | SQLite writer thread | PostgreSQL/pool | operational and concurrency expansion |
| Execution | fork/exec process group | container/cgroup backend | isolation scope explosion |
| Failure | LOST + manual retry | reconciliation/automatic retry | requires fencing/idempotency design |
| Logs | credit + files | drop, unbounded RAM, external service | violates correctness or adds infrastructure |
| State concurrency | one EventLoop writer | locks/thread pool/actors | harder ordering for small target scale |
| Controller availability | one process | HA consensus | explicitly non-goal |

## Implemented guarantee versus future idea

During the current architecture phase, none of the runtime guarantees is implemented yet. After
the later implementation passes its specified tests, the intended v1 guarantees are limited to:

| Intended tested v1 guarantee | Not guaranteed / future idea |
|---|---|
| one durable Task/Attempt history on one Controller | replicated/HA metadata |
| conservative LOST on uncertainty | remote reconciliation or exactly-once |
| manual retry preserving history | automatic retry policy |
| accounting-based placement | cgroup enforcement and utilization feedback |
| process-group timeout/cancel | container isolation and escaped-process control |
| lossless bounded local log path | replicated/retained/external log service |
| trusted private TCP | TLS, auth, RBAC, multi-tenancy |
| deterministic two-resource scheduling | priorities, preemption, GPUs, topology, gang scheduling |
| finite task/log queries | live streaming UI and analytics |

Interview answers must not present a future idea as implemented. The strongest answer states the
actual guarantee, the failure window, why the v1 trade-off is coherent, and what additional
mechanism a stronger guarantee would require.
