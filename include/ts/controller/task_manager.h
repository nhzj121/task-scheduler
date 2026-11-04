#pragma once

#include <deque>
#include <functional>
#include <variant>

#include "ts/controller/worker_registry.h"
#include "ts/storage/persistence_thread.h"

namespace ts::controller {

struct ClientRequest { ConnectionIdentity connection; std::uint64_t request_id{0}; };
enum class EffectKind {
    SubmitAccepted, RunEligible, CancelEligible, CancelAccepted, CancelCompleted,
    RetryAccepted, TerminalVisible, WorkerOffline, CloseConnection, RegistrationAccepted,
    Rejected, FailStop
};
struct Effect {
    EffectKind kind;
    std::optional<domain::TaskId> task_id;
    std::optional<domain::AttemptId> attempt_id;
    std::optional<SessionKey> session;
    std::optional<ClientRequest> client;
    std::uint64_t task_version{0}, attempt_version{0}, operation_id{0};
    // Durable response facts captured when emitted, never read from a later row.
    std::optional<std::string> cancel_requested_at{};
    std::optional<std::uint32_t> next_attempt_no{};
};
enum class EffectAdmission { Accepted, ClientGone, Full };
class PersistencePort {
public:
    virtual ~PersistencePort() = default;
    virtual storage::Admission try_post(storage::Envelope envelope) = 0;
};
class EffectPort {
public:
    virtual ~EffectPort() = default;
    // Nonblocking, non-reentrant. M7 executes eligible actions in order. Before
    // queuing a delayed RUN/CANCEL it MUST revalidate effect_is_current(); a RUN
    // crosses its irreversible boundary through record_dispatch() before bytes
    // enter the output path. ClientGone discards only a response, not a Task.
    virtual EffectAdmission emit(Effect effect) = 0;
};
enum class Decision { Accepted, Deferred, WaitingForLogs, Backpressure, Rejected, Stale, Degraded, WrongOwner };
enum class DispatchFact { NoBytesQueued, BytesMayHaveEscaped, Uncertain };
enum class ObservedOutcome { Exited, Signaled, TimedOut, Cancelled, LaunchFailed };
struct CancelEvent { ClientRequest client; };
struct RetryEvent { ClientRequest client; };
struct AckEvent { domain::AttemptId attempt_id; SessionKey session; bool accepted; };
struct StartedEvent { domain::AttemptId attempt_id; SessionKey session; };
// Receipt only: never authorizes terminalization or releases a reservation.
struct CancelAckEvent { domain::AttemptId attempt_id; SessionKey session; };
struct FinishedEvent {
    domain::AttemptId attempt_id;
    SessionKey session;
    ObservedOutcome outcome;
    std::optional<std::int32_t> exit_code, term_signal;
    std::optional<std::string> reason;
    std::string stdout_path, stderr_path;
    std::uint64_t stdout_bytes{0}, stderr_bytes{0}, stdout_sequence{0}, stderr_sequence{0};
    // No evidence means retain this observation, but do not issue Terminalize.
    // A later identical observation supplies M8's actual, Attempt-bound evidence.
    storage::LogEvidence evidence;
    bool log_persistence_failed{false};
};
using TaskEvent = std::variant<CancelEvent, RetryEvent, AckEvent, StartedEvent, CancelAckEvent, FinishedEvent>;
struct DecisionLimits {
    std::size_t pending_count{64}, pending_bytes{4U * 1024U * 1024U};
    std::size_t essential_count{4}, essential_bytes{65536};
    std::size_t events_per_task{32}, event_count{1024}, event_bytes{4U * 1024U * 1024U};
    std::size_t events_per_turn{64};
    std::uint64_t heartbeat_interval_ms{3000}, offline_timeout_ms{10000};
    std::uint64_t ack_timeout_ms{5000}, start_timeout_ms{10000};
    std::string log_prefix{"tasks"};
};
struct RetainedMetadata { std::size_t pending_count{0}, pending_bytes{0}, event_count{0}, event_bytes{0}; };
enum class CapacityAdmissionKind { Allowed, NoKnownWorkers, NoWorkerFits };
struct CapacityAdmission {
    CapacityAdmissionKind kind;
    std::size_t known_worker_count;
};

enum class TaskLifecycleKind { Accepted, Assigned, Running, Terminal };
struct AttemptLifecycleSnapshot {
    std::string attempt_id, worker_id, session_id;
    std::uint32_t attempt_no;
    domain::AttemptState state;
    std::uint64_t version;
    bool logs_complete;
};
struct TaskLifecycleEvent {
    TaskLifecycleKind kind;
    std::string task_id;
    domain::TaskState state;
    std::uint64_t version;
    std::uint32_t cpu_slots;
    std::uint64_t memory_mb;
    std::optional<AttemptLifecycleSnapshot> attempt;
    bool reservation_released;
};
// Owner-thread, non-reentrant, best-effort diagnostics. Retain copies, never
// references; no scheduling, persistence or transport decisions belong here.
using TaskLifecycleObserver = std::function<void(const TaskLifecycleEvent&)>;

// Real single-owner decision logic, with no EventLoop/SQLite/socket/runtime calls.
// Ports outlive this nonmovable owner and never reenter it synchronously.
class TaskManager final {
public:
    TaskManager(PersistencePort& persistence, EffectPort& effects, DecisionLimits limits = {},
                TaskLifecycleObserver observer = {});
    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    // Only recovered PENDING/terminal snapshots; never restore old live sessions.
    [[nodiscard]] Decision restore(std::vector<storage::TaskRow> tasks, std::vector<storage::AttemptRow> attempts);
    // Producers supply a Controller-generated canonical Task UUID, not a wire ID.
    [[nodiscard]] Decision submit(storage::TaskSpec task, ClientRequest client);
    [[nodiscard]] Decision receive(const domain::TaskId& task, TaskEvent event);
    [[nodiscard]] Decision complete(const storage::Completion& completion);
    [[nodiscard]] Decision register_worker(Registration registration);
    [[nodiscard]] Decision heartbeat(const SessionKey& session, std::uint64_t sequence);
    [[nodiscard]] Decision invalidate(const SessionKey& session, bool socket_closed = false);
    [[nodiscard]] Decision connection_closed(ConnectionIdentity connection);
    [[nodiscard]] Decision tick(std::uint64_t monotonic_ms, common::Timestamp wall_time);
    [[nodiscard]] Decision record_dispatch(const Effect& run, DispatchFact fact);
    [[nodiscard]] bool effect_is_current(const Effect& effect) const;
    // One explicit pass / bounded event batch. Completion never recursively runs
    // another scheduling pass; triggers coalesce into schedule_requested_.
    [[nodiscard]] Decision schedule();
    [[nodiscard]] Decision pump();
    [[nodiscard]] std::optional<storage::TaskRow> task(const domain::TaskId& id) const;
    [[nodiscard]] std::vector<storage::AttemptRow> attempts(const domain::TaskId& id) const;
    [[nodiscard]] bool finalization_pending(const domain::TaskId& id) const;
    [[nodiscard]] RetainedMetadata retained() const;
    // Read-only client admission policy, not scheduling/remaining-resource logic.
    [[nodiscard]] common::Result<CapacityAdmission> capacity_admission(const domain::ResourceRequest& request) const;
    [[nodiscard]] bool degraded() const noexcept { return failure_.has_value(); }
    [[nodiscard]] const std::optional<storage::Error>& failure() const noexcept { return failure_; }
    [[nodiscard]] const ResourceLedger& resources() const noexcept { return ledger_; }
    [[nodiscard]] const WorkerRegistry& workers() const noexcept { return workers_; }
private:
    struct Pending {
        storage::Envelope envelope;
        std::optional<ClientRequest> client;
        std::optional<SessionKey> session;
    };
    struct Entry {
        storage::TaskRow row;
        std::uint64_t generation;
        bool durable{false};
        std::map<domain::AttemptId, storage::AttemptRow> history;
        std::optional<Pending> pending;
        std::optional<ReservationId> reservation;
        std::optional<FinishedEvent> finalization;
        std::deque<TaskEvent> events;
        DispatchFact dispatch{DispatchFact::NoBytesQueued};
        bool run_offered{false};
        bool loss_observation_pending{false}; // diagnostics only, never business state
        std::optional<std::uint64_t> ack_deadline, start_deadline;
    };
    struct Call;
    [[nodiscard]] Decision issue(Entry& entry, storage::Command command, std::optional<ClientRequest> client = std::nullopt);
    [[nodiscard]] Decision process(Entry& entry, const TaskEvent& event);
    [[nodiscard]] Decision enqueue(Entry& entry, TaskEvent event);
    [[nodiscard]] Decision invalidate_impl(const SessionKey& session, bool closed);
    void progress();
    void losses();
    void apply(Entry& entry, const Pending& pending);
    void apply_loss(const Pending& pending);
    void observe(TaskLifecycleKind kind, Entry& entry, bool with_attempt = false,
                 bool reservation_released = false) noexcept;
    void offer_run(Entry& entry);
    void emit(EffectKind kind, const Entry* entry = nullptr, std::optional<ClientRequest> client = std::nullopt,
              std::optional<SessionKey> session = std::nullopt, std::uint64_t operation = 0);
    void fail(storage::Error error);
    [[nodiscard]] storage::AttemptRow* current(Entry& entry);
    [[nodiscard]] bool bound(const Entry& entry, const SessionKey& session) const;
    [[nodiscard]] bool room(const storage::Envelope& envelope) const;
    [[nodiscard]] Decision run_pass();
    PersistencePort& persistence_;
    EffectPort& effects_;
    DecisionLimits limits_;
    TaskLifecycleObserver observer_;
    WorkerRegistry workers_;
    ResourceLedger ledger_;
    std::thread::id owner_;
    bool inside_{false}, schedule_requested_{false};
    std::uint64_t next_operation_{1}, next_entity_{1}, now_{0};
    common::Timestamp wall_{common::Timestamp::from_unix_microseconds(0)};
    std::map<domain::TaskId, Entry> tasks_;
    std::map<domain::SessionId, Pending> losses_;
    std::optional<storage::Error> failure_;
};
} // namespace ts::controller
