# Milestone 4 Authorization / Codex Development Prompt
## Linux C++ Distributed Task Scheduler
## Authorized scope: Milestone 4 — Standalone Local Process Runtime

## 0. Authorization status

Architecture Gate, Milestone 1, and Milestone 2 have passed. Milestone 2 has been merged into `main`.

This prompt explicitly authorizes **Milestone 4 only**.

Milestone 3 remains unstarted. Milestone 5+ remain unauthorized.

The generic rules in `docs/spec/Codex_Full_Implementation_Prompt_v1.md` remain authoritative except for its historical milestone-authorization boundary; this prompt supersedes that boundary for this branch/session.

Do not redesign frozen architecture semantics.

## 1. Repository and branch

Repository:

```text
nhzj121/task-scheduler
```

Stable branch:

```text
main
```

Required development branch:

```text
milestone-4-process-runtime
```

Windows working tree:

```text
E:\Deck_all_need\projects\work\task_scheduler
```

WSL view of the same source tree:

```text
/mnt/e/Deck_all_need/projects/work/task_scheduler
```

Formal Linux build tree:

```text
/home/nhzj121/build/task_scheduler
```

Do not clone or copy another source tree into WSL.

## 2. Git preflight

Before production changes run:

```bash
git status --short
git branch --show-current
git remote -v
git fetch origin
git rev-parse main
git rev-parse origin/main
git log -1 --oneline main
```

Rules:

- `main` is read-only for M4 implementation.
- Do not commit M4 code to `main`.
- Do not merge M4 into `main`.
- Do not force-push, reset, or rewrite `main`.
- If local/remote main diverge, stop and report.
- If unrelated uncommitted work exists, stop; do not stash/discard/reset it automatically.

One narrow exception is allowed: `docs/spec/Milestone_4_Authorization_Codex_Development_Prompt.md` may already exist as the only untracked/uncommitted file because the user may place this authorization document into `docs/spec/` before branching. If so, carry it onto the M4 branch; do not commit it to `main`.

Create/switch to:

```bash
git switch main
git switch -c milestone-4-process-runtime
```

or inspect an existing M4 branch before using it.

Confirm:

```bash
git branch --show-current
```

returns:

```text
milestone-4-process-runtime
```

Normal M4-scoped commits and pushes to this branch are authorized.

## 3. Mandatory reading

Before implementing, re-read:

1. `AGENTS.md`
2. `docs/ARCHITECTURE_GATE.md`
3. `docs/ARCHITECTURE.md`
4. `docs/OWNERSHIP.md`
5. `docs/STATE_MACHINE.md`
6. `docs/FAILURE_MODEL.md`
7. `docs/TEST_STRATEGY.md`
8. `docs/IMPLEMENTATION_PLAN.md`
9. `docs/DECISIONS.md`
10. `docs/PROJECT_SCOPE.md`
11. `docs/spec/Codex_Full_Implementation_Prompt_v1.md`
12. relevant files under `docs/spec/`
13. accepted M1/M2 implementation and tests

Special attention:

- `IMPLEMENTATION_PLAN.md` Milestone 4;
- `OWNERSHIP.md` RunningTask / ProcessHandle / spawn transfer / reaping;
- `ARCHITECTURE.md` pre-fork execution preparation;
- `FAILURE_MODEL.md` timeout, natural leader exit, executable preparation failure;
- ADR-014 and ADR-015;
- `TEST_STRATEGY.md` runtime integration matrix.

If implementation exposes a real architectural contradiction, stop and report instead of silently choosing a new semantic rule.

## 4. M4 scope

M4 builds a **standalone local Linux process execution runtime** on top of M2.

Allowed production areas are primarily:

```text
include/ts/runtime/
src/runtime/
CMakeLists.txt
```

Small lower-layer fixes are allowed only for genuine generic M1/M2 defects and must be separately explained.

`runtime` may depend on:

```text
common
domain
io
```

It must not depend on:

```text
net
protocol
storage
scheduler
controller
worker
client
apps
```

Required M4 work includes:

```text
parent-side environment/argv/envp/cwd/PATH preparation
/dev/null stdin
stdout/stderr pipes
exec-status pipe
fork
minimal async-signal-safe child path
setpgid
dup2
fchdir
signal-mask restoration
direct execve
exec success/failure detection
non-blocking stdout/stderr capture
shared signalfd SIGCHLD integration
waitpid(-1, ..., WNOHANG) drain loop
ProcessHandle/running-process ownership
timeout/grace timerfd lifecycle
SIGTERM -> grace -> SIGKILL
natural leader result freezing
residual same-pgid cleanup
final stdout/stderr drain
explicit local cancellation/termination
runtime stress/fd/zombie tests
```

