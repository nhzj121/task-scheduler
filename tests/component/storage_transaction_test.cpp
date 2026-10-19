#include "../helpers/storage_fixture.h"

namespace ts::storage::test {
using TS = domain::TaskState;
using AS = domain::AttemptState;

TEST_F(StoreTest, SubmitDuplicateAndAssignmentCommitWithExactBindingAndVersions) {
    submit(1);
    auto duplicate = run(Submit{spec(1), kAt}); ASSERT_TRUE(duplicate.error);
    EXPECT_EQ(duplicate.error->code, ErrorCode::Constraint); EXPECT_FALSE(duplicate.committed);
    auto o = run(assignment(1)); ASSERT_TRUE(o.ok()); EXPECT_TRUE(o.committed);
    Reader db(directory.path);
    EXPECT_EQ(db.scalar("SELECT state FROM tasks"), "ACTIVE");
    EXPECT_EQ(db.scalar("SELECT state_version FROM tasks"), "1");
    EXPECT_EQ(db.scalar("SELECT latest_attempt_id FROM tasks"), uuid(10001));
    auto rows = run(ReadAttempts{uuid(1)}); ASSERT_TRUE(rows.ok()); ASSERT_EQ(rows.attempts.size(), 1U);
    const auto& a = rows.attempts[0];
    EXPECT_EQ(a.state, AS::ASSIGNED); EXPECT_EQ(a.state_version, 0U); EXPECT_EQ(a.attempt_no, 1U);
    EXPECT_EQ(a.worker_id, "worker-1"); EXPECT_EQ(a.session_id, uuid(1000)); EXPECT_EQ(a.assigned_at, kAt);
    EXPECT_FALSE(a.acknowledged_at); EXPECT_FALSE(a.started_at); EXPECT_FALSE(a.finished_at);
    EXPECT_FALSE(a.logs_complete); EXPECT_FALSE(a.stdout_final_sequence);
    auto stale = run(assignment(1)); ASSERT_TRUE(stale.error); EXPECT_EQ(stale.error->code, ErrorCode::Conflict);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM attempts"), "1");
}

TEST_F(StoreTest, IntermediateTransitionsAreExactAndCasProtectsTaskAttemptAndSession) {
    assigned(1);
    auto skip = run(Advance{av(1, 0), AS::RUNNING, kAt}); ASSERT_TRUE(skip.error); EXPECT_EQ(skip.error->code, ErrorCode::Invalid);
    auto wrong_session = av(1, 0); wrong_session.session_id = uuid(2000);
    EXPECT_FALSE(run(Advance{wrong_session, AS::STARTING, kAt}).ok());
    EXPECT_FALSE(run(Advance{av(1, 0, 0), AS::STARTING, kAt}).ok());
    ASSERT_TRUE(run(Advance{av(1, 0), AS::STARTING, kAt}).committed);
    EXPECT_FALSE(run(Advance{av(1, 0), AS::STARTING, kAt}).ok());
    ASSERT_TRUE(run(Advance{av(1, 1), AS::RUNNING, kLater}).committed);
    const auto o = run(ReadAttempts{uuid(1)}); ASSERT_TRUE(o.ok());
    EXPECT_EQ(o.attempts[0].state, AS::RUNNING); EXPECT_EQ(o.attempts[0].state_version, 2U);
    EXPECT_EQ(o.attempts[0].acknowledged_at, kAt); EXPECT_EQ(o.attempts[0].started_at, kLater);
    EXPECT_EQ(run(ReadTask{uuid(1)}).tasks[0].state_version, 1U);
}

TEST_F(StoreTest, PendingCancellationCreatesNoAttemptAndActiveCancellationPersistsOnlyIntent) {
    submit(1); ASSERT_TRUE(run(CancelPending{{uuid(1), 0}, kLater}).committed);
    EXPECT_EQ(run(ReadTask{uuid(1)}).tasks[0].state, TS::CANCELLED);
    EXPECT_TRUE(run(ReadAttempts{uuid(1)}).attempts.empty());
    EXPECT_FALSE(run(Retry{{uuid(1), 1}, kLater}).ok());
    running(2);
    ASSERT_TRUE(run(CancelIntent{{uuid(2), 1}, kLater}).committed);
    auto t = run(ReadTask{uuid(2)}).tasks[0];
    EXPECT_EQ(t.state, TS::ACTIVE); EXPECT_EQ(t.state_version, 2U); EXPECT_EQ(t.cancel_requested_at, kLater);
    EXPECT_EQ(run(ReadAttempts{uuid(2)}).attempts[0].state, AS::RUNNING);
    EXPECT_FALSE(run(CancelIntent{{uuid(2), 1}, kLater}).ok());
    EXPECT_FALSE(run(CancelIntent{{uuid(2), 2}, kLater}).ok());
    auto c = finish(2, AS::CANCELLED); c.expected.task.version = 2;
    EXPECT_TRUE(run(c).committed);
}

TEST_F(StoreTest, AllObservedTerminalOutcomesRequireDataAndNamespaceEvidence) {
    unsigned int n = 1;
    for (const auto to : {AS::SUCCEEDED, AS::FAILED, AS::TIMED_OUT, AS::CANCELLED}) {
        running(n); auto c = finish(n, to);
        c.stdout_bytes = 13; c.stderr_bytes = 8; c.stdout_final_sequence = 2; c.stderr_final_sequence = 1;
        c.exit_code = to == AS::SUCCEEDED ? 0 : 3; c.reason = "OBSERVED_OUTCOME";
        c.logs.attempt_id = uuid(9999); EXPECT_FALSE(run(c).ok()); c.logs.attempt_id = c.expected.attempt_id;
        c.logs.prior_writes_completed = false; EXPECT_FALSE(run(c).ok()); c.logs.prior_writes_completed = true;
        c.logs.final_counters_verified = false; EXPECT_FALSE(run(c).ok()); c.logs.final_counters_verified = true;
        c.logs.namespace_synced = false; EXPECT_FALSE(run(c).ok());
        c.logs.namespace_synced = true; c.logs.stderr_synced = false; EXPECT_FALSE(run(c).ok());
        c.logs.stderr_synced = true; c.logs.namespace_failed = true; EXPECT_FALSE(run(c).ok());
        c.logs.namespace_failed = false; c.stdout_final_sequence.reset(); EXPECT_FALSE(run(c).ok());
        c.stdout_final_sequence = 2;
        const auto done = run(c); ASSERT_TRUE(done.ok()); EXPECT_TRUE(done.committed);
        Reader db(directory.path);
        const auto row = db.rows("SELECT t.state,a.state,t.state_version,a.state_version,a.logs_complete,a.stdout_bytes,a.stderr_bytes FROM tasks t JOIN attempts a ON a.task_id=t.task_id WHERE t.task_id='" + uuid(n) + "'");
        ASSERT_EQ(row.size(), 1U);
        EXPECT_EQ(row[0][0], to == AS::SUCCEEDED ? "SUCCEEDED" : to == AS::CANCELLED ? "CANCELLED" : "FAILED");
        EXPECT_EQ(row[0][1], domain::to_string(to)); EXPECT_EQ(row[0][2], "2"); EXPECT_EQ(row[0][3], "3");
        EXPECT_EQ(row[0][4], "1"); EXPECT_EQ(row[0][5], "13"); EXPECT_EQ(row[0][6], "8");
        EXPECT_FALSE(run(c).ok()); ++n;
    }
}

TEST_F(StoreTest, ProvenEmptyIsOnlyForExplicitRejectionOrUndispatchedCancellation) {
    assigned(1); auto rejected = finish(1, AS::FAILED); rejected.expected.version = 0;
    rejected.reason = "DISPATCH_REJECTED"; rejected.logs = {}; rejected.logs.kind = LogEvidenceKind::ProvenEmpty;
    EXPECT_FALSE(run(rejected).ok()); rejected.logs.explicit_dispatch_rejection = true;
    rejected.logs.namespace_failed = true; EXPECT_FALSE(run(rejected).ok()); rejected.logs.namespace_failed = false;
    rejected.stdout_bytes = 1; EXPECT_FALSE(run(rejected).ok()); rejected.stdout_bytes = 0;
    EXPECT_TRUE(run(rejected).committed);
    assigned(2); auto cancelled = finish(2, AS::CANCELLED); cancelled.expected.version = 0;
    cancelled.logs = {}; cancelled.logs.kind = LogEvidenceKind::ProvenEmpty;
    EXPECT_FALSE(run(cancelled).ok()); cancelled.logs.no_dispatch_bytes = true; EXPECT_TRUE(run(cancelled).committed);
    assigned(3); auto lost = finish(3, AS::LOST); lost.expected.version = 0;
    lost.logs = rejected.logs; EXPECT_FALSE(run(lost).ok()); lost.logs = {}; lost.reason = "SESSION_LOST";
    ASSERT_TRUE(run(lost).committed); const auto a = run(ReadAttempts{uuid(3)}).attempts[0];
    EXPECT_EQ(a.state, AS::LOST); EXPECT_FALSE(a.logs_complete);
}

TEST_F(StoreTest, StartingFailureAndCancellationStillRequireBarrier) {
    for (unsigned int n = 1; n <= 2; ++n) {
        assigned(n); ASSERT_TRUE(run(Advance{av(n, 0), AS::STARTING, kAt}).ok());
        auto c = finish(n, n == 1 ? AS::FAILED : AS::CANCELLED); c.expected.version = 1;
        c.reason = "EXEC_FAILURE_OR_CANCEL";
        c.logs.kind = LogEvidenceKind::Incomplete; EXPECT_FALSE(run(c).ok());
        c.logs.kind = LogEvidenceKind::FinalBarrier; EXPECT_TRUE(run(c).committed);
    }
}

TEST_F(StoreTest, LogFailurePreservesCanonicalObservedOutcomeWithoutCompleteness) {
    running(1); auto c = finish(1, AS::FAILED); c.reason = "LOG_PERSISTENCE_ERROR";
    c.logs = {}; EXPECT_FALSE(run(c).ok());
    c.observed_outcome_json = "{\"z\":0,\"exit_code\":0,\"z\":1}"; EXPECT_FALSE(run(c).ok());
    c.observed_outcome_json = "{\"z\":0,\"exit_code\":0}";
    ASSERT_TRUE(run(c).committed);
    const auto a = run(ReadAttempts{uuid(1)}).attempts[0];
    EXPECT_FALSE(a.logs_complete); EXPECT_EQ(a.observed_outcome_json, "{\"exit_code\":0,\"z\":0}");
    EXPECT_EQ(run(ReadTask{uuid(1)}).tasks[0].state, TS::FAILED);
}

TEST_F(StoreTest, RetryPreservesEveryHistoricalAttemptAndIncrementsAttemptNumberOnNewAssignment) {
    running(1); ASSERT_TRUE(run(CancelIntent{{uuid(1), 1}, kLater}).committed);
    auto c = finish(1, AS::FAILED); c.expected.task.version = 2; ASSERT_TRUE(run(c).committed);
    Reader db(directory.path); const auto history = db.rows("SELECT * FROM attempts");
    EXPECT_FALSE(run(Retry{{uuid(1), 2}, kLater}).ok());
    ASSERT_TRUE(run(Retry{{uuid(1), 3}, kLater}).committed);
    EXPECT_EQ(db.rows("SELECT * FROM attempts"), history);
    auto t = run(ReadTask{uuid(1)}).tasks[0]; EXPECT_EQ(t.state, TS::PENDING);
    EXPECT_FALSE(t.cancel_requested_at); EXPECT_EQ(t.latest_attempt_id, uuid(10001));
    auto assign = assignment(1); assign.expected.version = 4; assign.attempt_id = uuid(20001);
    ASSERT_TRUE(run(assign).committed);
    auto attempts = run(ReadAttempts{uuid(1), 0, 1}); ASSERT_EQ(attempts.attempts.size(), 1U);
    EXPECT_TRUE(attempts.has_more); EXPECT_EQ(attempts.attempts[0].attempt_id, uuid(10001));
    attempts = run(ReadAttempts{uuid(1), 1, 1}); ASSERT_EQ(attempts.attempts.size(), 1U);
    EXPECT_EQ(attempts.attempts[0].attempt_id, uuid(20001)); EXPECT_EQ(attempts.attempts[0].attempt_no, 2U);
}

TEST_F(StoreTest, SessionLossAtomicallyUpdatesWholeSessionNotOtherSessions) {
    assigned(1); running(2); submit(3); ASSERT_TRUE(run(assignment(3, 2000)).committed);
    const auto o = run(LoseSession{uuid(1000), kLater, "HEARTBEAT_TIMEOUT"});
    ASSERT_TRUE(o.committed); EXPECT_EQ(o.affected, 2U);
    Reader db(directory.path);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM tasks WHERE state='FAILED'"), "2");
    EXPECT_EQ(db.scalar("SELECT count(*) FROM attempts WHERE state='LOST' AND logs_complete=0 AND reason='HEARTBEAT_TIMEOUT'"), "2");
    EXPECT_EQ(run(ReadTask{uuid(3)}).tasks[0].state, TS::ACTIVE);
    EXPECT_EQ(run(LoseSession{uuid(1000), kLater, "HEARTBEAT_TIMEOUT"}).affected, 0U);
}

TEST_F(StoreTest, TerminalVisibilityIsAtomicToIndependentReaderBeforeAndAfterCommit) {
    running(1); Reader db(directory.path);
    hook->arm(SqlStage::Commit, "COMMIT", SQLITE_OK, 1, true);
    ASSERT_EQ(channel->try_post(envelope(finish(1))), Admission::Accepted);
    const bool entered = hook->wait_entered(); EXPECT_TRUE(entered);
    EXPECT_EQ(db.rows("SELECT t.state,a.state FROM tasks t JOIN attempts a ON a.task_id=t.task_id"),
              (std::vector<std::vector<std::string>>{{"ACTIVE","RUNNING"}}));
    EXPECT_TRUE(channel->drain().value().empty());
    hook->release(); auto result = wait(1); ASSERT_EQ(result.size(), 1U); ASSERT_TRUE(result[0].outcome.committed);
    EXPECT_EQ(db.rows("SELECT t.state,a.state FROM tasks t JOIN attempts a ON a.task_id=t.task_id"),
              (std::vector<std::vector<std::string>>{{"SUCCEEDED","SUCCEEDED"}}));
}

TEST_F(StoreTest, ConcurrentQueuedAssignmentIntentsCannotCreateTwoLiveAttempts) {
    submit(1);
    auto a = assignment(1); auto b = a; b.attempt_id = uuid(30001);
    ASSERT_EQ(channel->try_post(envelope(a)), Admission::Accepted);
    ASSERT_EQ(channel->try_post(envelope(b)), Admission::Accepted);
    auto results = wait(2); ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(results[0].outcome.committed); ASSERT_TRUE(results[1].outcome.error);
    EXPECT_EQ(results[1].outcome.error->code, ErrorCode::Conflict);
    EXPECT_EQ(Reader(directory.path).scalar("SELECT count(*) FROM attempts"), "1");
}

TEST_F(StoreTest, VersionAndFinalCounterOverflowNeverWrapIntoSuccessfulUpdates) {
    submit(1);
    auto a = assignment(1); a.expected.version = static_cast<std::uint64_t>(INT64_MAX);
    auto failure = run(a); ASSERT_TRUE(failure.error); EXPECT_FALSE(failure.committed);
    EXPECT_EQ(run(ReadTask{uuid(1)}).tasks[0].state, TS::PENDING);
    running(2); auto c = finish(2); c.stdout_bytes = static_cast<std::uint64_t>(INT64_MAX) + 1;
    EXPECT_FALSE(run(c).ok()); c.stdout_bytes = 0; c.stderr_final_sequence = static_cast<std::uint64_t>(INT64_MAX) + 1;
    EXPECT_FALSE(run(c).ok());
    c.stderr_final_sequence = static_cast<std::uint64_t>(INT64_MAX);
    c.stdout_bytes = static_cast<std::uint64_t>(INT64_MAX);
    c.stderr_bytes = static_cast<std::uint64_t>(INT64_MAX); c.stdout_final_sequence = 1;
    EXPECT_TRUE(run(c).committed);
    const auto row = run(ReadAttempts{uuid(2)}).attempts[0];
    EXPECT_EQ(row.stdout_bytes, static_cast<std::uint64_t>(INT64_MAX));
    EXPECT_EQ(row.stderr_final_sequence, std::optional<std::uint64_t>(INT64_MAX));
}
} // namespace ts::storage::test
