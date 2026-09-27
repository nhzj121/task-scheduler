#include "ts/common/unique_fd.h"

#include <cerrno>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace ts::common {
namespace {

int open_dev_null() {
    return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

bool is_open(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

TEST(UniqueFd, IsMoveOnlyAndDefaultInvalid) {
    static_assert(!std::is_copy_constructible_v<UniqueFd>);
    static_assert(!std::is_copy_assignable_v<UniqueFd>);
    static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
    static_assert(std::is_nothrow_move_assignable_v<UniqueFd>);
    static_assert(std::is_nothrow_destructible_v<UniqueFd>);

    const UniqueFd fd;
    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.get(), -1);
}

TEST(UniqueFd, DestructionClosesOwnedDescriptorExactlyOnce) {
    const int raw_fd = open_dev_null();
    ASSERT_GE(raw_fd, 0);
    {
        const UniqueFd fd(raw_fd);
        ASSERT_TRUE(fd);
        EXPECT_TRUE(is_open(raw_fd));
    }
    EXPECT_FALSE(is_open(raw_fd));
}

TEST(UniqueFd, MoveConstructionAndAssignmentTransferOwnership) {
    const int first_raw = open_dev_null();
    const int second_raw = open_dev_null();
    ASSERT_GE(first_raw, 0);
    ASSERT_GE(second_raw, 0);

    UniqueFd first(first_raw);
    UniqueFd moved(std::move(first));
    EXPECT_FALSE(first);
    EXPECT_EQ(moved.get(), first_raw);

    UniqueFd target(second_raw);
    target = std::move(moved);
    EXPECT_FALSE(moved);
    EXPECT_EQ(target.get(), first_raw);
    EXPECT_FALSE(is_open(second_raw));

    target = std::move(target);
    EXPECT_EQ(target.get(), first_raw);
}

TEST(UniqueFd, ReleaseTransfersOwnership) {
    UniqueFd owner(open_dev_null());
    ASSERT_TRUE(owner);
    const int raw_fd = owner.release();

    EXPECT_FALSE(owner);
    EXPECT_TRUE(is_open(raw_fd));
    EXPECT_EQ(::close(raw_fd), 0);
}

TEST(UniqueFd, ResetClosesOldDescriptorAndHasSafeSelfReset) {
    const int old_raw = open_dev_null();
    const int replacement_raw = open_dev_null();
    ASSERT_GE(old_raw, 0);
    ASSERT_GE(replacement_raw, 0);

    UniqueFd owner(old_raw);
    owner.reset(old_raw);
    EXPECT_EQ(owner.get(), old_raw);
    EXPECT_TRUE(is_open(old_raw));

    owner.reset(replacement_raw);
    EXPECT_FALSE(is_open(old_raw));
    EXPECT_EQ(owner.get(), replacement_raw);
    EXPECT_TRUE(is_open(replacement_raw));
}

TEST(UniqueFd, InvalidOwnerDoesNotCloseAReusedDescriptorNumber) {
    UniqueFd former_owner(open_dev_null());
    ASSERT_TRUE(former_owner);
    const int reused_number = former_owner.get();
    former_owner.reset();

    int candidate = open_dev_null();
    ASSERT_GE(candidate, 0);
    if (candidate != reused_number) {
        ASSERT_EQ(::dup3(candidate, reused_number, O_CLOEXEC), reused_number);
        ASSERT_EQ(::close(candidate), 0);
        candidate = reused_number;
    }
    ASSERT_EQ(candidate, reused_number);

    former_owner.reset();
    EXPECT_TRUE(is_open(reused_number));
    EXPECT_EQ(::close(reused_number), 0);
}

}  // namespace
}  // namespace ts::common
