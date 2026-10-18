# Milestone 3 Authorization / Codex Development Prompt

## Linux C++ Distributed Task Scheduler

**Authorized milestone:** Milestone 3 — Frozen framing, protocol schemas, and TCP transport
**Authorized branch:** `milestone-3-protocol-net`
**Repository:** `nhzj121/task-scheduler`
**Reference baseline:** `main` at `2600605d2a576eb056b627af65de02efb79e82f4` (verified GitHub remote at authorization preparation)

---

## 0. Authorization and frozen architecture

Architecture Gate: **PASS**. Milestone 0: closed. Milestone 1: PASS. Milestone 2: PASS and merged. Milestone 4: PASS and merged into `main`.

The verified remote `main` commit `2600605d2a576eb056b627af65de02efb79e82f4` is a merge commit with parents:

- `3223d2468aa7262e35e9c8cfeed65ecd50719944` — previous M2-integrated main;
- `5b0e664f908fc509f842a939111f1afb0eb1cc6b` — complete M4 branch including its final formatting-only commit.

**Milestone 3 is the only newly authorized production milestone for this session.** M3 may use accepted M1, M2, and M4 code as its baseline, but must not extend M4 or begin M5/M6/M7/M8/M9/M10. Its own production modules are `protocol` and `net` only, plus narrowly necessary generic lower-layer correctness fixes.

`docs/spec/Codex_Full_Implementation_Prompt_v1.md` remains authoritative for general implementation rules. Its historic “M1 only” authorization is superseded by this explicit M3 authorization. Do not rewrite that old generic prompt merely to update the milestone number.

The frozen architecture is the source of truth. No convenience-driven changes to protocol IDs, wire format, JSON schemas, Task/Attempt/Worker state machines, session semantics, ownership, durability order, log-credit semantics, or failure behavior are permitted. A genuine ambiguity affecting externally observable behavior is a **STOP-and-report** condition, not permission to invent a rule.

This document authorizes implementation, testing, normal M3 branch commits, and pushes to that branch. It **does not** authorize merging into `main` or opening the next milestone.

---

## 1. Repository, environment, and Git preflight

### 1.1 Paths

Windows source and Git working tree:

```text
E:\Deck_all_need\projects\work\task_scheduler
```

WSL view of that **same** tree:

```text
/mnt/e/Deck_all_need/projects/work/task_scheduler
```

Formal Linux build tree on WSL ext4:

```text
/home/nhzj121/build/task_scheduler
```

Use Windows Codex Desktop for editing if desired and Windows Git for version control. Formal CMake, GCC, and CTest runs **must** be performed in WSL Ubuntu 22.04. Do not clone, copy, or maintain a parallel source tree in WSL; do not use an NTFS `build/` tree for formal acceptance.

### 1.2 Mandatory preflight — before any implementation

Inspect and report:

```bash
git status --short
git branch --show-current
git remote -v
git fetch origin
git rev-parse main
git rev-parse origin/main
git log -1 --format='%h %s' main
git branch -vv
```

**Confirm that both `main` and `origin/main` include the complete accepted M4 merge.** The initially verified remote baseline is `2600605d2a576eb056b627af65de02efb79e82f4`; report any change since then. A harmless newer remote commit is not an automatic authorization to adopt changed frozen semantics: inspect what changed. If the accepted baseline cannot be reconciled safely or `main` diverges from `origin/main`, STOP and report.

Do not commit M3 work on `main`, reset or force-push `main`, or rewrite existing history. If local `main` is clean and merely behind remote, `git pull --ff-only` is allowed after inspection.

### 1.3 Working-tree exception for this authorization document

Usually the working tree must be clean before branch creation. If the **only** local uncommitted or untracked file is:

```text
docs/spec/Milestone_3_Authorization_Codex_Development_Prompt.md
```

that is an explicitly authorized exception: the user may have placed this document in the repository before creating the M3 branch. Carry it onto `milestone-3-protocol-net` and commit it there, not on `main`.

Any other uncommitted edits (including local formatting edits) are a stop condition: **do not stash, reset, discard, overwrite, or absorb them without explicit user approval**. Ignored personal notes such as `.local-notes/` must remain untouched.

### 1.4 Required branch

Create from the accepted `main`:

```bash
git switch main
git switch -c milestone-3-protocol-net
```

If the branch already exists, inspect its ancestry, tracked changes, current HEAD, and remote tracking before reuse. Do not silently recreate, reset, or overwrite it.

Before editing production files, confirm:

```bash
git branch --show-current
# must print milestone-3-protocol-net
```

Publishing this branch and ordinary scoped commits are authorized:

