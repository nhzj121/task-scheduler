#include "state_store.h"

#include <cerrno>
#include <climits>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include "ts/common/timestamp.h"
#include "ts/domain/ids.h"

namespace ts::storage {
namespace {
using TS = domain::TaskState;
using AS = domain::AttemptState;
constexpr std::uint64_t kMax = static_cast<std::uint64_t>(INT64_MAX);
constexpr const char* kTasks =
    "CREATE TABLE tasks(task_id TEXT PRIMARY KEY NOT NULL,state TEXT NOT NULL CHECK(state IN "
    "('PENDING','ACTIVE','SUCCEEDED','FAILED','CANCELLED')),state_version INTEGER NOT NULL CHECK(state_version>=0),"
    "executable TEXT NOT NULL,args_json TEXT NOT NULL,env_json TEXT NOT NULL,cwd TEXT,"
    "cpu_slots INTEGER NOT NULL CHECK(cpu_slots>=1),memory_mb INTEGER NOT NULL CHECK(memory_mb>=0),"
    "timeout_ms INTEGER CHECK(timeout_ms>0),cancel_requested_at TEXT,latest_attempt_id TEXT,"
    "created_at TEXT NOT NULL,updated_at TEXT NOT NULL)";
constexpr const char* kAttempts =
    "CREATE TABLE attempts(attempt_id TEXT PRIMARY KEY NOT NULL,task_id TEXT NOT NULL REFERENCES tasks(task_id),"
    "attempt_no INTEGER NOT NULL CHECK(attempt_no>=1),state TEXT NOT NULL CHECK(state IN "
    "('ASSIGNED','STARTING','RUNNING','SUCCEEDED','FAILED','TIMED_OUT','CANCELLED','LOST')),"
    "state_version INTEGER NOT NULL CHECK(state_version>=0),worker_id TEXT NOT NULL,session_id TEXT NOT NULL,"
    "assigned_at TEXT,acknowledged_at TEXT,started_at TEXT,finished_at TEXT,exit_code INTEGER,term_signal INTEGER,"
    "reason TEXT,observed_outcome_json TEXT,stdout_path TEXT NOT NULL,stderr_path TEXT NOT NULL,"
    "stdout_bytes INTEGER NOT NULL DEFAULT 0 CHECK(stdout_bytes>=0),"
    "stderr_bytes INTEGER NOT NULL DEFAULT 0 CHECK(stderr_bytes>=0),"
    "stdout_final_sequence INTEGER CHECK(stdout_final_sequence>=0),"
    "stderr_final_sequence INTEGER CHECK(stderr_final_sequence>=0),"
    "logs_complete INTEGER NOT NULL DEFAULT 0 CHECK(logs_complete IN (0,1)),UNIQUE(task_id,attempt_no))";
constexpr const char* kIndexes[] = {
    "CREATE INDEX tasks_pending ON tasks(state,created_at,task_id)",
    "CREATE INDEX tasks_listing ON tasks(created_at DESC,task_id DESC)",
    "CREATE INDEX attempts_history ON attempts(task_id,attempt_no)",
    "CREATE INDEX attempts_session ON attempts(session_id,state)",
    "CREATE INDEX attempts_state ON attempts(state)"};

[[noreturn]] void fail(ErrorCode code, const std::string& message, bool fatal = false, int sqlite_code = 0) {
    throw StoreFailure{Error{code, sqlite_code, fatal, message.substr(0, 240)}};
}
void need(bool condition, const char* message) {
    if (!condition) fail(ErrorCode::Invalid, message);
}
void text(const std::string& s, bool nonempty = true) {
    need((!nonempty || !s.empty()) && s.find('\0') == std::string::npos, "empty or NUL-contaminated metadata");
}
void time(const std::string& s) { need(static_cast<bool>(common::Timestamp::parse(s)), "invalid canonical timestamp"); }
template<class Id> void id(const std::string& s) { need(static_cast<bool>(Id::parse(s)), "invalid canonical ID"); }
void integer(std::uint64_t v) { need(v <= kMax, "unsigned integer exceeds SQLite INT64_MAX"); }
void version(std::uint64_t v) { need(v < kMax, "state version cannot increment in SQLite range"); }
void optional_time(const std::optional<std::string>& s) { if (s) time(*s); }
void validate_spec(const TaskSpec& t) {
    id<domain::TaskId>(t.task_id); text(t.executable); if (t.cwd) text(*t.cwd);
    need(t.cpu_slots >= 1, "cpu_slots must be positive"); integer(t.memory_mb);
    if (t.timeout_ms) { integer(*t.timeout_ms); need(*t.timeout_ms > 0, "timeout must be positive"); }
    for (const auto& a : t.args) text(a, false);
    for (const auto& e : t.env) {
        text(e.first); text(e.second, false);
        need(e.first.find('=') == std::string::npos, "environment name contains equals");
    }
}
nlohmann::json json(const std::string& s) {
    // Reject duplicate object keys, rather than accepting last-key-wins disk corruption.
    std::vector<std::set<std::string>> objects;
    auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        if (event == nlohmann::json::parse_event_t::object_start) objects.emplace_back();
        if (event == nlohmann::json::parse_event_t::key &&
            !objects.back().insert(parsed.get<std::string>()).second) {
            fail(ErrorCode::Invalid, "duplicate JSON object key");
        }
        if (event == nlohmann::json::parse_event_t::object_end) objects.pop_back();
        return true;
    };
    return nlohmann::json::parse(s, callback);
}
std::optional<std::string> observed(const std::optional<std::string>& s) {
    if (!s) return std::nullopt;
    auto j = json(*s); need(j.is_object(), "observed outcome must be a JSON object");
    return j.dump();
}
} // namespace

