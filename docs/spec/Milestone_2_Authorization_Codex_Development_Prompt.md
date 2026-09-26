# Milestone 2 Authorization / Codex Development Prompt
## Linux C++ Distributed Task Scheduler
## Authorized scope: Milestone 2 — Linux fd ownership and EventLoop

---

# 0. Authorization status

The Architecture / Specification Gate has already **PASSED**.

Milestone 0 is closed.

Milestone 1 has already **PASSED implementation review**, including the small post-review cleanup.
The current stable `main` branch is the accepted baseline containing:

- frozen architecture/specification documents;
- CMake/CTest C++17 baseline;
- `Result`;
- `Timestamp`;
- generic `BoundedQueue`;
- strong Task/Attempt/Session/Worker IDs and AttemptNumber;
- checked resource value types;
- frozen Task/Attempt/Worker state enums and transition validation;
- strict Controller/Worker JSON config value parsing;
- Milestone 1 tests.

The latest verified M1 baseline has:

```text
34 / 34 tests passed
0 failed
```

This prompt is the explicit authorization to begin:

> **Milestone 2 — Linux fd ownership and EventLoop**

**Milestone 2 is the only newly authorized production milestone.**

Milestone 3, Milestone 4, and every later milestone remain unauthorized.

The generic implementation rules in
`docs/spec/Codex_Full_Implementation_Prompt_v1.md`
remain authoritative, except that its old “Milestone 1 only” authorization boundary is superseded
by this prompt for the current session.

Do not reinterpret this as permission to redesign the frozen architecture.

---

# 1. Repository and branch contract

Repository:

```text
nhzj121/task-scheduler
```

Stable branch:

```text
main
```

Milestone 2 development branch:

```text
milestone-2-event-loop
```

Windows working tree:

```text
E:\Deck_all_need\projects\work\task_scheduler
```

WSL view of the same working tree:

```text
/mnt/e/Deck_all_need/projects/work/task_scheduler
```

These are the same source tree. Do not clone or copy a second source tree into WSL.

The authoritative Linux build tree is:

```text
/home/nhzj121/build/task_scheduler
```

Do not use a Windows/NTFS `build/` directory as the formal Linux acceptance build.

---

# 2. Mandatory Git preflight

Before changing any production or test file, inspect Git state.

Run and report the relevant results for:

```bash
git status --short
git branch --show-current
git remote -v
git fetch origin
git rev-parse main
git rev-parse origin/main
git log -1 --oneline main
```

The following branch rules are mandatory.

## 2.1 `main` is read-only for Milestone 2 development

Do not implement M2 directly on `main`.

Do not commit M2 code to `main`.

Do not merge into `main`.

Do not force-push, reset, rewrite, or rebase `main`.

## 2.2 Working tree must be clean before branching

If the working tree contains unrelated uncommitted changes:

- stop;
- report them;
- do not stash, discard, reset, or overwrite them automatically.

## 2.3 Baseline must match the accepted main branch

Fetch `origin`.

If local `main` is clean and only behind `origin/main` with a safe fast-forward available, it may
be updated with `git pull --ff-only`.

If local `main` and `origin/main` have diverged, stop and report the condition instead of resolving
history automatically.

## 2.4 Work only on `milestone-2-event-loop`

If the branch does not yet exist locally:

```bash
git switch main
git switch -c milestone-2-event-loop
```

If it already exists, inspect it before using it. It must be a legitimate M2 branch based on the
accepted main baseline and must not contain unrelated work.

After switching, confirm:

```bash
git branch --show-current
```

must return:

```text
milestone-2-event-loop
```

Create/publish the remote branch if needed:

```bash
git push -u origin milestone-2-event-loop
```

This prompt authorizes ordinary milestone-scoped commits and pushes to
`milestone-2-event-loop`.

It does **not** authorize:

- merge to `main`;
- force push;
- history rewriting;
- deleting branches;
- creating or merging a PR unless explicitly requested later.

Keep commits reviewable and scoped to M2.

---

# 3. Mandatory document reading

Before implementing M2, re-read the relevant frozen contracts from the current repository.

At minimum:

