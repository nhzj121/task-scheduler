# Test Strategy

## Approved post-v1 capacity admission and wait

Retain prior test names and coverage. New admission_wait cases check zero known
capacity, joint single-node bounds, busy/OFFLINE advertisements, inactive
replacements, and side-effect-free submit/retry rejection. Check JSON/plain
capacity errors and dedicated CLI status independently from uncertain transport
failure. Wait coverage includes Task-vs-historical-Attempt state, monotonic
connect/request/sleep budgets, failure classification, signal interruption, fd
cleanup and actual file/terminal commit gates with real Controller/Worker/SQLite.
Run full CTest, twenty consecutive executions per new case and affected
ASan/UBSan/LSan validation; do not weaken existing crash or ordering assertions.

This is the pre-implementation verification contract. Tests must assert observable semantics,
resource cleanup, persistence order, and bounded behavior, not only happy-path return values.

## Test layers

| Layer | Process scope | Purpose |
|---|---|---|
| Unit | in-process | values, state rules, parser, scheduler, RAII, accounting, config |
| Component | in-process or forked helper | EventLoop, transport buffers, StateStore, LogWriter, runtime |
| Runtime integration | Worker runtime plus helper workload | fork/exec/pipes/signals/timers/reaping |
| Cluster integration | real Controller/Workers/taskctl on loopback | complete workflows and persistence/log ordering |
| Fault injection | real processes/sockets/files | uncertainty, crash, malformed input, storage failure |
| Sanitizer/tooling | selected suites | memory, UB, races, fd/zombie evidence |

All integration tests use temporary, test-owned directories and dynamically allocated loopback
ports. They must not depend on sleeps as correctness assertions: use bounded polling/event probes
with diagnostic deadlines. Every spawned process is tracked and cleaned up even on assertion
failure.

## Unit and component matrix

### Common/io/ownership

- `UniqueFd` default invalid state, close-on-destruction, move construction/assignment, `reset`,
  `release`, self-move defense as designed, and no double close after fd-number reuse;
- epoll add/modify/delete, LT drain behavior, stale registration generation ignored, deferred
  self-removal, and `EPOLLOUT` disabled when drained;
- timerfd expiration count drain; signalfd drain; eventfd cross-thread wakeup and queue drain;
- ConnectionManager closes/unregisters before fd owner destruction;
- bounded queue high/low-water behavior and stop semantics.

### Domain/state machine

Table-driven tests cover every valid Task and Attempt transition in `STATE_MACHINE.md`, including
owner event and terminal mapping. Every listed invalid transition is rejected. Property-style
sequences assert:

- at most one nonterminal Attempt per Task;
- terminal Attempts never change;
- Task `ACTIVE` iff one committed Attempt is nonterminal;
- retry preserves old Attempt rows and increments `attempt_no`;
- cancel intent alone never makes an active Task terminal;
- stale session/generation events cannot change state.

### Protocol and net

- exact 20-byte header golden vector for magic/version/type/request ID/length;
- explicit 64-bit network byte-order round trip;
- one-byte-at-a-time split header and payload;
- every split point in a frame; multiple frames in one receive; complete frame plus partial next;
- zero/maximum/over-maximum lengths without pre-validation allocation;
- invalid magic/version/type/role; zero request ID where nonzero is required;
- malformed UTF-8, malformed JSON, duplicate/unknown/missing fields, wrong scalar types, integer
  boundary/overflow cases;
- base64 errors, raw-size mismatch, per-stream sequence gap/duplicate, final-count mismatch, and
  log-credit violation;
- output buffer partial writes, `EINTR`, `EAGAIN`, peer close mid-frame, control priority at frame
  boundary, and partially sent frame non-interleaving;
- queue several stdout/stderr log frames for one Attempt, then generate `TASK_FINISHED`; force
  one-byte/short writes and repeated `EAGAIN`, and assert no finish byte reaches the socket before
  the last byte of every earlier same-Attempt log frame;
- prove heartbeat/ACK frames may bypass queued logs while the ineligible finish barrier remains
  blocked, and prove unrelated Attempt logs do not unnecessarily block that finish;
- encode/decode examples for every frozen message ID and schema;
- lightweight mutation/fuzz-style parser test with bounded allocation and no crash/hang.

### Scheduler/resources