```bash
git push -u origin milestone-3-protocol-net
```

**Never merge this branch into `main` in this session.** Do not create/merge a PR unless separately authorized.

---

## 2. Mandatory repository reading

Before designing M3 APIs, read these exact current-repository documents, not remembered or generated summaries:

1. `AGENTS.md`
2. `docs/ARCHITECTURE_GATE.md`
3. `docs/spec/Task_Scheduler_Final_Scope_v1.md`
4. `docs/spec/Codex_Full_Implementation_Prompt_v1.md`
5. `docs/PROJECT_SCOPE.md`
6. `docs/ARCHITECTURE.md`
7. `docs/PROTOCOL.md`
8. `docs/OWNERSHIP.md`
9. `docs/STATE_MACHINE.md`
10. `docs/FAILURE_MODEL.md`
11. `docs/TEST_STRATEGY.md`
12. `docs/IMPLEMENTATION_PLAN.md`
13. `docs/DECISIONS.md`
14. Relevant remaining `docs/spec/*` contract material.

Inspect accepted M1/M2/M4 source and the current build/test conventions, especially:

```text
include/ts/common/
include/ts/domain/
include/ts/io/
include/ts/runtime/
src/common/
src/domain/
src/io/
src/runtime/
tests/
CMakeLists.txt
```

Pay particular attention to:

- `IMPLEMENTATION_PLAN.md` — **Milestone 3**;
- `PROTOCOL.md` — all 26 IDs, header, every payload schema, request IDs, size/buffering limits, malformed handling, and per-Attempt finish barrier;
- `OWNERSHIP.md` — `ConnectionManager` owns `TcpConnection`, close order, and partially sent frame ownership;
- `ARCHITECTURE.md` — dependency graph, LT EventLoop contract, output scheduling and log-ordinal barrier;
- `TEST_STRATEGY.md` — protocol/net test matrix, mutation cases, and acceptance gate;
- `FAILURE_MODEL.md` — protocol fault classification; do **not** implement Controller/Worker business consequences in M3.

If any applicable frozen documents disagree on semantics, **STOP**, identify the specific sections, and request approval before continuing.

---

## 3. M3 scope, layers, and dependency direction

The frozen M3 definition is:

> **Future modules:** `protocol`, `net`, minimal loopback test helpers.
> **Work:** exact 20-byte codec, all v1 typed JSON messages and validation, incremental frame parser, bounded input/output queues, non-blocking listener/connect/accept/read/write, ConnectionManager.
> **Acceptance:** golden header/message vectors; every split point; multiple frames/read; partial send/EAGAIN; max-size/malformed/role-invalid rejection; callback lifetime and peer-close tests.

Permitted production areas (names may vary while preserving module ownership):

```text
include/ts/protocol/
src/protocol/
include/ts/net/
src/net/
CMakeLists.txt
```

Permitted tests and test-only utilities:

```text
tests/unit/
tests/component/
tests/helpers/                 # only small TCP loopback/golden-vector helpers
```

Preserve frozen dependency direction:

```text
protocol -> common, domain
net      -> common, io
```

`protocol` must **not** depend on `net`. `net` must **not** depend on `protocol` or `domain`, nor on `runtime`, `scheduler`, `storage`, `controller`, `worker`, `client`, or any application. The protocol module must perform no socket I/O; the net module must treat application frames as owned opaque bytes plus generic scheduling metadata.

Use an external adapter/test harness to compose generic net bytes with protocol codecs. Do not solve the layering problem by adding a reversed dependency, importing Task/Attempt semantics into `TcpConnection`, or building a premature Worker/Controller runtime.

### Forbidden M3 implementation

Do **not** implement:

- `apps/controller`, `apps/worker`, `apps/taskctl`, or production CLI;
- Worker registration **business state**, heartbeat scheduling/offline detection, reconnect orchestration, or session generation ownership;
- task submission execution, scheduling, cancellation orchestration, TaskManager, WorkerRegistry, ResourceLedger;
- process launch integration, fork/exec adaptation, RunningTask/ExecutionManager/ProcessRuntime extensions;
- SQLite, persistence thread, recovery, or durable effects;
- LogWriter, filesystem task logs, log durability barrier, or M8 credit/reservation coordinator;
- any workload execution or storage side effect triggered by decoded protocol messages;
- new message IDs, fields, success mappings, state transitions, or new protocol version;
- Boost.Asio, gRPC, TLS/auth, general thread pools, cgroups, HA, or other v1 non-goals.

Protocol types and validation of future messages **are authorized**. Their business execution is **not**.

---

## 4. Work breakdown and implementation checkpoints