## 5. Explicitly forbidden scope

Do not implement:

- TCP, listeners, connectors;
- `TcpConnection`, `ConnectionManager`;
- protocol framing or JSON messages;
- Worker registration/heartbeat/session protocol;
- SQLite/StateStore/PersistenceThread;
- Scheduler/TaskManager/WorkerRegistry/ResourceLedger;
- Controller runtime or Worker network runtime;
- LogWriter or durable log path;
- log credit/window protocol;
- `taskctl`;
- deployment/systemd/benchmark work;
- M3 or M5+ features.

Do not use:

```text
system()
/bin/sh -c
execvp
```

Do not add cgroups, containers, or namespaces.

Test-only helper executables are allowed.

## 6. Runtime/domain boundary

M4 owns OS/runtime facts, not business Task state.

Do not add or mutate frozen Task/Attempt states in runtime code.

A runtime outcome may preserve:

```text
launch success/failure
exit code
terminating signal
launch-failure stage + errno
timeout cause
cancel cause
raw stdout/stderr bytes/events
```

Later Worker/Controller layers map these facts to business states.

## 7. Prepared execution contract

All fallible C++ preparation happens in the parent before `fork`.

A move-only prepared-execution value must own everything required by the child without child-side C++ construction.

### Environment

- Snapshot inherited environment in parent.
- Last inherited duplicate key wins.
- Task/runtime override wins over inherited value.
- Key is nonempty and contains neither `=` nor NUL.
- Value contains no NUL.
- Emit final environment in deterministic lexicographic key order.
- Final merged `PATH` is authoritative.
- No environment merging in child.

### cwd

Capture absolute startup cwd before launches.

Resolve:

```text
null cwd      -> startup cwd
absolute cwd  -> that path
relative cwd  -> relative to startup cwd
```

Parent opens the execution directory with:

```text
O_RDONLY | O_DIRECTORY | O_CLOEXEC
```

Failure is pre-fork launch preparation failure.

### stdin

Parent opens `/dev/null` for child stdin using close-on-exec ownership.

No interactive stdin in v1.

### argv/envp stability

- `argv[0]` remains the submitted executable token.
- Submitted args follow.
- Finalize all owned strings before building pointer arrays.
- Build null-terminated stable `argv`/`envp` arrays only after storage is stable.
- No later vector/string reallocation may invalidate them.

### Executable resolution

If submitted executable contains `/`:

- no PATH lookup;
- pass submitted path to `execve`;
- relative slash path is interpreted after prepared cwd is applied.

If executable has no `/`:

- final merged PATH must exist;
- split on `:` preserving empty elements;
- empty entry means prepared cwd;
- relative PATH entry is resolved against prepared cwd;
- absolute PATH entry is used directly;
- choose first candidate that is not a directory and is executable under effective credentials;
- retain resolved absolute candidate for `execve`.

Missing PATH or no candidate is pre-fork launch preparation failure.

The documented trusted-environment check/exec TOCTOU remains accepted.

## 8. Pipe/fd setup

Before `fork`, create:

```text
stdout pipe
stderr pipe
exec-status pipe
```

Use close-on-exec creation where applicable.

Parent read ends become non-blocking before EventLoop registration.

Maintain exact close discipline on every success/failure path.

No fd leaks on:

- preparation failure;
- pipe failure;
- fork failure;
- child setup failure;
- exec failure;
- normal exit;
- timeout;
- cancel;
- stress churn.

Do not wait for child exit before reading stdout/stderr; that would recreate finite-pipe deadlock.

## 9. Exec-status pipe

The exec-status pipe distinguishes successful exec from setup/exec failure.

Requirements:

- child write end is CLOEXEC;
- successful `execve` -> EOF with no failure record;
- setup/exec failure -> fixed allocation-free record containing at least failure stage + errno;
- child uses only async-signal-safe writing;
- parent supports partial non-blocking record accumulation;
- malformed/truncated record is explicit runtime/internal launch failure, never success;
- workload exit code 127 after successful exec must not be confused with exec failure.

Do not generate child failure text strings in the child.

## 10. Post-fork child path