1. `AGENTS.md`
2. `docs/ARCHITECTURE_GATE.md`
3. `docs/ARCHITECTURE.md`
4. `docs/OWNERSHIP.md`
5. `docs/TEST_STRATEGY.md`
6. `docs/IMPLEMENTATION_PLAN.md`
7. `docs/DECISIONS.md`
8. `docs/FAILURE_MODEL.md`
9. `docs/spec/Codex_Full_Implementation_Prompt_v1.md`
10. relevant files under `docs/spec/`

Also inspect the already-passing M1 implementation before designing M2 APIs, especially:

- `include/ts/common/`
- `src/common/`
- `include/ts/domain/`
- `src/domain/`
- `tests/`
- `CMakeLists.txt`

Preserve the accepted M1 APIs unless an actual M1 correctness defect is discovered.

If implementation appears to require changing a frozen semantic contract, stop and report it.

---

# 4. Milestone 2 scope

The frozen M2 definition is:

```text
Milestone 2 — Linux fd ownership and EventLoop

Future modules:
- common/UniqueFd
- io/epoll
- io/EventLoop
- timerfd/signalfd/eventfd wrappers

Work:
- move-only fd ownership
- generation-safe registrations
- Level Triggered readiness
- deferred removal/destruction
- post/completion channel primitives

Acceptance:
- ownership wrapper tests
- add/modify/delete registration tests
- stale fd/generation tests
- read/drain-to-EAGAIN behavior
- write-interest toggling
- timer/signal/event wakeups
- no fd leak under repeated churn

Principal risk:
- fd reuse
- dangling callbacks
- callback self-destruction
```

The purpose of M2 is to build the generic Linux fd/EventLoop substrate used later by networking and
the process runtime.

M2 must remain independent of Task scheduling semantics.

---

# 5. Allowed production areas

M2 may create or modify production code only where needed for the generic I/O layer.

Expected/allowed areas include:

```text
include/ts/common/unique_fd.h
src/common/unique_fd.cpp                 # if a .cpp is useful

include/ts/io/
src/io/

CMakeLists.txt
small CMake support files if necessary
```

Reasonable `io` components may include implementation equivalents of:

```text
EventLoop
epoll registration/token types
EventFd / eventfd wrapper
TimerFd / timerfd wrapper
SignalFd / signalfd wrapper
generic wakeup/post/completion primitive
```

Exact file/class names are implementation details as long as the frozen ownership and dependency
contracts are preserved.

Tests may be added under appropriate unit/component locations, for example:

```text
tests/unit/
tests/component/
```

Do not create empty future module trees.

---

# 6. Explicitly forbidden M2 scope

Do **not** implement any Milestone 3 or Milestone 4+ production feature.

Specifically, M2 must not implement:

- TCP sockets as a transport layer;
- TCP listener/connector abstractions;
- `TcpConnection`;
- `ConnectionManager`;
- protocol framing;
- the 20-byte wire header codec;
- JSON protocol messages;
- Controller/Worker wire roles;
- request/response correlation;
- log protocol queues;
- `fork`;
- `execve`;
- process spawning;
- `ProcessHandle`;
- `RunningTask`;
- `ExecutionManager`;
- process groups;
- waitpid/reaping logic;
- workload stdout/stderr capture;
- SQLite persistence;
- `StateStore`;
- `PersistenceThread`;
- scheduler;
- `TaskManager`;
- `WorkerRegistry`;
- `ResourceLedger`;
- Controller runtime;
- Worker runtime;
- LogWriter;
- systemd/deployment artifacts;
- benchmark features.

Linux pipes, `eventfd`, `timerfd`, `signalfd`, and other test-only file descriptors may be used to
test the generic I/O substrate, but must not become an accidental process/network runtime.

Do not implement M3/M4 “because it makes EventLoop easier to demonstrate.”

---

# 7. Module dependency contract

The frozen dependency direction requires:

```text
common
  ↑
 io
```

`io` may depend on `common`.

`io` must not depend on:

- `domain`;
- `protocol`;
- `net`;
- `runtime`;
- `scheduler`;
- `storage`;
- `controller`;
- `worker`;
- `client`.

The generic I/O layer knows:

