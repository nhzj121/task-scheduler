#include "ts/controller/worker_registry.h"

#include <stdexcept>

namespace ts::controller {
WorkerRegistry::WorkerRegistry(std::uint64_t timeout, std::size_t maximum_workers)
    : owner_(std::this_thread::get_id()), timeout_(timeout), maximum_workers_(maximum_workers) {
    if (timeout == 0 || maximum_workers == 0) throw std::invalid_argument("invalid WorkerRegistry limits");
}
SessionKey WorkerRegistry::activate(const Registration& registration, WorkerRecord& record, std::uint64_t now) {
    SessionKey key{registration.worker_id, registration.session_id, next_generation_++, registration.connection};
    record.current = WorkerSession{key, registration.capacity, registration.maximum, now + timeout_, 0};
    record.capacity = registration.capacity; record.maximum = registration.maximum;
    record.state = domain::WorkerState::HEALTHY; record.invalidating = false; record.socket_closed = false;
    return key;
}
common::Result<RegistrationDecision> WorkerRegistry::register_worker(Registration registration, std::uint64_t now) {
    using R = common::Result<RegistrationDecision>;
    if (owner_ != std::this_thread::get_id() || registration.connection.value == 0 ||
        registration.connection.generation == 0 || (registration.maximum && *registration.maximum == 0) ||
        now > UINT64_MAX - timeout_ || next_generation_ == UINT64_MAX || seen_sessions_.count(registration.session_id))
        return R::failure("invalid/stale registration identity, owner or deadline");
    auto it = workers_.find(registration.worker_id);
    if (it == workers_.end() && workers_.size() == maximum_workers_) return R::failure("Worker registry capacity reached");
    if (it != workers_.end() && it->second.replacement) return R::failure("one replacement already pending");
    for (const auto& item : workers_) {
        const auto& r = item.second;
        if ((r.current && r.current->key.connection == registration.connection) ||
            (r.replacement && r.replacement->connection == registration.connection))
            return R::failure("connection already belongs to another registration");
    }
    if (it == workers_.end()) it = workers_.emplace(registration.worker_id, WorkerRecord{}).first;
    auto& record = it->second;
    seen_sessions_.insert(registration.session_id);
    if (!record.current) return R::success({activate(registration, record, now), std::nullopt});
    record.replacement = std::move(registration);
    record.invalidating = true;
    return R::success({std::nullopt, record.current->key});
}
common::Result<void> WorkerRegistry::invalidate(const SessionKey& key, bool socket_closed) {
    if (owner_ != std::this_thread::get_id()) return common::Result<void>::failure("wrong registry owner");
    auto it = workers_.find(key.worker_id);
    if (it == workers_.end() || !it->second.current || !(it->second.current->key == key))
        return common::Result<void>::failure("stale session invalidation");
    it->second.invalidating = true; it->second.socket_closed = it->second.socket_closed || socket_closed;
    return common::Result<void>::success();
}
common::Result<std::optional<SessionKey>> WorkerRegistry::finish_invalidation(const SessionKey& key, std::uint64_t now) {
    using R = common::Result<std::optional<SessionKey>>;
    auto it = workers_.find(key.worker_id);
    if (owner_ != std::this_thread::get_id() || it == workers_.end() || !it->second.current ||
        !(it->second.current->key == key) || !it->second.invalidating ||
        now > UINT64_MAX - timeout_ || next_generation_ == UINT64_MAX)
        return R::failure("stale invalidation completion or invalid deadline");
    auto& record = it->second;
    record.current.reset(); record.state = domain::WorkerState::OFFLINE; record.invalidating = false;
    std::optional<SessionKey> next;
    if (record.replacement) {
        next = activate(*record.replacement, record, now); record.replacement.reset();
    }
    return R::success(std::move(next));
}
bool WorkerRegistry::valid(const SessionKey& key, std::uint64_t now) const {
    if (owner_ != std::this_thread::get_id()) return false;
    const auto it = workers_.find(key.worker_id);
    return it != workers_.end() && it->second.state == domain::WorkerState::HEALTHY &&
           !it->second.invalidating && it->second.current && it->second.current->key == key &&
           now < it->second.current->heartbeat_deadline;
}
common::Result<void> WorkerRegistry::heartbeat(const SessionKey& key, std::uint64_t sequence,
                                              std::uint64_t now, common::Timestamp observed_at) {
    if (!valid(key, now) || now > UINT64_MAX - timeout_)
        return common::Result<void>::failure("heartbeat does not identify a live current session");
    auto& r = workers_.at(key.worker_id);
    if (sequence == 0 || sequence <= r.current->last_sequence ||
        now + timeout_ < r.current->heartbeat_deadline)
        return common::Result<void>::failure("non-advancing heartbeat sequence or monotonic time");
    r.current->last_sequence = sequence; r.current->heartbeat_deadline = now + timeout_;
    r.last_heartbeat_at = observed_at;
    return common::Result<void>::success();
}
std::optional<WorkerRecord> WorkerRegistry::inspect(const domain::WorkerId& worker) const {
    if (owner_ != std::this_thread::get_id()) return std::nullopt;
    const auto it = workers_.find(worker);
    return it == workers_.end() ? std::nullopt : std::optional<WorkerRecord>(it->second);
}
std::vector<SessionKey> WorkerRegistry::expired(std::uint64_t now) const {
    std::vector<SessionKey> result;
    if (owner_ != std::this_thread::get_id()) return result;
    for (const auto& item : workers_) {
        const auto& r = item.second;
        if (r.current && !r.invalidating && now >= r.current->heartbeat_deadline) result.push_back(r.current->key);
    }
    return result;
}
std::vector<SessionKey> WorkerRegistry::invalidating() const {
    std::vector<SessionKey> result;
    if (owner_ != std::this_thread::get_id()) return result;
    for (const auto& item : workers_) if (item.second.current && item.second.invalidating) result.push_back(item.second.current->key);
    return result;
}
std::vector<std::pair<domain::WorkerId, WorkerRecord>> WorkerRegistry::records() const {
    if (owner_ != std::this_thread::get_id()) return {};
    return {workers_.begin(), workers_.end()};
}
} // namespace ts::controller
