#include "ts/controller/resource_ledger.h"

#include <limits>

namespace ts::controller {
namespace {
bool add(Counters& counters, const domain::ResourceRequest& request) {
    if (request.cpu_slots() > UINT32_MAX - counters.cpu || request.memory_mb() > UINT64_MAX - counters.memory ||
        counters.attempts == UINT32_MAX) return false;
    counters.cpu += request.cpu_slots(); counters.memory += request.memory_mb(); ++counters.attempts;
    return true;
}
bool same(const Counters& a, const Counters& b) {
    return a.cpu == b.cpu && a.memory == b.memory && a.attempts == b.attempts;
}
bool subtract(Counters& counters, const domain::ResourceRequest& request) {
    if (counters.cpu < request.cpu_slots() || counters.memory < request.memory_mb() || counters.attempts == 0) return false;
    counters.cpu -= request.cpu_slots(); counters.memory -= request.memory_mb(); --counters.attempts;
    return true;
}
} // namespace

LedgerStatus ResourceLedger::install(const SessionKey& key, domain::ResourceCapacity capacity,
                                      std::optional<std::uint32_t> maximum) {
    if (owner_ != std::this_thread::get_id()) return LedgerStatus::WrongThread;
    if (key.generation == 0 || key.connection.value == 0 || key.connection.generation == 0 ||
        (maximum && *maximum == 0)) return LedgerStatus::Invalid;
    if (accounts_.count(key.worker_id)) return LedgerStatus::Conflict;
    accounts_.emplace(key.worker_id, AccountSnapshot{key, capacity, maximum, {}, {}, true});
    return LedgerStatus::Ok;
}
LedgerStatus ResourceLedger::disable(const SessionKey& key) {
    if (owner_ != std::this_thread::get_id()) return LedgerStatus::WrongThread;
    auto it = accounts_.find(key.worker_id);
    if (it == accounts_.end() || !(it->second.session == key)) return LedgerStatus::Conflict;
    it->second.accepting = false; return LedgerStatus::Ok;
}
LedgerStatus ResourceLedger::remove(const SessionKey& key) {
    if (owner_ != std::this_thread::get_id()) return LedgerStatus::WrongThread;
    auto it = accounts_.find(key.worker_id);
    if (it == accounts_.end() || !(it->second.session == key) ||
        it->second.tentative.attempts != 0 || it->second.committed.attempts != 0) return LedgerStatus::Conflict;
    accounts_.erase(it); return LedgerStatus::Ok;
}
LedgerStatus ResourceLedger::reserve(const ReservationId& id, domain::ResourceRequest request) {
    if (owner_ != std::this_thread::get_id()) return LedgerStatus::WrongThread;
    auto it = accounts_.find(id.session.worker_id);
    if (id.operation_id == 0) return LedgerStatus::Invalid;
    if (it == accounts_.end() || !(it->second.session == id.session) || !it->second.accepting ||
        reservations_.count(id.attempt_id)) return LedgerStatus::Conflict;
    auto& a = it->second;
    if (a.tentative.cpu > UINT32_MAX - a.committed.cpu || a.tentative.memory > UINT64_MAX - a.committed.memory ||
        a.tentative.attempts > UINT32_MAX - a.committed.attempts) return LedgerStatus::Overflow;
    const auto usage = domain::ResourceUsage::from_values(a.tentative.cpu + a.committed.cpu,
                                                         a.tentative.memory + a.committed.memory);
    const auto count = a.tentative.attempts + a.committed.attempts;
    if (!domain::fits_within(a.capacity, usage, request) || count == UINT32_MAX ||
        (a.max_concurrent_tasks && count >= *a.max_concurrent_tasks)) return LedgerStatus::Capacity;
    auto next = a.tentative;
    if (!add(next, request)) return LedgerStatus::Overflow;
    reservations_.emplace(id.attempt_id, Reservation{id, request, Phase::Tentative});
    a.tentative = next; return LedgerStatus::Ok;
}
LedgerStatus ResourceLedger::change(const ReservationId& id, bool committing, bool releasing) {
    if (owner_ != std::this_thread::get_id()) return LedgerStatus::WrongThread;
    auto it = reservations_.find(id.attempt_id);
    if (it == reservations_.end() || !(it->second.id == id)) return LedgerStatus::Conflict;
    auto& r = it->second;
    const auto expected = releasing ? Phase::Committed : Phase::Tentative;
    if (r.phase != expected) return LedgerStatus::Conflict;
    auto a = accounts_.find(id.session.worker_id);
    if (a == accounts_.end() || !(a->second.session == id.session)) return LedgerStatus::Conflict;
    auto tentative = a->second.tentative; auto committed = a->second.committed;
    if (!subtract(releasing ? committed : tentative, r.request)) return LedgerStatus::Conflict;
    if (committing && !add(committed, r.request)) return LedgerStatus::Overflow;
    a->second.tentative = tentative; a->second.committed = committed;
    if (committing) r.phase = Phase::Committed;
    else reservations_.erase(it);
    return LedgerStatus::Ok;
}
LedgerStatus ResourceLedger::commit(const ReservationId& id) { return change(id, true, false); }
LedgerStatus ResourceLedger::rollback(const ReservationId& id) { return change(id, false, false); }
LedgerStatus ResourceLedger::release(const ReservationId& id) { return change(id, false, true); }
LedgerStatus ResourceLedger::release_session(const SessionKey& key) {
    if (owner_ != std::this_thread::get_id()) return LedgerStatus::WrongThread;
    auto a = accounts_.find(key.worker_id);
    if (a == accounts_.end() || !(a->second.session == key) || a->second.tentative.attempts != 0)
        return LedgerStatus::Conflict;
    for (auto it = reservations_.begin(); it != reservations_.end();) {
        if (it->second.id.session == key) it = reservations_.erase(it);
        else ++it;
    }
    a->second.committed = {}; return LedgerStatus::Ok;
}
common::Result<AccountSnapshot> ResourceLedger::inspect(const domain::WorkerId& worker) const {
    if (owner_ != std::this_thread::get_id()) return common::Result<AccountSnapshot>::failure("ledger wrong owner thread");
    const auto it = accounts_.find(worker);
    if (it == accounts_.end()) return common::Result<AccountSnapshot>::failure("unknown Worker");
    return common::Result<AccountSnapshot>::success(it->second);
}
common::Result<std::vector<scheduler::WorkerSnapshot>> ResourceLedger::snapshots() const {
    if (audit() != LedgerStatus::Ok) return common::Result<std::vector<scheduler::WorkerSnapshot>>::failure("ledger invariant failure");
    std::vector<scheduler::WorkerSnapshot> result;
    for (const auto& item : accounts_) {
        const auto& a = item.second;
        result.push_back({item.first, a.capacity,
            domain::ResourceUsage::from_values(a.tentative.cpu + a.committed.cpu, a.tentative.memory + a.committed.memory),
            a.tentative.attempts + a.committed.attempts, a.max_concurrent_tasks,
            domain::WorkerState::HEALTHY, a.accepting, !a.accepting});
    }
    return common::Result<std::vector<scheduler::WorkerSnapshot>>::success(std::move(result));
}
LedgerStatus ResourceLedger::audit() const {
    if (owner_ != std::this_thread::get_id()) return LedgerStatus::WrongThread;
    for (const auto& item : accounts_) {
        const auto& a = item.second; Counters tentative, committed;
        for (const auto& reservation : reservations_) {
            const auto& r = reservation.second;
            if (r.id.session.worker_id == item.first) {
                if (!(r.id.session == a.session) || !add(r.phase == Phase::Tentative ? tentative : committed, r.request))
                    return LedgerStatus::Conflict;
            }
        }
        if (!same(tentative, a.tentative) || !same(committed, a.committed) ||
            tentative.cpu > a.capacity.cpu_slots() || committed.cpu > a.capacity.cpu_slots() - tentative.cpu ||
            tentative.memory > a.capacity.memory_mb() || committed.memory > a.capacity.memory_mb() - tentative.memory ||
            tentative.attempts > UINT32_MAX - committed.attempts ||
            (a.max_concurrent_tasks && tentative.attempts + committed.attempts > *a.max_concurrent_tasks))
            return LedgerStatus::Conflict;
    }
    for (const auto& reservation : reservations_) {
        if (!accounts_.count(reservation.second.id.session.worker_id)) return LedgerStatus::Conflict;
    }
    return LedgerStatus::Ok;
}
} // namespace ts::controller
