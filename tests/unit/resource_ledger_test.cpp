#include "ts/controller/resource_ledger.h"

#include <random>
#include <iomanip>
#include <sstream>
#include <gtest/gtest.h>

namespace ts::controller {
namespace {
SessionKey key() {
    return {domain::WorkerId::parse("worker").value(),
            domain::SessionId::parse("00000000-0000-4000-8000-000000000001").value(), 1, {7, 1}};
}
ReservationId reservation(std::uint64_t operation = 1) {
    std::ostringstream id; id << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << operation;
    return {domain::AttemptId::parse(id.str()).value(), operation, key()};
}
TEST(ResourceLedger, TentativeCommitRollbackReleaseAndDuplicateGuards) {
    ResourceLedger ledger; const auto session = key(); const auto r = reservation();
    EXPECT_EQ(ledger.install(session, domain::ResourceCapacity::create(2, 10).value(), 1), LedgerStatus::Ok);
    EXPECT_EQ(ledger.reserve(r, domain::ResourceRequest::create(2, 10).value()), LedgerStatus::Ok);
    EXPECT_EQ(ledger.reserve(r, domain::ResourceRequest::create(1, 0).value()), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.reserve(reservation(2), domain::ResourceRequest::create(1, 0).value()), LedgerStatus::Capacity);
    EXPECT_EQ(ledger.release(r), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.commit(r), LedgerStatus::Ok); EXPECT_EQ(ledger.commit(r), LedgerStatus::Conflict);
    auto account = ledger.inspect(session.worker_id).value();
    EXPECT_EQ(account.tentative.cpu, 0U); EXPECT_EQ(account.committed.cpu, 2U);
    EXPECT_EQ(ledger.rollback(r), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.release(r), LedgerStatus::Ok); EXPECT_EQ(ledger.release(r), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.reserve(r, domain::ResourceRequest::create(1, 0).value()), LedgerStatus::Ok);
    EXPECT_EQ(ledger.rollback(r), LedgerStatus::Ok); EXPECT_EQ(ledger.rollback(r), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.audit(), LedgerStatus::Ok);
}
TEST(ResourceLedger, WrongOperationConnectionOrSessionCannotStealReservation) {
    ResourceLedger ledger; auto r = reservation();
    ASSERT_EQ(ledger.install(key(), domain::ResourceCapacity::create(2, 0).value(), std::nullopt), LedgerStatus::Ok);
    ASSERT_EQ(ledger.reserve(r, domain::ResourceRequest::create(1, 0).value()), LedgerStatus::Ok);
    auto wrong = r; ++wrong.operation_id; EXPECT_EQ(ledger.rollback(wrong), LedgerStatus::Conflict);
    wrong = r; ++wrong.session.generation; EXPECT_EQ(ledger.commit(wrong), LedgerStatus::Conflict);
    wrong = r; ++wrong.session.connection.generation; EXPECT_EQ(ledger.rollback(wrong), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.release_session(key()), LedgerStatus::Conflict); // no early release of tentative
    EXPECT_EQ(ledger.disable(key()), LedgerStatus::Ok);
    EXPECT_EQ(ledger.reserve(reservation(2), domain::ResourceRequest::create(1, 0).value()), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.commit(r), LedgerStatus::Ok); // invalidation must retain old owner until LOST
    EXPECT_EQ(ledger.remove(key()), LedgerStatus::Conflict);
    EXPECT_EQ(ledger.release_session(key()), LedgerStatus::Ok);
    EXPECT_EQ(ledger.remove(key()), LedgerStatus::Ok); EXPECT_EQ(ledger.audit(), LedgerStatus::Ok);
}
TEST(ResourceLedger, BoundaryArithmeticAndTentativePlacementVisibility) {
    ResourceLedger ledger; const auto session = key();
    ASSERT_EQ(ledger.install(session, domain::ResourceCapacity::create(UINT32_MAX, UINT64_MAX).value(), 2), LedgerStatus::Ok);
    const auto r = reservation();
    EXPECT_EQ(ledger.reserve(r, domain::ResourceRequest::create(UINT32_MAX, UINT64_MAX).value()), LedgerStatus::Ok);
    auto snapshots = ledger.snapshots(); ASSERT_TRUE(snapshots);
    auto decision = scheduler::select(domain::ResourceRequest::create(1, 0).value(), snapshots.value());
    ASSERT_TRUE(decision); EXPECT_FALSE(decision.value());
    EXPECT_EQ(ledger.reserve(reservation(2), domain::ResourceRequest::create(1, 1).value()), LedgerStatus::Capacity);
    EXPECT_EQ(ledger.rollback(r), LedgerStatus::Ok); EXPECT_EQ(ledger.audit(), LedgerStatus::Ok);
}
TEST(ResourceLedger, SeededReservationTracesConserveResourcesAtEveryStep) {
    ResourceLedger ledger;
    ASSERT_EQ(ledger.install(key(), domain::ResourceCapacity::create(16, 64).value(), 12), LedgerStatus::Ok);
    std::mt19937 random(602);
    std::vector<std::pair<ReservationId, bool>> live;
    for (std::uint64_t step = 1; step <= 5000; ++step) {
        if (live.empty() || random() % 3 == 0) {
            auto r = reservation(step);
            const auto status = ledger.reserve(r, domain::ResourceRequest::create(1, 4).value());
            if (status == LedgerStatus::Ok) live.emplace_back(r, false);
            else EXPECT_EQ(status, LedgerStatus::Capacity);
        } else {
            const auto index = static_cast<std::size_t>(random()) % live.size();
            auto& r = live[index];
            if (!r.second && random() % 2 == 0) { EXPECT_EQ(ledger.commit(r.first), LedgerStatus::Ok); r.second = true; }
            else {
                EXPECT_EQ(r.second ? ledger.release(r.first) : ledger.rollback(r.first), LedgerStatus::Ok);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        ASSERT_EQ(ledger.audit(), LedgerStatus::Ok) << "seed=602 step=" << step;
        EXPECT_EQ(ledger.reservation_count(), live.size());
        const auto account = ledger.inspect(key().worker_id).value();
        std::uint32_t tentative = 0, committed_count = 0;
        for (const auto& r : live) { if (r.second) ++committed_count; else ++tentative; }
        EXPECT_EQ(account.tentative.attempts, tentative); EXPECT_EQ(account.committed.attempts, committed_count);
        EXPECT_EQ(account.tentative.cpu, tentative); EXPECT_EQ(account.committed.cpu, committed_count);
        EXPECT_EQ(account.tentative.memory, tentative * 4U); EXPECT_EQ(account.committed.memory, committed_count * 4U);
    }
    for (const auto& r : live) EXPECT_EQ(r.second ? ledger.release(r.first) : ledger.rollback(r.first), LedgerStatus::Ok);
    EXPECT_EQ(ledger.reservation_count(), 0U); EXPECT_EQ(ledger.audit(), LedgerStatus::Ok);
}
} // namespace
} // namespace ts::controller
