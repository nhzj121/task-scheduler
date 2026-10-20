#include "../helpers/storage_fixture.h"

#include <sys/wait.h>

namespace ts::storage::test {
using TS = domain::TaskState;
using AS = domain::AttemptState;
class TransactionFault : public StoreTest, public ::testing::WithParamInterface<std::tuple<int, SqlStage>> {
protected:
    Command prepare(int operation) {
        switch (operation) {
            case 0: return Submit{spec(1), kAt};
            case 1: submit(1); return assignment(1);
            case 2: assigned(1); return Advance{av(1, 0), AS::STARTING, kLater};
            case 3: running(1); return CancelIntent{{uuid(1), 1}, kLater};
            case 4: submit(1); return CancelPending{{uuid(1), 0}, kLater};
            case 5: running(1); return finish(1);
            case 6: running(1); EXPECT_TRUE(run(finish(1, AS::FAILED)).committed); return Retry{{uuid(1), 2}, kLater};
            case 7: assigned(1); running(2); return LoseSession{uuid(1000), kLater, "SESSION_INVALIDATED"};
            case 8: assigned(1); running(2); return Recover{kLater};
            default: throw std::logic_error("invalid test operation");
        }
    }
};
TEST_P(TransactionFault, SqlFailureRollsBackWholeOperationAndNeverEnablesEffect) {
    const auto operation = std::get<0>(GetParam()); const auto stage = std::get<1>(GetParam());
    const auto command = prepare(operation); Reader db(directory.path);
    const auto before_tasks = db.rows("SELECT * FROM tasks ORDER BY task_id");
    const auto before_attempts = db.rows("SELECT * FROM attempts ORDER BY attempt_id");
    const char* needle = operation == 0 ? "INSERT INTO tasks" : operation == 1 ? "INSERT INTO attempts" :
                         operation == 2 ? "UPDATE attempts" : "UPDATE tasks";
    const auto nth = (operation >= 7 && stage != SqlStage::Commit) ? 2U : 1U;
    hook->arm(stage, stage == SqlStage::Commit ? "COMMIT" : needle, SQLITE_IOERR, nth);
    const auto outcome = run(command); ASSERT_TRUE(outcome.error);
    EXPECT_FALSE(outcome.committed); EXPECT_TRUE(outcome.error->fatal);
    EXPECT_EQ(outcome.error->code, stage == SqlStage::Commit ? ErrorCode::UnknownCommit : ErrorCode::Io);
    EXPECT_EQ(db.rows("SELECT * FROM tasks ORDER BY task_id"), before_tasks);
    EXPECT_EQ(db.rows("SELECT * FROM attempts ORDER BY attempt_id"), before_attempts);
    channel->join(); // retire the owner after fatal error; release process lock
    db.exec("BEGIN IMMEDIATE"); db.exec("ROLLBACK"); // no leaked writer transaction
    const auto s = channel->status(); ASSERT_TRUE(s.error);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(99), kLater})), Admission::Degraded);
    bool effect = false;
    if (outcome.ok() && outcome.committed) effect = true;
    EXPECT_FALSE(effect); // tests only, not a fake production Controller
}
INSTANTIATE_TEST_SUITE_P(AllNineTransactions, TransactionFault,
    ::testing::Combine(::testing::Range(0, 9), ::testing::Values(SqlStage::Prepare, SqlStage::Bind, SqlStage::Step, SqlStage::Commit)));

TEST_F(StoreTest, AmbiguousPostCommitFailureIsFatalEvenWhenRowsActuallyCommitted) {
    hook->arm(SqlStage::AfterCommit, "COMMIT", SQLITE_IOERR);
    auto o = run(Submit{spec(1), kAt}); ASSERT_TRUE(o.error);
    EXPECT_EQ(o.error->code, ErrorCode::UnknownCommit); EXPECT_TRUE(o.error->fatal); EXPECT_FALSE(o.committed);
    Reader db(directory.path); EXPECT_EQ(db.scalar("SELECT state FROM tasks"), "PENDING");
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(2), kAt})), Admission::Degraded);
}

TEST_F(StoreTest, RollbackFailureRetiresConnectionAndNoCommandIsReportedCommitted) {
    submit(1); hook->arm(SqlStage::Step, "INSERT INTO attempts", SQLITE_FULL); hook->fail_rollback();
    auto o = run(assignment(1)); ASSERT_TRUE(o.error); EXPECT_TRUE(o.error->fatal); EXPECT_FALSE(o.committed);
    channel->join(); Reader db(directory.path);
    EXPECT_EQ(db.scalar("SELECT state FROM tasks"), "PENDING");
    EXPECT_EQ(db.scalar("SELECT count(*) FROM attempts"), "0");
    db.exec("BEGIN IMMEDIATE"); db.exec("ROLLBACK");
}

TEST_F(StoreTest, PhysicalWriterContentionTimesOutExplicitlyWithoutBlockingProducer) {
    Reader lock(directory.path); lock.exec("BEGIN IMMEDIATE");
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(1), kAt})), Admission::Accepted);
    const auto results = wait(1); const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_EQ(results.size(), 1U); ASSERT_TRUE(results[0].outcome.error);
    EXPECT_EQ(results[0].outcome.error->code, ErrorCode::Busy);
    EXPECT_TRUE(results[0].outcome.error->fatal); EXPECT_LT(elapsed, std::chrono::seconds(2));
    lock.exec("ROLLBACK"); EXPECT_EQ(lock.scalar("SELECT count(*) FROM tasks"), "0");
}

