#include "ts/io/event_loop.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "ts/common/unique_fd.h"

namespace ts::io {

class EventLoopTestPeer {
public:
    static common::Result<std::size_t> dispatch(
        EventLoop& loop,
        const std::vector<std::pair<std::uint64_t, EventMask>>& ready) {
        std::vector<epoll_event> events(ready.size());
        for (std::size_t index = 0U; index < ready.size(); ++index) {
            events[index].events = ready[index].second;
            events[index].data.u64 = ready[index].first;
        }
        return loop.dispatch_batch(events.data(), static_cast<int>(events.size()));
    }

    static void invalidate_epoll_fd(EventLoop& loop) {
        loop.epoll_fd_.reset();
    }
};

namespace {

struct Pipe {
    common::UniqueFd read_end;
    common::UniqueFd write_end;
};

struct CopyThrowsWhenEnabled {
    explicit CopyThrowsWhenEnabled(std::shared_ptr<bool> enabled_flag)
        : enabled(std::move(enabled_flag)) {}

    CopyThrowsWhenEnabled(const CopyThrowsWhenEnabled& other)
        : enabled(other.enabled) {
        if (*enabled) {
            throw std::runtime_error("callback copy failed");
        }
    }

    CopyThrowsWhenEnabled(CopyThrowsWhenEnabled&&) noexcept = default;
    CopyThrowsWhenEnabled& operator=(const CopyThrowsWhenEnabled&) = default;
    CopyThrowsWhenEnabled& operator=(CopyThrowsWhenEnabled&&) noexcept = default;

    void operator()(const RegistrationToken&, EventMask) const {}

    std::shared_ptr<bool> enabled;
};

Pipe make_pipe() {
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_NONBLOCK | O_CLOEXEC) == -1) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
    return Pipe{common::UniqueFd(fds[0]), common::UniqueFd(fds[1])};
}

void write_byte(int fd, char value = 'x') {
    for (;;) {
        const ssize_t written = ::write(fd, &value, sizeof(value));
        if (written == 1) {
            return;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "write");
    }
}

char read_byte(int fd) {
    char value = '\0';
    for (;;) {
        const ssize_t count = ::read(fd, &value, sizeof(value));
        if (count == 1) {
            return value;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "read");
    }
}

std::unique_ptr<EventLoop> make_loop() {
    auto result = EventLoop::create();
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result).value();
}

TEST(EventLoop, EpollDescriptorIsCloseOnExec) {
    auto loop = make_loop();
    const int flags = ::fcntl(loop->epoll_fd_for_observation(), F_GETFD);
    ASSERT_NE(flags, -1);
    EXPECT_NE(flags & FD_CLOEXEC, 0);
}

TEST(EventLoop, AddModifyRemoveAndStaleOperationsAreExplicit) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    std::vector<char> observed;
    auto added = loop->add(
        pipe.read_end.get(), kReadable,
        [&observed, &pipe](const RegistrationToken&, EventMask events) {
            EXPECT_NE(events & kReadable, 0U);
            observed.push_back(read_byte(pipe.read_end.get()));
        },
        RegistrationMetadata{7U, 42U});
    ASSERT_TRUE(added);
    const RegistrationToken token = added.value();
    EXPECT_EQ(token.handler_kind, 7U);
    EXPECT_EQ(token.owner_id, 42U);
    EXPECT_TRUE(loop->is_current(token));
    EXPECT_EQ(loop->registration_count(), 1U);

    EXPECT_FALSE(loop->add(pipe.read_end.get(), kReadable,
                           [](const RegistrationToken&, EventMask) {}));
    write_byte(pipe.write_end.get(), 'a');
    const auto dispatched = loop->poll_once(1'000);
    ASSERT_TRUE(dispatched);
    EXPECT_EQ(dispatched.value(), 1U);
    EXPECT_EQ(observed, (std::vector<char>{'a'}));

    ASSERT_TRUE(loop->modify(token, 0U));
    write_byte(pipe.write_end.get(), 'b');
    const auto disabled = loop->poll_once(0);
    ASSERT_TRUE(disabled);
    EXPECT_EQ(disabled.value(), 0U);

    ASSERT_TRUE(loop->modify(token, kReadable));
    const auto enabled = loop->poll_once(1'000);
    ASSERT_TRUE(enabled);
    EXPECT_EQ(enabled.value(), 1U);
    EXPECT_EQ(observed, (std::vector<char>{'a', 'b'}));

    ASSERT_TRUE(loop->remove(token));
    EXPECT_FALSE(loop->is_current(token));
    EXPECT_EQ(loop->registration_count(), 0U);
    EXPECT_FALSE(loop->modify(token, kReadable));
    EXPECT_FALSE(loop->remove(token));
    const auto removed = loop->poll_once(0);
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed.value(), 0U);
}

