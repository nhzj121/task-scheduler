#include "ts/domain/ids.h"

#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace ts::domain {
namespace detail {

bool is_canonical_uuid(std::string_view text) noexcept {
    if (text.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (text[index] != '-') {
                return false;
            }
            continue;
        }
        const char character = text[index];
        const bool decimal = character >= '0' && character <= '9';
        const bool lowercase_hex = character >= 'a' && character <= 'f';
        if (!decimal && !lowercase_hex) {
            return false;
        }
    }
    return true;
}

std::string generate_uuid_v4() {
    std::random_device random;
    std::uniform_int_distribution<unsigned> distribution(0U, 255U);
    std::array<unsigned, 16> bytes{};
    for (auto& byte : bytes) {
        byte = distribution(random);
    }
    bytes[6] = (bytes[6] & 0x0FU) | 0x40U;
    bytes[8] = (bytes[8] & 0x3FU) | 0x80U;

    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            stream << '-';
        }
        stream << std::setw(2) << bytes[index];
    }
    return stream.str();
}

}  // namespace detail

common::Result<WorkerId> WorkerId::parse(std::string value) {
    if (value.empty() || value.size() > kMaximumBytes) {
        return common::Result<WorkerId>::failure(
            "worker_id must contain 1..128 ASCII bytes");
    }
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0U || character > 0x7FU) {
            return common::Result<WorkerId>::failure(
                "worker_id must contain only non-NUL ASCII bytes");
        }
    }
    return common::Result<WorkerId>::success(WorkerId(std::move(value)));
}

const std::string& WorkerId::value() const noexcept {
    return value_;
}

common::Result<AttemptNumber> AttemptNumber::create(std::uint32_t value) {
    if (value == 0U) {
        return common::Result<AttemptNumber>::failure("attempt number must start at 1");
    }
    return common::Result<AttemptNumber>::success(AttemptNumber(value));
}

std::uint32_t AttemptNumber::value() const noexcept {
    return value_;
}

common::Result<AttemptNumber> AttemptNumber::next() const {
    if (value_ == std::numeric_limits<std::uint32_t>::max()) {
        return common::Result<AttemptNumber>::failure("attempt number overflow");
    }
    return common::Result<AttemptNumber>::success(AttemptNumber(value_ + 1U));
}

}  // namespace ts::domain
