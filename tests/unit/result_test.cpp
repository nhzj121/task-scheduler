#include "ts/common/result.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace ts::common {
namespace {

TEST(Result, ValueSuccessSupportsBooleanConstAndMutableAccess) {
    auto result = Result<std::string>::success("initial");

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(static_cast<bool>(result));
    result.value() = "updated";

    const auto& const_result = result;
    EXPECT_EQ(const_result.value(), "updated");
    EXPECT_THROW(static_cast<void>(const_result.error()), std::logic_error);
}

TEST(Result, ValueFailureExposesErrorAndRejectsValueAccess) {
    const auto result = Result<int>::failure("expected failure");

    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.error().message, "expected failure");
    EXPECT_THROW(static_cast<void>(result.value()), std::logic_error);
}

TEST(Result, RvalueAccessMovesTheStoredValue) {
    auto result = Result<std::unique_ptr<int>>::success(std::make_unique<int>(42));

    auto moved = std::move(result).value();

    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(*moved, 42);
}

TEST(Result, VoidSuccessAndFailureHaveExplicitAccessRules) {
    const auto success = Result<void>::success();
    EXPECT_TRUE(success.has_value());
    EXPECT_TRUE(static_cast<bool>(success));
    EXPECT_NO_THROW(success.value());
    EXPECT_THROW(static_cast<void>(success.error()), std::logic_error);

    const auto failure = Result<void>::failure("void failure");
    EXPECT_FALSE(failure.has_value());
    EXPECT_FALSE(static_cast<bool>(failure));
    EXPECT_EQ(failure.error().message, "void failure");
    EXPECT_THROW(failure.value(), std::logic_error);
}

}  // namespace
}  // namespace ts::common
