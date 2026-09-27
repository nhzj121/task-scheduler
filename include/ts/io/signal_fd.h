#pragma once

#include <vector>

#include <signal.h>
#include <sys/signalfd.h>

#include "ts/common/result.h"
#include "ts/common/unique_fd.h"

namespace ts::io {

class SignalFd final {
public:
    static common::Result<SignalFd> create(const sigset_t& mask);

    SignalFd(const SignalFd&) = delete;
    SignalFd& operator=(const SignalFd&) = delete;
    SignalFd(SignalFd&&) noexcept = default;
    SignalFd& operator=(SignalFd&&) noexcept = default;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] common::Result<std::vector<signalfd_siginfo>> drain() const;

private:
    explicit SignalFd(common::UniqueFd fd) noexcept;

    common::UniqueFd fd_;
};

}  // namespace ts::io
