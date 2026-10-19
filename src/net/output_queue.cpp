#include "ts/net/output_queue.h"

#include <algorithm>
#include <stdexcept>

namespace ts::net {
OutputQueue::OutputQueue(OutputLimits limits) : limits_(limits) {
    if (limits.control.bytes == 0 || limits.control.slots == 0 || limits.log.bytes == 0 || limits.log.slots == 0)
        throw std::invalid_argument("output queue limits must be positive");
}
common::Result<void> OutputQueue::enqueue(Outbound frame) {
    if (frame.bytes.empty()) return common::Result<void>::failure("empty outbound frame");
    if ((frame.emission && (frame.queue != QueueClass::Log || frame.emission->ordinal == 0)) ||
        (frame.dependency && frame.queue != QueueClass::Control))
        return common::Result<void>::failure("invalid generic ordering metadata");
    if (frame.emission) {
        const auto precedes = [&](const Outbound& prior) {
            return prior.emission && prior.emission->group == frame.emission->group &&
                   prior.emission->ordinal >= frame.emission->ordinal;
        };
        if (std::any_of(logs_.begin(), logs_.end(), precedes) || (current_ && precedes(current_->frame)))
            return common::Result<void>::failure("nonmonotonic queued group ordinal");
    }
    auto& owned_bytes = frame.queue == QueueClass::Control ? control_bytes_ : log_bytes_;
    auto& owned_slots = frame.queue == QueueClass::Control ? control_slots_ : log_slots_;
    const auto limit = frame.queue == QueueClass::Control ? limits_.control : limits_.log;
    if (frame.bytes.size() > limit.bytes - owned_bytes || owned_slots == limit.slots)
        return common::Result<void>::failure("bounded output queue full");
    const auto size = frame.bytes.size();
    auto& queue = frame.queue == QueueClass::Control ? control_ : logs_;
    queue.push_back(std::move(frame)); owned_bytes += size; ++owned_slots;
    return common::Result<void>::success();
}
bool OutputQueue::eligible(const Outbound& frame) const {
    if (!frame.dependency) return true;
    const auto blocks = [&](const Outbound& prior) {
        return prior.emission && prior.emission->group == frame.dependency->group &&
               prior.emission->ordinal <= frame.dependency->ordinal;
    };
    return !std::any_of(logs_.begin(), logs_.end(), blocks);
}
std::string_view OutputQueue::front() {
    if (!current_) {
        auto chosen = std::find_if(control_.begin(), control_.end(), [&](const Outbound& frame) { return eligible(frame); });
        if (chosen != control_.end()) { current_ = Current{std::move(*chosen),0}; control_.erase(chosen); }
        else if (!logs_.empty()) {
            auto log = logs_.begin();
            // Prefer logs needed by the first blocked dependency; unrelated groups are not a
            // global barrier. Same-group relative log order is preserved by monotonic ordinals.
            for (const auto& control : control_) {
                if (!control.dependency) continue;
                auto needed = std::find_if(logs_.begin(), logs_.end(), [&](const Outbound& entry) {
                    return entry.emission && entry.emission->group == control.dependency->group &&
                           entry.emission->ordinal <= control.dependency->ordinal;
                });
                if (needed != logs_.end()) { log = needed; break; }
            }
            current_ = Current{std::move(*log),0}; logs_.erase(log);
        }
    }
    if (!current_) return {};
    return std::string_view(current_->frame.bytes).substr(current_->offset);
}
common::Result<void> OutputQueue::advance(std::size_t count) {
    if (!current_ || count == 0 || count > current_->frame.bytes.size() - current_->offset)
        return common::Result<void>::failure("invalid output progress");
    current_->offset += count;
    if (current_->offset == current_->frame.bytes.size()) {
        auto& owned_bytes = current_->frame.queue == QueueClass::Control ? control_bytes_ : log_bytes_;
        auto& owned_slots = current_->frame.queue == QueueClass::Control ? control_slots_ : log_slots_;
        owned_bytes -= current_->frame.bytes.size(); --owned_slots; current_.reset();
    }
    return common::Result<void>::success();
}
void OutputQueue::retry_boundary() {
    if (!current_ || current_->offset != 0) return;
    auto& queue = current_->frame.queue == QueueClass::Control ? control_ : logs_;
    queue.push_front(std::move(current_->frame)); current_.reset();
}
std::size_t OutputQueue::bytes(QueueClass queue) const noexcept { return queue == QueueClass::Control ? control_bytes_ : log_bytes_; }
std::size_t OutputQueue::unsent_bytes(QueueClass queue) const noexcept {
    return bytes(queue) - ((current_ && current_->frame.queue == queue) ? current_->offset : 0U);
}
std::size_t OutputQueue::slots(QueueClass queue) const noexcept { return queue == QueueClass::Control ? control_slots_ : log_slots_; }
bool OutputQueue::empty() const noexcept { return control_slots_ == 0 && log_slots_ == 0; }
void OutputQueue::clear() noexcept {
    current_.reset(); control_.clear(); logs_.clear(); control_bytes_ = 0; log_bytes_ = 0; control_slots_ = 0; log_slots_ = 0;
}
} // namespace ts::net
