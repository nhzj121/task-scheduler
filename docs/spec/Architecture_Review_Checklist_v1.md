# Architecture Review Checklist v1
## Linux C++ Distributed Task Scheduler

Use this checklist after Codex completes the Architecture/Specification phase and **before** asking Codex to implement production code.

A result should not pass review merely because the documents are long. It passes only if the semantics are coherent across documents.

---

# 1. Scope

- [ ] Single Controller is explicit.
- [ ] Multiple Workers are explicit.
- [ ] `taskctl` operations match the requested CLI.
- [ ] Trusted-environment security boundary is prominent.
- [ ] Non-goals remain non-goals.
- [ ] No hidden Kubernetes/Docker/gRPC/Boost.Asio expansion.
- [ ] No GPU/CUDA/RDMA work slipped into v1.
- [ ] No production-readiness claim.

---

# 2. Task / Attempt model

- [ ] Task and Attempt are distinct.
- [ ] Task states are exactly or semantically equivalent to PENDING/ACTIVE/SUCCEEDED/FAILED/CANCELLED.
- [ ] Attempt states cover ASSIGNED/STARTING/RUNNING/SUCCEEDED/FAILED/TIMED_OUT/CANCELLED/LOST.
- [ ] Every state transition has an event.
- [ ] Every transition has one owner.
- [ ] Invalid transitions are documented.
- [ ] Manual retry preserves Attempt history.
- [ ] No automatic retry.
- [ ] No exactly-once claim.

---

# 3. Worker/session model

- [ ] Worker logical identity is separate from WorkerSession.
- [ ] WorkerSession is separate from TcpConnection.
- [ ] Reconnect creates a new session.
- [ ] Old session is invalidated.
- [ ] Old nonterminal Attempts become LOST.
- [ ] Heartbeat interval and offline timeout are configurable.
- [ ] Worker OFFLINE semantics are explicit.

---

# 4. Module boundaries

- [ ] `domain` has no I/O.
- [ ] `io` is generic fd/EventLoop infrastructure.
- [ ] `runtime` owns process-execution mechanics.
- [ ] `net` owns transport and buffers, not task semantics.
- [ ] `protocol` owns framing/serialization, not socket I/O.
- [ ] `scheduler` is pure/near-pure and does not perform network/storage/process side effects.
- [ ] `storage` does not decide state transitions.
- [ ] Controller orchestrates cluster state.
- [ ] Worker orchestrates local execution.
- [ ] `taskctl` remains thin.

---

# 5. Ownership

- [ ] Every owned fd has one owner.
- [ ] `UniqueFd` is move-only.
- [ ] EventLoop does not ambiguously own domain/network objects.
- [ ] ConnectionManager owns TcpConnection.
- [ ] Epoll registration is removed before fd owner destruction.
- [ ] ExecutionManager owns RunningTask.
- [ ] RunningTask ownership is documented.
- [ ] signalfd ownership is Worker/EventLoop-level.
- [ ] pid/pgid owner and reap responsibility are explicit.
- [ ] TaskManager owns live Task/Attempt state.
- [ ] WorkerRegistry owns Worker/WorkerSession state.
- [ ] Scheduler owns neither Task nor Worker.
- [ ] Destructors do not silently kill healthy workloads.
- [ ] LogWriterThread solely owns task-log namespace mutation plus file/directory fds and sync operations.

---

# 6. Runtime correctness

- [ ] Pipes are created before fork.
- [ ] child/parent pipe ends are closed correctly.
- [ ] stdout/stderr use separate channels.
- [ ] child process group is established.
- [ ] `dup2` behavior is specified.
- [ ] merged environment, argv/envp, cwd resources, and PATH resolution are prepared before fork.
- [ ] slash-containing executables bypass PATH and no-slash executables use the final merged PATH.
- [ ] the post-fork child path is limited to documented async-signal-safe setup plus `execve`.
- [ ] `exec` failure path is specified.
- [ ] stdout/stderr read ends are non-blocking.
- [ ] SIGCHLD is integrated via signalfd.
- [ ] waitpid(WNOHANG) drain/reap loop is specified.
- [ ] remaining stdout/stderr drain after process exit is addressed.
- [ ] the direct child is the workload leader and its natural result is frozen before cleanup.
- [ ] residual same-pgid processes receive SIGTERM -> grace -> SIGKILL without changing leader outcome.
- [ ] escaped-descendant and retained-pipe limitations are explicit.
- [ ] timeout uses timerfd.
- [ ] timeout/cancel use process-group SIGTERM -> grace -> SIGKILL.
- [ ] zombie prevention is explicit.

---

# 7. EventLoop / epoll

- [ ] epoll LT is explicit.
- [ ] read-until-EAGAIN policy is still correctly specified where needed.
- [ ] partial writes are buffered.
- [ ] EPOLLOUT interest is enabled only when needed.
- [ ] fd add/modify/delete lifecycle is documented.
- [ ] callback lifetime is safe.
- [ ] no blocking socket/pipe operation occurs in EventLoop.
- [ ] timerfd/signalfd/eventfd integration is coherent.

---

# 8. Protocol

