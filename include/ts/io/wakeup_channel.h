#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "ts/common/bounded_queue.h"
#include "ts/common/result.h"
#include "ts/io/event_fd.h"

namespace ts::io {

template <typename T>
class WakeupChannel final {
public:
    static common::Result<std::unique_ptr<WakeupChannel>> create(std::size_t capacity) {
        if (capacity == 0U) {
            return common::Result<std::unique_ptr<WakeupChannel>>::failure(
                "wakeup channel capacity must be positive");
        }
        auto event_fd = EventFd::create();
        if (!event_fd) {
            return common::Result<std::unique_ptr<WakeupChannel>>::failure(
                event_fd.error().message);
        }
        return common::Result<std::unique_ptr<WakeupChannel>>::success(
            std::unique_ptr<WakeupChannel>(
                new WakeupChannel(capacity, std::move(event_fd).value())));
    }

    WakeupChannel(const WakeupChannel&) = delete;
    WakeupChannel& operator=(const WakeupChannel&) = delete;
    WakeupChannel(WakeupChannel&&) = delete;
    WakeupChannel& operator=(WakeupChannel&&) = delete;

    [[nodiscard]] int fd() const noexcept {
        return event_fd_.fd();
    }

    [[nodiscard]] common::Result<common::QueuePushResult> try_post(T value) {
        const auto push_result = queue_.try_push(std::move(value));
        if (push_result != common::QueuePushResult::Pushed) {
            return common::Result<common::QueuePushResult>::success(push_result);
        }
        const auto notification = event_fd_.notify();
        if (!notification) {
            return common::Result<common::QueuePushResult>::failure(
                notification.error().message);
        }
        return common::Result<common::QueuePushResult>::success(push_result);
    }

    [[nodiscard]] common::Result<std::vector<T>> drain() {
        const auto wakeups = event_fd_.drain();
        if (!wakeups) {
            return common::Result<std::vector<T>>::failure(wakeups.error().message);
        }

        std::vector<T> values;
        // EventLoop consumers deliberately use only the non-blocking queue operation.
        while (auto value = queue_.try_pop()) {
            values.push_back(std::move(*value));
        }
        return common::Result<std::vector<T>>::success(std::move(values));
    }

    [[nodiscard]] common::Result<void> stop() {
        queue_.stop();
        return event_fd_.notify();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return queue_.capacity();
    }

    [[nodiscard]] std::size_t size() const {
        return queue_.size();
    }

    [[nodiscard]] bool stopped() const {
        return queue_.stopped();
    }

private:
    WakeupChannel(std::size_t capacity, EventFd event_fd)
        : queue_(capacity), event_fd_(std::move(event_fd)) {}

    common::BoundedQueue<T> queue_;
    EventFd event_fd_;
};

}  // namespace ts::io