Use four coherent M3 subphases. A subphase is not a new milestone and does not independently authorize later work.

| Checkpoint | Work | Evidence required |
|---|---|---|
| **M3-A** | 20-byte header codec; incremental bounded frame parser | exact golden bytes; every split boundary; payload length/error handling |
| **M3-B** | all 26 typed JSON message schemas, strict validation, pure protocol helper logic | every frozen JSON example encode/decode; invalid types/fields/roles/IDs; log payload checks |
| **M3-C** | nonblocking Linux TCP listener/connect/accept/read/write; `TcpConnection`, `ConnectionManager`, bounded buffers | real loopback, partial send/receive, EAGAIN, peer-close, fd/generation safety |
| **M3-D** | generic control/log output selection and per-Attempt ordering dependency; component/stress tests | short writes, no interleaving, FINISH barrier, control bypass, bounded resources |

Make small reviewable commits when a coherent subphase passes its tests. Do not fabricate placeholders or skip required protocol types because the business layer is not implemented yet.

---

## 5. Frozen wire format (M3-A)

Every wire frame is:

```text
Offset  Bytes  Field
0       4      magic           (uint32)
4       2      version         (uint16)
6       2      message_type    (uint16)
8       8      request_id      (uint64)
16      4      payload_length  (uint32)
20      N      UTF-8 JSON payload
```

Frozen constants:

```text
Header length:       20 bytes
Magic:               0x54534348  (ASCII TSCH)
Version:             1
Maximum JSON bytes:  4,194,304  (4 MiB)
Integer byte order:  network/big-endian
```

Encode/decode every integer explicitly; include tested 64-bit host/network byte-order conversion. **Never** serialize a C++ `struct` using its in-memory layout, alignment, or endianness. Maintain golden literal wire-byte test vectors independent of the same production encoder/decoder.

Zero payload length is reserved and invalid for the currently defined v1 messages. Reject payload lengths above 4 MiB from the validated header **before** allocating payload capacity. Do not seek a new magic marker to resynchronize a corrupt TCP stream.

### Incremental parser invariants

- Handle 1 byte at a time, any split of header/payload, multiple frames in one read, complete frame followed by partial frame.
- Never parse JSON until the declared complete payload is buffered.
- Reject invalid magic/version/type, wrong role/direction, reserved IDs, invalid request ID, and invalid lengths at the appropriate validated boundary.
- Represent parser status explicitly: incomplete, complete frame(s), or fatal protocol error; no ambiguous partial success.
- Enforce a configured finite input high-water mark large enough for one maximum frame without allowing unbounded accumulation of subsequent frames.
- A malformed header must not cause an allocation proportional to an attacker-supplied length.
- On invalid framing, close without byte-stream resynchronization.

No TCP socket operations belong in the `protocol` parser.

---

## 6. Frozen message identity table (M3-B)

Implement typed payload encoding/decoding and validation for **all 26** frozen message types:

| ID | Name |
|---|---|
| `0x0001` | `ERROR_RESPONSE` |
| `0x0101` | `CLIENT.SUBMIT_TASK` |
| `0x0102` | `CLIENT.SUBMIT_TASK_RESPONSE` |
| `0x0103` | `CLIENT.GET_TASK` |
| `0x0104` | `CLIENT.GET_TASK_RESPONSE` |
| `0x0105` | `CLIENT.LIST_TASKS` |
| `0x0106` | `CLIENT.LIST_TASKS_RESPONSE` |
| `0x0107` | `CLIENT.GET_LOGS` |
| `0x0108` | `CLIENT.GET_LOGS_RESPONSE` |
| `0x0109` | `CLIENT.CANCEL_TASK` |
| `0x010A` | `CLIENT.CANCEL_TASK_RESPONSE` |
| `0x010B` | `CLIENT.RETRY_TASK` |
| `0x010C` | `CLIENT.RETRY_TASK_RESPONSE` |
| `0x010D` | `CLIENT.LIST_WORKERS` |
| `0x010E` | `CLIENT.LIST_WORKERS_RESPONSE` |
| `0x0201` | `WORKER.REGISTER_WORKER` |
| `0x0202` | `WORKER.REGISTER_ACK` |
| `0x0203` | `WORKER.HEARTBEAT` |
| `0x0204` | `WORKER.RUN_TASK` |
| `0x0205` | `WORKER.RUN_TASK_ACK` |
| `0x0206` | `WORKER.TASK_STARTED` |
| `0x0207` | `WORKER.LOG_CHUNK` |
| `0x0208` | `WORKER.LOG_WINDOW_UPDATE` |
| `0x0209` | `WORKER.TASK_FINISHED` |
| `0x020A` | `WORKER.CANCEL_TASK` |
| `0x020B` | `WORKER.CANCEL_TASK_ACK` |

