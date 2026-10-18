#include "ts/protocol/frame.h"

#include <gtest/gtest.h>
#include <limits>
#include <random>

namespace ts::protocol {
namespace {
constexpr auto client = Direction::ClientToController;
const std::array<std::uint8_t, 20> golden{
    0x54,0x53,0x43,0x48,0,1,1,3,0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,0,0,0,2};
std::string wire(std::string payload = "{}") {
    return encode_frame({{Type::GetTask, 0x0123456789abcdefULL,
                          static_cast<std::uint32_t>(payload.size())}, std::move(payload)}, client).value();
}
TEST(Frame, LiteralGoldenHeaderAnd64BitOrder) {
    const Header header{Type::GetTask, 0x0123456789abcdefULL, 2};
    EXPECT_EQ(encode_header(header, client).value(), golden);
    EXPECT_EQ(decode_header(golden.data(), client).value().request_id, header.request_id);
    for (auto value : {0ULL, 1ULL, 0x0123456789abcdefULL, std::numeric_limits<unsigned long long>::max()}) {
        const auto bytes = encode_u64(value);
        EXPECT_EQ(decode_u64(bytes.data()), value);
    }
}
TEST(Frame, EverySplitAndSingleBytes) {
    const auto bytes = wire("{\"task_id\":\"8e7f5e0c-11fe-4dc4-8437-fcab1b332878\"}");
    for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
        FrameParser parser(client); std::size_t count = 0;
        const auto consume = [&](Frame&& frame) { ++count; EXPECT_EQ(frame.payload, bytes.substr(20)); return true; };
        ASSERT_TRUE(parser.feed(std::string_view(bytes).substr(0, cut), consume));
        ASSERT_TRUE(parser.feed(std::string_view(bytes).substr(cut), consume));
        EXPECT_EQ(count, 1U); EXPECT_EQ(parser.buffered_bytes(), 0U);
    }
    FrameParser parser(client); std::size_t count = 0;
    for (char byte : bytes) ASSERT_TRUE(parser.feed(std::string_view(&byte, 1), [&](Frame&&) { ++count; return true; }));
    EXPECT_EQ(count, 1U);
}
TEST(Frame, MergedFramesAndPartialTail) {
    FrameParser parser(client); std::size_t count = 0;
    const auto consume = [&](Frame&&) { ++count; return true; };
    const auto bytes = wire();
    auto result = parser.feed(bytes + bytes + bytes.substr(0, 21), consume);
    ASSERT_TRUE(result); EXPECT_EQ(result.value().frames, 2U); EXPECT_TRUE(result.value().incomplete);
    EXPECT_EQ(count, 2U); ASSERT_TRUE(parser.feed(bytes.substr(21), consume)); EXPECT_EQ(count, 3U);
}
TEST(Frame, MaximumAndEarlyRejectedLengthsNoResynchronization) {
    FrameParser parser(client);
    auto result = parser.feed(wire(std::string(kMaxPayload, 'x')), [](Frame&& frame) { EXPECT_EQ(frame.payload.size(), kMaxPayload); return true; });
    ASSERT_TRUE(result); EXPECT_EQ(result.value().frames, 1U);
    for (std::uint32_t length : {0U, kMaxPayload + 1U, 0xffffffffU}) {
        auto bad = golden;
        for (std::size_t i = 0; i < 4; ++i) bad[19 - i] = static_cast<std::uint8_t>(length >> (8U * i));
        FrameParser invalid(client);
        auto parsed = invalid.feed(std::string(reinterpret_cast<const char*>(bad.data()), bad.size()) + wire(), [](Frame&&) { ADD_FAILURE(); return true; });
        EXPECT_FALSE(parsed); EXPECT_LE(invalid.payload_capacity(), 20U); EXPECT_TRUE(invalid.failed());
        EXPECT_FALSE(invalid.feed(wire(), [](Frame&&) { return true; }));
    }
    EXPECT_THROW(FrameParser(client, kMaxPayload), std::invalid_argument);
}
TEST(Frame, HeaderRolesTypesAndIds) {
    for (std::size_t position : {0U, 4U, 6U}) {
        auto bad = golden; bad[position] = 0xff;
        EXPECT_FALSE(decode_header(bad.data(), client));
    }
    EXPECT_FALSE(decode_header(golden.data(), Direction::WorkerToController));
    for (auto id : {0x0001U, 0x0101U, 0x010eU, 0x0201U, 0x020bU}) EXPECT_TRUE(known_type(static_cast<Type>(id)));
    EXPECT_FALSE(encode_header({Type::GetTask, 0, 2}, client));
    EXPECT_FALSE(encode_header({Type::Heartbeat, 1, 2}, Direction::WorkerToController));
    EXPECT_TRUE(encode_header({Type::Heartbeat, 0, 2}, Direction::WorkerToController));
    EXPECT_TRUE(encode_header({Type::ErrorResponse, 0, 2}, Direction::ControllerToClient));
    EXPECT_EQ(response_type(Type::WorkerCancelTask), Type::WorkerCancelTaskAck);
    EXPECT_THROW(static_cast<void>(response_type(Type::Heartbeat)), std::invalid_argument);
    EXPECT_FALSE(encode_frame({{Type::GetTask, 1, 3}, "{}"}, client));
}
TEST(Frame, DeterministicMutationBoundedMemory) {
    std::mt19937 random(3467);
    for (int iteration = 0; iteration < 3000; ++iteration) {
        auto bytes = wire();
        const auto position = static_cast<std::size_t>(random() % bytes.size());
        bytes[position] = static_cast<char>(random() & 0xffU);
        FrameParser parser(client);
        const auto result = parser.feed(bytes, [](Frame&&) { return true; });
        static_cast<void>(result);
        EXPECT_LE(parser.buffered_bytes(), kHeaderSize + kMaxPayload);
        EXPECT_LE(parser.payload_capacity(), kMaxPayload);
    }
}
TEST(Frame, ConsumerFailureIsSticky) {
    FrameParser parser(client);
    EXPECT_FALSE(parser.feed(wire(), [](Frame&&) { return false; }));
    EXPECT_TRUE(parser.failed());
}
} // namespace
} // namespace ts::protocol
