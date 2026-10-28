#include "ts/logs/writer.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include "ts/domain/ids.h"

namespace {
using namespace ts;
using namespace logs;
struct Hook final : FilesystemHook {
    std::mutex mutex;
    std::vector<std::pair<Operation, std::string>> trace;
    std::optional<std::pair<Operation, std::string>> fault;
    std::size_t maximum{SIZE_MAX}; bool interrupt_once{false};
    std::optional<Operation> interrupt_operation;
    std::optional<std::size_t> writes_before_failure;
    std::vector<std::thread::id> owners;
    std::condition_variable cv;
    bool block_write{false}, entered{false}, proceed{false};
    FaultAction before(Operation op, const std::string& path, std::size_t) override {
        std::unique_lock<std::mutex> lock(mutex); trace.emplace_back(op, path); owners.push_back(std::this_thread::get_id());
        if (op == Operation::Write && block_write && !entered) { entered = true; cv.notify_all(); cv.wait(lock, [&] { return proceed; }); }
        if (fault && fault->first == op && (fault->second.empty() || path == fault->second)) return {EIO, 0};
        if (interrupt_operation && *interrupt_operation == op) { interrupt_operation.reset(); return {EINTR, 0}; }
        if (op == Operation::Write && interrupt_once) { interrupt_once = false; return {EINTR, 0}; }
        if (op == Operation::Write && writes_before_failure) {
            if (!*writes_before_failure) return {EIO, 0};
            --*writes_before_failure;
        }
        return {0, maximum};
    }
};
class LogWriter : public ::testing::Test {
protected:
    std::string root;
    std::shared_ptr<Hook> hook{std::make_shared<Hook>()};
    std::unique_ptr<Writer> writer;
    Binding binding;
    std::uint64_t operation{1};
    void SetUp() override {
        char pattern[] = "/tmp/task-scheduler-log-unit-XXXXXX";
        const auto* path = ::mkdtemp(pattern); ASSERT_NE(path, nullptr); root = path;
        binding = {domain::TaskId::generate().value(), domain::AttemptId::generate().value(), domain::SessionId::generate().value(), 1, 1, "", ""};
        const auto stem = "tasks/" + binding.task + "/attempt-1";
        binding.stdout_path = stem + ".stdout.log"; binding.stderr_path = stem + ".stderr.log";
    }
    void start(Limits limits = {}) {
        auto result = Writer::start(root, limits, hook); ASSERT_TRUE(result); writer = std::move(result).value();
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (writer->status().health == Health::Starting && std::chrono::steady_clock::now() < end) std::this_thread::yield();
        ASSERT_EQ(writer->status().health, Health::Ready);
    }
    Completion run(Command command) {
        command.binding = binding; command.operation = operation++;
        EXPECT_EQ(writer->try_post(std::move(command)), Admission::Accepted);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end) {
            auto result = writer->drain(); EXPECT_TRUE(result);
            if (result && !result.value().empty()) return std::move(result.value().front());
            std::this_thread::yield();
        }
        throw std::runtime_error("writer completion deadline");
    }
    void TearDown() override {
        { std::lock_guard<std::mutex> lock(hook->mutex); hook->proceed = true; hook->cv.notify_all(); }
        if (writer) { writer->stop(); writer->join(); writer.reset(); }
        if (!root.empty()) std::filesystem::remove_all(root); // exact mkdtemp-owned fixture only
    }
};
TEST_F(LogWriter, BinaryShortWritesAndEintrHaveExactIndependentVisibleCounters) {
    hook->maximum = 2; hook->interrupt_once = true; start();
    Command append; append.kind = Kind::Append; append.data = std::string("a\0\xffz", 4);
    auto out = run(append); ASSERT_FALSE(out.error); EXPECT_EQ(out.visible.out.bytes, 4U);
    append.stream = Stream::Err; append.data = "err"; auto err = run(append); ASSERT_FALSE(err.error);
    EXPECT_EQ(err.visible.err.bytes, 3U); EXPECT_EQ(err.visible.out.sequence, 1U);
    Command read; read.kind = Kind::Read; auto page = run(read); ASSERT_FALSE(page.error);
    EXPECT_EQ(page.data, std::string("a\0\xffz", 4)); EXPECT_EQ(page.snapshot_end, 4U);
    Command barrier; barrier.kind = Kind::Barrier; barrier.final = err.visible;
    auto done = run(barrier); ASSERT_FALSE(done.error); EXPECT_TRUE(done.barrier_ok);
}
TEST_F(LogWriter, EmptyBarrierSyncsBothFilesThenExactlyTheDirtyNamespaceChain) {
    start(); Command barrier; barrier.kind = Kind::Barrier; const auto done = run(barrier);
    ASSERT_FALSE(done.error); EXPECT_TRUE(done.barrier_ok);
    EXPECT_EQ(std::filesystem::file_size(root + "/" + binding.stdout_path), 0U);
    EXPECT_EQ(std::filesystem::file_size(root + "/" + binding.stderr_path), 0U);
    std::vector<std::pair<Operation, std::string>> syncs;
    { std::lock_guard<std::mutex> lock(hook->mutex);
      for (const auto& op : hook->trace) if (op.first == Operation::FileSync || op.first == Operation::DirectorySync) syncs.push_back(op); }
    const std::vector<std::pair<Operation, std::string>> expected{{Operation::FileSync, binding.stdout_path},
        {Operation::FileSync, binding.stderr_path}, {Operation::DirectorySync, "tasks/" + binding.task},
        {Operation::DirectorySync, "tasks"}, {Operation::DirectorySync, "."}};
    EXPECT_EQ(syncs, expected);
    EXPECT_FALSE(std::filesystem::exists(root + "/tasks/" + binding.task + "/attempt-1"));
}
TEST_F(LogWriter, EachFileAndDirectorySyncFailureNeverProducesSuccessfulEvidence) {
    const std::vector<std::pair<Operation, std::string>> faults{{Operation::FileSync, binding.stdout_path},
        {Operation::FileSync, binding.stderr_path}, {Operation::DirectorySync, "tasks/" + binding.task},
        {Operation::DirectorySync, "tasks"}, {Operation::DirectorySync, "."}};
    for (const auto& fault : faults) {
        hook = std::make_shared<Hook>(); hook->fault = fault; start();
        Command barrier; barrier.kind = Kind::Barrier; const auto done = run(barrier);
        ASSERT_TRUE(done.error); EXPECT_EQ(done.error->operation, fault.first); EXPECT_FALSE(done.barrier_ok);
        writer->stop(); writer->join(); writer.reset();
    }
}
TEST_F(LogWriter, SnapshotEndDoesNotGrowAndReadErrorsAreExplicit) {
    start(); Command append; append.data = "abc"; ASSERT_FALSE(run(append).error);
    Command read; read.kind = Kind::Read; read.limit = 1; auto first = run(read); ASSERT_FALSE(first.error);
    EXPECT_EQ(first.snapshot_end, 3U); EXPECT_EQ(first.data, "a");
    append.sequence = 1; append.data = "def"; ASSERT_FALSE(run(append).error);
    read.offset = 1; read.limit = 10; read.snapshot_end = first.snapshot_end;
    auto tail = run(read); ASSERT_FALSE(tail.error); EXPECT_EQ(tail.data, "bc");
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->fault = {{Operation::Read, binding.stdout_path}}; }
    EXPECT_TRUE(run(read).error);
}
TEST_F(LogWriter, RepeatedAttemptOnlySyncsTheDirectoryWhoseFilesWereCreated) {
    start(); Command barrier; barrier.kind = Kind::Barrier; ASSERT_TRUE(run(barrier).barrier_ok);
    Command forget; forget.kind = Kind::Forget; ASSERT_FALSE(run(forget).error);
    binding.attempt = domain::AttemptId::generate().value(); binding.number = 2;
    const auto stem = "tasks/" + binding.task + "/attempt-2";
    binding.stdout_path = stem + ".stdout.log"; binding.stderr_path = stem + ".stderr.log";
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->trace.clear(); }
    ASSERT_TRUE(run(barrier).barrier_ok);
    std::vector<std::string> directories;
    { std::lock_guard<std::mutex> lock(hook->mutex);
      for (const auto& op : hook->trace) if (op.first == Operation::DirectorySync) directories.push_back(op.second); }
    EXPECT_EQ(directories, std::vector<std::string>{"tasks/" + binding.task});
}
TEST_F(LogWriter, TraversalSymlinksAndNonregularFilesCannotEscapeTheTrustedRoot) {
    auto invalid = binding; invalid.stdout_path = "../outside";
    start(); Command command; command.kind = Kind::Barrier; command.binding = invalid; command.operation = 77;
    EXPECT_EQ(writer->try_post(command), Admission::Invalid);
    writer->stop(); writer->join(); writer.reset();
    std::filesystem::create_directory(root + "/outside");
    std::filesystem::create_directory_symlink(root + "/outside", root + "/tasks");
    start(); command.binding = binding;
    auto result = run(command); ASSERT_TRUE(result.error); EXPECT_FALSE(result.barrier_ok);
    EXPECT_TRUE(std::filesystem::is_empty(root + "/outside"));
    writer->stop(); writer->join(); writer.reset();
    std::filesystem::remove(root + "/tasks");
    std::filesystem::create_directories(root + "/tasks/" + binding.task);
    std::filesystem::create_directory(root + "/" + binding.stdout_path);
    start(); result = run(command); EXPECT_TRUE(result.error); EXPECT_FALSE(result.barrier_ok);
}
TEST_F(LogWriter, MkdirOpenWriteAndCloseFaultsAreTypedAndDoNotSucceed) {
    for (const auto operation_kind : {Operation::Mkdir, Operation::DirectoryOpen, Operation::FileOpen, Operation::Write, Operation::Close}) {
        hook = std::make_shared<Hook>();
        if (operation_kind == Operation::DirectoryOpen) hook->fault = {{operation_kind, "tasks"}};
        else hook->fault = {{operation_kind, ""}};
        start(); Command append; append.data = "abcd"; const auto result = run(append);
        ASSERT_TRUE(result.error); EXPECT_EQ(result.error->operation, operation_kind); EXPECT_FALSE(result.barrier_ok);
        writer->stop(); writer->join(); writer.reset();
        binding.task = domain::TaskId::generate().value(); binding.attempt = domain::AttemptId::generate().value();
        const auto stem = "tasks/" + binding.task + "/attempt-1";
        binding.stdout_path = stem + ".stdout.log"; binding.stderr_path = stem + ".stderr.log";
    }
}
TEST_F(LogWriter, PartialWriteFailureNeverPublishesAnIncompleteChunkOrOverwritesItsPrefix) {
    hook->maximum = 2; hook->writes_before_failure = 1; start();
    Command append; append.data = "abcdef"; const auto failed = run(append); ASSERT_TRUE(failed.error);
    EXPECT_EQ(failed.visible.out.bytes, 0U);
    std::ifstream input(root + "/" + binding.stdout_path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "ab");
    EXPECT_TRUE(run(append).error); // failed Attempt is poisoned; no retry of append
    Command barrier; barrier.kind = Kind::Barrier; barrier.final.out = {6, 1};
    EXPECT_FALSE(run(barrier).barrier_ok);
}
TEST_F(LogWriter, WriteSlotsAndBytesRemainOwnedUntilDrainAndBarrierReserveStaysAvailable) {
    hook->block_write = true; Limits limits; limits.bytes = 3; limits.write_slots = 1; limits.essential_slots = 1;
    start(limits);
    Command append; append.operation = 1; append.binding = binding; append.data = "abc";
    ASSERT_EQ(writer->try_post(append), Admission::Accepted);
    { std::unique_lock<std::mutex> lock(hook->mutex);
      ASSERT_TRUE(hook->cv.wait_for(lock, std::chrono::seconds(3), [&] { return hook->entered; })); }
    append.operation = 2; EXPECT_EQ(writer->try_post(append), Admission::Full);
    Command barrier; barrier.operation = 3; barrier.kind = Kind::Barrier; barrier.binding = binding; barrier.final.out = {3, 1};
    EXPECT_EQ(writer->try_post(barrier), Admission::Accepted);
    barrier.operation = 4; EXPECT_EQ(writer->try_post(barrier), Admission::Full);
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->proceed = true; hook->cv.notify_all(); }
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        std::size_t notifications = 0;
        { std::lock_guard<std::mutex> lock(hook->mutex);
          for (const auto& op : hook->trace) if (op.first == Operation::Notify) ++notifications; }
        if (notifications >= 2 || std::chrono::steady_clock::now() >= end) break;
        std::this_thread::yield();
    }
    EXPECT_EQ(writer->usage().writes, 1U); EXPECT_EQ(writer->usage().bytes, 3U);
    EXPECT_EQ(writer->try_post(append), Admission::Full);
    const auto completed = writer->drain(); ASSERT_TRUE(completed); ASSERT_EQ(completed.value().size(), 2U);
    EXPECT_EQ(completed.value()[0].kind, Kind::Append); EXPECT_TRUE(completed.value()[1].barrier_ok);
    EXPECT_EQ(writer->usage().writes, 0U); EXPECT_EQ(writer->usage().essential, 0U);
}
TEST_F(LogWriter, FilesystemHooksRunOnOneDedicatedThreadAndShutdownHasNoFdGrowth) {
    const auto count_fds = [] { return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{})); };
    const auto baseline = count_fds();
    for (int iteration = 0; iteration < 10; ++iteration) {
        start(); Command barrier; barrier.kind = Kind::Barrier; ASSERT_TRUE(run(barrier).barrier_ok);
        writer->stop(); writer->join(); writer.reset(); EXPECT_EQ(count_fds(), baseline);
    }
    std::lock_guard<std::mutex> lock(hook->mutex); ASSERT_FALSE(hook->owners.empty());
    for (const auto owner : hook->owners) EXPECT_NE(owner, std::this_thread::get_id());
}
TEST_F(LogWriter, UnsafeRootAndSymlinkDirectoryAreExplicitFailures) {
    ::chmod(root.c_str(), 0777);
    auto made = Writer::start(root); ASSERT_TRUE(made); writer = std::move(made).value();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writer->status().health == Health::Starting && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    EXPECT_EQ(writer->status().health, Health::Failed);
    writer->stop(); writer->join(); writer.reset();
    ASSERT_EQ(::chmod(root.c_str(), 0700), 0);
    std::filesystem::create_directory_symlink("/tmp", root + "/tasks");
    start(); Command barrier; barrier.kind = Kind::Barrier;
    const auto failed = run(barrier); ASSERT_TRUE(failed.error); EXPECT_FALSE(failed.barrier_ok);
    EXPECT_FALSE(std::filesystem::exists("/tmp/" + binding.task));
}
TEST_F(LogWriter, SafeSyncAndReadEintrRetryWithoutDuplicatingData) {
    start(); Command append; append.data = "abcdef"; ASSERT_FALSE(run(append).error);
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->interrupt_operation = Operation::FileSync; }
    Command barrier; barrier.kind = Kind::Barrier; barrier.final.out = {6, 1};
    ASSERT_TRUE(run(barrier).barrier_ok);
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->interrupt_operation = Operation::Read; hook->maximum = 2; }
    Command read; read.kind = Kind::Read;
    const auto result = run(read); ASSERT_FALSE(result.error); EXPECT_EQ(result.data, "abcdef");
}
TEST_F(LogWriter, NotificationFailureRetainsTheAlreadyPublishedCompletionAndStopsAdmission) {
    start();
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->fault = {{Operation::Notify, "eventfd"}}; }
    Command append; append.data = "x"; append.binding = binding; append.operation = operation++;
    ASSERT_EQ(writer->try_post(append), Admission::Accepted);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writer->status().health != Health::Failed && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    ASSERT_EQ(writer->status().health, Health::Failed);
    EXPECT_EQ(writer->usage().writes, 1U);
    ++append.operation; EXPECT_EQ(writer->try_post(append), Admission::Failed);
    const auto results = writer->drain(); ASSERT_TRUE(results); ASSERT_EQ(results.value().size(), 1U);
    EXPECT_FALSE(results.value()[0].error); EXPECT_EQ(results.value()[0].visible.out.bytes, 1U);
    EXPECT_EQ(writer->usage().writes, 0U);
}
TEST_F(LogWriter, EssentialBarrierRunsBeforeSnapshotsAfterItsPriorWriteAndStopDrainsOwners) {
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->block_write = true; }
    start();
    Command append; append.data = "x"; append.binding = binding; append.operation = operation++;
    ASSERT_EQ(writer->try_post(append), Admission::Accepted);
    { std::unique_lock<std::mutex> lock(hook->mutex); ASSERT_TRUE(hook->cv.wait_for(lock, std::chrono::seconds(2), [&] { return hook->entered; })); }
    Command read; read.kind = Kind::Read; read.binding = binding; read.operation = operation++;
    ASSERT_EQ(writer->try_post(read), Admission::Accepted);
    Command barrier; barrier.kind = Kind::Barrier; barrier.binding = binding; barrier.operation = operation++; barrier.final.out = {1, 1};
    ASSERT_EQ(writer->try_post(barrier), Admission::Accepted);
    writer->stop();
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->proceed = true; hook->cv.notify_all(); }
    writer->join();
    const auto result = writer->drain(); ASSERT_TRUE(result); ASSERT_EQ(result.value().size(), 3U);
    EXPECT_EQ(result.value()[0].kind, Kind::Append);
    EXPECT_EQ(result.value()[1].kind, Kind::Barrier); EXPECT_TRUE(result.value()[1].barrier_ok);
    EXPECT_EQ(result.value()[2].kind, Kind::Read); EXPECT_EQ(result.value()[2].data, "x");
    EXPECT_EQ(writer->usage().writes + writer->usage().essential + writer->usage().reads, 0U);
}
TEST_F(LogWriter, RetirementCannotEraseVisiblePositionsBeforeAnAcceptedSnapshot) {
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->block_write = true; }
    start();
    Command append; append.data = "x"; append.binding = binding; append.operation = operation++;
    ASSERT_EQ(writer->try_post(append), Admission::Accepted);
    { std::unique_lock<std::mutex> lock(hook->mutex); ASSERT_TRUE(hook->cv.wait_for(lock, std::chrono::seconds(2), [&] { return hook->entered; })); }
    Command read; read.kind = Kind::Read; read.binding = binding; read.operation = operation++;
    ASSERT_EQ(writer->try_post(read), Admission::Accepted);
    Command forget; forget.kind = Kind::Forget; forget.binding = binding; forget.operation = operation++;
    ASSERT_EQ(writer->try_post(forget), Admission::Accepted);
    writer->stop();
    { std::lock_guard<std::mutex> lock(hook->mutex); hook->proceed = true; hook->cv.notify_all(); }
    writer->join();
    const auto result = writer->drain(); ASSERT_TRUE(result); ASSERT_EQ(result.value().size(), 3U);
    EXPECT_EQ(result.value()[1].kind, Kind::Read); EXPECT_EQ(result.value()[1].data, "x");
    EXPECT_EQ(result.value()[1].snapshot_end, 1U); EXPECT_EQ(result.value()[2].kind, Kind::Forget);
}
} // namespace
