#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "ts/common/result.h"
#include "ts/net/connection_manager.h"
#include "ts/protocol/messages.h"

namespace ts::client {
// Single-threaded client, one outstanding request. Uses the same nonblocking M3
// transport/parser as the applications; no second socket implementation.
class Client final {
public:
    static common::Result<std::unique_ptr<Client>> connect(
        net::Endpoint endpoint, std::chrono::milliseconds timeout = std::chrono::seconds(5));
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    [[nodiscard]] common::Result<protocol::Message> request(protocol::Message message);
    // Consumer is synchronous; it must not retain references into a page.
    [[nodiscard]] common::Result<std::size_t> list(
        const std::function<void(const protocol::ListTasksResponse&)>& consume, std::uint64_t page_size = 100);
private:
    struct Impl;
    explicit Client(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::client
