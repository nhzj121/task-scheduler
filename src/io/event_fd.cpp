#include "ts/io/event_fd.h"

#include <cerrno>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace ts::io {
namespace {

std::string errno_message(const char* operation, int error_number) {
    return std::string(operation) + " failed: " +
           std::error_code(error_number, std::generic_category()).message();
}

}  // namespace

common::Result<EventFd> EventFd::create(std::uint64_t initial_value) {
    if (initial_value > std::numeric_limits<unsigned int>::max()) {
        return common::Result<EventFd>::failure(
            "eventfd initial value exceeds the Linux API range");
    }
    const int fd = ::eventfd(static_cast<unsigned int>(initial_value),
                             EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd == -1) {
        return common::Result<EventFd>::failure(errno_message("eventfd", errno));
    }
    return common::Result<EventFd>::success(EventFd(common::UniqueFd(fd)));
}

EventFd::EventFd(common::UniqueFd fd) noexcept : fd_(std::move(fd)) {}

int EventFd::fd() const noexcept {
    return fd_.get();
}

common::Result<void> EventFd::notify(std::uint64_t value) const {
    if (value == 0U || value == std::numeric_limits<std::uint64_t>::max()) {
        return common::Result<void>::failure(
            "eventfd notification value must be in 1..UINT64_MAX-1");
    }
    for (;;) {
        const ssize_t written = ::write(fd_.get(), &value, sizeof(value));
        if (written == static_cast<ssize_t>(sizeof(value))) {
            return common::Result<void>::success();
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written == -1) {
            return common::Result<void>::failure(errno_message("eventfd write", errno));
        }
        return common::Result<void>::failure("eventfd write returned a short count");
    }
}

common::Result<std::uint64_t> EventFd::drain() const {
    std::uint64_t total = 0U;
    for (;;) {
        std::uint64_t value = 0U;
        const ssize_t read_count = ::read(fd_.get(), &value, sizeof(value));
        if (read_count == static_cast<ssize_t>(sizeof(value))) {
            if (value > std::numeric_limits<std::uint64_t>::max() - total) {
                return common::Result<std::uint64_t>::failure(
                    "eventfd drain count overflow");
            }
            total += value;
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
                errno_message("eventfd read", errno));
        }
        return common::Result<std::uint64_t>::failure(
            "eventfd read returned a short count");
    }
}

}  // namespace ts::io
