#include "ts/io/event_loop.h"

#include <array>
#include <cerrno>
#include <exception>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace ts::io {
namespace {

constexpr std::size_t kMaximumEventsPerBatch = 64U;

std::string errno_message(const char* operation, int error_number) {
    return std::string(operation) + " failed: " +
           std::error_code(error_number, std::generic_category()).message();
}

int epoll_control(int epoll_fd, int operation, int fd, epoll_event* event) noexcept {
    int result = -1;
    do {
        result = ::epoll_ctl(epoll_fd, operation, fd, event);
    } while (result == -1 && errno == EINTR);
    return result;
}

}  // namespace

common::Result<std::unique_ptr<EventLoop>> EventLoop::create() {
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd == -1) {
        return common::Result<std::unique_ptr<EventLoop>>::failure(
            errno_message("epoll_create1", errno));
    }
    return common::Result<std::unique_ptr<EventLoop>>::success(
        std::unique_ptr<EventLoop>(
            new EventLoop(common::UniqueFd(fd), std::this_thread::get_id())));
}

EventLoop::EventLoop(common::UniqueFd epoll_fd, std::thread::id owner_thread) noexcept
    : epoll_fd_(std::move(epoll_fd)), owner_thread_(owner_thread) {}

common::Result<RegistrationToken> EventLoop::add(int fd, EventMask interest,
                                                  Callback callback,
                                                  RegistrationMetadata metadata) {
    if (const auto owner = require_owner_thread(); !owner) {
        return common::Result<RegistrationToken>::failure(owner.error().message);
    }
    if (fd < 0) {
        return common::Result<RegistrationToken>::failure(
            "cannot register an invalid file descriptor");
    }
    if (!callback) {
        return common::Result<RegistrationToken>::failure(
            "cannot register an empty callback");
    }
    if (const auto valid = validate_interest(interest); !valid) {
        return common::Result<RegistrationToken>::failure(valid.error().message);
    }
    if (keys_by_fd_.find(fd) != keys_by_fd_.end()) {
        return common::Result<RegistrationToken>::failure(
            "file descriptor is already registered");
    }
    if (next_key_ == 0U || next_key_ == std::numeric_limits<std::uint64_t>::max()) {
        return common::Result<RegistrationToken>::failure(
            "EventLoop registration generation space is exhausted");
    }

    const std::uint64_t key = next_key_++;
    const RegistrationToken token{fd, key, key, metadata.handler_kind, metadata.owner_id};
    Registration registration{token, interest, std::move(callback)};
    const auto [iterator, inserted] = registrations_.emplace(key, std::move(registration));
    if (!inserted) {
        return common::Result<RegistrationToken>::failure(
            "EventLoop registration key collision");
    }
    const auto [fd_iterator, fd_inserted] = keys_by_fd_.emplace(fd, key);
    if (!fd_inserted) {
        registrations_.erase(iterator);
        return common::Result<RegistrationToken>::failure(
            "file descriptor registration collision");
    }

    epoll_event event{};
    event.events = interest;
    event.data.u64 = key;
    if (epoll_control(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event) == -1) {
        const int saved_errno = errno;
        keys_by_fd_.erase(fd_iterator);
        registrations_.erase(iterator);
        return common::Result<RegistrationToken>::failure(
            errno_message("epoll_ctl(ADD)", saved_errno));
    }
    return common::Result<RegistrationToken>::success(token);
}

common::Result<void> EventLoop::modify(const RegistrationToken& token,
                                        EventMask interest) {
    if (const auto owner = require_owner_thread(); !owner) {
        return owner;
    }
    if (const auto valid = validate_interest(interest); !valid) {
        return valid;
    }
    const auto iterator = registrations_.find(token.key);
    if (iterator == registrations_.end() || iterator->second.token != token) {
        return common::Result<void>::failure(
            "cannot modify a stale or unknown registration token");
    }

    epoll_event event{};
    event.events = interest;
    event.data.u64 = token.key;
    if (epoll_control(epoll_fd_.get(), EPOLL_CTL_MOD, token.fd, &event) == -1) {
        return common::Result<void>::failure(errno_message("epoll_ctl(MOD)", errno));
    }
    iterator->second.interest = interest;
    return common::Result<void>::success();
}