TEST_F(StoreTest, SimulatedFullAndIoErrorsAreTypedAndAbortQueuedAcceptedWorkLosslessly) {
    hook->arm(SqlStage::Step, "INSERT INTO tasks", SQLITE_FULL, 1, true);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(1), kAt})), Admission::Accepted);
    const bool entered = hook->wait_entered(); EXPECT_TRUE(entered);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(2), kAt})), Admission::Accepted);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(3), kAt})), Admission::Accepted);
    hook->release(); const auto results = wait(3); ASSERT_EQ(results.size(), 3U);
    ASSERT_TRUE(results[0].outcome.error); EXPECT_EQ(results[0].outcome.error->code, ErrorCode::Full);
    for (std::size_t i = 1; i < results.size(); ++i) {
        ASSERT_TRUE(results[i].outcome.error); EXPECT_EQ(results[i].outcome.error->code, ErrorCode::Aborted);
    }
    EXPECT_EQ(channel->usage().count, 0U);
    EXPECT_EQ(Reader(directory.path).scalar("SELECT count(*) FROM tasks"), "0");
}

TEST_F(StoreTest, StartupRecoveryReopensDatabaseAndPreservesPendingAndTerminalHistory) {
    submit(1); assigned(2); assigned(3);
    ASSERT_TRUE(run(Advance{av(3, 0), AS::STARTING, kAt}).committed);
    running(4); running(5); ASSERT_TRUE(run(finish(5)).committed);
    Reader db(directory.path);
    const auto pending = db.rows("SELECT * FROM tasks WHERE task_id='" + uuid(1) + "'");
    const auto terminal = db.rows("SELECT * FROM attempts WHERE task_id='" + uuid(5) + "'");
    channel.reset(); open(); EXPECT_EQ(channel->status().recovered_attempts, 3U);
    EXPECT_EQ(db.rows("SELECT * FROM tasks WHERE task_id='" + uuid(1) + "'"), pending);
    EXPECT_EQ(db.rows("SELECT * FROM attempts WHERE task_id='" + uuid(5) + "'"), terminal);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM tasks WHERE state='FAILED'"), "3");
    EXPECT_EQ(db.scalar("SELECT count(*) FROM attempts WHERE state='LOST' AND logs_complete=0 AND reason='CONTROLLER_RESTART'"), "3");
    EXPECT_EQ(db.scalar("SELECT count(*) FROM sqlite_master WHERE type='table'"), "2");
    // Paths in the rows have never existed: recovery has no log-file inference path.
    EXPECT_FALSE(std::filesystem::exists(directory.path + "/logs"));
}

TEST_F(StoreTest, FailedStartupRecoveryIsAtomicAndNeverPublishesReady) {
    assigned(1); running(2); channel.reset();
    Reader db(directory.path); const auto tasks = db.rows("SELECT * FROM tasks ORDER BY task_id");
    const auto attempts = db.rows("SELECT * FROM attempts ORDER BY attempt_id");
    hook->arm(SqlStage::Step, "UPDATE tasks", SQLITE_IOERR, 2);
    auto result = persistence::Channel::start({directory.path, 50, kLater, hook}); ASSERT_TRUE(result);
    channel = std::move(result).value(); EXPECT_FALSE(startup(*channel));
    ASSERT_TRUE(channel->status().error); EXPECT_EQ(channel->status().health, Health::Degraded);
    EXPECT_EQ(db.rows("SELECT * FROM tasks ORDER BY task_id"), tasks);
    EXPECT_EQ(db.rows("SELECT * FROM attempts ORDER BY attempt_id"), attempts);
    EXPECT_EQ(channel->try_post(envelope(Submit{spec(3), kAt})), Admission::Degraded);
}

TEST_F(StoreTest, DuplicateAttemptInsertRollsBackTaskAssignmentAndIsNotDiskFailure) {
    assigned(1); submit(2); auto c = assignment(2); c.attempt_id = uuid(10001);
    auto o = run(c); ASSERT_TRUE(o.error); EXPECT_EQ(o.error->code, ErrorCode::Constraint);
    EXPECT_FALSE(o.error->fatal); EXPECT_FALSE(o.committed);
    EXPECT_EQ(run(ReadTask{uuid(2)}).tasks[0].state, TS::PENDING);
    EXPECT_EQ(channel->status().health, Health::Ready);
}

TEST(StorageRecovery, AbruptProcessExitRetainsCommittedWalAndReleasesOwnershipLock) {
    TempDirectory directory;
    const pid_t child = ::fork(); ASSERT_GE(child, 0);
    if (child == 0) {
        ::execl(TS_STORAGE_LOCK_HELPER_PATH, TS_STORAGE_LOCK_HELPER_PATH, directory.path.c_str(), "--seed-and-crash", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0; ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status)); ASSERT_EQ(WEXITSTATUS(status), 0);
    EXPECT_TRUE(std::filesystem::exists(directory.path + "/metadata.sqlite3-wal"));
    auto result = persistence::Channel::start({directory.path, 50, kLater, nullptr}); ASSERT_TRUE(result);
    auto c = std::move(result).value(); ASSERT_TRUE(startup(*c)); EXPECT_EQ(c->status().recovered_attempts, 1U);
    Reader db(directory.path);
    EXPECT_EQ(db.rows("SELECT t.state,a.state,a.logs_complete FROM tasks t JOIN attempts a ON a.task_id=t.task_id"),
              (std::vector<std::vector<std::string>>{{"FAILED","LOST","0"}}));
}
} // namespace ts::storage::test