- file descriptors;
- readiness masks;
- registration identities;
- callbacks/handlers;
- wakeups/timers/signals;

but it does not know:

- Tasks;
- Attempts;
- Workers;
- TCP framing;
- process lifecycle;
- database transactions;
- scheduling.

Preserve this direction in CMake targets and includes.

---

# 8. `UniqueFd` contract

Implement a Linux fd ownership wrapper equivalent to the frozen ownership contract.

Required semantics:

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

Exact spelling may vary only if the same semantics are preserved.

Requirements:

- exactly one owner for an owned fd;
- default state is invalid;
- copy is deleted;
- move transfers ownership;
- moved-from object becomes invalid;
- destructor never throws;
- `get()` is non-owning observation;
- `release()` transfers ownership and leaves the wrapper invalid;
- `reset()` closes the previously owned fd, then adopts the replacement according to a safe,
  documented self-reset policy;
- no double-close;
- no ownership hidden in raw `int`;
- wrappers that create kernel fds should use CLOEXEC at creation where the Linux API supports it.

On Linux, do not blindly retry `close()` after `EINTR`; a retry can close a reused descriptor.
The destructor must remain non-throwing.

Important:

`UniqueFd` does **not** know whether the fd is registered with epoll.

A registered fd must be unregistered before the owning `UniqueFd` closes/resets it.

Do not make `UniqueFd` implicitly unregister from EventLoop.

---

# 9. EventLoop ownership contract

The EventLoop owns:

- its epoll fd;
- its registration table;
- registration generations/identities;
- its own generic deferred-action bookkeeping.

The EventLoop does **not** own the registered fd merely because the fd is registered.

The registered object's actual owner remains elsewhere.

The EventLoop must never use `shared_ptr` everywhere as a substitute for lifecycle design.

The EventLoop must not store an owning raw pointer to an upper-layer object in epoll user data.

---

# 10. epoll creation and trigger model

Use Linux epoll directly.

Requirements:

- create epoll with `epoll_create1(EPOLL_CLOEXEC)`;
- use Level Triggered semantics;
- do not add `EPOLLET`;
- do not silently switch to edge-triggered or one-shot semantics;
- use non-blocking file descriptors for readiness-driven sources where appropriate;
- handle `EINTR` explicitly around `epoll_wait`;
- EventLoop callbacks must be short and must not introduce hidden blocking I/O.

Level Triggered behavior must be observable in tests:

- if readable state is not fully drained, readiness remains visible;
- once drained to `EAGAIN`, readable readiness stops until new data arrives.

The EventLoop itself does not need to read application data. The registered handler owns the
source-specific drain behavior.

---

# 11. Generation-safe registration contract

Fd numbers are reusable Linux integers.

Therefore the EventLoop must not identify a live registration using only:

```text
fd
```

and must not place a bare owner pointer in `epoll_event.data.ptr`.

Use a stable registration identity/generation scheme equivalent to the frozen concept:

```text
RegistrationToken = {
    fd,
    generation,
    handler_kind,
    owner_id
}
```

The exact internal representation may differ, but all of these semantics must hold:

1. every registration has a generation/identity that changes across replacement/re-registration;
2. kernel epoll user data resolves through EventLoop-owned registration state;
3. a stale event cannot be applied to a new logical registration that happens to reuse the same
   integer fd;
4. before invoking a handler, EventLoop re-validates that the registration is still current;
5. removal invalidates the registration so later stale events are ignored;
6. upper modules can later associate a stable `owner_id`/kind without EventLoop knowing Task or TCP
   semantics.

A robust implementation may use a unique 64-bit registration key in `epoll_event.data.u64` and
resolve it through the registration table. Do not store a pointer whose pointee may be destroyed
before a queued epoll event is consumed.

Avoid designs where fd reuse can cause an event for old object A to call new object B.

---

# 12. Registration API requirements

Provide explicit operations equivalent to:

```text
add/register
modify interest
remove/unregister
```

They must support later use by sockets, pipes, signalfd, timerfd, and eventfd without knowing those
higher-level semantics.

Requirements:

