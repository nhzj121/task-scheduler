#include "ts/common/timestamp.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <tuple>

namespace ts::common {
namespace {

constexpr std::int64_t kSecondsPerDay = 86'400;
constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

constexpr bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr unsigned days_in_month(int year, unsigned month) noexcept {
    constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30,
                                             31, 31, 30, 31, 30, 31};
    if (month == 2U && is_leap_year(year)) {
        return 29U;
    }
    return days[month - 1U];
}

// Howard Hinnant's civil calendar algorithms, with the Unix epoch as day zero.
constexpr std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2U ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const int adjusted_month = static_cast<int>(month) + (month > 2U ? -3 : 9);
    const unsigned day_of_year =
        (153U * static_cast<unsigned>(adjusted_month) + 2U) / 5U + day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146'097 +
           static_cast<std::int64_t>(day_of_era) - 719'468;
}

constexpr std::tuple<int, unsigned, unsigned> civil_from_days(std::int64_t days) noexcept {
    days += 719'468;
    const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146'097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1'460U + day_of_era / 36'524U -
         day_of_era / 146'096U) /
        365U;
    int year = static_cast<int>(year_of_era) + static_cast<int>(era * 400);
    const unsigned day_of_year =
        day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    const unsigned day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    const int month_value = static_cast<int>(month_prime) + (month_prime < 10U ? 3 : -9);
    const unsigned month = static_cast<unsigned>(month_value);
    year += month <= 2U ? 1 : 0;
    return {year, month, day};
}

bool parse_digits(std::string_view text, std::size_t offset, std::size_t count,
                  unsigned& value) noexcept {
    value = 0U;
    for (std::size_t index = 0; index < count; ++index) {
        const char character = text[offset + index];
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * 10U + static_cast<unsigned>(character - '0');
    }
    return true;
}

std::pair<std::int64_t, std::int64_t> floor_seconds_and_micros(
    std::int64_t unix_microseconds) noexcept {
    std::int64_t seconds = unix_microseconds / kMicrosecondsPerSecond;
    std::int64_t micros = unix_microseconds % kMicrosecondsPerSecond;
    if (micros < 0) {
        micros += kMicrosecondsPerSecond;
        --seconds;
    }
    return {seconds, micros};
}

}  // namespace

Timestamp Timestamp::now() {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return Timestamp(micros.count());
}

Timestamp Timestamp::from_unix_microseconds(std::int64_t value) noexcept {
    return Timestamp(value);
}

Result<Timestamp> Timestamp::parse(std::string_view text) {
    if (text.size() != 27U || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != '.' || text[26] != 'Z') {
        return Result<Timestamp>::failure(
            "timestamp must use YYYY-MM-DDTHH:MM:SS.ffffffZ");
    }

    unsigned year = 0U;
    unsigned month = 0U;
    unsigned day = 0U;
    unsigned hour = 0U;
    unsigned minute = 0U;
    unsigned second = 0U;
    unsigned microsecond = 0U;
    if (!parse_digits(text, 0U, 4U, year) || !parse_digits(text, 5U, 2U, month) ||
        !parse_digits(text, 8U, 2U, day) || !parse_digits(text, 11U, 2U, hour) ||
        !parse_digits(text, 14U, 2U, minute) || !parse_digits(text, 17U, 2U, second) ||
        !parse_digits(text, 20U, 6U, microsecond)) {
        return Result<Timestamp>::failure("timestamp contains a non-decimal component");
    }

    const int signed_year = static_cast<int>(year);
    if (month < 1U || month > 12U || day < 1U || day > days_in_month(signed_year, month) ||
        hour > 23U || minute > 59U || second > 59U) {
        return Result<Timestamp>::failure("timestamp contains an out-of-range component");
    }

    const std::int64_t days = days_from_civil(signed_year, month, day);
    const std::int64_t seconds =
        days * kSecondsPerDay + static_cast<std::int64_t>(hour) * 3'600 +
        static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    if (seconds >
            (std::numeric_limits<std::int64_t>::max() -
             static_cast<std::int64_t>(microsecond)) /
                kMicrosecondsPerSecond ||
        seconds < std::numeric_limits<std::int64_t>::min() / kMicrosecondsPerSecond) {
        return Result<Timestamp>::failure("timestamp is outside the supported integer range");
    }
    return Result<Timestamp>::success(Timestamp(
        seconds * kMicrosecondsPerSecond + static_cast<std::int64_t>(microsecond)));
}

std::int64_t Timestamp::unix_microseconds() const noexcept {
    return unix_microseconds_;
}

std::string Timestamp::to_string() const {
    const auto [seconds, micros] = floor_seconds_and_micros(unix_microseconds_);
    std::int64_t days = seconds / kSecondsPerDay;
    std::int64_t seconds_of_day = seconds % kSecondsPerDay;
    if (seconds_of_day < 0) {
        seconds_of_day += kSecondsPerDay;
        --days;
    }

    const auto [year, month, day] = civil_from_days(days);
    if (year < 0 || year > 9'999) {
        throw std::out_of_range("timestamp year cannot be represented with four digits");
    }
    const auto hour = static_cast<unsigned>(seconds_of_day / 3'600);
    const auto minute = static_cast<unsigned>((seconds_of_day % 3'600) / 60);
    const auto second = static_cast<unsigned>(seconds_of_day % 60);

    std::array<char, 28> buffer{};
    const int written = std::snprintf(
        buffer.data(), buffer.size(), "%04d-%02u-%02uT%02u:%02u:%02u.%06lldZ", year,
        month, day, hour, minute, second, static_cast<long long>(micros));
    if (written != 27) {
        throw std::runtime_error("failed to format timestamp");
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

}  // namespace ts::common