class Statement final {
public:
    Statement(StateStore& store, const char* sql) : store_(store), sql_(sql) {
        store_.require_owner(); store_.hook(SqlStage::Prepare, sql_);
        const int rc = sqlite3_prepare_v2(store_.db_, sql_, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            if (stmt_ && sqlite3_finalize(stmt_) != SQLITE_OK) store_.poisoned_ = true;
            store_.check(rc, "prepare");
        }
    }
    ~Statement() {
        if (stmt_) {
            const int rc = sqlite3_finalize(stmt_);
            // finalize repeats an already reported step error; all other errors poison
            // the connection. Normal exits use finish(), which reports errors directly.
            if (rc != SQLITE_OK && !step_failed_) store_.poisoned_ = true;
        }
    }
    void bind(int i, const std::string& s) {
        need(s.size() <= static_cast<std::size_t>(INT_MAX), "text too large");
        store_.hook(SqlStage::Bind, sql_);
        store_.check(sqlite3_bind_text(stmt_, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT), "bind text");
    }
    void bind(int i, const std::optional<std::string>& s) { if (s) bind(i, *s); else null(i); }
    void bind(int i, std::uint64_t n) {
        integer(n); store_.hook(SqlStage::Bind, sql_);
        store_.check(sqlite3_bind_int64(stmt_, i, static_cast<sqlite3_int64>(n)), "bind integer");
    }
    void bind(int i, const std::optional<std::uint64_t>& n) { if (n) bind(i, *n); else null(i); }
    void signed_number(int i, const std::optional<std::int32_t>& n) {
        if (!n) { null(i); return; }
        store_.hook(SqlStage::Bind, sql_);
        store_.check(sqlite3_bind_int64(stmt_, i, *n), "bind signed integer");
    }
    void null(int i) { store_.hook(SqlStage::Bind, sql_); store_.check(sqlite3_bind_null(stmt_, i), "bind null"); }
    bool row() {
        store_.hook(SqlStage::Step, sql_);
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        step_failed_ = true; store_.check(rc, "step"); return false;
    }
    void done() { need(!row(), "unexpected SQL row"); finish(); }
    void finish() {
        auto* s = stmt_; stmt_ = nullptr;
        store_.check(sqlite3_finalize(s), "finalize");
    }
    int type(int i) const { return sqlite3_column_type(stmt_, i); }
    std::string string(int i) const {
        if (type(i) != SQLITE_TEXT) fail(ErrorCode::Corrupt, "non-text persisted column", true);
        const auto* p = sqlite3_column_text(stmt_, i);
        const auto n = sqlite3_column_bytes(stmt_, i);
        if (!p) fail(ErrorCode::Corrupt, "unreadable persisted text", true);
        return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
    }
    std::optional<std::string> opt_string(int i) const {
        return type(i) == SQLITE_NULL ? std::nullopt : std::optional<std::string>(string(i));
    }
    std::int64_t signed_int(int i) const {
        if (type(i) != SQLITE_INTEGER) fail(ErrorCode::Corrupt, "non-integer persisted column", true);
        return sqlite3_column_int64(stmt_, i);
    }
    std::uint64_t number(int i) const {
        const auto n = signed_int(i);
        if (n < 0) fail(ErrorCode::Corrupt, "negative unsigned persisted column", true);
        return static_cast<std::uint64_t>(n);
    }
    std::optional<std::uint64_t> opt_number(int i) const {
        return type(i) == SQLITE_NULL ? std::nullopt : std::optional<std::uint64_t>(number(i));
    }
    std::optional<std::int32_t> opt_signed(int i) const {
        if (type(i) == SQLITE_NULL) return std::nullopt;
        const auto n = signed_int(i);
        if (n < INT32_MIN || n > INT32_MAX) fail(ErrorCode::Corrupt, "signed column outside int32", true);
        return static_cast<std::int32_t>(n);
    }
    TaskRow task_row() const {
        try { return decode_task(); }
        catch (const StoreFailure& e) { fail(ErrorCode::Corrupt, "invalid persisted Task: " + e.error.message, true); }
        catch (const std::exception& e) { fail(ErrorCode::Corrupt, e.what(), true); }
    }
    AttemptRow attempt_row() const {
        try { return decode_attempt(); }
        catch (const StoreFailure& e) { fail(ErrorCode::Corrupt, "invalid persisted Attempt: " + e.error.message, true); }
        catch (const std::exception& e) { fail(ErrorCode::Corrupt, e.what(), true); }
    }
private:
    TaskRow decode_task() const {
        TaskRow t;
        t.spec.task_id = string(0); t.state = domain::parse_task_state(string(1)).value();
        t.state_version = number(2); t.spec.executable = string(3);
        auto a = json(string(4)); auto e = json(string(5));
        need(a.is_array() && e.is_object(), "invalid persisted args/env JSON shape");
        for (const auto& v : a) { need(v.is_string(), "non-string argument"); t.spec.args.push_back(v.get<std::string>()); }
        for (auto it = e.begin(); it != e.end(); ++it) {
            need(it.value().is_string(), "non-string environment"); t.spec.env.emplace(it.key(), it.value().get<std::string>());
        }
        t.spec.cwd = opt_string(6);
        const auto cpu = number(7); need(cpu <= UINT32_MAX, "cpu_slots overflow");
        t.spec.cpu_slots = static_cast<std::uint32_t>(cpu); t.spec.memory_mb = number(8);
        t.spec.timeout_ms = opt_number(9); t.cancel_requested_at = opt_string(10);
        t.latest_attempt_id = opt_string(11); t.created_at = string(12); t.updated_at = string(13);
        validate_spec(t.spec); optional_time(t.cancel_requested_at); time(t.created_at); time(t.updated_at);
        if (t.latest_attempt_id) id<domain::AttemptId>(*t.latest_attempt_id);
        return t;
    }
    AttemptRow decode_attempt() const {
        AttemptRow a;
        a.attempt_id = string(0); a.task_id = string(1);
        const auto n = number(2); need(n > 0 && n <= UINT32_MAX, "attempt number outside uint32");
        a.attempt_no = static_cast<std::uint32_t>(n); a.state = domain::parse_attempt_state(string(3)).value();
        a.state_version = number(4); a.worker_id = string(5); a.session_id = string(6);
        a.assigned_at = opt_string(7); a.acknowledged_at = opt_string(8);
        a.started_at = opt_string(9); a.finished_at = opt_string(10);
        a.exit_code = opt_signed(11); a.term_signal = opt_signed(12); a.reason = opt_string(13);
        a.observed_outcome_json = observed(opt_string(14)); a.stdout_path = string(15); a.stderr_path = string(16);
        a.stdout_bytes = number(17); a.stderr_bytes = number(18);
        a.stdout_final_sequence = opt_number(19); a.stderr_final_sequence = opt_number(20);
        const auto complete = number(21); need(complete <= 1, "invalid logs_complete"); a.logs_complete = complete == 1;
        id<domain::AttemptId>(a.attempt_id); id<domain::TaskId>(a.task_id);
        id<domain::SessionId>(a.session_id); id<domain::WorkerId>(a.worker_id);
        optional_time(a.assigned_at); optional_time(a.acknowledged_at); optional_time(a.started_at); optional_time(a.finished_at);
        text(a.stdout_path); text(a.stderr_path); if (a.reason) text(*a.reason);
        need(a.assigned_at.has_value(), "assigned attempt missing assignment timestamp");
        need(domain::is_terminal(a.state) == a.finished_at.has_value(), "terminal timestamp inconsistent");
        if (a.state == AS::STARTING || a.state == AS::RUNNING) need(a.acknowledged_at.has_value(), "missing ACK timestamp");
        if (a.state == AS::RUNNING) need(a.started_at.has_value(), "missing start timestamp");
        need(!a.logs_complete || (domain::is_terminal(a.state) && a.state != AS::LOST &&
             a.stdout_final_sequence && a.stderr_final_sequence), "invalid completeness metadata");
        if (a.exit_code) need(*a.exit_code >= 0 && *a.exit_code <= 255, "invalid persisted exit code");
        if (a.term_signal) need(*a.term_signal > 0 && *a.term_signal <= 64, "invalid persisted termination signal");
        need(!(a.exit_code && a.term_signal), "mutually exclusive leader outcomes");
        return a;
    }
private:
    StateStore& store_;
    const char* sql_;
    sqlite3_stmt* stmt_{nullptr};
    bool step_failed_{false};
};

