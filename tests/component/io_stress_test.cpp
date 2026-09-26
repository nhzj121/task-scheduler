#include "ts/io/event_fd.h"
#include "ts/io/event_loop.h"
#include "ts/io/timer_fd.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "ts/common/unique_fd.h"

namespace ts::io {
namespace {

struct Pipe {
    common::UniqueFd read_end;
    common::UniqueFd write_end;
};

Pipe make_pipe() {
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_NONBLOCK | O_CLOEXEC) == -1) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
    return Pipe{common::UniqueFd(fds[0]), common::UniqueFd(fds[1])};
}

std::unique_ptr<EventLoop> make_loop() {
    auto result = EventLoop::create();
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result).value();
}

std::size_t count_open_fds() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
        throw std::system_error(errno, std::generic_category(), "opendir /proc/self/fd");
    }
    std::size_t count = 0U;
    while (const dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        ++count;
    }
    if (::closedir(directory) == -1) {
        throw std::system_error(errno, std::generic_category(), "closedir /proc/self/fd");
    }
    return count;
}

void write_one(int fd) {
    const char value = 'x';
    ssize_t result = -1;
    do {
        result = ::write(fd, &value, sizeof(value));
    } while (result == -1 && errno == EINTR);
    if (result != 1) {
        throw std::system_error(errno, std::generic_category(), "write");
    }
}

TEST(IoStress, RepeatedRegistrationSelfRemovalWakeupAndTimerChurnDoesNotLeakFds) {
    auto loop = make_loop();
    const std::size_t baseline = count_open_fds();
    std::size_t callbacks = 0U;
    std::size_t deferred = 0U;

    for (std::size_t iteration = 0U; iteration < 500U; ++iteration) {
        auto pipe = make_pipe();
        auto registration = loop->add(
            pipe.read_end.get(), kReadable,
            [&loop, &callbacks, &deferred](const RegistrationToken& token, EventMask) {
                ++callbacks;
                EXPECT_TRUE(loop->remove(token));
                EXPECT_TRUE(loop->defer([&deferred] { ++deferred; }));
            });
        ASSERT_TRUE(registration) << "iteration=" << iteration;
        write_one(pipe.write_end.get());
        const auto dispatched = loop->poll_once(1'000);
        ASSERT_TRUE(dispatched) << "iteration=" << iteration;
        ASSERT_EQ(dispatched.value(), 1U) << "iteration=" << iteration;
        ASSERT_EQ(loop->registration_count(), 0U) << "iteration=" << iteration;
    }

    for (std::size_t iteration = 0U; iteration < 100U; ++iteration) {
        auto created = EventFd::create();
        ASSERT_TRUE(created) << "iteration=" << iteration;
        auto event_fd = std::move(created).value();
        auto registration = loop->add(
            event_fd.fd(), kReadable,
            [&loop, &event_fd, &callbacks](const RegistrationToken& token, EventMask) {
                const auto drained = event_fd.drain();
                EXPECT_TRUE(drained);
                ++callbacks;
                EXPECT_TRUE(loop->remove(token));
            });
        ASSERT_TRUE(registration) << "iteration=" << iteration;
        ASSERT_TRUE(event_fd.notify()) << "iteration=" << iteration;
        const auto dispatched = loop->poll_once(1'000);
        ASSERT_TRUE(dispatched) << "iteration=" << iteration;
        ASSERT_EQ(dispatched.value(), 1U) << "iteration=" << iteration;
    }

    for (std::size_t iteration = 0U; iteration < 100U; ++iteration) {
        auto created = TimerFd::create();
        ASSERT_TRUE(created) << "iteration=" << iteration;
        auto timer = std::move(created).value();
        auto registration = loop->add(
            timer.fd(), kReadable,
            [&loop, &timer, &callbacks](const RegistrationToken& token, EventMask) {
                const auto drained = timer.drain();
                EXPECT_TRUE(drained);
                ++callbacks;
                EXPECT_TRUE(loop->remove(token));
            });
        ASSERT_TRUE(registration) << "iteration=" << iteration;
        ASSERT_TRUE(timer.arm_once(std::chrono::microseconds(100)))
            << "iteration=" << iteration;
        const auto dispatched = loop->poll_once(1'000);
        ASSERT_TRUE(dispatched) << "iteration=" << iteration;
        ASSERT_EQ(dispatched.value(), 1U) << "iteration=" << iteration;
    }

    EXPECT_EQ(callbacks, 700U);
    EXPECT_EQ(deferred, 500U);
    EXPECT_EQ(loop->registration_count(), 0U);
    EXPECT_EQ(count_open_fds(), baseline);
}

}  // namespace
}  // namespace ts::io
