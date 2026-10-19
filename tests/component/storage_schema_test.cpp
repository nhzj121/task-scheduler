#include "../helpers/storage_fixture.h"

#include <limits>
#include <sys/wait.h>

namespace ts::storage::test {

TEST_F(StoreTest, SchemaVersionColumnsTypesNullabilityAndIndexes) {
    Reader db(directory.path);
    EXPECT_EQ(db.scalar("PRAGMA user_version"), "1");
    EXPECT_EQ(db.scalar("PRAGMA journal_mode"), "wal");
    EXPECT_EQ(db.scalar("SELECT count(*) FROM sqlite_master WHERE type='table'"), "2");
    const auto tasks = db.rows("PRAGMA table_info(tasks)");
    const std::vector<std::string> task_names{"task_id","state","state_version","executable","args_json","env_json","cwd","cpu_slots","memory_mb","timeout_ms","cancel_requested_at","latest_attempt_id","created_at","updated_at"};
    ASSERT_EQ(tasks.size(), task_names.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        EXPECT_EQ(tasks[i][1], task_names[i]);
        EXPECT_EQ(tasks[i][2], (i == 2 || i == 7 || i == 8 || i == 9) ? "INTEGER" : "TEXT");
        EXPECT_EQ(tasks[i][3], (i == 6 || i == 9 || i == 10 || i == 11) ? "0" : "1");
        EXPECT_EQ(tasks[i][5], i == 0 ? "1" : "0");
    }
    const auto attempts = db.rows("PRAGMA table_info(attempts)");
    const std::vector<std::string> attempt_names{"attempt_id","task_id","attempt_no","state","state_version","worker_id","session_id","assigned_at","acknowledged_at","started_at","finished_at","exit_code","term_signal","reason","observed_outcome_json","stdout_path","stderr_path","stdout_bytes","stderr_bytes","stdout_final_sequence","stderr_final_sequence","logs_complete"};
    ASSERT_EQ(attempts.size(), attempt_names.size());
    for (std::size_t i = 0; i < attempts.size(); ++i) {
        EXPECT_EQ(attempts[i][1], attempt_names[i]);
        EXPECT_EQ(attempts[i][2], (i == 2 || i == 4 || i == 11 || i == 12 || i >= 17) ? "INTEGER" : "TEXT");
        EXPECT_EQ(attempts[i][3], (i <= 6 || i == 15 || i == 16 || i == 17 || i == 18 || i == 21) ? "1" : "0");
        EXPECT_EQ(attempts[i][5], i == 0 ? "1" : "0");
    }
    const auto fk = db.rows("PRAGMA foreign_key_list(attempts)");
    ASSERT_EQ(fk.size(), 1U); EXPECT_EQ(fk[0][2], "tasks"); EXPECT_EQ(fk[0][3], "task_id");
    EXPECT_EQ(db.rows("PRAGMA index_info(tasks_pending)"), (std::vector<std::vector<std::string>>{{"0","1","state"},{"1","12","created_at"},{"2","0","task_id"}}));
    const auto list = db.rows("PRAGMA index_xinfo(tasks_listing)");
    EXPECT_EQ(list[0][2], "created_at"); EXPECT_EQ(list[0][3], "1");
    EXPECT_EQ(list[1][2], "task_id"); EXPECT_EQ(list[1][3], "1");
    EXPECT_EQ(db.rows("PRAGMA index_info(attempts_history)").size(), 2U);
    EXPECT_EQ(db.rows("PRAGMA index_info(attempts_session)").size(), 2U);
    EXPECT_EQ(db.rows("PRAGMA index_info(attempts_state)").size(), 1U);
    const auto s = channel->status();
    EXPECT_EQ(s.journal_wal, 1); EXPECT_EQ(s.synchronous, 2); EXPECT_EQ(s.foreign_keys, 1); EXPECT_EQ(s.busy_timeout_ms, 50);
    const auto owners = hook->thread_ids(); ASSERT_EQ(owners.size(), 1U);
    EXPECT_NE(*owners.begin(), std::this_thread::get_id());
}

TEST_F(StoreTest, ActualSqlConstraintsEnforceStatesRangesUniqueAndForeignKeys) {
    assigned(1); Reader db(directory.path);
    for (const auto* change : {"state='UNKNOWN'", "state=NULL", "cpu_slots=0", "memory_mb=-1", "timeout_ms=0", "state_version=-1", "task_id=NULL"})
        EXPECT_EQ(db.exec_code(std::string("UPDATE tasks SET ") + change), SQLITE_CONSTRAINT) << change;
    for (const auto* change : {"state='ACTIVE'", "state=NULL", "attempt_no=0", "logs_complete=2", "stdout_bytes=-1", "stderr_final_sequence=-1", "state_version=-1", "worker_id=NULL", "task_id='orphan'"})
        EXPECT_EQ(db.exec_code(std::string("UPDATE attempts SET ") + change), SQLITE_CONSTRAINT) << change;
    EXPECT_EQ(db.exec_code("INSERT INTO attempts SELECT 'other',task_id,attempt_no,state,state_version,worker_id,session_id,assigned_at,acknowledged_at,started_at,finished_at,exit_code,term_signal,reason,observed_outcome_json,stdout_path,stderr_path,stdout_bytes,stderr_bytes,stdout_final_sequence,stderr_final_sequence,logs_complete FROM attempts"), SQLITE_CONSTRAINT);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM attempts"), "1");
    for (const auto state : {"PENDING", "ACTIVE", "SUCCEEDED", "FAILED", "CANCELLED"})
        EXPECT_EQ(db.exec_code(std::string("UPDATE tasks SET state='") + state + "'"), SQLITE_OK);
    for (const auto state : {"ASSIGNED", "STARTING", "RUNNING", "SUCCEEDED", "FAILED", "TIMED_OUT", "CANCELLED", "LOST"})
        EXPECT_EQ(db.exec_code(std::string("UPDATE attempts SET state='") + state + "'"), SQLITE_OK);
}

TEST_F(StoreTest, CanonicalJsonNullableFieldsAndSignedIntegerBoundaryRoundTrip) {
    auto s = spec(1); s.cwd = "/tmp"; s.memory_mb = static_cast<std::uint64_t>(INT64_MAX);
    s.timeout_ms = static_cast<std::uint64_t>(INT64_MAX); s.cpu_slots = UINT32_MAX;
    s.args = {"", "quote\"", "unicode-中文"}; s.env["EMPTY"] = "";
    ASSERT_TRUE(run(Submit{s, kAt}).ok());
    const auto result = run(ReadTask{s.task_id}); ASSERT_TRUE(result.ok()); ASSERT_EQ(result.tasks.size(), 1U);
    EXPECT_FALSE(result.committed); const auto& t = result.tasks[0];
    EXPECT_EQ(t.spec.args, s.args); EXPECT_EQ(t.spec.env, s.env); EXPECT_EQ(t.spec.cwd, s.cwd);
    EXPECT_EQ(t.spec.memory_mb, s.memory_mb); EXPECT_EQ(t.spec.timeout_ms, s.timeout_ms); EXPECT_EQ(t.spec.cpu_slots, UINT32_MAX);
    EXPECT_FALSE(t.cancel_requested_at); EXPECT_FALSE(t.latest_attempt_id); EXPECT_EQ(t.created_at, kAt);
    Reader db(directory.path); EXPECT_EQ(db.scalar("SELECT env_json FROM tasks"), "{\"A\":\"first\",\"EMPTY\":\"\",\"Z\":\"last\"}");
    s.task_id = uuid(2); s.memory_mb++;
    auto invalid = run(Submit{s, kAt}); ASSERT_TRUE(invalid.error); EXPECT_EQ(invalid.error->code, ErrorCode::Invalid);
    s.memory_mb = 0; s.timeout_ms = static_cast<std::uint64_t>(INT64_MAX) + 1;
    EXPECT_FALSE(run(Submit{s, kAt}).ok());
    EXPECT_EQ(db.scalar("SELECT count(*) FROM tasks"), "1");
}

TEST_F(StoreTest, InvalidMetadataCannotTruncateCBindingsOrCoerceTypes) {
    for (int which = 0; which < 6; ++which) {
        auto s = spec(1); std::string at = kAt;
        if (which == 0) s.task_id += '\0';
        if (which == 1) s.executable = std::string("a\0b", 3);
        if (which == 2) s.cwd = std::string("a\0b", 3);
        if (which == 3) s.env["BAD=KEY"] = "value";
        if (which == 4) s.args.push_back(std::string("a\0b", 3));
        if (which == 5) at = "2026-10-19T00:00:00Z";
        auto o = run(Submit{s, at}); ASSERT_TRUE(o.error); EXPECT_EQ(o.error->code, ErrorCode::Invalid);
    }
    EXPECT_EQ(Reader(directory.path).scalar("SELECT count(*) FROM tasks"), "0");
}

TEST_F(StoreTest, StableTuplePaginationAndPendingOrderingIncludingTimestampTies) {
    submit(1); submit(3); submit(2);
    ListTasks query; query.limit = 2;
    auto first = run(query); ASSERT_TRUE(first.ok()); ASSERT_EQ(first.tasks.size(), 2U);
    EXPECT_EQ(first.tasks[0].spec.task_id, uuid(3)); EXPECT_EQ(first.tasks[1].spec.task_id, uuid(2));
    ASSERT_TRUE(first.next); EXPECT_TRUE(first.has_more); query.after = first.next;
    auto last = run(query); ASSERT_TRUE(last.ok()); ASSERT_EQ(last.tasks.size(), 1U);
    EXPECT_EQ(last.tasks[0].spec.task_id, uuid(1)); EXPECT_FALSE(last.has_more);
    query.after.reset(); query.state = domain::TaskState::PENDING; query.oldest_first = true;
    first = run(query); EXPECT_EQ(first.tasks[0].spec.task_id, uuid(1)); EXPECT_EQ(first.tasks[1].spec.task_id, uuid(2));
    query.limit = 0; EXPECT_FALSE(run(query).ok()); query.limit = 201; EXPECT_FALSE(run(query).ok());
    EXPECT_FALSE(run(ReadTask{uuid(1)}, 512).ok()); // real bounded snapshot, not truncated success
}

TEST_F(StoreTest, ReopenIsIdempotentAndControllerLifetimeLockExcludesAnotherProcess) {
    submit(1);
    const pid_t child = ::fork(); ASSERT_GE(child, 0);
    if (child == 0) {
        // No SQLite or C++ runtime activity in this multithreaded post-fork path.
        ::execl(TS_STORAGE_LOCK_HELPER_PATH, TS_STORAGE_LOCK_HELPER_PATH, directory.path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0; ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status), 0);
    channel.reset(); open(); EXPECT_EQ(run(ReadTask{uuid(1)}).tasks[0].state, domain::TaskState::PENDING);
}

TEST(StorageOpen, UnsupportedUnversionedAndIncompatibleDatabasesAreNotRecreated) {
    for (const std::string& setup : {std::string("PRAGMA user_version=2"), std::string("CREATE TABLE unrelated(x TEXT)"),
                                   std::string("CREATE TABLE tasks(x TEXT); PRAGMA user_version=1")}) {
        TempDirectory directory;
        { Reader db(directory.path); db.exec(setup); }
        auto result = persistence::Channel::start({directory.path, 50, kAt, nullptr}); ASSERT_TRUE(result);
        auto c = std::move(result).value(); EXPECT_FALSE(startup(*c));
        ASSERT_TRUE(c->status().error); EXPECT_EQ(c->status().error->code, ErrorCode::Unsupported);
        c.reset(); Reader db(directory.path);
        if (setup == "CREATE TABLE unrelated(x TEXT)") {
            EXPECT_EQ(db.scalar("PRAGMA user_version"), "0");
            EXPECT_EQ(db.scalar("SELECT name FROM sqlite_master WHERE type='table'"), "unrelated");
        } else {
            EXPECT_NE(db.scalar("PRAGMA user_version"), "0");
            EXPECT_EQ(db.scalar("SELECT count(*) FROM sqlite_master WHERE name='attempts'"), "0");
        }
    }
}

TEST_F(StoreTest, MalformedPersistedRowsAreExplicitFatalErrors) {
    submit(1); Reader db(directory.path);
    db.exec("UPDATE tasks SET args_json='[1]'");
    auto o = run(ReadTask{uuid(1)}); ASSERT_TRUE(o.error);
    EXPECT_EQ(o.error->code, ErrorCode::Corrupt); EXPECT_TRUE(o.error->fatal);
    EXPECT_FALSE(o.committed);
}

class CorruptTaskRow : public StoreTest, public ::testing::WithParamInterface<const char*> {};
TEST_P(CorruptTaskRow, TypedSnapshotsRejectMalformedPersistedValues) {
    submit(1); Reader db(directory.path); db.exec(std::string("UPDATE tasks SET ") + GetParam());
    auto o = run(ListTasks{}); ASSERT_TRUE(o.error);
    EXPECT_EQ(o.error->code, ErrorCode::Corrupt); EXPECT_TRUE(o.error->fatal); EXPECT_FALSE(o.committed);
}
INSTANTIATE_TEST_SUITE_P(StrictDiskTypesAndShapes, CorruptTaskRow, ::testing::Values(
    "cpu_slots='not-a-number'", "memory_mb=1.5", "timeout_ms='not-a-number'", "cwd=x'6162'",
    "created_at='yesterday'", "task_id='not-a-uuid'", "args_json='[1]'",
    "env_json='{\"A\":\"one\",\"A\":\"two\"}'", "latest_attempt_id='not-a-uuid'", "state_version=1.5"));

class CorruptAttemptRow : public StoreTest, public ::testing::WithParamInterface<const char*> {};
TEST_P(CorruptAttemptRow, HistoryRejectsMalformedNullableFieldsAndLifecycle) {
    assigned(1); Reader db(directory.path); db.exec(std::string("UPDATE attempts SET ") + GetParam());
    auto o = run(ReadAttempts{uuid(1)}); ASSERT_TRUE(o.error);
    EXPECT_EQ(o.error->code, ErrorCode::Corrupt); EXPECT_TRUE(o.error->fatal); EXPECT_FALSE(o.committed);
}
INSTANTIATE_TEST_SUITE_P(StrictDiskTypesAndLifecycle, CorruptAttemptRow, ::testing::Values(
    "exit_code='text'", "stdout_final_sequence='text'", "assigned_at=NULL",
    "finished_at='2026-10-19T02:02:03.000001Z'", "observed_outcome_json='[]'", "session_id='invalid'"));
} // namespace ts::storage::test
