#include "ts/persistence/channel.h"

#include <exception>
#include <utility>

namespace ts::persistence {
class Channel::Signal final : public storage::CompletionNotification {
public:
    explicit Signal(io::EventFd fd) : event(std::move(fd)) {}
    bool notify() noexcept override {
        try { return static_cast<bool>(event.notify()); } catch (...) { return false; }
    }
    io::EventFd event;
};
Channel::Channel(std::shared_ptr<Signal> signal, std::unique_ptr<storage::PersistenceThread> worker)
    : signal_(std::move(signal)), worker_(std::move(worker)) {}
Channel::~Channel() { worker_->stop(); worker_->join(); }
common::Result<std::unique_ptr<Channel>> Channel::start(storage::StoreOptions options, storage::ChannelLimits limits) {
    auto fd = io::EventFd::create();
    if (!fd) return common::Result<std::unique_ptr<Channel>>::failure(fd.error().message);
    try {
        auto signal = std::make_shared<Signal>(std::move(fd).value());
        auto worker = storage::PersistenceThread::start(std::move(options), limits, signal);
        return common::Result<std::unique_ptr<Channel>>::success(
            std::unique_ptr<Channel>(new Channel(std::move(signal), std::move(worker))));
    } catch (const std::exception& e) {
        return common::Result<std::unique_ptr<Channel>>::failure(e.what());
    }
}
int Channel::fd() const noexcept { return signal_->event.fd(); }
storage::Admission Channel::try_post(storage::Envelope envelope) { return worker_->try_post(std::move(envelope)); }
common::Result<std::vector<storage::Completion>> Channel::drain() {
    // Drain notification first, then values. A concurrent publish either appears in
    // this drain or leaves eventfd readable for the next one (possibly a stale wake).
    auto drained = signal_->event.drain();
    if (!drained) return common::Result<std::vector<storage::Completion>>::failure(drained.error().message);
    return common::Result<std::vector<storage::Completion>>::success(worker_->drain());
}
storage::Status Channel::status() const { return worker_->status(); }
storage::Usage Channel::usage() const { return worker_->usage(); }
void Channel::stop() { worker_->stop(); }
void Channel::join() { worker_->join(); }
} // namespace ts::persistence
