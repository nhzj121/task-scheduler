#include "ts/common/timestamp.h"

#include <gtest/gtest.h>

namespace ts::common {
namespace {

TEST(Timestamp, FormatsAndParsesEpochWithMicroseconds) {
    const auto value = Timestamp::from_unix_microseconds(123'456);
    EXPECT_EQ(value.to_string(), "1970-01-01T00:00:00.123456Z");

    const auto parsed = Timestamp::parse(value.to_string());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value(), value);
}

TEST(Timestamp, HandlesDatesBeforeEpochAndLeapDay) {
    const auto before_epoch = Timestamp::from_unix_microseconds(-1);
    EXPECT_EQ(before_epoch.to_string(), "1969-12-31T23:59:59.999999Z");

    const auto leap = Timestamp::parse("2024-02-29T12:34:56.000001Z");
    ASSERT_TRUE(leap);
    EXPECT_EQ(leap.value().to_string(), "2024-02-29T12:34:56.000001Z");
}

TEST(Timestamp, RejectsNonCanonicalAndInvalidValues) {
    EXPECT_FALSE(Timestamp::parse("2026-09-26T08:15:30Z"));
    EXPECT_FALSE(Timestamp::parse("2023-02-29T00:00:00.000000Z"));
    EXPECT_FALSE(Timestamp::parse("2024-01-01T24:00:00.000000Z"));
    EXPECT_FALSE(Timestamp::parse("2024-01-01T00:00:60.000000Z"));
    EXPECT_FALSE(Timestamp::parse("2024-01-01t00:00:00.000000Z"));
}

TEST(Timestamp, NowProducesCanonicalRoundTrippableText) {
    const auto now = Timestamp::now();
    const auto text = now.to_string();
    const auto reparsed = Timestamp::parse(text);
    ASSERT_TRUE(reparsed);
    EXPECT_EQ(reparsed.value(), now);
}

}  // namespace
}  // namespace ts::common