- duplicate registration is diagnosed;
- invalid/stale registration operations are diagnosed or safely ignored according to an explicit
  API contract;
- interest masks are explicit;
- readable and writable interest can be independently changed;
- `EPOLLOUT` is not permanently enabled by EventLoop policy;
- owner code can enable writable interest only while pending output exists in later milestones.

Mandatory owner cleanup order remains:

```text
1. logically stop new work for the resource
2. EPOLL_CTL_DEL
3. invalidate/remove registration token
4. close/reset owning UniqueFd
5. destroy owning object after callback dispatch can no longer reference it
```

M2 should provide the primitives needed to respect this order; M2 must not invent upper-layer
object ownership.

---

# 13. Callback lifetime and self-removal

A callback must be able to request removal of its own registration without causing use-after-free,
iterator invalidation, or dispatch through a dangling pointer.

The EventLoop dispatch algorithm must be safe when callbacks:

- unregister themselves;
- unregister another registration;
- modify their own interest;
- schedule owner destruction after the callback;
- cause later events from the current epoll batch to become stale.

Do not immediately destroy an upper-layer owner from a callback while the current dispatch batch
still relies on its identity.

Provide a generic deferred-action/destruction mechanism or equivalent safe dispatch discipline.

Deferred work must run only at a defined safe point after the relevant current epoll callback/batch
no longer references the owner identity.

If an object is removed earlier in the current event batch, later events for its stale registration
must be ignored after generation validation.

The EventLoop remains the owner of registration records, not of the higher-level objects.

---

# 14. Thread-affinity and cross-thread rules

The intended EventLoop state is single-writer/thread-affine.

Do not add locks around every EventLoop operation merely to make the entire loop generally
thread-safe.

Normal registration mutation and callback dispatch should occur on the EventLoop thread.

Only the explicitly designed wakeup/post/completion path may cross threads.

The existing generic `BoundedQueue` has blocking and non-blocking APIs.

Critical rule:

```text
EventLoop thread must never call BoundedQueue::wait_pop().
```

The EventLoop side must use non-blocking queue draining such as `try_pop()`.

A dedicated producer/consumer thread in later milestones may use blocking APIs where its ownership
contract allows it.

---

# 15. `eventfd` primitive

Provide a generic Linux `eventfd` wrapper/primitives suitable for later cross-thread completion
wakeup.

Requirements:

- create with `EFD_NONBLOCK | EFD_CLOEXEC`;
- unique fd ownership;
- explicit notify/signal operation;
- drain operation;
- handle `EINTR`;
- drain until `EAGAIN` where applicable;
- do not busy-spin;
- do not lose a wakeup under normal supported usage;
- do not encode persistence/log semantics into this generic type.

If M2 implements a generic post/completion channel using the existing bounded queue plus eventfd:

- capacity must remain bounded;
- producer failure/full/stopped state must be explicit;
- successful enqueue must cause a wakeup;
- EventLoop drains the eventfd and then drains queued values non-blockingly;
- no condition-variable wait is allowed on the EventLoop thread.

Do not implement `PersistenceQueue` or `LogQueue` semantics in M2.

---

# 16. `timerfd` primitive

Provide a generic timerfd wrapper suitable for later task timers and Controller maintenance timers.

Requirements:

- `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)`;
- one-shot arming;
- periodic arming only if kept generic and useful;
- explicit disarm;
- explicit drain/read of expiration count;
- handle `EINTR`;
- read until drained / `EAGAIN` as appropriate;
- no Task/Worker timeout policy inside the wrapper.

M2 tests should validate expiration delivery and disarm/rearm behavior without relying on large
arbitrary sleeps as correctness assertions.

Use bounded deadlines and observable readiness.

---

# 17. `signalfd` primitive

Provide a generic signalfd wrapper suitable for later Worker `SIGCHLD` integration.

Requirements:

- create with `SFD_NONBLOCK | SFD_CLOEXEC`;
- consume `signalfd_siginfo` records;
- drain until `EAGAIN`;
- no asynchronous signal-handler business logic;
- no Worker/child reaping logic in M2.

Signal-mask ownership policy remains an upper-layer responsibility.

The wrapper must not silently impose Worker-specific global signal semantics.

