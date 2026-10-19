#include "ts/net/connection_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ts::net {
namespace {
std::string diagnostic(const char* call, int error = errno) { return std::string(call) + ": " + std::strerror(error); }
struct Address { sockaddr_storage storage{}; socklen_t size{0}; int family{0}; };
common::Result<Address> address(const Endpoint& endpoint) {
    if (endpoint.address.find('\0') != std::string::npos) return common::Result<Address>::failure("NUL in numeric address");
    Address result;
    if (endpoint.address.find(':') != std::string::npos) {
        sockaddr_in6 value{}; value.sin6_family = AF_INET6; value.sin6_port = htons(endpoint.port);
        if (::inet_pton(AF_INET6, endpoint.address.c_str(), &value.sin6_addr) != 1) return common::Result<Address>::failure("invalid numeric IPv6 address");
        std::memcpy(&result.storage, &value, sizeof(value)); result.size = sizeof(value); result.family = AF_INET6;
    } else {
        sockaddr_in value{}; value.sin_family = AF_INET; value.sin_port = htons(endpoint.port);
        if (::inet_pton(AF_INET, endpoint.address.c_str(), &value.sin_addr) != 1) return common::Result<Address>::failure("invalid numeric IPv4 address");
        std::memcpy(&result.storage, &value, sizeof(value)); result.size = sizeof(value); result.family = AF_INET;
    }
    return common::Result<Address>::success(result);
}
common::Result<void> normalize(int fd) {
    int flags;
    do { flags = ::fcntl(fd, F_GETFL); } while (flags < 0 && errno == EINTR);
    if (flags < 0) return common::Result<void>::failure(diagnostic("fcntl F_GETFL"));
    int status;
    do { status = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK); } while (status < 0 && errno == EINTR);
    if (status < 0) return common::Result<void>::failure(diagnostic("fcntl O_NONBLOCK"));
    do { flags = ::fcntl(fd, F_GETFD); } while (flags < 0 && errno == EINTR);
    if (flags < 0) return common::Result<void>::failure(diagnostic("fcntl F_GETFD"));
    do { status = ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC); } while (status < 0 && errno == EINTR);
    if (status < 0) return common::Result<void>::failure(diagnostic("fcntl FD_CLOEXEC"));
    return common::Result<void>::success();
}
void explicit_failure(common::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
} // namespace

struct ConnectionManager::Impl {
    struct TcpConnection {
        ConnectionId id;
        common::UniqueFd socket;
        std::optional<io::RegistrationToken> token;
        bool connecting, closing{false}, read_enabled{true};
        io::EventMask interest{0};
        std::vector<char> input; std::size_t input_used{0};
        OutputQueue output;
        TcpConnection(ConnectionId identity, common::UniqueFd fd, bool pending, const TransportLimits& limits)
            : id(identity), socket(std::move(fd)), connecting(pending), input(limits.input_bytes), output(limits.output) {}
    };
    struct Listener { common::UniqueFd socket; std::optional<io::RegistrationToken> token; };
    io::EventLoop& loop;
    Callbacks callbacks;
    TransportLimits limits;
    SocketIo operations;
    std::thread::id owner{std::this_thread::get_id()};
    // Only a lifetime sentinel is shared, never a connection or its socket/buffers.
    std::shared_ptr<int> alive{std::make_shared<int>(0)};
    std::uint64_t next_id{1};
    std::unordered_map<std::uint64_t, std::unique_ptr<TcpConnection>> connections;
    std::unordered_map<std::uint64_t, Listener> listeners;

