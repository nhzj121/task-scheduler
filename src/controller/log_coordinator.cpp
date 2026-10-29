#include "ts/controller/log_coordinator.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace ts::controller {
namespace {
using R = common::Result<void>;
template<class T> T checked(common::Result<T> r) {
    if (!r) throw std::runtime_error(r.error().message);
    return std::move(r).value();
}
void checked(R r) { if (!r) throw std::runtime_error(r.error().message); }
std::uint64_t now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
logs::Binding binding(const storage::AttemptRow& row, std::uint64_t generation) {
    return {row.task_id, row.attempt_id, row.session_id, generation, row.attempt_no, row.stdout_path, row.stderr_path};
}
}
struct LogCoordinator::Impl {
    struct Entry {
        logs::Binding binding;
        SessionKey session;
        protocol::LogContext accepted{{}, {}, 0, 0};
        logs::Positions written;
        std::size_t writes{0};
        std::optional<FinishedEvent> finished;
        bool invalidated{false}, error{false}, barrier_queued{false}, barrier_ok{false}, delivered{false}, terminal{false}, forgetting{false};
    };
    struct Pending {
        logs::Kind kind;
        logs::Binding binding;
        logs::Stream stream;
        std::uint64_t sequence, bytes;
        std::optional<ClientRequest> client;
        std::optional<protocol::GetLogs> request;
        bool complete{false};
    };
    struct Session { SessionKey key; bool poisoned{false}; LogCredit replenish_due{}; };
    domain::QueueConfig config;
    TaskManager& manager;
    LogCreditLedger ledger;
    std::unique_ptr<logs::Writer> writer;
    std::map<domain::AttemptId, Entry> entries;
    std::map<std::uint64_t, Pending> pending;
    std::map<std::string, Session> sessions;
    std::uint64_t next{1}; std::size_t round{0};
    bool stopping{false};
    Impl(domain::QueueConfig c, TaskManager& core) : config(c), manager(core), ledger(c.log_bytes, c.log_commands - c.log_control_reserve) {}
    std::uint64_t operation() {
        if (next == UINT64_MAX) throw std::overflow_error("log operation identity exhausted");
        return next++;
    }
    bool live(const SessionKey& key) const { return !stopping && manager.workers().valid(key, now()); }
    void poison(const SessionKey& key) {
        const auto session = sessions.find(key.session_id.value());
        if (session != sessions.end()) session->second.poisoned = true;
    }
    logs::Admission post(logs::Command command, std::optional<ClientRequest> client = {},
                         std::optional<protocol::GetLogs> request = {}, bool complete = false) {
        const auto id = command.operation;
        pending.emplace(id, Pending{command.kind, command.binding, command.stream, command.sequence,
            command.data.size(), client, request, complete});
        const auto admitted = writer->try_post(std::move(command));
        if (admitted != logs::Admission::Accepted) pending.erase(id);
        return admitted;
    }
    void forget(Entry& entry) {
        if (entry.forgetting || entry.writes || entry.barrier_queued) return;
        logs::Command command; command.operation = operation(); command.kind = logs::Kind::Forget; command.binding = entry.binding;
        const auto admitted = post(std::move(command));
        if (admitted == logs::Admission::Accepted) entry.forgetting = true;
        else if (admitted != logs::Admission::Full) throw std::runtime_error("log owner cleanup admission failed");
    }
    void invalidate(SessionKey key) {
        if (manager.invalidate(key) == Decision::Degraded) throw std::runtime_error("log-failure session loss persistence unavailable");
        static_cast<void>(ledger.reclaim(key.session_id.value())); sessions.erase(key.session_id.value());
        for (auto& entry : entries) if (entry.second.session == key) entry.second.invalidated = true;
    }
};
LogCoordinator::LogCoordinator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LogCoordinator::~LogCoordinator() = default;
common::Result<std::unique_ptr<LogCoordinator>> LogCoordinator::create(std::string root, domain::QueueConfig config,
    TaskManager& manager, std::shared_ptr<logs::FilesystemHook> hook) {
    using Result = common::Result<std::unique_ptr<LogCoordinator>>;
    if (!config.log_control_reserve || config.log_commands <= config.log_control_reserve)
        return Result::failure("invalid essential log reserve");
    try {
        auto impl = std::make_unique<Impl>(config, manager);
        logs::Limits limits; limits.bytes = config.log_bytes; limits.write_slots = config.log_commands - config.log_control_reserve;
        limits.essential_slots = config.log_control_reserve;
        impl->writer = checked(logs::Writer::start(std::move(root), limits, std::move(hook)));
        return Result::success(std::unique_ptr<LogCoordinator>(new LogCoordinator(std::move(impl))));
    } catch (const std::exception& e) { return Result::failure(e.what()); }
}
R LogCoordinator::reserve(const domain::SessionId& session) {
    if (impl_->stopping) return R::failure("log admission stopped");
    return impl_->ledger.reserve(session.value(), {impl_->config.initial_session_log_window_bytes, impl_->config.initial_session_log_window_chunks});
}
R LogCoordinator::activate(const SessionKey& session) {
    auto& s = *impl_; auto result = s.ledger.activate(session.session_id.value(), session.generation);
    if (result) s.sessions.emplace(session.session_id.value(), Impl::Session{session, false});
    return result;
}
void LogCoordinator::reclaim(const domain::SessionId& session) {
    const auto id = session.value();
    auto& s = *impl_; static_cast<void>(s.ledger.reclaim(id)); s.sessions.erase(id);
    for (auto& entry : s.entries) if (entry.second.binding.session == id) entry.second.invalidated = true;
}
R LogCoordinator::bind(const storage::AttemptRow& row, const SessionKey& session) {
    auto& s = *impl_;
    if (s.stopping || s.entries.size() >= 4096 || s.entries.count(domain::AttemptId::parse(row.attempt_id).value()) ||
        row.session_id != session.session_id.value() || !logs::valid_binding(binding(row, session.generation))) return R::failure("invalid/full log Attempt ownership");
    s.entries.emplace(domain::AttemptId::parse(row.attempt_id).value(), Impl::Entry{binding(row, session.generation), session, {}, {}, 0, {}, false, false, false, false, false, false, false});
    return R::success();
}
LogAdmission LogCoordinator::append(const protocol::LogChunk& chunk) {
    auto& s = *impl_; const auto it = s.entries.find(chunk.attempt_id);
    if (it == s.entries.end() || it->second.finished || it->second.invalidated || !s.live(it->second.session) ||
        it->second.binding.session != chunk.session_id.value() || it->second.binding.task != chunk.task_id.value() ||
        !(it->second.session.worker_id == chunk.worker_id)) return LogAdmission::ProtocolViolation;
    auto& entry = it->second; auto context = entry.accepted;
    const auto credit = s.ledger.outstanding(chunk.session_id.value()); if (!credit) return LogAdmission::ProtocolViolation;
    context.credit_bytes = credit->bytes; context.credit_chunks = credit->slots;
    const auto accepted = protocol::check_log(chunk, context); if (!accepted) return LogAdmission::ProtocolViolation;
    auto raw = protocol::base64_decode(chunk.data_base64, 65536); if (!raw) return LogAdmission::ProtocolViolation;
    logs::Command command; command.operation = s.operation(); command.kind = logs::Kind::Append;
    command.binding = entry.binding; command.stream = chunk.stream == protocol::Stream::Stdout ? logs::Stream::Out : logs::Stream::Err;
    command.sequence = chunk.sequence; command.data = std::move(raw).value();
    if (!s.ledger.accept(command.operation, command.binding, command.data.size())) return LogAdmission::InvariantFailure;
    // No second ordinary-admission contest: all capacity came from granted credit.
    if (s.post(std::move(command)) != logs::Admission::Accepted) return LogAdmission::InvariantFailure;
    ++entry.writes; entry.accepted = accepted.value(); return LogAdmission::Accepted;
}
R LogCoordinator::finish(FinishedEvent observation) {
    auto& s = *impl_; const auto it = s.entries.find(observation.attempt_id);
    if (it == s.entries.end() || it->second.finished || it->second.invalidated || !(it->second.session == observation.session))
        return R::failure("stale/duplicate FINISH log binding");
    auto& entry = it->second;
    if (entry.accepted.out.bytes != observation.stdout_bytes || entry.accepted.err.bytes != observation.stderr_bytes ||
        entry.accepted.out.next_sequence != observation.stdout_sequence || entry.accepted.err.next_sequence != observation.stderr_sequence)
        return R::failure("FINISH differs from already accepted chunks");
    const auto task = domain::TaskId::parse(entry.binding.task).value();
    const auto result = s.manager.receive(task, observation);
    if (result != Decision::WaitingForLogs && result != Decision::Deferred) return R::failure("FINISH metadata admission unavailable");
    entry.finished = std::move(observation); return R::success();
}
R LogCoordinator::read(const storage::AttemptRow& row, protocol::GetLogs request, ClientRequest client) {
    auto& s = *impl_;
    logs::Command command; command.operation = s.operation(); command.kind = logs::Kind::Read;
    command.binding = binding(row, 0); const auto known = s.entries.find(domain::AttemptId::parse(row.attempt_id).value());
    if (known != s.entries.end()) command.binding = known->second.binding;
    command.stream = request.stream == protocol::Stream::Stdout ? logs::Stream::Out : logs::Stream::Err;
    command.offset = request.offset; command.limit = request.limit_bytes; command.snapshot_end = request.snapshot_end;
    command.historical_visible = row.logs_complete ? (command.stream == logs::Stream::Out ? row.stdout_bytes : row.stderr_bytes) : 0;
    command.proven_empty = row.logs_complete && row.stdout_bytes == 0 && row.stderr_bytes == 0 &&
        row.stdout_final_sequence == std::optional<std::uint64_t>(0) && row.stderr_final_sequence == std::optional<std::uint64_t>(0) &&
        ((row.reason && *row.reason == "DISPATCH_REJECTED") || (row.state == domain::AttemptState::CANCELLED && !row.acknowledged_at));
    command.allow_missing_empty = !row.logs_complete;
    const auto admitted = s.post(std::move(command), client, request, row.logs_complete);
    return admitted == logs::Admission::Accepted ? R::success() : R::failure("snapshot read capacity/unavailable");
}
common::Result<std::vector<LogReply>> LogCoordinator::drain() {
    using Result = common::Result<std::vector<LogReply>>;
    auto& s = *impl_; auto values = s.writer->drain(); if (!values) return Result::failure(values.error().message);
    std::vector<LogReply> replies;
    try {
        for (auto& result : values.value()) {
            const auto pending = s.pending.find(result.operation);
            if (pending == s.pending.end() || !(pending->second.binding == result.binding) || pending->second.kind != result.kind ||
                pending->second.sequence != result.sequence || pending->second.bytes != result.raw_size || pending->second.stream != result.stream)
                throw std::runtime_error("log completion identity/counters invariant");
            auto operation = std::move(pending->second); s.pending.erase(pending);
            if (result.kind == logs::Kind::Read) {
                // Only invalid snapshot bounds are a client argument error.
                // Unsafe/malformed filesystem entries are server-side read failures.
                protocol::Message message = protocol::ErrorResponse{result.error && result.error->code == logs::ErrorCode::Invalid &&
                    result.error->operation == logs::Operation::Read ?
                    protocol::ErrorCode::InvalidArgument : protocol::ErrorCode::Internal, "snapshot read failed", false, nlohmann::json::object()};
                if (!result.error) {
                    message = protocol::GetLogsResponse{domain::TaskId::parse(result.binding.task).value(),
                        domain::AttemptNumber::create(result.binding.number).value(), operation.request->stream, result.offset,
                        result.offset + result.data.size(), result.snapshot_end, protocol::base64_encode(result.data),
                        result.offset + result.data.size() == result.snapshot_end, operation.complete};
                }
                replies.push_back({*operation.client, std::move(message)}); continue;
            }
            const auto entry = s.entries.find(domain::AttemptId::parse(result.binding.attempt).value());
            if (entry == s.entries.end() || !(entry->second.binding == result.binding)) throw std::runtime_error("missing log command owner");
            auto& owned = entry->second;
            if (result.kind == logs::Kind::Append) {
                checked(s.ledger.complete(result.operation, result.binding, result.raw_size));
                if (!owned.writes) throw std::runtime_error("log inflight underflow");
                --owned.writes;
                if (!result.error) {
                    auto& position = result.stream == logs::Stream::Out ? owned.written.out : owned.written.err;
                    const auto visible = result.stream == logs::Stream::Out ? result.visible.out : result.visible.err;
                    if (position.sequence != result.sequence || visible.bytes != position.bytes + result.raw_size || visible.sequence != position.sequence + 1)
                        throw std::runtime_error("log visible progress invariant");
                    position = visible;
                    const auto session = s.sessions.find(result.binding.session);
                    if (session != s.sessions.end() && session->second.key.generation == result.binding.generation && !owned.invalidated) {
                        auto& due = session->second.replenish_due;
                        if (due.bytes > UINT64_MAX - result.raw_size || due.slots == UINT64_MAX) throw std::runtime_error("replenishment overflow");
                        due.bytes += result.raw_size; ++due.slots;
                    }
                }
            } else if (result.kind == logs::Kind::Barrier) {
                owned.barrier_queued = false;
                if (!result.error && !result.barrier_ok) throw std::runtime_error("missing final barrier evidence");
                if (!result.error && (!(result.visible.out == owned.written.out) || !(result.visible.err == owned.written.err)))
                    throw std::runtime_error("barrier written counters mismatch");
                owned.barrier_ok = !result.error && result.barrier_ok;
            } else {
                if (result.error) throw std::runtime_error("log owner cleanup failed");
                s.entries.erase(entry); continue;
            }
            if (result.error) {
                owned.error = true; s.poison(owned.session);
                if (!owned.finished && !owned.invalidated) s.invalidate(owned.session);
            }
        }
        return Result::success(std::move(replies));
    } catch (const std::exception& e) { return Result::failure(e.what()); }
}
R LogCoordinator::pump(const std::function<bool(const SessionKey&, LogCredit)>& send_grant) {
    auto& s = *impl_;
    try {
        if (s.writer->status().health == logs::Health::Failed) return R::failure("LogWriter internal/channel failure");
        for (auto& item : s.entries) {
            auto& entry = item.second;
            if (!s.live(entry.session)) entry.invalidated = true;
            if (entry.invalidated || entry.terminal) { s.forget(entry); continue; }
            if (!entry.finished || entry.delivered || entry.writes) continue;
            if (!entry.error && (entry.written.out.bytes != entry.finished->stdout_bytes || entry.written.err.bytes != entry.finished->stderr_bytes ||
                entry.written.out.sequence != entry.finished->stdout_sequence || entry.written.err.sequence != entry.finished->stderr_sequence))
                return R::failure("final accepted/written counters differ");
            if (entry.error || entry.barrier_ok) {
                auto finish = *entry.finished;
                if (entry.error) finish.log_persistence_failed = true;
                else finish.evidence = {storage::LogEvidenceKind::FinalBarrier, entry.binding.attempt,
                    true, true, true, true, true, false, false, false};
                const auto result = s.manager.receive(domain::TaskId::parse(entry.binding.task).value(), std::move(finish));
                if (result == Decision::Accepted || result == Decision::Deferred) entry.delivered = true;
                else if (result != Decision::Backpressure) return R::failure("final log metadata transition rejected");
            } else if (!entry.barrier_queued) {
                logs::Command command; command.operation = s.operation(); command.kind = logs::Kind::Barrier;
                command.binding = entry.binding; command.final = entry.written;
                const auto admitted = s.post(std::move(command));
                if (admitted == logs::Admission::Accepted) entry.barrier_queued = true;
                else if (admitted != logs::Admission::Full) return R::failure("essential log barrier admission unavailable");
            }
        }
        // Round-robin bounded initial-sized windows; reserve before announcing.
        std::vector<std::string> sessions;
        for (const auto& session : s.sessions) sessions.push_back(session.first);
        if (!sessions.empty()) {
            const auto start = s.round++ % sessions.size();
            for (std::size_t i = 0; i < sessions.size(); ++i) {
                const auto found = s.sessions.find(sessions[(start + i) % sessions.size()]);
                if (found == s.sessions.end()) continue;
                if (!s.live(found->second.key)) { reclaim(found->second.key.session_id); continue; }
                if (found->second.poisoned) continue;
                const auto key = found->second.key; const auto credit = s.ledger.outstanding(key.session_id.value());
                if (!credit) continue;
                const auto free = s.ledger.partition().free;
                const auto due = found->second.replenish_due;
                const LogCredit grant{std::min({free.bytes, s.config.initial_session_log_window_bytes - credit->bytes, due.bytes}),
                    std::min({free.slots, s.config.initial_session_log_window_chunks - credit->slots, due.slots})};
                if (grant.bytes || grant.slots) {
                    checked(s.ledger.grant(key.session_id.value(), key.generation, grant));
                    found->second.replenish_due.bytes -= grant.bytes; found->second.replenish_due.slots -= grant.slots;
                    if (!send_grant(key, grant)) reclaim(key.session_id);
                }
            }
        }
        return R::success();
    } catch (const std::exception& e) { return R::failure(e.what()); }
}
void LogCoordinator::terminal(const domain::AttemptId& attempt) {
    auto& s = *impl_; const auto it = s.entries.find(attempt); if (it == s.entries.end()) return;
    it->second.terminal = true;
    const auto session = s.sessions.find(it->second.binding.session);
    if (session != s.sessions.end() && session->second.poisoned) s.invalidate(session->second.key);
}
int LogCoordinator::fd() const noexcept { return impl_->writer->fd(); }
logs::Status LogCoordinator::status() const { return impl_->writer->status(); }
logs::Usage LogCoordinator::usage() const { return impl_->writer->usage(); }
void LogCoordinator::stop() { impl_->stopping = true; impl_->writer->stop(); }
void LogCoordinator::join() { impl_->writer->join(); }
} // namespace ts::controller
