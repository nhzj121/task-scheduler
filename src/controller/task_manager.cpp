#include "ts/controller/task_manager.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace ts::controller {
namespace {
using TS = domain::TaskState;
using AS = domain::AttemptState;
constexpr auto integer_max = static_cast<std::uint64_t>(INT64_MAX);
storage::Error internal(std::string message) {
    return {storage::ErrorCode::Internal, 0, true, std::move(message)};
}
bool text(const std::string& s) { return !s.empty() && s.find('\0') == std::string::npos; }
bool valid_spec(const storage::TaskSpec& s) {
    if (!domain::TaskId::parse(s.task_id) || !text(s.executable) || s.cpu_slots == 0 ||
        s.memory_mb > integer_max || (s.timeout_ms && (*s.timeout_ms == 0 || *s.timeout_ms > integer_max)) ||
        (s.cwd && !text(*s.cwd))) return false;
    for (const auto& a : s.args) if (a.find('\0') != std::string::npos) return false;
    for (const auto& e : s.env) if (!text(e.first) || e.first.find('=') != std::string::npos ||
                                   e.second.find('\0') != std::string::npos) return false;
    return true;
}
std::optional<ClientRequest> client_of(const TaskEvent& e) {
    if (const auto* c = std::get_if<CancelEvent>(&e)) return c->client;
    if (const auto* r = std::get_if<RetryEvent>(&e)) return r->client;
    return std::nullopt;
}
std::size_t event_bytes(const TaskEvent& event) {
    std::size_t n = sizeof(TaskEvent);
    auto add = [&](std::size_t size) { n = size > SIZE_MAX - n ? SIZE_MAX : n + size; };
    std::visit([&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (!std::is_same_v<T, CancelEvent> && !std::is_same_v<T, RetryEvent>) {
            add(e.attempt_id.value().capacity()); add(e.session.worker_id.value().capacity());
            add(e.session.session_id.value().capacity());
        }
        if constexpr (std::is_same_v<T, FinishedEvent>) {
            add(e.stdout_path.capacity()); add(e.stderr_path.capacity()); add(e.evidence.attempt_id.capacity());
            if (e.reason) add(e.reason->capacity());
        }
    }, event);
    return n;
}
AS target(const FinishedEvent& f) {
    switch (f.outcome) {
        case ObservedOutcome::Exited: return f.exit_code == 0 ? AS::SUCCEEDED : AS::FAILED;
        case ObservedOutcome::TimedOut: return AS::TIMED_OUT;
        case ObservedOutcome::Cancelled: return AS::CANCELLED;
        case ObservedOutcome::Signaled:
        case ObservedOutcome::LaunchFailed: return AS::FAILED;
    }
    return AS::FAILED;
}
bool same_observation(const FinishedEvent& a, const FinishedEvent& b) {
    return a.attempt_id == b.attempt_id && a.session == b.session && a.outcome == b.outcome &&
        a.exit_code == b.exit_code && a.term_signal == b.term_signal && a.reason == b.reason &&
        a.stdout_path == b.stdout_path && a.stderr_path == b.stderr_path &&
        a.stdout_bytes == b.stdout_bytes && a.stderr_bytes == b.stderr_bytes &&
        a.stdout_sequence == b.stdout_sequence && a.stderr_sequence == b.stderr_sequence;
}
TS task_target(AS state) {
    return state == AS::SUCCEEDED ? TS::SUCCEEDED : state == AS::CANCELLED ? TS::CANCELLED : TS::FAILED;
}
} // namespace

