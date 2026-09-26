#include "ts/io/event_fd.h"
#include "ts/io/event_loop.h"
#include "ts/io/signal_fd.h"
#include "ts/io/timer_fd.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace ts::io {
namespace {

std::unique_ptr<EventLoop> make_loop() {
    auto result = EventLoop::create();
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result).value();
}

void expect_nonblocking_close_on_exec(int fd) {
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    ASSERT_NE(descriptor_flags, -1);
    EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);

    const int status_flags = ::fcntl(fd, F_GETFL);
    ASSERT_NE(status_flags, -1);
    EXPECT_NE(status_flags & O_NONBLOCK, 0);
}

class ScopedSignalMask final {
public:
    explicit ScopedSignalMask(const sigset_t& blocked) {
        const int result = ::pthread_sigmask(SIG_BLOCK, &blocked, &original_);
        if (result != 0) {
            throw std::system_error(result, std::generic_category(), "pthread_sigmask");
        }
    }

    ~ScopedSignalMask() {
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &original_, nullptr));
    }

    ScopedSignalMask(const ScopedSignalMask&) = delete;
    ScopedSignalMask& operator=(const ScopedSignalMask&) = delete;

private:
    sigset_t original_{};
};

TEST(EventFd, CreatesWithRequiredFlagsAndDrainsAggregatedNotifications) {
    auto created = EventFd::create();
    ASSERT_TRUE(created);
    auto event_fd = std::move(created).value();
    expect_nonblocking_close_on_exec(event_fd.fd());

    EXPECT_FALSE(event_fd.notify(0U));
    ASSERT_TRUE(event_fd.notify(2U));
    ASSERT_TRUE(event_fd.notify(3U));
    const auto drained = event_fd.drain();
    ASSERT_TRUE(drained);
    EXPECT_EQ(drained.value(), 5U);
    const auto empty = event_fd.drain();
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty.value(), 0U);
}

TEST(EventFd, CrossThreadNotificationWakesEventLoop) {
    auto loop = make_loop();
    auto created = EventFd::create();
    ASSERT_TRUE(created);
    auto event_fd = std::move(created).value();
    std::uint64_t observed = 0U;
    auto registration = loop->add(
        event_fd.fd(), kReadable,
        [&event_fd, &observed](const RegistrationToken&, EventMask) {
            const auto drained = event_fd.drain();
            EXPECT_TRUE(drained);
            if (drained) {
                observed += drained.value();
            }
        });
    ASSERT_TRUE(registration);

    bool notification_succeeded = false;
    std::thread producer([&event_fd, &notification_succeeded] {
        notification_succeeded = static_cast<bool>(event_fd.notify(4U));
    });
    const auto dispatched = loop->poll_once(1'000);
    producer.join();
    ASSERT_TRUE(dispatched);
    EXPECT_EQ(dispatched.value(), 1U);
    EXPECT_TRUE(notification_succeeded);
    EXPECT_EQ(observed, 4U);
    EXPECT_TRUE(loop->remove(registration.value()));
}

TEST(TimerFd, UsesMonotonicClockAndSupportsDisarmAndRearm) {
    EXPECT_EQ(TimerFd::clock_id(), CLOCK_MONOTONIC);
    auto loop = make_loop();
    auto created = TimerFd::create();
    ASSERT_TRUE(created);
    auto timer = std::move(created).value();
    expect_nonblocking_close_on_exec(timer.fd());

    std::uint64_t expirations = 0U;
    auto registration = loop->add(
        timer.fd(), kReadable,
        [&timer, &expirations](const RegistrationToken&, EventMask) {
            const auto drained = timer.drain();
            EXPECT_TRUE(drained);
            if (drained) {
                expirations += drained.value();
            }
        });
    ASSERT_TRUE(registration);

    ASSERT_TRUE(timer.arm_once(std::chrono::milliseconds(50)));
    ASSERT_TRUE(timer.disarm());
    const auto disarmed = loop->poll_once(10);
    ASSERT_TRUE(disarmed);
    EXPECT_EQ(disarmed.value(), 0U);
    EXPECT_EQ(expirations, 0U);

    EXPECT_FALSE(timer.arm_once(std::chrono::nanoseconds::zero()));
    ASSERT_TRUE(timer.arm_once(std::chrono::milliseconds(1)));
    const auto rearmed = loop->poll_once(1'000);
    ASSERT_TRUE(rearmed);
    EXPECT_EQ(rearmed.value(), 1U);
    EXPECT_GE(expirations, 1U);
    const auto drained = loop->poll_once(0);
    ASSERT_TRUE(drained);
    EXPECT_EQ(drained.value(), 0U);
    EXPECT_TRUE(loop->remove(registration.value()));
}

TEST(SignalFd, DeliversBlockedSignalAndDrainsRecordsWithoutHandler) {
    sigset_t signal_set{};
    ASSERT_EQ(::sigemptyset(&signal_set), 0);
    ASSERT_EQ(::sigaddset(&signal_set, SIGUSR1), 0);

    sigset_t original_mask{};
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, nullptr, &original_mask), 0);
    {
        ScopedSignalMask blocked(signal_set);
        auto loop = make_loop();
        auto created = SignalFd::create(signal_set);
        ASSERT_TRUE(created);
        auto signal_fd = std::move(created).value();
        expect_nonblocking_close_on_exec(signal_fd.fd());

        std::vector<signalfd_siginfo> records;
        auto registration = loop->add(
            signal_fd.fd(), kReadable,
            [&signal_fd, &records](const RegistrationToken&, EventMask) {
                auto drained = signal_fd.drain();
                EXPECT_TRUE(drained);
                if (drained) {
                    records = std::move(drained).value();
                }
            });
        ASSERT_TRUE(registration);

        ASSERT_EQ(::kill(::getpid(), SIGUSR1), 0);
        const auto dispatched = loop->poll_once(1'000);
        ASSERT_TRUE(dispatched);
        EXPECT_EQ(dispatched.value(), 1U);
        ASSERT_EQ(records.size(), 1U);
        EXPECT_EQ(records.front().ssi_signo, static_cast<std::uint32_t>(SIGUSR1));
        const auto empty = signal_fd.drain();
        ASSERT_TRUE(empty);
        EXPECT_TRUE(empty.value().empty());
        EXPECT_TRUE(loop->remove(registration.value()));
    }

    sigset_t restored_mask{};
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, nullptr, &restored_mask), 0);
    EXPECT_EQ(::sigismember(&restored_mask, SIGUSR1),
              ::sigismember(&original_mask, SIGUSR1));
}

}  // namespace
}  // namespace ts::io
