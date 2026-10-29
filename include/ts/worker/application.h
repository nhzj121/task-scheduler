#pragma once
#include <memory>
#include "ts/domain/config.h"
#include "ts/net/connection_manager.h"
namespace ts::worker {
struct ApplicationOptions { net::SocketIo socket_io; };
class Application final {
public:
    static common::Result<std::unique_ptr<Application>> create(domain::WorkerConfig config, ApplicationOptions options = {});
    ~Application();
    int run();
private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::worker