Between `fork` and `execve`, child code must not:

- allocate C++ containers;
- construct strings;
- perform PATH lookup;
- merge environment;
- log;
- use iostream/spdlog;
- throw/catch exceptions;
- acquire C++ mutexes;
- call hidden unsafe helper layers.

The reachable child path should be restricted to the prepared low-level lifecycle operations equivalent to:

```text
close
setpgid
dup2
fchdir
restore prepared signal mask
close redundant fds
execve
fixed failure-record write
_exit
```

Before M4 PASS, audit every function actually reachable in this path.

## 11. Process groups

Each workload is a new process group.

The direct child is the workload leader.

Parent and child both use the benign `setpgid(pid,pid)` race-closing pattern.

Do not accept arbitrary `setpgid` failure as success. A usable pgid may be recorded only when the intended group is established/confirmed.

All lifecycle signals target:

```text
kill(-pgid, signal)
```

not merely the leader pid.

Never signal a negative pgid before group ownership is valid.

## 12. Shared SIGCHLD/signalfd model

Use the M2 signalfd/EventLoop substrate.

- Block `SIGCHLD` before child creation.
- Use one shared signalfd for runtime child-state notification.
- Do not create one signalfd per child.
- Do not install an async SIGCHLD business handler.
- On signalfd readiness, drain it and then call:

```text
waitpid(-1, &status, WNOHANG)
```

until no reapable direct child remains.

Map every returned pid to exactly one runtime-owned process object.

The signalfd record itself does not identify lifecycle ownership.

## 13. Runtime ownership

Use explicit ownership equivalent to:

```text
standalone ProcessRuntime/test coordinator
  owns running-process objects

running process owns:
  ProcessHandle
  stdout read UniqueFd
  stderr read UniqueFd
  exec-status read UniqueFd
  timeout/grace TimerFd
  PreparedExec while required
  EventLoop registration tokens
  runtime-local output state
```

`ProcessHandle` is move-only and preserves at least:

```text
leader pid
pgid
group-establishment state
reap state
termination cause
immutable leader outcome once frozen
group-cleanup phase
```

A normal destructor must not silently SIGKILL a healthy workload.

The owner must not destroy a live/unreaped process lifecycle accidentally.

## 14. EventLoop callback lifetime

Preserve M2 contracts:

- EventLoop owns registrations, not source fds/owners;
- no owning raw pointers in callbacks;
- use stable runtime IDs/generations or equivalent owner lookup;
- unregister before `UniqueFd` close;
- owner destruction occurs only after safe callback/deferred point;
- do not bypass generation/fd-reuse protection.

## 15. stdout/stderr capture

stdout and stderr remain separate raw byte streams.

Requirements:

- non-blocking parent read ends;
- drain readiness to `EAGAIN`;
- preserve arbitrary bytes including NUL/non-UTF-8;
- independent EOF tracking;
- continue drain after leader reap;
- rapid exit must not lose tail;
- large output must not deadlock;
- no implicit text decoding;
- no stdout/stderr merge.

M4 does not implement base64, protocol framing, log credit, durable files, or network transport.

Avoid a permanently unbounded production-owned “collect all output forever” buffer. A chunk/event callback to a finite test sink is acceptable.

Design so later M8 can pause/resume pipe read interest without rewriting process lifecycle semantics, but do not implement M8 credit now.

## 16. Exec-success boundary

Timeout begins only after exec success is proven by the exec-status pipe.

M4 should expose this as a runtime-local launch-success fact/event.

Do not mutate `AttemptState::RUNNING` in runtime code.

A child that fails before successful exec never becomes runtime-started.

## 17. Natural leader outcome

When direct child `waitpid` result is observed without a previously installed timeout/cancel cause:

1. freeze natural exit code or signal immediately;
2. never overwrite it because cleanup signals are later sent;
3. probe the process group;
4. if same-pgid members remain, perform:

```text
SIGTERM
-> grace timer
-> SIGKILL if still present
```

5. continue group-disappearance checks;
6. continue stdout/stderr drain to EOF;
7. complete only after all runtime completion conditions are satisfied.

Leader exit 0 remains natural success fact even if residual child cleanup later uses SIGKILL.

Late timeout/cancel must not rewrite a frozen natural outcome.

## 18. Timeout and cancellation

Timeout uses M2 monotonic timerfd and begins only after exec success.

Timeout flow:

```text
install TIMEOUT as first termination cause
SIGTERM -pgid
arm grace
if group remains -> SIGKILL -pgid
continue reap + drain
```

Cancellation uses the same mechanics with a distinct cause.

First installed termination cause wins.

Do not claim completion merely because SIGKILL was sent.

M4 local cancellation is runtime-only; do not implement Controller cancel protocol or `cancel_requested_at`.

## 19. Process-group probing

Use known valid pgid only.

Conceptually:

```text
kill(-pgid, 0)
```

Interpret:

```text
0      -> group members may remain
EPERM  -> group members may remain
ESRCH  -> group absent
```

Other errors are explicit diagnostics/failures.

Group probing does not replace `waitpid` of the direct child.

## 20. Escaped descendant limitation

A descendant that calls `setsid()` or otherwise leaves the workload group is outside v1 termination control.

If it retains stdout/stderr write fds, EOF and completion may remain pending indefinitely.

Do not fabricate completion and do not add cgroups/namespaces to fix it.

Test this limitation with a controlled helper and externally clean the escaped fixture so the test suite terminates.

## 21. Race handling

Handle/test:

- child exits before registrations settle;
- SIGCHLD near exec-status handling;
- pipe EOF before/after reap;
- exec-status EOF and leader exit close together;
- many child exits in one notification;
- timeout/cancel racing natural exit.

Retain facts until lifecycle resolution. Do not infer semantics from event arrival order alone.

## 22. Runtime completion contract

A local launched process is runtime-complete only after required local facts are settled, including as applicable:

- direct child reaped;
- natural/termination outcome fixed;
- same-pgid cleanup resolved;
- stdout EOF/diagnosed closure;
- stderr EOF/diagnosed closure;
- exec-status lifecycle resolved;
- timers disarmed/unregistered;
- all EventLoop registrations safely removed.

M4 does not generate `TASK_FINISHED`.

Return/emit a structured local runtime completion result for later Worker integration.

## 23. Fixed child-failure stages

Define a small fixed child-safe stage enum/record for setup/exec failures, with equivalent stages such as:

```text
setpgid
stdin dup
stdout dup
stderr dup
fchdir
signal-mask restore
execve
```

Exact names are implementation details.

Requirements:

- fixed representation;
- no allocation;
- exact errno retained;
- `execve` failure distinguishable from setup failure;
- no protocol numeric IDs.

## 24. Error handling

Do not:

- swallow pipe/fork/fcntl/epoll/waitpid/kill/timer errors;
- infer exec success from exit code;
- treat unknown child state as success;
- ignore a live unreaped child;
- double-close fds;
- block EventLoop on pipe reads;
- use blocking `waitpid(...,0)` on EventLoop path;
- continue after ownership corruption.

`EINTR` handling must be syscall-appropriate.

`EAGAIN` on non-blocking pipe read ends the current drain only.

`ECHILD` is not automatically proof of correct completion; reconcile it against owned lifecycle state.

## 25. Test-only helper workloads

Create deterministic tiny helper executables only for tests.

Useful modes include:

```text
exit 0
exit N
raise signal
stdout/stderr output
binary output
large output
print argv/env/cwd
ignore SIGTERM
handle SIGTERM
spawn same-pgid descendant
leader exits while descendant holds pipe
descendant setsid and holds pipe
rapid exit
```

Prefer direct helper executables over shell scripts where exec semantics matter.

## 26. Required M4 tests

All accepted M1/M2 tests must remain passing.

Add tests for at least:

### Preparation

- inherited env + override precedence;
- lexicographic env order;
- exact argv and `argv[0]`;
- null/absolute/relative cwd;
- invalid cwd;
- PATH override;
- empty/relative/absolute PATH entries;
- missing PATH for no-slash executable;
- nonexistent no-slash executable;
- slash executable bypasses PATH;
- relative slash executable after prepared cwd;
- invalid env key/value;
- stable argv/envp through fork.

### Exec status

- exec success EOF/no failure record;
- setup/exec failure stage + errno;
- successful workload exit 127 is not exec failure;
- malformed/truncated status record if test seam can inject it.

### Outcomes

- exit 0;
- nonzero exit code;
- controlled unexpected signal;
- rapid exit;
- many simultaneous child exits;
- no direct-child zombies.

### Output

- stdout only;
- stderr only;
- interleaved streams;
- binary/NUL bytes;
- output larger than pipe capacity;
- tail drain after leader exit;
- exact bytes in test sink;
- no stream merge.