void StateStore::require_owner() const {
    if (owner_ != std::this_thread::get_id()) fail(ErrorCode::Internal, "SQLite thread-affinity violation", true);
}
void StateStore::check(int code, const char* operation) {
    if (code == SQLITE_OK) return;
    const int base = code & 0xff;
    ErrorCode category = ErrorCode::Internal;
    bool fatal = true;
    switch (base) {
        case SQLITE_CONSTRAINT: category = ErrorCode::Constraint; fatal = false; break;
        case SQLITE_BUSY: case SQLITE_LOCKED: category = ErrorCode::Busy; break;
        case SQLITE_FULL: category = ErrorCode::Full; break;
        case SQLITE_IOERR: case SQLITE_CANTOPEN: case SQLITE_READONLY: category = ErrorCode::Io; break;
        case SQLITE_CORRUPT: case SQLITE_NOTADB: category = ErrorCode::Corrupt; break;
        default: break;
    }
    fail(category, std::string(operation) + ": " + (db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(code)), fatal, code);
}
void StateStore::hook(SqlStage stage, const char* sql_text) {
    if (hook_) check(hook_->before(stage, sql_text), "injected SQLite boundary");
}
void StateStore::sql(const char* s, SqlStage stage) {
    hook(stage, s);
    Statement q(*this, s); q.done();
}