IDs not listed are reserved. Never renumber, alias, or infer new IDs from names.

### Exact JSON schema rules

Every message payload must be exactly **one UTF-8 JSON object**. Implement each message's required/optional/nullable fields and nested objects directly from `docs/PROTOCOL.md`, including the documented defaults and constraints. Do not use one generic unvalidated `nlohmann::json` object as a substitute for typed messages.

Strict validation must include:

- missing/extra/duplicate JSON object keys (including nested objects);
- wrong scalar types, nullability errors, negative values, zero where prohibited, overflow/truncation, integer/float confusion;
- UTF-8 validation and malformed JSON rejection;
- exact strong `task_id`, `attempt_id`, `session_id`, `worker_id`, and `attempt_no` semantics from the existing M1 domain types;
- timestamp format, documented state strings, resource bounds, timeout/duration constraints as applicable;
- array element types, pagination parameters, log offsets, nullable cursor/snapshot fields, and nesting;
- no conversion of a failing field into a default value unless the field is explicitly optional and absent;
- errors with sufficient context for a caller to apply the frozen client/Worker failure policy.

Be careful: a generic JSON decoder may silently coalesce duplicate keys. The **duplicate-key rejection requirement must be proven with deliberate duplicate-key tests**, not assumed from `nlohmann::json`'s normal object representation. If existing JSON tooling is inadequate, implement the narrowest correct validation mechanism consistent with accepted dependencies; do not add a new JSON dependency without approval.

### Request IDs and direction

- A **request** carries a nonzero request ID, unique among outstanding requests on that connection.
- A direct **response** copies the request ID from the corresponding request.
- `WORKER.HEARTBEAT`, `WORKER.TASK_STARTED`, `WORKER.LOG_CHUNK`, `WORKER.LOG_WINDOW_UPDATE`, `WORKER.TASK_FINISHED` are events with request ID **zero**.
- `ERROR_RESPONSE` preserves the parsed offending client request ID when appropriate.
- IDs are connection-scoped correlation values, **not** idempotency keys.
- Validate direction/role explicitly: client-to-Controller, Controller-to-client, Worker-to-Controller, Controller-to-Worker.
- Cross-message outstanding-ID tracking may be exercised in test-only peer state; do not create a Controller/Worker session registry in M3.

### Frozen application semantics must be represented, not executed

For example, `CLIENT.SUBMIT_TASK` may be validated and encoded; M3 must not create a Task. `WORKER.RUN_TASK` may be validated and decoded; M3 must not fork a workload. `CLIENT.GET_LOGS` may be validated; M3 must not read log files. `WORKER.REGISTER_ACK` can be decoded; M3 must not decide whether a Worker is healthy.

---

## 7. Logs and protocol-level consistency checks

Implement codec/validator behavior for:

- `WORKER.LOG_CHUNK` base64 encoding/decoding of arbitrary bytes;
- exact `raw_size` match with decoded byte count;
- raw chunk size **1..65,536 bytes**, no empty EOF log chunk;
- stream identifiers `stdout` and `stderr`;
- `WORKER.TASK_FINISHED` terminal counters and sequence fields;
- `WORKER.LOG_WINDOW_UPDATE` fields and numeric constraints;
- `GET_LOGS` response base64 and snapshot/pagination field schemas.

Provide **pure protocol helpers or test-scoped tracking state** for sequence-gap/duplicate, final-count mismatch, and invalid/insufficient-credit cases where `TEST_STRATEGY.md` requires those checks. This must not introduce a production Controller/Worker session ownership model or the two-dimensional M8 grant/reservation coordinator.

Treat the difference explicitly:

- **M3:** validation of a supplied protocol message and, where applicable, lightweight consistency checking against explicit caller-provided context; generic send-order dependency primitives.
- **M7/M8/M9:** ownership of real Worker sessions and Attempts, credit grant/admission policy, log persistence, LOST transition, durable finalization.

Do not claim to test full M8 credit accounting or Controller LOST semantics with M3 mocks. If a required check cannot be expressed without violating the frozen module boundary, stop and report the precise contract ambiguity rather than implementing a new business subsystem.

---

## 8. Bounded input/output memory

M3 must use finite, configured limits for **both** receive and send paths.

Requirements:

