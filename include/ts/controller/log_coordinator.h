#pragma once
#include <functional>
#include "ts/controller/task_manager.h"
#include "ts/controller/log_credit_ledger.h"
#include "ts/domain/config.h"
#include "ts/protocol/messages.h"

namespace ts::controller {
enum class LogAdmission { Accepted, ProtocolViolation, InvariantFailure };
struct LogReply { ClientRequest client; protocol::Message message; };
// EventLoop-only owner of credits, operation correlations and finalization facts.
// Writer values never contain application pointers or borrowed file/socket fds.
class LogCoordinator final {
public:
    static common::Result<std::unique_ptr<LogCoordinator>> create(std::string root,
        domain::QueueConfig config, TaskManager& manager, std::shared_ptr<logs::FilesystemHook> hook = {});
    ~LogCoordinator();
    [[nodiscard]] common::Result<void> reserve(const domain::SessionId& session);
    [[nodiscard]] common::Result<void> activate(const SessionKey& session);
    void reclaim(const domain::SessionId& session);
    [[nodiscard]] common::Result<void> bind(const storage::AttemptRow& row, const SessionKey& session);
    [[nodiscard]] LogAdmission append(const protocol::LogChunk& chunk);
    [[nodiscard]] common::Result<void> finish(FinishedEvent observation);
    [[nodiscard]] common::Result<void> read(const storage::AttemptRow& row, protocol::GetLogs request, ClientRequest client);
    [[nodiscard]] common::Result<std::vector<LogReply>> drain();
    [[nodiscard]] common::Result<void> pump(const std::function<bool(const SessionKey&, LogCredit)>& send_grant);
    void terminal(const domain::AttemptId& attempt);
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] logs::Status status() const;
    [[nodiscard]] logs::Usage usage() const;
    void stop();
    void join();
private:
    struct Impl;
    explicit LogCoordinator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::controller
