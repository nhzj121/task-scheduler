#include "ts/net/connection_manager.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace ts::io {
// Existing M2 friend seam: exhaust registration identities without invalidating epoll or
// weakening its production error handling. This test binary does not link the M2 tests.
class EventLoopTestPeer {
public:
    static void exhaust_identities(EventLoop& loop) {
        loop.next_key_ = std::numeric_limits<std::uint64_t>::max();
    }
    static void restore_identities(EventLoop& loop) { loop.next_key_ = 2; }
};
} // namespace ts::io

namespace ts::net {
namespace {
using Clock = std::chrono::steady_clock;

std::size_t open_fds() {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator("/proc/self/fd"),
        std::filesystem::directory_iterator{}));
}

bool ready(int fd, short events) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, 50);
        if (result > 0) return true;
        if (result < 0 && errno != EINTR) return false;
    }
    errno = ETIMEDOUT;
    return false;
}

int finish_connect(int fd) {
    if (!ready(fd, POLLOUT)) return -1;
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return -1;
    if (error != 0) { errno = error; return -1; }
    return 0;
}

common::UniqueFd native_connect(const Endpoint& endpoint) {
    common::UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) throw std::runtime_error("native socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (::inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("invalid test endpoint");
    const int status = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (status < 0 && ((errno != EINPROGRESS && errno != EINTR) || finish_connect(fd.get()) < 0))
        throw std::runtime_error("native connect failed");
    return fd;
}

struct NativeListener {
    common::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    Endpoint endpoint{"127.0.0.1", 0};
    NativeListener() {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!fd || ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(fd.get(), 16) < 0) throw std::runtime_error("native listen failed");
        socklen_t length = sizeof(address);
        if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0)
            throw std::runtime_error("native getsockname failed");
        endpoint.port = ntohs(address.sin_port);
    }
    common::UniqueFd accept() const {
        if (!ready(fd.get(), POLLIN)) throw std::runtime_error("native accept deadline");
        int accepted;
        do { accepted = ::accept4(fd.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC); }
        while (accepted < 0 && errno == EINTR);
        if (accepted < 0) throw std::runtime_error("native accept failed");
        return common::UniqueFd(accepted);
    }
};

struct NativePair { common::UniqueFd peer, adopted; };
NativePair native_pair() {
    NativeListener listener;
    auto peer = native_connect(listener.endpoint);
    return {std::move(peer), listener.accept()};
}

template <typename Predicate> void poll_until(io::EventLoop& loop, Predicate complete) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!complete() && Clock::now() < deadline) {
        auto result = loop.poll_once(10);
        ASSERT_TRUE(result) << (result ? "" : result.error().message);
    }
    ASSERT_TRUE(complete());
}