- Per-connection input bound supports a full maximum-size frame; never grows unbounded under many queued frames.
- Control and log output queues are separately bounded by explicit configured hard limits.
- Bound owned encoded frame bytes and any queued-frame metadata; count unsent/partially sent bytes against the limit.
- Reject/backpressure/close according to documented transport/session policy rather than silently dropping bytes or allocating beyond the bound.
- A normal partial `send()` or `EAGAIN` must preserve unsent bytes exactly once.
- A peer that stops reading must not cause unbounded process memory growth.
- Buffers must stay owned until fully sent or the connection is explicitly closed and the unsent data is diagnosed/discarded under the appropriate policy.
- Do not interpret a configured smaller high-water mark as permission to silently reduce the frozen 4 MiB protocol maximum.

Use an explicit policy for when a complete frame cannot fit current queue capacity. No unbounded fallback queue.

---

## 9. Nonblocking TCP transport (M3-C)

Implement generic Linux TCP substrate for:

```text
socket
bind
listen
accept4 / accept
nonblocking connect completion
recv
send
close
```

Use M2 `UniqueFd` and `EventLoop`, Level Triggered epoll, CLOEXEC and nonblocking creation/flags where available.

### Listener / connector

- Nonblocking listener; accept available connections until `EAGAIN`.
- Connection attempts may complete immediately or with `EINPROGRESS`; verify completion using readiness and `SO_ERROR`.
- Handle `EINTR`, `EAGAIN`, and fatal errors as appropriate for each syscall.
- Do not block the EventLoop thread in DNS resolution or connect, or add synchronous read/write waits.
- Use loopback/ephemeral-port component tests; avoid reliance on external public services.

### Receive path

- On readable LT readiness, receive repeatedly until `EAGAIN`, orderly EOF, or fatal error, subject to already-frozen finite memory/readiness constraints.
- Feed bytes into a bounded generic transport input path and protocol parser **through an external composition boundary**; `net` itself must not parse Task/Worker JSON.
- Handle zero-byte `recv` as orderly peer close, including closure halfway through a frame.
- Reject oversized frames from protocol layer before allocation; do not resynchronize a malformed stream.

### Send path

- Own encoded buffers and current byte offset.
- On writable readiness, advance partial sends and handle `EINTR`/`EAGAIN`; never assume one `send()` transmits a complete frame.
- Prevent SIGPIPE from terminating the process when the peer closes during send (use the appropriate Linux socket send semantics).
- Enable `EPOLLOUT` only when pending eligible bytes require it; remove it promptly after drain.
- Never spin forever on an always-writable socket or run an unbounded loop of application callbacks.
- Respect partial-frame non-interleaving: once sending a frame begins, its remaining bytes precede every other frame's first byte.

Use explicit close causes and peer-close notifications as generic values; **M7** decides whether that means Worker-session invalidation or a client transport-only failure.

---

## 10. `TcpConnection` / `ConnectionManager` ownership

Use the frozen ownership topology:

```text
ConnectionManager
    owns unordered_map<ConnectionId, unique_ptr<TcpConnection>>
        TcpConnection owns:
            socket UniqueFd
            bounded InputBuffer
            bounded control OutputQueue
            bounded log OutputQueue
            current partial-frame state
```

Requirements:

- Use stable ConnectionId and generation-safe M2 registration tokens; do not use the integer fd alone as an object identity.
- Callback dispatch borrows objects for one callback; do not capture owning/dangling raw `TcpConnection*` in epoll data.
- Callback self-close, close of another connection within the current epoll batch, and fd-number reuse must be safe.
- Normal cleanup order: mark closing/stop new frames -> inform generic owner/caller by value -> unregister -> diagnose/discard unsent buffers -> close socket `UniqueFd` -> deferred owner destruction.
- Do not delete an object on its own callback stack.
- Closing a client transport must not mutate any Task; closing a Worker transport must not perform Controller LOST transaction inside `net`.
- Later M7 integration can attach stable Worker/session generations outside this module.

Avoid broad shared ownership as a replacement for lifecycle reasoning.

---

## 11. Control/log send ordering and the frozen FINISH barrier (M3-D)

This is a mandatory **M3 send-scheduler correctness target**, even though durable logging is M8.

The frozen wire contract requires:

1. Two separate bounded logical queues: control and log.
2. The currently partially sent frame always completes before any new frame begins.
3. At a **new frame boundary**, heartbeat/ACK/unrelated control may be chosen ahead of queued logs.
4. Each generated log chunk may be associated with an opaque, monotonic **per-Attempt emission ordinal across both stdout and stderr**.
5. A `TASK_FINISHED` frame depends on the **last previously generated same-Attempt log ordinal** and must not begin its first byte before all earlier same-Attempt log frames are completely sent.
6. Other Attempts' logs must not impose an unnecessary global barrier.
7. While one FINISH is ineligible, unrelated control frames (heartbeat/ACK) remain eligible.

