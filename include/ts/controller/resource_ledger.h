#pragma once

#include <map>
#include <thread>

#include "ts/scheduler/scheduler.h"

namespace ts::controller {

struct ConnectionIdentity {
    std::uint64_t value, generation;
    friend bool operator==(ConnectionIdentity a, ConnectionIdentity b) noexcept {
        return a.value == b.value && a.generation == b.generation;
    }
};
struct SessionKey {
    domain::WorkerId worker_id;
    domain::SessionId session_id;
    std::uint64_t generation;
    ConnectionIdentity connection;
    friend bool operator==(const SessionKey& a, const SessionKey& b) noexcept {
        return a.worker_id == b.worker_id && a.session_id == b.session_id &&
               a.generation == b.generation && a.connection == b.connection;
    }
};
struct ReservationId {
    domain::AttemptId attempt_id;
    std::uint64_t operation_id;
    SessionKey session;
    friend bool operator==(const ReservationId& a, const ReservationId& b) noexcept {
        return a.attempt_id == b.attempt_id && a.operation_id == b.operation_id && a.session == b.session;
    }
};
enum class LedgerStatus { Ok, Invalid, Conflict, Capacity, Overflow, WrongThread };
struct Counters { std::uint32_t cpu{0}; std::uint64_t memory{0}; std::uint32_t attempts{0}; };
struct AccountSnapshot {
    SessionKey session;
    domain::ResourceCapacity capacity;
    std::optional<std::uint32_t> max_concurrent_tasks;
    Counters tentative, committed;
    bool accepting;
};

class ResourceLedger final {
public:
    ResourceLedger() : owner_(std::this_thread::get_id()) {}
    ResourceLedger(const ResourceLedger&) = delete;
    ResourceLedger& operator=(const ResourceLedger&) = delete;
    [[nodiscard]] LedgerStatus install(const SessionKey& session, domain::ResourceCapacity capacity,
                                      std::optional<std::uint32_t> maximum);
    [[nodiscard]] LedgerStatus disable(const SessionKey& session);
    [[nodiscard]] LedgerStatus remove(const SessionKey& session);
    [[nodiscard]] LedgerStatus reserve(const ReservationId& id, domain::ResourceRequest request);
    // Repeated commit/rollback/release is Conflict, never silent idempotent success.
    [[nodiscard]] LedgerStatus commit(const ReservationId& id);
    [[nodiscard]] LedgerStatus rollback(const ReservationId& id);
    // Caller supplies the durable terminal/session-loss completion gate. This
    // memory-only primitive never manufactures a persistence authorization.
    [[nodiscard]] LedgerStatus release(const ReservationId& id);
    [[nodiscard]] LedgerStatus release_session(const SessionKey& session);
    [[nodiscard]] common::Result<AccountSnapshot> inspect(const domain::WorkerId& worker) const;
    [[nodiscard]] common::Result<std::vector<scheduler::WorkerSnapshot>> snapshots() const;
    [[nodiscard]] LedgerStatus audit() const;
    [[nodiscard]] std::size_t reservation_count() const noexcept { return reservations_.size(); }
private:
    enum class Phase { Tentative, Committed };
    struct Reservation { ReservationId id; domain::ResourceRequest request; Phase phase; };
    [[nodiscard]] LedgerStatus change(const ReservationId& id, bool committing, bool releasing);
    std::thread::id owner_;
    std::map<domain::WorkerId, AccountSnapshot> accounts_;
    std::map<domain::AttemptId, Reservation> reservations_;
};
} // namespace ts::controller
