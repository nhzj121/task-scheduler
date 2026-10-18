#include "ts/runtime/process_runtime.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ts/common/unique_fd.h"

namespace {

using namespace std::chrono_literals;
using ts::common::UniqueFd;
using ts::runtime::ProcessRuntime;

std::size_t fd_count() {
    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        static_cast<void>(entry);
        ++count;
    }
    return count;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// This is an independent executable: closing stdio never affects GoogleTest's fds.
void exercise(int closed_mask, bool close_before_runtime) {
    std::vector<UniqueFd> saved;
    for (int fd = 0; fd <= 2; ++fd) {
        saved.emplace_back(::fcntl(fd, F_DUPFD_CLOEXEC, 3));
        require(static_cast<bool>(saved.back()), "save stdio");
    }
    const std::size_t baseline = fd_count();
    const auto close_selected = [closed_mask] {
        for (int fd = 0; fd <= 2; ++fd) {
            if ((closed_mask & (1 << fd)) != 0) {
                require(::close(fd) == 0, "close selected stdio");
            }
        }
    };
    if (close_before_runtime) {
        close_selected();
    }

    std::string stdout_bytes;
    std::vector<std::uint8_t> stderr_bytes;
    auto created = ProcessRuntime::create(
        [&](ts::runtime::RuntimeProcessId, ts::runtime::OutputStream stream,
            const std::uint8_t* data, std::size_t size) {
            if (stream == ts::runtime::OutputStream::STDOUT) {
                stdout_bytes.append(reinterpret_cast<const char*>(data), size);
            } else {
                stderr_bytes.insert(stderr_bytes.end(), data, data + size);
            }
            return ts::common::Result<void>::success();
        });
    require(static_cast<bool>(created), "create runtime");
    auto runtime = std::move(created).value();
    if (!close_before_runtime) {
        close_selected();
    }
    const std::size_t running_baseline = fd_count();
    const std::string cwd = std::filesystem::path(TS_RUNTIME_HELPER_PATH).parent_path().string();
    for (int iteration = 0; iteration < 8; ++iteration) {
        stdout_bytes.clear();
        stderr_bytes.clear();
        ts::runtime::LaunchOptions options;
        options.execution.executable = TS_RUNTIME_HELPER_PATH;
        options.execution.arguments = {"stdio-check"};
        options.execution.cwd = cwd;
        options.termination_grace = 20ms;
        auto launched = runtime->launch(options);
        require(static_cast<bool>(launched), "launch helper");
        const auto wait_completion = [&] {
            std::optional<ts::runtime::ProcessCompletion> result;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!result.has_value() && std::chrono::steady_clock::now() < deadline) {
                require(static_cast<bool>(runtime->poll_once(20)), "poll helper");
                for (auto& completion : runtime->take_completions()) {
                    result = std::move(completion);
                }
                static_cast<void>(runtime->take_started());
            }
            require(result.has_value(), "completion deadline");
            return *result;
        };
        const auto success = wait_completion();
        require(success.exec_succeeded && !success.launch_failure.has_value(),
                "successful exec-status");
        require(success.leader_outcome.has_value() && success.leader_outcome->exit_code == 0,
                "helper exit");
        require(stdout_bytes == "stdin-eof;cwd=" + cwd + "\n", "stdin/stdout/cwd");
        require(stderr_bytes == std::vector<std::uint8_t>{'e', 0U, 0xfeU}, "stderr binary");
        require(success.stdout_complete && success.stderr_complete, "stream EOF");

        options.execution.executable = "/definitely/missing/closed-stdio-helper";
        require(static_cast<bool>(runtime->launch(options)), "launch missing executable");
        const auto failure = wait_completion();
        require(!failure.exec_succeeded && failure.launch_failure.has_value(),
                "failure exec-status");
        require(failure.launch_failure->stage == ts::runtime::ChildFailureStage::EXECVE &&
                    failure.launch_failure->error_number == ENOENT,
                "exact exec failure");
        for (const auto* completed : {&success, &failure}) {
            require(::kill(-completed->pgid, 0) == -1 && errno == ESRCH, "group absent");
        }
        int status = 0;
        require(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "no zombies");
        require(runtime->registration_count() == 1U, "registration baseline");
        require(fd_count() == running_baseline, "launch fd baseline");
    }
    runtime.reset();
    for (int fd = 0; fd <= 2; ++fd) {
        require(::dup2(saved[static_cast<std::size_t>(fd)].get(), fd) == fd, "restore stdio");
    }
    require(fd_count() == baseline, "runtime fd baseline");
}

}  // namespace

int main() {
    // Save a diagnostic fd that stays usable even when stderr is deliberately closed.
    UniqueFd diagnostic(::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3));
    if (!diagnostic) {
        return 1;
    }
    try {
        for (int mask = 1; mask <= 7; ++mask) {
            exercise(mask, true);
            exercise(mask, false);
        }
    } catch (const std::exception& error) {
        static_cast<void>(::dprintf(diagnostic.get(), "closed-stdio fixture: %s\n", error.what()));
        return 1;
    }
    return 0;
}
