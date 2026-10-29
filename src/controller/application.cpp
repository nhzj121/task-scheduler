#include "ts/controller/application.h"

#include <chrono>
#include <algorithm>
#include <iterator>
#include <deque>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <spdlog/spdlog.h>

#include "ts/controller/task_manager.h"
#include "ts/controller/log_coordinator.h"
#include "ts/controller/views.h"
#include "ts/io/signal_fd.h"
#include "ts/io/timer_fd.h"
#include "ts/persistence/channel.h"

namespace ts::controller {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t query_base = std::uint64_t{1} << 63U;
std::uint64_t monotonic_ms() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); }
template<class T> T checked(common::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void checked(common::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void data_root(domain::ControllerConfig& config) {
    const std::filesystem::path root(config.data_dir);
    if (!root.is_absolute() || root.lexically_normal() == root.root_path()) throw std::invalid_argument("unsafe Controller data root");
    const bool created = std::filesystem::create_directories(root);
    if (created && ::chmod(config.data_dir.c_str(), 0700) != 0) throw std::runtime_error("cannot protect data root");
    struct stat status{};
    if (::lstat(config.data_dir.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 0022) != 0) throw std::invalid_argument("data root must be an owned, non-symlink directory without group/world write");
    config.data_dir = std::filesystem::canonical(root).string();
}
std::string wire(const protocol::Message& message, std::uint64_t request, protocol::Direction direction) {
    auto frame = protocol::encode_message(message, request, direction);
    if (!frame) throw std::runtime_error(frame.error().message);
    auto bytes = protocol::encode_frame(frame.value(), direction);
    if (!bytes) throw std::runtime_error(bytes.error().message);
    return std::move(bytes).value();
}
}
struct Application::Impl final : PersistencePort, EffectPort {
    struct Observation {
        domain::TaskId task;
        bool acked{false}, started{false}, finished{false};
    };
    struct Peer {
        struct CancelRequest { domain::TaskId task; domain::AttemptId attempt; SessionKey session; };
        ConnectionIdentity identity;
        protocol::FrameParser parser;
        bool closing{false}, close_after_flush{false}, registered{false}, quarantined{false};
        std::set<std::uint64_t> requests;
        std::map<std::uint64_t, bool> mutations;
        std::optional<Registration> registration;
        std::optional<SessionKey> session;
        std::optional<std::uint64_t> registration_request;
        std::uint64_t next_request{1};
        std::map<std::uint64_t, domain::AttemptId> runs;
        std::map<std::uint64_t, CancelRequest> cancels;
        std::map<domain::AttemptId, Observation> observations;
        Peer(ConnectionIdentity token, protocol::Direction direction) : identity(token), parser(direction) {}
    };
    enum class QueryKind { RestoreTasks, RestoreAttempts, GetTask, GetAttempts, GetVerify, List };
    struct Query {
        QueryKind kind;
        storage::Envelope envelope;
        std::optional<ClientRequest> client;
        std::optional<storage::TaskRow> task;
        std::vector<storage::AttemptRow> history;
        bool sent{false};
        unsigned int retries{0};
        std::optional<protocol::GetLogs> log_request{};
    };
    domain::ControllerConfig config;
    ApplicationOptions options;
    domain::SessionId instance{domain::SessionId::generate()};
    std::unique_ptr<LogCoordinator> logs;
    std::unique_ptr<io::EventLoop> loop;
    std::unique_ptr<net::ConnectionManager> clients, workers;
    std::unique_ptr<persistence::Channel> channel;
    std::unique_ptr<TaskManager> manager;
    std::optional<io::SignalFd> signals;
    std::optional<io::TimerFd> timer;
    sigset_t previous_mask{}; bool mask_owned{false};
    std::optional<io::RegistrationToken> signal_token, timer_token, persistence_token, log_token;
    std::optional<net::ListenerId> client_listener, worker_listener;
    std::map<std::uint64_t, std::unique_ptr<Peer>> client_peers, worker_peers;
    std::map<std::uint64_t, Query> queries;
    std::deque<Effect> effects;
    std::vector<storage::TaskRow> restore_tasks;
    std::vector<storage::AttemptRow> restore_attempts;
    std::size_t restore_task_index{0}, restore_owned_bytes{0};
    std::optional<storage::TaskCursor> restore_cursor;
    std::uint64_t next_query{query_base};
    std::size_t registration_admissions{0};
    bool initialized_reads{false}, ready{false}, stop_requested{false}, stopping{false}, fatal{false};
    bool restored{false};
    Clock::time_point stop_deadline{}, startup_deadline{Clock::now() + std::chrono::seconds(30)};

    Impl(domain::ControllerConfig value, ApplicationOptions bounds)
        : config(std::move(value)), options(std::move(bounds)) {}
    ~Impl() {
        // Owners are torn down outside dispatch. Any deferred erasures are either
        // consumed while this owner lives or discarded with the final EventLoop.
        if (logs) { logs->stop(); logs->join(); }
        if (channel) { channel->stop(); channel->join(); }
        manager.reset(); clients.reset(); workers.reset(); client_peers.clear(); worker_peers.clear();
        for (const auto* token : {&log_token, &persistence_token, &timer_token, &signal_token}) {
            if (*token && loop && !loop->remove(**token)) std::terminate();
        }
        logs.reset(); channel.reset(); timer.reset(); signals.reset(); loop.reset();
        if (mask_owned && ::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr) != 0) std::terminate();
    }
    void fail(const std::string& reason) {
        if (!fatal) spdlog::error("Controller fail-stop: {}", reason);
        fatal = true; stop_requested = true; ready = false;
    }
    void initialize() {
        const auto& l = options.limits;
        if (!l.connections_per_role || !l.requests_per_connection || !l.queries || !l.effects || !l.registrations ||
            l.restore_bytes < 512 || l.reply_bytes < 512 || l.reply_bytes > 4U * 1024U * 1024U || config.queues.persistence_commands < 2)
            throw std::invalid_argument("invalid application bounds or unusable persistence reserve");
        for (const auto ms : {config.heartbeat.interval_ms, config.heartbeat.offline_timeout_ms, config.dispatch.ack_timeout_ms,
                             config.dispatch.start_timeout_ms, config.termination_grace_ms})
            if (!ms || ms > static_cast<std::uint64_t>(INT64_MAX) / 1000000) throw std::invalid_argument("timer duration exceeds Linux range");
        data_root(config);
        sigset_t mask{}; ::sigemptyset(&mask); ::sigaddset(&mask, SIGINT); ::sigaddset(&mask, SIGTERM);
        if (::pthread_sigmask(SIG_BLOCK, &mask, &previous_mask) != 0) throw std::runtime_error("cannot block shutdown signals");
        mask_owned = true; loop = checked(io::EventLoop::create()); signals.emplace(checked(io::SignalFd::create(mask)));
        timer.emplace(checked(io::TimerFd::create()));
        signal_token = checked(loop->add(signals->fd(), io::kReadable, [this](const auto&, auto) {
            checked(signals->drain()); stop_requested = true;
        }));
        timer_token = checked(loop->add(timer->fd(), io::kReadable, [this](const auto&, auto) {
            checked(timer->drain()); checked(timer->arm_once(std::chrono::milliseconds(20)));
        }));
        checked(timer->arm_once(std::chrono::milliseconds(20)));
        storage::ChannelLimits bounds; bounds.count = config.queues.persistence_commands;
        bounds.essential_count = std::min(std::size_t{4}, bounds.count - 1);
        storage::StoreOptions store{config.data_dir, 100, common::Timestamp::now().to_string(), options.persistence_hook};
        channel = checked(persistence::Channel::start(std::move(store), bounds));
        DecisionLimits decisions; decisions.pending_count = std::min(std::size_t{64}, bounds.count);
        decisions.essential_count = std::min(std::size_t{4}, decisions.pending_count - 1);
        decisions.heartbeat_interval_ms = config.heartbeat.interval_ms; decisions.offline_timeout_ms = config.heartbeat.offline_timeout_ms;
        decisions.ack_timeout_ms = config.dispatch.ack_timeout_ms; decisions.start_timeout_ms = config.dispatch.start_timeout_ms;
        manager = std::make_unique<TaskManager>(*this, *this, decisions);
        logs = checked(LogCoordinator::create(config.data_dir, config.queues, *manager, options.log_hook));
        log_token = checked(loop->add(logs->fd(), io::kReadable, [this](const auto&, auto) { log_completions(); }));
        persistence_token = checked(loop->add(channel->fd(), io::kReadable, [this](const auto&, auto) { persistence_ready(); }));
        net::TransportLimits limits; limits.maximum_connections = options.limits.connections_per_role;
        clients = checked(net::ConnectionManager::create(*loop, {
            [this](auto id) { connected(id, false); }, [this](auto id, auto bytes) { return received(id, bytes, false); },
            [this](auto event) { closed(event, false); }
        }, limits, options.client_io));
        workers = checked(net::ConnectionManager::create(*loop, {
            [this](auto id) { connected(id, true); }, [this](auto id, auto bytes) { return received(id, bytes, true); },
            [this](auto event) { closed(event, true); }
        }, limits, options.worker_io));
    }
    storage::Admission try_post(storage::Envelope envelope) override {
        if (fatal || envelope.correlation.operation_id >= query_base) return storage::Admission::Degraded;
        return channel->try_post(std::move(envelope));
    }
    EffectAdmission emit(Effect effect) override {
        // No recursive TaskManager call, socket operation or response from here.
        if (effect.client && !client_current(*effect.client)) return EffectAdmission::ClientGone;
        if (effects.size() >= options.limits.effects) return EffectAdmission::Full;
        effects.push_back(std::move(effect)); return EffectAdmission::Accepted;
    }
    bool client_current(const ClientRequest& client) const {
        const auto it = client_peers.find(client.connection.value);
        return it != client_peers.end() && !it->second->closing && it->second->identity == client.connection &&
               it->second->requests.count(client.request_id);
    }
    void connected(net::ConnectionId id, bool worker) {
        auto& transport = worker ? workers : clients;
        const auto snap = transport->inspect(id);
        if (!snap || !ready || stop_requested || stopping) { checked(transport->close(id)); return; }
        auto peer = std::make_unique<Peer>(ConnectionIdentity{id.value, snap->registration.generation},
            worker ? protocol::Direction::WorkerToController : protocol::Direction::ClientToController);
        (worker ? worker_peers : client_peers).emplace(id.value, std::move(peer));
    }
    void closed(net::CloseEvent event, bool worker) {
        auto& peers = worker ? worker_peers : client_peers; const auto it = peers.find(event.id.value);
        if (it == peers.end() || it->second->closing) return;
        auto& peer = *it->second; peer.closing = true;
        if (worker && !manager->degraded()) {
            const auto result = manager->connection_closed(peer.identity);
            if (result == Decision::Degraded) fail("session-close persistence unavailable");
        }
        // Reclaim UNUSED grants now, not accepted queued/inflight writes.
        if (worker && peer.registration) logs->reclaim(peer.registration->session_id);
        checked(loop->defer([this, id = event.id, worker] {
            (worker ? worker_peers : client_peers).erase(id.value);
        }));
    }
    void reply(ClientRequest client, protocol::Message message) {
        if (!client_current(client)) return;
        std::string encoded;
        try { encoded = wire(message, client.request_id, protocol::Direction::ControllerToClient); }
        catch (const std::exception&) {
            encoded = wire(protocol::ErrorResponse{protocol::ErrorCode::TemporarilyUnavailable,
                "response exceeds bounded v1 encoding capacity", true, nlohmann::json::object()}, client.request_id,
                protocol::Direction::ControllerToClient);
        }
        const auto sent = clients->send({client.connection.value}, {std::move(encoded)});
        // A lost client cannot roll back a durable Task or invalidate a Worker.
        if (!sent) spdlog::warn("client response transport closed: {}", sent.error().message);
        const auto it = client_peers.find(client.connection.value);
        if (it != client_peers.end() && it->second->identity == client.connection) {
            it->second->requests.erase(client.request_id); it->second->mutations.erase(client.request_id);
        }
    }
    void error(ClientRequest client, protocol::ErrorCode code, const std::string& message, bool retryable = false) {
        reply(client, protocol::ErrorResponse{code, message, retryable, nlohmann::json::object()});
    }
    std::size_t received(net::ConnectionId id, std::string_view bytes, bool worker) {
        auto& peers = worker ? worker_peers : client_peers; const auto it = peers.find(id.value);
        if (it == peers.end() || it->second->closing) return bytes.size();
        auto& peer = *it->second;
        if (worker && peer.quarantined) return bytes.size();
        const auto fed = peer.parser.feed(bytes, [&](protocol::Frame&& frame) {
            if (worker && peer.quarantined) return true;
            auto decoded = protocol::decode_message(frame, worker ? protocol::Direction::WorkerToController : protocol::Direction::ClientToController);
            if (!decoded) {
                if (!worker && decoded.error().kind == protocol::ErrorKind::Schema && frame.header.request_id &&
                    !peer.requests.count(frame.header.request_id)) {
                    peer.requests.insert(frame.header.request_id);
                    error({peer.identity, frame.header.request_id}, protocol::ErrorCode::InvalidArgument, decoded.error().message);
                    return true;
                }
                return false;
            }
            if (worker) return worker_message(peer, frame.header, std::move(decoded).value());
            if (!peer.requests.insert(frame.header.request_id).second || peer.requests.size() > options.limits.requests_per_connection) return false;
            client_message({peer.identity, frame.header.request_id}, std::move(decoded).value()); return true;
        });
        if (!fed || peer.close_after_flush) {
            if (!fed && worker && peer.session) {
                // Logical protocol loss fences all future input immediately, but
                // does not intentionally close the socket before bulk LOST commits.
                // Continue draining bounded TCP input; no old packet is applied.
                peer.quarantined = true;
                if (manager->invalidate(*peer.session) == Decision::Degraded) fail("protocol-loss persistence unavailable");
            } else if (!fed) checked((worker ? workers : clients)->close(id));
        }
        return bytes.size();
    }
    bool post_query(Query query) {
        if (queries.size() >= options.limits.queries || next_query == UINT64_MAX) return false;
        query.envelope.correlation = storage::correlate(query.envelope.command, next_query++);
        query.envelope.response_bytes = options.limits.reply_bytes;
        queries.emplace(query.envelope.correlation.operation_id, std::move(query)); return true;
    }
    void client_message(ClientRequest client, protocol::Message message) {
        if (!ready || fatal || stop_requested || stopping) { error(client, protocol::ErrorCode::TemporarilyUnavailable, "Controller is not ready", true); return; }
        std::visit([&](auto&& request) {
            using T = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<T, protocol::SubmitTask>) {
                const auto id = domain::TaskId::generate();
                storage::TaskSpec spec{id.value(), request.executable, request.args, request.env, request.cwd,
                    request.resources.cpu_slots(), request.resources.memory_mb(), request.timeout_ms};
                // Retained M6 snapshots are application memory too. Admission
                // charges spec copies/container overhead plus one M7 Attempt.
                // Conservative charges are retained for this process lifetime;
                // there is no history eviction or retry subsystem in M7.
                const storage::Envelope retained{{}, storage::Submit{spec, common::Timestamp::now().to_string()}, 512};
                const auto owned = storage::owned_bytes(retained);
                if (owned > (SIZE_MAX - 8192) / 4) {
                    error(client, protocol::ErrorCode::TemporarilyUnavailable, "retained metadata range", true); return;
                }
                const auto charge = owned * 4 + 8192;
                if (charge > options.limits.restore_bytes || restore_owned_bytes > options.limits.restore_bytes - charge) {
                    error(client, protocol::ErrorCode::TemporarilyUnavailable, "retained metadata capacity", true); return;
                }
                const auto result = manager->submit(std::move(spec), client);
                if (result == Decision::Accepted) restore_owned_bytes += charge;
                if (result == Decision::Backpressure) error(client, protocol::ErrorCode::TemporarilyUnavailable, "metadata admission pressure", true);
                else if (result == Decision::Rejected) error(client, protocol::ErrorCode::InvalidArgument, "Task metadata exceeds accepted storage/domain ranges");
                else if (result != Decision::Accepted) fail("submit persistence unavailable");
            } else if constexpr (std::is_same_v<T, protocol::CancelTask> || std::is_same_v<T, protocol::RetryTask>) {
                constexpr bool retry = std::is_same_v<T, protocol::RetryTask>;
                const auto row = manager->task(request.task_id);
                if (!row) { error(client, protocol::ErrorCode::NotFound, "Task not found"); return; }
                if (!retry && row->state == domain::TaskState::CANCELLED) {
                    reply(client, protocol::CancelTaskResponse{request.task_id, protocol::CancelStatus::AlreadyCancelled,
                        domain::TaskState::CANCELLED, row->cancel_requested_at ?
                        std::optional<common::Timestamp>{common::Timestamp::parse(*row->cancel_requested_at).value()} : std::nullopt}); return;
                }
                constexpr std::size_t charge = 8192;
                if (retry && (charge > options.limits.restore_bytes || restore_owned_bytes > options.limits.restore_bytes - charge)) {
                    error(client, protocol::ErrorCode::TemporarilyUnavailable, "retained history capacity", true); return;
                }
                // Proof comes from a current adapter-owned effect, before transport
                // admission, never from a transient empty socket output queue.
                if (!retry) for (const auto& effect : effects) {
                    if (effect.kind != EffectKind::RunEligible || !effect.task_id || !(*effect.task_id == request.task_id) ||
                        !effect.session || !effect.attempt_id || !manager->effect_is_current(effect)) continue;
                    const auto worker = worker_peers.find(effect.session->connection.value);
                    if (worker != worker_peers.end() && worker->second->identity == effect.session->connection &&
                        !worker->second->observations.count(*effect.attempt_id))
                        static_cast<void>(manager->record_dispatch(effect, DispatchFact::NoBytesQueued));
                }
                client_peers.at(client.connection.value)->mutations.emplace(client.request_id, retry);
                const auto result = manager->receive(request.task_id, retry ? TaskEvent{RetryEvent{client}} : TaskEvent{CancelEvent{client}});
                if (result == Decision::Accepted || result == Decision::Deferred) { if (retry) restore_owned_bytes += charge; }
                else if (result == Decision::Backpressure) error(client, protocol::ErrorCode::TemporarilyUnavailable, "mutation admission pressure", true);
                else if (result == Decision::Rejected) error(client, protocol::ErrorCode::Conflict, "Task transition not permitted");
                else fail("client mutation persistence unavailable");
            } else if constexpr (std::is_same_v<T, protocol::GetTask>) {
                if (!post_query({QueryKind::GetTask, {{}, storage::ReadTask{request.task_id.value()}, 512}, client, {}, {}, false}))
                    error(client, protocol::ErrorCode::TemporarilyUnavailable, "query capacity", true);
            } else if constexpr (std::is_same_v<T, protocol::GetLogs>) {
                Query query{QueryKind::GetTask, {{}, storage::ReadTask{request.task_id.value()}, 512}, client, {}, {}, false};
                query.log_request = request;
                if (!post_query(std::move(query))) error(client, protocol::ErrorCode::TemporarilyUnavailable, "query capacity", true);
            } else if constexpr (std::is_same_v<T, protocol::ListTasks>) {
                std::optional<storage::TaskCursor> cursor;
                if (request.page_token) {
                    const auto parsed = parse_page_token(*request.page_token);
                    if (!parsed) { error(client, protocol::ErrorCode::InvalidArgument, parsed.error().message); return; }
                    cursor = parsed.value();
                }
                if (!post_query({QueryKind::List, {{}, storage::ListTasks{{}, false, cursor, static_cast<std::uint32_t>(request.page_size)}, 512}, client, {}, {}, false}))
                    error(client, protocol::ErrorCode::TemporarilyUnavailable, "query capacity", true);
            } else if constexpr (std::is_same_v<T, protocol::ListWorkers>) {
                protocol::ListWorkersResponse response;
                for (const auto& item : manager->workers().records()) {
                    const auto& record = item.second;
                    if (!record.capacity) { fail("Worker advertisement invariant"); return; }
                    auto reserved = domain::ResourceUsage::zero(); std::uint32_t count = 0;
                    const auto usage = manager->resources().inspect(item.first);
                    if (usage) {
                        reserved = domain::ResourceUsage::from_values(usage.value().tentative.cpu + usage.value().committed.cpu,
                            usage.value().tentative.memory + usage.value().committed.memory);
                        count = usage.value().tentative.attempts + usage.value().committed.attempts;
                    }
                    response.workers.push_back({item.first, record.state,
                        record.current ? std::optional<domain::SessionId>(record.current->key.session_id) : std::nullopt,
                        *record.capacity, reserved, count, record.maximum, record.last_heartbeat_at});
                }
                reply(client, std::move(response));
            } else error(client, protocol::ErrorCode::Unsupported, "operation is deferred beyond M7");
        }, std::move(message));
    }
    bool worker_message(Peer& peer, protocol::Header header, protocol::Message message) {
        if (auto* request = std::get_if<protocol::RegisterWorker>(&message)) {
            if (peer.registration || !ready || stop_requested || stopping) return false;
            auto reject = [&](const std::string& diagnostic) {
                const auto encoded = wire(protocol::RegisterAck{protocol::RegistrationRejected{false, "REGISTRATION_REJECTED", diagnostic}},
                    header.request_id, protocol::Direction::ControllerToWorker);
                const auto sent = workers->send({peer.identity.value}, {encoded});
                peer.close_after_flush = true; return static_cast<bool>(sent);
            };
            if (request->worker_version.empty() || request->worker_version.size() > 128 || request->worker_version.find('\0') != std::string::npos)
                return reject("invalid Worker version diagnostic");
            // M6 retains session UUID tombstones. Bound successful registration
            // admissions across this Controller lifetime, not only open sockets.
            if (registration_admissions >= options.limits.registrations) return reject("registration metadata capacity");
            const auto reserved = logs->reserve(request->session_id);
            if (!reserved) return reject(reserved.error().message);
            peer.registration = Registration{request->worker_id, request->session_id, peer.identity, request->capacity, request->max_concurrent_tasks};
            peer.registration_request = header.request_id;
            const auto result = manager->register_worker(*peer.registration);
            if (result == Decision::Rejected) {
                logs->reclaim(request->session_id); return reject("identity/session registration rejected");
            }
            if (result != Decision::Accepted) { fail("registration persistence unavailable"); return false; }
            ++registration_admissions;
            const auto record = manager->workers().inspect(request->worker_id);
            if (record && record->current && record->current->key.connection == peer.identity) peer.session = record->current->key;
            return true;
        }
        if (!peer.registered || !peer.session) return false;
        if (!manager->workers().valid(*peer.session, monotonic_ms())) {
            peer.quarantined = true;
            if (manager->invalidate(*peer.session) == Decision::Degraded) fail("expired-session persistence unavailable");
            return !fatal;
        }
        const auto& key = *peer.session;
        if (const auto* ack = std::get_if<protocol::WorkerCancelTaskAck>(&message)) {
            const auto pending = peer.cancels.find(header.request_id);
            if (!(ack->worker_id == key.worker_id) || !(ack->session_id == key.session_id) || pending == peer.cancels.end() ||
                !(pending->second.session == key) || !(pending->second.attempt == ack->attempt_id)) return false;
            const auto task = pending->second.task;
            peer.cancels.erase(pending);
            const auto row = manager->task(task);
            if (row && row->state == domain::TaskState::ACTIVE) {
                const auto observation = peer.observations.find(ack->attempt_id);
                // FINISH may already be validated but deferred behind an earlier
                // metadata mutation, before TaskManager installs finalization.
                const bool observed = observation != peer.observations.end() && observation->second.finished;
                if (!ack->accepted && row->latest_attempt_id == ack->attempt_id.value() &&
                    !observed && !manager->finalization_pending(task)) {
                    peer.quarantined = true;
                    if (manager->invalidate(key) == Decision::Degraded) fail("negative CANCEL ACK loss persistence unavailable");
                    return !fatal;
                }
                const auto result = manager->receive(task, CancelAckEvent{ack->attempt_id, key});
                if (result == Decision::Degraded) fail("cancel receipt unavailable");
                else if (result != Decision::Accepted && result != Decision::Deferred && result != Decision::Stale) return false;
            }
            return !fatal;
        }
        bool valid = true;
        std::visit([&](const auto& event) {
            using T = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<T, protocol::Heartbeat> || std::is_same_v<T, protocol::RunTaskAck> ||
                          std::is_same_v<T, protocol::TaskStarted> || std::is_same_v<T, protocol::LogChunk> ||
                          std::is_same_v<T, protocol::TaskFinished>) {
                if (!(event.worker_id == key.worker_id) || !(event.session_id == key.session_id)) { valid = false; return; }
                if constexpr (std::is_same_v<T, protocol::Heartbeat>) {
                    // A duplicate is ignored, not a new wire error and never a lease refresh.
                    const auto result = manager->heartbeat(key, event.sequence);
                    if (result == Decision::Degraded) fail("heartbeat decision unavailable");
                } else {
                    const auto found = peer.observations.find(event.attempt_id);
                    if (found == peer.observations.end()) { valid = false; return; }
                    auto& observation = found->second;
                    if constexpr (std::is_same_v<T, protocol::RunTaskAck>) {
                        const auto request = peer.runs.find(header.request_id);
                        if (request == peer.runs.end() || !(request->second == event.attempt_id) || observation.acked || observation.finished) {
                            valid = false; return;
                        }
                        peer.runs.erase(request); observation.acked = event.accepted;
                        if (!event.accepted) observation.finished = true;
                        const auto result = manager->receive(observation.task, AckEvent{event.attempt_id, key, event.accepted});
                        if (result == Decision::Degraded) fail("ACK persistence unavailable");
                        else if (result != Decision::Accepted && result != Decision::Deferred) valid = false;
                    } else {
                        if (!(event.task_id == observation.task) || !observation.acked || observation.finished) { valid = false; return; }
                        if constexpr (std::is_same_v<T, protocol::TaskStarted>) {
                            if (observation.started) { valid = false; return; }
                            observation.started = true;
                            const auto result = manager->receive(event.task_id, StartedEvent{event.attempt_id, key});
                            if (result == Decision::Degraded) fail("START persistence unavailable");
                            else if (result != Decision::Accepted && result != Decision::Deferred) valid = false;
                        } else if constexpr (std::is_same_v<T, protocol::LogChunk>) {
                            if (!observation.started) { valid = false; return; }
                            const auto admitted = logs->append(event);
                            if (admitted == LogAdmission::InvariantFailure) fail("credited chunk cannot enter reserved LogWriter capacity");
                            valid = admitted == LogAdmission::Accepted;
                        } else {
                            if ((event.outcome == protocol::Outcome::LaunchFailed && observation.started) ||
                                (event.outcome != protocol::Outcome::LaunchFailed && event.outcome != protocol::Outcome::Cancelled && !observation.started)) { valid = false; return; }
                            const auto history = manager->attempts(event.task_id);
                            const auto row = std::find_if(history.begin(), history.end(), [&](const auto& a) { return a.attempt_id == event.attempt_id.value(); });
                            if (row == history.end()) { valid = false; return; }
                            ObservedOutcome outcome;
                            switch (event.outcome) {
                                case protocol::Outcome::Exited: outcome = ObservedOutcome::Exited; break;
                                case protocol::Outcome::Signaled: outcome = ObservedOutcome::Signaled; break;
                                case protocol::Outcome::TimedOut: outcome = ObservedOutcome::TimedOut; break;
                                case protocol::Outcome::Cancelled: outcome = ObservedOutcome::Cancelled; break;
                                case protocol::Outcome::LaunchFailed: outcome = ObservedOutcome::LaunchFailed; break;
                                default: valid = false; return;
                            }
                            FinishedEvent finished{event.attempt_id, key, outcome, event.exit_code, event.term_signal, event.reason,
                                row->stdout_path, row->stderr_path, event.stdout_bytes, event.stderr_bytes,
                                event.stdout_next_sequence, event.stderr_next_sequence, {}, false};
                            const auto result = logs->finish(std::move(finished));
                            if (manager->degraded()) fail("FINISH retention unavailable");
                            if (!result) valid = false;
                            else {
                                observation.finished = true;
                                spdlog::info("FINISH observed task={} attempt={} awaiting log barrier/metadata", event.task_id.value(), event.attempt_id.value());
                            }
                        }
                    }
                }
            } else valid = false;
        }, message);
        return valid && !fatal;
    }
    void drive_queries() {
        if (fatal || stopping) return;
        for (auto it = queries.begin(); it != queries.end();) {
            if (it->second.sent) { ++it; continue; }
            const auto admission = channel->try_post(it->second.envelope);
            if (admission == storage::Admission::Accepted) { it->second.sent = true; ++it; }
            else if (admission == storage::Admission::Full) break;
            else if (admission == storage::Admission::Oversized && it->second.client) {
                error(*it->second.client, protocol::ErrorCode::TemporarilyUnavailable, "bounded query response capacity", true);
                it = queries.erase(it);
            } else { fail("read-channel admission unavailable"); break; }
        }
    }
    void restore_next_history() {
        if (restore_task_index < restore_tasks.size()) {
            if (!post_query({QueryKind::RestoreAttempts,
                {{}, storage::ReadAttempts{restore_tasks[restore_task_index].spec.task_id, 0, 200}, 512}, {}, {}, {}, false}))
                fail("startup history query bound");
            return;
        }
        if (manager->restore(std::move(restore_tasks), std::move(restore_attempts)) != Decision::Accepted) {
            fail("recovered snapshot rejected by M6"); return;
        }
        restored = true; start_listeners();
    }
    void start_listeners() {
        if (ready || !restored || stopping || fatal || logs->status().health != logs::Health::Ready) return;
        client_listener = checked(clients->listen({config.client_listen.address, config.client_listen.port}));
        worker_listener = checked(workers->listen({config.worker_listen.address, config.worker_listen.port}));
        ready = true;
        spdlog::info("READY client={}:{} worker={}:{} (durable task logs)", config.client_listen.address,
            config.client_listen.port, config.worker_listen.address, config.worker_listen.port);
    }
    bool charge_restore(const storage::Outcome& outcome) {
        const auto bytes = storage::owned_bytes(outcome);
        if (bytes > options.limits.restore_bytes || restore_owned_bytes > options.limits.restore_bytes - bytes) {
            fail("startup snapshot exceeds bounded restore memory; no history was dropped"); return false;
        }
        restore_owned_bytes += bytes; return true;
    }
    bool repost(Query query, storage::Command command) {
        query.envelope.command = std::move(command); query.sent = false;
        if (post_query(std::move(query))) return true;
        fail("continuation metadata bound"); return false;
    }
    void query_complete(Query query, storage::Outcome outcome) {
        if (stopping) return;
        const bool restoring = query.kind == QueryKind::RestoreTasks || query.kind == QueryKind::RestoreAttempts;
        if (outcome.error) {
            if (outcome.error->fatal) { fail(outcome.error->message); return; }
            if (restoring && outcome.error->code == storage::ErrorCode::Capacity) {
                bool reduced = false;
                std::visit([&](auto& command) {
                    using T = std::decay_t<decltype(command)>;
                    if constexpr (std::is_same_v<T, storage::ListTasks> || std::is_same_v<T, storage::ReadAttempts>) {
                        if (command.limit > 1) { command.limit = std::max(std::uint32_t{1}, command.limit / 2); reduced = true; }
                    }
                }, query.envelope.command);
                if (reduced) { const auto command = query.envelope.command; static_cast<void>(repost(std::move(query), command)); return; }
            }
            if (restoring) { fail("startup read failed: " + outcome.error->message); return; }
            if (query.client) error(*query.client,
                query.kind == QueryKind::GetTask && outcome.error->code == storage::ErrorCode::Conflict ? protocol::ErrorCode::NotFound : protocol::ErrorCode::TemporarilyUnavailable,
                outcome.error->message);
            return;
        }
        if (outcome.committed) { fail("read completion incorrectly claims mutation commit"); return; }
        if (query.kind == QueryKind::RestoreTasks) {
            if (!charge_restore(outcome)) return;
            restore_tasks.insert(restore_tasks.end(), std::make_move_iterator(outcome.tasks.begin()), std::make_move_iterator(outcome.tasks.end()));
            if (outcome.has_more) {
                if (!outcome.next) { fail("missing startup cursor"); return; }
                const auto old = std::get<storage::ListTasks>(query.envelope.command);
                static_cast<void>(repost(std::move(query), storage::ListTasks{{}, true, outcome.next, old.limit}));
            } else restore_next_history();
        } else if (query.kind == QueryKind::RestoreAttempts) {
            if (!charge_restore(outcome)) return;
            auto command = std::get<storage::ReadAttempts>(query.envelope.command);
            if (outcome.has_more) {
                if (outcome.attempts.empty()) { fail("history cursor did not advance"); return; }
                command.after_attempt_no = outcome.attempts.back().attempt_no;
            }
            restore_attempts.insert(restore_attempts.end(), std::make_move_iterator(outcome.attempts.begin()), std::make_move_iterator(outcome.attempts.end()));
            if (outcome.has_more) static_cast<void>(repost(std::move(query), std::move(command)));
            else { ++restore_task_index; restore_next_history(); }
        } else if (query.kind == QueryKind::GetTask) {
            if (outcome.tasks.size() != 1) { fail("GET_TASK completion cardinality"); return; }
            query.task = std::move(outcome.tasks.front()); query.kind = QueryKind::GetAttempts;
            const storage::ReadAttempts command{query.task->spec.task_id, 0, 200};
            static_cast<void>(repost(std::move(query), command));
        } else if (query.kind == QueryKind::GetAttempts) {
            query.history.insert(query.history.end(), std::make_move_iterator(outcome.attempts.begin()), std::make_move_iterator(outcome.attempts.end()));
            storage::Outcome retained; retained.tasks.push_back(*query.task); retained.attempts = std::move(query.history);
            if (storage::owned_bytes(retained) > options.limits.reply_bytes) {
                error(*query.client, protocol::ErrorCode::TemporarilyUnavailable, "complete history exceeds bounded v1 response"); return;
            }
            query.history = std::move(retained.attempts);
            if (outcome.has_more) {
                if (query.history.empty()) { fail("empty history continuation"); return; }
                const storage::ReadAttempts command{query.task->spec.task_id, query.history.back().attempt_no, 200};
                static_cast<void>(repost(std::move(query), command));
            } else {
                query.kind = QueryKind::GetVerify; const storage::ReadTask command{query.task->spec.task_id};
                static_cast<void>(repost(std::move(query), command));
            }
        } else if (query.kind == QueryKind::GetVerify) {
            if (outcome.tasks.size() != 1) { fail("GET verification cardinality"); return; }
            const auto& current = outcome.tasks.front();
            bool stable = current.state_version == query.task->state_version && current.latest_attempt_id == query.task->latest_attempt_id;
            const auto id = domain::TaskId::parse(current.spec.task_id).value();
            const auto live = manager->attempts(id);
            if (live.size() != query.history.size()) stable = false;
            else for (std::size_t i = 0; i < live.size(); ++i)
                if (live[i].attempt_id != query.history[i].attempt_id || live[i].state_version != query.history[i].state_version) stable = false;
            if (!stable) {
                if (++query.retries > 3) { error(*query.client, protocol::ErrorCode::TemporarilyUnavailable, "Task changed during paged history read", true); return; }
                query.kind = QueryKind::GetTask; query.task.reset(); query.history.clear();
                static_cast<void>(repost(std::move(query), storage::ReadTask{id.value()})); return;
            }
            if (query.log_request) {
                const auto& request = *query.log_request;
                const auto selected = std::find_if(query.history.begin(), query.history.end(), [&](const auto& attempt) {
                    return request.attempt_no ? attempt.attempt_no == request.attempt_no->value() :
                        current.latest_attempt_id == attempt.attempt_id;
                });
                if (selected == query.history.end()) { error(*query.client, protocol::ErrorCode::NotFound, "Attempt not found"); return; }
                const auto admitted = logs->read(*selected, request, *query.client);
                if (!admitted) error(*query.client, protocol::ErrorCode::TemporarilyUnavailable, admitted.error().message, true);
                return;
            }
            protocol::GetTaskResponse response{task_view(current), {}};
            for (const auto& attempt : query.history) response.attempts.push_back(attempt_view(attempt,
                current.latest_attempt_id == attempt.attempt_id && manager->finalization_pending(id)));
            reply(*query.client, std::move(response));
        } else {
            protocol::ListTasksResponse response;
            for (const auto& task : outcome.tasks) {
                const auto id = domain::TaskId::parse(task.spec.task_id).value();
                const auto live = manager->task(id);
                if (!live || live->state_version != task.state_version) {
                    error(*query.client, protocol::ErrorCode::TemporarilyUnavailable, "committed view advanced", true); return;
                }
                const auto count = manager->attempts(id).size();
                if (count > UINT32_MAX) { fail("Attempt count range"); return; }
                response.tasks.push_back({id, task.state, task.spec.executable, static_cast<std::uint32_t>(count),
                    common::Timestamp::parse(task.created_at).value(), common::Timestamp::parse(task.updated_at).value()});
            }
            if (outcome.has_more) {
                if (!outcome.next) { fail("missing task-list cursor"); return; }
                response.next_page_token = page_token(*outcome.next);
            }
            reply(*query.client, std::move(response));
        }
    }
    void persistence_ready() {
        auto completions = checked(channel->drain());
        for (auto& completion : completions) {
            if (completion.correlation.operation_id >= query_base) {
                const auto it = queries.find(completion.correlation.operation_id);
                if (it == queries.end()) { if (!stopping) fail("unknown query completion"); continue; }
                if (!(it->second.envelope.correlation == completion.correlation)) { fail("query correlation mismatch"); continue; }
                auto query = std::move(it->second); queries.erase(it); query_complete(std::move(query), std::move(completion.outcome));
            } else if (!fatal) {
                const auto result = manager->complete(completion);
                if (result == Decision::Degraded || result == Decision::Stale) fail("mutation completion rejected or persistence degraded");
            }
        }
        const auto status = channel->status();
        if (status.health == storage::Health::Degraded || status.health == storage::Health::Stopped) {
            if (!stopping) fail(status.error ? status.error->message : "persistence stopped");
        } else if (status.health == storage::Health::Ready && !initialized_reads && !stopping) {
            initialized_reads = true;
            if (!post_query({QueryKind::RestoreTasks, {{}, storage::ListTasks{{}, true, {}, 200}, 512}, {}, {}, {}, false})) fail("startup query bound");
        }
    }
    void log_completions() {
        auto responses = logs->drain();
        if (!responses) { fail(responses.error().message); return; }
        for (auto& response : responses.value()) if (!fatal && !stopping) reply(response.client, std::move(response.message));
        start_listeners();
    }
    void drain_effects() {
        const auto count = std::min(effects.size(), std::size_t{256});
        for (std::size_t i = 0; i < count && !fatal; ++i) {
            auto effect = std::move(effects.front()); effects.pop_front();
            if (effect.kind == EffectKind::FailStop) { fail("M6 fail-stop decision"); break; }
            if (effect.kind == EffectKind::SubmitAccepted && effect.client && effect.task_id) {
                const auto task = manager->task(*effect.task_id);
                if (!task) { fail("durable submit row missing"); break; }
                reply(*effect.client, protocol::SubmitTaskResponse{*effect.task_id, domain::TaskState::PENDING,
                    common::Timestamp::parse(task->created_at).value()});
            } else if (effect.kind == EffectKind::CancelAccepted || effect.kind == EffectKind::CancelCompleted) {
                if (!effect.client || !effect.task_id) { fail("cancel response fact missing"); break; }
                reply(*effect.client, protocol::CancelTaskResponse{*effect.task_id,
                    effect.kind == EffectKind::CancelAccepted ? protocol::CancelStatus::Accepted : protocol::CancelStatus::Completed,
                    effect.kind == EffectKind::CancelAccepted ? domain::TaskState::ACTIVE : domain::TaskState::CANCELLED,
                    effect.cancel_requested_at ? std::optional<common::Timestamp>{common::Timestamp::parse(*effect.cancel_requested_at).value()} : std::nullopt});
            } else if (effect.kind == EffectKind::RetryAccepted) {
                if (!effect.client || !effect.task_id || !effect.next_attempt_no) { fail("retry response fact missing"); break; }
                reply(*effect.client, protocol::RetryTaskResponse{*effect.task_id, domain::TaskState::PENDING,
                    domain::AttemptNumber::create(*effect.next_attempt_no).value()});
            } else if (effect.kind == EffectKind::Rejected && effect.client) {
                const auto p = client_peers.find(effect.client->connection.value);
                const auto row = effect.task_id ? manager->task(*effect.task_id) : std::nullopt;
                if (p != client_peers.end() && p->second->identity == effect.client->connection &&
                    p->second->mutations.count(effect.client->request_id) && !p->second->mutations.at(effect.client->request_id) &&
                    row && row->state == domain::TaskState::CANCELLED)
                    reply(*effect.client, protocol::CancelTaskResponse{*effect.task_id, protocol::CancelStatus::AlreadyCancelled,
                        domain::TaskState::CANCELLED, row->cancel_requested_at ?
                        std::optional<common::Timestamp>{common::Timestamp::parse(*row->cancel_requested_at).value()} : std::nullopt});
                else error(*effect.client, protocol::ErrorCode::Conflict, "durable transition rejected");
            } else if (effect.kind == EffectKind::RegistrationAccepted && effect.session) {
                const auto& key = *effect.session; const auto it = worker_peers.find(key.connection.value);
                if (it == worker_peers.end() || it->second->closing || !(it->second->identity == key.connection)) {
                    if (manager->connection_closed(key.connection) == Decision::Degraded) fail("vanished registration cleanup unavailable");
                    continue;
                }
                auto& peer = *it->second;
                if (!manager->workers().valid(key, monotonic_ms())) continue;
                if (!peer.registration || !(peer.registration->session_id == key.session_id) || !peer.registration_request) {
                    fail("registration effect correlation invariant"); break;
                }
                peer.session = key;
                checked(logs->activate(key));
                protocol::RegisterAck ack{protocol::RegistrationAccepted{true, config.heartbeat.interval_ms,
                    config.heartbeat.offline_timeout_ms, config.termination_grace_ms, config.queues.initial_session_log_window_bytes,
                    config.queues.initial_session_log_window_chunks, instance}};
                const auto sent = workers->send({key.connection.value}, {wire(ack, *peer.registration_request, protocol::Direction::ControllerToWorker)});
                if (sent) { peer.registered = true; spdlog::info("Worker registered id={} session={}", key.worker_id.value(), key.session_id.value()); }
            } else if (effect.kind == EffectKind::RunEligible && effect.session && effect.task_id && effect.attempt_id) {
                if (stop_requested || stopping || !manager->effect_is_current(effect)) continue;
                const auto it = worker_peers.find(effect.session->connection.value);
                if (it == worker_peers.end() || it->second->closing || !(it->second->identity == effect.session->connection)) {
                    if (manager->connection_closed(effect.session->connection) == Decision::Degraded) fail("dispatch connection unavailable");
                    continue;
                }
                auto& peer = *it->second;
                if (!peer.registered) { fail("RUN precedes registration ACK queueing"); break; }
                if (peer.runs.size() >= options.limits.requests_per_connection) {
                    if (manager->record_dispatch(effect, DispatchFact::NoBytesQueued) != Decision::Accepted) continue;
                    effects.push_back(std::move(effect)); continue;
                }
                if (peer.observations.size() >= 1024 || peer.next_request == UINT64_MAX) {
                    const auto result = manager->invalidate(*effect.session); if (result == Decision::Degraded) fail("dispatch metadata capacity cleanup unavailable");
                    continue;
                }
                const auto task = manager->task(*effect.task_id); const auto history = manager->attempts(*effect.task_id);
                const auto attempt = std::find_if(history.begin(), history.end(), [&](const auto& a) { return a.attempt_id == effect.attempt_id->value(); });
                if (!task || attempt == history.end()) { fail("durable RUN binding missing"); break; }
                const auto& s = task->spec;
                protocol::RunTask run{effect.session->worker_id, effect.session->session_id, *effect.task_id, *effect.attempt_id,
                    domain::AttemptNumber::create(attempt->attempt_no).value(), s.executable, s.args, s.env, s.cwd,
                    domain::ResourceRequest::create(s.cpu_slots, s.memory_mb).value(), s.timeout_ms};
                const auto request = peer.next_request++; auto encoded = wire(run, request, protocol::Direction::ControllerToWorker);
                peer.runs.emplace(request, *effect.attempt_id);
                peer.observations.emplace(*effect.attempt_id, Observation{*effect.task_id, false, false, false});
                const auto bound = logs->bind(*attempt, *effect.session);
                if (!bound) { if (manager->invalidate(*effect.session) == Decision::Degraded) fail(bound.error().message); continue; }
                // Record possible transmission BEFORE any byte enters M3's output
                // path. A failed enqueue/partial send is conservative session loss.
                if (!manager->effect_is_current(effect) || manager->record_dispatch(effect, DispatchFact::BytesMayHaveEscaped) != Decision::Accepted) {
                    peer.runs.erase(request); peer.observations.erase(*effect.attempt_id); continue;
                }
                const auto sent = workers->send({effect.session->connection.value}, {std::move(encoded)});
                if (!sent) spdlog::warn("RUN transport closed: {}", sent.error().message);
            } else if (effect.kind == EffectKind::WorkerOffline && effect.session) {
                logs->reclaim(effect.session->session_id);
            } else if (effect.kind == EffectKind::CloseConnection && effect.session) {
                const auto it = worker_peers.find(effect.session->connection.value);
                if (it != worker_peers.end() && !it->second->closing && it->second->identity == effect.session->connection)
                    checked(workers->close({effect.session->connection.value}));
            } else if (effect.kind == EffectKind::TerminalVisible && effect.session && effect.attempt_id) {
                logs->terminal(*effect.attempt_id);
                const auto it = worker_peers.find(effect.session->connection.value);
                if (it != worker_peers.end() && it->second->identity == effect.session->connection)
                    it->second->observations.erase(*effect.attempt_id);
            } else if (effect.kind == EffectKind::CancelEligible) {
                if (stopping || stop_requested || !effect.session || !effect.task_id || !effect.attempt_id ||
                    !manager->effect_is_current(effect)) continue;
                const auto found = worker_peers.find(effect.session->connection.value);
                if (found == worker_peers.end() || found->second->closing || !found->second->registered ||
                    !(found->second->identity == effect.session->connection)) continue;
                auto& peer = *found->second;
                if (std::any_of(peer.cancels.begin(), peer.cancels.end(), [&](const auto& c) { return c.second.attempt == *effect.attempt_id; })) continue;
                if (peer.cancels.size() >= options.limits.requests_per_connection) { effects.push_back(std::move(effect)); continue; }
                if (peer.next_request == UINT64_MAX) {
                    if (manager->invalidate(*effect.session) == Decision::Degraded) fail("cancel request space exhausted");
                    continue;
                }
                const auto request = peer.next_request++;
                const auto encoded = wire(protocol::WorkerCancelTask{effect.session->worker_id, effect.session->session_id,
                    *effect.task_id, *effect.attempt_id, config.termination_grace_ms}, request, protocol::Direction::ControllerToWorker);
                if (!manager->effect_is_current(effect)) continue;
                peer.cancels.emplace(request, Peer::CancelRequest{*effect.task_id, *effect.attempt_id, *effect.session});
                const auto sent = workers->send({effect.session->connection.value}, {encoded});
                if (!sent) spdlog::warn("CANCEL transport closed: {}", sent.error().message);
            }
        }
    }
    void begin_stop() {
        stopping = true; ready = false; stop_deadline = Clock::now() + std::chrono::seconds(5);
        if (client_listener) { checked(clients->close_listener(*client_listener)); client_listener.reset(); }
        if (worker_listener) { checked(workers->close_listener(*worker_listener)); worker_listener.reset(); }
        queries.clear(); // Sent reads are drained but never reinterpreted as mutation commits.
        if (!fatal && !manager->degraded()) for (const auto& record : manager->workers().records()) {
            if (record.second.current && manager->invalidate(record.second.current->key) == Decision::Degraded) {
                fail("shutdown session-loss persistence unavailable"); break;
            }
            if (record.second.current) logs->reclaim(record.second.current->key.session_id);
        }
    }
    void close_sockets() {
        std::vector<net::ConnectionId> ids;
        for (const auto& p : worker_peers) if (!p.second->closing) ids.push_back({p.first});
        for (const auto id : ids) checked(workers->close(id));
        ids.clear();
        for (const auto& p : client_peers) if (!p.second->closing) ids.push_back({p.first});
        for (const auto id : ids) checked(clients->close(id));
    }
    void flush_rejections() {
        for (const auto& p : worker_peers) if (!p.second->closing && p.second->close_after_flush) {
            const auto state = workers->inspect({p.first});
            if (state && state->control_bytes == 0 && state->log_bytes == 0) checked(workers->close({p.first}));
        }
    }
    int run() {
        try {
            while (true) {
                const auto polled = loop->poll_once(1000);
                if (!polled) fail(polled.error().message);
                const auto status = channel->status();
                if (!fatal && logs->status().health == logs::Health::Failed) fail("LogWriter channel/owner failure");
                if (!fatal && (status.health == storage::Health::Degraded || status.health == storage::Health::Stopped))
                    fail(status.error ? status.error->message : "persistence unexpectedly stopped");
                if (!fatal && manager->tick(monotonic_ms(), common::Timestamp::now()) == Decision::Degraded) fail("maintenance decision degraded");
                if (!ready && !stopping && Clock::now() >= startup_deadline) fail("startup recovery/restore deadline");
                if (stop_requested && !stopping) begin_stop();
                if (fatal) { close_sockets(); break; }
                drain_effects();
                if (!fatal) {
                    const auto progress = logs->pump([this](const SessionKey& key, LogCredit credit) {
                        if (stopping || !manager->workers().valid(key, monotonic_ms())) return false;
                        return static_cast<bool>(workers->send({key.connection.value}, {wire(protocol::LogWindowUpdate{credit.bytes, credit.slots},
                            0, protocol::Direction::ControllerToWorker)}));
                    });
                    if (!progress) fail(progress.error().message);
                }
                if (!stopping && !fatal) {
                    drive_queries();
                    const auto result = manager->pump();
                    if (result == Decision::Degraded) fail("decision pump degraded");
                }
                flush_rejections();
                if (fatal) continue;
                const auto log_usage = logs->usage();
                if (stopping && manager->retained().pending_count == 0 && channel->usage().count == 0 &&
                    !log_usage.writes && !log_usage.essential && !log_usage.reads) {
                    drain_effects(); close_sockets(); break;
                }
                if (stopping && Clock::now() >= stop_deadline) { fail("bounded shutdown cleanup deadline"); close_sockets(); break; }
            }
        } catch (const std::exception& e) {
            fail(e.what());
            // Destruction below is outside EventLoop dispatch and unregisters all
            // sockets. Workers then own the explicit local process cleanup.
        }
        logs->stop(); logs->join(); channel->stop(); channel->join();
        return fatal ? 1 : 0;
    }
};
Application::Application(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Application::~Application() = default;
common::Result<std::unique_ptr<Application>> Application::create(domain::ControllerConfig config, ApplicationOptions options) {
    using R = common::Result<std::unique_ptr<Application>>;
    try {
        auto impl = std::make_unique<Impl>(std::move(config), std::move(options)); impl->initialize();
        return R::success(std::unique_ptr<Application>(new Application(std::move(impl))));
    } catch (const std::exception& e) { return R::failure(e.what()); }
}
int Application::run() { return impl_->run(); }
} // namespace ts::controller