Keep the net output scheduler **generic**: model opaque group/dependency keys, ordinals, and eligibility metadata supplied by a higher layer. Do not make `net` import `AttemptId`, understand `TASK_FINISHED`, decode JSON, or own Worker state.

Implement focused output-scheduler tests proving:

- one-byte and short writes;
- repeated `EAGAIN` and continuation;
- partial current frame never interleaved;
- control prioritization only at frame boundaries;
- per-group FINISH dependence despite multi-stream logs;
- unrelated groups do not block FINISH;
- eligible control can bypass an ineligible FINISH;
- bounded bytes and queued slots under backpressure.

The production M3 abstraction must be sufficient for later Worker integration without violating the frozen protocol. Do **not** pre-build the M8 credit/reservation system or LogWriter.

---

## 12. Invalid input and error policy

Implement the frozen classification, but keep the side effects in later application owners.

- Invalid magic, unsupported version, or payload length above 4 MiB: reject/close immediately without payload allocation.
- Unknown type or wrong connection role: protocol error. A **client** may receive bounded `ERROR_RESPONSE/UNSUPPORTED` if safe before closing; a **Worker** connection closes (M7 handles session invalidation).
- Syntactically malformed UTF-8 or JSON: close.
- Validly framed client semantic/schema errors: provide enough typed error information for a bounded `ERROR_RESPONSE/INVALID_ARGUMENT`.
- Worker schema/protocol violations: close; Controller-side LOST persistence belongs to M7/M9.
- Log sequence, base64, byte count, or credit violations are typed protocol failures, not silently sanitized.
- Connection EOF/reset/fatal send/recv errors: notify generic owner/caller by value and close safely.

Do not accidentally expose unbounded diagnostic error messages or echo untrusted entire payloads. Do not make a bad client frame crash the process or disrupt an unrelated connection.

---

## 13. M3 unit test matrix

All accepted M1, M2, and M4 regression tests must remain passing. New M3 tests must cover:

### 13.1 Header and framing

- exact **literal** 20-byte golden header bytes, including 32/16/64-bit big-endian fields;
- 64-bit host/network-order round-trip including nontrivial bit patterns;
- one-byte-at-a-time header and payload;
- every possible cut position in at least one full frame;
- two or more complete frames in one chunk;
- full frame plus partial next;
- zero-length payload reserved behavior;
- exactly maximum payload and one byte over; no allocation based on unvalidated length;
- magic/version/type/request-ID validity and overflow/bounds;
- malformed header never scans payload for a new magic;
- input buffer remains within configured limit.

### 13.2 JSON message schemas — **every frozen ID**

- golden fixture for each of the 26 message types;
- decoding and encoding that independently match the documented JSON semantics;
- required/optional/nullable field coverage and documented defaults;
- missing, unknown, duplicate top-level and nested fields;
- wrong scalar/array/object types, null where forbidden, negative numbers, unsigned overflow, fractional instead of integer values;
- invalid UTF-8/malformed JSON/trailing non-whitespace documents;
- valid/invalid UUIDs, WorkerId and AttemptNo, states and timestamps;
- role/direction invalid cases and nonzero/zero request-ID rules;
- all frozen enum/string tokens without synonyms or implicit coercion.

### 13.3 Logs and pure consistency helpers

- raw binary data including NUL and invalid UTF-8 bytes round-trips through base64;
- invalid base64 alphabets/padding, decoded size mismatch and 64-KiB chunk bound;
- independent stdout/stderr sequence and duplicate/gap detection;
- declared terminal sequence/byte counts versus accepted chunks;
- invalid credit numeric fields and insufficient caller-provided credit test cases;
- no claim that these tests are M8's durable queue-credit accounting.

---

## 14. M3 real-socket / component test matrix

Use actual Linux loopback sockets and M2 EventLoop for tests. No production Controller/Worker runtime.

Minimum coverage:

- nonblocking listener and accepted fds; CLOEXEC, no unexpected blocking;
- nonblocking connector including `EINPROGRESS`/`SO_ERROR` behavior when observable;
- repeated accept until `EAGAIN`;
- multiple simultaneous connections;
- partial receive across every frame boundary;
- multiple frames in one read;
- partial writes and `EAGAIN` under controlled socket backpressure;
- write-interest enable/disable and no writable-interest busy loop when queue empty;
- peer graceful FIN during header, during payload, and after complete frame;
- peer reset/broken pipe during send, including SIGPIPE suppression;
- malformed client versus Worker-role classification via test-only caller adapter;
- callback self-removal, cross-removal, same-batch stale event, fd reuse, deferred close;
- connection manager retains ownership until safe destruction;
- bounded input and both outbound queues under a peer that stops reading;
- control/log priority and all per-Attempt finish-barrier cases from section 11;
- repeated open/send/close churn and `/proc/self/fd` leak checks;
- mutation/fuzz-style parser tests with fixed seeds and timeout/resource caps.