For tests, a test-owned signal such as `SIGUSR1` may be blocked in a controlled scope, delivered,
consumed through signalfd, and the original signal mask restored afterward.

Do not implement `waitpid` or `SIGCHLD` workload semantics in M2.

---

# 18. Error handling rules

M2 errors must be explicit.

Do not:

- swallow failed `epoll_ctl`;
- log-and-continue after an invariant violation;
- silently reuse stale registration tokens;
- convert a failed kernel operation into success;
- throw from RAII destructors;
- close an fd twice to “be safe”;
- retry `close()` blindly on Linux after `EINTR`.

Use existing `Result` or a small M2-local error value where appropriate.

Do not redesign M1 `Result` into a universal structured-error framework just for M2.

Where errno is relevant for debugging, preserve enough information for meaningful tests/reporting.

---

# 19. M2 test requirements

Tests are part of Milestone 2, not optional hardening.

All existing M1 tests must remain passing.

At minimum add tests for the following.

## 19.1 UniqueFd

Verify:

- default-invalid state;
- destruction closes exactly once;
- move construction;
- move assignment;
- moved-from invalid state;
- `release()` transfers ownership;
- `reset()` closes old ownership and adopts new ownership correctly;
- safe self-reset policy;
- no double-close after fd-number reuse;
- wrapper destruction does not throw.

## 19.2 Registration add/modify/remove

Verify:

- add/register succeeds;
- duplicate add is rejected/diagnosed;
- modify changes interest;
- remove unregisters;
- removed registrations no longer dispatch;
- stale registration operations do not mutate current state.

## 19.3 fd reuse / generation safety

Create a deterministic test or test seam proving:

```text
old registration for fd N
    ↓ removed/closed
Linux reuses integer fd N
    ↓ new registration
old/stale event identity
    ↓
must NOT invoke the new owner
```

Do not rely only on “we did not observe a failure.”

Test the generation/identity rule directly.

## 19.4 Level Triggered readiness

Use a non-blocking pipe/event source to prove:

- partial/non-drained readable state remains ready across loop iterations;
- draining to `EAGAIN` removes readiness until new input arrives.

Do not add `EPOLLET`.

## 19.5 Writable-interest toggling

Using a generic Linux fd suitable for the test, prove:

- writable interest may be enabled;
- writable readiness is delivered;
- interest may be removed/disabled;
- EventLoop does not continuously request writable callbacks after it is no longer needed.

This is an EventLoop interest test, not a TCP implementation.

## 19.6 Callback self-removal / deferred lifetime

Verify:

- callback can unregister itself safely;
- callback can request deferred destruction/action;
- later events in the same batch do not call a stale registration;
- removing another registration during dispatch is safe;
- no owning raw pointer remains reachable after the declared safe point.

## 19.7 eventfd wakeup

Verify:

- eventfd wakes the loop;
- multiple notifications are correctly drained/aggregated according to the wrapper contract;
- cross-thread notification works;
- EventLoop performs no blocking queue wait.

## 19.8 timerfd

Verify:

- timer becomes readable;
- expiration count is consumed;
- disarm prevents a pending future expiration;
- rearm works;
- monotonic clock is used.

## 19.9 signalfd

Verify:

- a blocked test signal is delivered through signalfd;
- records are drained correctly;
- test restores its signal mask;
- no async signal handler is required.

## 19.10 Repeated churn / fd leak

Stress registration and fd ownership through repeated creation/removal/destruction.

Use Linux diagnostics such as `/proc/self/fd` where practical.

Prove no growing fd leak beyond stable expected infrastructure descriptors.

Also stress:

- fd reuse;
- self-removal;
- deferred actions;
- repeated wakeup/timer registration.

---

# 20. Deterministic testing rules

Do not use arbitrary long `sleep()` calls as correctness assertions.

Use:

- readiness;
- bounded polling;
- condition/future only outside EventLoop thread;
- explicit test deadlines;
- eventfd/timerfd observability;
- deterministic registration test seams where needed.

A timeout may protect a test from hanging, but “wait N ms and hope” is not the correctness model.

Every failing component test should provide enough detail to identify the fd/token/event that failed.

