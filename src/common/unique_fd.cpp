#include "ts/common/unique_fd.h"

#include <unistd.h>

#include <utility>

namespace ts::common {

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() noexcept {
    reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::get() const noexcept {
    return fd_;
}

UniqueFd::operator bool() const noexcept {
    return fd_ >= 0;
}

int UniqueFd::release() noexcept {
    return std::exchange(fd_, -1);
}

void UniqueFd::reset(int fd) noexcept {
    if (fd == fd_) {
        return;
    }
    const int old_fd = std::exchange(fd_, fd);
    if (old_fd >= 0) {
        // Linux may have already released old_fd when close reports EINTR. Retrying could close
        // an unrelated descriptor that reused the number, so close is deliberately attempted once.
        static_cast<void>(::close(old_fd));
    }
}

}  // namespace ts::common
