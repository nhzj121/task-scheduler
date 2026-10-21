#pragma once
#include <memory>
#include "ts/domain/config.h"
namespace ts::worker {
class Application final {
public:
    static common::Result<std::unique_ptr<Application>> create(domain::WorkerConfig config);
    ~Application();
    int run();
private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ts::worker
