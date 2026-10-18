#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ts::common {

enum class QueuePushResult {
    Pushed,
    Full,
    Stopped,
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0U) {
            throw std::invalid_argument("bounded queue capacity must be positive");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;

    [[nodiscard]] QueuePushResult try_push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return QueuePushResult::Stopped;
        }
        if (queue_.size() == capacity_) {
            return QueuePushResult::Full;
        }
        queue_.push_back(std::move(value));
        not_empty_.notify_one();
        return QueuePushResult::Pushed;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

    // Drains values accepted before stop. Returns nullopt only after stop and drain.
    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }
 
    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        not_empty_.notify_all();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] bool stopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> queue_;
    bool stopped_{false};
};

}  // namespace ts::common
