#include "ts/runtime/prepared_exec.h"

#include <cerrno>
#include <filesystem>
#include <map>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

namespace ts::runtime {
namespace {

std::string errno_message(const char* operation, int error_number) {
    return std::string(operation) + " failed: " +
           std::error_code(error_number, std::generic_category()).message();
}

bool contains_nul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

common::Result<void> validate_environment_key(const std::string& key) {
    if (key.empty() || key.find('=') != std::string::npos || contains_nul(key)) {
        return common::Result<void>::failure(
            "environment key must be nonempty and contain neither '=' nor NUL");
    }
    return common::Result<void>::success();
}

common::Result<void> insert_environment_entry(
    std::map<std::string, std::string>& environment, const std::string& entry) {
    const std::size_t separator = entry.find('=');
    if (separator == std::string::npos) {
        return common::Result<void>::failure(
            "inherited environment entry does not contain '='");
    }
    const std::string key = entry.substr(0U, separator);
    const std::string value = entry.substr(separator + 1U);
    if (const auto valid = validate_environment_key(key); !valid) {
        return valid;
    }
    if (contains_nul(value)) {
        return common::Result<void>::failure(
            "environment value must not contain NUL");
    }
    environment[key] = value;
    return common::Result<void>::success();
}

common::Result<std::string> resolve_executable(
    const std::string& executable, const std::map<std::string, std::string>& environment,
    const std::string& prepared_cwd) {
    if (executable.find('/') != std::string::npos) {
        return common::Result<std::string>::success(executable);
    }

    const auto path_iterator = environment.find("PATH");
    if (path_iterator == environment.end()) {
        return common::Result<std::string>::failure(
            "PATH is required when executable contains no slash");
    }

    const std::string& path_value = path_iterator->second;
    std::size_t beginning = 0U;
    for (;;) {
        const std::size_t end = path_value.find(':', beginning);
        const std::string entry =
            end == std::string::npos
                ? path_value.substr(beginning)
                : path_value.substr(beginning, end - beginning);
        std::filesystem::path base = entry.empty()
                                         ? std::filesystem::path(prepared_cwd)
                                         : std::filesystem::path(entry);
        if (base.is_relative()) {
            base = std::filesystem::path(prepared_cwd) / base;
        }
        const std::string candidate = (base / executable).lexically_normal().string();

        struct stat status {};
        if (::stat(candidate.c_str(), &status) == 0 && !S_ISDIR(status.st_mode) &&
            ::faccessat(AT_FDCWD, candidate.c_str(), X_OK, AT_EACCESS) == 0) {
            return common::Result<std::string>::success(candidate);
        }

        if (end == std::string::npos) {
            break;
        }
        beginning = end + 1U;
    }
    return common::Result<std::string>::failure(
        "no executable candidate was found in the merged PATH");
}

}  // namespace

common::Result<PreparedExec> PreparedExec::prepare(
    const ExecutionSpec& specification,
    const std::vector<std::string>& inherited_environment,
    const std::string& startup_cwd, const sigset_t& child_signal_mask) {
    if (specification.executable.empty() || contains_nul(specification.executable)) {
        return common::Result<PreparedExec>::failure(
            "executable must be nonempty and contain no NUL");
    }
    if (!std::filesystem::path(startup_cwd).is_absolute() || contains_nul(startup_cwd)) {
        return common::Result<PreparedExec>::failure(
            "startup cwd must be an absolute path without NUL");
    }

    std::map<std::string, std::string> environment;
    for (const auto& entry : inherited_environment) {
        if (const auto inserted = insert_environment_entry(environment, entry); !inserted) {
            return common::Result<PreparedExec>::failure(inserted.error().message);
        }
    }
    for (const auto& override_entry : specification.environment_overrides) {
        if (const auto valid = validate_environment_key(override_entry.first); !valid) {
            return common::Result<PreparedExec>::failure(valid.error().message);
        }
        if (contains_nul(override_entry.second)) {
            return common::Result<PreparedExec>::failure(
                "environment value must not contain NUL");
        }
        environment[override_entry.first] = override_entry.second;
    }

    std::filesystem::path cwd_path(startup_cwd);
    if (specification.cwd.has_value()) {
        if (contains_nul(*specification.cwd)) {
            return common::Result<PreparedExec>::failure("cwd must not contain NUL");
        }
        const std::filesystem::path requested(*specification.cwd);
        cwd_path = requested.is_absolute() ? requested
                                           : std::filesystem::path(startup_cwd) / requested;
    }
    cwd_path = cwd_path.lexically_normal();
    const std::string prepared_cwd = cwd_path.string();

    common::UniqueFd cwd_fd(::open(prepared_cwd.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!cwd_fd) {
        return common::Result<PreparedExec>::failure(errno_message("open cwd", errno));
    }
    common::UniqueFd stdin_fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (!stdin_fd) {
        return common::Result<PreparedExec>::failure(errno_message("open /dev/null", errno));
    }

    auto executable_path =
        resolve_executable(specification.executable, environment, prepared_cwd);
    if (!executable_path) {
        return common::Result<PreparedExec>::failure(executable_path.error().message);
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(specification.arguments.size() + 1U);
    argv_storage.push_back(specification.executable);
    for (const auto& argument : specification.arguments) {
        if (contains_nul(argument)) {
            return common::Result<PreparedExec>::failure(
                "argv values must not contain NUL");
        }
        argv_storage.push_back(argument);
    }

    std::vector<std::string> environment_storage;
    environment_storage.reserve(environment.size());
    for (const auto& entry : environment) {
        environment_storage.push_back(entry.first + "=" + entry.second);
    }

    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1U);
    for (auto& argument : argv_storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    std::vector<char*> envp;
    envp.reserve(environment_storage.size() + 1U);
    for (auto& entry : environment_storage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    return common::Result<PreparedExec>::success(PreparedExec(
        specification.executable, std::move(executable_path).value(), prepared_cwd,
        std::move(argv_storage), std::move(environment_storage), std::move(argv),
        std::move(envp), std::move(cwd_fd), std::move(stdin_fd), child_signal_mask));
}

common::Result<std::vector<std::string>> PreparedExec::snapshot_environment() {
    std::vector<std::string> snapshot;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        snapshot.emplace_back(*entry);
    }
    return common::Result<std::vector<std::string>>::success(std::move(snapshot));
}

common::Result<std::string> PreparedExec::capture_startup_cwd() {
    std::error_code error;
    const std::filesystem::path cwd = std::filesystem::current_path(error);
    if (error) {
        return common::Result<std::string>::failure(
            "current_path failed: " + error.message());
    }
    if (!cwd.is_absolute()) {
        return common::Result<std::string>::failure(
            "captured startup cwd is not absolute");
    }
    return common::Result<std::string>::success(cwd.lexically_normal().string());
}

PreparedExec::PreparedExec(
    std::string executable_token, std::string executable_path, std::string cwd,
    std::vector<std::string> argv_storage,
    std::vector<std::string> environment_storage, std::vector<char*> argv,
    std::vector<char*> envp, common::UniqueFd cwd_fd, common::UniqueFd stdin_fd,
    sigset_t child_signal_mask) noexcept
    : executable_token_(std::move(executable_token)),
      executable_path_(std::move(executable_path)),
      cwd_(std::move(cwd)),
      argv_storage_(std::move(argv_storage)),
      environment_storage_(std::move(environment_storage)),
      argv_(std::move(argv)),
      envp_(std::move(envp)),
      cwd_fd_(std::move(cwd_fd)),
      stdin_fd_(std::move(stdin_fd)),
      child_signal_mask_(child_signal_mask) {}

const std::string& PreparedExec::executable_token() const noexcept {
    return executable_token_;
}

const std::string& PreparedExec::executable_path() const noexcept {
    return executable_path_;
}

const std::string& PreparedExec::cwd() const noexcept {
    return cwd_;
}

const std::vector<std::string>& PreparedExec::argv_storage() const noexcept {
    return argv_storage_;
}

const std::vector<std::string>& PreparedExec::environment_storage() const noexcept {
    return environment_storage_;
}

char* const* PreparedExec::argv() noexcept {
    return argv_.data();
}

char* const* PreparedExec::envp() noexcept {
    return envp_.data();
}

int PreparedExec::cwd_fd() const noexcept {
    return cwd_fd_.get();
}

int PreparedExec::stdin_fd() const noexcept {
    return stdin_fd_.get();
}

const sigset_t& PreparedExec::child_signal_mask() const noexcept {
    return child_signal_mask_;
}

}  // namespace ts::runtime
