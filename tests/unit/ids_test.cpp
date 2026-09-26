#include "ts/domain/ids.h"

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

namespace ts::domain {
namespace {

TEST(Ids, UuidTypesAreNotInterchangeable) {
    static_assert(!std::is_same_v<TaskId, AttemptId>);
    static_assert(!std::is_same_v<TaskId, SessionId>);
    static_assert(!std::is_convertible_v<TaskId, AttemptId>);
}

TEST(Ids, ParsesOnlyCanonicalLowercaseUuid) {
    constexpr auto valid = "8e7f5e0c-11fe-4dc4-8437-fcab1b332878";
    const auto parsed = TaskId::parse(valid);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().value(), valid);

    EXPECT_FALSE(TaskId::parse("8E7F5E0C-11FE-4DC4-8437-FCAB1B332878"));
    EXPECT_FALSE(TaskId::parse("8e7f5e0c11fe4dc48437fcab1b332878"));
    EXPECT_FALSE(TaskId::parse("not-a-uuid"));
}

TEST(Ids, GeneratesCanonicalVersion4Uuid) {
    const auto generated = SessionId::generate().value();
    EXPECT_TRUE(detail::is_canonical_uuid(generated));
    ASSERT_EQ(generated.size(), 36U);
    EXPECT_EQ(generated[14], '4');
    EXPECT_NE(std::string("89ab").find(generated[19]), std::string::npos);
}

TEST(Ids, WorkerIdEnforcesExplicitWireBounds) {
    EXPECT_FALSE(WorkerId::parse(""));
    EXPECT_TRUE(WorkerId::parse("worker-a"));
    EXPECT_TRUE(WorkerId::parse(std::string(WorkerId::kMaximumBytes, 'a')));
    EXPECT_FALSE(WorkerId::parse(std::string(WorkerId::kMaximumBytes + 1U, 'a')));
    EXPECT_FALSE(WorkerId::parse(std::string("bad\0id", 6U)));
    EXPECT_FALSE(WorkerId::parse(std::string(1U, static_cast<char>(0x80))));
}

TEST(Ids, AttemptNumbersStartAtOneAndDetectOverflow) {
    EXPECT_FALSE(AttemptNumber::create(0U));
    const auto first = AttemptNumber::create(1U);
    ASSERT_TRUE(first);
    const auto second = first.value().next();
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().value(), 2U);

    const auto maximum = AttemptNumber::create(std::numeric_limits<std::uint32_t>::max());
    ASSERT_TRUE(maximum);
    EXPECT_FALSE(maximum.value().next());
}

}  // namespace
}  // namespace ts::domain
