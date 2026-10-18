#include "ts/runtime/process_runtime.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ts/common/unique_fd.h"
#include "ts/io/event_loop.h"
#include "ts/io/signal_fd.h"
#include "ts/io/timer_fd.h"

namespace ts::runtime {
namespace {

constexpr std::uint32_t kChildFailureMagic = 0x54534346U;
constexpr std::uint16_t kChildFailureVersion = 1U;
constexpr std::size_t kReadBufferSize = 16U * 1024U;
constexpr std::size_t kOutputReadAttemptsPerDispatch = 4U;
constexpr std::chrono::nanoseconds kGroupProbeInterval =
    std::chrono::milliseconds(10);

struct ChildFailureRecord {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t stage;
    std::int32_t error_number;
};

static_assert(sizeof(ChildFailureRecord) == 12U,
              "child failure record must have a fixed representation");

struct ChildLaunchData {
    int stdout_read_fd;
    int stdout_write_fd;
    int stderr_read_fd;
    int stderr_write_fd;
    int status_read_fd;
    int status_write_fd;
    int cwd_fd;
    int stdin_fd;
    const char* executable_path;
    char* const* argv;
    char* const* envp;
    const sigset_t* child_signal_mask;
};

struct PipePair {
    common::UniqueFd read_end;
    common::UniqueFd write_end;
};

std::string errno_message(const char* operation, int error_number) {
    return std::string(operation) + " failed: " +
           std::error_code(error_number, std::generic_category()).message();
}

common::Result<common::UniqueFd> duplicate_above_stdio(int fd) {
    int duplicate = -1;
    do {
        duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    } while (duplicate == -1 && errno == EINTR);
    if (duplicate == -1) {
        return common::Result<common::UniqueFd>::failure(
            errno_message("fcntl(F_DUPFD_CLOEXEC)", errno));
    }
    return common::Result<common::UniqueFd>::success(common::UniqueFd(duplicate));
}

common::Result<PipePair> create_pipe() {
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_CLOEXEC) == -1) {
        return common::Result<PipePair>::failure(errno_message("pipe2", errno));
    }
    PipePair pipe{common::UniqueFd(fds[0]), common::UniqueFd(fds[1])};
    for (common::UniqueFd* end : {&pipe.read_end, &pipe.write_end}) {
        if (end->get() <= STDERR_FILENO) {
            auto elevated = duplicate_above_stdio(end->get());
            if (!elevated) {
                return common::Result<PipePair>::failure(elevated.error().message);
            }
            *end = std::move(elevated).value();
        }
    }
    return common::Result<PipePair>::success(std::move(pipe));
}

common::Result<void> make_nonblocking(int fd) {
    int flags = -1;
    do {
        flags = ::fcntl(fd, F_GETFL);
    } while (flags == -1 && errno == EINTR);
    if (flags == -1) {
        return common::Result<void>::failure(errno_message("fcntl(F_GETFL)", errno));
    }
    int result = -1;
    do {
        result = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        return common::Result<void>::failure(errno_message("fcntl(F_SETFL)", errno));
    }
    return common::Result<void>::success();
}

int retry_dup2(int old_fd, int new_fd) noexcept {
    int result = -1;
    do {
        result = ::dup2(old_fd, new_fd);
    } while (result == -1 && errno == EINTR);
    return result;
}