### Timeout/cancel

- timeout starts after exec success;
- cooperative timeout;
- uncooperative timeout -> SIGKILL;
- cooperative cancel;
- uncooperative cancel;
- first termination cause wins;
- late timer does not overwrite frozen natural outcome.

### Process groups

- same-pgid process tree termination;
- leader exits 0 while same-pgid descendant holds stdout;
- cleanup signals do not overwrite natural success;
- nonzero/signal leader result remains frozen through cleanup;
- escaped `setsid` limitation demonstrated and fixture externally cleaned.

### Hygiene

- no fd growth under repeated runs;
- no direct-child zombies;
- no lingering controlled groups;
- EventLoop registration count returns to baseline;
- timeout/cancel churn stable.

## 27. Async-signal-safe child-path audit

Before M4 PASS, produce a dedicated review of the exact code reachable between `fork()` and `execve()`/`_exit()`.

List every function/syscall invoked there.

Verify no hidden C++ allocation, logging, exception, mutex, PATH/env transformation, or unsafe helper is reachable.

This audit is mandatory even if all tests pass.

## 28. Stress and diagnostics

Repeat/stress at least:

- rapid exit;
- many concurrent exits;
- large output;
- timeout;
- cancellation;
- process-group cleanup;
- fd/registration churn.

Use deterministic readiness/handshakes where possible. Timeouts may guard against hangs but should not be the correctness mechanism.

Inspect `/proc` and/or equivalent diagnostics for fd/zombie evidence where practical.

Do not claim sanitizer success unless actual sanitizer runs are performed.

## 29. Formal WSL acceptance

Use WSL Ubuntu 22.04.

Source:

```text
/mnt/e/Deck_all_need/projects/work/task_scheduler
```

Build:

```text
/home/nhzj121/build/task_scheduler
```

Configure after M4 CMake changes:

```bash
wsl.exe -d Ubuntu22.04.3 -- bash -lc '
set -euo pipefail
cmake \
  -S /mnt/e/Deck_all_need/projects/work/task_scheduler \
  -B /home/nhzj121/build/task_scheduler \
  -DCMAKE_BUILD_TYPE=Debug
'
```

Build:

```bash
wsl.exe -d Ubuntu22.04.3 -- bash -lc '
set -euo pipefail
cmake --build /home/nhzj121/build/task_scheduler -j
'
```

Tests:

```bash
wsl.exe -d Ubuntu22.04.3 -- bash -lc '
set -euo pipefail
ctest \
  --test-dir /home/nhzj121/build/task_scheduler \
  --output-on-failure
'
```

Do not delete:

```text
/home/nhzj121/build/task_scheduler/BUILD_COMMANDS.md
```

Windows/MinGW success is not formal M4 acceptance evidence.

## 30. Regression requirements

Before M4 PASS:

- all M1 tests pass;
- all M2 tests pass;
- M2 generation/fd-reuse/callback lifetime behavior remains unchanged;
- accepted common/domain semantics remain unchanged.

Do not rewrite EventLoop merely for runtime convenience.

## 31. Suggested implementation order

```text
1. Git/branch preflight
2. reread M4 contracts
3. runtime outcome/error/value model
4. PreparedExec + unit tests
5. pipe/fd setup
6. fixed exec-status record
7. fork/minimal child/exec path
8. ProcessHandle ownership
9. shared SIGCHLD + waitpid drain
10. EventLoop stdout/stderr/exec-status capture
11. natural exit + final drain
12. timeout/cancel state machine
13. residual same-pgid cleanup
14. escaped-descendant fixture
15. concurrent/rapid/large-output stress tests
16. fd/zombie/process-group hygiene review
17. child-path async-signal-safe audit
18. full regression build/test
19. source/diff review
20. commit + push M4 branch
21. M4 report
22. STOP
```

Do not start M3 afterward.

## 32. Architecture stop rules

Stop rather than silently introducing:

- shell execution;
- new Task/Attempt states;
- protocol changes;
- thread-per-process design;
- async signal-handler business logic;
- blocking EventLoop waitpid;
- pid-only termination instead of pgid;
- child-side PATH/env construction;
- child-side allocation/logging;
- destructor SIGKILL of healthy workloads;
- cleanup signal replacing natural leader result;
- claims that escaped descendants are controlled;
- cgroups/namespaces;
- permanent unbounded production buffers;
- changed M2 ownership semantics;
- broad `shared_ptr` ownership to hide lifetime reasoning;
- M3/M5+ features.

