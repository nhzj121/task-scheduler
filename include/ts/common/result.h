#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace ts::common {

struct Error {
    std::string message;
};

template <typename T>
class [[nodiscard]] Result {
public:
    static Result success(T value) {
        return Result(std::move(value));
    }

    static Result failure(std::string message) {
        return Result(Error{std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const T& value() const& {
        if (!has_value()) {
            throw std::logic_error("attempted to read value from failed Result");
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T& value() & {
        if (!has_value()) {
            throw std::logic_error("attempted to read value from failed Result");
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        if (!has_value()) {
            throw std::logic_error("attempted to read value from failed Result");
        }
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const Error& error() const& {
        if (has_value()) {
            throw std::logic_error("attempted to read error from successful Result");
        }
        return std::get<Error>(storage_);
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    static Result success() {
        return Result(std::monostate{});
    }

    static Result failure(std::string message) {
        return Result(Error{std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<std::monostate>(storage_);
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const {
        if (!has_value()) {
            throw std::logic_error("attempted to read value from failed Result");
        }
    }

    [[nodiscard]] const Error& error() const& {
        if (has_value()) {
            throw std::logic_error("attempted to read error from successful Result");
        }
        return std::get<Error>(storage_);
    }

private:
    explicit Result(std::monostate value) : storage_(value) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<std::monostate, Error> storage_;
};

}  // namespace ts::common