StateStore::StateStore(const StoreOptions& options) : owner_(std::this_thread::get_id()), hook_(options.hook) {
    try {
        need(options.busy_timeout_ms > 0 && options.busy_timeout_ms <= 5000, "busy timeout outside 1..5000 ms");
        text(options.data_directory);
        const std::filesystem::path root(options.data_directory);
        need(root.is_absolute() && std::filesystem::is_directory(root), "data directory must exist and be absolute");
        const auto canonical = std::filesystem::canonical(root);
        const auto lock_path = (canonical / "controller.lock").string();
        lock_.reset(::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (!lock_) fail(ErrorCode::Io, "cannot open controller ownership lock", true);
        struct stat lock_stat {};
        if (::fstat(lock_.get(), &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode))
            fail(ErrorCode::Io, "controller lock is not a regular file", true);
        if (::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0)
            fail(errno == EWOULDBLOCK ? ErrorCode::LockUnavailable : ErrorCode::Io,
                 "exclusive controller data-directory lock unavailable", true);
        const auto path = (canonical / "metadata.sqlite3").string();
        {
            common::UniqueFd file(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (!file) fail(ErrorCode::Io, "cannot open metadata database file", true);
            struct stat db_stat {};
            if (::fstat(file.get(), &db_stat) != 0 || !S_ISREG(db_stat.st_mode))
                fail(ErrorCode::Io, "metadata database is not regular", true);
            // Close the validation fd BEFORE SQLite acquires POSIX locks on
            // this inode. Closing any other fd afterwards would release this
            // process's fcntl locks behind SQLite's back (even on another thread).
        }
        check(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr), "open");
        check(sqlite3_extended_result_codes(db_, 1), "extended result codes");
        // Bound individual SQLite allocations even if an externally damaged database
        // contains a huge value. Per-command response limits remain separately enforced.
        static_cast<void>(sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, 8 * 1024 * 1024));
        need(sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, -1) <= 8 * 1024 * 1024, "SQLite length limit unavailable");
        check(sqlite3_busy_timeout(db_, options.busy_timeout_ms), "busy_timeout");
        sql("PRAGMA foreign_keys=ON");
        { Statement q(*this, "PRAGMA journal_mode=WAL");
          need(q.row() && q.string(0) == "wal", "WAL unavailable"); q.finish(); }
        sql("PRAGMA synchronous=FULL");
        auto s = settings();
        need(s.foreign_keys == 1 && s.synchronous == 2 && s.busy_timeout_ms == options.busy_timeout_ms,
             "SQLite PRAGMA verification failed");
        verify_schema();
    } catch (...) {
        bool cleanup_failed = false;
        if (db_) {
            if (!sqlite3_get_autocommit(db_) && sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK)
                cleanup_failed = true;
            const int rc = sqlite3_close(db_);
            if (rc != SQLITE_OK) {
                cleanup_failed = true;
                if (sqlite3_close_v2(db_) != SQLITE_OK) poisoned_ = true;
            }
            db_ = nullptr;
        }
        if (cleanup_failed) fail(ErrorCode::Internal, "SQLite startup failed and cleanup reported failure", true);
        throw;
    }
}
StateStore::~StateStore() {
    if (db_) {
        // Last-resort RAII path after an explicit failure; normal run() uses close().
        if (!sqlite3_get_autocommit(db_)) {
            const int rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            if (rc != SQLITE_OK) poisoned_ = true;
        }
        const int rc = sqlite3_close(db_);
        if (rc != SQLITE_OK) {
            poisoned_ = true;
            if (sqlite3_close_v2(db_) != SQLITE_OK) poisoned_ = true;
        }
    }
}
void StateStore::close() {
    require_owner();
    if (!db_) return;
    if (!sqlite3_get_autocommit(db_)) sql("ROLLBACK", SqlStage::Rollback);
    if (sqlite3_next_stmt(db_, nullptr)) fail(ErrorCode::Internal, "unfinalized SQLite statements", true);
    check(sqlite3_close(db_), "close"); db_ = nullptr; lock_.reset();
}
Status StateStore::settings() {
    require_owner(); Status s;
    { Statement q(*this, "PRAGMA journal_mode"); need(q.row(), "missing journal mode"); s.journal_wal = q.string(0) == "wal" ? 1 : 0; q.finish(); }
    { Statement q(*this, "PRAGMA synchronous"); need(q.row(), "missing synchronous"); s.synchronous = static_cast<int>(q.number(0)); q.finish(); }
    { Statement q(*this, "PRAGMA foreign_keys"); need(q.row(), "missing foreign_keys"); s.foreign_keys = static_cast<int>(q.number(0)); q.finish(); }
    { Statement q(*this, "PRAGMA busy_timeout"); need(q.row(), "missing busy_timeout"); s.busy_timeout_ms = static_cast<int>(q.number(0)); q.finish(); }
    return s;
}
void StateStore::verify_schema() {
    std::uint64_t v = 0, tables = 0;
    { Statement q(*this, "PRAGMA user_version"); need(q.row(), "missing schema version"); v = q.number(0); q.finish(); }
    { Statement q(*this, "SELECT count(*) FROM sqlite_master WHERE name NOT GLOB 'sqlite_*'");
      need(q.row(), "missing schema count"); tables = q.number(0); q.finish(); }
    if (v == 0 && tables == 0) {
        sql("BEGIN IMMEDIATE");
        sql(kTasks); sql(kAttempts); for (const auto* index : kIndexes) sql(index);
        sql("PRAGMA user_version=1"); sql("COMMIT", SqlStage::Commit);
    } else if (v != 1) fail(ErrorCode::Unsupported, "unsupported or unversioned nonempty SQLite database", true);
    // A v1 layout is exact, not inferred from a version marker alone. Reject views,
    // triggers, extra runtime tables or a weakened CHECK/index definition.
    std::set<std::string> expected{kTasks, kAttempts};
    expected.insert(std::begin(kIndexes), std::end(kIndexes));
    Statement q(*this, "SELECT sql FROM sqlite_master WHERE name NOT GLOB 'sqlite_*'");
    while (q.row()) {
        if (expected.erase(q.string(0)) != 1) fail(ErrorCode::Unsupported, "incompatible schema-v1 definition", true);
    }
    q.finish();
    if (!expected.empty()) fail(ErrorCode::Unsupported, "incomplete schema-v1 database", true);
}

