#include "ts/controller/log_credit_ledger.h"
#include <random>
#include <gtest/gtest.h>
#include "ts/domain/ids.h"

namespace {
using namespace ts;
logs::Binding identity() {
    logs::Binding b{domain::TaskId::generate().value(), domain::AttemptId::generate().value(), domain::SessionId::generate().value(), 1, 1, "", ""};
    const auto stem = "tasks/" + b.task + "/attempt-1";
    b.stdout_path = stem + ".stdout.log"; b.stderr_path = stem + ".stderr.log"; return b;
}
TEST(LogCreditLedger, ReclaimNeverReleasesInflightOwnershipAndDuplicateCompletionCannotMintCredit) {
    controller::LogCreditLedger ledger(10, 3); auto b = identity();
    ASSERT_TRUE(ledger.reserve(b.session, {10, 3})); ASSERT_TRUE(ledger.activate(b.session, 1));
    ASSERT_TRUE(ledger.accept(1, b, 1)); ASSERT_TRUE(ledger.accept(2, b, 9));
    EXPECT_FALSE(ledger.accept(3, b, 1)); ASSERT_TRUE(ledger.reclaim(b.session));
    EXPECT_EQ(ledger.partition().free.bytes, 0U); EXPECT_EQ(ledger.partition().free.slots, 1U);
    auto stale = b; ++stale.generation; EXPECT_FALSE(ledger.complete(1, stale, 1));
    ASSERT_TRUE(ledger.complete(1, b, 1)); EXPECT_FALSE(ledger.complete(1, b, 1));
    ASSERT_TRUE(ledger.complete(2, b, 9)); EXPECT_EQ(ledger.partition().free.bytes, 10U);
    EXPECT_EQ(ledger.partition().free.slots, 3U); EXPECT_FALSE(ledger.grant(b.session, 1, {1, 1}));
}
TEST(LogCreditLedger, IndependentDimensionsAndUint64CapacityCannotOverflow) {
    controller::LogCreditLedger ledger(UINT64_MAX, UINT64_MAX); auto b = identity();
    ASSERT_TRUE(ledger.reserve(b.session, {UINT64_MAX, UINT64_MAX})); ASSERT_TRUE(ledger.activate(b.session, 1));
    EXPECT_FALSE(ledger.grant(b.session, 1, {1, 0})); EXPECT_FALSE(ledger.grant(b.session, 1, {0, 1}));
    ASSERT_TRUE(ledger.accept(1, b, 1)); ASSERT_TRUE(ledger.complete(1, b, 1));
    ASSERT_TRUE(ledger.grant(b.session, 1, {1, 0})); ASSERT_TRUE(ledger.grant(b.session, 1, {0, 1}));
    EXPECT_FALSE(ledger.grant(b.session, 1, {0, 0})); EXPECT_EQ(ledger.partition().granted.bytes, UINT64_MAX);
    EXPECT_EQ(ledger.partition().granted.slots, UINT64_MAX);
}
TEST(LogCreditLedger, SeededRandomTransfersConserveBothPartitionsAtEveryStep) {
    for (const auto seed : {8U, 813U, 20261028U}) {
        SCOPED_TRACE(seed); std::mt19937 random(seed); controller::LogCreditLedger ledger(1000, 37);
        std::vector<logs::Binding> sessions;
        for (unsigned int i = 0; i < 4; ++i) { auto b = identity(); ASSERT_TRUE(ledger.reserve(b.session, {200, 8})); ASSERT_TRUE(ledger.activate(b.session, 1)); sessions.push_back(b); }
        struct Pending { std::uint64_t op, bytes; logs::Binding binding; };
        std::vector<Pending> work; std::uint64_t next = 1;
        for (int step = 0; step < 10000; ++step) {
            auto& b = sessions[random() % sessions.size()]; const auto p = ledger.partition();
            switch (random() % 4) {
                case 0: {
                    const auto credit = ledger.outstanding(b.session); const std::uint64_t bytes = 1 + random() % 40;
                    if (credit && credit->slots && credit->bytes >= bytes) { ASSERT_TRUE(ledger.accept(next, b, bytes)); work.push_back({next++, bytes, b}); }
                    break;
                }
                case 1:
                    if (!work.empty()) { const auto w = work.back(); work.pop_back(); ASSERT_TRUE(ledger.complete(w.op, w.binding, w.bytes)); }
                    break;
                case 2:
                    if (ledger.outstanding(b.session) && (p.free.bytes || p.free.slots)) { ASSERT_TRUE(ledger.grant(b.session, 1, {p.free.bytes, p.free.slots})); }
                    break;
                default:
                    if (ledger.reclaim(b.session)) { const auto free = ledger.partition().free; if (free.bytes && free.slots) { ASSERT_TRUE(ledger.reserve(b.session, {1, 1})); ASSERT_TRUE(ledger.activate(b.session, 1)); } }
                    break;
            }
            const auto state = ledger.partition();
            EXPECT_EQ(state.free.bytes + state.granted.bytes + state.queued.bytes, 1000U);
            EXPECT_EQ(state.free.slots + state.granted.slots + state.queued.slots, 37U);
        }
    }
}
} // namespace