    Impl(io::EventLoop& event_loop, Callbacks handlers, TransportLimits bounds, SocketIo syscalls)
        : loop(event_loop), callbacks(std::move(handlers)), limits(bounds), operations(std::move(syscalls)) {
        if (!operations.send) operations.send = ::send;
        if (!operations.recv) operations.recv = ::recv;
        if (!operations.connect) operations.connect = ::connect;
    }
    ~Impl() {
        if (owner != std::this_thread::get_id()) std::terminate();
        alive.reset();
        for (auto& entry : listeners) if (entry.second.token && !loop.remove(*entry.second.token)) std::terminate();
        for (auto& entry : connections) if (entry.second->token && !loop.remove(*entry.second->token)) std::terminate();
    }
    bool on_owner() const { return owner == std::this_thread::get_id(); }
    bool at_capacity() const { return connections.size() >= limits.maximum_connections; }
    std::uint64_t allocate_id() {
        if (next_id == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("transport identity exhausted");
        return next_id++;
    }
    TcpConnection* find(ConnectionId id) {
        const auto it = connections.find(id.value); return it == connections.end() ? nullptr : it->second.get();
    }
    common::Result<void> update(TcpConnection& connection) {
        if (connection.closing) return common::Result<void>::success();
        const auto interest = (connection.read_enabled ? (io::kReadable | io::kReadHangup) : 0U) |
            ((connection.connecting || !connection.output.empty()) ? io::kWritable : 0U);
        if (interest == connection.interest) return common::Result<void>::success();
        auto changed = loop.modify(*connection.token, interest);
        if (changed) connection.interest = interest;
        return changed;
    }
    common::Result<void> close(ConnectionId id, CloseCause cause, int error = 0) {
        auto* connection = find(id);
        if (!connection) return common::Result<void>::failure("unknown connection identity");
        std::exception_ptr callback_failure;
        if (!connection->closing) {
            connection->closing = true;
            const CloseEvent event{id,cause,error,connection->output.unsent_bytes(QueueClass::Control),connection->output.unsent_bytes(QueueClass::Log)};
            try { if (callbacks.closed) { auto callback = callbacks.closed; callback(event); } }
            catch (...) { callback_failure = std::current_exception(); }
        }
        if (connection->token) {
            auto removed = loop.remove(*connection->token);
            if (!removed) return removed; // Explicit fatal cleanup failure: retain owner and fd.
            connection->token.reset();
        }
        connection->output.clear(); connection->socket.reset();
        const std::weak_ptr<int> lifetime = alive;
        auto deferred = loop.defer([this, lifetime, id] { if (!lifetime.expired()) connections.erase(id.value); });
        if (!deferred) return deferred; // Already closed, retained until manager destruction.
        if (callback_failure) return common::Result<void>::failure("close callback threw (transport cleanup completed)");
        return common::Result<void>::success();
    }
    common::Result<void> notify_connected(TcpConnection& connection) {
        try { if (callbacks.connected) { auto callback = callbacks.connected; callback(connection.id); } }
        catch (...) {
            auto cleaned = close(connection.id,CloseCause::CallbackError);
            if (!cleaned) return common::Result<void>::failure("connected callback threw; cleanup failed: " + cleaned.error().message);
            return common::Result<void>::failure("connected callback threw (transport cleanup completed)");
        }
        return common::Result<void>::success();
    }
    common::Result<ConnectionId> adopt(common::UniqueFd socket, bool connecting) {
        if (!on_owner()) return common::Result<ConnectionId>::failure("not transport owner thread");
        // Include closing owners until the safe point: churn cannot build an unbounded retired list.
        if (at_capacity()) return common::Result<ConnectionId>::failure("connection bound reached");
        auto flags = normalize(socket.get());
        if (!flags) return common::Result<ConnectionId>::failure(flags.error().message);
        const ConnectionId id{allocate_id()};
        auto owned = std::make_unique<TcpConnection>(id,std::move(socket),connecting,limits);
        auto* connection = owned.get();
        connections.emplace(id.value,std::move(owned));
        const std::weak_ptr<int> lifetime = alive;
        connection->interest = io::kReadable | io::kReadHangup | (connecting ? io::kWritable : 0U);
        auto token = loop.add(connection->socket.get(),connection->interest,
            [this,lifetime,id](const io::RegistrationToken& registration, io::EventMask events) {
                if (lifetime.expired()) return;
                auto* current = find(id);
                if (current && !current->closing && current->token && *current->token == registration) dispatch(*current,events);
            }, {3U,id.value});
        if (!token) { connections.erase(id.value); return common::Result<ConnectionId>::failure(token.error().message); }
        connection->token = token.value();
        if (!connecting) {
            auto notified = notify_connected(*connection);
            if (!notified) return common::Result<ConnectionId>::failure(notified.error().message);
        }
        return common::Result<ConnectionId>::success(id);
    }
    void receive(TcpConnection& connection) {
        std::size_t budget = limits.read_budget, calls = 0;
        while (!connection.closing && connection.read_enabled && budget > 0 && calls++ < limits.syscall_budget) {
            const auto room = connection.input.size() - connection.input_used;
            if (room == 0) { explicit_failure(close(connection.id,CloseCause::InputOverflow)); return; }
            const auto count = operations.recv(connection.socket.get(),connection.input.data() + connection.input_used,
                                                std::min({room,budget,std::size_t{16384}}),0);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                explicit_failure(close(connection.id,CloseCause::ReceiveError,errno)); return;
            }
            if (count == 0) { explicit_failure(close(connection.id,CloseCause::PeerEof)); return; }
            const auto read = static_cast<std::size_t>(count);
            if (read > std::min({room,budget,std::size_t{16384}})) throw std::runtime_error("recv hook count exceeds buffer");
            connection.input_used += read; budget -= read;
            try {
                auto callback = callbacks.bytes;
                const auto consumed = callback ? callback(connection.id,{connection.input.data(),connection.input_used}) : 0U;
                if (consumed > connection.input_used) throw std::logic_error("input callback consumed beyond buffer");
                if (connection.closing) return;
                connection.input_used -= consumed;
                std::memmove(connection.input.data(),connection.input.data() + consumed,connection.input_used);
            } catch (...) { explicit_failure(close(connection.id,CloseCause::CallbackError)); throw; }
        }
    }
    void transmit(TcpConnection& connection) {
        std::size_t budget = limits.write_budget, calls = 0;
        while (!connection.closing && budget > 0 && calls++ < limits.syscall_budget) {
            const auto bytes = connection.output.front();
            if (bytes.empty()) break;
            const auto count = operations.send(connection.socket.get(),bytes.data(),std::min(bytes.size(),budget),MSG_NOSIGNAL);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                explicit_failure(close(connection.id,CloseCause::SendError,errno)); return;
            }
            if (count == 0) { explicit_failure(close(connection.id,CloseCause::SendError,EIO)); return; }
            const auto sent = static_cast<std::size_t>(count);
            if (sent > std::min(bytes.size(),budget)) throw std::runtime_error("send hook count exceeds buffer");
            explicit_failure(connection.output.advance(sent)); budget -= sent;
        }
        connection.output.retry_boundary();
        explicit_failure(update(connection));
    }
    void dispatch(TcpConnection& connection, io::EventMask events) {
        if (connection.connecting) {
            int error = 0; socklen_t length = sizeof(error); int status;
            do { status = ::getsockopt(connection.socket.get(),SOL_SOCKET,SO_ERROR,&error,&length); } while (status < 0 && errno == EINTR);
            if (status < 0 || error != 0) { explicit_failure(close(connection.id,CloseCause::ConnectError,status < 0 ? errno : error)); return; }
            connection.connecting = false; explicit_failure(notify_connected(connection));
            if (connection.closing) return;
        }
        if ((events & (io::kReadable | io::kReadHangup | EPOLLHUP)) != 0 && connection.read_enabled) receive(connection);
        if (connection.closing) return;
        if ((events & EPOLLHUP) != 0 && !connection.read_enabled) {
            explicit_failure(close(connection.id,CloseCause::PeerEof)); return;
        }
        if ((events & EPOLLERR) != 0) {
            int error = 0; socklen_t length = sizeof(error); int status;
            do { status = ::getsockopt(connection.socket.get(),SOL_SOCKET,SO_ERROR,&error,&length); } while (status < 0 && errno == EINTR);
            explicit_failure(close(connection.id,CloseCause::ReceiveError,status < 0 ? errno : error)); return;
        }
        if ((events & io::kWritable) != 0) transmit(connection);
        else explicit_failure(update(connection));
    }
    void accept(ListenerId id) {
        std::size_t calls = 0;
        while (calls++ < limits.syscall_budget) {
            const auto it = listeners.find(id.value); if (it == listeners.end()) return;
            const int fd = ::accept4(it->second.socket.get(),nullptr,nullptr,SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR || errno == ECONNABORTED) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                throw std::runtime_error(diagnostic("accept4"));
            }
            common::UniqueFd socket(fd);
            // Expected admission pressure, not a callback/internal failure. The local owner
            // closes the excess socket; this attempt still consumes the bounded accept budget.
            // No callbacks occur between this owner-thread capacity check and adopt().
            if (at_capacity()) continue;
            auto accepted = adopt(std::move(socket),false);
            if (!accepted) throw std::runtime_error(accepted.error().message);
        }
    }
};
ConnectionManager::ConnectionManager(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ConnectionManager::~ConnectionManager() = default;
common::Result<std::unique_ptr<ConnectionManager>> ConnectionManager::create(io::EventLoop& loop, Callbacks callbacks, TransportLimits limits, SocketIo operations) {
    if (limits.maximum_frame_bytes == 0 || limits.input_bytes < limits.maximum_frame_bytes ||
        limits.output.control.bytes < limits.maximum_frame_bytes || limits.output.log.bytes < limits.maximum_frame_bytes ||
        limits.output.control.slots == 0 || limits.output.log.slots == 0 || limits.maximum_connections == 0 || limits.maximum_listeners == 0 ||
        limits.read_budget == 0 || limits.write_budget == 0 || limits.syscall_budget == 0)
        return common::Result<std::unique_ptr<ConnectionManager>>::failure("invalid bounded transport limits");
    return common::Result<std::unique_ptr<ConnectionManager>>::success(std::unique_ptr<ConnectionManager>(new ConnectionManager(std::make_unique<Impl>(loop,std::move(callbacks),limits,std::move(operations)))));
}
common::Result<ListenerId> ConnectionManager::listen(const Endpoint& target, int backlog) {
    if (!impl_->on_owner()) return common::Result<ListenerId>::failure("not transport owner thread");
    if (backlog <= 0 || impl_->listeners.size() >= impl_->limits.maximum_listeners) return common::Result<ListenerId>::failure("invalid backlog or listener bound");
    auto parsed = address(target); if (!parsed) return common::Result<ListenerId>::failure(parsed.error().message);
    const auto& addr = parsed.value();
    common::UniqueFd socket(::socket(addr.family,SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0));
    if (!socket) return common::Result<ListenerId>::failure(diagnostic("socket"));
    const int enabled = 1;
    if (::setsockopt(socket.get(),SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled)) < 0 ||
        ::bind(socket.get(),reinterpret_cast<const sockaddr*>(&addr.storage),addr.size) < 0 || ::listen(socket.get(),backlog) < 0)
        return common::Result<ListenerId>::failure(diagnostic("listen setup"));
    const ListenerId id{impl_->allocate_id()}; const std::weak_ptr<int> lifetime = impl_->alive; auto* owner = impl_.get();
    // Establish the listener's stable owner before installing a kernel registration.
    auto inserted = impl_->listeners.emplace(id.value,Impl::Listener{std::move(socket),std::nullopt});
    auto token = impl_->loop.add(inserted.first->second.socket.get(),io::kReadable,[owner,lifetime,id](const io::RegistrationToken&,io::EventMask) {
        if (!lifetime.expired()) owner->accept(id);
    },{4U,id.value});
    if (!token) { impl_->listeners.erase(id.value); return common::Result<ListenerId>::failure(token.error().message); }
    inserted.first->second.token = token.value();
    return common::Result<ListenerId>::success(id);
}
common::Result<Endpoint> ConnectionManager::endpoint(ListenerId id) const {
    if (!impl_->on_owner()) return common::Result<Endpoint>::failure("not transport owner thread");
    const auto it = impl_->listeners.find(id.value); if (it == impl_->listeners.end()) return common::Result<Endpoint>::failure("unknown listener");
    sockaddr_storage storage{}; socklen_t length = sizeof(storage);
    if (::getsockname(it->second.socket.get(),reinterpret_cast<sockaddr*>(&storage),&length) < 0) return common::Result<Endpoint>::failure(diagnostic("getsockname"));
    char text[INET6_ADDRSTRLEN]{}; std::uint16_t port;
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (!::inet_ntop(AF_INET,&addr->sin_addr,text,sizeof(text))) return common::Result<Endpoint>::failure(diagnostic("inet_ntop"));
        port = ntohs(addr->sin_port);
    } else {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (!::inet_ntop(AF_INET6,&addr->sin6_addr,text,sizeof(text))) return common::Result<Endpoint>::failure(diagnostic("inet_ntop"));
        port = ntohs(addr->sin6_port);
    }
    return common::Result<Endpoint>::success({text,port});
}
common::Result<void> ConnectionManager::close_listener(ListenerId id) {
    if (!impl_->on_owner()) return common::Result<void>::failure("not transport owner thread");
    const auto it = impl_->listeners.find(id.value); if (it == impl_->listeners.end()) return common::Result<void>::failure("unknown listener");
    auto removed = impl_->loop.remove(*it->second.token); if (!removed) return removed;
    impl_->listeners.erase(it); return common::Result<void>::success();
}
common::Result<ConnectionId> ConnectionManager::connect(const Endpoint& target) {
    if (!impl_->on_owner()) return common::Result<ConnectionId>::failure("not transport owner thread");
    auto parsed = address(target); if (!parsed) return common::Result<ConnectionId>::failure(parsed.error().message);
    const auto& addr = parsed.value();
    common::UniqueFd socket(::socket(addr.family,SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0));
    if (!socket) return common::Result<ConnectionId>::failure(diagnostic("socket"));
    const auto status = impl_->operations.connect(socket.get(),reinterpret_cast<const sockaddr*>(&addr.storage),addr.size);
    if (status < 0 && errno != EINPROGRESS && errno != EINTR) return common::Result<ConnectionId>::failure(diagnostic("connect"));
    return impl_->adopt(std::move(socket),status < 0);
}
common::Result<ConnectionId> ConnectionManager::adopt(common::UniqueFd socket) { return impl_->adopt(std::move(socket),false); }
common::Result<void> ConnectionManager::send(ConnectionId id, Outbound frame) {
    if (!impl_->on_owner()) return common::Result<void>::failure("not transport owner thread");
    auto* connection = impl_->find(id); if (!connection || connection->closing) return common::Result<void>::failure("connection is not open");
    auto queued = connection->output.enqueue(std::move(frame));
    if (!queued) { auto closed = impl_->close(id,CloseCause::OutputOverflow); if (!closed) return closed; return queued; }
    return impl_->update(*connection);
}
common::Result<void> ConnectionManager::close(ConnectionId id, CloseCause cause) {
    if (!impl_->on_owner()) return common::Result<void>::failure("not transport owner thread");
    return impl_->close(id,cause);
}
common::Result<void> ConnectionManager::set_read_enabled(ConnectionId id, bool enabled) {
    if (!impl_->on_owner()) return common::Result<void>::failure("not transport owner thread");
    auto* connection = impl_->find(id); if (!connection || connection->closing) return common::Result<void>::failure("connection is not open");
    connection->read_enabled = enabled; return impl_->update(*connection);
}
std::optional<ConnectionSnapshot> ConnectionManager::inspect(ConnectionId id) const {
    if (!impl_->on_owner()) return std::nullopt;
    auto* connection = impl_->find(id); if (!connection) return std::nullopt;
    return ConnectionSnapshot{connection->socket.get(),connection->connecting,connection->closing,connection->interest,
        connection->input_used,connection->output.bytes(QueueClass::Control),connection->output.bytes(QueueClass::Log),
        connection->output.slots(QueueClass::Control),connection->output.slots(QueueClass::Log),
        connection->token.value_or(io::RegistrationToken{-1,0,0,0,0})};
}
std::size_t ConnectionManager::connection_count() const noexcept { return impl_->connections.size(); }
} // namespace ts::net
