#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>

#include "ts/common/unique_fd.h"
#include "ts/io/event_loop.h"
#include "ts/net/output_queue.h"

namespace ts::net {
struct ConnectionId {
    std::uint64_t value;
    friend bool operator==(ConnectionId a, ConnectionId b) noexcept { return a.value == b.value; }
};
struct ListenerId { std::uint64_t value; };
struct Endpoint { std::string address; std::uint16_t port; };
enum class CloseCause { Local, PeerEof, ConnectError, ReceiveError, SendError, InputOverflow, OutputOverflow, CallbackError };
struct CloseEvent {
    ConnectionId id; CloseCause cause; int system_error;
    std::size_t discarded_control_bytes, discarded_log_bytes;
};
struct TransportLimits {
    // Generic encoded-message capacity; protocol adapters must supply/support their wire max.
    std::size_t maximum_frame_bytes{4194324};
    std::size_t input_bytes{4194324};
    OutputLimits output;
    std::size_t maximum_connections{256}, maximum_listeners{8};
    std::size_t read_budget{65536}, write_budget{65536}, syscall_budget{64};
};
struct Callbacks {
    std::function<void(ConnectionId)> connected;
    // Synchronous borrow. Return bytes consumed; no pointer may survive this callback.
    // Retained input is bounded; a full unconsumed buffer closes explicitly, never grows.
    std::function<std::size_t(ConnectionId, std::string_view)> bytes;
    std::function<void(CloseEvent)> closed;
};
// Optional deterministic syscall seam. Same errno/count contract as Linux; hooks must not throw.
struct SocketIo {
    std::function<ssize_t(int, const void*, std::size_t, int)> send;
    std::function<ssize_t(int, void*, std::size_t, int)> recv;
    std::function<int(int, const sockaddr*, socklen_t)> connect;
};
struct ConnectionSnapshot {
    int fd; bool connecting, closing; io::EventMask interest;
    std::size_t input_bytes, control_bytes, log_bytes, control_slots, log_slots;
    io::RegistrationToken registration;
};
class ConnectionManager final {
public:
    static common::Result<std::unique_ptr<ConnectionManager>> create(
        io::EventLoop& loop, Callbacks callbacks, TransportLimits limits = {}, SocketIo operations = {});
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    // Owner-thread only. Manager and EventLoop outlive all callbacks; destroy manager before loop.
    [[nodiscard]] common::Result<ListenerId> listen(const Endpoint& endpoint, int backlog = 128);
    [[nodiscard]] common::Result<Endpoint> endpoint(ListenerId id) const;
    [[nodiscard]] common::Result<void> close_listener(ListenerId id);
    // Connected-callback copy/invocation failures close with CallbackError. Synchronous
    // completion returns a failed Result; deferred completion fails EventLoop::poll_once.
    [[nodiscard]] common::Result<ConnectionId> connect(const Endpoint& endpoint);
    // Takes exclusive ownership of an already connected TCP fd (also normalizes its flags).
    [[nodiscard]] common::Result<ConnectionId> adopt(common::UniqueFd socket);
    [[nodiscard]] common::Result<void> send(ConnectionId id, Outbound frame);
    [[nodiscard]] common::Result<void> close(ConnectionId id, CloseCause cause = CloseCause::Local);
    [[nodiscard]] common::Result<void> set_read_enabled(ConnectionId id, bool enabled);
    [[nodiscard]] std::optional<ConnectionSnapshot> inspect(ConnectionId id) const;
    [[nodiscard]] std::size_t connection_count() const noexcept;
private:
    struct Impl;
    explicit ConnectionManager(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::net