void send_bytes(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        if (!ready(fd, POLLOUT)) throw std::runtime_error("native send deadline");
        const auto sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (sent <= 0) throw std::runtime_error("native send failed");
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

std::string receive_bytes(int fd, std::size_t count) {
    std::string bytes(count, '\0');
    std::size_t offset = 0;
    while (offset < count) {
        if (!ready(fd, POLLIN)) throw std::runtime_error("native receive deadline");
        const auto received = ::recv(fd, bytes.data() + offset, count - offset, 0);
        if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (received <= 0) throw std::runtime_error("native receive failed");
        offset += static_cast<std::size_t>(received);
    }
    return bytes;
}

void expect_eof(int fd) {
    ASSERT_TRUE(ready(fd, POLLIN));
    char byte{};
    EXPECT_EQ(::recv(fd, &byte, 1, 0), 0);
}

TransportLimits cleanup_limits() {
    TransportLimits limits;
    limits.maximum_frame_bytes = 64;
    limits.input_bytes = 64;
    limits.output = {{256, 8}, {256, 8}};
    return limits;
}

TEST(ConnectionManagerCleanup, CapacityRejectsExcessWithoutPollFailureAndReusesSlot) {
    const auto before = open_fds();
    {
        auto loop = std::move(io::EventLoop::create()).value();
        auto limits = cleanup_limits();
        limits.maximum_connections = 1;
        limits.syscall_budget = 2;
        std::unique_ptr<ConnectionManager> manager;
        std::vector<ConnectionId> connected;
        std::vector<CloseEvent> closed;
        std::string received;
        manager = std::move(ConnectionManager::create(*loop, {
            [&](ConnectionId id) { connected.push_back(id); },
            [&](ConnectionId id, std::string_view bytes) {
                received.append(bytes);
                EXPECT_TRUE(manager->send(id, {std::string(bytes)}));
                return bytes.size();
            },
            [&](CloseEvent event) { closed.push_back(event); }
        }, limits)).value();
        const auto listener = manager->listen({"127.0.0.1", 0}).value();
        const auto endpoint = manager->endpoint(listener).value();
        auto first = native_connect(endpoint);
        poll_until(*loop, [&] { return connected.size() == 1; });
        const auto established_fds = open_fds();
        std::vector<common::UniqueFd> excess;
        for (int i = 0; i < 5; ++i) excess.push_back(native_connect(endpoint));
        auto polled = loop->poll_once(0);
        ASSERT_TRUE(polled);
        EXPECT_EQ(manager->connection_count(), 1U);
        EXPECT_EQ(loop->registration_count(), 2U);
        expect_eof(excess[0].get());
        expect_eof(excess[1].get());
        char byte{};
        EXPECT_EQ(::recv(excess[2].get(), &byte, 1, MSG_DONTWAIT), -1);
        EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK); // Accept budget was not bypassed.
        for (int i = 0; i < 2; ++i) ASSERT_TRUE(loop->poll_once(0));
        for (const auto& peer : excess) expect_eof(peer.get());
        EXPECT_EQ(open_fds(), established_fds + excess.size()); // No retained accepted sockets.
        excess.clear();
        EXPECT_EQ(open_fds(), established_fds);
        EXPECT_EQ(connected.size(), 1U);
        EXPECT_TRUE(closed.empty()); // Rejected sockets never acquired connection identities.
        send_bytes(first.get(), "still-alive");
        poll_until(*loop, [&] {
            return received == "still-alive" && manager->inspect(connected[0])->control_bytes == 0;
        });
        EXPECT_EQ(receive_bytes(first.get(), 11), "still-alive");
        ASSERT_TRUE(manager->close(connected[0]));
        EXPECT_EQ(manager->connection_count(), 1U); // Retired owner still counts until safe point.
        ASSERT_TRUE(loop->poll_once(0));
        EXPECT_EQ(manager->connection_count(), 0U);
        auto replacement = native_connect(endpoint);
        poll_until(*loop, [&] { return connected.size() == 2; });
        EXPECT_NE(connected[0].value, connected[1].value);
        received.clear();
        send_bytes(replacement.get(), "reused");
        poll_until(*loop, [&] {
            return received == "reused" && manager->inspect(connected[1])->control_bytes == 0;
        });
        EXPECT_EQ(receive_bytes(replacement.get(), 6), "reused");
        ASSERT_TRUE(manager->close(connected[1]));
        ASSERT_TRUE(loop->poll_once(0));
        EXPECT_EQ(closed.size(), 2U);
        EXPECT_EQ(loop->registration_count(), 1U);
    }
    EXPECT_EQ(open_fds(), before);
}

