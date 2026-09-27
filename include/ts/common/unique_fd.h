#pragma once

namespace ts::common {

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept;
    ~UniqueFd() noexcept;

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    [[nodiscard]] int get() const noexcept;
    explicit operator bool() const noexcept;

    [[nodiscard]] int release() noexcept;
    void reset(int fd = -1) noexcept;

private:
    int fd_;
};

}  // namespace ts::common