---

# 21. Sanitizer and diagnostics expectations

M2 introduces fd lifetime and callback lifetime risks.

At minimum:

- keep warnings-as-errors;
- run the normal full CTest suite;
- run repeated M2 stress tests.

If the repository already has practical ASan/UBSan/TSan support, use it for M2 component tests.

If it does not yet have sanitizer infrastructure, do not turn M2 into a CI/tooling milestone solely
to add a large sanitizer framework. Report this as a follow-up for the documented sanitizer phase.

Do not claim sanitizer success unless an actual sanitizer run was executed.

---

# 22. Formal Linux build/test environment

The formal acceptance environment is WSL Ubuntu 22.04, not MinGW.

Source:

```text
/mnt/e/Deck_all_need/projects/work/task_scheduler
```

Build tree:

```text
/home/nhzj121/build/task_scheduler
```

Use the existing WSL GCC/CMake baseline.

From Windows Codex Desktop, invoke WSL explicitly where needed.

Formal configure:

```bash
wsl.exe -d Ubuntu22.04.3 -- bash -lc '
set -euo pipefail
cmake   -S /mnt/e/Deck_all_need/projects/work/task_scheduler   -B /home/nhzj121/build/task_scheduler   -DCMAKE_BUILD_TYPE=Debug
'
```

Formal build:

```bash
wsl.exe -d Ubuntu22.04.3 -- bash -lc '
set -euo pipefail
cmake --build /home/nhzj121/build/task_scheduler -j
'
```

Formal tests:

```bash
wsl.exe -d Ubuntu22.04.3 -- bash -lc '
set -euo pipefail
ctest   --test-dir /home/nhzj121/build/task_scheduler   --output-on-failure
'
```

Because M2 changes CMake targets/sources, perform configure again before the final acceptance build.

Do not delete unrelated files under `/home/nhzj121/build/task_scheduler`; that build directory may
contain the user's local `BUILD_COMMANDS.md`.

If a clean object rebuild is needed, use CMake's clean target or another non-destructive approach
rather than deleting the user's local note.

Do not treat a successful Windows/MinGW build as M2 acceptance evidence.

---

# 23. M1 regression requirement

M2 must preserve all accepted M1 behavior.

Before declaring M2 complete:

- all previous M1 tests must still pass;
- M1 enum/state semantics must remain unchanged;
- strong IDs/resources/config behavior must remain unchanged;
- do not rewrite M1 APIs merely for EventLoop convenience.

If an actual M1 defect is discovered:

- fix it only if it is clearly a correctness issue;
- explain it explicitly in the M2 report;
- confirm no architecture semantic change;
- rerun the full regression suite.

---

# 24. Suggested implementation sequence

Use small coherent steps.

A recommended order is:

```text
1. Git/branch preflight
2. reread M2 contracts
3. UniqueFd + tests
4. epoll registration table/token/generation model + tests
5. EventLoop run/dispatch/add/modify/remove + LT tests
6. self-removal + deferred-action safety tests
7. eventfd wrapper / wakeup primitive + tests
8. timerfd wrapper + tests
9. signalfd wrapper + tests
10. repeated churn/fd-reuse/leak stress tests
11. full M1+M2 build/test
12. focused M2 source review
13. milestone commits/push
14. Milestone 2 report
15. STOP
```

Do not start M3 or M4 after step 14.

---

# 25. Architecture-change stop rule

Stop implementation and report before changing any frozen contract.

Examples that require stopping:

- changing Task/Attempt/Worker states;
- modifying protocol IDs/schema;
- changing ownership of sockets/tasks/sessions;
- changing persistence semantics;
- changing failure mappings;
- replacing LT epoll with ET;
- making EventLoop own upper-layer objects;
- introducing a general thread pool;
- introducing a new external dependency/subsystem;
- changing the Controller/Worker concurrency model;
- adding M3 network semantics;
- adding M4 process semantics.

For M2 specifically, stop if the only way forward appears to require:

- raw owner pointers in epoll user data;
- no generation protection;
- closing registered fds before unregistering;
- blocking waits on EventLoop thread;
- unbounded cross-thread queues;
- broad shared ownership to hide callback lifetime.

