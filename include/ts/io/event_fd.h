#pragma once

#include <cstdint>

#include "ts/common/result.h"
#include "ts/common/unique_fd.h"

namespace ts::io {

class EventFd final {
public:
    static common::Result<EventFd> create(std::uint64_t initial_value = 0U);

    EventFd(const EventFd&) = delete;
    EventFd& operator=(const EventFd&) = delete;
    EventFd(EventFd&&) noexcept = default;
    EventFd& operator=(EventFd&&) noexcept = default;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] common::Result<void> notify(std::uint64_t value = 1U) const;
    [[nodiscard]] common::Result<std::uint64_t> drain() const;

private:
    explicit EventFd(common::UniqueFd fd) noexcept;

    common::UniqueFd fd_;
};

}  // namespace ts::io