TaskRow StateStore::task(const std::string& task_id) {
    id<domain::TaskId>(task_id);
    Statement q(*this, "SELECT * FROM tasks WHERE task_id=?"); q.bind(1, task_id);
    if (!q.row()) fail(ErrorCode::Conflict, "Task not found");
    TaskRow row;
    try { row = q.task_row(); } catch (const StoreFailure& e) {
        fail(ErrorCode::Corrupt, "invalid persisted Task: " + e.error.message, true);
    } catch (const std::exception& e) { fail(ErrorCode::Corrupt, e.what(), true); }
    q.finish(); return row;
}
AttemptRow StateStore::attempt(const std::string& attempt_id) {
    id<domain::AttemptId>(attempt_id);
    Statement q(*this, "SELECT * FROM attempts WHERE attempt_id=?"); q.bind(1, attempt_id);
    if (!q.row()) fail(ErrorCode::Conflict, "Attempt not found");
    AttemptRow row;
    try { row = q.attempt_row(); } catch (const StoreFailure& e) {
        fail(ErrorCode::Corrupt, "invalid persisted Attempt: " + e.error.message, true);
    } catch (const std::exception& e) { fail(ErrorCode::Corrupt, e.what(), true); }
    q.finish(); return row;
}
TaskRow StateStore::expected_task(const TaskVersion& expected, TS state) {
    version(expected.version);
    auto t = task(expected.task_id);
    if (t.state != state || t.state_version != expected.version) fail(ErrorCode::Conflict, "Task state/version conflict");
    return t;
}
AttemptRow StateStore::active(const AttemptVersion& e) {
    version(e.version); id<domain::SessionId>(e.session_id);
    auto t = expected_task(e.task, TS::ACTIVE); auto a = attempt(e.attempt_id);
    if (a.task_id != e.task.task_id || a.state_version != e.version || a.session_id != e.session_id ||
        !t.latest_attempt_id || *t.latest_attempt_id != a.attempt_id || domain::is_terminal(a.state))
        fail(ErrorCode::Conflict, "Attempt identity/binding/version conflict");
    return a;
}
void StateStore::changed() {
    if (sqlite3_changes(db_) != 1) fail(ErrorCode::Conflict, "conditional SQL update affected no unique row");
}
void StateStore::update_task(const TaskRow& old, TS state, const std::string& at,
                            const std::optional<std::string>& cancel,
                            const std::optional<std::string>& latest) {
    version(old.state_version); time(at);
    Statement q(*this, "UPDATE tasks SET state=?,state_version=state_version+1,updated_at=?,"
                      "cancel_requested_at=?,latest_attempt_id=? WHERE task_id=? AND state=? AND state_version=?");
    q.bind(1, std::string(domain::to_string(state))); q.bind(2, at); q.bind(3, cancel); q.bind(4, latest);
    q.bind(5, old.spec.task_id); q.bind(6, std::string(domain::to_string(old.state))); q.bind(7, old.state_version);
    q.done(); changed();
}

