#pragma once

#include <functional>
#include <memory>
#include "ts/domain/config.h"
#include "ts/net/connection_manager.h"
#include "ts/protocol/messages.h"

namespace ts::worker {
// One owner thread. Sends own their bytes; room() returns a transport value snapshot.
// The runtime keeps SIGCHLD/process ownership; this adapter never owns a child fd.
class ExecutionManager final {
public:
    using Send = std::function<common::Result<void>(net::Outbound)>;
    using Room = std::function<std::optional<net::ConnectionSnapshot>()>;
    static common::Result<std::unique_ptr<ExecutionManager>> create(
        domain::WorkerConfig config, domain::SessionId session, Send send, Room room);
    ~ExecutionManager();
    [[nodiscard]] common::Result<void> activate(const protocol::RegistrationAccepted& ack);
    [[nodiscard]] common::Result<void> grant(const protocol::LogWindowUpdate& window);
    [[nodiscard]] common::Result<void> accept(const protocol::RunTask& task, std::uint64_t request);
    [[nodiscard]] common::Result<void> tick(); // bounded, nonblocking M4 poll
    [[nodiscard]] common::Result<void> stop(); // cancel owned groups; keep ticking until empty
    [[nodiscard]] std::vector<domain::AttemptId> running() const;
private:
    struct Impl;
    explicit ExecutionManager(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::worker
