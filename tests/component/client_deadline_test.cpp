#include "ts/client/client.h"
#include "ts/common/unique_fd.h"
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <filesystem>

namespace ts::client {
namespace {
using Clock = std::chrono::steady_clock;
struct Peer {
    common::UniqueFd listener{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    net::Endpoint endpoint{"127.0.0.1", 0};
    std::thread thread;
    std::atomic<int> received{-1};
    Peer() {
        if (!listener) throw std::runtime_error("socket");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(listener.get(), 1))
            throw std::runtime_error("listen");
        socklen_t size = sizeof(address);
        if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &size)) throw std::runtime_error("getsockname");
        endpoint.port = ntohs(address.sin_port);
    }
    void serve(bool close_now = false) {
        thread = std::thread([this, close_now] {
            pollfd fd{listener.get(), POLLIN, 0};
            if (::poll(&fd, 1, 500) <= 0) return;
            common::UniqueFd socket{::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)};
            if (!socket || close_now) return;
            fd = {socket.get(), POLLIN, 0};
            if (::poll(&fd, 1, 500) > 0) {
                char bytes[4096];
                received = static_cast<int>(::recv(socket.get(), bytes, sizeof(bytes), 0));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        });
    }
    void join() { if (thread.joinable()) thread.join(); }
    ~Peer() { join(); }
};
TEST(ClientDeadline, ExpiredConnectDoesNotOpenConnection) {
    Peer peer;
    Deadline budget{Clock::now() - std::chrono::milliseconds(1)};
    EXPECT_FALSE(Client::connect(peer.endpoint, std::chrono::seconds(5), &budget));
    EXPECT_TRUE(budget.expired);
    pollfd fd{peer.listener.get(), POLLIN, 0};
    EXPECT_EQ(::poll(&fd, 1, 0), 0);
}
TEST(ClientDeadline, ExpiredRequestDoesNotSendAndConnectionIsReleased) {
    Peer peer; peer.serve();
    auto made = Client::connect(peer.endpoint); ASSERT_TRUE(made);
    auto client = std::move(made).value();
    Deadline budget{Clock::now() - std::chrono::milliseconds(1)};
    EXPECT_FALSE(client->request(protocol::GetTask{domain::TaskId::generate()}, &budget));
    EXPECT_TRUE(budget.expired);
    client.reset(); peer.join();
    EXPECT_EQ(peer.received, 0);
}
TEST(ClientDeadline, OverallBudgetClipsAnOutstandingRequest) {
    Peer peer; peer.serve();
    auto made = Client::connect(peer.endpoint); ASSERT_TRUE(made);
    const auto start = Clock::now();
    Deadline budget{start + std::chrono::milliseconds(25)};
    EXPECT_FALSE(made.value()->request(protocol::GetTask{domain::TaskId::generate()}, &budget));
    EXPECT_TRUE(budget.expired);
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(1));
}
TEST(ClientDeadline, OrdinaryRequestTimeoutIsNotOverallExpiry) {
    Peer peer; peer.serve();
    auto made = Client::connect(peer.endpoint, std::chrono::milliseconds(30)); ASSERT_TRUE(made);
    Deadline budget{Clock::now() + std::chrono::seconds(2)};
    EXPECT_FALSE(made.value()->request(protocol::GetTask{domain::TaskId::generate()}, &budget));
    EXPECT_FALSE(budget.expired);
}
TEST(ClientDeadline, TransportCloseIsNotOverallExpiry) {
    Peer peer; peer.serve(true);
    Deadline budget{Clock::now() + std::chrono::seconds(2)};
    auto made = Client::connect(peer.endpoint, std::chrono::seconds(1), &budget);
    if (made) { EXPECT_FALSE(made.value()->request(protocol::GetTask{domain::TaskId::generate()}, &budget)); }
    EXPECT_FALSE(budget.expired);
}
TEST(ClientDeadline, RepeatedExpiredConnectDoesNotGrowFileDescriptors) {
    Peer peer;
    const auto count = [] {
        std::size_t size = 0;
        for (const auto& item : std::filesystem::directory_iterator("/proc/self/fd")) {
            static_cast<void>(item); ++size;
        }
        return size;
    };
    const auto before = count();
    for (int i = 0; i < 10; ++i) {
        Deadline budget{Clock::now() - std::chrono::milliseconds(1)};
        EXPECT_FALSE(Client::connect(peer.endpoint, std::chrono::seconds(5), &budget));
    }
    EXPECT_EQ(count(), before);
}
} // namespace
} // namespace ts::client