Use explicit synchronization and bounded deadlines. Avoid arbitrary long sleeps as correctness evidence. Repeat key parser, lifecycle, backpressure, and scheduling tests multiple times.

A deterministic send/recv test seam is allowed for injecting short I/O, `EINTR`, and `EAGAIN`; **also** run real loopback integration tests. Test hooks must not change production semantics.

---

## 15. Acceptance and scope requirements

Do not declare M3 PASS merely because two peers using the same buggy implementation can talk to each other. Required evidence includes independent wire golden bytes and the documented JSON examples.

Before PASS verify:

- no frozen wire ID/field/schema was altered;
- the parser is incremental, bounded, and rejects invalid framing early;
- no unvalidated duplicate JSON key can be silently accepted;
- `protocol` remains socket-free and `net` remains unaware of Task/Attempt/Worker semantics;
- network buffers/queues are finite, with exact ownership for partial sends;
- LT/EventLoop generation and deferred destruction are preserved;
- the FINISH barrier is correctly implemented without a global log-queue block;
- no M4 process runtime or other accepted M1/M2/M4 behavior changed;
- no M5+ business/persistence/logging subsystem was added;
- no mandatory tests were skipped, disabled, weakened, or replaced by placeholder assertions.

If a required M3 feature cannot be implemented without changing a frozen contract, STOP. Do not mark acceptance PASS with an unresolved blocker.

---

## 16. Formal WSL Linux build, tests, and diagnostics

The official environment is WSL Ubuntu 22.04 with the existing GCC/CMake `-Werror` baseline. Configure again after CMake target changes.

From a Windows shell/Codex Desktop, run the equivalent of:

```powershell
wsl.exe -d Ubuntu22.04.3 -- bash -lc 'set -euo pipefail; cmake -S /mnt/e/Deck_all_need/projects/work/task_scheduler -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug'
wsl.exe -d Ubuntu22.04.3 -- bash -lc 'set -euo pipefail; cmake --build /home/nhzj121/build/task_scheduler --target clean; test -f /home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md'
wsl.exe -d Ubuntu22.04.3 -- bash -lc 'set -euo pipefail; cmake --build /home/nhzj121/build/task_scheduler -j'
wsl.exe -d Ubuntu22.04.3 -- bash -lc 'set -euo pipefail; ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure'
```

Use `ctest -R` or a dedicated M3 CTest label to repeat M3 tests and report **test count × iterations**. Ensure shell pipelines do not conceal CTest failure: use `set -o pipefail` or avoid piping through `tail` unless the actual CTest exit code is preserved.

Do **not** delete:

```text
/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
```

Do not `rm -rf` the build tree. Use CMake clean targets for a warning-as-error rebuild. Do not treat Windows/MinGW builds as Linux acceptance evidence.

If sanitizer infrastructure is already available, run relevant ASan/UBSan and report actual results. If it is not available, do not convert M3 into M10 tooling work and do not claim sanitizer success. Repeat socket churn and check for fd/memory growth under bounded traffic.

---

## 17. Suggested implementation sequence and reviewable commits

```text
1. Git status/branch/origin/main baseline preflight
2. Read all M3 frozen contracts and accepted interfaces
3. Implement 20-byte codec + independent golden vectors
4. Implement bounded incremental parser + split/boundary tests
5. Implement 26 typed JSON schemas and strict validators
6. Implement pure log payload/sequence/final-count test helpers
7. Implement nonblocking listener and connector
8. Implement bounded TcpConnection receive and send buffers
9. Implement ConnectionManager ownership/deferred teardown
10. Implement generic control/log output scheduling + FINISH dependency metadata
11. Run real loopback tests, fault injection, memory/fd stress
12. Run complete M1/M2/M4 regression suite with all M3 tests
13. Audit wire schemas, module dependencies, and callback lifetime
14. Inspect git diff and create/push M3-scoped commits
15. Produce Milestone 3 report
16. STOP
```

Suggested commit boundaries (not mandatory):

```text
protocol: implement frozen wire header and streaming parser
protocol: add all v1 typed JSON schemas and validators
net: add nonblocking TCP transport and bounded ConnectionManager
net: implement ordered control/log frame scheduling
net: add loopback, backpressure and lifecycle component tests
```

