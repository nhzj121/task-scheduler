#include "ts/client/client.h"

#include <algorithm>

namespace ts::client {
struct Client::Impl {
    std::unique_ptr<io::EventLoop> loop;
    std::unique_ptr<net::ConnectionManager> transport;
    protocol::FrameParser parser{protocol::Direction::ControllerToClient};
    std::optional<net::ConnectionId> connection;
    std::optional<protocol::Message> reply;
    std::optional<std::string> error;
    std::chrono::milliseconds timeout;
    std::uint64_t next_id{1}, pending_id{0};
    protocol::Type expected{protocol::Type::ErrorResponse};
    bool connected{false};
    explicit Impl(std::unique_ptr<io::EventLoop> event_loop, std::chrono::milliseconds deadline)
        : loop(std::move(event_loop)), timeout(deadline) {}
    ~Impl() { transport.reset(); }
    bool budget_expired(Deadline* deadline) {
        if (!deadline || std::chrono::steady_clock::now() < deadline->at) return false;
        deadline->expired = true; error = "overall wait deadline expired"; return true;
    }
    bool wait(const std::function<bool()>& ready, Deadline* deadline) {
        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        const auto normal_end = now + std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now));
        const auto end = deadline ? std::min(normal_end, deadline->at) : normal_end;
        for (;;) {
            if (error) return false; // a completed failure precedes budget expiry
            if (Clock::now() >= end) {
                // Attribute a late wakeup to the earlier deadline, even if both
                // deadlines have elapsed while this process was descheduled.
                if (deadline && deadline->at <= normal_end) {
                    deadline->expired = true; error = "overall wait deadline expired";
                } else error = "TCP request deadline expired; a submitted Task may already have committed";
                return false;
            }
            if (ready()) return true;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            const auto poll_ms = static_cast<int>(std::max<std::int64_t>(1, std::min<std::int64_t>(50, remaining)));
            const auto polled = loop->poll_once(poll_ms);
            if (!polled) error = polled.error().message;
        }
    }
    std::size_t bytes(net::ConnectionId id, std::string_view data) {
        auto fed = parser.feed(data, [&](protocol::Frame&& frame) {
            if (!pending_id || reply || frame.header.request_id != pending_id ||
                (frame.header.type != expected && frame.header.type != protocol::Type::ErrorResponse)) {
                error = "response type/request ID does not match outstanding request"; return false;
            }
            auto decoded = protocol::decode_message(frame, protocol::Direction::ControllerToClient);
            if (!decoded) { error = decoded.error().message; return false; }
            reply = std::move(decoded).value(); return true;
        });
        if (!fed) {
            if (!error) error = fed.error().message;
            const auto closed = transport->close(id);
            if (!closed) error = closed.error().message;
        }
        return data.size();
    }
};
Client::Client(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Client::~Client() = default;
common::Result<std::unique_ptr<Client>> Client::connect(net::Endpoint endpoint, std::chrono::milliseconds timeout, Deadline* deadline) {
    using R = common::Result<std::unique_ptr<Client>>;
    if (timeout.count() <= 0) return R::failure("client timeout must be positive");
    auto loop = io::EventLoop::create(); if (!loop) return R::failure(loop.error().message);
    auto impl = std::make_unique<Impl>(std::move(loop).value(), timeout); auto* owner = impl.get();
    auto transport = net::ConnectionManager::create(*impl->loop, {
        [owner](auto id) { owner->connection = id; owner->connected = true; },
        [owner](auto id, auto bytes) { return owner->bytes(id, bytes); },
        [owner](auto) { if (!owner->reply && !owner->error) owner->error = "TCP connection closed; submission outcome may be unknown"; }
    });
    if (!transport) return R::failure(transport.error().message);
    impl->transport = std::move(transport).value();
    if (impl->budget_expired(deadline)) return R::failure(*impl->error);
    const auto connected = impl->transport->connect(endpoint);
    if (!connected) return R::failure(connected.error().message);
    if (!impl->wait([&] { return impl->connected; }, deadline)) return R::failure(*impl->error);
    return R::success(std::unique_ptr<Client>(new Client(std::move(impl))));
}
common::Result<protocol::Message> Client::request(protocol::Message message, Deadline* deadline) {
    using R = common::Result<protocol::Message>;
    auto& s = *impl_;
    if (s.error) return R::failure(*s.error);
    if (s.budget_expired(deadline)) return R::failure(*s.error);
    if (s.pending_id || s.next_id == UINT64_MAX) return R::failure("client request identity unavailable");
    const auto id = s.next_id++;
    auto frame = protocol::encode_message(message, id, protocol::Direction::ClientToController);
    if (!frame) return R::failure(frame.error().message);
    auto encoded = protocol::encode_frame(frame.value(), protocol::Direction::ClientToController);
    if (!encoded) return R::failure(encoded.error().message);
    if (s.budget_expired(deadline)) return R::failure(*s.error);
    s.pending_id = id; s.expected = protocol::response_type(frame.value().header.type); s.reply.reset();
    const auto sent = s.transport->send(*s.connection, {std::move(encoded).value()});
    if (!sent) { s.error = sent.error().message; return R::failure(*s.error); }
    if (!s.wait([&] { return s.reply.has_value(); }, deadline)) return R::failure(*s.error);
    auto reply = std::move(*s.reply); s.reply.reset(); s.pending_id = 0;
    return R::success(std::move(reply));
}
common::Result<std::size_t> Client::list(
    const std::function<void(const protocol::ListTasksResponse&)>& consume, std::uint64_t size) {
    using R = common::Result<std::size_t>; std::optional<std::string> token; std::size_t count = 0;
    do {
        auto response = request(protocol::ListTasks{size, token});
        if (!response) return R::failure(response.error().message);
        if (const auto* error = std::get_if<protocol::ErrorResponse>(&response.value())) return R::failure(error->message);
        const auto& page = std::get<protocol::ListTasksResponse>(response.value());
        if (page.next_page_token && page.next_page_token == token) return R::failure("server pagination cursor did not advance");
        consume(page); count += page.tasks.size(); token = page.next_page_token;
    } while (token);
    return R::success(count);
}
common::Result<std::uint64_t> Client::logs(protocol::GetLogs request,
    const std::function<void(const protocol::GetLogsResponse&)>& consume) {
    using R = common::Result<std::uint64_t>;
    std::uint64_t total = 0;
    request.limit_bytes = std::min(request.limit_bytes, protocol::kMaxLogPage);
    try {
        for (;;) {
            auto response = this->request(request);
            if (!response) return R::failure(response.error().message);
            if (const auto* error = std::get_if<protocol::ErrorResponse>(&response.value())) return R::failure(error->message);
            const auto* page = std::get_if<protocol::GetLogsResponse>(&response.value());
            if (!page || !(page->task_id == request.task_id) || page->stream != request.stream || page->offset != request.offset ||
                (request.attempt_no && !(page->attempt_no == *request.attempt_no)) ||
                (request.snapshot_end && page->snapshot_end != *request.snapshot_end) ||
                (!page->eof_snapshot && page->next_offset <= request.offset)) return R::failure("log snapshot identity/progress mismatch");
            const auto count = page->next_offset - page->offset;
            if (count > UINT64_MAX - total) return R::failure("log snapshot byte count overflow");
            consume(*page); total += count;
            if (page->eof_snapshot) return R::success(total);
            request.attempt_no = page->attempt_no; request.snapshot_end = page->snapshot_end; request.offset = page->next_offset;
        }
    } catch (const std::exception& e) { return R::failure(e.what()); }
}
} // namespace ts::client