[[noreturn]] void child_failure(int status_fd, ChildFailureStage stage,
                                int error_number) noexcept {
    ChildFailureRecord record;
    record.magic = kChildFailureMagic;
    record.version = kChildFailureVersion;
    record.stage = static_cast<std::uint16_t>(stage);
    record.error_number = static_cast<std::int32_t>(error_number);

    const char* bytes = reinterpret_cast<const char*>(&record);
    std::size_t written = 0U;
    while (written < sizeof(record)) {
        const ssize_t result =
            ::write(status_fd, bytes + written, sizeof(record) - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::_exit(127);
}

void child_close(int fd, int status_fd) noexcept {
    if (::close(fd) == -1) {
        child_failure(status_fd, ChildFailureStage::CLOSE_FDS, errno);
    }
}

[[noreturn]] void child_exec(const ChildLaunchData* data) noexcept {
    child_close(data->stdout_read_fd, data->status_write_fd);
    child_close(data->stderr_read_fd, data->status_write_fd);
    child_close(data->status_read_fd, data->status_write_fd);

    int result = -1;
    do {
        result = ::setpgid(0, 0);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        child_failure(data->status_write_fd, ChildFailureStage::SETPGID, errno);
    }
    if (retry_dup2(data->stdin_fd, STDIN_FILENO) == -1) {
        child_failure(data->status_write_fd, ChildFailureStage::STDIN_DUP, errno);
    }
    if (retry_dup2(data->stdout_write_fd, STDOUT_FILENO) == -1) {
        child_failure(data->status_write_fd, ChildFailureStage::STDOUT_DUP, errno);
    }
    if (retry_dup2(data->stderr_write_fd, STDERR_FILENO) == -1) {
        child_failure(data->status_write_fd, ChildFailureStage::STDERR_DUP, errno);
    }

    do {
        result = ::fchdir(data->cwd_fd);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        child_failure(data->status_write_fd, ChildFailureStage::FCHDIR, errno);
    }

    do {
        result = ::sigprocmask(SIG_SETMASK, data->child_signal_mask, nullptr);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        child_failure(data->status_write_fd,
                      ChildFailureStage::SIGNAL_MASK_RESTORE, errno);
    }

    child_close(data->stdin_fd, data->status_write_fd);
    child_close(data->cwd_fd, data->status_write_fd);
    child_close(data->stdout_write_fd, data->status_write_fd);
    child_close(data->stderr_write_fd, data->status_write_fd);

    ::execve(data->executable_path, data->argv, data->envp);
    child_failure(data->status_write_fd, ChildFailureStage::EXECVE, errno);
}

enum class TimerPurpose {
    NONE,
    TIMEOUT,
    GRACE,
    GROUP_PROBE,
};

enum class GroupPresence {
    PRESENT,
    ABSENT,
};

}  // namespace

class ProcessRuntime::Impl final {
public:
    struct RunningProcess {
        RunningProcess(RuntimeProcessId process_id, ProcessHandle process_handle,
                       common::UniqueFd stdout_read, common::UniqueFd stderr_read,
                       common::UniqueFd status_read, io::TimerFd process_timer,
                       std::optional<std::chrono::nanoseconds> process_timeout,
                       std::chrono::nanoseconds process_grace) noexcept
            : id(process_id),
              handle(std::move(process_handle)),
              stdout_fd(std::move(stdout_read)),
              stderr_fd(std::move(stderr_read)),
              status_fd(std::move(status_read)),
              timer(std::move(process_timer)),
              timeout(process_timeout),
              grace(process_grace) {}

        RuntimeProcessId id;
        ProcessHandle handle;
        common::UniqueFd stdout_fd;
        common::UniqueFd stderr_fd;
        common::UniqueFd status_fd;
        io::TimerFd timer;
        std::optional<io::RegistrationToken> stdout_registration;
        std::optional<io::RegistrationToken> stderr_registration;
        std::optional<io::RegistrationToken> status_registration;
        std::optional<io::RegistrationToken> timer_registration;
        std::optional<std::chrono::nanoseconds> timeout;
        std::chrono::nanoseconds grace;
        TimerPurpose timer_purpose{TimerPurpose::NONE};
        std::array<std::uint8_t, sizeof(ChildFailureRecord) + 1U> status_bytes{};
        std::size_t status_size{0U};
        bool status_overflow{false};
        bool exec_resolved{false};
        bool exec_succeeded{false};
        std::optional<LaunchFailure> launch_failure;
        bool stdout_eof{false};
        bool stderr_eof{false};
        bool stdout_enabled{true};
        bool stderr_enabled{true};
        bool completion_scheduled{false};
        bool registration_recovery{false};
    };

    Impl(std::unique_ptr<io::EventLoop> event_loop, io::SignalFd signal_fd,
         sigset_t previous_signal_mask, std::string startup_cwd,
         std::vector<std::string> inherited_environment, OutputSink output_sink) noexcept
        : event_loop_(std::move(event_loop)),
          signal_fd_(std::move(signal_fd)),
          previous_signal_mask_(previous_signal_mask),
          startup_cwd_(std::move(startup_cwd)),
          inherited_environment_(std::move(inherited_environment)),
          output_sink_(std::move(output_sink)) {}

    ~Impl() {
        if (!processes_.empty()) {
            std::terminate();
        }
        if (signal_registration_.has_value()) {
            const auto removed = event_loop_->remove(*signal_registration_);
            if (!removed) {
                std::terminate();
            }
            signal_registration_.reset();
        }
        if (::pthread_sigmask(SIG_SETMASK, &previous_signal_mask_, nullptr) != 0) {
            std::terminate();
        }
    }

    common::Result<void> initialize() {
        auto registration = event_loop_->add(
            signal_fd_.fd(), io::kReadable,
            [this](const io::RegistrationToken&, io::EventMask) { handle_sigchld(); },
            io::RegistrationMetadata{1U, 0U});
        if (!registration) {
            return common::Result<void>::failure(registration.error().message);
        }
        signal_registration_ = registration.value();
        return common::Result<void>::success();
    }

    common::Result<RuntimeProcessId> launch(const LaunchOptions& options) {
        if (options.termination_grace.count() <= 0) {
            return common::Result<RuntimeProcessId>::failure(
                "termination grace must be positive");
        }
        if (options.timeout.has_value() && options.timeout->count() <= 0) {
            return common::Result<RuntimeProcessId>::failure(
                "process timeout must be positive when present");
        }
        auto prepared = PreparedExec::prepare(options.execution, inherited_environment_,
                                              startup_cwd_, previous_signal_mask_);
        if (!prepared) {
            return common::Result<RuntimeProcessId>::failure(prepared.error().message);
        }

        // Even when stdio was closed at startup, every child setup source must be above
        // 0/1/2 before fork. dup2 destinations can then never alias a later setup source.
        auto child_cwd = duplicate_above_stdio(prepared.value().cwd_fd());
        if (!child_cwd) {
            return common::Result<RuntimeProcessId>::failure(child_cwd.error().message);
        }
        auto child_stdin = duplicate_above_stdio(prepared.value().stdin_fd());
        if (!child_stdin) {
            return common::Result<RuntimeProcessId>::failure(child_stdin.error().message);
        }

        auto stdout_pipe = create_pipe();
        if (!stdout_pipe) {
            return common::Result<RuntimeProcessId>::failure(stdout_pipe.error().message);
        }
        auto stderr_pipe = create_pipe();
        if (!stderr_pipe) {
            return common::Result<RuntimeProcessId>::failure(stderr_pipe.error().message);
        }
        auto status_pipe = create_pipe();
        if (!status_pipe) {
            return common::Result<RuntimeProcessId>::failure(status_pipe.error().message);
        }
        auto timer = io::TimerFd::create();
        if (!timer) {
            return common::Result<RuntimeProcessId>::failure(timer.error().message);
        }
        if (next_process_id_ == 0U ||
            next_process_id_ == std::numeric_limits<RuntimeProcessId>::max()) {
            return common::Result<RuntimeProcessId>::failure(
                "runtime process identity space is exhausted");
        }

        const auto stdout_nonblocking =
            make_nonblocking(stdout_pipe.value().read_end.get());
        const auto stderr_nonblocking =
            make_nonblocking(stderr_pipe.value().read_end.get());
        const auto status_nonblocking = make_nonblocking(status_pipe.value().read_end.get());
        if (!stdout_nonblocking || !stderr_nonblocking || !status_nonblocking) {
            return common::Result<RuntimeProcessId>::failure(
                !stdout_nonblocking
                    ? stdout_nonblocking.error().message
                    : !stderr_nonblocking ? stderr_nonblocking.error().message
                                          : status_nonblocking.error().message);
        }

        ChildLaunchData child_data;
        child_data.stdout_read_fd = stdout_pipe.value().read_end.get();
        child_data.stdout_write_fd = stdout_pipe.value().write_end.get();
        child_data.stderr_read_fd = stderr_pipe.value().read_end.get();
        child_data.stderr_write_fd = stderr_pipe.value().write_end.get();
        child_data.status_read_fd = status_pipe.value().read_end.get();
        child_data.status_write_fd = status_pipe.value().write_end.get();
        child_data.cwd_fd = child_cwd.value().get();
        child_data.stdin_fd = child_stdin.value().get();
        child_data.executable_path = prepared.value().executable_path().c_str();
        child_data.argv = prepared.value().argv();
        child_data.envp = prepared.value().envp();
        child_data.child_signal_mask = &prepared.value().child_signal_mask();

        const pid_t pid = ::fork();
        if (pid == -1) {
            return common::Result<RuntimeProcessId>::failure(errno_message("fork", errno));
        }
        if (pid == 0) {
            child_exec(&child_data);
        }

        stdout_pipe.value().write_end.reset();
        stderr_pipe.value().write_end.reset();
        status_pipe.value().write_end.reset();

        bool group_established = false;
        int setpgid_result = -1;
        do {
            setpgid_result = ::setpgid(pid, pid);
        } while (setpgid_result == -1 && errno == EINTR);
        if (setpgid_result == 0) {
            group_established = true;
        } else {
            const int setpgid_error = errno;
            if (setpgid_error == EACCES || setpgid_error == EPERM) {
                const pid_t observed_group = ::getpgid(pid);
                group_established = observed_group == pid;
            }
        }

        const RuntimeProcessId id = next_process_id_++;
        auto running = std::make_unique<RunningProcess>(
            id, ProcessHandle(pid, pid, group_established),
            std::move(stdout_pipe).value().read_end,
            std::move(stderr_pipe).value().read_end,
            std::move(status_pipe).value().read_end, std::move(timer).value(),
            options.timeout, options.termination_grace);
        RunningProcess* running_pointer = running.get();
        processes_.emplace(id, std::move(running));
        pid_to_process_.emplace(pid, id);

        auto stdout_registration = register_process_fd(
            running_pointer->stdout_fd.get(), io::kReadable | io::kReadHangup,
            [this, id](const io::RegistrationToken&, io::EventMask) {
                drain_output(id, OutputStream::STDOUT);
            },
            io::RegistrationMetadata{2U, id});
        auto stderr_registration = register_process_fd(
            running_pointer->stderr_fd.get(), io::kReadable | io::kReadHangup,
            [this, id](const io::RegistrationToken&, io::EventMask) {
                drain_output(id, OutputStream::STDERR);
            },
            io::RegistrationMetadata{3U, id});
        auto status_registration = register_process_fd(
            running_pointer->status_fd.get(), io::kReadable | io::kReadHangup,
            [this, id](const io::RegistrationToken&, io::EventMask) {
                drain_exec_status(id);
            },
            io::RegistrationMetadata{4U, id});
        auto timer_registration = register_process_fd(
            running_pointer->timer.fd(), io::kReadable,
            [this, id](const io::RegistrationToken&, io::EventMask) {
                handle_timer(id);
            },
            io::RegistrationMetadata{5U, id});

        if (stdout_registration) {
            running_pointer->stdout_registration = stdout_registration.value();
        }
        if (stderr_registration) {
            running_pointer->stderr_registration = stderr_registration.value();
        }
        if (status_registration) {
            running_pointer->status_registration = status_registration.value();
        }
        if (timer_registration) {
            running_pointer->timer_registration = timer_registration.value();
        }
        if (!stdout_registration || !stderr_registration || !status_registration ||
            !timer_registration) {
            const std::string error = !stdout_registration
                ? stdout_registration.error().message
                : !stderr_registration ? stderr_registration.error().message
                : !status_registration ? status_registration.error().message
                                       : timer_registration.error().message;
            record_error("process fd registration failed: " + error);
            // Keep every fd and lifecycle fact. Missing registrations are serviced with
            // bounded nonblocking polls until exec, termination, reap and EOF settle.
            running_pointer->registration_recovery = true;
            begin_termination(*running_pointer, TerminationCause::INTERNAL_ERROR);
        }
        return common::Result<RuntimeProcessId>::success(id);
    }

    common::Result<bool> cancel(RuntimeProcessId process_id) {
        const auto iterator = processes_.find(process_id);
        if (iterator == processes_.end()) {
            return common::Result<bool>::success(false);
        }
        RunningProcess& process = *iterator->second;
        if (!process.handle.install_termination_cause(TerminationCause::CANCELLED)) {
            return common::Result<bool>::success(false);
        }
        if (process.exec_resolved && process.exec_succeeded &&
            process.handle.group_established()) {
            send_term(process);
        }
        return common::Result<bool>::success(true);
    }

    common::Result<std::size_t> poll_once(int timeout_ms) {
        if (timeout_ms < -1) {
            return common::Result<std::size_t>::failure("invalid runtime poll timeout");
        }
        if (service_registration_recovery() && (timeout_ms < 0 || timeout_ms > 10)) {
            timeout_ms = 10;
        }
        const auto polled = event_loop_->poll_once(timeout_ms);
        if (!polled) {
            return polled;
        }
        if (fatal_error_.has_value()) {
            std::string error = std::move(*fatal_error_);
            fatal_error_.reset();
            return common::Result<std::size_t>::failure(std::move(error));
        }
        return polled;
    }

    common::Result<void> set_output_enabled(RuntimeProcessId process_id,
                                             OutputStream stream, bool enabled) {
        const auto iterator = processes_.find(process_id);
        if (iterator == processes_.end()) {
            return common::Result<void>::failure("unknown runtime process id");
        }
        RunningProcess& process = *iterator->second;
        auto& registration = stream == OutputStream::STDOUT
                                 ? process.stdout_registration
                                 : process.stderr_registration;
        if (!registration.has_value()) {
            return common::Result<void>::failure("output stream is already closed");
        }
        const auto modified = event_loop_->modify(
            *registration, enabled ? io::kReadable | io::kReadHangup : 0U);
        if (!modified) {
            return modified;
        }
        if (stream == OutputStream::STDOUT) {
            process.stdout_enabled = enabled;
        } else {
            process.stderr_enabled = enabled;
        }
        return common::Result<void>::success();
    }

    std::vector<RuntimeProcessId> take_started() {
        std::vector<RuntimeProcessId> result;
        result.swap(started_);
        return result;
    }

    std::vector<ProcessCompletion> take_completions() {
        std::vector<ProcessCompletion> result;
        result.swap(completions_);
        return result;
    }

    std::size_t running_count() const noexcept {
        return processes_.size();
    }

    std::size_t registration_count() const noexcept {
        return event_loop_->registration_count();
    }

    void fail_next_registrations_for_testing(std::uint32_t mask) {
        registration_failure_mask_ = mask;
    }

    int output_fd_for_testing(RuntimeProcessId id, OutputStream stream) {
        RunningProcess* process = find(id);
        if (process == nullptr) {
            return -1;
        }
        return stream == OutputStream::STDOUT ? process->stdout_fd.get()
                                               : process->stderr_fd.get();
    }

private:
    common::Result<io::RegistrationToken> register_process_fd(
        int fd, io::EventMask interest, io::EventLoop::Callback callback,
        io::RegistrationMetadata metadata) {
        const std::uint32_t bit = std::uint32_t{1U} << metadata.handler_kind;
        if ((registration_failure_mask_ & bit) != 0U) {
            registration_failure_mask_ &= ~bit;
            return common::Result<io::RegistrationToken>::failure(
                "injected registration failure for kind " +
                std::to_string(metadata.handler_kind));
        }
        return event_loop_->add(fd, interest, std::move(callback), metadata);
    }

    bool service_registration_recovery() {
        std::vector<RuntimeProcessId> recovering;
        for (const auto& entry : processes_) {
            if (entry.second->registration_recovery && !entry.second->completion_scheduled) {
                recovering.push_back(entry.first);
            }
        }
        // Sinks may launch another process. Snapshot IDs before calling them so map
        // insertion/rehashing cannot invalidate this recovery pass.
        for (const RuntimeProcessId id : recovering) {
            RunningProcess* current = find(id);
            if (current == nullptr || current->completion_scheduled) {
                continue;
            }
            RunningProcess& process = *current;
            if (!process.status_registration.has_value() && process.status_fd) {
                drain_exec_status(process.id);
            }
            if (!process.stdout_registration.has_value() && process.stdout_fd) {
                drain_output(process.id, OutputStream::STDOUT);
            }
            if (!process.stderr_registration.has_value() && process.stderr_fd) {
                drain_output(process.id, OutputStream::STDERR);
            }
            if (!process.timer_registration.has_value()) {
                handle_timer(process.id);
            }
            maybe_complete(process);
        }
        return !recovering.empty();
    }

    void record_error(std::string message) {
        if (!fatal_error_.has_value()) {
            fatal_error_ = std::move(message);
        }
    }

    RunningProcess* find(RuntimeProcessId id) noexcept {
        const auto iterator = processes_.find(id);
        return iterator == processes_.end() ? nullptr : iterator->second.get();
    }

    void handle_sigchld() {
        const auto drained = signal_fd_.drain();
        if (!drained) {
            record_error(drained.error().message);
            return;
        }
        for (;;) {
            int status = 0;
            const pid_t pid = ::waitpid(-1, &status, WNOHANG);
            if (pid > 0) {
                const auto owner = pid_to_process_.find(pid);
                if (owner == pid_to_process_.end()) {
                    record_error("waitpid returned an unowned child pid");
                    continue;
                }
                RunningProcess* process = find(owner->second);
                pid_to_process_.erase(owner);
                if (process == nullptr || !process->handle.record_reap_status(status)) {
                    record_error("invalid or duplicate direct-child reap result");
                    continue;
                }
                resolve_group_after_reap(*process);
                maybe_complete(*process);
                continue;
            }
            if (pid == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                if (!pid_to_process_.empty()) {
                    record_error("waitpid reported ECHILD while owned children remain");
                }
                break;
            }
            record_error(errno_message("waitpid(WNOHANG)", errno));
            break;
        }
    }

    void drain_output(RuntimeProcessId id, OutputStream stream) {
        RunningProcess* process = find(id);
        if (process == nullptr) {
            return;
        }
        common::UniqueFd& fd =
            stream == OutputStream::STDOUT ? process->stdout_fd : process->stderr_fd;
        std::array<std::uint8_t, kReadBufferSize> buffer{};
        // LT readiness remains pending when the budget is exhausted. Recheck interest
        // after each sink call, including when a stale HUP event dispatches while paused.
        for (std::size_t attempt = 0U; attempt < kOutputReadAttemptsPerDispatch; ++attempt) {
            if (!(stream == OutputStream::STDOUT ? process->stdout_enabled
                                                 : process->stderr_enabled)) {
                return;
            }
            const ssize_t count = ::read(fd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                const auto delivered = output_sink_(
                    id, stream, buffer.data(), static_cast<std::size_t>(count));
                if (!delivered) {
                    record_error("output sink failed: " + delivered.error().message);
                    begin_termination(*process, TerminationCause::INTERNAL_ERROR);
                    return;
                }
                continue;
            }
            if (count == 0) {
                close_output(*process, stream);
                maybe_complete(*process);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            record_error(errno_message("process output read", errno));
            close_output(*process, stream);
            begin_termination(*process, TerminationCause::INTERNAL_ERROR);
            maybe_complete(*process);
            return;
        }
    }

    void drain_exec_status(RuntimeProcessId id) {
        RunningProcess* process = find(id);
        if (process == nullptr) {
            return;
        }
        std::array<std::uint8_t, 32U> buffer{};
        for (;;) {
            const ssize_t count =
                ::read(process->status_fd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                const std::size_t amount = static_cast<std::size_t>(count);
                const std::size_t remaining =
                    process->status_bytes.size() - process->status_size;
                const std::size_t copied = amount < remaining ? amount : remaining;
                std::memcpy(process->status_bytes.data() + process->status_size,
                            buffer.data(), copied);
                process->status_size += copied;
                if (copied != amount) {
                    process->status_overflow = true;
                }
                continue;
            }
            if (count == 0) {
                resolve_exec_status(*process);
                close_status(*process);
                maybe_complete(*process);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            record_error(errno_message("exec-status read", errno));
            process->exec_resolved = true;
            process->launch_failure =
                LaunchFailure{ChildFailureStage::MALFORMED_STATUS, errno};
            close_status(*process);
            begin_termination(*process, TerminationCause::INTERNAL_ERROR);
            maybe_complete(*process);
            return;
        }
    }

    void resolve_exec_status(RunningProcess& process) {
        if (process.exec_resolved) {
            return;
        }
        process.exec_resolved = true;
        if (process.status_size == 0U && !process.status_overflow) {
            process.exec_succeeded = true;
            process.handle.confirm_group_established();
            started_.push_back(process.id);
            if (process.handle.reaped()) {
                resolve_group_after_reap(process);
            }
            if (process.handle.termination_cause() != TerminationCause::NONE) {
                send_term(process);
            } else if (process.timeout.has_value() && !process.handle.reaped() &&
                       process.handle.cleanup_phase() == GroupCleanupPhase::NONE) {
                arm_timer(process, *process.timeout, TimerPurpose::TIMEOUT);
            }
            return;
        }

        if (!process.status_overflow && process.status_size == sizeof(ChildFailureRecord)) {
            ChildFailureRecord record{};
            std::memcpy(&record, process.status_bytes.data(), sizeof(record));
            const bool valid_stage =
                record.stage >= static_cast<std::uint16_t>(ChildFailureStage::SETPGID) &&
                record.stage <= static_cast<std::uint16_t>(ChildFailureStage::CLOSE_FDS);
            if (record.magic == kChildFailureMagic &&
                record.version == kChildFailureVersion && valid_stage &&
                record.error_number > 0) {
                process.launch_failure = LaunchFailure{
                    static_cast<ChildFailureStage>(record.stage),
                    static_cast<int>(record.error_number)};
                if (process.launch_failure->stage != ChildFailureStage::SETPGID) {
                    process.handle.confirm_group_established();
                }
                return;
            }
        }
        process.launch_failure =
            LaunchFailure{ChildFailureStage::MALFORMED_STATUS, EPROTO};
        begin_termination(process, TerminationCause::INTERNAL_ERROR);
    }

    void close_output(RunningProcess& process, OutputStream stream) {
        auto& registration = stream == OutputStream::STDOUT
                                 ? process.stdout_registration
                                 : process.stderr_registration;
        auto& fd = stream == OutputStream::STDOUT ? process.stdout_fd
                                                   : process.stderr_fd;
        if (registration.has_value()) {
            const auto removed = event_loop_->remove(*registration);
            if (!removed) {
                record_error(removed.error().message);
                return;
            }
            registration.reset();
        }
        fd.reset();
        if (stream == OutputStream::STDOUT) {
            process.stdout_eof = true;
        } else {
            process.stderr_eof = true;
        }
    }

    void close_status(RunningProcess& process) {
        if (process.status_registration.has_value()) {
            const auto removed = event_loop_->remove(*process.status_registration);
            if (!removed) {
                record_error(removed.error().message);
                return;
            }
            process.status_registration.reset();
        }
        process.status_fd.reset();
    }

    common::Result<GroupPresence> probe_group(const RunningProcess& process) {
        if (!process.handle.group_established()) {
            return common::Result<GroupPresence>::failure(
                "cannot probe an unconfirmed process group");
        }
        if (::kill(-process.handle.pgid(), 0) == 0) {
            return common::Result<GroupPresence>::success(GroupPresence::PRESENT);
        }
        if (errno == EPERM) {
            return common::Result<GroupPresence>::success(GroupPresence::PRESENT);
        }
        if (errno == ESRCH) {
            return common::Result<GroupPresence>::success(GroupPresence::ABSENT);
        }
        return common::Result<GroupPresence>::failure(
            errno_message("process-group probe", errno));
    }

    void resolve_group_after_reap(RunningProcess& process) {
        if (!process.handle.group_established()) {
            process.handle.set_cleanup_phase(GroupCleanupPhase::ABSENT);
            return;
        }
        const auto presence = probe_group(process);
        if (!presence) {
            record_error(presence.error().message);
            return;
        }
        if (presence.value() == GroupPresence::ABSENT) {
            process.handle.set_cleanup_phase(GroupCleanupPhase::ABSENT);
            disarm_timer(process);
            return;
        }
        if (process.handle.cleanup_phase() == GroupCleanupPhase::ABSENT) {
            // Exec-status may confirm group establishment after the leader was reaped.
            process.handle.set_cleanup_phase(GroupCleanupPhase::NONE);
        }
        if (process.handle.cleanup_phase() == GroupCleanupPhase::NONE) {
            send_term(process);
        }
    }

    void begin_termination(RunningProcess& process, TerminationCause cause) {
        if (process.handle.termination_cause() == TerminationCause::NONE) {
            static_cast<void>(process.handle.install_termination_cause(cause));
        }
        if (process.exec_resolved &&
            (process.exec_succeeded || cause == TerminationCause::INTERNAL_ERROR) &&
            process.handle.group_established() &&
            process.handle.cleanup_phase() == GroupCleanupPhase::NONE) {
            send_term(process);
        }
    }

    void send_term(RunningProcess& process) {
        if (!process.handle.group_established() ||
            process.handle.cleanup_phase() != GroupCleanupPhase::NONE) {
            return;
        }
        if (::kill(-process.handle.pgid(), SIGTERM) == -1) {
            if (errno == ESRCH) {
                process.handle.set_cleanup_phase(GroupCleanupPhase::ABSENT);
                disarm_timer(process);
                maybe_complete(process);
                return;
            }
            record_error(errno_message("kill process group SIGTERM", errno));
            return;
        }
        process.handle.set_cleanup_phase(GroupCleanupPhase::TERM_SENT);
        arm_timer(process, process.grace, TimerPurpose::GRACE);
    }

    void send_kill(RunningProcess& process) {
        const int result = ::kill(-process.handle.pgid(), SIGKILL);
        const int signal_error = result == -1 ? errno : 0;
        if (result == -1 && signal_error != ESRCH) {
            record_error(errno_message("kill process group SIGKILL", signal_error));
            return;
        }
        if (signal_error == ESRCH) {
            process.handle.set_cleanup_phase(GroupCleanupPhase::ABSENT);
            disarm_timer(process);
            maybe_complete(process);
            return;
        }
        process.handle.set_cleanup_phase(GroupCleanupPhase::KILL_SENT);
        arm_timer(process, kGroupProbeInterval, TimerPurpose::GROUP_PROBE);
    }

    void handle_timer(RuntimeProcessId id) {
        RunningProcess* process = find(id);
        if (process == nullptr) {
            return;
        }
        const auto drained = process->timer.drain();
        if (!drained) {
            record_error(drained.error().message);
            return;
        }
        if (drained.value() == 0U) {
            return;
        }
        const TimerPurpose purpose = process->timer_purpose;
        process->timer_purpose = TimerPurpose::NONE;
        if (purpose == TimerPurpose::TIMEOUT) {
            if (process->handle.install_termination_cause(TerminationCause::TIMEOUT)) {
                send_term(*process);
            }
            return;
        }
        if (purpose == TimerPurpose::GRACE) {
            const auto presence = probe_group(*process);
            if (!presence) {
                record_error(presence.error().message);
                return;
            }
            if (presence.value() == GroupPresence::ABSENT) {
                process->handle.set_cleanup_phase(GroupCleanupPhase::ABSENT);
                maybe_complete(*process);
            } else {
                send_kill(*process);
            }
            return;
        }
        if (purpose == TimerPurpose::GROUP_PROBE) {
            const auto presence = probe_group(*process);
            if (!presence) {
                record_error(presence.error().message);
                return;
            }
            if (presence.value() == GroupPresence::ABSENT) {
                process->handle.set_cleanup_phase(GroupCleanupPhase::ABSENT);
                maybe_complete(*process);
            } else {
                arm_timer(*process, kGroupProbeInterval, TimerPurpose::GROUP_PROBE);
            }
        }
    }

    void arm_timer(RunningProcess& process, std::chrono::nanoseconds delay,
                   TimerPurpose purpose) {
        const auto armed = process.timer.arm_once(delay);
        if (!armed) {
            record_error(armed.error().message);
            return;
        }
        process.timer_purpose = purpose;
    }

    void disarm_timer(RunningProcess& process) {
        const auto disarmed = process.timer.disarm();
        if (!disarmed) {
            record_error(disarmed.error().message);
        }
        process.timer_purpose = TimerPurpose::NONE;
    }

    void cleanup_timer(RunningProcess& process) {
        disarm_timer(process);
        if (process.timer_registration.has_value()) {
            const auto removed = event_loop_->remove(*process.timer_registration);
            if (!removed) {
                record_error(removed.error().message);
                return;
            }
            process.timer_registration.reset();
        }
    }

    void maybe_complete(RunningProcess& process) {
        if (process.completion_scheduled || !process.exec_resolved ||
            !process.handle.reaped() || !process.stdout_eof || !process.stderr_eof ||
            process.handle.cleanup_phase() != GroupCleanupPhase::ABSENT) {
            return;
        }
        cleanup_timer(process);
        if (process.timer_registration.has_value()) {
            return;
        }
        process.completion_scheduled = true;
        const RuntimeProcessId id = process.id;
        const auto deferred = event_loop_->defer([this, id] { finalize(id); });
        if (!deferred) {
            process.completion_scheduled = false;
            record_error(deferred.error().message);
        }
    }

    void finalize(RuntimeProcessId id) {
        const auto iterator = processes_.find(id);
        if (iterator == processes_.end()) {
            return;
        }
        RunningProcess& process = *iterator->second;
        completions_.push_back(ProcessCompletion{
            process.id,
            process.handle.leader_pid(),
            process.handle.pgid(),
            process.exec_succeeded,
            process.launch_failure,
            process.handle.termination_cause(),
            process.handle.leader_outcome(),
            process.stdout_eof,
            process.stderr_eof,
        });
        processes_.erase(iterator);
    }

    std::unique_ptr<io::EventLoop> event_loop_;
    io::SignalFd signal_fd_;
    std::optional<io::RegistrationToken> signal_registration_;
    sigset_t previous_signal_mask_{};
    std::string startup_cwd_;
    std::vector<std::string> inherited_environment_;
    OutputSink output_sink_;
    RuntimeProcessId next_process_id_{1U};
    std::unordered_map<RuntimeProcessId, std::unique_ptr<RunningProcess>> processes_;
    std::unordered_map<pid_t, RuntimeProcessId> pid_to_process_;
    std::vector<RuntimeProcessId> started_;
    std::vector<ProcessCompletion> completions_;
    std::optional<std::string> fatal_error_;
    std::uint32_t registration_failure_mask_{0U};
};

common::Result<std::unique_ptr<ProcessRuntime>> ProcessRuntime::create(
    OutputSink output_sink) {
    if (!output_sink) {
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            "ProcessRuntime requires an output sink");
    }

    sigset_t child_mask{};
    if (::sigemptyset(&child_mask) == -1 || ::sigaddset(&child_mask, SIGCHLD) == -1) {
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            errno_message("prepare SIGCHLD mask", errno));
    }
    sigset_t previous_mask{};
    const int mask_error = ::pthread_sigmask(SIG_BLOCK, &child_mask, &previous_mask);
    if (mask_error != 0) {
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            errno_message("pthread_sigmask(SIG_BLOCK)", mask_error));
    }

    auto event_loop = io::EventLoop::create();
    if (!event_loop) {
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr));
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            event_loop.error().message);
    }
    auto signal_fd = io::SignalFd::create(child_mask);
    if (!signal_fd) {
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr));
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            signal_fd.error().message);
    }
    auto startup_cwd = PreparedExec::capture_startup_cwd();
    if (!startup_cwd) {
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr));
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            startup_cwd.error().message);
    }
    auto environment = PreparedExec::snapshot_environment();
    if (!environment) {
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr));
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            environment.error().message);
    }

    auto implementation = std::make_unique<Impl>(
        std::move(event_loop).value(), std::move(signal_fd).value(), previous_mask,
        std::move(startup_cwd).value(), std::move(environment).value(),
        std::move(output_sink));
    const auto initialized = implementation->initialize();
    if (!initialized) {
        return common::Result<std::unique_ptr<ProcessRuntime>>::failure(
            initialized.error().message);
    }
    return common::Result<std::unique_ptr<ProcessRuntime>>::success(
        std::unique_ptr<ProcessRuntime>(new ProcessRuntime(std::move(implementation))));
}