Those are implementation failures, not acceptable shortcuts.

---

# 26. Source review before M2 PASS

Before claiming success, perform a focused review for:

- every fd has one clear owner;
- every kernel-created fd has CLOEXEC where applicable;
- readiness-driven fds are non-blocking where required;
- EventLoop owns epoll + registration table, not source fds/owners;
- no bare fd is the sole stale-event identity;
- no epoll user-data pointer can dangle;
- stale generation is rejected;
- unregister precedes owner fd close in tests/usage;
- callback self-removal is safe;
- deferred actions run at the documented safe point;
- EventLoop does not call `BoundedQueue::wait_pop()`;
- no hidden Task/TCP/process/database semantics entered `io`;
- no M1 regression;
- no TODO/fake production behavior;
- no disabled required test.

---

# 27. Git completion and remote review state

After M2 implementation and formal WSL tests pass:

1. inspect:
   ```bash
   git status
   git diff --check
   git diff --stat main...HEAD
   git log --oneline main..HEAD
   ```
2. ensure only M2-scoped changes exist;
3. commit remaining valid M2 changes on `milestone-2-event-loop`;
4. push:
   ```bash
   git push origin milestone-2-event-loop
   ```
5. verify the branch is up to date on `origin`.

Do not merge it.

The branch must remain available for external review as:

```text
main...milestone-2-event-loop
```

---

# 28. Milestone 2 report format

At the end, provide a report with the following sections.

## A. Git / baseline

Include:

- current branch;
- main baseline commit SHA;
- M2 head commit SHA;
- whether remote branch was pushed;
- whether working tree is clean.

## B. Implementation summary

Describe:

- UniqueFd;
- EventLoop;
- registration/generation strategy;
- deferred callback/destruction strategy;
- eventfd;
- timerfd;
- signalfd;
- cross-thread wake/post primitive if implemented.

Also state what was intentionally not implemented.

## C. Files changed

List:

- files added;
- files modified;
- files removed.

## D. Ownership / lifetime review

Explain:

- who owns epoll fd;
- who owns registered source fds;
- how registration identity/generation works;
- how fd reuse is made safe;
- how callback self-removal is handled;
- when deferred actions run;
- how unregister-before-close is preserved.

## E. Build and test evidence

Provide exact commands and concise results for:

- configure;
- build;
- full CTest;
- focused M2 stress/repeat tests;
- sanitizer runs only if actually executed.

Report final test count and failures.

## F. Acceptance matrix

Explicitly report PASS/FAIL for:

```text
UniqueFd ownership
epoll add/modify/delete
Level Triggered behavior
stale fd/generation safety
fd reuse protection
callback self-removal
deferred destruction
writable-interest toggling
eventfd wakeup
timerfd wakeup/disarm/rearm
signalfd delivery/drain
cross-thread post/completion primitive
repeated churn/no fd leak
M1 regression
module dependency direction
scope compliance
```

## G. Deviations

If none:

```text
No architecture or semantic deviation.
```

If any exist, describe them precisely.

## H. Risks / follow-up

List only real issues relevant to later M3/M4 integration.

Do not begin solving M3/M4 issues in this milestone.

## I. Git review summary

Include:

```text
git diff --stat main...HEAD
git log --oneline main..HEAD
```

or concise equivalent evidence.

## J. Gate

End with exactly:

```text
Milestone 2: PASS — awaiting explicit review; Milestone 3 and Milestone 4 have not started.
```

or:

```text
Milestone 2: NOT PASSED — blockers remain.
```

Then STOP.

---

# 29. Final instruction

Milestone 2 is a generic Linux I/O foundation.

Implement:

```text
ownership
fd lifecycle
epoll LT
registration identity/generation
safe callback dispatch
deferred destruction
eventfd
timerfd
signalfd
bounded wakeup/post primitives
```

Do not implement the systems that will later consume those primitives.

Preserve the frozen architecture.

Preserve M1.

Work only on:

```text
milestone-2-event-loop
```

Do not modify `main`.

Do not start M3.

Do not start M4.

Milestone 2 is authorized.
