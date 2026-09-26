#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

#include "ts/common/result.h"
#include "ts/common/unique_fd.h"

namespace ts::io {

using EventMask = std::uint32_t;

inline constexpr EventMask kReadable = EPOLLIN;
inline constexpr EventMask kWritable = EPOLLOUT;
inline constexpr EventMask kReadHangup = EPOLLRDHUP;

struct RegistrationToken {
    int fd;
    std::uint64_t generation;
    std::uint64_t key;
    std::uint32_t handler_kind;
    std::uint64_t owner_id;

    friend bool operator==(const RegistrationToken& lhs,
                           const RegistrationToken& rhs) noexcept {
        return lhs.fd == rhs.fd && lhs.generation == rhs.generation &&
               lhs.key == rhs.key && lhs.handler_kind == rhs.handler_kind &&
               lhs.owner_id == rhs.owner_id;
    }

    friend bool operator!=(const RegistrationToken& lhs,
                           const RegistrationToken& rhs) noexcept {
        return !(lhs == rhs);
    }
};

struct RegistrationMetadata {
    std::uint32_t handler_kind{0U};
    std::uint64_t owner_id{0U};
};

class EventLoopTestPeer;

class EventLoop final {
public:
    using Callback = std::function<void(const RegistrationToken&, EventMask)>;
    using DeferredAction = std::function<void()>;

    static common::Result<std::unique_ptr<EventLoop>> create();

    ~EventLoop() = default;
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // All registration mutation and polling are owner-thread operations. Registration is
    // non-owning: the caller must remove a token successfully before closing its source fd.
    [[nodiscard]] common::Result<RegistrationToken> add(
        int fd, EventMask interest, Callback callback,
        RegistrationMetadata metadata = {});
    [[nodiscard]] common::Result<void> modify(const RegistrationToken& token,
                                               EventMask interest);
    [[nodiscard]] common::Result<void> remove(const RegistrationToken& token);

    [[nodiscard]] common::Result<std::size_t> poll_once(int timeout_ms);

    // Actions queued while dispatching run after the entire current epoll batch and after the
    // copied callback objects have left scope. Actions queued by a deferred action wait for the
    // next poll safe point.
    [[nodiscard]] common::Result<void> defer(DeferredAction action);

    [[nodiscard]] bool is_current(const RegistrationToken& token) const noexcept;
    [[nodiscard]] std::size_t registration_count() const noexcept;
    [[nodiscard]] int epoll_fd_for_observation() const noexcept;

private:
    struct Registration {
        RegistrationToken token;
        EventMask interest;
        Callback callback;
    };

    EventLoop(common::UniqueFd epoll_fd, std::thread::id owner_thread) noexcept;

    [[nodiscard]] common::Result<void> require_owner_thread() const;
    [[nodiscard]] common::Result<std::size_t> dispatch_batch(const epoll_event* events,
                                                              int count);
    [[nodiscard]] common::Result<void> run_deferred_actions();
    [[nodiscard]] static common::Result<void> validate_interest(EventMask interest);

    friend class EventLoopTestPeer;

    common::UniqueFd epoll_fd_;
    std::thread::id owner_thread_;
    std::uint64_t next_key_{1U};
    std::unordered_map<std::uint64_t, Registration> registrations_;
    std::unordered_map<int, std::uint64_t> keys_by_fd_;
    std::vector<DeferredAction> deferred_actions_;
};

}  // namespace ts::io
