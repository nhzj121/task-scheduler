#pragma once

#include <chrono>
#include <cstdint>

#include <time.h>

#include "ts/common/result.h"
#include "ts/common/unique_fd.h"

namespace ts::io {

class TimerFd final {
public:
    static common::Result<TimerFd> create();

    TimerFd(const TimerFd&) = delete;
    TimerFd& operator=(const TimerFd&) = delete;
    TimerFd(TimerFd&&) noexcept = default;
    TimerFd& operator=(TimerFd&&) noexcept = default;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] static constexpr clockid_t clock_id() noexcept {
        return CLOCK_MONOTONIC;
    }

    [[nodiscard]] common::Result<void> arm_once(std::chrono::nanoseconds delay) const;
    [[nodiscard]] common::Result<void> disarm() const;
    [[nodiscard]] common::Result<std::uint64_t> drain() const;

private:
    explicit TimerFd(common::UniqueFd fd) noexcept;

    common::UniqueFd fd_;
};

}  // namespace ts::io
