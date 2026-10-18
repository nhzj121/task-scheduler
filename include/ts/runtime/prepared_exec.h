#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <signal.h>

#include "ts/common/result.h"
#include "ts/common/unique_fd.h"

namespace ts::runtime {

struct ExecutionSpec {
    std::string executable;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> environment_overrides;
    std::optional<std::string> cwd;
};

class PreparedExec final {
public:
    static common::Result<PreparedExec> prepare(
        const ExecutionSpec& specification,
        const std::vector<std::string>& inherited_environment,
        const std::string& startup_cwd, const sigset_t& child_signal_mask);

    static common::Result<std::vector<std::string>> snapshot_environment();
    static common::Result<std::string> capture_startup_cwd();

    PreparedExec(const PreparedExec&) = delete;
    PreparedExec& operator=(const PreparedExec&) = delete;
    PreparedExec(PreparedExec&&) noexcept = default;
    PreparedExec& operator=(PreparedExec&&) noexcept = default;

    [[nodiscard]] const std::string& executable_token() const noexcept;
    [[nodiscard]] const std::string& executable_path() const noexcept;
    [[nodiscard]] const std::string& cwd() const noexcept;
    [[nodiscard]] const std::vector<std::string>& argv_storage() const noexcept;
    [[nodiscard]] const std::vector<std::string>& environment_storage() const noexcept;
    [[nodiscard]] char* const* argv() noexcept;
    [[nodiscard]] char* const* envp() noexcept;
    [[nodiscard]] int cwd_fd() const noexcept;
    [[nodiscard]] int stdin_fd() const noexcept;
    [[nodiscard]] const sigset_t& child_signal_mask() const noexcept;

private:
    PreparedExec(std::string executable_token, std::string executable_path,
                 std::string cwd, std::vector<std::string> argv_storage,
                 std::vector<std::string> environment_storage,
                 std::vector<char*> argv, std::vector<char*> envp,
                 common::UniqueFd cwd_fd, common::UniqueFd stdin_fd,
                 sigset_t child_signal_mask) noexcept;

    std::string executable_token_;
    std::string executable_path_;
    std::string cwd_;
    std::vector<std::string> argv_storage_;
    std::vector<std::string> environment_storage_;
    std::vector<char*> argv_;
    std::vector<char*> envp_;
    common::UniqueFd cwd_fd_;
    common::UniqueFd stdin_fd_;
    sigset_t child_signal_mask_{};
};

}  // namespace ts::runtime