Outcome StateStore::perform(const Submit& c) {
    validate_spec(c.task); time(c.at);
    Statement q(*this, "INSERT INTO tasks VALUES(?,'PENDING',0,?,?,?,?,?,?,?,NULL,NULL,?,?)");
    q.bind(1, c.task.task_id); q.bind(2, c.task.executable);
    q.bind(3, nlohmann::json(c.task.args).dump()); q.bind(4, nlohmann::json(c.task.env).dump());
    q.bind(5, c.task.cwd); q.bind(6, static_cast<std::uint64_t>(c.task.cpu_slots));
    q.bind(7, c.task.memory_mb); q.bind(8, c.task.timeout_ms); q.bind(9, c.at); q.bind(10, c.at);
    q.done(); Outcome o; o.affected = 1; return o;
}
Outcome StateStore::perform(const Assign& c) {
    auto t = expected_task(c.expected, TS::PENDING);
    id<domain::AttemptId>(c.attempt_id); id<domain::WorkerId>(c.worker_id);
    id<domain::SessionId>(c.session_id); text(c.stdout_path); text(c.stderr_path); time(c.at);
    std::uint64_t next = 1;
    { Statement q(*this, "SELECT count(*),coalesce(max(attempt_no),0) FROM attempts WHERE task_id=? AND state IN ('ASSIGNED','STARTING','RUNNING')");
      q.bind(1, t.spec.task_id); need(q.row(), "missing attempt count");
      if (q.number(0) != 0) fail(ErrorCode::Conflict, "Task already has nonterminal Attempt");
      q.finish(); }
    { Statement q(*this, "SELECT coalesce(max(attempt_no),0) FROM attempts WHERE task_id=?");
      q.bind(1, t.spec.task_id); need(q.row(), "missing latest attempt number"); next = q.number(0) + 1; q.finish(); }
    need(next <= UINT32_MAX, "attempt number exhausted");
    update_task(t, TS::ACTIVE, c.at, t.cancel_requested_at, c.attempt_id);
    Statement q(*this, "INSERT INTO attempts(attempt_id,task_id,attempt_no,state,state_version,worker_id,session_id,assigned_at,stdout_path,stderr_path)"
                      " VALUES(?,?,?,'ASSIGNED',0,?,?,?,?,?)");
    q.bind(1, c.attempt_id); q.bind(2, t.spec.task_id); q.bind(3, next); q.bind(4, c.worker_id);
    q.bind(5, c.session_id); q.bind(6, c.at); q.bind(7, c.stdout_path); q.bind(8, c.stderr_path); q.done();
    Outcome o; o.affected = 1; return o;
}
Outcome StateStore::perform(const Advance& c) {
    auto a = active(c.expected); time(c.at);
    need((a.state == AS::ASSIGNED && c.to == AS::STARTING) || (a.state == AS::STARTING && c.to == AS::RUNNING),
         "not an intermediate Attempt transition");
    Statement q(*this, c.to == AS::STARTING ?
        "UPDATE attempts SET state='STARTING',state_version=state_version+1,acknowledged_at=? WHERE attempt_id=? AND state='ASSIGNED' AND state_version=?" :
        "UPDATE attempts SET state='RUNNING',state_version=state_version+1,started_at=? WHERE attempt_id=? AND state='STARTING' AND state_version=?");
    q.bind(1, c.at); q.bind(2, a.attempt_id); q.bind(3, a.state_version); q.done(); changed();
    Outcome o; o.affected = 1; return o;
}
Outcome StateStore::perform(const CancelIntent& c) {
    auto t = expected_task(c.expected, TS::ACTIVE); time(c.at);
    if (t.cancel_requested_at) fail(ErrorCode::Conflict, "cancellation intent already committed");
    update_task(t, TS::ACTIVE, c.at, c.at, t.latest_attempt_id);
    Outcome o; o.affected = 1; return o;
}
Outcome StateStore::perform(const CancelPending& c) {
    auto t = expected_task(c.expected, TS::PENDING);
    update_task(t, TS::CANCELLED, c.at, t.cancel_requested_at, t.latest_attempt_id);
    Outcome o; o.affected = 1; return o;
}
Outcome StateStore::perform(const Retry& c) {
    auto t = expected_task(c.expected, TS::FAILED);
    update_task(t, TS::PENDING, c.at, std::nullopt, t.latest_attempt_id);
    Outcome o; o.affected = 1; return o;
}
Outcome StateStore::perform(const Terminalize& c) {
    auto a = active(c.expected); auto t = task(a.task_id); time(c.at);
    need(domain::is_terminal(c.to) && domain::can_transition(a.state, c.to), "invalid terminal Attempt transition");
    if (c.exit_code) need(*c.exit_code >= 0 && *c.exit_code <= 255, "invalid exit code");
    if (c.term_signal) need(*c.term_signal > 0 && *c.term_signal <= 64, "invalid termination signal");
    need(!(c.exit_code && c.term_signal), "mutually exclusive leader outcomes");
    if (c.to == AS::SUCCEEDED) need(c.exit_code == std::optional<std::int32_t>(0), "SUCCEEDED requires leader exit code zero");
    text(c.stdout_path); text(c.stderr_path); integer(c.stdout_bytes); integer(c.stderr_bytes);
    // All terminal paths retain the log namespace established by assignment.
    need(c.stdout_path == a.stdout_path && c.stderr_path == a.stderr_path,
         "terminal log paths differ from assigned Attempt paths");
    if (c.stdout_final_sequence) integer(*c.stdout_final_sequence);
    if (c.stderr_final_sequence) integer(*c.stderr_final_sequence);
    if (c.reason) text(*c.reason);
    auto facts = observed(c.observed_outcome_json);
    const bool log_error = c.reason && *c.reason == "LOG_PERSISTENCE_ERROR";
    bool complete = false;
    if (c.to == AS::LOST) {
        need(c.logs.kind == LogEvidenceKind::Incomplete && !log_error && c.reason.has_value(), "LOST needs a reason and cannot claim complete logs");
    } else if (log_error) {
        need(c.to == AS::FAILED && c.logs.kind == LogEvidenceKind::Incomplete && facts.has_value(),
             "log-persistence failure requires FAILED, incomplete logs and observed outcome");
    } else if (c.logs.kind == LogEvidenceKind::ProvenEmpty) {
        const bool rejection = c.to == AS::FAILED && c.reason && *c.reason == "DISPATCH_REJECTED" && c.logs.explicit_dispatch_rejection;
        const bool cancelled = c.to == AS::CANCELLED && c.logs.no_dispatch_bytes;
        need(a.state == AS::ASSIGNED && (rejection || cancelled) && !c.logs.namespace_failed &&
             c.stdout_bytes == 0 && c.stderr_bytes == 0 && c.stdout_final_sequence == std::optional<std::uint64_t>(0) &&
             c.stderr_final_sequence == std::optional<std::uint64_t>(0), "invalid proven-empty authorization");
        complete = true;
    } else {
        need(a.state != AS::ASSIGNED && c.logs.kind == LogEvidenceKind::FinalBarrier &&
             c.logs.attempt_id == a.attempt_id && c.logs.prior_writes_completed && c.logs.final_counters_verified &&
             c.logs.stdout_synced && c.logs.stderr_synced && c.logs.namespace_synced && !c.logs.namespace_failed &&
             c.stdout_final_sequence && c.stderr_final_sequence, "missing final log data/namespace barrier evidence");
        complete = true;
    }
    TS target = TS::FAILED;
    if (c.to == AS::SUCCEEDED) target = TS::SUCCEEDED;
    if (c.to == AS::CANCELLED) target = TS::CANCELLED;
    Statement q(*this, "UPDATE attempts SET state=?,state_version=state_version+1,finished_at=?,exit_code=?,term_signal=?,reason=?,"
                      "observed_outcome_json=?,stdout_path=?,stderr_path=?,stdout_bytes=?,stderr_bytes=?,stdout_final_sequence=?,"
                      "stderr_final_sequence=?,logs_complete=? WHERE attempt_id=? AND state=? AND state_version=? AND session_id=?");
    q.bind(1, std::string(domain::to_string(c.to))); q.bind(2, c.at); q.signed_number(3, c.exit_code);
    q.signed_number(4, c.term_signal); q.bind(5, c.reason); q.bind(6, facts); q.bind(7, c.stdout_path);
    q.bind(8, c.stderr_path); q.bind(9, c.stdout_bytes); q.bind(10, c.stderr_bytes);
    q.bind(11, c.stdout_final_sequence); q.bind(12, c.stderr_final_sequence);
    q.bind(13, std::uint64_t{complete ? 1U : 0U}); q.bind(14, a.attempt_id);
    q.bind(15, std::string(domain::to_string(a.state))); q.bind(16, a.state_version); q.bind(17, a.session_id);
    q.done(); changed(); update_task(t, target, c.at, t.cancel_requested_at, t.latest_attempt_id);
    Outcome o; o.affected = 1; return o;
}
Outcome StateStore::lose(const std::optional<std::string>& session, const std::string& at, const std::string& reason) {
    time(at); text(reason); if (session) id<domain::SessionId>(*session);
    // Iterate bounded row-at-a-time: a multi-session recovery is one transaction, not
    // a giant snapshot or one transaction per Task. SELECT is finalized before updates.
    Outcome o;
    for (;;) {
        std::optional<std::string> next;
        { Statement q(*this, session ?
            "SELECT attempt_id FROM attempts WHERE state IN ('ASSIGNED','STARTING','RUNNING') AND session_id=? ORDER BY attempt_id LIMIT 1" :
            "SELECT attempt_id FROM attempts WHERE state IN ('ASSIGNED','STARTING','RUNNING') ORDER BY attempt_id LIMIT 1");
          if (session) q.bind(1, *session);
          if (q.row()) next = q.string(0);
          q.finish(); }
        if (!next) break;
        auto a = attempt(*next); auto t = task(a.task_id);
        if (t.state != TS::ACTIVE || !t.latest_attempt_id || *t.latest_attempt_id != a.attempt_id)
            fail(ErrorCode::Corrupt, "nonterminal Attempt has no matching ACTIVE Task", true);
        version(a.state_version);
        Statement q(*this, "UPDATE attempts SET state='LOST',state_version=state_version+1,finished_at=?,reason=?,logs_complete=0 "
                          "WHERE attempt_id=? AND state=? AND state_version=?");
        q.bind(1, at); q.bind(2, reason); q.bind(3, a.attempt_id);
        q.bind(4, std::string(domain::to_string(a.state))); q.bind(5, a.state_version); q.done(); changed();
        update_task(t, TS::FAILED, at, t.cancel_requested_at, t.latest_attempt_id); ++o.affected;
    }
    // ACTIVE with no live Attempt also means a damaged database; don't publish Ready.
    Statement q(*this, "SELECT count(*) FROM tasks t WHERE state='ACTIVE' AND NOT EXISTS"
                      "(SELECT 1 FROM attempts a WHERE a.task_id=t.task_id AND a.attempt_id=t.latest_attempt_id "
                      "AND a.state IN ('ASSIGNED','STARTING','RUNNING'))");
    need(q.row(), "missing ACTIVE consistency result");
    if (q.number(0) != 0) fail(ErrorCode::Corrupt, "orphan ACTIVE Task", true);
    q.finish(); return o;
}
Outcome StateStore::perform(const LoseSession& c) { return lose(c.session_id, c.at, c.reason); }
Outcome StateStore::perform(const Recover& c) { return lose(std::nullopt, c.at, "CONTROLLER_RESTART"); }
Outcome StateStore::perform(const ReadTask& c) {
    Outcome o; o.tasks.push_back(task(c.task_id)); return o;
}
Outcome StateStore::perform(const ReadAttempts& c) {
    id<domain::TaskId>(c.task_id); need(c.limit >= 1 && c.limit <= 200, "history page limit outside 1..200");
    Statement q(*this, "SELECT * FROM attempts WHERE task_id=? AND attempt_no>? ORDER BY attempt_no LIMIT ?");
    q.bind(1, c.task_id); q.bind(2, static_cast<std::uint64_t>(c.after_attempt_no));
    q.bind(3, static_cast<std::uint64_t>(c.limit) + 1);
    Outcome o;
    while (q.row()) {
        if (o.attempts.size() == c.limit) { o.has_more = true; break; }
        o.attempts.push_back(q.attempt_row());
        if (owned_bytes(o) > response_budget_) fail(ErrorCode::Capacity, "history response exceeds reserved bytes");
    }
    q.finish(); return o;
}
Outcome StateStore::perform(const ListTasks& c) {
    need(c.limit >= 1 && c.limit <= 200, "task page limit outside 1..200");
    if (c.after) { time(c.after->created_at); id<domain::TaskId>(c.after->task_id); }
    if (c.state) need(static_cast<bool>(domain::parse_task_state(domain::to_string(*c.state))), "invalid Task filter");
    // Only constant SQL branches, never concatenate an input identifier or path.
    const char* sql_text = c.oldest_first ?
        "SELECT * FROM tasks WHERE (? IS NULL OR state=?) AND (? IS NULL OR (created_at,task_id)>(?,?)) ORDER BY created_at,task_id LIMIT ?" :
        "SELECT * FROM tasks WHERE (? IS NULL OR state=?) AND (? IS NULL OR (created_at,task_id)<(?,?)) ORDER BY created_at DESC,task_id DESC LIMIT ?";
    Statement q(*this, sql_text);
    std::optional<std::string> state;
    if (c.state) state = std::string(domain::to_string(*c.state));
    q.bind(1, state); q.bind(2, state);
    if (c.after) { q.bind(3, c.after->created_at); q.bind(4, c.after->created_at); q.bind(5, c.after->task_id); }
    else { q.null(3); q.null(4); q.null(5); }
    q.bind(6, static_cast<std::uint64_t>(c.limit) + 1);
    Outcome o;
    while (q.row()) {
        if (o.tasks.size() == c.limit) { o.has_more = true; break; }
        o.tasks.push_back(q.task_row());
        if (owned_bytes(o) > response_budget_) fail(ErrorCode::Capacity, "task response exceeds reserved bytes");
    }
    q.finish();
    if (o.has_more) o.next = TaskCursor{o.tasks.back().created_at, o.tasks.back().spec.task_id};
    return o;
}

