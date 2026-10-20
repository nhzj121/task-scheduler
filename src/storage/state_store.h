#pragma once

#include <memory>
#include <thread>
#include <sqlite3.h>

#include "ts/common/unique_fd.h"
#include "ts/storage/persistence_thread.h"

namespace ts::storage {

struct StoreFailure { Error error; };

// Internal, not a public synchronous API. Constructed/used/destroyed in run().
class StateStore final {
public:
    explicit StateStore(const StoreOptions& options);
    ~StateStore();
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;
    Outcome execute(const Command& command, std::size_t response_budget);
    void close();
    [[nodiscard]] Status settings();
private:
    friend class Statement;
    void require_owner() const;
    void check(int code, const char* operation);
    void hook(SqlStage stage, const char* sql);
    void sql(const char* text, SqlStage stage = SqlStage::Step);
    void verify_schema();
    TaskRow task(const std::string& id);
    AttemptRow attempt(const std::string& id);
    void update_task(const TaskRow& old, domain::TaskState state,
                     const std::string& at, const std::optional<std::string>& cancel,
                     const std::optional<std::string>& latest);
    Outcome perform(const Submit& command);
    Outcome perform(const Assign& command);
    Outcome perform(const Advance& command);
    Outcome perform(const CancelIntent& command);
    Outcome perform(const CancelPending& command);
    Outcome perform(const Terminalize& command);
    Outcome perform(const Retry& command);
    Outcome perform(const LoseSession& command);
    Outcome perform(const Recover& command);
    Outcome perform(const ReadTask& command);
    Outcome perform(const ReadAttempts& command);
    Outcome perform(const ListTasks& command);
    Outcome lose(const std::optional<std::string>& session,
                 const std::string& at, const std::string& reason);
    AttemptRow active(const AttemptVersion& expected);
    TaskRow expected_task(const TaskVersion& expected, domain::TaskState state);
    void changed();
    sqlite3* db_{nullptr};
    common::UniqueFd lock_;
    std::thread::id owner_;
    std::shared_ptr<SqliteHook> hook_;
    bool poisoned_{false};
    std::size_t response_budget_{0};
};

} // namespace ts::storage
