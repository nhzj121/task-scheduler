#include "ts/net/output_queue.h"
#include <gtest/gtest.h>

namespace ts::net {
namespace {
Outbound log(std::string bytes, std::uint64_t group, std::uint64_t ordinal) { return {std::move(bytes),QueueClass::Log,OrderPoint{group,ordinal},std::nullopt}; }
Outbound control(std::string bytes, std::optional<OrderPoint> dependency = {}) { return {std::move(bytes),QueueClass::Control,std::nullopt,dependency}; }
std::string drain(OutputQueue& output, std::size_t maximum = 1) {
    std::string wire;
    while (!output.empty()) {
        const auto bytes = output.front();
        if (bytes.empty()) { ADD_FAILURE() << "queue made no progress"; break; }
        const auto count = std::min(maximum,bytes.size()); wire.append(bytes.substr(0,count));
        EXPECT_TRUE(output.advance(count));
    }
    return wire;
}
TEST(OutputQueue, PartialFramePinnedAndControlPriorityOnlyAtBoundary) {
    OutputQueue queue;
    ASSERT_TRUE(queue.enqueue(log("log1",1,1))); EXPECT_EQ(queue.front(),"log1");
    ASSERT_TRUE(queue.advance(1)); ASSERT_TRUE(queue.enqueue(log("log2",1,2)));
    ASSERT_TRUE(queue.enqueue(control("heartbeat")));
    EXPECT_EQ(drain(queue),"og1heartbeatlog2");
    EXPECT_EQ(queue.bytes(QueueClass::Log),0U); EXPECT_EQ(queue.slots(QueueClass::Control),0U);
}
TEST(OutputQueue, FinishBarrierCrossStreamAndEligibleControlBypass) {
    OutputQueue queue;
    ASSERT_TRUE(queue.enqueue(log("stdout-A",10,1)));
    EXPECT_EQ(queue.front(),"stdout-A"); ASSERT_TRUE(queue.advance(2));
    ASSERT_TRUE(queue.enqueue(log("stderr-B",10,2)));
    ASSERT_TRUE(queue.enqueue(control("FINISH",OrderPoint{10,2})));
    ASSERT_TRUE(queue.enqueue(control("ACK")));
    EXPECT_EQ(drain(queue,2),"dout-AACKstderr-BFINISH");
}
TEST(OutputQueue, OtherGroupsDoNotImposeGlobalFinishBarrier) {
    OutputQueue queue;
    ASSERT_TRUE(queue.enqueue(log("unrelated",22,1)));
    ASSERT_TRUE(queue.enqueue(log("same-group",11,1)));
    ASSERT_TRUE(queue.enqueue(control("FINISH",OrderPoint{11,1})));
    EXPECT_EQ(drain(queue),"same-groupFINISHunrelated");
    ASSERT_TRUE(queue.enqueue(log("unrelated2",22,2)));
    ASSERT_TRUE(queue.enqueue(control("FINISH2",OrderPoint{11,1})));
    EXPECT_EQ(drain(queue),"FINISH2unrelated2");
}
TEST(OutputQueue, BoundedBytesAndSlotsIncludeWholePartiallySentFrame) {
    OutputQueue queue({{5,2},{6,1}});
    ASSERT_TRUE(queue.enqueue(log("123456",1,1))); EXPECT_EQ(queue.front(),"123456"); ASSERT_TRUE(queue.advance(5));
    EXPECT_EQ(queue.bytes(QueueClass::Log),6U); EXPECT_EQ(queue.slots(QueueClass::Log),1U);
    EXPECT_FALSE(queue.enqueue(log("x",1,2))); ASSERT_TRUE(queue.advance(1));
    ASSERT_TRUE(queue.enqueue(control("123"))); ASSERT_TRUE(queue.enqueue(control("45")));
    EXPECT_FALSE(queue.enqueue(control("6"))); EXPECT_EQ(queue.bytes(QueueClass::Control),5U);
    queue.clear(); EXPECT_TRUE(queue.empty()); EXPECT_EQ(queue.bytes(QueueClass::Control),0U);
}
TEST(OutputQueue, InvalidMetadataAndProgressExplicitlyRejected) {
    OutputQueue queue;
    EXPECT_FALSE(queue.enqueue(control(""))); EXPECT_FALSE(queue.advance(1));
    EXPECT_FALSE(queue.enqueue(log("x",1,0)));
    ASSERT_TRUE(queue.enqueue(log("one",1,1))); EXPECT_FALSE(queue.enqueue(log("old",1,1)));
    EXPECT_EQ(queue.front(),"one"); EXPECT_FALSE(queue.advance(4)); EXPECT_FALSE(queue.advance(0));
    EXPECT_THROW(OutputQueue(OutputLimits{{0,1},{1,1}}),std::invalid_argument);
}
TEST(OutputQueue, SlotLimitsRemainIndependentOfByteLimitsAndPartialProgress) {
    OutputQueue queue({{100,1},{100,1}});
    ASSERT_TRUE(queue.enqueue(control("abc"))); EXPECT_FALSE(queue.enqueue(control("d")));
    EXPECT_EQ(queue.front(),"abc"); ASSERT_TRUE(queue.advance(2)); EXPECT_FALSE(queue.enqueue(control("e")));
    ASSERT_TRUE(queue.enqueue(log("log",7,1))); EXPECT_FALSE(queue.enqueue(log("next",7,2)));
    ASSERT_TRUE(queue.advance(1)); ASSERT_TRUE(queue.enqueue(control("f")));
    EXPECT_EQ(drain(queue),"flog");
}
} // namespace
} // namespace ts::net