TEST(EventLoop, GenuineDeleteFailureRemainsExplicitAndKeepsLogicalRegistration) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    auto added = loop->add(
        pipe.read_end.get(), kReadable,
        [](const RegistrationToken&, EventMask) {});
    ASSERT_TRUE(added);

    EventLoopTestPeer::invalidate_epoll_fd(*loop);
    const auto removed = loop->remove(added.value());
    ASSERT_FALSE(removed);
    EXPECT_NE(removed.error().message.find("epoll_ctl(DEL)"), std::string::npos);
    EXPECT_TRUE(loop->is_current(added.value()));
    EXPECT_EQ(loop->registration_count(), 1U);
}

TEST(EventLoop, AlreadyAbsentKernelRegistrationIsRemovedLogicallyAndFdCanReregister) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    int callbacks = 0;
    auto old_registration = loop->add(
        pipe.read_end.get(), kReadable,
        [&callbacks](const RegistrationToken&, EventMask) { ++callbacks; });
    ASSERT_TRUE(old_registration);

    ASSERT_EQ(::epoll_ctl(loop->epoll_fd_for_observation(), EPOLL_CTL_DEL,
                          pipe.read_end.get(), nullptr),
              0);
    ASSERT_TRUE(loop->remove(old_registration.value()));
    EXPECT_FALSE(loop->is_current(old_registration.value()));
    EXPECT_EQ(loop->registration_count(), 0U);

    auto new_registration = loop->add(
        pipe.read_end.get(), kReadable,
        [&callbacks](const RegistrationToken&, EventMask) { ++callbacks; });
    ASSERT_TRUE(new_registration);
    EXPECT_NE(old_registration.value().generation,
              new_registration.value().generation);

    const auto stale = EventLoopTestPeer::dispatch(
        *loop, {{old_registration.value().key, kReadable}});
    ASSERT_TRUE(stale);
    EXPECT_EQ(stale.value(), 0U);
    EXPECT_EQ(callbacks, 0);

    const auto current = EventLoopTestPeer::dispatch(
        *loop, {{new_registration.value().key, kReadable}});
    ASSERT_TRUE(current);
    EXPECT_EQ(current.value(), 1U);
    EXPECT_EQ(callbacks, 1);
    EXPECT_TRUE(loop->remove(new_registration.value()));
}

TEST(EventLoop, AlreadyClosedDescriptorCleanupDoesNotLeaveAStaleRegistration) {
    auto loop = make_loop();
    auto old_pipe = make_pipe();
    const int reused_fd_number = old_pipe.read_end.get();
    auto old_registration = loop->add(
        reused_fd_number, kReadable,
        [](const RegistrationToken&, EventMask) {});
    ASSERT_TRUE(old_registration);

    old_pipe.read_end.reset();
    ASSERT_TRUE(loop->remove(old_registration.value()));
    EXPECT_FALSE(loop->is_current(old_registration.value()));
    EXPECT_EQ(loop->registration_count(), 0U);

    auto new_pipe = make_pipe();
    common::UniqueFd reused_read_end;
    if (new_pipe.read_end.get() == reused_fd_number) {
        reused_read_end = std::move(new_pipe.read_end);
    } else {
        ASSERT_EQ(::dup3(new_pipe.read_end.get(), reused_fd_number, O_CLOEXEC),
                  reused_fd_number);
        reused_read_end.reset(reused_fd_number);
        new_pipe.read_end.reset();
    }

    auto new_registration = loop->add(
        reused_read_end.get(), kReadable,
        [](const RegistrationToken&, EventMask) {});
    ASSERT_TRUE(new_registration);
    EXPECT_NE(old_registration.value().generation,
              new_registration.value().generation);
    EXPECT_TRUE(loop->remove(new_registration.value()));
}

TEST(EventLoop, RejectsEdgeTriggeredAndOneShotInterest) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    EXPECT_FALSE(loop->add(pipe.read_end.get(), kReadable | EPOLLET,
                           [](const RegistrationToken&, EventMask) {}));
    EXPECT_FALSE(loop->add(pipe.read_end.get(), kReadable | EPOLLONESHOT,
                           [](const RegistrationToken&, EventMask) {}));
}

