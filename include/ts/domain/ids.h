#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

#include "ts/common/result.h"

namespace ts::domain {
namespace detail {

[[nodiscard]] bool is_canonical_uuid(std::string_view text) noexcept;
[[nodiscard]] std::string generate_uuid_v4();

}  // namespace detail

template <typename Tag>
class UuidId {
public:
    static common::Result<UuidId> parse(std::string text) {
        if (!detail::is_canonical_uuid(text)) {
            return common::Result<UuidId>::failure(
                "ID must be a canonical lowercase UUID");
        }
        return common::Result<UuidId>::success(UuidId(std::move(text)));
    }

    static UuidId generate() {
        return UuidId(detail::generate_uuid_v4());
    }

    [[nodiscard]] const std::string& value() const noexcept {
        return value_;
    }

    friend bool operator==(const UuidId& lhs, const UuidId& rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator<(const UuidId& lhs, const UuidId& rhs) noexcept {
        return lhs.value_ < rhs.value_;
    }

private:
    explicit UuidId(std::string value) : value_(std::move(value)) {}

    std::string value_;
};

struct TaskIdTag;
struct AttemptIdTag;
struct SessionIdTag;

using TaskId = UuidId<TaskIdTag>;
using AttemptId = UuidId<AttemptIdTag>;
using SessionId = UuidId<SessionIdTag>;

class WorkerId {
public:
    static constexpr std::size_t kMaximumBytes = 128U;

    static common::Result<WorkerId> parse(std::string value);

    [[nodiscard]] const std::string& value() const noexcept;

    friend bool operator==(const WorkerId& lhs, const WorkerId& rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator<(const WorkerId& lhs, const WorkerId& rhs) noexcept {
        return lhs.value_ < rhs.value_;
    }

private:
    explicit WorkerId(std::string value) : value_(std::move(value)) {}

    std::string value_;
};

class AttemptNumber {
public:
    static common::Result<AttemptNumber> create(std::uint32_t value);

    [[nodiscard]] std::uint32_t value() const noexcept;
    [[nodiscard]] common::Result<AttemptNumber> next() const;

    friend bool operator==(AttemptNumber lhs, AttemptNumber rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

private:
    explicit AttemptNumber(std::uint32_t value) noexcept : value_(value) {}

    std::uint32_t value_;
};

struct UuidIdHash {
    template <typename Tag>
    std::size_t operator()(const UuidId<Tag>& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};

}  // namespace ts::domain
