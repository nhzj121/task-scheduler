#pragma once

#include <memory>
#include "ts/domain/config.h"
#include "ts/net/connection_manager.h"
#include "ts/storage/persistence_thread.h"

namespace ts::controller {
struct ApplicationLimits {
    std::size_t connections_per_role{32}, requests_per_connection{16}, queries{64}, effects{4096};
    std::size_t registrations{4096};
    std::size_t restore_bytes{64U * 1024U * 1024U}, reply_bytes{4U * 1024U * 1024U};
};
struct ApplicationOptions {
    ApplicationLimits limits;
    // Existing technical seams; normal executables leave these empty. Hooks
    // execute only on the real PersistenceThread, never on the EventLoop.
    std::shared_ptr<storage::SqliteHook> persistence_hook;
    net::SocketIo client_io, worker_io;
};
class Application final {
public:
    static common::Result<std::unique_ptr<Application>> create(domain::ControllerConfig config, ApplicationOptions options = {});
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    // One owner thread. SIGINT/SIGTERM drive explicit shutdown; joins occur only
    // after dispatch has stopped, never inside a callback.
    int run();
private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::controller