struct TaskManager::Call {
    TaskManager& manager;
    bool entered;
    explicit Call(TaskManager& m) : manager(m), entered(m.owner_ == std::this_thread::get_id() && !m.inside_) {
        if (entered) m.inside_ = true;
    }
    ~Call() { if (entered) manager.inside_ = false; }
};
TaskManager::TaskManager(PersistencePort& persistence, EffectPort& effects, DecisionLimits limits,
                         TaskLifecycleObserver observer)
    : persistence_(persistence), effects_(effects), limits_(std::move(limits)),
      observer_(std::move(observer)),
      workers_(limits_.offline_timeout_ms), owner_(std::this_thread::get_id()) {
    if (!limits_.pending_count || !limits_.essential_count || limits_.essential_count >= limits_.pending_count ||
        !limits_.pending_bytes || !limits_.essential_bytes || limits_.essential_bytes >= limits_.pending_bytes ||
        !limits_.events_per_task || !limits_.event_count || !limits_.event_bytes || !limits_.events_per_turn ||
        !limits_.heartbeat_interval_ms || limits_.offline_timeout_ms <= limits_.heartbeat_interval_ms ||
        !limits_.ack_timeout_ms || !limits_.start_timeout_ms || !text(limits_.log_prefix))
        throw std::invalid_argument("invalid decision metadata/liveness limits");
}
void TaskManager::fail(storage::Error error) {
    if (failure_) return;
    failure_ = std::move(error);
    // failure() remains authoritative even when the effect port is itself full.
    try { static_cast<void>(effects_.emit({EffectKind::FailStop, {}, {}, {}, {}, 0, 0, 0})); }
    catch (...) { /* Already fail-stopped: no further business effects are permitted. */ }
}
void TaskManager::emit(EffectKind kind, const Entry* entry, std::optional<ClientRequest> client,
                       std::optional<SessionKey> session, std::uint64_t operation) {
    if (failure_) return;
    Effect e{kind, {}, {}, std::move(session), std::move(client), 0, 0, operation};
    if (entry) {
        e.task_id = domain::TaskId::parse(entry->row.spec.task_id).value();
        e.task_version = entry->row.state_version;
        e.cancel_requested_at = entry->row.cancel_requested_at;
        if (kind == EffectKind::RetryAccepted) {
            std::uint32_t maximum = 0;
            for (const auto& old : entry->history) maximum = std::max(maximum, old.second.attempt_no);
            e.next_attempt_no = maximum + 1;
        }
        if (entry->row.latest_attempt_id) {
            e.attempt_id = domain::AttemptId::parse(*entry->row.latest_attempt_id).value();
            e.attempt_version = entry->history.at(*e.attempt_id).state_version;
        }
    }
    try {
        const auto admission = effects_.emit(std::move(e));
        if (admission == EffectAdmission::Full || (admission == EffectAdmission::ClientGone && !client))
            fail(internal("effect port cannot admit required action"));
    } catch (...) { fail(internal("effect port threw")); }
}
storage::AttemptRow* TaskManager::current(Entry& entry) {
    if (!entry.row.latest_attempt_id) return nullptr;
    const auto id = domain::AttemptId::parse(*entry.row.latest_attempt_id);
    if (!id) return nullptr;
    const auto it = entry.history.find(id.value());
    return it == entry.history.end() ? nullptr : &it->second;
}
bool TaskManager::bound(const Entry& entry, const SessionKey& session) const {
    return entry.reservation && entry.reservation->session == session;
}
RetainedMetadata TaskManager::retained() const {
    RetainedMetadata r;
    for (const auto& pair : tasks_) {
        const auto& e = pair.second;
        if (e.pending) { ++r.pending_count; r.pending_bytes += storage::owned_bytes(e.pending->envelope); }
        for (const auto& event : e.events) { ++r.event_count; r.event_bytes += event_bytes(event); }
        if (e.finalization) { ++r.event_count; r.event_bytes += event_bytes(TaskEvent{*e.finalization}); }
    }
    for (const auto& loss : losses_) { ++r.pending_count; r.pending_bytes += storage::owned_bytes(loss.second.envelope); }
    return r;
}
bool TaskManager::room(const storage::Envelope& envelope) const {
    const auto r = retained();
    const bool essential = storage::is_essential(envelope.command);
    const auto count = limits_.pending_count - (essential ? 0 : limits_.essential_count);
    const auto bytes = limits_.pending_bytes - (essential ? 0 : limits_.essential_bytes);
    const auto n = storage::owned_bytes(envelope);
    return r.pending_count < count && n <= bytes && r.pending_bytes <= bytes - n;
}
Decision TaskManager::issue(Entry& entry, storage::Command command, std::optional<ClientRequest> client) {
    if (entry.pending) return Decision::Deferred;
    if (next_operation_ == UINT64_MAX) { fail(internal("operation identity exhausted")); return Decision::Degraded; }
    std::optional<SessionKey> session;
    if (entry.reservation) session = entry.reservation->session;
    storage::Envelope envelope{storage::correlate(command, next_operation_++, entry.generation,
                                                session ? session->generation : 0), std::move(command), 512};
    const bool essential = storage::is_essential(envelope.command);
    auto admission = storage::Admission::Full;
    if (room(envelope)) {
        try { admission = persistence_.try_post(envelope); }
        catch (...) { fail(internal("persistence port threw; admission uncertain")); return Decision::Degraded; }
    }
    if (admission == storage::Admission::Accepted) {
        entry.pending = Pending{std::move(envelope), std::move(client), std::move(session)};
        return Decision::Accepted;
    }
    if (essential || (admission != storage::Admission::Full && admission != storage::Admission::NotReady)) {
        fail({storage::ErrorCode::Capacity, 0, true, "required persistence admission failed"});
        return Decision::Degraded;
    }
    return Decision::Backpressure;
}
Decision TaskManager::submit(storage::TaskSpec spec, ClientRequest client) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    if (!valid_spec(spec) || !client.connection.value || !client.connection.generation || !client.request_id)
        return Decision::Rejected;
    const auto id = domain::TaskId::parse(spec.task_id).value();
    if (tasks_.count(id)) return Decision::Rejected;
    if (next_entity_ == UINT64_MAX) { fail(internal("entity generation exhausted")); return Decision::Degraded; }
    Entry entry{}; entry.row.spec = std::move(spec); entry.generation = next_entity_++;
    const auto result = issue(entry, storage::Submit{entry.row.spec, wall_.to_string()}, client);
    if (result == Decision::Accepted) tasks_.emplace(id, std::move(entry));
    return result;
}
Decision TaskManager::restore(std::vector<storage::TaskRow> rows, std::vector<storage::AttemptRow> attempts) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    if (!tasks_.empty() || !ledger_.snapshots().value().empty()) return Decision::Rejected;
    std::map<domain::TaskId, Entry> restored;
    for (auto& row : rows) {
        const auto id = domain::TaskId::parse(row.spec.task_id);
        if (!id || !valid_spec(row.spec) || row.state == TS::ACTIVE || !common::Timestamp::parse(row.created_at) ||
            !common::Timestamp::parse(row.updated_at) || row.state_version >= integer_max || restored.count(id.value()))
            return Decision::Rejected;
        Entry e{}; e.row = std::move(row); e.generation = next_entity_++; e.durable = true;
        restored.emplace(id.value(), std::move(e));
    }
    for (auto& a : attempts) {
        const auto t = domain::TaskId::parse(a.task_id); const auto id = domain::AttemptId::parse(a.attempt_id);
        if (!t || !id || !restored.count(t.value()) || !domain::is_terminal(a.state) || !a.attempt_no ||
            a.state_version >= integer_max || restored.at(t.value()).history.count(id.value())) return Decision::Rejected;
        auto& history = restored.at(t.value()).history;
        for (const auto& previous : history) if (previous.second.attempt_no == a.attempt_no) return Decision::Rejected;
        history.emplace(id.value(), std::move(a));
    }
    for (const auto& item : restored) {
        const auto& e = item.second;
        if (e.row.latest_attempt_id) {
            const auto id = domain::AttemptId::parse(*e.row.latest_attempt_id);
            if (!id || !e.history.count(id.value())) return Decision::Rejected;
        } else if (!e.history.empty()) return Decision::Rejected;
    }
    tasks_ = std::move(restored); schedule_requested_ = true;
    return Decision::Accepted;
}
Decision TaskManager::enqueue(Entry& entry, TaskEvent event) {
    const auto r = retained(); const auto bytes = event_bytes(event);
    if (entry.events.size() + (entry.finalization ? 1U : 0U) >= limits_.events_per_task ||
        r.event_count >= limits_.event_count || bytes > limits_.event_bytes || r.event_bytes > limits_.event_bytes - bytes) {
        if (!client_of(event)) { fail(internal("required deferred event exceeds bounded metadata capacity")); return Decision::Degraded; }
        return Decision::Backpressure;
    }
    entry.events.push_back(std::move(event)); return Decision::Deferred;
}
Decision TaskManager::receive(const domain::TaskId& id, TaskEvent event) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    const auto it = tasks_.find(id); if (it == tasks_.end()) return Decision::Rejected;
    auto& e = it->second;
    // Reject stale Worker facts before allowing them to consume deferred space.
    bool stale = false;
    std::visit([&](const auto& ev) {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (!std::is_same_v<T, CancelEvent> && !std::is_same_v<T, RetryEvent>) {
            stale = !bound(e, ev.session) || !workers_.valid(ev.session, now_) ||
                    ((e.pending && std::holds_alternative<storage::Assign>(e.pending->envelope.command))
                         ? e.pending->envelope.correlation.attempt_id != ev.attempt_id.value()
                         : (!e.row.latest_attempt_id || *e.row.latest_attempt_id != ev.attempt_id.value()));
        }
    }, event);
    if (stale) return Decision::Stale;
    if (std::holds_alternative<AckEvent>(event)) e.ack_deadline.reset();
    if (std::holds_alternative<StartedEvent>(event) && current(e) && current(e)->state == AS::STARTING)
        e.start_deadline.reset();
    if (e.pending || !e.events.empty() || (e.reservation && !workers_.valid(e.reservation->session, now_)))
        return enqueue(e, std::move(event));
    const auto result = process(e, event);
    if (result == Decision::Backpressure && !client_of(event)) return enqueue(e, std::move(event));
    return result;
}
Decision TaskManager::process(Entry& e, const TaskEvent& event) {
    if (!e.durable) return Decision::Rejected;
    auto* a = current(e);
    const storage::TaskVersion tv{e.row.spec.task_id, e.row.state_version};
    storage::AttemptVersion av{tv, a ? a->attempt_id : "", a ? a->state_version : 0, a ? a->session_id : ""};
    if (e.row.state_version >= integer_max || (a && a->state_version >= integer_max)) {
        fail(internal("state version exhausted")); return Decision::Degraded;
    }
    return std::visit([&](const auto& ev) -> Decision {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, RetryEvent>) {
            if (e.row.state != TS::FAILED) return Decision::Rejected;
            for (const auto& old : e.history)
                if (old.second.attempt_no == UINT32_MAX) return Decision::Rejected;
            return issue(e, storage::Retry{tv, wall_.to_string()}, ev.client);
        } else if constexpr (std::is_same_v<T, CancelEvent>) {
            if (e.row.state == TS::PENDING) return issue(e, storage::CancelPending{tv, wall_.to_string()}, ev.client);
            if (e.row.state != TS::ACTIVE || !a) return Decision::Rejected;
            if (e.row.cancel_requested_at) { emit(EffectKind::CancelAccepted, &e, ev.client); return Decision::Accepted; }
            if (a->state == AS::ASSIGNED && e.dispatch == DispatchFact::NoBytesQueued) {
                storage::Terminalize cmd{}; cmd.expected = av; cmd.to = AS::CANCELLED; cmd.at = wall_.to_string();
                cmd.stdout_path = a->stdout_path; cmd.stderr_path = a->stderr_path;
                cmd.stdout_final_sequence = 0; cmd.stderr_final_sequence = 0;
                cmd.logs.kind = storage::LogEvidenceKind::ProvenEmpty; cmd.logs.no_dispatch_bytes = true;
                return issue(e, std::move(cmd), ev.client);
            }
            return issue(e, storage::CancelIntent{tv, wall_.to_string()}, ev.client);
        } else {
            if (!a || e.row.state != TS::ACTIVE || a->attempt_id != ev.attempt_id.value() ||
                !bound(e, ev.session) || !workers_.valid(ev.session, now_)) return Decision::Stale;
            if constexpr (std::is_same_v<T, AckEvent>) {
                if (a->state != AS::ASSIGNED) return Decision::Stale;
                if (ev.accepted) {
                    const auto result = issue(e, storage::Advance{av, AS::STARTING, wall_.to_string()});
                    if (result == Decision::Accepted || result == Decision::Backpressure) e.ack_deadline.reset();
                    return result;
                }
                storage::Terminalize cmd{}; cmd.expected = av; cmd.to = AS::FAILED;
                cmd.reason = "DISPATCH_REJECTED"; cmd.at = wall_.to_string();
                cmd.stdout_path = a->stdout_path; cmd.stderr_path = a->stderr_path;
                cmd.stdout_final_sequence = 0; cmd.stderr_final_sequence = 0;
                cmd.logs.kind = storage::LogEvidenceKind::ProvenEmpty; cmd.logs.explicit_dispatch_rejection = true;
                return issue(e, std::move(cmd));
            } else if constexpr (std::is_same_v<T, StartedEvent>) {
                if (a->state != AS::STARTING) return Decision::Stale;
                const auto result = issue(e, storage::Advance{av, AS::RUNNING, wall_.to_string()});
                if (result == Decision::Accepted || result == Decision::Backpressure) e.start_deadline.reset();
                return result;
            } else if constexpr (std::is_same_v<T, CancelAckEvent>) {
                return e.row.cancel_requested_at ? Decision::Accepted : Decision::Rejected;
            } else {
                if (ev.stdout_path != a->stdout_path || ev.stderr_path != a->stderr_path ||
                    ev.stdout_bytes > integer_max || ev.stderr_bytes > integer_max ||
                    ev.stdout_sequence > integer_max || ev.stderr_sequence > integer_max ||
                    (ev.exit_code && (*ev.exit_code < 0 || *ev.exit_code > 255)) ||
                    (ev.term_signal && (*ev.term_signal <= 0 || *ev.term_signal > 64)) ||
                    (ev.exit_code && ev.term_signal) ||
                    (ev.outcome == ObservedOutcome::Exited && !ev.exit_code) ||
                    (ev.outcome == ObservedOutcome::Signaled && !ev.term_signal) ||
                    (e.finalization && !same_observation(*e.finalization, ev))) return Decision::Rejected;
                const auto to = ev.log_persistence_failed ? AS::FAILED : target(ev);
                if (a->state == AS::ASSIGNED || !domain::can_transition(a->state, to)) return Decision::Rejected;
                if (!ev.log_persistence_failed && ev.evidence.kind == storage::LogEvidenceKind::Incomplete) {
                    if (!e.finalization) {
                        const auto admitted = enqueue(e, event);
                        if (admitted != Decision::Deferred) return admitted;
                        e.finalization = std::get<FinishedEvent>(std::move(e.events.back())); e.events.pop_back();
                    }
                    e.ack_deadline.reset(); e.start_deadline.reset();
                    return Decision::WaitingForLogs;
                }
                const auto& proof = ev.evidence;
                if (!ev.log_persistence_failed && (proof.kind != storage::LogEvidenceKind::FinalBarrier ||
                    proof.attempt_id != a->attempt_id || !proof.prior_writes_completed || !proof.final_counters_verified ||
                    !proof.stdout_synced || !proof.stderr_synced || !proof.namespace_synced || proof.namespace_failed))
                    return Decision::Rejected;
                storage::Terminalize cmd{}; cmd.expected = av; cmd.to = to; cmd.at = wall_.to_string();
                cmd.exit_code = ev.exit_code; cmd.term_signal = ev.term_signal; cmd.reason = ev.reason;
                cmd.stdout_path = ev.stdout_path; cmd.stderr_path = ev.stderr_path;
                cmd.stdout_bytes = ev.stdout_bytes; cmd.stderr_bytes = ev.stderr_bytes;
                cmd.stdout_final_sequence = ev.stdout_sequence; cmd.stderr_final_sequence = ev.stderr_sequence;
                cmd.logs = proof;
                if (ev.log_persistence_failed) {
                    cmd.logs = {}; cmd.reason = "LOG_PERSISTENCE_ERROR";
                    nlohmann::json facts{{"outcome", static_cast<int>(ev.outcome)}};
                    if (ev.exit_code) facts["exit_code"] = *ev.exit_code;
                    if (ev.term_signal) facts["term_signal"] = *ev.term_signal;
                    if (ev.reason) facts["reason"] = *ev.reason;
                    cmd.observed_outcome_json = facts.dump();
                }
                e.ack_deadline.reset(); e.start_deadline.reset();
                return issue(e, std::move(cmd));
            }
        }
    }, event);
}