- filter `OFFLINE` and stale sessions;
- exact CPU boundary, memory boundary, zero-memory request/capacity rule, max concurrency;
- infeasible result when any dimension fails;
- projected dominant-utilization ranking across CPU- and memory-heavy requests;
- exact rational comparison on close ratios without floating-point tie drift;
- lexicographic `worker_id` tie-break independent of input order;
- infeasible oldest Task is skipped while a later feasible Task can place;
- tentative reservation blocks a second placement; failed assignment rolls it back;
- committed reservation releases only after terminal/session-loss commit completion;
- no negative usage, overflow, or capacity oversubscription across randomized reserve/release
  sequences.

### SQLite StateStore/PersistenceThread

- schema creation, exact schema version, constraints, foreign keys, indexes, and prepared
  statements;
- submit/assignment/intermediate/terminal/cancel-intent/retry/session-loss/recovery transaction
  success and rollback on injected statement failure;
- Task+Attempt terminal atomicity observed from a second connection after commit;
- state-version conflict reports failure and leaves data unchanged;
- uniqueness of `(task_id, attempt_no)` and preservation of history;
- persistence completion generation rejects a stale EventLoop operation;
- Controller recovery keeps `PENDING`, converts all nonterminal Attempts to `LOST`, maps Tasks to
  `FAILED`, and retains terminal rows;
- SQLite busy/full/I/O error path gates external side-effect mock and enters fail-stop mode;
- queue bound causes backpressure/rejection without blocking EventLoop.

### LogWriter/LogCoordinator

- deterministic safe path creation; traversal-like IDs cannot escape log root;
- creation tracks namespace-dirty parent directories: a new task/attempt directory requires its
  parent to be synchronized, and new stdout/stderr entries require their containing directory to
  be synchronized;
- separate stdout/stderr ordering, binary bytes including NUL/non-UTF-8, large multi-chunk append;
- write completion advances visible offset and replenishes exactly consumed credit;
- no credit replenishment before write completion;
- every grant atomically moves raw bytes and chunk slots from free capacity to per-session
  outstanding reservation; receive transfers both to queued/in-flight ownership; disconnect
  reclaims unused credit; aggregate grants can never exceed either capacity;
- randomized credit-capacity conservation sequences assert
  `free + granted + queued_or_inflight == configured_capacity` independently for bytes and slots;
- many one-byte chunks cannot exhaust unreserved command entries because each consumes a granted
  slot; byte credit alone is insufficient to transmit;
- Worker decrements byte/slot credit when generating a chunk, never reads beyond current credit,
  and cannot build an unbounded unsent log-frame queue on a stalled socket;
- Worker grants never consume the reserved finalization-command slots, and snapshot reads cannot
  starve a required final barrier;
- a correctly credited chunk always admits even at the configured limit; an injected mismatch
  between reservation accounting and actual queue admission triggers the internal-invariant
  fail-stop path, never normal socket-read suspension or log dropping;
- final barrier occurs after all writes, calls both stream `fdatasync`s, then `fsync`s every
  remaining namespace-dirty containing directory deepest-to-root, returns final sizes, and enables
  terminal transaction only afterward;
- directory sync is performed for namespace creation but not per chunk; a directory already
  synchronized and not dirtied again is not redundantly synced at every Attempt barrier;
- explicit dispatch rejection and pre-dispatch cancellation expose proven-empty complete logs,
  while LOST never uses that shortcut;
- write/file-sync/directory-sync barrier error produces `logs_complete=false` and
  `LOG_PERSISTENCE_ERROR` behavior;
- snapshot read fixes `snapshot_end`, pages exact bytes, and excludes later appends;
- read requests cannot starve writes/barriers; bounded queue never exceeds byte or slot limits.

### Config

- valid Controller/Worker JSON; defaults; unknown field rejection;
- invalid ports/addresses, queue sizes, capacities, heartbeat relationship, negative/overflow
  durations, paths, and duplicate worker IDs as applicable;
- security-safe defaults bind loopback.

## Runtime integration matrix

Use tiny deterministic helper workloads built only in the implementation phase.

