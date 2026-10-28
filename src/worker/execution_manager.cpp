#include "ts/worker/execution_manager.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include "ts/runtime/process_runtime.h"

namespace ts::worker {
namespace {
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
constexpr std::size_t metadata_limit = 8U * 1024U * 1024U;
}
struct ExecutionManager::Impl {
    struct Running {
        protocol::RunTask task;
        std::size_t metadata_bytes;
        bool started{false};
        protocol::StreamPosition out, err;
        std::uint64_t ordinal{0};
    };
    domain::WorkerConfig config;
    domain::SessionId session;
    Send send;
    Room room;
    std::unique_ptr<runtime::ProcessRuntime> runtime;
    std::map<runtime::RuntimeProcessId, Running> processes;
    std::set<domain::AttemptId> seen;
    domain::ResourceUsage usage{domain::ResourceUsage::zero()};
    std::size_t metadata{0}, tick_bytes{0}, tick_chunks{0};
    std::uint64_t bytes{0}, chunks{0}, grace_ms{1000};
    bool active{false}, stopping{false};
    Impl(domain::WorkerConfig c, domain::SessionId s, Send sender, Room probe)
        : config(std::move(c)), session(s), send(std::move(sender)), room(std::move(probe)) {}
    std::size_t allowance(runtime::RuntimeProcessId id) const {
        if (stopping) return 16384; // invalid session: drain only for local group cleanup
        const auto it = processes.find(id);
        if (it == processes.end()) return 0;
        const auto observation = runtime->observe(id);
        if (!observation || (!it->second.started && !observation->launch_failure)) return 0;
        const auto socket = room();
        const net::OutputLimits limits;
        if (!socket || socket->closing || chunks == 0 || tick_chunks == 0 ||
            socket->log_slots >= limits.log.slots || socket->log_bytes >= limits.log.bytes) return 0;
        const auto available = limits.log.bytes - socket->log_bytes;
        if (available <= 4096) return 0;
        // Fixed identity/envelope allowance, base64 expansion rounded down. Actual
        // typed encoder is still checked before admission; no unsent raw backlog.
        const auto raw_room = ((available - 4096) / 4) * 3;
        return std::min({std::size_t{16384}, static_cast<std::size_t>(std::min<std::uint64_t>(bytes, 16384)), tick_bytes, raw_room});
    }
    common::Result<void> output(runtime::RuntimeProcessId id, runtime::OutputStream stream,
                                 const std::uint8_t* data, std::size_t size) {
        using R = common::Result<void>;
        if (stopping) return R::success(); // never report these bytes as transmitted/durable
        const auto it = processes.find(id);
        if (it == processes.end() || !it->second.started || size > bytes || chunks == 0 ||
            size > tick_bytes || tick_chunks == 0) return R::failure("M7 output admission invariant");
        auto& task = it->second;
        auto& position = stream == runtime::OutputStream::STDOUT ? task.out : task.err;
        if (position.bytes > UINT64_MAX - size || position.next_sequence == UINT64_MAX || task.ordinal == UINT64_MAX)
            return R::failure("log sequence exhausted");
        protocol::LogChunk chunk{config.worker_id, session, task.task.task_id, task.task.attempt_id,
            stream == runtime::OutputStream::STDOUT ? protocol::Stream::Stdout : protocol::Stream::Stderr,
            position.next_sequence, static_cast<std::uint32_t>(size),
            protocol::base64_encode(std::string_view(reinterpret_cast<const char*>(data), size))};
        bytes -= size; --chunks; tick_bytes -= size; --tick_chunks;
        ++position.next_sequence; position.bytes += size; ++task.ordinal;
        return send({wire(chunk, 0), net::QueueClass::Log, net::OrderPoint{id, task.ordinal}, {}});
    }
    void finish(const protocol::RunTask& task, const runtime::ProcessCompletion* completion,
                const Running* running, std::string failure = {}) {
        protocol::TaskFinished message{config.worker_id, session, task.task_id, task.attempt_id,
            protocol::Outcome::LaunchFailed, {}, {}, {}, 0, 0, 0, 0};
        if (running) {
            message.stdout_next_sequence = running->out.next_sequence;
            message.stderr_next_sequence = running->err.next_sequence;
            message.stdout_bytes = running->out.bytes; message.stderr_bytes = running->err.bytes;
        }
        if (!failure.empty()) message.reason = failure.substr(0, 256);
        else if (completion && !completion->exec_succeeded) {
            message.reason = completion->launch_failure ?
                "child launch failed at stage " + std::to_string(static_cast<unsigned int>(completion->launch_failure->stage)) +
                ", errno " + std::to_string(completion->launch_failure->error_number) : "launch failed before exec confirmation";
        } else if (completion) {
            if (!completion->leader_outcome) throw std::runtime_error("completion without reaped leader outcome");
            const auto& outcome = *completion->leader_outcome;
            message.exit_code = outcome.exit_code; message.term_signal = outcome.term_signal;
            if (completion->termination_cause == runtime::TerminationCause::TIMEOUT) message.outcome = protocol::Outcome::TimedOut;
            else if (completion->termination_cause == runtime::TerminationCause::CANCELLED) message.outcome = protocol::Outcome::Cancelled;
            else if (completion->termination_cause == runtime::TerminationCause::INTERNAL_ERROR)
                throw std::runtime_error("runtime internal failure after exec");
            else message.outcome = outcome.exit_code ? protocol::Outcome::Exited : protocol::Outcome::Signaled;
        }
        net::Outbound frame{wire(message, 0)};
        if (running && running->ordinal) frame.dependency = net::OrderPoint{completion->process_id, running->ordinal};
        checked(send(std::move(frame)));
    }
    void interests() {
        for (const auto& entry : processes) {
            const auto observation = runtime->observe(entry.first);
            if (!observation) continue;
            const bool enabled = allowance(entry.first) != 0;
            if (!observation->stdout_complete) checked(runtime->set_output_enabled(entry.first, runtime::OutputStream::STDOUT, enabled));
            if (!observation->stderr_complete) checked(runtime->set_output_enabled(entry.first, runtime::OutputStream::STDERR, enabled));
        }
    }
};
ExecutionManager::ExecutionManager(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ExecutionManager::~ExecutionManager() = default;
common::Result<std::unique_ptr<ExecutionManager>> ExecutionManager::create(
    domain::WorkerConfig config, domain::SessionId session, Send send, Room room) {
    using R = common::Result<std::unique_ptr<ExecutionManager>>;
    if (!send || !room) return R::failure("missing execution transport ports");
    auto impl = std::make_unique<Impl>(std::move(config), session, std::move(send), std::move(room));
    auto* owner = impl.get();
    auto runtime = runtime::ProcessRuntime::create(
        [owner](auto id, auto stream, const auto* data, auto size) { return owner->output(id, stream, data, size); },
        [owner](auto id, auto) { return owner->allowance(id); });
    if (!runtime) return R::failure(runtime.error().message);
    impl->runtime = std::move(runtime).value();
    return R::success(std::unique_ptr<ExecutionManager>(new ExecutionManager(std::move(impl))));
}
common::Result<void> ExecutionManager::activate(const protocol::RegistrationAccepted& ack) {
    using R = common::Result<void>; auto& s = *impl_;
    if (s.active || s.stopping || !ack.accepted || !ack.initial_log_window_bytes || !ack.initial_log_window_chunks ||
        !ack.termination_grace_ms || ack.termination_grace_ms > static_cast<std::uint64_t>(INT64_MAX) / 1000000)
        return R::failure("invalid/repeated execution activation");
    s.active = true; s.bytes = ack.initial_log_window_bytes; s.chunks = ack.initial_log_window_chunks;
    s.grace_ms = ack.termination_grace_ms; return R::success();
}
common::Result<void> ExecutionManager::grant(const protocol::LogWindowUpdate& window) {
    using R = common::Result<void>; auto& s = *impl_;
    if (!s.active || s.stopping || (!window.grant_bytes && !window.grant_chunks) ||
        window.grant_bytes > UINT64_MAX - s.bytes || window.grant_chunks > UINT64_MAX - s.chunks)
        return R::failure("invalid/overflowing log window update");
    s.bytes += window.grant_bytes; s.chunks += window.grant_chunks;
    // The paced main-loop tick re-enables eligible pipe registrations; no busy spin
    // and no read callback is invoked recursively from this transport callback.
    return R::success();
}
common::Result<void> ExecutionManager::accept(const protocol::RunTask& task, std::uint64_t request) {
    using R = common::Result<void>; auto& s = *impl_;
    try {
        if (!s.active || s.stopping || !request || !(task.worker_id == s.config.worker_id) || !(task.session_id == s.session))
            return R::failure("RUN identity/phase mismatch");
        auto frame = protocol::encode_message(task, request, protocol::Direction::ControllerToWorker);
        if (!frame) return R::failure(frame.error().message);
        const auto charge = frame.value().payload.size();
        const bool fits = domain::fits_within(s.config.capacity, s.usage, task.resources) &&
            (!s.config.max_concurrent_tasks || s.processes.size() < *s.config.max_concurrent_tasks) &&
            s.processes.size() < 1024 && s.seen.size() < 4096 && charge <= metadata_limit - s.metadata &&
            (!task.timeout_ms || *task.timeout_ms <= static_cast<std::uint64_t>(INT64_MAX) / 1000000) &&
            !s.seen.count(task.attempt_id);
        protocol::RunTaskAck ack{s.config.worker_id, s.session, task.attempt_id, fits, {}, {}};
        if (!fits) { ack.error_code = "LOCAL_CAPACITY_OR_DUPLICATE"; ack.message = "local execution admission rejected"; }
        checked(s.send({wire(ack, request)})); // accepted ACK is queued BEFORE real preparation/fork
        if (!fits) return R::success();
        s.seen.insert(task.attempt_id);
        runtime::LaunchOptions options;
        options.execution = {task.executable, task.args, {}, task.cwd};
        for (const auto& env : task.env) options.execution.environment_overrides.push_back(env);
        if (task.timeout_ms) options.timeout = std::chrono::milliseconds(*task.timeout_ms);
        options.termination_grace = std::chrono::milliseconds(s.grace_ms);
        // Finish allocating argv/options before installing the temporary owner.
        // Allocate/copy the owner BEFORE fork. Rekeying this node after launch
        // does not allocate, so a live M4 child cannot be lost to map allocation.
        s.processes.emplace(0, Impl::Running{task, charge, false, {}, {}, 0});
        s.metadata += charge; s.usage = checked(domain::add(s.usage, task.resources));
        auto launched = s.runtime->launch(options);
        if (!launched) {
            s.usage = checked(domain::subtract(s.usage, task.resources));
            s.metadata -= charge; s.processes.erase(0);
            s.finish(task, nullptr, nullptr, launched.error().message); return R::success();
        }
        const auto id = launched.value();
        auto owner = s.processes.extract(0); owner.key() = id;
        s.processes.insert(std::move(owner));
        checked(s.runtime->set_output_enabled(id, runtime::OutputStream::STDOUT, false));
        checked(s.runtime->set_output_enabled(id, runtime::OutputStream::STDERR, false));
        return R::success();
    } catch (const std::exception& e) { return R::failure(e.what()); }
}
common::Result<void> ExecutionManager::tick() {
    using R = common::Result<void>; auto& s = *impl_;
    try {
        s.tick_bytes = 65536; s.tick_chunks = 8;
        s.interests();
        checked(s.runtime->poll_once(0));
        for (const auto id : s.runtime->take_started()) {
            auto it = s.processes.find(id); const auto observed = s.runtime->observe(id);
            if (it == s.processes.end() || !observed || !observed->exec_succeeded || observed->leader_pid <= 0)
                throw std::runtime_error("exec observation lost before START");
            if (!s.stopping) checked(s.send({wire(protocol::TaskStarted{s.config.worker_id, s.session,
                it->second.task.task_id, it->second.task.attempt_id, observed->leader_pid}, 0)}));
            it->second.started = true;
        }
        std::optional<std::string> send_failure;
        for (const auto& done : s.runtime->take_completions()) {
            const auto it = s.processes.find(done.process_id);
            if (it == s.processes.end() || !done.stdout_complete || !done.stderr_complete)
                throw std::runtime_error("runtime completion ownership/EOF invariant");
            if (!s.stopping) {
                try { s.finish(it->second.task, &done, &it->second); }
                catch (const std::exception& e) { if (!send_failure) send_failure = e.what(); }
            }
            s.usage = checked(domain::subtract(s.usage, it->second.task.resources));
            s.metadata -= it->second.metadata_bytes; s.processes.erase(it);
        }
        if (send_failure) return R::failure(*send_failure);
        s.interests(); return R::success();
    } catch (const std::exception& e) { return R::failure(e.what()); }
}
common::Result<void> ExecutionManager::stop() {
    using R = common::Result<void>; auto& s = *impl_; s.stopping = true;
    std::optional<std::string> first;
    for (const auto& task : s.processes) {
        const auto result = s.runtime->cancel(task.first);
        if (!result && !first) first = result.error().message;
    }
    if (first) return R::failure(*first);
    return R::success();
}
std::vector<domain::AttemptId> ExecutionManager::running() const {
    std::vector<domain::AttemptId> result;
    for (const auto& task : impl_->processes) result.push_back(task.second.task.attempt_id);
    return result;
}
} // namespace ts::worker