Decision TaskManager::complete(const storage::Completion& completion) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    Entry* entry = nullptr; Pending* pending = nullptr;
    for (auto& item : tasks_) if (item.second.pending && item.second.pending->envelope.correlation == completion.correlation) {
        entry = &item.second; pending = &*entry->pending; break;
    }
    if (!pending) for (auto& item : losses_) if (item.second.envelope.correlation == completion.correlation) {
        pending = &item.second; break;
    }
    if (!pending) return Decision::Stale;
    const auto& o = completion.outcome;
    if (o.error) {
        // Rollback is not release: a definitely failed Assign never acquired a
        // durable Attempt. UnknownCommit (or a contradictory committed error)
        // must instead retain tentative ownership for application fail-stop.
        if (entry && std::holds_alternative<storage::Assign>(pending->envelope.command) &&
            entry->reservation && !o.committed && o.error->code != storage::ErrorCode::UnknownCommit) {
            if (ledger_.rollback(*entry->reservation) != LedgerStatus::Ok) {
                fail(internal("assignment rollback invariant")); return Decision::Degraded;
            }
            entry->reservation.reset(); schedule_requested_ = true;
        }
        if (o.committed || o.error->fatal || o.error->code == storage::ErrorCode::UnknownCommit ||
            storage::is_essential(pending->envelope.command) ||
            (o.error->code != storage::ErrorCode::Conflict && o.error->code != storage::ErrorCode::Invalid &&
             o.error->code != storage::ErrorCode::Constraint)) {
            // Uncertain commits retain ownership, including tentative reservations.
            fail(*o.error); return Decision::Degraded;
        }
        if (entry) {
            const auto client = pending->client;
            const auto session = pending->session;
            const bool reconcile = std::holds_alternative<storage::Advance>(pending->envelope.command) ||
                                   std::holds_alternative<storage::CancelIntent>(pending->envelope.command);
            entry->pending.reset();
            if (client) emit(EffectKind::Rejected, entry, client);
            // A failed active CAS cannot authorize a command or silently leave
            // an Attempt stuck with no ACK/start deadline. Reconcile by the
            // existing conservative session-loss transaction, never by a jump
            // in memory or by rolling back committed reservations.
            if (reconcile && session && !failure_) static_cast<void>(invalidate_impl(*session, false));
        }
        progress(); losses(); return failure_ ? Decision::Degraded : Decision::Rejected;
    }
    std::size_t expected = 1;
    if (!entry) {
        expected = 0;
        for (const auto& item : tasks_) if (pending->session && bound(item.second, *pending->session) &&
                                          item.second.row.state == TS::ACTIVE) ++expected;
    }
    // Reads/notifications and malformed successful payloads never prove a mutation.
    if (!o.committed || o.affected != expected || !o.tasks.empty() || !o.attempts.empty() || o.has_more || o.next)
        return Decision::Stale;
    Pending done = *pending;
    if (entry) {
        const auto& c = done.envelope.correlation;
        if (c.entity_generation != entry->generation || c.expected_task_version != entry->row.state_version ||
            (done.session && (!entry->reservation || !(entry->reservation->session == *done.session))))
            return Decision::Stale;
        const auto* a = current(*entry);
        if (!std::holds_alternative<storage::Assign>(done.envelope.command) && !c.attempt_id.empty() &&
            (!a || c.attempt_id != a->attempt_id || c.expected_attempt_version != a->state_version))
            return Decision::Stale;
        entry->pending.reset(); apply(*entry, done);
    } else {
        losses_.erase(done.session->session_id); apply_loss(done);
    }
    if (!failure_) { progress(); losses(); }
    if (!failure_) for (auto& item : tasks_) offer_run(item.second);
    return failure_ ? Decision::Degraded : Decision::Accepted;
}
void TaskManager::observe(TaskLifecycleKind kind, Entry& e, bool with_attempt, bool released) noexcept {
    if (!observer_ || failure_) return;
    try {
        TaskLifecycleEvent event{kind, e.row.spec.task_id, e.row.state, e.row.state_version,
                                 e.row.spec.cpu_slots, e.row.spec.memory_mb, {}, released};
        if (with_attempt) {
            const auto* a = current(e);
            if (!a) return;
            event.attempt = AttemptLifecycleSnapshot{a->attempt_id, a->worker_id, a->session_id,
                a->attempt_no, a->state, a->state_version, a->logs_complete};
        }
        observer_(event);
    } catch (...) { /* Diagnostics never authorize or prevent business effects. */ }
}
void TaskManager::apply(Entry& e, const Pending& pending) {
    const auto op = pending.envelope.correlation.operation_id;
    std::visit([&](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, storage::Submit>) {
            e.durable = true; e.row.created_at = c.at; e.row.updated_at = c.at;
            observe(TaskLifecycleKind::Accepted, e);
            schedule_requested_ = true; emit(EffectKind::SubmitAccepted, &e, pending.client, {}, op);
        } else if constexpr (std::is_same_v<T, storage::Assign>) {
            storage::AttemptRow a; a.task_id = c.expected.task_id; a.attempt_id = c.attempt_id;
            for (const auto& h : e.history) a.attempt_no = std::max(a.attempt_no, h.second.attempt_no + 1);
            a.worker_id = c.worker_id; a.session_id = c.session_id; a.assigned_at = c.at;
            a.stdout_path = c.stdout_path; a.stderr_path = c.stderr_path;
            e.history.emplace(domain::AttemptId::parse(a.attempt_id).value(), std::move(a));
            e.row.state = TS::ACTIVE; ++e.row.state_version; e.row.updated_at = c.at; e.row.latest_attempt_id = c.attempt_id;
            e.run_offered = false; e.dispatch = DispatchFact::NoBytesQueued;
            if (!e.reservation || ledger_.commit(*e.reservation) != LedgerStatus::Ok) fail(internal("assignment commit invariant"));
            observe(TaskLifecycleKind::Assigned, e, true);
        } else if constexpr (std::is_same_v<T, storage::Advance>) {
            auto* a = current(e); a->state = c.to; ++a->state_version;
            if (c.to == AS::STARTING) {
                a->acknowledged_at = c.at; e.ack_deadline.reset();
                if (now_ > UINT64_MAX - limits_.start_timeout_ms) { fail(internal("start deadline overflow")); return; }
                e.start_deadline = now_ + limits_.start_timeout_ms;
            } else {
                a->started_at = c.at; e.start_deadline.reset();
                observe(TaskLifecycleKind::Running, e, true);
            }
        } else if constexpr (std::is_same_v<T, storage::CancelIntent>) {
            e.row.cancel_requested_at = c.at; ++e.row.state_version; e.row.updated_at = c.at;
            emit(EffectKind::CancelAccepted, &e, pending.client, {}, op);
            if (e.reservation && workers_.valid(e.reservation->session, now_))
                emit(EffectKind::CancelEligible, &e, {}, e.reservation->session, op);
        } else if constexpr (std::is_same_v<T, storage::CancelPending>) {
            e.row.state = TS::CANCELLED; ++e.row.state_version; e.row.updated_at = c.at;
            observe(TaskLifecycleKind::Terminal, e);
            emit(EffectKind::CancelCompleted, &e, pending.client, {}, op);
        } else if constexpr (std::is_same_v<T, storage::Retry>) {
            e.row.state = TS::PENDING; ++e.row.state_version; e.row.updated_at = c.at; e.row.cancel_requested_at.reset();
            schedule_requested_ = true; emit(EffectKind::RetryAccepted, &e, pending.client, {}, op);
        } else if constexpr (std::is_same_v<T, storage::Terminalize>) {
            auto* a = current(e); a->state = c.to; ++a->state_version; a->finished_at = c.at;
            a->exit_code = c.exit_code; a->term_signal = c.term_signal; a->reason = c.reason;
            a->observed_outcome_json = c.observed_outcome_json;
            a->stdout_bytes = c.stdout_bytes; a->stderr_bytes = c.stderr_bytes;
            a->stdout_final_sequence = c.stdout_final_sequence; a->stderr_final_sequence = c.stderr_final_sequence;
            a->logs_complete = c.logs.kind != storage::LogEvidenceKind::Incomplete;
            e.row.state = task_target(c.to); ++e.row.state_version; e.row.updated_at = c.at;
            e.finalization.reset(); e.ack_deadline.reset(); e.start_deadline.reset();
            if (!e.reservation || ledger_.release(*e.reservation) != LedgerStatus::Ok) {
                fail(internal("terminal release invariant")); return;
            }
            e.reservation.reset(); schedule_requested_ = true;
            observe(TaskLifecycleKind::Terminal, e, true, true);
            emit(EffectKind::TerminalVisible, &e, {}, pending.session, op);
            if (pending.client) emit(EffectKind::CancelCompleted, &e, pending.client, {}, op);
        } else {
            fail(internal("unsupported Task mutation completion"));
        }
    }, pending.envelope.command);
}
void TaskManager::offer_run(Entry& e) {
    auto* a = current(e);
    if (failure_ || !a || a->state != AS::ASSIGNED || e.pending || !e.events.empty() || e.finalization ||
        e.row.cancel_requested_at || e.run_offered || !e.reservation || !workers_.valid(e.reservation->session, now_)) return;
    e.run_offered = true;
    // Once eligibility leaves this owner it cannot infer whether an adapter has
    // queued bytes. Only a fresh explicit NoBytesQueued fact proves local cancel.
    e.dispatch = DispatchFact::Uncertain;
    emit(EffectKind::RunEligible, &e, {}, e.reservation->session, e.reservation->operation_id);
}
bool TaskManager::effect_is_current(const Effect& effect) const {
    if (owner_ != std::this_thread::get_id() || failure_ || !effect.task_id || !effect.attempt_id || !effect.session)
        return false;
    const auto it = tasks_.find(*effect.task_id); if (it == tasks_.end()) return false;
    const auto& e = it->second;
    if (e.row.state != TS::ACTIVE || !e.row.latest_attempt_id || *e.row.latest_attempt_id != effect.attempt_id->value() ||
        !bound(e, *effect.session) || !workers_.valid(*effect.session, now_)) return false;
    const auto& a = e.history.at(*effect.attempt_id);
    if (effect.kind == EffectKind::RunEligible) {
        if (e.pending || !e.events.empty() || e.row.cancel_requested_at || e.finalization || a.state != AS::ASSIGNED ||
            e.row.state_version != effect.task_version || a.state_version != effect.attempt_version ||
            !e.reservation || e.reservation->operation_id != effect.operation_id) return false;
        return true;
    }
    if (effect.kind == EffectKind::CancelEligible)
        return e.row.cancel_requested_at.has_value() && !domain::is_terminal(a.state) && !e.finalization &&
               (!e.pending || !storage::is_essential(e.pending->envelope.command));
    return false;
}
Decision TaskManager::record_dispatch(const Effect& effect, DispatchFact fact) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    if (effect.kind != EffectKind::RunEligible || !effect_is_current(effect)) return Decision::Stale;
    auto& e = tasks_.at(*effect.task_id);
    if (e.dispatch == DispatchFact::BytesMayHaveEscaped && fact == DispatchFact::NoBytesQueued) return Decision::Rejected;
    e.dispatch = fact;
    if (fact == DispatchFact::Uncertain) return invalidate_impl(*effect.session, false);
    if (fact == DispatchFact::NoBytesQueued) e.ack_deadline.reset();
    if (fact != DispatchFact::NoBytesQueued) {
        if (now_ > UINT64_MAX - limits_.ack_timeout_ms) { fail(internal("ACK deadline overflow")); return Decision::Degraded; }
        if (!e.ack_deadline) e.ack_deadline = now_ + limits_.ack_timeout_ms;
    }
    return Decision::Accepted;
}
void TaskManager::progress() {
    std::size_t remaining = limits_.events_per_turn;
    for (auto& item : tasks_) {
        auto& e = item.second;
        while (!failure_ && remaining && !e.pending && !e.events.empty() &&
               (!e.reservation || workers_.valid(e.reservation->session, now_))) {
            TaskEvent event = std::move(e.events.front()); e.events.pop_front(); --remaining;
            const auto result = process(e, event);
            if (result == Decision::Backpressure) { e.events.push_front(std::move(event)); break; }
            if ((result == Decision::Rejected || result == Decision::Stale) && client_of(event))
                emit(EffectKind::Rejected, &e, client_of(event));
        }
        if (!remaining || failure_) break;
    }
}
Decision TaskManager::run_pass() {
    std::vector<scheduler::PendingTask> pending;
    for (const auto& item : tasks_) {
        const auto& e = item.second;
        if (e.durable && e.row.state == TS::PENDING && !e.pending && e.events.empty())
            pending.push_back({item.first, common::Timestamp::parse(e.row.created_at).value(),
                              domain::ResourceRequest::create(e.row.spec.cpu_slots, e.row.spec.memory_mb).value()});
    }
    schedule_requested_ = false;
    for (const auto& candidate : scheduler::pending_order(std::move(pending))) {
        auto snapshots = ledger_.snapshots();
        if (!snapshots) { fail(internal(snapshots.error().message)); return Decision::Degraded; }
        for (auto& worker : snapshots.value()) {
            const auto record = workers_.inspect(worker.worker_id);
            worker.current_session = record && record->current && workers_.valid(record->current->key, now_);
        }
        const auto selected = scheduler::select(candidate.request, snapshots.value());
        if (!selected) { fail(internal(selected.error().message)); return Decision::Degraded; }
        if (!selected.value()) continue;
        auto& e = tasks_.at(candidate.task_id);
        std::uint32_t attempt_no = 1;
        for (const auto& previous : e.history) {
            if (previous.second.attempt_no == UINT32_MAX) { fail(internal("Attempt number exhausted")); return Decision::Degraded; }
            attempt_no = std::max(attempt_no, previous.second.attempt_no + 1);
        }
        const auto key = workers_.inspect(*selected.value())->current->key;
        ReservationId reservation{domain::AttemptId::generate(), next_operation_, key};
        if (ledger_.reserve(reservation, candidate.request) != LedgerStatus::Ok) {
            fail(internal("scheduler/ledger feasibility disagreement")); return Decision::Degraded;
        }
        e.reservation = reservation;
        const auto base = limits_.log_prefix + "/" + candidate.task_id.value() + "/attempt-" + std::to_string(attempt_no);
        storage::Assign cmd{{candidate.task_id.value(), e.row.state_version}, reservation.attempt_id.value(),
                            key.worker_id.value(), key.session_id.value(), base + ".stdout.log", base + ".stderr.log", wall_.to_string()};
        const auto result = issue(e, std::move(cmd));
        if (result != Decision::Accepted) {
            if (!failure_) {
                if (ledger_.rollback(reservation) != LedgerStatus::Ok) fail(internal("unadmitted assignment rollback"));
                e.reservation.reset(); schedule_requested_ = true;
            }
            return failure_ ? Decision::Degraded : result;
        }
    }
    return Decision::Accepted;
}
Decision TaskManager::schedule() {
    Call call(*this); if (!call.entered) { if (owner_ == std::this_thread::get_id()) schedule_requested_ = true; return Decision::WrongOwner; }
    if (failure_) return Decision::Degraded;
    progress(); losses();
    return failure_ ? Decision::Degraded : run_pass();
}
Decision TaskManager::pump() {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    progress(); losses();
    if (failure_) return Decision::Degraded;
    for (auto& item : tasks_) offer_run(item.second);
    return schedule_requested_ ? run_pass() : Decision::Accepted;
}

