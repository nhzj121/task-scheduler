#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "ts/storage/types.h"

namespace ts::storage {

// Narrow technical boundary for deterministic SQLite fault tests. A hook must not
// call into application owners. Production uses no hook; all calls are real SQLite.
enum class SqlStage { Prepare, Bind, Step, Commit, AfterCommit, Rollback };
class SqliteHook {
public:
    virtual ~SqliteHook() = default;
    virtual int before(SqlStage stage, const char* sql) = 0;
};
class CompletionNotification {
public:
    virtual ~CompletionNotification() = default;
    // Technical, thread-safe resource notification only; may run on producer or
    // persistence thread. No business callback/object access is permitted.
    virtual bool notify() noexcept = 0;
};
struct StoreOptions {
    // Existing absolute trusted-local directory; metadata.sqlite3 and controller.lock
    // are deterministic children. No task-log filesystem operations occur here.
    std::string data_directory;
    int busy_timeout_ms{100}; // accepted range 1..5000; no unbounded busy retry
    std::string recovery_at;
    std::shared_ptr<SqliteHook> hook;
};
struct ChannelLimits {
    std::size_t count{64}, command_bytes{4U * 1024U * 1024U};
    std::size_t completion_bytes{8U * 1024U * 1024U};
    std::size_t essential_count{4}, essential_command_bytes{16384};
    std::size_t essential_completion_bytes{16384};
};
enum class Admission { Accepted, Full, Oversized, Invalid, NotReady, Stopped, Degraded };
enum class Health { Starting, Ready, Degraded, Stopped };
struct Status {
    Health health{Health::Starting};
    std::optional<Error> error;
    std::size_t recovered_attempts{0};
    int journal_wal{0}, synchronous{0}, foreign_keys{0}, busy_timeout_ms{0};
};
struct Usage { std::size_t count, command_bytes, completion_bytes; };

// Sole SQLite owner. start() is asynchronous: inspect status after notification.
// Startup recovery commits before Ready. No public method executes SQL on its caller.
// stop() stops admission and drains accepted work; fatal failures abort remaining
// accepted commands explicitly. join() is an owner shutdown operation, never a poll
// callback operation. Notifications and channels outlive the joined SQLite owner.
// Future EventLoop owners serialize one outstanding operation per Task, allocate
// unique operation IDs, and validate correlation + current generations on completion.
class PersistenceThread final {
public:
    static std::unique_ptr<PersistenceThread> start(
        StoreOptions options, ChannelLimits limits,
        std::shared_ptr<CompletionNotification> notification);
    ~PersistenceThread();
    PersistenceThread(const PersistenceThread&) = delete;
    PersistenceThread& operator=(const PersistenceThread&) = delete;
    [[nodiscard]] Admission try_post(Envelope envelope);
    [[nodiscard]] std::vector<Completion> drain();
    [[nodiscard]] Status status() const;
    [[nodiscard]] Usage usage() const;
    void stop();
    void join();
private:
    struct Pending { Envelope envelope; std::size_t bytes; };
    struct Finished { Completion completion; std::size_t bytes, response_bytes; };
    PersistenceThread(StoreOptions options, ChannelLimits limits,
                      std::shared_ptr<CompletionNotification> notification);
    void run() noexcept;
    void signal() noexcept;
    StoreOptions options_;
    ChannelLimits limits_;
    std::shared_ptr<CompletionNotification> notification_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Pending> pending_;
    // Fixed capacity allocated before the worker starts: publishing after COMMIT
    // must not depend on allocating a completion queue node.
    std::vector<Finished> finished_;
    Usage usage_{0, 0, 0};
    Status status_;
    bool stopping_{false};
    std::thread thread_;
};

} // namespace ts::storage
