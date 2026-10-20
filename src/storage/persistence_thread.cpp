#include "ts/storage/persistence_thread.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "state_store.h"

namespace ts::storage {
namespace {
Error internal(const std::string& message) {
    return {ErrorCode::Internal, 0, true, message.substr(0, 240)};
}
} // namespace

PersistenceThread::PersistenceThread(StoreOptions options, ChannelLimits limits,
                                     std::shared_ptr<CompletionNotification> notification)
    : options_(std::move(options)), limits_(limits), notification_(std::move(notification)) {
    finished_.reserve(limits_.count);
}
std::unique_ptr<PersistenceThread> PersistenceThread::start(
    StoreOptions options, ChannelLimits limits, std::shared_ptr<CompletionNotification> notification) {
    if (!notification || limits.count <= limits.essential_count || limits.essential_count == 0 ||
        limits.command_bytes <= limits.essential_command_bytes || limits.essential_command_bytes == 0 ||
        limits.completion_bytes <= limits.essential_completion_bytes || limits.essential_completion_bytes < 512)
        throw std::invalid_argument("persistence limits must include usable ordinary and essential count/bytes");
    auto worker = std::unique_ptr<PersistenceThread>(new PersistenceThread(std::move(options), limits, std::move(notification)));
    worker->thread_ = std::thread([ptr = worker.get()] { ptr->run(); });
    return worker;
}
PersistenceThread::~PersistenceThread() { stop(); join(); }
void PersistenceThread::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    available_.notify_one();
}
void PersistenceThread::join() { if (thread_.joinable()) thread_.join(); }
Status PersistenceThread::status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
Usage PersistenceThread::usage() const { std::lock_guard<std::mutex> lock(mutex_); return usage_; }
void PersistenceThread::signal() noexcept {
    if (!notification_->notify()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.error) status_.error = internal("completion notification failed; drain/status remain authoritative");
        status_.health = Health::Degraded; stopping_ = true;
        available_.notify_one();
    }
}
Admission PersistenceThread::try_post(Envelope e) {
    const auto bytes = owned_bytes(e);
    const bool essential = is_essential(e.command);
    std::unique_lock<std::mutex> lock(mutex_);
    if (status_.error) return Admission::Degraded;
    if (stopping_) return Admission::Stopped;
    if (status_.health != Health::Ready) return Admission::NotReady;
    const auto count_limit = essential ? limits_.count : limits_.count - limits_.essential_count;
    const auto byte_limit = essential ? limits_.command_bytes : limits_.command_bytes - limits_.essential_command_bytes;
    const auto result_limit = essential ? limits_.completion_bytes : limits_.completion_bytes - limits_.essential_completion_bytes;
    const auto identity = correlate(e.command, e.correlation.operation_id, e.correlation.entity_generation, e.correlation.session_generation);
    if (e.response_bytes < 512 || e.correlation.operation_id == 0 || !(e.correlation == identity)) return Admission::Invalid;
    const bool oversized = bytes > byte_limit || e.response_bytes > result_limit;
    const bool full = usage_.count >= count_limit || bytes > byte_limit - std::min(usage_.command_bytes, byte_limit) ||
                      e.response_bytes > result_limit - std::min(usage_.completion_bytes, result_limit);
    if (oversized || full) {
        if (!essential) return oversized ? Admission::Oversized : Admission::Full;
        status_.error = Error{ErrorCode::Capacity, 0, true, "essential persistence admission unavailable"};
        status_.health = Health::Degraded; stopping_ = true;
        lock.unlock(); available_.notify_one(); signal(); return Admission::Degraded;
    }
    // Credit is held through execution and completed-result retention, not just
    // while queued. A stalled consumer therefore cannot grow either queue.
    pending_.push_back(Pending{std::move(e), bytes});
    ++usage_.count; usage_.command_bytes += bytes;
    usage_.completion_bytes += pending_.back().envelope.response_bytes;
    lock.unlock(); available_.notify_one(); return Admission::Accepted;
}
std::vector<Completion> PersistenceThread::drain() {
    std::vector<Completion> results;
    std::lock_guard<std::mutex> lock(mutex_);
    results.reserve(finished_.size());
    for (auto& f : finished_) {
        results.push_back(std::move(f.completion));
        --usage_.count; usage_.command_bytes -= f.bytes; usage_.completion_bytes -= f.response_bytes;
    }
    finished_.clear();
    return results;
}
void PersistenceThread::run() noexcept {
    std::unique_ptr<StateStore> store;
    try {
        store = std::make_unique<StateStore>(options_);
        auto s = store->settings();
        const auto recovery = store->execute(Recover{options_.recovery_at}, 512);
        if (!recovery.ok()) throw StoreFailure{*recovery.error};
        s.recovered_attempts = recovery.affected;
        { std::lock_guard<std::mutex> lock(mutex_); s.health = Health::Ready; status_ = std::move(s); }
        signal();
        for (;;) {
            Pending pending;
            bool abort = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                available_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) break;
                pending = std::move(pending_.front()); pending_.pop_front();
                abort = status_.error.has_value();
            }
            Outcome outcome;
            if (abort) outcome.error = Error{ErrorCode::Aborted, 0, true, "accepted command aborted after persistence failure"};
            else outcome = store->execute(pending.envelope.command, pending.envelope.response_bytes);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (outcome.error && outcome.error->fatal) {
                    if (!status_.error) status_.error = outcome.error;
                    status_.health = Health::Degraded; stopping_ = true;
                }
                finished_.push_back(Finished{Completion{std::move(pending.envelope.correlation), std::move(outcome)},
                                             pending.bytes, pending.envelope.response_bytes});
            }
            signal();
        }
        store->close(); store.reset();
    } catch (const StoreFailure& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.error) status_.error = e.error;
        status_.error->fatal = true;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.error) status_.error = internal(e.what());
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.error) status_.error = internal("unknown persistence thread exception");
    }
    // Exception boundary also resolves already accepted values, never false success.
    // SQLite RAII teardown remains on this thread, including a failed startup.
    if (store) {
        try { store->close(); }
        catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!status_.error) status_.error = internal("SQLite teardown failed");
            else { status_.error->fatal = true; status_.error->message += "; teardown failed"; }
        }
    }
    store.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pending_.empty()) {
            auto& p = pending_.front(); Outcome o;
            o.error = Error{ErrorCode::Aborted, 0, true, "accepted command aborted during persistence shutdown"};
            finished_.push_back(Finished{Completion{std::move(p.envelope.correlation), std::move(o)}, p.bytes, p.envelope.response_bytes});
            pending_.pop_front();
        }
        stopping_ = true; status_.health = status_.error ? Health::Degraded : Health::Stopped;
    }
    signal();
}
} // namespace ts::storage