| Scenario | Assertions |
|---|---|
| exit 0 | exec confirmed, `RUNNING`, exit code 0, reaped, `SUCCEEDED` mapping |
| exit nonzero | exact exit code, `FAILED` mapping |
| SIGSEGV/self-signal | exact signal, unexpected-signal `FAILED` |
| nonexistent executable | exec-status errno, never `RUNNING`, launch `FAILED` |
| invalid cwd/environment edge | explicit launch failure, no leaked child/fd |
| PATH lookup with merged override | parent selects executable using overridden PATH; child uses prepared `execve` path |
| slash executable | no PATH lookup; relative slash path resolves after prepared cwd |
| missing PATH/no-slash executable | deterministic pre-fork launch preparation failure |
| prepared argv/envp stability | inherited duplicate/Task override precedence, lexicographic env order, exact argv, stable pointers through fork, no child transformation hook |
| stdout only / stderr only / interleaved | bytes exact and streams separate |
| arbitrary binary output | byte-for-byte after base64 path |
| output larger than pipe/socket/window/queue | no loss/deadlock; workload is observably backpressured and later completes |
| timeout | monotonic deadline, SIGTERM then optional SIGKILL, `TIMED_OUT` |
| cancel cooperative | SIGTERM observed, `CANCELLED`, logs drained |
| cancel uncooperative | SIGKILL after configured grace, reaped |
| process tree in same pgid | leader and descendants terminated |
| leader exits 0 while same-pgid child holds stdout open | leader result remains success; child receives TERM then KILL if needed; EOF/log tail precedes finish |
| leader exits nonzero/signal with residual group | cleanup signals do not overwrite recorded exit/signal outcome |
| descendant calls `setsid` and retains pipe | test demonstrates documented lack of termination/finite-completion guarantee and cleans fixture externally |
| rapid exit before registration events settle | reap status retained until pipe EOF; no missing tail |
| many simultaneous child exits | `waitpid(WNOHANG)` loop drains all; no zombies |

After each test, inspect `/proc/<worker>/fd` against a baseline where practical and use
`waitpid`/`/proc` checks to prove no direct-child zombie remains. A deliberate `setsid` escape test
documents, rather than contradicts, the v1 limitation.

## Cluster integration scenarios

1. **One Worker happy path:** submit, durable `PENDING`, placement, `STARTING`, `RUNNING`, exact
   logs, `SUCCEEDED`, restart query retains history.
2. **Multiple Workers:** advertise different resources, submit chosen requests, verify exact
   dominant-load choice and tie-break.
3. **No feasible Worker:** Task stays `PENDING`; later Worker registration/resource release
   triggers placement.
4. **Pagination:** more than two task pages; `taskctl list` returns every expected ID once.
5. **Log snapshot:** read while running, append more, prove initial snapshot end stays fixed; a new
   invocation sees later bytes.
6. **Terminal/log order:** delay LogWriter/barrier and prove status remains nonterminal with
   `finalization_pending`; release barrier, commit, then prove terminal and complete tail.
7. **Pending cancel:** no Attempt, durable `CANCELLED`, no Worker command.
8. **Active cancel:** response is `accepted/ACTIVE`; later `CANCELLED` only after reap/log barrier;
   repeat request is idempotent while pending.
9. **Cancellation race:** force natural exit before cancel handling and retain natural outcome.
10. **Timeout:** exact terminal mapping and final logs.
11. **Manual retry:** failed Attempt 1 retained; Task `PENDING`; Attempt 2 later created and can
    succeed on another Worker.
12. **Client disconnect:** close after submit bytes and prove committed work continues; document
    unknown response case.
13. **Idle Worker disconnect/reconnect:** `OFFLINE`, fresh session, then `HEALTHY`.
14. **Running Worker disconnect:** Attempt `LOST`, Task `FAILED`, reservation released only after
    DB commit; Worker terminates local group before reconnect.
15. **Replacement registration:** same worker ID/new session invalidates live old session before
    new ACK.
16. **Heartbeat timeout:** pause/drop heartbeat while socket stays open; observe same loss path.
17. **Controller restart:** retain `PENDING`, convert old active Attempt to `LOST`, Task to
    `FAILED`, zero reservations, require new sessions.
18. **Backpressure isolation:** saturate log path and verify bounded memory, workload pipe pressure,
    continued socket reads and heartbeat/cancel control handling, and no byte loss.
19. **Finish wire barrier:** stall Worker socket with queued/partially written log frames, let the
    leader exit, and prove Controller cannot parse `TASK_FINISHED` until all prior same-Attempt log
    frames arrive in full and in order.

## Fault-injection matrix