common::Result<void> EventLoop::remove(const RegistrationToken& token) {
    if (const auto owner = require_owner_thread(); !owner) {
        return owner;
    }
    const auto iterator = registrations_.find(token.key);
    if (iterator == registrations_.end() || iterator->second.token != token) {
        return common::Result<void>::failure(
            "cannot remove a stale or unknown registration token");
    }
    const auto fd_iterator = keys_by_fd_.find(token.fd);
    if (fd_iterator == keys_by_fd_.end() || fd_iterator->second != token.key) {
        return common::Result<void>::failure(
            "EventLoop registration tables are inconsistent");
    }

    if (epoll_control(epoll_fd_.get(), EPOLL_CTL_DEL, token.fd, nullptr) == -1) {
        return common::Result<void>::failure(errno_message("epoll_ctl(DEL)", errno));
    }
    keys_by_fd_.erase(fd_iterator);
    registrations_.erase(iterator);
    return common::Result<void>::success();
}

common::Result<std::size_t> EventLoop::poll_once(int timeout_ms) {
    if (const auto owner = require_owner_thread(); !owner) {
        return common::Result<std::size_t>::failure(owner.error().message);
    }
    if (timeout_ms < -1) {
        return common::Result<std::size_t>::failure(
            "epoll timeout must be -1 or nonnegative");
    }

    std::array<epoll_event, kMaximumEventsPerBatch> events{};
    int count = -1;
    do {
        count = ::epoll_wait(epoll_fd_.get(), events.data(),
                             static_cast<int>(events.size()), timeout_ms);
    } while (count == -1 && errno == EINTR);
    if (count == -1) {
        return common::Result<std::size_t>::failure(errno_message("epoll_wait", errno));
    }
    return dispatch_batch(events.data(), count);
}

common::Result<void> EventLoop::defer(DeferredAction action) {
    if (const auto owner = require_owner_thread(); !owner) {
        return owner;
    }
    if (!action) {
        return common::Result<void>::failure("cannot defer an empty action");
    }
    deferred_actions_.push_back(std::move(action));
    return common::Result<void>::success();
}

bool EventLoop::is_current(const RegistrationToken& token) const noexcept {
    const auto iterator = registrations_.find(token.key);
    return iterator != registrations_.end() && iterator->second.token == token;
}

std::size_t EventLoop::registration_count() const noexcept {
    return registrations_.size();
}

int EventLoop::epoll_fd_for_observation() const noexcept {
    return epoll_fd_.get();
}

common::Result<void> EventLoop::require_owner_thread() const {
    if (std::this_thread::get_id() != owner_thread_) {
        return common::Result<void>::failure(
            "EventLoop operation attempted from a non-owner thread");
    }
    return common::Result<void>::success();
}

common::Result<std::size_t> EventLoop::dispatch_batch(const epoll_event* events,
                                                       int count) {
    std::size_t dispatched = 0U;
    for (int index = 0; index < count; ++index) {
        const auto iterator = registrations_.find(events[index].data.u64);
        if (iterator == registrations_.end()) {
            continue;
        }

        const RegistrationToken token = iterator->second.token;
        std::string callback_failure;
        {
            // The copy keeps a self-removing callback alive through its invocation. Its scope
            // ends before deferred destruction is allowed to run.
            Callback callback = iterator->second.callback;
            try {
                callback(token, events[index].events);
            } catch (const std::exception& exception) {
                callback_failure =
                    "EventLoop callback threw: " + std::string(exception.what());
            } catch (...) {
                callback_failure =
                    "EventLoop callback threw a non-standard exception";
            }
        }
        if (!callback_failure.empty()) {
            const auto deferred = run_deferred_actions();
            if (!deferred) {
                return common::Result<std::size_t>::failure(
                    "callback and deferred action failed: " + deferred.error().message);
            }
            return common::Result<std::size_t>::failure(std::move(callback_failure));
        }
        ++dispatched;
    }

    if (const auto deferred = run_deferred_actions(); !deferred) {
        return common::Result<std::size_t>::failure(deferred.error().message);
    }
    return common::Result<std::size_t>::success(dispatched);
}

common::Result<void> EventLoop::run_deferred_actions() {
    std::vector<DeferredAction> actions;
    actions.swap(deferred_actions_);
    try {
        for (auto& action : actions) {
            action();
        }
    } catch (const std::exception& exception) {
        return common::Result<void>::failure(
            "deferred EventLoop action threw: " + std::string(exception.what()));
    } catch (...) {
        return common::Result<void>::failure(
            "deferred EventLoop action threw a non-standard exception");
    }
    return common::Result<void>::success();
}

common::Result<void> EventLoop::validate_interest(EventMask interest) {
    EventMask forbidden = EPOLLET | EPOLLONESHOT;
#ifdef EPOLLEXCLUSIVE
    forbidden |= EPOLLEXCLUSIVE;
#endif
    if ((interest & forbidden) != 0U) {
        return common::Result<void>::failure(
            "EventLoop supports Level Triggered registrations only");
    }
    return common::Result<void>::success();
}

}  // namespace ts::io
