#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "ts/common/result.h"
#include "ts/net/connection_manager.h"
#include "ts/protocol/messages.h"

namespace ts::client {
// Caller-owned synchronous budget. expired is set only when this budget, not a
// prior transport/protocol failure or the normal request timeout, stops a call.
struct Deadline {
    std::chrono::steady_clock::time_point at;
    bool expired{false};
};
// Single-threaded client, one outstanding request. Uses the same nonblocking M3
// transport/parser as the applications; no second socket implementation.
class Client final {
public:
    static common::Result<std::unique_ptr<Client>> connect(
        net::Endpoint endpoint, std::chrono::milliseconds timeout = std::chrono::seconds(5),
        Deadline* deadline = nullptr);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    [[nodiscard]] common::Result<protocol::Message> request(protocol::Message message, Deadline* deadline = nullptr);
    // Consumer is synchronous; it must not retain references into a page.
    [[nodiscard]] common::Result<std::size_t> list(
        const std::function<void(const protocol::ListTasksResponse&)>& consume, std::uint64_t page_size = 100);
    [[nodiscard]] common::Result<std::uint64_t> logs(protocol::GetLogs request,
        const std::function<void(const protocol::GetLogsResponse&)>& consume);
private:
    struct Impl;
    explicit Client(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::client
