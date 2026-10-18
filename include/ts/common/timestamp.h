#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ts/common/result.h"

namespace ts::common { 

class Timestamp {
public:
    static Timestamp now();
    static Timestamp from_unix_microseconds(std::int64_t value) noexcept;
    static Result<Timestamp> parse(std::string_view text);

    [[nodiscard]] std::int64_t unix_microseconds() const noexcept;
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(Timestamp lhs, Timestamp rhs) noexcept {
        return lhs.unix_microseconds_ == rhs.unix_microseconds_;
    }

    friend bool operator<(Timestamp lhs, Timestamp rhs) noexcept {
        return lhs.unix_microseconds_ < rhs.unix_microseconds_;
    }

private:
    explicit Timestamp(std::int64_t unix_microseconds) noexcept
        : unix_microseconds_(unix_microseconds) {}

    std::int64_t unix_microseconds_;
};

}  // namespace ts::common