TEST(ConnectionManagerCleanup, DirectAdoptCapacityFailureClosesOnlyRejectedSocket) {
    const auto before = open_fds();
    {
        auto loop = std::move(io::EventLoop::create()).value();
        auto limits = cleanup_limits(); limits.maximum_connections = 1;
        std::size_t calls = 0;
        auto manager = std::move(ConnectionManager::create(*loop, {
            [&](ConnectionId) { ++calls; }, {}, {}
        }, limits)).value();
        auto first = native_pair();
        auto accepted = manager->adopt(std::move(first.adopted)); ASSERT_TRUE(accepted);
        auto excess = native_pair(); const int rejected_fd = excess.adopted.get();
        auto rejected = manager->adopt(std::move(excess.adopted));
        ASSERT_FALSE(rejected);
        EXPECT_EQ(::fcntl(rejected_fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
        EXPECT_EQ(calls, 1U); EXPECT_EQ(manager->connection_count(), 1U);
        EXPECT_EQ(loop->registration_count(), 1U);
        EXPECT_FALSE(manager->inspect(accepted.value())->closing);
    }
    EXPECT_EQ(open_fds(), before);
}

TEST(ConnectionManagerCleanup, AcceptRegistrationFailureStillFailsPollExplicitly) {
    const auto before = open_fds();
    {
        auto loop = std::move(io::EventLoop::create()).value();
        std::size_t calls = 0;
        auto manager = std::move(ConnectionManager::create(*loop, {
            [&](ConnectionId) { ++calls; }, {}, {}
        }, cleanup_limits())).value();
        const auto listener = manager->listen({"127.0.0.1", 0}).value();
        auto peer = native_connect(manager->endpoint(listener).value());
        io::EventLoopTestPeer::exhaust_identities(*loop);
        const auto polled = loop->poll_once(1000);
        io::EventLoopTestPeer::restore_identities(*loop);
        ASSERT_FALSE(polled);
        EXPECT_NE(polled.error().message.find("generation"), std::string::npos);
        EXPECT_EQ(calls, 0U); EXPECT_EQ(manager->connection_count(), 0U);
        EXPECT_EQ(loop->registration_count(), 1U);
        expect_eof(peer.get());
        auto next = native_connect(manager->endpoint(listener).value());
        poll_until(*loop, [&] { return calls == 1; });
        EXPECT_EQ(manager->connection_count(), 1U);
    }
    EXPECT_EQ(open_fds(), before);
}

struct CallbackFailureHarness {
    std::unique_ptr<io::EventLoop> loop{std::move(io::EventLoop::create()).value()};
    std::unique_ptr<ConnectionManager> manager;
    bool fail_connected{true}, close_first{false}, fail_closed{false}, nonstandard{false};
    std::vector<ConnectionId> connected;
    std::vector<CloseEvent> closed;
    std::optional<ConnectionSnapshot> before_callback_failure;
    bool notified_with_live_owner{false};
    explicit CallbackFailureHarness(SocketIo operations = {}) {
        manager = std::move(ConnectionManager::create(*loop, {
            [&](ConnectionId id) {
                connected.push_back(id);
                before_callback_failure = manager->inspect(id);
                if (close_first) { EXPECT_TRUE(manager->close(id)); }
                if (fail_connected) {
                    if (nonstandard) throw 17;
                    throw std::runtime_error("connected test failure");
                }
            }, {},
            [&](CloseEvent event) {
                closed.push_back(event);
                auto state = manager->inspect(event.id);
                notified_with_live_owner = state && state->closing &&
                    loop->is_current(state->registration) && ::fcntl(state->fd, F_GETFD) >= 0;
                if (fail_closed) throw std::runtime_error("closed test failure");
            }
        }, cleanup_limits(), std::move(operations))).value();
    }
    void expect_cleanup(bool safe_point_ran, CloseCause cause = CloseCause::CallbackError,
                        std::size_t remaining_registrations = 0) {
        ASSERT_EQ(connected.size(), 1U);
        ASSERT_EQ(closed.size(), 1U);
        EXPECT_EQ(closed[0].id, connected[0]); EXPECT_EQ(closed[0].cause, cause);
        EXPECT_TRUE(notified_with_live_owner);
        ASSERT_TRUE(before_callback_failure);
        EXPECT_FALSE(loop->is_current(before_callback_failure->registration));
        EXPECT_EQ(::fcntl(before_callback_failure->fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
        EXPECT_EQ(loop->registration_count(), remaining_registrations);
        if (!safe_point_ran) {
            auto closing = manager->inspect(connected[0]); ASSERT_TRUE(closing);
            EXPECT_TRUE(closing->closing); EXPECT_EQ(manager->connection_count(), 1U);
            EXPECT_TRUE(loop->poll_once(0));
        }
        EXPECT_FALSE(manager->inspect(connected[0]));
        EXPECT_EQ(manager->connection_count(), 0U);
        EXPECT_EQ(closed.size(), 1U);
    }
};

TEST(ConnectionManagerCleanup, ThrowingConnectedDuringDirectAdoptReturnsFailure) {
    const auto before = open_fds();
    {
        CallbackFailureHarness harness;
        auto pair = native_pair();
        const auto result = harness.manager->adopt(std::move(pair.adopted));
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().message.find("connected callback threw"), std::string::npos);
        harness.expect_cleanup(false);
        expect_eof(pair.peer.get());
    }
    EXPECT_EQ(open_fds(), before);
}

TEST(ConnectionManagerCleanup, ThrowingConnectedDuringImmediateConnectReturnsFailure) {
    const auto before = open_fds();
    {
        NativeListener listener;
        bool completed = false;
        SocketIo operations;
        operations.connect = [&](int fd, const sockaddr* address, socklen_t length) {
            const auto status = ::connect(fd, address, length);
            if (status < 0 && errno != EINPROGRESS && errno != EINTR) return status;
            // Test-only bounded synchronization: real TCP is complete before reporting 0.
            // Production uses ::connect directly and never performs this wait.
            const int result = status == 0 ? 0 : finish_connect(fd);
            completed = result == 0;
            return result;
        };
        CallbackFailureHarness harness(std::move(operations));
        const auto result = harness.manager->connect(listener.endpoint);
        ASSERT_TRUE(completed); ASSERT_FALSE(result);
        EXPECT_NE(result.error().message.find("connected callback threw"), std::string::npos);
        harness.expect_cleanup(false);
        auto peer = listener.accept(); expect_eof(peer.get());
    }
    EXPECT_EQ(open_fds(), before);
}

TEST(ConnectionManagerCleanup, ThrowingConnectedDuringAsyncConnectFailsPollAfterCleanup) {
    const auto before = open_fds();
    {
        NativeListener listener;
        CallbackFailureHarness harness; harness.nonstandard = true;
        const auto pending = harness.manager->connect(listener.endpoint); ASSERT_TRUE(pending);
        ASSERT_TRUE(harness.manager->inspect(pending.value())->connecting);
        EXPECT_TRUE(harness.connected.empty());
        const auto polled = harness.loop->poll_once(1000);
        ASSERT_FALSE(polled);
        EXPECT_NE(polled.error().message.find("connected callback threw"), std::string::npos);
        harness.expect_cleanup(true);
        auto peer = listener.accept(); expect_eof(peer.get());
    }
    EXPECT_EQ(open_fds(), before);
}

TEST(ConnectionManagerCleanup, ThrowingConnectedDuringAcceptFailsPollButKeepsListener) {
    const auto before = open_fds();
    {
        CallbackFailureHarness harness;
        const auto listener = harness.manager->listen({"127.0.0.1", 0}).value();
        auto peer = native_connect(harness.manager->endpoint(listener).value());
        const auto polled = harness.loop->poll_once(1000); ASSERT_FALSE(polled);
        harness.expect_cleanup(true, CloseCause::CallbackError, 1);
        expect_eof(peer.get());
        harness.fail_connected = false;
        auto next = native_connect(harness.manager->endpoint(listener).value());
        poll_until(*harness.loop, [&] { return harness.connected.size() == 2; });
        EXPECT_EQ(harness.manager->connection_count(), 1U);
        EXPECT_EQ(harness.closed.size(), 1U);
    }
    EXPECT_EQ(open_fds(), before);
}

TEST(ConnectionManagerCleanup, SelfCloseThenConnectedThrowNotifiesOnlyOnce) {
    const auto before = open_fds();
    {
        CallbackFailureHarness harness; harness.close_first = true;
        auto pair = native_pair();
        EXPECT_FALSE(harness.manager->adopt(std::move(pair.adopted)));
        harness.expect_cleanup(false, CloseCause::Local);
    }
    EXPECT_EQ(open_fds(), before);
}

TEST(ConnectionManagerCleanup, ConnectedAndClosedExceptionsRemainExplicitWithoutLeaks) {
    const auto before = open_fds();
    {
        CallbackFailureHarness harness; harness.fail_closed = true;
        auto pair = native_pair();
        const auto result = harness.manager->adopt(std::move(pair.adopted)); ASSERT_FALSE(result);
        EXPECT_NE(result.error().message.find("connected callback threw"), std::string::npos);
        EXPECT_NE(result.error().message.find("close callback threw"), std::string::npos);
        harness.expect_cleanup(false);
    }
    EXPECT_EQ(open_fds(), before);
}

struct ConnectedCopyFailure {
    std::shared_ptr<bool> armed;
    std::size_t* invocations;
    ConnectedCopyFailure(std::shared_ptr<bool> flag, std::size_t& calls)
        : armed(std::move(flag)), invocations(&calls) {}
    ConnectedCopyFailure(const ConnectedCopyFailure& other)
        : armed(other.armed), invocations(other.invocations) {
        if (*armed) throw std::runtime_error("connected callback copy failed");
    }
    void operator()(ConnectionId) const { ++*invocations; }
};

TEST(ConnectionManagerCleanup, ConnectedCallbackCopyFailureReturnsFailureAndCleansOwner) {
    const auto before = open_fds();
    {
        auto loop = std::move(io::EventLoop::create()).value();
        auto armed = std::make_shared<bool>(false);
        std::size_t invocations = 0;
        std::vector<CloseEvent> closed;
        auto manager = std::move(ConnectionManager::create(*loop, {
            ConnectedCopyFailure(armed, invocations), {},
            [&](CloseEvent event) { closed.push_back(event); }
        }, cleanup_limits())).value();
        *armed = true;
        auto pair = native_pair();
        const auto result = manager->adopt(std::move(pair.adopted)); ASSERT_FALSE(result);
        EXPECT_EQ(invocations, 0U); ASSERT_EQ(closed.size(), 1U);
        EXPECT_EQ(closed[0].cause, CloseCause::CallbackError);
        EXPECT_EQ(loop->registration_count(), 0U); EXPECT_EQ(manager->connection_count(), 1U);
        ASSERT_TRUE(loop->poll_once(0)); EXPECT_EQ(manager->connection_count(), 0U);
        EXPECT_EQ(closed.size(), 1U);
    }
    EXPECT_EQ(open_fds(), before);
}
} // namespace
} // namespace ts::net
