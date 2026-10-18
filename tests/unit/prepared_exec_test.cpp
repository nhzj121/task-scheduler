#include "ts/runtime/prepared_exec.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace ts::runtime {
namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/task-scheduler-prepared-XXXXXX";
        path_ = ::mkdtemp(pattern.data());
        if (path_.empty()) {
            throw std::runtime_error("mkdtemp failed");
        }
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

private:
    std::string path_;
};

sigset_t empty_mask() {
    sigset_t mask{};
    EXPECT_EQ(::sigemptyset(&mask), 0);
    return mask;
}

void make_executable(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "fixture";
    file.close();
    ASSERT_EQ(::chmod(path.c_str(), 0700), 0);
}

TEST(PreparedExec, EnvironmentOverrideOrderArgvAndPointersRemainStableAfterMove) {
    TemporaryDirectory directory;
    ExecutionSpec specification{"/bin/true", {"alpha", "beta"},
                                {{"B", "override"}, {"C", "three"}}, std::nullopt};
    auto prepared = PreparedExec::prepare(
        specification, {"B=first", "A=one", "B=last"}, directory.path(), empty_mask());
    ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared.value().argv_storage(),
              (std::vector<std::string>{"/bin/true", "alpha", "beta"}));
    EXPECT_EQ(prepared.value().environment_storage(),
              (std::vector<std::string>{"A=one", "B=override", "C=three"}));

    PreparedExec moved = std::move(prepared).value();
    ASSERT_NE(moved.argv(), nullptr);
    ASSERT_NE(moved.envp(), nullptr);
    EXPECT_STREQ(moved.argv()[0], "/bin/true");
    EXPECT_STREQ(moved.argv()[2], "beta");
    EXPECT_EQ(moved.argv()[3], nullptr);
    EXPECT_STREQ(moved.envp()[1], "B=override");
    EXPECT_EQ(moved.envp()[3], nullptr);
}

TEST(PreparedExec, ResolvesNullRelativeAndAbsoluteCwdAndOpensDevNull) {
    TemporaryDirectory startup;
    const auto child = std::filesystem::path(startup.path()) / "child";
    ASSERT_TRUE(std::filesystem::create_directory(child));
    for (const auto& requested :
         std::vector<std::optional<std::string>>{std::nullopt, std::string("child"),
                                                 child.string()}) {
        ExecutionSpec specification{"/bin/true", {}, {}, requested};
        auto prepared = PreparedExec::prepare(specification, {}, startup.path(), empty_mask());
        ASSERT_TRUE(prepared);
        const std::string expected = requested.has_value() ? child.string() : startup.path();
        EXPECT_EQ(prepared.value().cwd(), expected);
        EXPECT_NE(::fcntl(prepared.value().cwd_fd(), F_GETFD), -1);
        EXPECT_NE(::fcntl(prepared.value().stdin_fd(), F_GETFD), -1);
        EXPECT_NE(::fcntl(prepared.value().stdin_fd(), F_GETFD) & FD_CLOEXEC, 0);
    }
}

TEST(PreparedExec, UsesMergedPathWithEmptyRelativeAndAbsoluteEntries) {
    TemporaryDirectory startup;
    const auto relative_bin = std::filesystem::path(startup.path()) / "relative-bin";
    const auto absolute_bin = std::filesystem::path(startup.path()) / "absolute-bin";
    ASSERT_TRUE(std::filesystem::create_directory(relative_bin));
    ASSERT_TRUE(std::filesystem::create_directory(absolute_bin));
    make_executable(std::filesystem::path(startup.path()) / "from-empty");
    make_executable(relative_bin / "from-relative");
    make_executable(absolute_bin / "from-absolute");

    const std::vector<std::pair<std::string, std::string>> cases{
        {"from-empty", ":missing"},
        {"from-relative", "relative-bin"},
        {"from-absolute", absolute_bin.string()},
    };
    for (const auto& item : cases) {
        ExecutionSpec specification{item.first, {}, {{"PATH", item.second}}, std::nullopt};
        auto prepared = PreparedExec::prepare(
            specification, {"PATH=/definitely/not-used"}, startup.path(), empty_mask());
        ASSERT_TRUE(prepared);
        EXPECT_TRUE(std::filesystem::path(prepared.value().executable_path()).is_absolute());
        EXPECT_EQ(std::filesystem::path(prepared.value().executable_path()).filename(),
                  item.first);
    }
}

TEST(PreparedExec, MissingPathAndMissingCandidateFailBeforeFork) {
    TemporaryDirectory startup;
    ExecutionSpec no_path{"missing", {}, {}, std::nullopt};
    EXPECT_FALSE(PreparedExec::prepare(no_path, {}, startup.path(), empty_mask()));
    ExecutionSpec no_candidate{"missing", {}, {{"PATH", startup.path()}}, std::nullopt};
    EXPECT_FALSE(PreparedExec::prepare(no_candidate, {}, startup.path(), empty_mask()));
}

TEST(PreparedExec, SlashExecutableBypassesPathAndPreservesRelativeToken) {
    TemporaryDirectory startup;
    ExecutionSpec specification{"./not-checked-until-exec", {}, {}, std::nullopt};
    auto prepared = PreparedExec::prepare(specification, {}, startup.path(), empty_mask());
    ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared.value().executable_path(), "./not-checked-until-exec");
    EXPECT_EQ(prepared.value().argv_storage().front(), "./not-checked-until-exec");
}

TEST(PreparedExec, RejectsInvalidEnvironmentArgumentsAndCwd) {
    TemporaryDirectory startup;
    ExecutionSpec bad_key{"/bin/true", {}, {{"", "value"}}, std::nullopt};
    EXPECT_FALSE(PreparedExec::prepare(bad_key, {}, startup.path(), empty_mask()));
    bad_key.environment_overrides = {{"A=B", "value"}};
    EXPECT_FALSE(PreparedExec::prepare(bad_key, {}, startup.path(), empty_mask()));
    ExecutionSpec bad_value{"/bin/true", {},
                            {{"A", std::string("x\0y", 3U)}}, std::nullopt};
    EXPECT_FALSE(PreparedExec::prepare(bad_value, {}, startup.path(), empty_mask()));
    ExecutionSpec bad_argument{"/bin/true", {std::string("x\0y", 3U)}, {}, std::nullopt};
    EXPECT_FALSE(PreparedExec::prepare(bad_argument, {}, startup.path(), empty_mask()));
    ExecutionSpec bad_cwd{"/bin/true", {}, {}, "missing"};
    EXPECT_FALSE(PreparedExec::prepare(bad_cwd, {}, startup.path(), empty_mask()));
}

}  // namespace
}  // namespace ts::runtime
