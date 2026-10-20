#pragma once

#include "ts/io/event_fd.h"
#include "ts/storage/persistence_thread.h"

namespace ts::persistence {

// Integration target only: storage has no io dependency. The application borrows
// fd(), unregisters it before destruction, and joins before the eventfd can close.
class Channel final {
public:
    static common::Result<std::unique_ptr<Channel>> start(
        storage::StoreOptions options, storage::ChannelLimits limits = {});
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] storage::Admission try_post(storage::Envelope envelope);
    [[nodiscard]] common::Result<std::vector<storage::Completion>> drain();
    [[nodiscard]] storage::Status status() const;
    [[nodiscard]] storage::Usage usage() const;
    void stop();
    void join();
private:
    class Signal;
    Channel(std::shared_ptr<Signal> signal,
            std::unique_ptr<storage::PersistenceThread> worker);
    std::shared_ptr<Signal> signal_;
    std::unique_ptr<storage::PersistenceThread> worker_;
};

} // namespace ts::persistence
