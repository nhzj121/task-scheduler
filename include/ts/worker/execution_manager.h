#pragma once

#include <functional>
#include <memory>
#include "ts/domain/config.h"
#include "ts/net/connection_manager.h"
#include "ts/protocol/messages.h"

namespace ts::worker {
enum class ExecutionLifecycleKind { Started, Finished };
struct ExecutionLifecycleEvent {
    ExecutionLifecycleKind kind;
    std::string task_id, attempt_id, worker_id, session_id, executable;
    std::uint32_t attempt_no;
    std::optional<std::int32_t> pid;
    std::optional<protocol::Outcome> outcome;
    std::optional<std::int32_t> exit_code, term_signal;
};
// Owner-thread, non-reentrant, best-effort value snapshots; never terminal proof.
using ExecutionLifecycleObserver = std::function<void(const ExecutionLifecycleEvent&)>;
// One owner thread. Sends own their bytes; room() returns a transport value snapshot.
// The runtime keeps SIGCHLD/process ownership; this adapter never owns a child fd.
class ExecutionManager final {
public:
    using Send = std::function<common::Result<void>(net::Outbound)>;
    using Room = std::function<std::optional<net::ConnectionSnapshot>()>;
    static common::Result<std::unique_ptr<ExecutionManager>> create(
        domain::WorkerConfig config, domain::SessionId session, Send send, Room room,
        ExecutionLifecycleObserver observer = {});
    ~ExecutionManager();
    [[nodiscard]] common::Result<void> activate(const protocol::RegistrationAccepted& ack);
    [[nodiscard]] common::Result<void> grant(const protocol::LogWindowUpdate& window);
    [[nodiscard]] common::Result<void> accept(const protocol::RunTask& task, std::uint64_t request);
    [[nodiscard]] common::Result<void> cancel(const protocol::WorkerCancelTask& task, std::uint64_t request);
    // Called only after complete cleanup of the previous session. Shared SIGCHLD
    // ownership is Worker-wide; no per-Attempt owner or wire state survives.
    [[nodiscard]] common::Result<void> reset_session(domain::SessionId session);
    [[nodiscard]] common::Result<void> tick(); // bounded, nonblocking M4 poll
    [[nodiscard]] common::Result<void> stop(); // cancel owned groups; keep ticking until empty
    [[nodiscard]] std::vector<domain::AttemptId> running() const;
private:
    struct Impl;
    explicit ExecutionManager(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::worker
