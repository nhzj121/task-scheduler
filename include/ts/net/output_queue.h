#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include "ts/common/result.h"

namespace ts::net {
enum class QueueClass { Control, Log };
// Opaque caller-owned identity and monotonically increasing emission ordinal (starts at 1).
// Producers supply one group across all streams and capture the last generated ordinal in a
// control dependency. This layer neither interprets bytes nor owns application lifecycle state.
struct OrderPoint { std::uint64_t group, ordinal; };
struct Outbound {
    std::string bytes;
    QueueClass queue{QueueClass::Control};
    std::optional<OrderPoint> emission{};
    std::optional<OrderPoint> dependency{};
};
struct QueueLimit { std::size_t bytes, slots; };
struct OutputLimits {
    QueueLimit control{8U * 1024U * 1024U, 128};
    QueueLimit log{8U * 1024U * 1024U, 128};
};
class OutputQueue final {
public:
    explicit OutputQueue(OutputLimits limits = {});
    [[nodiscard]] common::Result<void> enqueue(Outbound frame);
    // Borrow valid only until enqueue/advance/clear. A selected partial frame stays pinned.
    [[nodiscard]] std::string_view front();
    [[nodiscard]] common::Result<void> advance(std::size_t bytes);
    [[nodiscard]] std::size_t bytes(QueueClass queue) const noexcept;
    [[nodiscard]] std::size_t unsent_bytes(QueueClass queue) const noexcept;
    [[nodiscard]] std::size_t slots(QueueClass queue) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    void clear() noexcept;
private:
    struct Current { Outbound frame; std::size_t offset{0}; };
    [[nodiscard]] bool eligible(const Outbound& frame) const;
    OutputLimits limits_;
    std::deque<Outbound> control_, logs_;
    std::optional<Current> current_;
    std::size_t control_bytes_{0}, log_bytes_{0}, control_slots_{0}, log_slots_{0};
};
} // namespace ts::net
