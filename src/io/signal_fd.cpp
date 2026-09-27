#include "ts/io/signal_fd.h"

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace ts::io {
namespace {

std::string errno_message(const char* operation, int error_number) {
    return std::string(operation) + " failed: " +
           std::error_code(error_number, std::generic_category()).message();
}

}  // namespace

common::Result<SignalFd> SignalFd::create(const sigset_t& mask) {
    const int fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd == -1) {
        return common::Result<SignalFd>::failure(errno_message("signalfd", errno));
    }
    return common::Result<SignalFd>::success(SignalFd(common::UniqueFd(fd)));
}

SignalFd::SignalFd(common::UniqueFd fd) noexcept : fd_(std::move(fd)) {}

int SignalFd::fd() const noexcept {
    return fd_.get();
}

common::Result<std::vector<signalfd_siginfo>> SignalFd::drain() const {
    std::vector<signalfd_siginfo> records;
    for (;;) {
        signalfd_siginfo information{};
        const ssize_t read_count = ::read(fd_.get(), &information, sizeof(information));
        if (read_count == static_cast<ssize_t>(sizeof(information))) {
            records.push_back(information);
            continue;
        }
        if (read_count == -1 && errno == EINTR) {
            continue;
        }
        if (read_count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return common::Result<std::vector<signalfd_siginfo>>::success(
                std::move(records));
        }
        if (read_count == -1) {
            return common::Result<std::vector<signalfd_siginfo>>::failure(
                errno_message("signalfd read", errno));
        }
        return common::Result<std::vector<signalfd_siginfo>>::failure(
            "signalfd read returned a short count");
    }
}

}  // namespace ts::io