Outcome StateStore::execute(const Command& command, std::size_t response_budget) {
    require_owner(); response_budget_ = response_budget;
    Outcome o;
    bool committing = false;
    try {
        if (poisoned_) fail(ErrorCode::Internal, "SQLite connection is poisoned", true);
        need(response_budget >= 512, "completion budget below minimum");
        if (is_read(command)) sql("BEGIN"); else sql("BEGIN IMMEDIATE");
        o = std::visit([&](const auto& c) { return perform(c); }, command);
        if (owned_bytes(o) > response_budget) fail(ErrorCode::Capacity, "response exceeds reserved bytes");
        committing = true;
        sql("COMMIT", SqlStage::Commit);
        hook(SqlStage::AfterCommit, "COMMIT");
        committing = false;
        o.committed = !is_read(command);
        return o;
    } catch (const StoreFailure& e) {
        o = {}; o.error = e.error;
    } catch (const nlohmann::json::exception& e) {
        o = {}; o.error = Error{is_read(command) ? ErrorCode::Corrupt : ErrorCode::Invalid, 0, is_read(command), std::string(e.what()).substr(0, 240)};
    } catch (const std::exception& e) {
        o = {}; o.error = Error{ErrorCode::Internal, 0, true, std::string(e.what()).substr(0, 240)};
    } catch (...) {
        o = {}; o.error = Error{ErrorCode::Internal, 0, true, "unknown SQLite command exception"};
    }
    if (committing) {
        o.error->code = ErrorCode::UnknownCommit; o.error->fatal = true;
        o.error->message = "commit outcome unknown; no external authorization";
    }
    if (!sqlite3_get_autocommit(db_)) {
        try { sql("ROLLBACK", SqlStage::Rollback); }
        catch (...) {
            // Even if the injected/real rollback boundary fails, try one real rollback
            // for cleanup only, never claim command success or blindly retry its write.
            const int rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            poisoned_ = true; o.error->fatal = true;
            o.error->message = rc == SQLITE_OK ? "rollback boundary failed; connection retired" : "rollback failed; connection retired";
        }
    }
    if (!sqlite3_get_autocommit(db_) || poisoned_) { o.error->fatal = true; poisoned_ = true; }
    return o;
}

} // namespace ts::storage
