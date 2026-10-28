#include "ts/worker/application.h"

#include <chrono>
#include <sys/epoll.h>
#include <pthread.h>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include "ts/io/signal_fd.h"
#include "ts/io/timer_fd.h"
#include "ts/worker/execution_manager.h"

namespace ts::worker {
namespace {
using Clock = std::chrono::steady_clock;
template<class T> T checked(common::Result<T> r) {
    if (!r) throw std::runtime_error(r.error().message);
    return std::move(r).value();
}
void checked(common::Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
std::string wire(const protocol::Message& m, std::uint64_t request) {
    auto frame = protocol::encode_message(m, request, protocol::Direction::WorkerToController);
    if (!frame) throw std::runtime_error(frame.error().message);
    auto result = protocol::encode_frame(frame.value(), protocol::Direction::WorkerToController);
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
}
struct Application::Impl {
    domain::WorkerConfig config;
    ApplicationOptions options;
    domain::SessionId session{domain::SessionId::generate()};
    std::unique_ptr<io::EventLoop> loop;
    std::unique_ptr<net::ConnectionManager> transport;
    std::unique_ptr<ExecutionManager> execution;
    std::optional<net::ConnectionId> connection;
    protocol::FrameParser parser{protocol::Direction::ControllerToWorker};
    std::optional<io::SignalFd> signals;
    std::optional<io::TimerFd> timer;
    std::optional<io::RegistrationToken> signal_token, timer_token;
    sigset_t previous_mask{}; bool mask_owned{false};
    bool registered{false}, stopping{false}, stop_requested{false}, fatal{false}, tick_due{false};
    std::uint64_t heartbeat_ms{3000}, heartbeat_sequence{0}, grace_ms{1000};
    Clock::time_point next_heartbeat{}, connect_deadline{Clock::now() + std::chrono::seconds(5)}, stop_deadline{};
    Impl(domain::WorkerConfig c, ApplicationOptions o) : config(std::move(c)), options(std::move(o)) {}
    ~Impl() {
        transport.reset();
        for (const auto* token : {&timer_token, &signal_token})
            if (*token && loop && !loop->remove(**token)) std::terminate();
        signals.reset(); timer.reset(); loop.reset();
        // Reverse nesting: runtime captured the pre-INT/TERM mask, so restore
        // the application mask first, then destroy its SIGCHLD owner last.
        if (mask_owned && ::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr) != 0) std::terminate();
        execution.reset();
    }
    void fail(const std::string& why) {
        if (!fatal) spdlog::error("Worker stopping: {}", why);
        fatal = true; stop_requested = true;
    }
    common::Result<void> send(net::Outbound frame) {
        if (!connection || stop_requested) return common::Result<void>::failure("Worker session unavailable");
        return transport->send(*connection, std::move(frame));
    }
    void initialize() {
        loop = checked(io::EventLoop::create());
        transport = checked(net::ConnectionManager::create(*loop, {
            [this](auto id) {
                connection = id;
                checked(send({wire(protocol::RegisterWorker{config.worker_id, session, config.capacity,
                    config.max_concurrent_tasks, "task-scheduler-m7"}, 1)}));
            },
            [this](auto id, auto data) { return bytes(id, data); },
            [this](auto) { connection.reset(); if (!stopping) fail("Controller connection closed; no M7 reconnect"); }
        }, {}, std::move(options.socket_io)));
        // MUST precede SIGINT/SIGTERM blocking. M4's restored child mask must
        // allow normal group termination. No fork path is implemented here.
        execution = checked(ExecutionManager::create(config, session,
            [this](auto frame) { return send(std::move(frame)); },
            [this]() { return connection ? transport->inspect(*connection) : std::optional<net::ConnectionSnapshot>{}; }));
        sigset_t mask; ::sigemptyset(&mask); ::sigaddset(&mask, SIGINT); ::sigaddset(&mask, SIGTERM);
        if (::pthread_sigmask(SIG_BLOCK, &mask, &previous_mask) != 0) throw std::runtime_error("Worker signal mask");
        mask_owned = true;
        signals.emplace(checked(io::SignalFd::create(mask)));
        signal_token = checked(loop->add(signals->fd(), EPOLLIN, [this](auto, auto) {
            static_cast<void>(checked(signals->drain())); stop_requested = true;
        }));
        timer.emplace(checked(io::TimerFd::create()));
        timer_token = checked(loop->add(timer->fd(), EPOLLIN, [this](auto, auto) {
            static_cast<void>(checked(timer->drain())); tick_due = true;
            checked(timer->arm_once(std::chrono::milliseconds(10)));
        }));
        checked(timer->arm_once(std::chrono::milliseconds(10)));
        checked(transport->connect({config.controller.address, config.controller.port}));
    }
    std::size_t bytes(net::ConnectionId id, std::string_view data) {
        const auto fed = parser.feed(data, [this](protocol::Frame&& frame) {
            auto decoded = protocol::decode_message(frame, protocol::Direction::ControllerToWorker);
            if (!decoded) { fail(decoded.error().message); return false; }
            if (!registered) {
                const auto* ack = std::get_if<protocol::RegisterAck>(&decoded.value());
                if (!ack || frame.header.request_id != 1) { fail("expected correlated REGISTER_ACK first"); return false; }
                const auto* accepted = std::get_if<protocol::RegistrationAccepted>(&ack->result);
                if (!accepted) { fail("registration rejected: " + std::get<protocol::RegistrationRejected>(ack->result).message); return false; }
                if (accepted->heartbeat_interval_ms > static_cast<std::uint64_t>(INT64_MAX) / 1000000) {
                    fail("heartbeat exceeds clock range"); return false;
                }
                const auto activated = execution->activate(*accepted);
                if (!activated) { fail(activated.error().message); return false; }
                heartbeat_ms = accepted->heartbeat_interval_ms; grace_ms = accepted->termination_grace_ms;
                registered = true; next_heartbeat = Clock::now();
                spdlog::info("REGISTERED worker={} session={}", config.worker_id.value(), session.value());
            } else if (const auto* task = std::get_if<protocol::RunTask>(&decoded.value())) {
                const auto result = execution->accept(*task, frame.header.request_id);
                if (!result) { fail(result.error().message); return false; }
            } else if (const auto* grant = std::get_if<protocol::LogWindowUpdate>(&decoded.value())) {
                const auto result = execution->grant(*grant);
                if (!result) { fail(result.error().message); return false; }
            } else { fail("unsupported or out-of-phase Controller command in M7"); return false; }
            return !stop_requested;
        });
        if (!fed) {
            if (!stop_requested) fail(fed.error().message);
            checked(transport->close(id));
        }
        return data.size();
    }
    int run() {
        while (true) {
            try {
                const auto polled = loop->poll_once(100);
                if (!polled) fail(polled.error().message);
                if (!registered && !stopping && Clock::now() >= connect_deadline) fail("registration deadline");
                if (stop_requested && !stopping) {
                    stopping = true;
                    stop_deadline = Clock::now() + std::chrono::milliseconds(grace_ms) + std::chrono::seconds(5);
                    const auto result = execution->stop(); if (!result) fail(result.error().message);
                    if (connection) checked(transport->close(*connection));
                }
                if (tick_due) {
                    tick_due = false; const auto result = execution->tick();
                    if (!result) fail(result.error().message);
                }
                if (registered && !stop_requested && Clock::now() >= next_heartbeat) {
                    if (heartbeat_sequence == UINT64_MAX) fail("heartbeat sequence exhausted");
                    else {
                        checked(send({wire(protocol::Heartbeat{config.worker_id, session, ++heartbeat_sequence, execution->running()}, 0)}));
                        next_heartbeat = Clock::now() + std::chrono::milliseconds(heartbeat_ms);
                    }
                }
                if (stopping && execution->running().empty()) break;
                if (stopping && Clock::now() >= stop_deadline) {
                    // Never destroy a live ProcessRuntime (its ownership contract
                    // forbids abandonment). Cleanup continues, with explicit error.
                    fail("local cleanup exceeded deadline; still draining owned children");
                }
            } catch (const std::exception& e) { fail(e.what()); }
        }
        return fatal ? 1 : 0;
    }
};
Application::Application(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Application::~Application() = default;
common::Result<std::unique_ptr<Application>> Application::create(domain::WorkerConfig config, ApplicationOptions options) {
    using R = common::Result<std::unique_ptr<Application>>;
    try {
        auto impl = std::make_unique<Impl>(std::move(config), std::move(options)); impl->initialize();
        return R::success(std::unique_ptr<Application>(new Application(std::move(impl))));
    } catch (const std::exception& e) { return R::failure(e.what()); }
}
int Application::run() { return impl_->run(); }
} // namespace ts::worker