No need to obtain separate user approval between M3-A/B/C/D; all four are within this single authorization. No permission is granted to begin M5 merely because M3 passes.

---

## 18. Mandatory Git completion checks

Before committing, inspect:

```bash
git status --short
git diff --check
git diff --stat main...HEAD
git log --oneline main..HEAD
```

`git diff` for uncommitted changes must also be checked before the final commit. Verify changes are limited to M3 protocol/net, its tests/CMake, and this authorization document, except any explicitly justified genuine lower-layer correctness fix.

Commit and push to:

```text
milestone-3-protocol-net
```

Verify remote M3 branch HEAD equals the local HEAD and the working tree is clean (except any user-owned unrelated files that you were instructed not to touch).

**Do not merge M3 into `main`.** Do not force-push, rewrite baseline history, delete branches, or start M5+.

---

## 19. Required final Milestone 3 report

Use the following report sections; give exact file paths, commands, counts, and evidence rather than unverified PASS claims.

### A. Git / baseline

- current branch;
- accepted `main` baseline full SHA;
- M3 HEAD full SHA;
- ahead/behind, remote sync, working-tree status;
- confirmation that `main` was not modified or merged.

### B. Implementation summary

- wire codec + incremental parser;
- typed JSON schemas for all 26 messages;
- role/request-ID validation;
- log/base64/sequence consistency helpers;
- TCP listener/connect/accept;
- `TcpConnection` and `ConnectionManager` ownership;
- bounded input/control/log queues;
- send priority / partial-frame ownership / FINISH barrier;
- what M3 intentionally does **not** implement.

### C. Files changed

Added, modified, removed; module dependencies and CMake targets.

### D. Protocol conformance review

Explain exact header layout, byte order, payload limits, schema completeness, duplicate-key handling, request/response correlation and malformed-input classification. Confirm **26/26 frozen message schemas** implemented and tested, or mark the gate NOT PASSED.

### E. Network ownership/lifetime review

Who owns each socket, buffer, queued frame, registration token, callback identity; unregister-before-close/deferred destruction; partial send and peer-close behavior.

### F. Acceptance matrix (PASS/FAIL with tests)

```text
20-byte golden header
64-bit network byte order
incremental split/merge parser
4 MiB maximum / oversize rejection
bounded input allocation
all 26 typed JSON messages
unknown/missing/duplicate/nullable field rules
UTF-8/JSON/numeric validation
role and request-ID validation
base64/raw_size/sequence/final-count validation
nonblocking listener/connector/accept
partial recv/send and EINTR/EAGAIN
EPOLLOUT interest toggling
bounded control/log queues
partial-frame non-interleaving
control priority at frame boundary
same-Attempt FINISH send barrier
unrelated Attempt isolation
peer close/reset/SIGPIPE safety
ConnectionManager fd reuse/callback lifetime
loopback and repeated churn/no fd leak
M1 regression
M2 regression
M4 regression
module dependency compliance
M3-only scope compliance
```

### G. Build and test evidence

- exact WSL configure/build/CTest commands;
- complete CTest count passed/failed;
- M3 focused repeat count and iterations;
- loopback/fault/fuzz test results;
- memory/fd growth diagnostics;
- sanitizer results **only if actually run**;
- confirmation `BUILD_COMMANDS.md` was preserved.

### H. Deviations / limitations

- explicit architecture/semantic deviation, or state **none**;
- any incomplete M3 requirement must block PASS;
- genuine remaining M7/M8 integration dependencies, but no premature implementation.

### I. Git review summary

```text
git diff --stat main...HEAD
git log --oneline main..HEAD
```

### J. Gate and STOP

If and only if all M3 acceptance requirements and regression tests pass, end with:

```text
Milestone 3: PASS — awaiting explicit review; Milestone 5 and later milestones have not started.
```

Otherwise end with:

```text
Milestone 3: NOT PASSED — blockers remain.
```

Then **STOP**. Do not merge `main`, start Milestone 5, or add new non-M3 functionality.

---

## 20. Final authorization

Build only the frozen M3 communication foundation:

```text
20-byte frame header
      +
incremental bounded parser
      +
26 strict typed message schemas
      +
nonblocking Linux TCP transport
      +
generation-safe ConnectionManager
      +
bounded prioritized output queues
      +
per-Attempt FINISH wire-order dependency
      +
complete tests
```

Do not build the Controller/Worker systems that will consume it later.

Preserve M1, M2, and M4. Work **only** on `milestone-3-protocol-net`. Do **not** modify or merge `main`. Do **not** begin M5+.

**Milestone 3 is explicitly authorized.**
