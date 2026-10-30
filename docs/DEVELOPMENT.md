# Development and validation

The accepted core is C++17/Linux. Follow AGENTS and the frozen docs before edits.
Core dependencies: pthreads, nlohmann_json, SQLite3, spdlog; tests additionally use
GTest and Python 3.10+. All project targets use strict GCC warnings as errors.

Use independent build trees. The established WSL formal tree is
`/home/nhzj121/build/task_scheduler`; preserve its `BUILD_COMMANDS.md` and private
notes. `cmake --build BUILD --target clean` cleans targets, not user artifacts.

```bash
cmake -S . -B /home/nhzj121/build/task_scheduler -DCMAKE_BUILD_TYPE=Debug
cmake --build /home/nhzj121/build/task_scheduler -j4
ctest --test-dir /home/nhzj121/build/task_scheduler --output-on-failure
ctest --test-dir /home/nhzj121/build/task_scheduler -L m10 --output-on-failure --repeat until-fail:20
```

Accepted labels include unit, component, runtime, m3, m5, m6, m7, m8, m9. New m10
tests exercise DESTDIR installed binaries/configs, operator errors/locks/perms,
CLI workflow, safe registry ownership/reruns and benchmark terminal accounting.
Tests use unique Linux `/tmp` leaves and loopback ports; failure artifacts remain
at the printed path. No test targets an operator data directory.

```bash
python3 scripts/run_validation.py --mode asan --source "$PWD" \
  --build "$HOME/build/task-scheduler-asan" --jobs 2
CXX=g++-12 python3 scripts/run_validation.py --mode tsan --source "$PWD" \
  --build "$HOME/build/task-scheduler-tsan" --jobs 2 --fixed-address-layout
```

`TS_SANITIZER=address` instruments all project libraries, executables and helpers
with ASan+UBSan, frame pointers, debug info and leak detection. `thread` is a
separate TSan build; never mix both. Non-PIE instrumentation executables avoid
PIE/shadow-address collisions without changing production defaults or host ASLR.
GTest discovery is deferred until test execution in these trees. The runner logs
every command/exit/duration and checks a real threaded runtime probe before tests.
ASan sets detect_leaks=1 and UBSan halt/stacktrace; TSan halt_on_error=1. No project
suppression or global leak-disable is used.

On kernels where TSan's pre-main probe reports an incompatible mapping, the runner
also tries `setarch MACHINE -R` for each instrumented process. This changes only
that process's address layout, never host ASLR/sysctl; successful execution must
still pass the real tests with no suppressions.
`--fixed-address-layout` selects this mode explicitly, including for runtimes that
exit before the probe's main without a useful mapping diagnostic.

Use GCC 12.3 for the Ubuntu 22.04 TSan job. In the measured GCC 11.4 environment,
the standalone `tests/helpers/tsan_clockwait_probe.cpp` also reports a double lock
with only standard mutex/condition-variable handshakes; GCC 12.3 runs it cleanly.
The accepted persistence tests remain unchanged. Reproduce independently with
`g++ -std=c++17 -g -fsanitize=thread -fno-pie -no-pie -pthread
tests/helpers/tsan_clockwait_probe.cpp -o /tmp/ts-clockwait-UNIQUE`, then run
`TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R /tmp/ts-clockwait-UNIQUE`.
Select a new sanitizer build tree when changing compilers. An unsupported runtime
does not excuse a project finding or permit suppressing synchronization checks.

TSan deliberately selects real queue/wakeup/persistence/log-writer handoffs rather
than claiming the whole fork-based cluster matrix. A pre-main mapping/runtime
failure is exit 2 / ENVIRONMENT_LIMITED, with zero functional tests passed; it is
not a successful sanitizer gate. Project findings are exit 1 and require repair.
`validation.json` plus logs are the evidence, not merely the presence of flags.

Production ownership graph: common/domain below io/protocol/net/runtime/storage;
storage does not depend on io (persistence channel is a separate composition);
Controller/Worker apps compose owners. No Controller child execution and no Worker
SQLite/log-file writes. See ARCHITECTURE_IMPLEMENTED for request/dataflow details.

Deterministic M5 SQLite and M8 filesystem hooks live in test-only helpers. Existing
M9 tests hold commits/barriers and inject wrong frames/short writes. Do not add
production sleep/fake success, disable a failing required test, or weaken a frozen
wire/state contract. Runtime post-fork setup remains async-signal-safe and must
not call allocator/logging/environment transformations.

Benchmark tooling is separate from CTest's quick correctness exercise:

```bash
python3 scripts/benchmark.py --bin-dir /home/nhzj121/build/task_scheduler \
  --root /tmp/task-scheduler-benchmark-NEW_UNIQUE_LEAF \
  --output /tmp/task-scheduler-benchmark-NEW_UNIQUE_LEAF/results.json \
  --workers 1 2 --tasks 20 --repeats 3 --warmup 2 --bytes 32768 --build-type Debug
```

Use a new root/output, preserve raw data and report failure scenarios. Completion
requires real durable terminal state, complete exact tails and immutable Attempt
identity. Client process/polling overhead is included. No performance change to
the scheduler, log barriers or FULL SQLite mode is allowed to improve a number.