Decision TaskManager::register_worker(Registration registration) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    const auto decision = workers_.register_worker(std::move(registration), now_);
    if (!decision) return Decision::Rejected;
    if (decision.value().accepted) {
        const auto key = *decision.value().accepted; const auto record = workers_.inspect(key.worker_id);
        if (ledger_.install(key, record->current->capacity, record->current->maximum) != LedgerStatus::Ok) {
            fail(internal("new session resource account invariant")); return Decision::Degraded;
        }
        emit(EffectKind::RegistrationAccepted, nullptr, {}, key); schedule_requested_ = true;
    }
    if (decision.value().invalidate) {
        if (ledger_.disable(*decision.value().invalidate) != LedgerStatus::Ok) {
            fail(internal("replacement account invalidation invariant")); return Decision::Degraded;
        }
        losses();
    }
    return failure_ ? Decision::Degraded : Decision::Accepted;
}
Decision TaskManager::heartbeat(const SessionKey& session, std::uint64_t sequence) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    return workers_.heartbeat(session, sequence, now_, wall_) ? Decision::Accepted : Decision::Stale;
}
Decision TaskManager::invalidate_impl(const SessionKey& session, bool closed) {
    if (!workers_.invalidate(session, closed)) return Decision::Stale;
    if (ledger_.disable(session) != LedgerStatus::Ok) { fail(internal("invalidation ledger invariant")); return Decision::Degraded; }
    losses(); return failure_ ? Decision::Degraded : Decision::Accepted;
}
Decision TaskManager::invalidate(const SessionKey& session, bool closed) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    return invalidate_impl(session, closed);
}
Decision TaskManager::connection_closed(ConnectionIdentity connection) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    if (workers_.discard_replacement(connection)) return Decision::Accepted;
    for (const auto& item : workers_.records()) if (item.second.current && item.second.current->key.connection == connection)
        return invalidate_impl(item.second.current->key, true);
    return Decision::Stale;
}
void TaskManager::losses() {
    for (const auto& key : workers_.invalidating()) {
        if (failure_ || losses_.count(key.session_id)) continue;
        bool busy = false;
        // Bulk LOST must be AFTER every old-session command, including an Assign
        // that may already have committed but whose completion has not arrived.
        // No new old-session command can start once invalidating is set.
        for (const auto& pair : tasks_) if (pair.second.pending && pair.second.pending->session &&
                                           *pair.second.pending->session == key) { busy = true; break; }
        if (busy) continue;
        if (next_operation_ == UINT64_MAX) { fail(internal("operation identity exhausted")); return; }
        storage::Command command = storage::LoseSession{key.session_id.value(), wall_.to_string(), "SESSION_LOST"};
        storage::Envelope envelope{storage::correlate(command, next_operation_++, 0, key.generation), command, 512};
        auto admission = storage::Admission::Full;
        if (room(envelope)) {
            try { admission = persistence_.try_post(envelope); }
            catch (...) { fail(internal("session-loss admission uncertain")); return; }
        }
        if (admission != storage::Admission::Accepted) {
            fail({storage::ErrorCode::Capacity, 0, true, "essential session-loss admission unavailable"}); return;
        }
        losses_.emplace(key.session_id, Pending{std::move(envelope), {}, key});
    }
}
void TaskManager::apply_loss(const Pending& pending) {
    const auto& key = *pending.session;
    const auto& cmd = std::get<storage::LoseSession>(pending.envelope.command);
    for (auto& item : tasks_) {
        auto& e = item.second;
        if (!bound(e, key) || e.row.state != TS::ACTIVE) continue;
        auto* a = current(e);
        if (!a || domain::is_terminal(a->state) || e.pending) { fail(internal("bulk loss serialization invariant")); return; }
        a->state = AS::LOST; ++a->state_version; a->finished_at = cmd.at; a->reason = cmd.reason; a->logs_complete = false;
        e.row.state = TS::FAILED; ++e.row.state_version; e.row.updated_at = cmd.at;
        e.loss_observation_pending = true;
        e.finalization.reset(); e.ack_deadline.reset(); e.start_deadline.reset();
    }
    if (ledger_.release_session(key) != LedgerStatus::Ok) { fail(internal("bulk durable release invariant")); return; }
    for (auto& item : tasks_) if (bound(item.second, key)) {
        item.second.reservation.reset();
        if (item.second.loss_observation_pending) {
            item.second.loss_observation_pending = false;
            observe(TaskLifecycleKind::Terminal, item.second, true, true);
        }
        emit(EffectKind::TerminalVisible, &item.second, {}, key, pending.envelope.correlation.operation_id);
    }
    emit(EffectKind::WorkerOffline, nullptr, {}, key, pending.envelope.correlation.operation_id);
    const auto previous = workers_.inspect(key.worker_id);
    if (!previous->socket_closed) emit(EffectKind::CloseConnection, nullptr, {}, key);
    if (ledger_.remove(key) != LedgerStatus::Ok) { fail(internal("old session account removal invariant")); return; }
    const auto next = workers_.finish_invalidation(key, now_);
    if (!next) { fail(internal(next.error().message)); return; }
    if (next.value()) {
        const auto record = workers_.inspect(key.worker_id);
        if (ledger_.install(*next.value(), record->current->capacity, record->current->maximum) != LedgerStatus::Ok) {
            fail(internal("replacement session install invariant")); return;
        }
        emit(EffectKind::RegistrationAccepted, nullptr, {}, *next.value());
    }
    schedule_requested_ = true;
}
Decision TaskManager::tick(std::uint64_t monotonic_ms, common::Timestamp wall_time) {
    Call call(*this); if (!call.entered) return Decision::WrongOwner;
    if (failure_) return Decision::Degraded;
    const auto maximum = std::max({limits_.ack_timeout_ms, limits_.start_timeout_ms, limits_.offline_timeout_ms});
    if (monotonic_ms < now_ || monotonic_ms > UINT64_MAX - maximum) return Decision::Rejected;
    now_ = monotonic_ms; wall_ = wall_time;
    for (const auto& key : workers_.expired(now_)) static_cast<void>(invalidate_impl(key, false));
    for (auto& item : tasks_) {
        auto& e = item.second;
        if (e.reservation && ((e.ack_deadline && now_ >= *e.ack_deadline) ||
                              (e.start_deadline && now_ >= *e.start_deadline)) &&
            workers_.valid(e.reservation->session, now_)) static_cast<void>(invalidate_impl(e.reservation->session, false));
        if (failure_) break;
    }
    return failure_ ? Decision::Degraded : Decision::Accepted;
}
std::optional<storage::TaskRow> TaskManager::task(const domain::TaskId& id) const {
    if (owner_ != std::this_thread::get_id()) return std::nullopt;
    const auto it = tasks_.find(id);
    return it == tasks_.end() || !it->second.durable ? std::nullopt : std::optional<storage::TaskRow>(it->second.row);
}
std::vector<storage::AttemptRow> TaskManager::attempts(const domain::TaskId& id) const {
    std::vector<storage::AttemptRow> result;
    if (owner_ != std::this_thread::get_id()) return result;
    const auto it = tasks_.find(id); if (it == tasks_.end()) return result;
    for (const auto& history : it->second.history) result.push_back(history.second);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.attempt_no < b.attempt_no; });
    return result;
}
bool TaskManager::finalization_pending(const domain::TaskId& id) const {
    if (owner_ != std::this_thread::get_id()) return false;
    const auto it = tasks_.find(id); return it != tasks_.end() && it->second.finalization.has_value();
}
} // namespace ts::controller