| Fault | Injection | Required observation |
|---|---|---|
| kill Worker | `SIGKILL` Worker during RUNNING | Controller detects, commits LOST/FAILED, releases after commit |
| drop/reset Worker TCP | test proxy or socket fault hook | same session-loss semantics; Worker local termination |
| delayed/missing RUN_TASK_ACK | Worker test hook | deadline invalidates session, Attempt LOST, no retry |
| child killed externally | signal child | unexpected signal FAILED unless requested termination already won |
| malformed client frame | byte injector | bounded error/close, Controller survives |
| malformed Worker/log sequence | byte injector | session invalidation and LOST |
| SQLite statement/commit failure | failpoint or full/read-only test DB | no gated RUN/CANCEL/response side effect; fail-stop |
| log append failure | failpoint or controlled filesystem error | no drop/false completeness; infrastructure FAILED if DB works |
| final file `fdatasync` failure | LogWriter failpoint | `logs_complete=false`, no success result exposed |
| required directory `fsync` failure | LogWriter failpoint on each newly created namespace level | `logs_complete=false`, `LOG_PERSISTENCE_ERROR`, no success result exposed |
| crash after file `fdatasync` but before/after directory `fsync` | deterministic barrier + remount/restart-capable Linux filesystem harness where available | terminal metadata never precedes required namespace durability; successful case retains directory and file entries |
| Controller `SIGKILL` at ordering points | deterministic barriers | restart state always follows durable predecessor, never fabricated success |
| output connection stalls | proxy stops reads | buffers stay bounded; credit reaches zero; workload backpressure |

Crash-order tests place barriers immediately before/after assignment commit, dispatch queueing,
log barrier, terminal commit, and resource release. They verify “durable transition precedes
irreversible side effect” and that memory accounting is never falsely described as cross-medium
ACID.

Log namespace crash tests additionally place barriers after `mkdir`/`O_CREAT`, after file
`fdatasync`, after each required directory `fsync`, and before terminal metadata commit. Unit and
component failpoints verify call ordering on every platform; the supported Linux integration
harness verifies crash persistence on a filesystem that documents directory `fsync` support.

## Sanitizers and diagnostics

- **ASan + UBSan:** all unit/component tests and practical runtime/cluster tests.
- **TSan:** queue/channel/state tests and selected Controller/Worker integration tests; suppress
  only documented third-party/runtime findings, never project races.
- **LeakSanitizer:** where compatible with fork-based tests.
- Compiler warnings are enabled and treated as errors for project code in CI.
- Optional `strace` diagnostics validate process/fd sequences; they are not the sole assertion.
- Repeated stress runs cover child exit, connection churn, log backpressure, and shutdown.

Forking from a multithreaded test harness is handled carefully: production Worker is single-threaded,
and child-after-fork code uses only async-signal-safe operations until exec.

## Guarantee-to-test traceability

| Claimed guarantee | Required tests |
|---|---|
| durable submit before success | StateStore transaction + crash-order cluster test |
| assignment persistence before dispatch | gated fake transport unit test + process crash barriers |
| no terminal before complete durable tail and namespace | LogWriter file/directory barrier unit, directory-sync fault/crash-order test, and delayed-barrier cluster test |
| TASK_FINISHED never overtakes log tail | output-scheduler short-write unit tests + stalled-socket cluster test |
| Task/Attempt separation and history | transition tests + manual retry cluster test |
| Worker/session/connection separation | replacement/reconnect tests |
| no intentional log loss, bounded memory | credit unit tests + saturation integration test |
| credited chunks have pre-reserved admission | capacity conservation/property tests + invariant-failure injection |
| no zombies/fd leaks | runtime matrix + `/proc`/sanitizer repeated checks |
| leader result survives residual-group cleanup | same-pgid descendant runtime tests |
| child path is prebuilt and async-signal-safe | PATH/env/argv preparation tests + child-path syscall review/test hook |
| partial TCP correctness | split/multi/partial/EAGAIN protocol tests |
| deterministic scheduling | scheduler unit vectors + multi-Worker cluster test |
| conservative restart/loss | Worker kill, session replacement, Controller restart tests |
| no automatic retry/exactly-once claim | assert one Attempt after loss until explicit retry |

## Reproducibility

CTest labels separate `unit`, `component`, `runtime`, `cluster`, `fault`, and `sanitizer` suites.
Every failing integration test prints process logs, Controller DB dump summary, ports, random seed,
state history, queue watermarks, and remaining pids/fds. Randomized tests record their seed. Tests
use fixed protocol/config defaults unless the case declares an override.

## Pass/fail gate

The implementation is acceptable only when:

- all required tests pass repeatedly on the supported Linux/GCC baseline;
- no unexplained sanitizer report, project data race, fd leak, or direct-child zombie remains;
- fault tests observe the exact documented state and persistence ordering;
- memory remains within configured bounded buffers during sustained log/network pressure;
- every frozen message golden test and state transition table agrees with the documents;
- no skipped core test is counted as a pass; environment-dependent sanitizer exclusions are
  explicit and separately reviewed.
