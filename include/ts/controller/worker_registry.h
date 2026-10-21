#pragma once

#include <set>

#include "ts/controller/resource_ledger.h"

namespace ts::controller {

struct Registration {
    domain::WorkerId worker_id;
    domain::SessionId session_id;
    ConnectionIdentity connection;
    domain::ResourceCapacity capacity;
    std::optional<std::uint32_t> maximum;
};
struct WorkerSession {
    SessionKey key;
    domain::ResourceCapacity capacity;
    std::optional<std::uint32_t> maximum;
    std::uint64_t heartbeat_deadline, last_sequence{0};
};
struct WorkerRecord {
    domain::WorkerState state{domain::WorkerState::OFFLINE};
    std::optional<WorkerSession> current;
    std::optional<Registration> replacement;
    bool invalidating{false}, socket_closed{false};
    std::optional<common::Timestamp> last_heartbeat_at;
};
struct RegistrationDecision {
    std::optional<SessionKey> accepted;
    std::optional<SessionKey> invalidate;
};

// Data-only owner. TaskManager coordinates its decisions with the ledger and
// persistence port; it never closes a socket or encodes an ACK itself.
class WorkerRegistry final {
public:
    explicit WorkerRegistry(std::uint64_t offline_timeout_ms = 10000,
                            std::size_t maximum_workers = 1024);
    WorkerRegistry(const WorkerRegistry&) = delete;
    WorkerRegistry& operator=(const WorkerRegistry&) = delete;
    [[nodiscard]] common::Result<RegistrationDecision> register_worker(Registration registration, std::uint64_t now);
    [[nodiscard]] common::Result<void> invalidate(const SessionKey& session, bool socket_closed);
    // Caller has committed the whole old-session LOST transaction and released
    // its reservations. Only then may OFFLINE / new HEALTHY become observable.
    [[nodiscard]] common::Result<std::optional<SessionKey>> finish_invalidation(const SessionKey& session, std::uint64_t now);
    [[nodiscard]] common::Result<void> heartbeat(const SessionKey& session, std::uint64_t sequence,
                                                std::uint64_t now, common::Timestamp observed_at);
    [[nodiscard]] bool valid(const SessionKey& session, std::uint64_t now) const;
    [[nodiscard]] std::optional<WorkerRecord> inspect(const domain::WorkerId& worker) const;
    [[nodiscard]] std::vector<SessionKey> expired(std::uint64_t now) const;
    [[nodiscard]] std::vector<SessionKey> invalidating() const;
private:
    [[nodiscard]] SessionKey activate(const Registration& registration, WorkerRecord& record, std::uint64_t now);
    std::thread::id owner_;
    std::uint64_t timeout_, next_generation_{1};
    std::size_t maximum_workers_;
    std::map<domain::WorkerId, WorkerRecord> workers_;
    // Identity tombstones, not pending events or durable leases. They prohibit
    // accepting a previously used session UUID within this Controller lifetime.
    std::set<domain::SessionId> seen_sessions_;
};
} // namespace ts::controller
