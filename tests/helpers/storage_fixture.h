#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include <poll.h>
#include <sqlite3.h>
#include <unistd.h>
#include <gtest/gtest.h>

#include "ts/persistence/channel.h"

namespace ts::storage::test {
inline constexpr const char* kAt = "2026-10-19T01:02:03.000001Z";
inline constexpr const char* kLater = "2026-10-19T02:02:03.000001Z";
inline std::string uuid(unsigned int n) {
    char buffer[37];
    static_cast<void>(std::snprintf(buffer, sizeof(buffer), "00000000-0000-4000-8000-%012x", n));
    return buffer;
}
class TempDirectory {
public:
    TempDirectory() {
        char pattern[] = "/tmp/ts-storage-XXXXXX";
        const auto* created = ::mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~TempDirectory() {
        // Only the exact mkdtemp-created leaf; never the build tree/user notes.
        std::error_code ec; std::filesystem::remove_all(path, ec);
    }
    std::string path;
};
class Reader {
public:
    explicit Reader(const std::string& directory) {
        if (sqlite3_open((directory + "/metadata.sqlite3").c_str(), &db) != SQLITE_OK)
            throw std::runtime_error("test inspector open failed");
        if (sqlite3_busy_timeout(db, 1000) != SQLITE_OK)
            throw std::runtime_error("test inspector busy timeout failed");
        exec("PRAGMA foreign_keys=ON");
    }
    ~Reader() { EXPECT_EQ(sqlite3_close(db), SQLITE_OK); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    int exec_code(const std::string& sql) { return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr); }
    void exec(const std::string& sql) {
        if (exec_code(sql) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    std::vector<std::vector<std::string>> rows(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        std::vector<std::vector<std::string>> result;
        int code = SQLITE_OK;
        while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int i = 0; i < sqlite3_column_count(statement); ++i) {
                const auto* p = sqlite3_column_text(statement, i);
                row.push_back(p ? reinterpret_cast<const char*>(p) : "<NULL>");
            }
            result.push_back(std::move(row));
        }
        EXPECT_EQ(code, SQLITE_DONE); EXPECT_EQ(sqlite3_finalize(statement), SQLITE_OK);
        return result;
    }
    std::string scalar(const std::string& sql) { return rows(sql).at(0).at(0); }
    sqlite3* db{nullptr};
};
// Explicitly arm/disarm a single technical call boundary. Blocking is test-only and
// released by a condition variable handshake, not nondeterministic sleeps.
class Hook final : public SqliteHook {
public:
    void arm(SqlStage stage, std::string needle, int code, std::size_t nth = 1, bool pause = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        stage_ = stage; needle_ = std::move(needle); code_ = code; remaining_ = nth;
        pause_ = pause; entered_ = false; released_ = false; armed_ = true;
    }
    void fail_rollback() { std::lock_guard<std::mutex> lock(mutex_); rollback_ = true; }
    void disarm() { std::lock_guard<std::mutex> lock(mutex_); armed_ = false; rollback_ = false; }
    void release() { std::lock_guard<std::mutex> lock(mutex_); released_ = true; condition_.notify_all(); }
    bool wait_entered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(3), [&] { return entered_; });
    }
    int before(SqlStage stage, const char* sql) override {
        std::unique_lock<std::mutex> lock(mutex_);
        owners.insert(std::this_thread::get_id());
        if (stage == SqlStage::Rollback && rollback_) return SQLITE_IOERR;
        if (!armed_ || stage != stage_ || std::string(sql).find(needle_) == std::string::npos) return SQLITE_OK;
        if (--remaining_ != 0) return SQLITE_OK;
        armed_ = false; entered_ = true; condition_.notify_all();
        if (pause_ && !condition_.wait_for(lock, std::chrono::seconds(5), [&] { return released_; }))
            return SQLITE_INTERRUPT;
        return code_;
    }
    std::set<std::thread::id> thread_ids() { std::lock_guard<std::mutex> lock(mutex_); return owners; }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    SqlStage stage_{SqlStage::Step};
    std::string needle_;
    int code_{SQLITE_OK};
    std::size_t remaining_{1};
    bool pause_{false}, entered_{false}, released_{false}, armed_{false}, rollback_{false};
    std::set<std::thread::id> owners;
};
inline TaskSpec spec(unsigned int n) {
    TaskSpec t; t.task_id = uuid(n); t.executable = "/usr/bin/true";
    t.args = {"one", "two"}; t.env = {{"Z", "last"}, {"A", "first"}};
    return t;
}
inline Assign assignment(unsigned int n, unsigned int session = 1000) {
    return {{uuid(n), 0}, uuid(n + 10000), "worker-1", uuid(session), "logs/out", "logs/err", kAt};
}
inline AttemptVersion av(unsigned int n, std::uint64_t attempt_version, std::uint64_t task_version = 1) {
    return {{uuid(n), task_version}, uuid(n + 10000), attempt_version, uuid(1000)};
}
inline Terminalize finish(unsigned int n, domain::AttemptState state = domain::AttemptState::SUCCEEDED) {
    Terminalize c; c.expected = av(n, 2); c.to = state; c.at = kLater;
    if (state == domain::AttemptState::SUCCEEDED) c.exit_code = 0;
    c.stdout_path = "logs/out"; c.stderr_path = "logs/err";
    c.stdout_final_sequence = 0; c.stderr_final_sequence = 0;
    c.logs.kind = LogEvidenceKind::FinalBarrier;
    c.logs.attempt_id = c.expected.attempt_id;
    c.logs.prior_writes_completed = true; c.logs.final_counters_verified = true;
    c.logs.stdout_synced = true; c.logs.stderr_synced = true; c.logs.namespace_synced = true;
    return c;
}
inline bool readable(int fd, int timeout = 3000) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout) == 1 && (p.revents & POLLIN) != 0;
}
inline bool startup(persistence::Channel& c) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (c.status().health == Health::Starting && std::chrono::steady_clock::now() < end) {
        if (readable(c.fd())) { auto result = c.drain(); EXPECT_TRUE(result); }
    }
    return c.status().health == Health::Ready;
}
class StoreTest : public ::testing::Test {
protected:
    void SetUp() override { open(); }
    void TearDown() override { hook->release(); channel.reset(); }
    void open(ChannelLimits limits = {}) {
        auto result = persistence::Channel::start({directory.path, 50, kLater, hook}, limits);
        ASSERT_TRUE(result) << result.error().message;
        channel = std::move(result).value();
        ASSERT_TRUE(startup(*channel)) << (channel->status().error ? channel->status().error->message : "startup did not finish");
    }
    Envelope envelope(Command command, std::size_t budget = 65536) {
        auto correlation = correlate(command, next_operation++, 7, 9);
        return {std::move(correlation), std::move(command), budget};
    }
    std::vector<Completion> wait(std::size_t count) {
        std::vector<Completion> all;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (all.size() < count && std::chrono::steady_clock::now() < end) {
            if (!readable(channel->fd(), 100)) continue;
            auto completions = channel->drain();
            if (!completions) throw std::runtime_error(completions.error().message);
            for (auto& c : completions.value()) all.push_back(std::move(c));
        }
        EXPECT_EQ(all.size(), count);
        return all;
    }
    Outcome run(Command command, std::size_t budget = 65536) {
        const auto admitted = channel->try_post(envelope(std::move(command), budget));
        EXPECT_EQ(admitted, Admission::Accepted);
        if (admitted != Admission::Accepted) throw std::runtime_error("test command not admitted");
        auto results = wait(1);
        if (results.empty()) throw std::runtime_error("test completion timeout");
        return std::move(results.front().outcome);
    }
    void submit(unsigned int n) { auto o = run(Submit{spec(n), kAt}); ASSERT_TRUE(o.ok()) << o.error->message; ASSERT_TRUE(o.committed); }
    void assigned(unsigned int n) { submit(n); auto o = run(assignment(n)); ASSERT_TRUE(o.ok()) << o.error->message; }
    void running(unsigned int n) {
        assigned(n);
        ASSERT_TRUE(run(Advance{av(n, 0), domain::AttemptState::STARTING, kAt}).ok());
        ASSERT_TRUE(run(Advance{av(n, 1), domain::AttemptState::RUNNING, kAt}).ok());
    }
    TempDirectory directory;
    std::shared_ptr<Hook> hook{std::make_shared<Hook>()};
    std::unique_ptr<persistence::Channel> channel;
    std::uint64_t next_operation{1};
};
} // namespace ts::storage::test
