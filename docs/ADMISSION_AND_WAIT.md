# Known-capacity admission and taskctl wait

Approved post-v1 policy revision, 2026-11-04. This changes client submit/retry
admission. It does not change Task/Attempt transitions, placement, schema or wire IDs.

## Submission and retry

Before creating a Task UUID, charging retained metadata or posting a Submit,
Controller checks its known, last activated Worker capacity advertisements.
At least one single node must fit requested CPU and memory together. OFFLINE
advertisements retained in memory count; pending replacement advertisements do
not. Current reservations, maximum concurrency, health and socket readiness are
irrelevant to this specification check. A busy or offline node can therefore
admit work without being eligible to execute it immediately.

No known advertisements rejects, including immediately after Controller restart
until a Worker is activated. There is no advertisement persistence. If all known
nodes are too small, reject rather than awaiting an unknown future node.
Existing accepted/recovered PENDING tasks are not cancelled or re-admitted.
Acceptance does not reserve future capacity or promise eventual execution.

Only FAILED Tasks can be manually retried. Check existence/state first, then
apply the same capacity policy to the original specification. A rejected retry
does not change Task/Attempt versions, cancellation intent, counters or logs.
Accepted retry still creates no Attempt until normal scheduling commits assignment.

Both capacity errors reuse ERROR_RESPONSE with INVALID_ARGUMENT, retryable=false,
and these details:

| Field | Meaning |
| --- | --- |
| reason | NO_KNOWN_WORKERS or NO_KNOWN_WORKER_FITS |
| requested_cpu_slots | Requested CPU slots |
| requested_memory_mb | Requested accounted memory |
| known_worker_count | Number of last activated known advertisements |

No UUID is returned for rejected submission; no Task/Attempt row is inserted.
taskctl submit/retry return 3 only for the recognized typed capacity error.
Normal output goes to stderr with empty stdout; --json retains the complete
error object on stdout. Other error handling and successful UUID output stay
unchanged. Scripts must save the exit status immediately:

```bash
TASK_ID=$(taskctl submit --cpu 8 --memory 16384 /bin/true)
rc=$?
case "$rc" in
  0) printf 'Accepted: %s\n' "$TASK_ID" ;;
  3) printf 'Capacity admission rejected; inspect the diagnostic.\n' >&2 ;;
  *) printf 'Submission not confirmed; inspect the error before resubmitting.\n' >&2 ;;
esac
```

Only a received capacity rejection proves no Task was created. Loss of a submit
response can still mean a committed Task exists; do not automatically resubmit.
retryable=false prohibits automatic immediate retry, not an operator retry after
the cluster/specification has changed.

## Wait

```bash
taskctl wait TASK_ID
taskctl wait TASK_ID --timeout 60
taskctl --json wait TASK_ID
```

Immediately query GET_TASK and then wait one second after each nonterminal reply.
Reuse one connection with one outstanding request. No watch/subscription, extra
thread, automatic reconnect or task-output download is introduced.

Wait for Task SUCCEEDED, FAILED or CANCELLED; an old terminal Attempt on a retried
PENDING Task does not end observation. Normal output prints one final summary,
including latest_recorded_attempt_id and available outcome details, clearly
distinguishing historical records from a newly executed Attempt. JSON prints one
final complete GET_TASK_RESPONSE. EXITED/exit_code alone is not the wait decision.

| Exit | Meaning |
| ---: | --- |
| 0 | Observed SUCCEEDED |
| 1 | Observed FAILED or CANCELLED, including TIMED_OUT/LOST Attempts |
| 2 | Wait arguments, server error, transport/protocol/identity error |
| 124 | Optional whole wait deadline reached |
| 130 | Normal shell status when interrupted by SIGINT |

Server errors retain their JSON form under --json. Local error/timeout diagnostics
go to stderr without a fabricated terminal object. The workload's exit code is
in the result and is not copied to the command exit status.

--timeout is a positive integer in seconds, not the submit execution timeout.
Omission means no overall deadline. The monotonic budget includes connection,
queries and sleep; each call also retains the existing five-second network
timeout. The earlier deadline wins, including on a late OS wakeup. Successful
responses must finish validation before the overall deadline. Scheduling and
polling granularity prevent hard real-time guarantees.

Timeout, communication failure and Ctrl-C only stop observation. They never
submit, cancel or retry a Task. A cancelled/LOST terminal snapshot can have
incomplete logs; wait reports the fact without waiting for impossible completeness.
Another client's FAILED-to-PENDING retry between polls may hide that earlier
terminal observation. Polling is not an event history.

## Implementation and validation

TaskManager exposes a read-only CapacityAdmission query for the Controller
client boundary. Internal decision tests can still model already accepted Tasks;
network clients cannot bypass admission. Registry invariant/owner failures remain
internal failures rather than being reported as resource insufficiency.

Client connect/request accept an optional borrowed Deadline pointer, default
nullptr. The caller owns its monotonic at value and expired flag for the duration
of each synchronous call; only actual overall-budget expiry sets the flag.
No old call site must change. Failures remain sticky on an unusable Client.

New CTest cases use admission_wait. Run full regression and:

```bash
ctest --test-dir BUILD -L admission_wait --output-on-failure
ctest --test-dir BUILD -L admission_wait --output-on-failure --repeat until-fail:20
```

The isolated local demo now uses specification-valid blockers occupying each
Worker's slots to demonstrate PENDING cancellation; it cleans only managed work.
Existing fault tests use explicit accepted OFFLINE advertisements when they need
PENDING without a live executor, preserving their original durability assertions.