- [ ] Header wire format is byte-for-byte specified.
- [ ] Numeric message type IDs are frozen.
- [ ] network byte order is specified.
- [ ] uint64 endianness is handled explicitly.
- [ ] raw C++ struct memory is not used as wire representation.
- [ ] maximum payload is defined.
- [ ] split frame is handled.
- [ ] multiple frames per recv are handled.
- [ ] partial send is handled.
- [ ] `TASK_FINISHED` cannot overtake previously generated same-Attempt `LOG_CHUNK` frames.
- [ ] finish ordering is preserved across queued logs, control priority, and partial writes.
- [ ] malformed JSON is handled.
- [ ] invalid magic/version is handled.
- [ ] role-invalid messages are handled.
- [ ] request_id semantics are clear.
- [ ] log chunk stream/sequence/encoding is specified.
- [ ] granted log credit pre-reserves matching Controller LogQueue raw-byte and command-slot capacity.
- [ ] normal log backpressure occurs before transmission and Controller keeps reading control traffic.
- [ ] failure to admit a correctly credited chunk is treated as an internal invariant failure.

---

# 9. Scheduler/resources

- [ ] Resource accounting is explicitly not cgroup enforcement.
- [ ] Worker capacity and usage are modeled.
- [ ] Scheduler filters HEALTHY Workers.
- [ ] CPU and memory feasibility checks are defined.
- [ ] feasible Worker selection minimizes projected dominant utilization across CPU and memory.
- [ ] projected-utilization ratios are compared exactly and ties use lexicographic `worker_id`.
- [ ] skipping infeasible older Tasks and the resulting starvation limitation are documented.
- [ ] resource reserve/release moments are defined.
- [ ] failure paths return/reconcile accounting resources.
- [ ] Scheduler does not send network messages itself.

---

# 10. Persistence/recovery

- [ ] SQLite schema supports Task/Attempt history.
- [ ] transaction boundaries are explicit.
- [ ] indexes/uniqueness constraints are sensible.
- [ ] large task logs are not stored as SQLite blobs.
- [ ] Controller startup recovery is explicit.
- [ ] PENDING survives restart.
- [ ] old active Attempts become LOST.
- [ ] corresponding Tasks become FAILED.
- [ ] persistence-thread ownership is explicit.
- [ ] EventLoop/persistence handoff is race-safe.
- [ ] New task/attempt directory and log-file entries have required containing-directory `fsync` before `logs_complete=true`.
- [ ] Directory synchronization is creation/namespace-only rather than per log chunk.
- [ ] A required directory-sync failure cannot produce `logs_complete=true`.

---

# 11. Failure model

- [ ] exit 0 behavior specified.
- [ ] nonzero exit behavior specified.
- [ ] unexpected signal behavior specified.
- [ ] timeout behavior specified.
- [ ] pending cancel behavior specified.
- [ ] running cancel behavior specified.
- [ ] Worker timeout behavior specified.
- [ ] Worker disconnect while running behavior specified.
- [ ] client disconnect behavior specified.
- [ ] Controller restart behavior specified.
- [ ] dispatch uncertainty acknowledged.
- [ ] retry/duplicate-execution risk documented.

---

# 12. Testing

- [ ] Unit tests cover ownership wrappers.
- [ ] Protocol framing has split/multi-frame tests.
- [ ] Scheduler has deterministic tests.
- [ ] State machine transition tests exist.
- [ ] Runtime tests include large stdout/stderr.
- [ ] runtime tests cover pre-fork PATH/environment/argv preparation and minimal child behavior.
- [ ] runtime tests cover natural leader exit with residual same-pgid descendants and preserved result.
- [ ] Timeout test exists.
- [ ] cancellation test exists.
- [ ] process-tree kill test exists.
- [ ] zombie check exists.
- [ ] cluster test uses multiple Workers.
- [ ] Worker failure test exists.
- [ ] Worker reconnect test exists.
- [ ] Controller restart test exists.
- [ ] malformed protocol test exists.
- [ ] partial-write tests prove `TASK_FINISHED` never overtakes earlier same-Attempt log frames.
- [ ] credit-capacity conservation and invariant-failure tests exist.
- [ ] log namespace creation, directory-sync failure, and crash-order durability tests exist.
- [ ] sanitizer plan exists.

---

# 13. Implementation plan

- [ ] Plan is dependency-ordered.
- [ ] Plan uses small milestones.
- [ ] Each milestone has acceptance tests.
- [ ] No “implement the whole system” milestone.
- [ ] Core interfaces are frozen before upper layers.
- [ ] Protocol/state semantics are not deferred to implementation.
- [ ] Vertical slices appear early enough to test real behavior.
- [ ] There are architecture review checkpoints.

---

# 14. Documentation quality

- [ ] Diagrams and prose agree.
- [ ] State names agree across all docs.
- [ ] Message names agree across all docs.
- [ ] Module names agree across all docs.
- [ ] Failure semantics agree with state transitions.
- [ ] Ownership agrees with cleanup sequences.
- [ ] Database schema can represent documented states.
- [ ] Test plan verifies every claimed guarantee.
- [ ] Limitations are explicit rather than hidden.

---

# Final gate

Implementation should begin only if:

- [ ] no unresolved core semantic contradiction remains;
- [ ] no ownership ambiguity remains for fd/socket/pid/task/session;
- [ ] protocol framing is frozen;
- [ ] Task/Attempt/Worker state machines are frozen;
- [ ] restart/failure behavior is frozen;
- [ ] module dependency direction is frozen;
- [ ] implementation plan is executable without inventing new core architecture.