TEST(EventLoop, LevelTriggeredReadinessPersistsUntilDrainToEagain) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    std::vector<char> observed;
    auto added = loop->add(
        pipe.read_end.get(), kReadable,
        [&observed, &pipe](const RegistrationToken&, EventMask) {
            observed.push_back(read_byte(pipe.read_end.get()));
        });
    ASSERT_TRUE(added);

    write_byte(pipe.write_end.get(), '1');
    write_byte(pipe.write_end.get(), '2');
    const auto first = loop->poll_once(1'000);
    const auto second = loop->poll_once(1'000);
    const auto drained = loop->poll_once(0);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(drained);
    EXPECT_EQ(first.value(), 1U);
    EXPECT_EQ(second.value(), 1U);
    EXPECT_EQ(drained.value(), 0U);
    EXPECT_EQ(observed, (std::vector<char>{'1', '2'}));

    EXPECT_TRUE(loop->remove(added.value()));
}

TEST(EventLoop, WritableInterestCanBeEnabledAndDisabled) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    int writable_callbacks = 0;
    auto added = loop->add(
        pipe.write_end.get(), kWritable,
        [&loop, &writable_callbacks](const RegistrationToken& token, EventMask events) {
            EXPECT_NE(events & kWritable, 0U);
            ++writable_callbacks;
            EXPECT_TRUE(loop->modify(token, 0U));
        });
    ASSERT_TRUE(added);

    const auto first = loop->poll_once(1'000);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value(), 1U);
    EXPECT_EQ(writable_callbacks, 1);
    const auto disabled = loop->poll_once(0);
    ASSERT_TRUE(disabled);
    EXPECT_EQ(disabled.value(), 0U);

    ASSERT_TRUE(loop->modify(added.value(), kWritable));
    const auto second = loop->poll_once(1'000);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value(), 1U);
    EXPECT_EQ(writable_callbacks, 2);
    EXPECT_TRUE(loop->remove(added.value()));
}

TEST(EventLoop, CallbackCanRemoveItselfAndDeferOwnerDestruction) {
    struct Owner {
        explicit Owner(bool& destroyed) : destroyed_(destroyed) {}
        ~Owner() {
            destroyed_ = true;
        }
        bool& destroyed_;
    };

    auto loop = make_loop();
    auto pipe = make_pipe();
    bool destroyed = false;
    std::unordered_map<std::uint64_t, std::unique_ptr<Owner>> owners;
    owners.emplace(91U, std::make_unique<Owner>(destroyed));
    bool callback_completed = false;

    auto added = loop->add(
        pipe.read_end.get(), kReadable,
        [&loop, &owners, &destroyed, &callback_completed](const RegistrationToken& token,
                                                          EventMask) {
            EXPECT_FALSE(destroyed);
            EXPECT_TRUE(loop->remove(token));
            EXPECT_TRUE(loop->defer([&owners, owner_id = token.owner_id] {
                owners.erase(owner_id);
            }));
            EXPECT_FALSE(destroyed);
            callback_completed = true;
        },
        RegistrationMetadata{3U, 91U});
    ASSERT_TRUE(added);

    write_byte(pipe.write_end.get());
    const auto dispatched = loop->poll_once(1'000);
    ASSERT_TRUE(dispatched);
    EXPECT_EQ(dispatched.value(), 1U);
    EXPECT_TRUE(callback_completed);
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(owners.empty());
    EXPECT_EQ(loop->registration_count(), 0U);

    const auto stale = EventLoopTestPeer::dispatch(
        *loop, {{added.value().key, kReadable}});
    ASSERT_TRUE(stale);
    EXPECT_EQ(stale.value(), 0U);
}

TEST(EventLoop, CallbackCopyFailureUsesTheDispatchExceptionBoundary) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    auto throw_on_copy = std::make_shared<bool>(false);
    EventLoop::Callback callback = CopyThrowsWhenEnabled(throw_on_copy);
    auto added = loop->add(pipe.read_end.get(), kReadable, std::move(callback));
    ASSERT_TRUE(added);

    *throw_on_copy = true;
    const auto dispatched = EventLoopTestPeer::dispatch(
        *loop, {{added.value().key, kReadable}});
    ASSERT_FALSE(dispatched);
    EXPECT_NE(dispatched.error().message.find("callback copy failed"),
              std::string::npos);
    EXPECT_TRUE(loop->is_current(added.value()));

    *throw_on_copy = false;
    EXPECT_TRUE(loop->remove(added.value()));
}

TEST(EventLoop, DeferredActionsRunCapturedCleanupAfterFirstFailure) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    std::vector<int> observed;
    auto added = loop->add(
        pipe.read_end.get(), kReadable,
        [&loop, &observed](const RegistrationToken&, EventMask) {
            auto first = loop->defer([&observed] {
                observed.push_back(1);
                throw std::runtime_error("first deferred failure");
            });
            if (!first) {
                throw std::runtime_error(first.error().message);
            }

            auto second = loop->defer([&loop, &observed] {
                observed.push_back(2);
                auto next_safe_point = loop->defer(
                    [&observed] { observed.push_back(4); });
                if (!next_safe_point) {
                    throw std::runtime_error(next_safe_point.error().message);
                }
            });
            if (!second) {
                throw std::runtime_error(second.error().message);
            }

            auto third = loop->defer([&observed] {
                observed.push_back(3);
                throw std::runtime_error("later deferred failure");
            });
            if (!third) {
                throw std::runtime_error(third.error().message);
            }
        });
    ASSERT_TRUE(added);

    const auto first_safe_point = EventLoopTestPeer::dispatch(
        *loop, {{added.value().key, kReadable}});
    ASSERT_FALSE(first_safe_point);
    EXPECT_NE(first_safe_point.error().message.find("first deferred failure"),
              std::string::npos);
    EXPECT_EQ(first_safe_point.error().message.find("later deferred failure"),
              std::string::npos);
    EXPECT_EQ(observed, (std::vector<int>{1, 2, 3}));

    const auto second_safe_point = loop->poll_once(0);
    ASSERT_TRUE(second_safe_point);
    EXPECT_EQ(observed, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(loop->remove(added.value()));
}

TEST(EventLoop, RemovingAnotherRegistrationSkipsItsLaterBatchEvent) {
    auto loop = make_loop();
    auto first_pipe = make_pipe();
    auto second_pipe = make_pipe();
    int first_callbacks = 0;
    int second_callbacks = 0;
    RegistrationToken second_token{};

    auto second = loop->add(
        second_pipe.read_end.get(), kReadable,
        [&second_callbacks](const RegistrationToken&, EventMask) { ++second_callbacks; });
    ASSERT_TRUE(second);
    second_token = second.value();

    auto first = loop->add(
        first_pipe.read_end.get(), kReadable,
        [&loop, &first_callbacks, &second_token](const RegistrationToken&, EventMask) {
            ++first_callbacks;
            EXPECT_TRUE(loop->remove(second_token));
        });
    ASSERT_TRUE(first);

    const auto dispatched = EventLoopTestPeer::dispatch(
        *loop, {{first.value().key, kReadable}, {second.value().key, kReadable}});
    ASSERT_TRUE(dispatched);
    EXPECT_EQ(dispatched.value(), 1U);
    EXPECT_EQ(first_callbacks, 1);
    EXPECT_EQ(second_callbacks, 0);
    EXPECT_TRUE(loop->remove(first.value()));
}

TEST(EventLoop, StaleKeyCannotDispatchToReusedFileDescriptor) {
    auto loop = make_loop();
    auto old_pipe = make_pipe();
    const int reused_fd_number = old_pipe.read_end.get();
    int old_callbacks = 0;
    auto old_registration = loop->add(
        reused_fd_number, kReadable,
        [&old_callbacks](const RegistrationToken&, EventMask) { ++old_callbacks; },
        RegistrationMetadata{1U, 100U});
    ASSERT_TRUE(old_registration);
    ASSERT_TRUE(loop->remove(old_registration.value()));
    old_pipe.read_end.reset();

    auto new_pipe = make_pipe();
    common::UniqueFd reused_read_end;
    if (new_pipe.read_end.get() == reused_fd_number) {
        reused_read_end = std::move(new_pipe.read_end);
    } else {
        ASSERT_EQ(::dup3(new_pipe.read_end.get(), reused_fd_number, O_CLOEXEC),
                  reused_fd_number);
        reused_read_end.reset(reused_fd_number);
        new_pipe.read_end.reset();
    }

    int new_callbacks = 0;
    auto new_registration = loop->add(
        reused_read_end.get(), kReadable,
        [&new_callbacks](const RegistrationToken&, EventMask) { ++new_callbacks; },
        RegistrationMetadata{2U, 200U});
    ASSERT_TRUE(new_registration);
    EXPECT_NE(old_registration.value().generation,
              new_registration.value().generation);
    EXPECT_EQ(old_registration.value().fd, new_registration.value().fd);

    const auto stale = EventLoopTestPeer::dispatch(
        *loop, {{old_registration.value().key, kReadable}});
    ASSERT_TRUE(stale);
    EXPECT_EQ(stale.value(), 0U);
    EXPECT_EQ(old_callbacks, 0);
    EXPECT_EQ(new_callbacks, 0);

    const auto current = EventLoopTestPeer::dispatch(
        *loop, {{new_registration.value().key, kReadable}});
    ASSERT_TRUE(current);
    EXPECT_EQ(current.value(), 1U);
    EXPECT_EQ(new_callbacks, 1);
    EXPECT_TRUE(loop->remove(new_registration.value()));
}

TEST(EventLoop, RegistrationMutationIsThreadAffine) {
    auto loop = make_loop();
    auto pipe = make_pipe();
    bool cross_thread_succeeded = true;
    std::thread other([&loop, &pipe, &cross_thread_succeeded] {
        const auto result = loop->add(
            pipe.read_end.get(), kReadable,
            [](const RegistrationToken&, EventMask) {});
        cross_thread_succeeded = static_cast<bool>(result);
    });
    other.join();
    EXPECT_FALSE(cross_thread_succeeded);
    EXPECT_EQ(loop->registration_count(), 0U);
}

}  // namespace
}  // namespace ts::io
