#include "ts/io/timer_fd.h"

#include <cerrno>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <sys/timerfd.h>
#include <unistd.h>

namespace ts::io {
namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;

std::string errno_message(const char* operation, int error_number) {
    return std::string(operation) + " failed: " +
           std::error_code(error_number, std::generic_category()).message();
}

}  // namespace

common::Result<TimerFd> TimerFd::create() {
    const int fd = ::timerfd_create(clock_id(), TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd == -1) {
        return common::Result<TimerFd>::failure(errno_message("timerfd_create", errno));
    }
    return common::Result<TimerFd>::success(TimerFd(common::UniqueFd(fd)));
}

TimerFd::TimerFd(common::UniqueFd fd) noexcept : fd_(std::move(fd)) {}

int TimerFd::fd() const noexcept {
    return fd_.get();
}

common::Result<void> TimerFd::arm_once(std::chrono::nanoseconds delay) const {
    if (delay.count() <= 0) {
        return common::Result<void>::failure(
            "one-shot timer delay must be positive");
    }
    const std::int64_t nanoseconds = delay.count();
    itimerspec specification{};
    specification.it_value.tv_sec =
        static_cast<time_t>(nanoseconds / kNanosecondsPerSecond);
    specification.it_value.tv_nsec =
        static_cast<long>(nanoseconds % kNanosecondsPerSecond);
    if (::timerfd_settime(fd_.get(), 0, &specification, nullptr) == -1) {
        return common::Result<void>::failure(errno_message("timerfd_settime", errno));
    }
    return common::Result<void>::success();
}

common::Result<void> TimerFd::disarm() const {
    const itimerspec specification{};
    if (::timerfd_settime(fd_.get(), 0, &specification, nullptr) == -1) {
        return common::Result<void>::failure(errno_message("timerfd_settime", errno));
    }
    return common::Result<void>::success();
}

common::Result<std::uint64_t> TimerFd::drain() const {
    std::uint64_t total = 0U;
    for (;;) {
        std::uint64_t expirations = 0U;
        const ssize_t read_count = ::read(fd_.get(), &expirations, sizeof(expirations));
        if (read_count == static_cast<ssize_t>(sizeof(expirations))) {
            if (expirations > std::numeric_limits<std::uint64_t>::max() - total) {
                return common::Result<std::uint64_t>::failure(
                    "timerfd expiration count overflow");
            }
            total += expirations;
            continue;
        }
        if (read_count == -1 && errno == EINTR) {
            continue;
        }
        if (read_count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return common::Result<std::uint64_t>::success(total);
        }
        if (read_count == -1) {
            return common::Result<std::uint64_t>::failure(
                errno_message("timerfd read", errno));
        }
        return common::Result<std::uint64_t>::failure(
            "timerfd read returned a short count");
    }
}

}  // namespace ts::io