ProcessRuntime::ProcessRuntime(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

ProcessRuntime::~ProcessRuntime() = default;

void ProcessRuntime::fail_next_registrations_for_testing(std::uint32_t mask) {
    implementation_->fail_next_registrations_for_testing(mask);
}

int ProcessRuntime::output_fd_for_testing(RuntimeProcessId id, OutputStream stream) {
    return implementation_->output_fd_for_testing(id, stream);
}

common::Result<RuntimeProcessId> ProcessRuntime::launch(const LaunchOptions& options) {
    return implementation_->launch(options);
}

common::Result<bool> ProcessRuntime::cancel(RuntimeProcessId process_id) {
    return implementation_->cancel(process_id);
}

common::Result<std::size_t> ProcessRuntime::poll_once(int timeout_ms) {
    return implementation_->poll_once(timeout_ms);
}

common::Result<void> ProcessRuntime::set_output_enabled(RuntimeProcessId process_id,
                                                         OutputStream stream,
                                                         bool enabled) {
    return implementation_->set_output_enabled(process_id, stream, enabled);
}

std::vector<RuntimeProcessId> ProcessRuntime::take_started() {
    return implementation_->take_started();
}

std::vector<ProcessCompletion> ProcessRuntime::take_completions() {
    return implementation_->take_completions();
}

std::size_t ProcessRuntime::running_count() const noexcept {
    return implementation_->running_count();
}

std::size_t ProcessRuntime::registration_count() const noexcept {
    return implementation_->registration_count();
}

}  // namespace ts::runtime
