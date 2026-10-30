#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

void write_all(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t written = 0U;
    while (written < size) {
        const ssize_t result = ::write(fd, bytes + written, size - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        ::_exit(120);
    }
}

void write_text(int fd, const std::string& value) {
    write_all(fd, value.data(), value.size());
}

[[noreturn]] void pause_forever() {
    for (;;) {
        static_cast<void>(::pause());
    }
}

void exit_on_term(int) {
    ::_exit(0);
}

int parse_int(const char* text) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0L || value > 1'000'000L) {
        return -1;
    }
    return static_cast<int>(value);
}

void make_sync_pipe(int (&fds)[2]) {
    if (::pipe2(fds, O_CLOEXEC) == -1) {
        ::_exit(116);
    }
}

void notify_parent(int fd) {
    const unsigned char ready = 1U;
    write_all(fd, &ready, sizeof(ready));
    static_cast<void>(::close(fd));
}

void wait_for_child(int fd) {
    unsigned char ready = 0U;
    for (;;) {
        const ssize_t count = ::read(fd, &ready, sizeof(ready));
        if (count == 1) {
            break;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        ::_exit(117);
    }
    static_cast<void>(::close(fd));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 100;
    }
    const std::string mode(argv[1]);
    if (mode == "fail-until-file" && argc == 3) {
        write_text(STDOUT_FILENO, "attempt-output\n");
        write_text(STDERR_FILENO, "attempt-diagnostic\n");
        return ::access(argv[2], F_OK) == 0 ? 0 : 7;
    }
    if (mode == "continuous") {
        static_cast<void>(::signal(SIGTERM, SIG_IGN));
        std::vector<unsigned char> block(4096U, 0x5aU);
        for (;;) {
            write_all(STDOUT_FILENO, block.data(), block.size());
        }
    }
    if (mode == "stdio-check") {
        char byte = '\0';
        if (::read(STDIN_FILENO, &byte, 1U) != 0) {
            return 118;
        }
        char cwd[4096];
        if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
            return 119;
        }
        write_text(STDOUT_FILENO, "stdin-eof;cwd=" + std::string(cwd) + "\n");
        const unsigned char bytes[]{'e', 0U, 0xfeU};
        write_all(STDERR_FILENO, bytes, sizeof(bytes));
        return 0;
    }
    if (mode == "exit" && argc == 3) {
        const int code = parse_int(argv[2]);
        return code >= 0 && code <= 255 ? code : 101;
    }
    if (mode == "signal" && argc == 3) {
        const int signal_number = parse_int(argv[2]);
        if (signal_number <= 0) {
            return 102;
        }
        static_cast<void>(::raise(signal_number));
        return 103;
    }
    if (mode == "emit") {
        const std::vector<unsigned char> stdout_bytes{'o', 'u', 't', 0U, 0xffU, '\n'};
        const std::vector<unsigned char> stderr_bytes{'e', 'r', 'r', 0U, 0xfeU, '\n'};
        write_all(STDOUT_FILENO, stdout_bytes.data(), stdout_bytes.size());
        write_all(STDERR_FILENO, stderr_bytes.data(), stderr_bytes.size());
        return 0;
    }
    if (mode == "interleave") {
        for (int index = 0; index < 32; ++index) {
            const unsigned char stdout_byte = static_cast<unsigned char>('A' + index % 26);
            const unsigned char stderr_byte = static_cast<unsigned char>('a' + index % 26);
            write_all(STDOUT_FILENO, &stdout_byte, 1U);
            write_all(STDERR_FILENO, &stderr_byte, 1U);
        }
        return 0;
    }
    if (mode == "large" && argc == 3) {
        const int count = parse_int(argv[2]);
        if (count < 0) {
            return 104;
        }
        std::vector<unsigned char> block(4096U);
        for (std::size_t index = 0U; index < block.size(); ++index) {
            block[index] = static_cast<unsigned char>(index % 251U);
        }
        int remaining = count;
        while (remaining > 0) {
            const std::size_t amount =
                remaining < static_cast<int>(block.size())
                    ? static_cast<std::size_t>(remaining)
                    : block.size();
            write_all(STDOUT_FILENO, block.data(), amount);
            write_all(STDERR_FILENO, block.data(), amount);
            remaining -= static_cast<int>(amount);
        }
        return 0;
    }
    if (mode == "describe") {
        std::vector<char> cwd(4096U);
        if (::getcwd(cwd.data(), cwd.size()) == nullptr) {
            return 105;
        }
        write_text(STDOUT_FILENO, "cwd=" + std::string(cwd.data()) + "\n");
        const char* environment = std::getenv("TS_RUNTIME_TEST_VALUE");
        write_text(STDOUT_FILENO,
                   "env=" + std::string(environment == nullptr ? "<missing>" : environment) +
                       "\n");
        for (int index = 0; index < argc; ++index) {
            write_text(STDOUT_FILENO,
                       "arg" + std::to_string(index) + "=" + argv[index] + "\n");
        }
        return 0;
    }
    if (mode == "stdin-eof") {
        char byte = '\0';
        const ssize_t count = ::read(STDIN_FILENO, &byte, 1U);
        if (count == 0) {
            write_text(STDOUT_FILENO, "stdin-eof\n");
            return 0;
        }
        return 106;
    }
    if (mode == "cooperative") {
        struct sigaction action {};
        action.sa_handler = exit_on_term;
        static_cast<void>(::sigemptyset(&action.sa_mask));
        if (::sigaction(SIGTERM, &action, nullptr) == -1) {
            return 107;
        }
        write_text(STDOUT_FILENO, "ready\n");
        pause_forever();
    }
    if (mode == "ignore-term") {
        static_cast<void>(::signal(SIGTERM, SIG_IGN));
        write_text(STDOUT_FILENO, "ready\n");
        pause_forever();
    }
    if (mode == "residual" && argc == 3) {
        const int leader_code = parse_int(argv[2]);
        int synchronization[2]{-1, -1};
        make_sync_pipe(synchronization);
        const pid_t child = ::fork();
        if (child == -1) {
            return 108;
        }
        if (child == 0) {
            static_cast<void>(::close(synchronization[0]));
            static_cast<void>(::signal(SIGTERM, SIG_IGN));
            write_text(STDOUT_FILENO, "residual-tail\n");
            notify_parent(synchronization[1]);
            pause_forever();
        }
        static_cast<void>(::close(synchronization[1]));
        wait_for_child(synchronization[0]);
        return leader_code >= 0 && leader_code <= 255 ? leader_code : 109;
    }
    if (mode == "residual-signal") {
        int synchronization[2]{-1, -1};
        make_sync_pipe(synchronization);
        const pid_t child = ::fork();
        if (child == -1) {
            return 113;
        }
        if (child == 0) {
            static_cast<void>(::close(synchronization[0]));
            static_cast<void>(::signal(SIGTERM, SIG_IGN));
            write_text(STDOUT_FILENO, "signal-tail\n");
            notify_parent(synchronization[1]);
            pause_forever();
        }
        static_cast<void>(::close(synchronization[1]));
        wait_for_child(synchronization[0]);
        static_cast<void>(::raise(SIGUSR2));
        return 114;
    }
    if (mode == "tree-ignore") {
        static_cast<void>(::signal(SIGTERM, SIG_IGN));
        const pid_t child = ::fork();
        if (child == -1) {
            return 115;
        }
        if (child == 0) {
            pause_forever();
        }
        write_text(STDOUT_FILENO, "tree-ready\n");
        pause_forever();
    }
    if (mode == "escape") {
        int synchronization[2]{-1, -1};
        make_sync_pipe(synchronization);
        const pid_t child = ::fork();
        if (child == -1) {
            return 110;
        }
        if (child == 0) {
            static_cast<void>(::close(synchronization[0]));
            if (::setsid() == -1) {
                ::_exit(111);
            }
            static_cast<void>(::signal(SIGTERM, SIG_IGN));
            write_text(STDOUT_FILENO,
                       "escaped=" + std::to_string(static_cast<long long>(::getpid())) + "\n");
            notify_parent(synchronization[1]);
            pause_forever();
        }
        static_cast<void>(::close(synchronization[1]));
        wait_for_child(synchronization[0]);
        return 0;
    }
    return 112;
}
