#include "ts/controller/provisional_staging.h"
#include "ts/controller/views.h"

#include <gtest/gtest.h>

namespace ts::controller {
class StagingTestPeer {
public:
    static std::string bytes(const ProvisionalStaging& staging, const domain::SessionId& session) {
        const auto& buffer = staging.sessions_.at(session); return {buffer.raw.data(), buffer.bytes};
    }
    static void break_capacity(ProvisionalStaging& staging, const domain::SessionId& session) {
        staging.sessions_.at(session).raw.clear();
    }
};
namespace {
domain::QueueConfig bounds() { return {64, 10, 5, 1, 5, 2}; }
protocol::LogChunk chunk(const domain::SessionId& session, std::string data, std::uint64_t sequence = 0) {
    return {domain::WorkerId::parse("worker").value(), session,
        domain::TaskId::parse("00000000-0000-4000-8000-000000000001").value(),
        domain::AttemptId::parse("00000000-0000-4000-8000-000000000002").value(),
        protocol::Stream::Stdout, sequence, static_cast<std::uint32_t>(data.size()), protocol::base64_encode(data)};
}
TEST(M7Staging, RealReservedMemoryConservesBothDimensionsAndNeverReplenishes) {
    ProvisionalStaging staging(bounds()); const auto a = domain::SessionId::generate(), b = domain::SessionId::generate();
    ASSERT_TRUE(staging.reserve(a)); ASSERT_TRUE(staging.reserve(b)); EXPECT_FALSE(staging.reserve(domain::SessionId::generate()));
    protocol::LogContext context{{}, {}, 0, 0}; const std::string binary("a\0b", 3);
    EXPECT_EQ(staging.append(chunk(a, binary), context), StagingAdmission::Accepted);
    EXPECT_EQ(staging.append(chunk(a, "cd", 1), context), StagingAdmission::Accepted);
    EXPECT_EQ(StagingTestPeer::bytes(staging, a), binary + "cd");
    EXPECT_EQ(staging.append(chunk(a, "e", 2), context), StagingAdmission::ProtocolViolation);
    const auto usage = staging.usage(); EXPECT_EQ(usage.reserved_bytes, 10U); EXPECT_EQ(usage.reserved_chunks, 4U);
    EXPECT_EQ(usage.staged_bytes, 5U); EXPECT_EQ(usage.staged_chunks, 2U);
    EXPECT_TRUE(staging.reclaim(a)); EXPECT_FALSE(staging.reclaim(a));
    EXPECT_EQ(staging.usage().reserved_bytes, 5U); EXPECT_EQ(staging.usage().staged_bytes, 0U);
    EXPECT_TRUE(staging.reserve(domain::SessionId::generate()));
}
TEST(M7Staging, ChunkSlotsAndByteCreditAreIndependentAndReserveExcludesControlSlots) {
    auto q = bounds(); q.log_bytes = 100; q.initial_session_log_window_chunks = 3;
    ProvisionalStaging staging(q); const auto session = domain::SessionId::generate(); ASSERT_TRUE(staging.reserve(session));
    EXPECT_FALSE(staging.reserve(domain::SessionId::generate())); // one free log slot cannot satisfy a three-slot grant
    protocol::LogContext context{{}, {}, 0, 0};
    EXPECT_EQ(staging.append(chunk(session, "123456"), context), StagingAdmission::ProtocolViolation);
    EXPECT_EQ(staging.append(chunk(session, "123"), context), StagingAdmission::Accepted);
    EXPECT_EQ(staging.append(chunk(session, "45", 1), context), StagingAdmission::Accepted);
    EXPECT_EQ(staging.append(chunk(session, "6", 2), context), StagingAdmission::ProtocolViolation);
    EXPECT_EQ(staging.usage().staged_chunks, 2U);
}
TEST(M7Staging, CreditedCapacityMismatchIsInvariantFailureNotBackpressure) {
    ProvisionalStaging staging(bounds()); const auto id = domain::SessionId::generate(); ASSERT_TRUE(staging.reserve(id));
    StagingTestPeer::break_capacity(staging, id); protocol::LogContext context{{}, {}, 0, 0};
    EXPECT_EQ(staging.append(chunk(id, "a"), context), StagingAdmission::InvariantFailure);
    EXPECT_EQ(staging.usage().staged_bytes, 0U); EXPECT_EQ(context.out.next_sequence, 0U);
}
TEST(M7Staging, SequenceViolationNeverMutatesRetainedPosition) {
    ProvisionalStaging staging(bounds()); const auto id = domain::SessionId::generate(); ASSERT_TRUE(staging.reserve(id));
    protocol::LogContext context{{}, {}, 0, 0};
    EXPECT_EQ(staging.append(chunk(id, "a", 1), context), StagingAdmission::ProtocolViolation);
    EXPECT_EQ(context.out.next_sequence, 0U); EXPECT_EQ(staging.usage().staged_bytes, 0U);
}
TEST(M7Views, OpaqueTupleCursorIsCanonicalBoundedAndStrict) {
    storage::TaskCursor cursor{"2026-10-21T00:00:00.000000Z", "00000000-0000-4000-8000-000000000001"};
    auto parsed = parse_page_token(page_token(cursor)); ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().task_id, cursor.task_id); EXPECT_EQ(parsed.value().created_at, cursor.created_at);
    for (const auto& invalid : {std::string{}, std::string(257, 'x'), std::string("!!!!"),
         protocol::base64_encode("{}"), protocol::base64_encode("[1,2]"),
         protocol::base64_encode("[\"invalid-time\",\"invalid-id\"]"),
         protocol::base64_encode("[\"2026-10-21T00:00:00.000000Z\",\"00000000-0000-4000-8000-000000000001\",3]")})
        EXPECT_FALSE(parse_page_token(invalid));
}
} // namespace
} // namespace ts::controller