## 33. Final source review checklist

Before PASS explicitly inspect:

```text
PreparedExec owns all child-needed storage
argv/envp pointers remain stable
child path contains no C++ construction after fork
no execvp/system/shell
cwd semantics match frozen contract
PATH empty/relative entries are correct
stdin is /dev/null
stdout/stderr remain separate
exec-status EOF means exec success only
exit 127 ambiguity eliminated
all pipe ends close correctly
parent read fds non-blocking
SIGCHLD blocked before child creation
one shared signalfd
waitpid(-1,WNOHANG) drains all children
pid -> owner mapping exact
ProcessHandle cannot vanish unreaped
pgid establishment proven
all signals target -pgid
first termination cause wins
natural leader outcome freezes before residual cleanup
cleanup does not rewrite outcome
stdout/stderr drain continues after reap
completion waits for EOF/group cleanup
timer registrations removed before close
EventLoop registrations removed before fd close
no raw owning pointers in callbacks
M2 fd-reuse protections intact
no zombies
no fd leak
no M3/M5+ code
```

## 34. Git completion

After validation:

```bash
git status
git diff --check
git diff --stat main...HEAD
git log --oneline main..HEAD
```

Commit valid M4 changes on:

```text
milestone-4-process-runtime
```

Push:

```bash
git push origin milestone-4-process-runtime
```

Do not merge.

Leave branch ready for review as:

```text
main...milestone-4-process-runtime
```

## 35. M4 final report

Report sections:

### A. Git / baseline

- branch;
- main baseline SHA;
- M4 HEAD SHA;
- ahead/behind;
- remote status;
- clean working tree.

### B. Implementation summary

- PreparedExec;
- env/PATH/cwd/argv preparation;
- ProcessHandle/runtime owner;
- pipes;
- exec-status record;
- child setup;
- process groups;
- signalfd/SIGCHLD;
- EventLoop output capture;
- timeout;
- cancellation;
- residual cleanup.

State intentionally excluded M3/M5+ work.

### C. Files changed

List created/modified/removed files.

### D. Ownership review

Explain pid/pgid/fd/timer/signalfd/EventLoop registration owners and cleanup order.

### E. Child-path audit

List every function/syscall reachable post-fork before exec/_exit and why the path is async-signal-safe under the frozen design.

### F. Runtime semantic matrix

PASS/FAIL at least:

```text
parent-side environment preparation
cwd preparation
PATH lookup
slash executable behavior
stable argv/envp
/dev/null stdin
exec-status success/failure distinction
exit-127 distinction
exit 0
exit nonzero
unexpected signal
stdout exact
stderr exact
binary output
large output
rapid-exit tail drain
shared SIGCHLD reap loop
many simultaneous exits
timeout
cooperative cancel
uncooperative cancel
same-pgid process-tree kill
natural leader result freeze
residual group cleanup
escaped setsid limitation demonstrated
no direct-child zombies
no fd growth
M1 regression
M2 regression
module dependency direction
scope compliance
```

### G. Build/test evidence

Give exact commands and results for configure/build/full CTest/focused repeat tests/fd-zombie diagnostics and any sanitizers actually run.

### H. Deviations

If none:

```text
No architecture or semantic deviation.
```

### I. Risks / follow-up

Only real M3/M7/M8 integration risks; do not implement them.

### J. Git review summary

Include concise `git diff --stat main...HEAD` and `git log --oneline main..HEAD`.

### K. Gate

End exactly with one of:

```text
Milestone 4: PASS — awaiting explicit review; Milestone 3 and Milestone 5+ have not started.
```

or:

```text
Milestone 4: NOT PASSED — blockers remain.
```

Then STOP.

## 36. Final instruction

M4 must prove this local lifecycle:

```text
prepare in parent
    ↓
pipes + fork
    ↓
minimal child setup
    ↓
exec success/failure proof
    ↓
EventLoop output capture
    ↓
SIGCHLD / waitpid
    ↓
natural outcome or termination cause
    ↓
process-group cleanup
    ↓
final pipe drain
    ↓
structured local completion
```

Preserve M1.

Preserve M2.

Do not implement M3.

Do not implement M5+.

Work only on:

```text
milestone-4-process-runtime
```

Do not modify or merge into `main`.

Milestone 4 is authorized.
