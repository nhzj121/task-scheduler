#include "ts/domain/resources.h"

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace ts::domain {
namespace {

TEST(Resources, ValidatesCpuAndAllowsZeroMemory) {
    EXPECT_FALSE(ResourceCapacity::create(0U, 1U));
    EXPECT_FALSE(ResourceRequest::create(0U, 0U));

    const auto capacity = ResourceCapacity::create(1U, 0U);
    const auto request = ResourceRequest::create(1U, 0U);
    ASSERT_TRUE(capacity);
    ASSERT_TRUE(request);
    EXPECT_TRUE(fits_within(capacity.value(), ResourceUsage::zero(), request.value()));
}

TEST(Resources, ExactCapacityBoundaryFitsAndOneOverDoesNot) {
    const auto capacity = ResourceCapacity::create(8U, 16'384U).value();
    const auto usage = ResourceUsage::from_values(6U, 16'000U);
    const auto exact = ResourceRequest::create(2U, 384U).value();
    const auto cpu_over = ResourceRequest::create(3U, 1U).value();
    const auto memory_over = ResourceRequest::create(1U, 385U).value();

    EXPECT_TRUE(fits_within(capacity, usage, exact));
    EXPECT_FALSE(fits_within(capacity, usage, cpu_over));
    EXPECT_FALSE(fits_within(capacity, usage, memory_over));
}

TEST(Resources, AdditionAndSubtractionRoundTrip) {
    const auto request = ResourceRequest::create(2U, 512U).value();
    const auto added = add(ResourceUsage::from_values(3U, 1'024U), request);
    ASSERT_TRUE(added);
    EXPECT_EQ(added.value().cpu_slots(), 5U);
    EXPECT_EQ(added.value().memory_mb(), 1'536U);

    const auto subtracted = subtract(added.value(), request);
    ASSERT_TRUE(subtracted);
    EXPECT_EQ(subtracted.value().cpu_slots(), 3U);
    EXPECT_EQ(subtracted.value().memory_mb(), 1'024U);
}

TEST(Resources, DetectsOverflowAndUnderflow) {
    const auto one = ResourceRequest::create(1U, 1U).value();
    EXPECT_FALSE(add(ResourceUsage::from_values(std::numeric_limits<std::uint32_t>::max(), 0U),
                     one));
    EXPECT_FALSE(add(ResourceUsage::from_values(0U, std::numeric_limits<std::uint64_t>::max()),
                     one));
    EXPECT_FALSE(subtract(ResourceUsage::zero(), one));
}

TEST(Resources, RejectsAlreadyOversubscribedUsage) {
    const auto capacity = ResourceCapacity::create(4U, 100U).value();
    const auto request = ResourceRequest::create(1U, 0U).value();
    EXPECT_FALSE(fits_within(capacity, ResourceUsage::from_values(5U, 0U), request));
    EXPECT_FALSE(fits_within(capacity, ResourceUsage::from_values(0U, 101U), request));
}

}  // namespace
}  // namespace ts::domain
